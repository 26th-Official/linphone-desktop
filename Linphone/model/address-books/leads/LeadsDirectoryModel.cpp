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

#include "LeadsDirectoryModel.hpp"

#include "model/core/CoreModel.hpp"
#include "model/tool/ToolModel.hpp"
#include "tool/Utils.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

DEFINE_ABSTRACT_OBJECT(LeadsDirectoryModel)

// Config keys live in their own section so they cannot collide with upstream settings
// and are easy to spot in linphonerc.
static constexpr char kSection[] = "leads_directory";
static constexpr char kListName[] = "leads_directory";

std::shared_ptr<LeadsDirectoryModel> LeadsDirectoryModel::gLeadsDirectoryModel;

LeadsDirectoryModel::LeadsDirectoryModel(QObject *parent) : QObject(parent) {
	mustBeInLinphoneThread(getClassName());
	// Parented to this, so it is destroyed on the linphone thread along with the model.
	mNetwork = new QNetworkAccessManager(this);
}

LeadsDirectoryModel::~LeadsDirectoryModel() {
	mustBeInLinphoneThread("~" + getClassName());
}

std::shared_ptr<LeadsDirectoryModel> LeadsDirectoryModel::create(QObject *parent) {
	auto instance = std::make_shared<LeadsDirectoryModel>(parent);
	gLeadsDirectoryModel = instance;
	return instance;
}

std::shared_ptr<LeadsDirectoryModel> LeadsDirectoryModel::getInstance() {
	// Created on first use, as FriendsManager does (FriendsManager.cpp:86). This avoids
	// depending on being constructed before MagicSearchModel, which connects to us in its
	// own constructor. Must be called from the linphone thread.
	if (!gLeadsDirectoryModel) gLeadsDirectoryModel = LeadsDirectoryModel::create(nullptr);
	return gLeadsDirectoryModel;
}

QString LeadsDirectoryModel::getBaseUrl() const {
	auto core = CoreModel::getInstance() ? CoreModel::getInstance()->getCore() : nullptr;
	if (!core) return QString();
	return Utils::coreStringToAppString(core->getConfig()->getString(kSection, "url", "")).trimmed();
}

QString LeadsDirectoryModel::getToken() const {
	auto core = CoreModel::getInstance() ? CoreModel::getInstance()->getCore() : nullptr;
	if (!core) return QString();
	return Utils::coreStringToAppString(core->getConfig()->getString(kSection, "token", "")).trimmed();
}

bool LeadsDirectoryModel::isEnabled() const {
	auto core = CoreModel::getInstance() ? CoreModel::getInstance()->getCore() : nullptr;
	if (!core) return false;
	if (!core->getConfig()->getBool(kSection, "enabled", false)) return false;
	return !getBaseUrl().isEmpty();
}

int LeadsDirectoryModel::getMinCharacters() const {
	auto core = CoreModel::getInstance() ? CoreModel::getInstance()->getCore() : nullptr;
	if (!core) return 3;
	return core->getConfig()->getInt(kSection, "min_characters", 3);
}

std::shared_ptr<linphone::FriendList> LeadsDirectoryModel::getLeadsFriendList() const {
	auto core = CoreModel::getInstance() ? CoreModel::getInstance()->getCore() : nullptr;
	if (!core) return nullptr;

	auto friendList = core->getFriendListByName(kListName);
	if (!friendList) {
		friendList = core->createFriendList();
		friendList->setDisplayName(kListName);
		// ApplicationCache, as ldap_friends uses (ToolModel.cpp:518). Two reasons, both
		// load-bearing: the list is not persisted to the friends database, and no
		// presence subscription is issued for its members. Without this, searching would
		// slowly accumulate thousands of leads on disk and emit a SIP SUBSCRIBE for each.
		friendList->setType(linphone::FriendList::Type::ApplicationCache);
		core->addFriendList(friendList);
	}
	return friendList;
}

bool LeadsDirectoryModel::materialiseLead(const QString &name, const QString &phone) {
	mustBeInLinphoneThread(log().arg(Q_FUNC_INFO));
	auto core = CoreModel::getInstance() ? CoreModel::getInstance()->getCore() : nullptr;
	if (!core || name.isEmpty() || phone.isEmpty()) return false;

	auto friendList = getLeadsFriendList();
	if (!friendList) return false;

	// The same lead comes back on every keystroke that matches it. Adding it twice would
	// show duplicates and grow the list without bound.
	if (friendList->findFriendByPhoneNumber(Utils::appStringToCoreString(phone))) return false;

	auto linphoneFriend = core->createFriend();
	if (!linphoneFriend) return false;
	linphoneFriend->setName(Utils::appStringToCoreString(name));

	// A phone number alone is not dialable: findFriend() and the call UI resolve by
	// address, so the number has to be interpreted into a SIP address as well.
	// interpretUrl applies the account's domain, turning "+31612154344" into
	// "sip:+31612154344@<domain>".
	auto address = ToolModel::interpretUrl(phone);
	if (address) linphoneFriend->setAddress(address);

	auto number = linphone::Factory::get()->createFriendPhoneNumber(Utils::appStringToCoreString(phone), "");
	if (number) linphoneFriend->addPhoneNumberWithLabel(number);

	if (friendList->addFriend(linphoneFriend) != linphone::FriendList::Status::OK) {
		lWarning() << log().arg("Failed to add lead to the friend list: %1").arg(name);
		return false;
	}
	// Tells the rest of the app a friend now exists for this address. CallHistoryCore and
	// CallCore listen for this to fill in a display name they previously could not resolve.
	emit CoreModel::getInstance()->friendCreated(linphoneFriend);
	return true;
}

void LeadsDirectoryModel::search(const QString &filter) {
	mustBeInLinphoneThread(log().arg(Q_FUNC_INFO));

	if (!isEnabled()) return;

	// The contacts tab sends "*" when its search box is empty. Returning every lead is
	// never useful and would defeat the point of not syncing, so it is dropped here as
	// well as being rejected server-side.
	const QString trimmed = filter.trimmed();
	if (trimmed.isEmpty() || trimmed == "*") return;
	if (trimmed.size() < getMinCharacters()) return;

	QUrl url(getBaseUrl() + "/contacts/search");
	if (!url.isValid() || url.host().isEmpty()) {
		lWarning() << log().arg("Leads directory URL is not valid: %1").arg(getBaseUrl());
		return;
	}
	QUrlQuery query;
	query.addQueryItem("q", trimmed);
	query.addQueryItem("limit", "25");
	url.setQuery(query);

	QNetworkRequest request(url);
	const QString token = getToken();
	if (!token.isEmpty()) {
		request.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());
	}
	// The user is typing; a reply that arrives late is worthless. Keep it short so a dead
	// server cannot leave requests pending for the default two minutes.
	request.setTransferTimeout(5000);

	const quint64 generation = ++mGeneration;
	QNetworkReply *reply = mNetwork->get(request);
	// `filter` is passed through untouched rather than the trimmed form used for the
	// query: the listener compares it against its own record of the current search, which
	// is not trimmed, so a trimmed value would never match and the re-search would be lost.
	// `this` as context means Qt disconnects automatically if the model dies in flight.
	connect(reply, &QNetworkReply::finished, this,
	        [this, reply, filter, generation]() { handleSearchReply(reply, filter, generation); });
}

void LeadsDirectoryModel::handleSearchReply(QNetworkReply *reply, const QString &filter, quint64 generation) {
	mustBeInLinphoneThread(log().arg(Q_FUNC_INFO));
	// deleteLater rather than delete: we are inside the reply's own finished signal.
	reply->deleteLater();

	// A reply from an older keystroke would inject results for a filter the user has
	// already moved past.
	if (generation != mGeneration) return;

	if (reply->error() != QNetworkReply::NoError) {
		// Expected whenever the server is down or unreachable. Local contacts still work,
		// so this is a warning rather than something the user needs to see. No signal is
		// emitted: there is nothing new to show, so re-running the search would be waste.
		lWarning() << log().arg("Leads directory request failed: %1").arg(reply->errorString());
		return;
	}

	QJsonParseError parseError;
	const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseError);
	if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
		lWarning() << log().arg("Leads directory returned malformed JSON: %1").arg(parseError.errorString());
		return;
	}

	// Bound the cache: a long session doing a lot of searching would otherwise keep
	// growing this list for the lifetime of the process.
	auto friendList = getLeadsFriendList();
	if (friendList && (int)friendList->getFriends().size() > kMaxCachedLeads) {
		lInfo() << log().arg("Clearing the leads cache (over %1 entries)").arg(kMaxCachedLeads);
		// getFriends() returns the list by value, so removing while iterating is safe.
		for (auto &cached : friendList->getFriends())
			friendList->removeFriend(cached);
	}

	int added = 0;
	const QJsonArray contacts = doc.object().value("contacts").toArray();
	for (const auto &value : contacts) {
		const QJsonObject contact = value.toObject();
		if (materialiseLead(contact.value("name").toString(), contact.value("phone").toString())) ++added;
	}

	lInfo() << log().arg("Leads directory: %1 result(s) for '%2', %3 new").arg(contacts.size()).arg(filter).arg(added);

	// Only worth re-running the SDK query if something genuinely new landed in the friend
	// list. When every hit was already cached, the first pass already found them.
	if (added > 0) emit searchFinished(filter);
}
