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

#ifndef LEADS_DIRECTORY_MODEL_H_
#define LEADS_DIRECTORY_MODEL_H_

#include "tool/AbstractObject.hpp"
#include <QObject>
#include <QString>
#include <linphone++/linphone.hh>

class QNetworkAccessManager;
class QNetworkReply;

/*
 * Queries a remote HTTP directory of CRM leads and materialises matches as
 * linphone::Friend objects.
 *
 * Contacts are deliberately NOT synced into the client. A CRM grows without bound, and
 * a local copy would mean a slow startup, a growing database, and -- because
 * FriendCore::save() calls updateSubscriptions() -- a SIP SUBSCRIBE per contact, which
 * would flood the provider. Instead the server is queried live and only the handful of
 * matching leads ever exist in memory.
 *
 * The SDK owns search-source aggregation and its source list is a fixed enum, so results
 * cannot be registered as a new search source. They are injected as friends in a friend
 * list, which the SDK's existing Friends source then picks up. This mirrors what the app
 * already does for LDAP in MagicSearchModel::updateFriendListWithFriend().
 *
 * Lives on the linphone thread. That thread runs a Qt event loop (Thread::run() calls
 * exec()), so QNetworkAccessManager works here and no thread hop is needed between the
 * HTTP reply and creating friends.
 */
class LeadsDirectoryModel : public QObject, public AbstractObject {
	Q_OBJECT

public:
	LeadsDirectoryModel(QObject *parent = nullptr);
	~LeadsDirectoryModel();

	static std::shared_ptr<LeadsDirectoryModel> create(QObject *parent = nullptr);
	static std::shared_ptr<LeadsDirectoryModel> getInstance();

	// True when a URL is configured and the feature is switched on. Callers check this
	// before doing any work, so a disabled directory costs nothing.
	bool isEnabled() const;
	// Below this many characters the server is not queried at all. Mirrors the existing
	// CardDAV min_characters gate.
	int getMinCharacters() const;

	// Fires an async search. Returns immediately; searchFinished is emitted once matches
	// have been added to the friend list, or straight away if there is nothing to do.
	void search(const QString &filter);

signals:
	// Carries the filter it corresponds to so a listener can discard stale replies.
	void searchFinished(QString filter);

private:
	static std::shared_ptr<LeadsDirectoryModel> gLeadsDirectoryModel;

	QString getBaseUrl() const;
	QString getToken() const;

	std::shared_ptr<linphone::FriendList> getLeadsFriendList() const;
	// Returns true if a friend was added, false if this lead is already present.
	bool materialiseLead(const QString &name, const QString &phone);
	void handleSearchReply(QNetworkReply *reply, const QString &filter, quint64 generation);

	QNetworkAccessManager *mNetwork = nullptr;

	// Typing produces overlapping requests. Only the newest generation is allowed to
	// touch the friend list, so a slow earlier reply cannot overwrite newer results.
	quint64 mGeneration = 0;

	// The cache is not persisted (ApplicationCache list type) but a long session with a
	// lot of searching would still grow it without bound. Cleared wholesale past this.
	static constexpr int kMaxCachedLeads = 1000;

	DECLARE_ABSTRACT_OBJECT
};

#endif
