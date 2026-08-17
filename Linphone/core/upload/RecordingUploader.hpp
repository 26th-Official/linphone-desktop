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

#ifndef RECORDING_UPLOADER_H_
#define RECORDING_UPLOADER_H_

#include "tool/AbstractObject.hpp"

#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>

// =============================================================================
// Uploads finished call recordings to the linphone-helper service.
//
// Lives on the Qt main thread: QNetworkAccessManager is not thread safe and the only
// caller, CallCore, already hops to this thread with invokeToCore. The local file is
// never deleted, so a failure here can delay a recording reaching S3 but cannot lose it.
// =============================================================================

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

class RecordingUploader : public QObject, public AbstractObject {
	Q_OBJECT

public:
	static RecordingUploader *getInstance();

	RecordingUploader(QObject *parent = nullptr);
	~RecordingUploader();

	// Call facts sent alongside the recording, so the server can match an object back to a
	// call without parsing the filename. Everything here is a plain value copied on the Qt
	// main thread; nothing holds a linphone object, so the struct is safe to keep in the
	// retry queue and to persist across a restart.
	struct CallMetadata {
		QString callId;        // SIP Call-ID -- the join key for a provider-side webhook
		QString remoteAddress; // sip:+31...@domain
		QString remoteName;    // resolved display name, may be empty
		QString localAddress;  // which local account took the call
		QString direction;     // "incoming" | "outgoing"
		QString status;        // "Success" | "Missed" | "Declined" | ...
		int durationSeconds = -1;
		QString encryption;    // media encryption, may be empty

		bool isEmpty() const {
			return callId.isEmpty() && remoteAddress.isEmpty();
		}
	};

	// Entry point. Safe to call for every finished recording: it returns immediately if
	// uploading is disabled or unconfigured.
	void uploadRecording(const QString &filePath, const CallMetadata &metadata = {});

signals:
	// Emitted for a successful upload (key = the object key returned by the server) and for
	// a permanent failure (key empty, error set). Retryable failures stay silent: they are
	// queued and will emit later, and one popup per hiccup would be noise.
	void uploadFinished(const QString &filePath, const QString &key);
	void uploadFailed(const QString &filePath, const QString &error);

private:
	// A pending upload: the file path, how many times we have tried it, and the call facts
	// captured when the recording finished. The metadata is carried through the queue
	// because the call object is long gone by the time a retry fires.
	struct PendingUpload {
		QString filePath;
		int attempts = 0;
		CallMetadata metadata;
	};

	void start(const QString &filePath, const CallMetadata &metadata, int attempts);
	// Polls the file size until it stops changing, then calls send(). Timer-driven rather
	// than a sleep: this runs on the Qt main thread. See the comment in the .cpp.
	void awaitSettled(const QString &filePath, const CallMetadata &metadata, int attempts, qint64 lastSize,
	                  int checks);
	void send(const QString &filePath, const CallMetadata &metadata, int attempts, qint64 size);
	void onReplyFinished(QNetworkReply *reply, const QString &filePath, const CallMetadata &metadata, int attempts);

	// Retry bookkeeping.
	void enqueue(const QString &filePath, const CallMetadata &metadata, int attempts);
	void flushQueue();
	void scheduleRetry();
	void loadQueue();
	void saveQueue();

	// Settings, read fresh on each upload so a settings change takes effect without restart.
	bool isEnabled() const;
	QString serverUrl() const;
	QString apiToken() const;

	QNetworkAccessManager *mNetwork = nullptr;
	QTimer *mRetryTimer = nullptr;

	QQueue<PendingUpload> mQueue;
	// Paths currently settling or in flight, so a retry sweep cannot upload the same file
	// twice.
	QSet<QString> mInFlight;

	DECLARE_ABSTRACT_OBJECT
};

#endif // RECORDING_UPLOADER_H_
