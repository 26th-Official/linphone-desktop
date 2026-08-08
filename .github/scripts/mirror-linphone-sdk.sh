#!/usr/bin/env bash
#
# Mirror linphone-sdk (and its ~31 nested submodules) into a single flat Git repo.
#
# Why: the SDK and its dependencies live on gitlab.linphone.org, which drops roughly
# half of all incoming connections. A recursive submodule checkout needs ~33 clones
# from that host, so CI either takes 25+ minutes or fails outright. Collapsing the
# whole tree into one repository turns that into a single fast clone from GitHub.
#
# Run this from a machine that CAN reach gitlab.linphone.org. Re-run it whenever the
# linphone-sdk submodule pointer in this repo changes.
#
# Usage:
#   .github/scripts/mirror-linphone-sdk.sh
#   MIRROR_REMOTE=git@github.com:you/your-mirror.git .github/scripts/mirror-linphone-sdk.sh
#
set -euo pipefail

MIRROR_REMOTE="${MIRROR_REMOTE:-git@github.com:26th-Official/linphone-sdk-mirror.git}"
MIRROR_BRANCH="${MIRROR_BRANCH:-main}"
SDK_URL="${SDK_URL:-https://gitlab.linphone.org/BC/public/linphone-sdk.git}"

# gitlab.linphone.org refuses connections from datacenter IP ranges, so a cloud VM
# needs a proxy to reach it. Set GIT_PROXY to an HTTP proxy to route Git through it,
# e.g. GIT_PROXY=http://127.0.0.1:1080 (Cloudflare WARP). Passing this via `git -c`
# exports it through GIT_CONFIG_PARAMETERS, which nested submodule clones inherit.
GIT_PROXY="${GIT_PROXY:-}"
git_cfg=()
if [ -n "$GIT_PROXY" ]; then
  git_cfg=(-c "http.proxy=$GIT_PROXY" -c "https.proxy=$GIT_PROXY")
  echo "==> Routing Git through proxy: $GIT_PROXY"
fi

repo_root=$(git rev-parse --show-toplevel)
sdk_sha=$(git -C "$repo_root" rev-parse HEAD:external/linphone-sdk)

# Reuse a stable working directory instead of a fresh mktemp each run. Fetching the
# SDK costs ~300 MB over a proxy, so a failure at the final push (bad credentials,
# say) must not throw that away -- a re-run resumes from whatever is already here.
# Delete this directory by hand to force a clean rebuild.
workdir="${MIRROR_WORKDIR:-$HOME/.cache/linphone-sdk-mirror}"
mkdir -p "$workdir"
sdk="$workdir/linphone-sdk"

echo "==> Pinned SDK commit: $sdk_sha"
echo "==> Mirror target:     $MIRROR_REMOTE ($MIRROR_BRANCH)"
echo "==> Work directory:    $workdir (kept between runs)"
echo

# The flattened tree has no .gitmodules and no nested .git dirs, so its presence is
# what marks the fetch+flatten phase as already finished by an earlier run.
# A flattened tree is only reusable if it recorded the same pinned commit AND still has
# real content. Trusting the marker file alone let an empty flattened shell be reused.
flattened_is_usable=0
if [ -f "$sdk/MIRROR_SOURCE.txt" ] && [ ! -f "$sdk/.gitmodules" ]; then
  if grep -q "$sdk_sha" "$sdk/MIRROR_SOURCE.txt" 2>/dev/null &&
     [ -n "$(find "$sdk/external" -type f -print -quit 2>/dev/null)" ]; then
    flattened_is_usable=1
  else
    echo "==> Discarding stale/empty flattened tree from a previous run"
    rm -rf "$sdk"
  fi
fi

if [ "$flattened_is_usable" = 1 ]; then
  echo "==> Reusing the flattened tree from a previous run"
  echo "    (delete $workdir to rebuild from scratch)"
else
  # Full clone, not --depth 1: we need to check out one specific pinned commit.
  # Reuse requires the clone to actually CONTAIN the pinned commit -- a bare `-d .git`
  # test wrongly matched the flattened mirror repo a previous run left here, which has
  # its own .git but none of upstream's history, so the fetch silently did nothing.
  if [ -d "$sdk/.git" ] && git -C "$sdk" cat-file -e "$sdk_sha^{commit}" 2>/dev/null; then
    echo "==> Reusing existing linphone-sdk clone (has the pinned commit)"
  else
    if [ -d "$sdk/.git" ]; then
      echo "==> Discarding unusable tree at $sdk (does not contain $sdk_sha)"
    fi
    echo "==> Cloning linphone-sdk (this pulls ~223 MB, be patient)"
    rm -rf "$sdk"
    git "${git_cfg[@]}" clone --quiet "$SDK_URL" "$sdk"
  fi
  # Fail loudly if the pinned commit still is not reachable -- everything downstream
  # (.gitmodules, submodule list, verification) is meaningless without it.
  if ! git -C "$sdk" cat-file -e "$sdk_sha^{commit}" 2>/dev/null; then
    echo "!! Clone does not contain pinned commit $sdk_sha -- aborting." >&2
    exit 1
  fi
  git -C "$sdk" checkout --quiet --force "$sdk_sha"

  # Completeness must be judged by what is on disk, NOT by git's exit code.
  # `submodule update` exits 0 even when most clones failed, and `submodule status`
  # prints nothing for a module whose clone never started -- so both of git's own
  # signals report success on a tree that is mostly empty directories. Walking every
  # .gitmodules and testing each path for actual content is the only honest check.
  # Expected paths come from the COMMITTED .gitmodules, read via `git show`. Reading the
  # working tree instead would be wrong twice over: flattening deletes those files, and a
  # module that was never fetched has no .gitmodules of its own to recurse into.
  expected_top_level_paths() {
    git -C "$sdk" show "$sdk_sha:.gitmodules" 2>/dev/null \
      | sed -n 's/^[[:space:]]*path[[:space:]]*=[[:space:]]*//p'
  }

  # A fetched submodule always contains files; an unfetched one is an empty dir or absent.
  # Counting files (not dir emptiness) is what distinguishes the two reliably.
  missing_paths() {
    local p
    while IFS= read -r p; do
      [ -n "$p" ] || continue
      if [ -z "$(find "$sdk/$p" -type f -print -quit 2>/dev/null)" ]; then
        printf '%s\n' "$p"
      fi
    done < <(expected_top_level_paths)
  }

  echo "==> Fetching nested submodules (sequential; the proxy 503s under load)"
  for attempt in 1 2 3 4 5 6 7 8 9 10; do
    # --jobs 1 deliberately: concurrent fetches trip proxy rate limiting.
    # Errors are tee'd, never discarded -- silencing them once hid the 503s entirely.
    git "${git_cfg[@]}" -C "$sdk" submodule update --init --recursive --depth 1 --jobs 1 \
      2>&1 | tee -a "$workdir/fetch.log" | grep -E "^(Submodule path|fatal|error)" || true

    mapfile -t still_missing < <(missing_paths)
    mapfile -t all_expected < <(expected_top_level_paths)
    # Guard against the check itself silently degenerating: if the expected list comes
    # back empty, .gitmodules could not be read and a "0 missing" result is meaningless.
    if [ "${#all_expected[@]}" -eq 0 ]; then
      echo "!! Cannot read .gitmodules at $sdk_sha -- refusing to certify completeness." >&2
      exit 1
    fi
    present=$(( ${#all_expected[@]} - ${#still_missing[@]} ))
    if [ "${#still_missing[@]}" -eq 0 ]; then
      echo "    verified: ${present}/${#all_expected[@]} submodule paths contain files (attempt $attempt)"
      break
    fi

    if [ "$attempt" = 10 ]; then
      echo "!! Gave up after 10 attempts. ${#still_missing[@]} path(s) still empty:" >&2
      printf '     %s\n' "${still_missing[@]}" >&2
      echo "   Re-run to resume; the clone is cached in $workdir" >&2
      exit 1
    fi

    # Back off progressively: 503s mean the proxy wants us to slow down.
    backoff=$((attempt * 15))
    echo "    attempt $attempt: ${present}/${#all_expected[@]} present, ${#still_missing[@]} still empty, retrying in ${backoff}s"
    sleep "$backoff"
  done
fi

# Flatten unless a previous run already did: .gitmodules is the marker, since a
# fresh clone always has .git and testing for that would skip flattening entirely.
if [ -f "$sdk/.gitmodules" ] || [ ! -f "$sdk/MIRROR_SOURCE.txt" ]; then
  # Last gate before the irreversible step. Flattening deletes all .git metadata, so a
  # tree flattened while incomplete cannot be topped up -- it has to be re-fetched from
  # scratch. Count empty dirs one final time and refuse rather than ship a broken mirror.
  # Re-derive the expected list here rather than trusting a variable set earlier: this
  # branch also runs when the fetch block was skipped entirely by the resume check.
  mapfile -t gate_expected < <(git -C "$sdk" show "$sdk_sha:.gitmodules" 2>/dev/null \
    | sed -n 's/^[[:space:]]*path[[:space:]]*=[[:space:]]*//p')
  if [ "${#gate_expected[@]}" -eq 0 ]; then
    echo "!! Refusing to flatten: cannot read .gitmodules to confirm completeness." >&2
    exit 1
  fi
  empty_before_flatten=0
  for p in "${gate_expected[@]}"; do
    if [ -z "$(find "$sdk/$p" -type f -print -quit 2>/dev/null)" ]; then
      empty_before_flatten=$((empty_before_flatten + 1))
      echo "     empty: $p" >&2
    fi
  done
  if [ "$empty_before_flatten" -gt 0 ]; then
    echo "!! Refusing to flatten: $empty_before_flatten of ${#gate_expected[@]} submodule path(s) have no files." >&2
    echo "   Flattening is irreversible, so an incomplete tree must not be committed." >&2
    echo "   Re-run to continue fetching (progress is cached in $workdir)." >&2
    exit 1
  fi

  # Record provenance before flattening, so the mirror says what it was built from.
  cat > "$sdk/MIRROR_SOURCE.txt" <<EOF
Flattened mirror of linphone-sdk for CI use.

source:  $SDK_URL
commit:  $sdk_sha

Generated by .github/scripts/mirror-linphone-sdk.sh in linphone-desktop.
Nested submodules were fetched with --depth 1 and their Git metadata removed,
so this is a source snapshot, not a usable upstream clone. Do not develop here;
re-run the script to refresh it.
EOF

  # Prevent Git from rewriting line endings. The tree contains shell scripts, patches
  # and assembly that a CRLF conversion on a Windows runner would silently corrupt.
  printf '* -text\n' > "$sdk/.gitattributes"

  echo "==> Flattening: removing nested Git metadata"
  find "$sdk" -mindepth 2 -name .git \( -type d -o -type f \) -prune -exec rm -rf {} +
  find "$sdk" -name .gitmodules -delete
  rm -rf "$sdk/.git"

  echo "==> Building the mirror commit"
  git -C "$sdk" init --quiet -b "$MIRROR_BRANCH"
  git -C "$sdk" add -A
  git -C "$sdk" -c user.name="sdk-mirror" -c user.email="sdk-mirror@localhost" \
      commit --quiet -m "linphone-sdk snapshot at ${sdk_sha:0:12}"
else
  echo "==> Mirror commit already built, reusing it"
fi

size=$(du -sh "$sdk" | cut -f1)
files=$(git -C "$sdk" ls-files | wc -l)
echo "    $files files, $size on disk"

echo "==> Pushing to $MIRROR_REMOTE"
echo "    This force-pushes '$MIRROR_BRANCH'. The mirror is disposable by design;"
echo "    nothing but generated snapshots should ever live on that branch."
read -r -p "    Continue? [y/N] " reply
[ "$reply" = y ] || [ "$reply" = Y ] || { echo "Aborted."; exit 1; }

# Re-point origin each run so switching between SSH and HTTPS just works.
git -C "$sdk" remote remove origin 2>/dev/null || true
git -C "$sdk" remote add origin "$MIRROR_REMOTE"
git -C "$sdk" push --force --quiet origin "$MIRROR_BRANCH"

echo
echo "Done. Mirror now holds linphone-sdk @ ${sdk_sha:0:12}"
echo "If SDK_MIRROR in .github/workflows/windows-build.yml does not already point"
echo "at $MIRROR_REMOTE, update it."
