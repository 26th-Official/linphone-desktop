/*
 * Copyright (c) 2010-2024 Belledonne Communications SARL.
 *
 * This file is part of linphone-desktop
 * (see https://www.linphone.org).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QTimer>
#include <QUrl>

#include "core/App.hpp"
#include "core/setting/SettingsCore.hpp"
#include "tool/Utils.hpp"

#include "RecordingUploader.hpp"

DEFINE_ABSTRACT_OBJECT(RecordingUploader)

namespace {
// Retry pacing for a failed upload. The first retry is quick because the usual cause is a
// momentary network blip; the cap keeps a long outage from spinning.
constexpr int RetryBaseMs = 30 * 1000;
constexpr int RetryMaxMs = 30 * 60 * 1000;
constexpr int MaxAttempts = 10;

// Settle polling: how long to wait between size samples, and how many samples before
// giving up and handing the file to the slow retry queue.
constexpr int SettleIntervalMs = 250;
constexpr int MaxSettleChecks = 8;

// Where the pending queue is persisted. QSettings rather than the linphone config: this is
// client bookkeeping, not user-facing configuration, and it must survive a crash.
constexpr char QueueGroup[] = "recordingUpload";
constexpr char QueueKey[] = "pending";
} // namespace

RecordingUploader *RecordingUploader::getInstance() {
	static RecordingUploader *instance = new RecordingUploader(App::getInstance());
	return instance;
}

RecordingUploader::RecordingUploader(QObject *parent) : QObject(parent) {
	mNetwork = new QNetworkAccessManager(this);

	mRetryTimer = new QTimer(this);
	mRetryTimer->setSingleShot(true);
	connect(mRetryTimer, &QTimer::timeout, this, &RecordingUploader::flushQueue);

	loadQueue();
	// Anything left from the previous run is retried after a delay rather than at startup,
	// where it would compete with core initialisation for the network.
	if (!mQueue.isEmpty()) {
		lInfo() << log().arg("Pending recording uploads from a previous run:") << mQueue.size();
		mRetryTimer->start(RetryBaseMs);
	}
}

RecordingUploader::~RecordingUploader() {
}

// -----------------------------------------------------------------------------
// Settings
// -----------------------------------------------------------------------------

// Read fresh each time rather than cached, so changing the URL or token takes effect
// without restarting the app.

bool RecordingUploader::isEnabled() const {
	auto settings = App::getInstance()->getSettings();
	if (!settings) return false;
	return settings->getRecordingUploadEnabled() && !serverUrl().isEmpty() && !apiToken().isEmpty();
}

QString RecordingUploader::serverUrl() const {
	auto settings = App::getInstance()->getSettings();
	// Trailing slash would produce "//recordings", which some proxies treat as a different
	// path than the server's route.
	QString url = settings ? settings->getRecordingUploadUrl().trimmed() : QString();
	while (url.endsWith(u'/'))
		url.chop(1);
	return url;
}

QString RecordingUploader::apiToken() const {
	auto settings = App::getInstance()->getSettings();
	return settings ? settings->getRecordingUploadToken().trimmed() : QString();
}

// -----------------------------------------------------------------------------
// Upload
// -----------------------------------------------------------------------------

void RecordingUploader::uploadRecording(const QString &filePath) {
	if (filePath.isEmpty()) return;
	if (!isEnabled()) return;
	start(filePath, 0);
}

void RecordingUploader::start(const QString &filePath, int attempts) {
	// Reserve the path up front: a retry sweep firing while this upload is still settling
	// would otherwise start a second copy of the same file.
	if (mInFlight.contains(filePath)) return;
	mInFlight.insert(filePath);
	awaitSettled(filePath, attempts, -1, 0);
}

// The SDK finalises the .mkv when recording stops, but recordingChanged(false) is emitted
// during that same teardown, so the last bytes may not have reached disk when we are
// called. Uploading then would store a truncated recording -- silent corruption, which is
// far worse than a visible failure. So wait for two consecutive identical, non-zero size
// samples before sending.
//
// Polled with a timer rather than a sleep: this runs on the Qt main thread, so blocking
// even briefly would freeze the UI at the exact moment the call ends.
void RecordingUploader::awaitSettled(const QString &filePath, int attempts, qint64 lastSize, int checks) {
	QFileInfo info(filePath);
	if (!info.exists() || !info.isFile()) {
		// Nothing to upload. Not an error worth surfacing: the local file is the
		// authoritative copy and the user may simply have moved it.
		lWarning() << log().arg("Recording no longer exists, not uploading:") << filePath;
		mInFlight.remove(filePath);
		return;
	}

	const qint64 size = info.size();
	if (size > 0 && size == lastSize) {
		send(filePath, attempts, size);
		return;
	}

	if (checks >= MaxSettleChecks) {
		// Still changing after ~2s. Unexpected, so hand it to the slow retry path rather
		// than sending a file that is still being written.
		lWarning() << log().arg("Recording still being written, deferring upload:") << filePath;
		mInFlight.remove(filePath);
		enqueue(filePath, attempts);
		return;
	}

	QTimer::singleShot(SettleIntervalMs, this, [this, filePath, attempts, size, checks]() {
		awaitSettled(filePath, attempts, size, checks + 1);
	});
}

void RecordingUploader::send(const QString &filePath, int attempts, qint64 size) {
	const QUrl url(serverUrl() + QStringLiteral("/recordings"));
	if (!url.isValid() || url.scheme().isEmpty()) {
		lWarning() << log().arg("Invalid upload URL:") << serverUrl();
		mInFlight.remove(filePath);
		emit uploadFailed(filePath, QStringLiteral("invalid server address"));
		return;
	}

	auto *file = new QFile(filePath);
	if (!file->open(QIODevice::ReadOnly)) {
		lWarning() << log().arg("Cannot open recording for upload:") << filePath;
		delete file;
		mInFlight.remove(filePath);
		emit uploadFailed(filePath, QStringLiteral("recording could not be opened"));
		return;
	}

	// QHttpMultiPart streams from the QFile rather than loading the whole recording into
	// memory, which matters for a long call. It takes ownership of the device, so the file
	// is closed and deleted with the multipart.
	auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
	QHttpPart filePart;
	const QString fileName = QFileInfo(filePath).fileName();
	filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
	                   QVariant(QStringLiteral("form-data; name=\"file\"; filename=\"%1\"").arg(fileName)));
	filePart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant(QMimeDatabase().mimeTypeForFile(filePath).name()));
	filePart.setBodyDevice(file);
	file->setParent(multiPart);
	multiPart->append(filePart);

	QNetworkRequest request(url);
	request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + apiToken().toUtf8());

	lInfo() << log().arg("Uploading recording:") << filePath << size << "bytes, attempt" << (attempts + 1);

	auto *reply = mNetwork->post(request, multiPart);
	multiPart->setParent(reply);
	connect(reply, &QNetworkReply::finished, this,
	        [this, reply, filePath, attempts]() { onReplyFinished(reply, filePath, attempts); });
}

void RecordingUploader::onReplyFinished(QNetworkReply *reply, const QString &filePath, int attempts) {
	mInFlight.remove(filePath);
	reply->deleteLater();

	const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const QByteArray body = reply->readAll();
	const QJsonObject json = QJsonDocument::fromJson(body).object();

	if (status == 200 || status == 201 || status == 202) {
		// {"status":"ok","key":...,"bucket":...,"bytes":...}
		const QString key = json.value("key").toString();
		lInfo() << log().arg("Recording uploaded:") << key;
		emit uploadFinished(filePath, key);
		// Success means the network is back, so drain anything queued behind it.
		if (!mQueue.isEmpty()) {
			mRetryTimer->stop();
			flushQueue();
		}
		return;
	}

	// FastAPI reports errors as {"detail": "..."}; fall back to the raw body, and then to
	// the transport error, which is what a connection failure looks like (status 0).
	const QString detail = json.contains("detail") ? json.value("detail").toString() : QString::fromUtf8(body).trimmed();
	const QString message = detail.isEmpty() ? reply->errorString() : detail;

	// The service chooses its status codes so the client can tell these apart: a bad file
	// is permanent, an upstream S3 failure (502) is not. Retrying a 415 forever would just
	// fill the log, and a rejected token will not fix itself either.
	const bool permanent = (status == 400 || status == 401 || status == 403 || status == 413 || status == 415);

	if (permanent) {
		lWarning() << log().arg("Upload rejected, not retrying. HTTP") << status << message << filePath;
		emit uploadFailed(filePath, message);
		return;
	}

	lWarning() << log().arg("Upload failed, will retry. HTTP") << status << message << filePath;
	enqueue(filePath, attempts + 1);
}

// -----------------------------------------------------------------------------
// Retry queue
// -----------------------------------------------------------------------------

void RecordingUploader::enqueue(const QString &filePath, int attempts) {
	if (attempts >= MaxAttempts) {
		// Stop retrying, but say so plainly. The recording is still on disk, so this is
		// recoverable by hand: nothing has been lost.
		lWarning() << log().arg("Giving up on upload after") << attempts << "attempts:" << filePath;
		emit uploadFailed(filePath, QStringLiteral("upload failed repeatedly, recording kept locally"));
		saveQueue();
		return;
	}

	for (auto &pending : mQueue) {
		if (pending.filePath == filePath) {
			pending.attempts = attempts;
			saveQueue();
			scheduleRetry();
			return;
		}
	}

	mQueue.enqueue({filePath, attempts});
	saveQueue();
	scheduleRetry();
}

void RecordingUploader::scheduleRetry() {
	if (mRetryTimer->isActive()) return;

	// Exponential backoff, capped. Driven by the fewest attempts in the queue so one
	// stubborn file cannot delay a freshly queued one.
	int attempts = MaxAttempts;
	for (const auto &pending : mQueue)
		attempts = qMin(attempts, pending.attempts);
	if (mQueue.isEmpty()) attempts = 0;

	qint64 delay = RetryBaseMs;
	for (int i = 1; i < attempts && delay < RetryMaxMs; ++i)
		delay *= 2;

	mRetryTimer->start(int(qMin<qint64>(delay, RetryMaxMs)));
}

void RecordingUploader::flushQueue() {
	if (mQueue.isEmpty()) return;
	if (!isEnabled()) return;

	// Snapshot and clear: start() re-enqueues on failure, which would otherwise mutate the
	// queue while it is being iterated.
	const auto pendingNow = mQueue;
	mQueue.clear();
	saveQueue();

	for (const auto &pending : pendingNow) {
		if (!QFileInfo::exists(pending.filePath)) {
			lInfo() << log().arg("Queued recording no longer exists, dropping:") << pending.filePath;
			continue;
		}
		start(pending.filePath, pending.attempts);
	}
}

void RecordingUploader::loadQueue() {
	QSettings settings;
	settings.beginGroup(QueueGroup);
	const auto stored = settings.value(QueueKey).toStringList();
	settings.endGroup();

	// Stored as "attempts|path". The count goes first and only the first separator is
	// significant, so a path containing '|' still round-trips.
	for (const auto &entry : stored) {
		const int sep = entry.indexOf(u'|');
		if (sep <= 0) continue;
		const QString path = entry.mid(sep + 1);
		if (!path.isEmpty()) mQueue.enqueue({path, entry.left(sep).toInt()});
	}
}

void RecordingUploader::saveQueue() {
	QStringList stored;
	stored.reserve(mQueue.size());
	for (const auto &pending : mQueue)
		stored << QStringLiteral("%1|%2").arg(pending.attempts).arg(pending.filePath);

	QSettings settings;
	settings.beginGroup(QueueGroup);
	settings.setValue(QueueKey, stored);
	settings.endGroup();
}
