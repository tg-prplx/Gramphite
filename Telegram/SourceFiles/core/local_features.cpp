/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/local_features.h"
#include "core/local_feature_journal.h"

#include "api/api_updates.h"
#include "apiwrap.h"
#include "core/file_location.h"
#include "data/data_changes.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/data_birthday.h"
#include "data/data_emoji_statuses.h"
#include "lang/lang_keys.h"
#include "window/window_session_controller.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "storage/storage_account.h"
#include "storage/cache/storage_cache_database.h"
#include "storage/storage_encryption.h"
#include "storage/file_download.h"
#include "base/unixtime.h"
#include "ui/userpic_view.h"
#include <QtCore/QBuffer>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>

namespace Core {
namespace {

using Storage::Cache::Database;
using Storage::Cache::Key;
using Storage::Cache::Error;

constexpr auto kMessageLimit = 100000;
constexpr auto kProfileLimit = 6000;
constexpr auto kEventLimit = 100;
constexpr auto kProfileEventLimit = 500;
constexpr auto kSessionLimit = 2000;
constexpr auto kMediaLimit = qint64(64 * 1024 * 1024);
constexpr auto kArchiveLimit = qint64(4096) * 1024 * 1024;
constexpr auto kRecordsLimit = qint64(256 * 1024 * 1024);
constexpr auto kFlushDelay = crl::time(30000);
constexpr auto kBucketSize = 128;
constexpr auto kProfileShards = 256;
constexpr auto kLoadedCache = 256;
constexpr auto kMetaVersion = 2;

constexpr auto kMetaKey = Key{ 0, 1 };
constexpr auto kFilesKey = Key{ 0, 2 };

[[nodiscard]] Key BucketKey(qint32 bucket) {
	return { 0, 0x100000000ULL + quint32(bucket) };
}

[[nodiscard]] Key ShardKey(int shard) {
	return { 0, 0x10000ULL + quint64(shard) };
}

[[nodiscard]] Key RecordKey(FullMsgId id) {
	return { id.peer.value, quint64(id.msg.bare) };
}

[[nodiscard]] int ProfileShard(const QString &key) {
	return int(key.toULongLong() % kProfileShards);
}

[[nodiscard]] FullMsgId EntryId(const LocalMessageEntry &entry) {
	return FullMsgId(PeerId(entry.peer), MsgId(entry.msg));
}

QString FeatureKey(LocalFeature feature) {
	switch (feature) {
	case LocalFeature::GhostMessages: return u"ghostMessages"_q;
	case LocalFeature::GhostStories: return u"ghostStories"_q;
	case LocalFeature::GhostOnline: return u"ghostOnline"_q;
	case LocalFeature::EditHistory: return u"editHistory"_q;
	case LocalFeature::KeepDeleted: return u"keepDeleted"_q;
	case LocalFeature::KeepExpired: return u"keepExpired"_q;
	case LocalFeature::CopyProtected: return u"copyProtected"_q;
	case LocalFeature::ProfileHistory: return u"profileHistory"_q;
	case LocalFeature::OnlineHistory: return u"onlineHistory"_q;
	case LocalFeature::BlockHint: return u"blockHint"_q;
	case LocalFeature::RegistrationEstimate: return u"registrationEstimate"_q;
	}
	Unexpected("Unknown local feature.");
}

qint64 Now() {
	return QDateTime::currentSecsSinceEpoch();
}

void AppendEvent(QJsonObject &record, QJsonObject event) {
	auto events = record[u"events"].toArray();
	event[u"time"] = Now();
	events.append(event);
	while (events.size() > kProfileEventLimit) {
		events.removeFirst();
	}
	record[u"events"] = events;
	record[u"updated"] = Now();
}

QJsonArray Records(const QJsonObject &records) {
	auto list = std::vector<QJsonObject>();
	for (const auto &record : records) {
		list.push_back(record.toObject());
	}
	ranges::sort(list, ranges::greater(), [](const auto &record) {
		return record[u"updated"].toInteger();
	});
	auto result = QJsonArray();
	for (const auto &record : list) {
		result.append(record);
	}
	return result;
}

} // namespace

LocalFeatures::LocalFeatures(not_null<Main::Session*> session)
: _session(session)
, _flushTimer([=] { flush(); })
, _onlineTimer([=] { checkOnlineExpiry(); }) {
	_session->local().setLocalFeaturesUser(_session->userId().bare);
	openArchive();
	loadArchive();
	using Flag = Data::PeerUpdate::Flag;
	_session->changes().peerUpdates(Flag::Name | Flag::Username
		| Flag::Usernames | Flag::Photo | Flag::About | Flag::OnlineStatus
		| Flag::PhoneNumber | Flag::EmojiStatus | Flag::Birthday
	) | rpl::on_next([=](const Data::PeerUpdate &update) {
		observePeer(update.peer, bool(update.flags & Flag::OnlineStatus));
	}, _lifetime);
	_session->data().enumerateUsers([=](not_null<UserData*> user) {
		observePeer(user);
	});
	_session->downloaderTaskFinished() | rpl::on_next([=] {
		finishVersionMedia();
		captureUserpics();
		const auto pending = _pendingMedia;
		for (const auto id : pending) {
			if (const auto item = _session->data().message(id)) {
				rememberExpiringMedia(item);
			} else { _pendingMedia.remove(id); }
		}
	}, _lifetime);
	captureUserpics();
	trim();
}

LocalFeatures::~LocalFeatures() {
	for (const auto shard : base::take(_volatileShards)) {
		_dirtyShards.insert(shard);
	}
	flush();
	if (_archive) {
		auto semaphore = crl::semaphore();
		_archive->close([&] { semaphore.release(); });
		semaphore.acquire();
	}
}

bool LocalFeatures::enabled(LocalFeature feature) const {
	return _settings[FeatureKey(feature)].toBool();
}

void LocalFeatures::setEnabled(LocalFeature feature, bool value) {
	if (enabled(feature) == value) {
		return;
	}
	_settings[FeatureKey(feature)] = value;
	settingsChanged();
	if (feature == LocalFeature::GhostOnline) {
		_session->api().updates().updateOnline();
	}
	if (feature == LocalFeature::ProfileHistory
		|| feature == LocalFeature::OnlineHistory
		|| feature == LocalFeature::BlockHint) {
		_session->data().enumerateUsers([=](not_null<UserData*> user) {
			observePeer(user);
		});
		captureUserpics();
	}
	_changes.fire({});
}

bool LocalFeatures::ghostMode() const {
	return enabled(LocalFeature::GhostMessages)
		&& enabled(LocalFeature::GhostStories)
		&& enabled(LocalFeature::GhostOnline);
}

void LocalFeatures::setGhostMode(bool enabled) {
	setEnabled(LocalFeature::GhostMessages, enabled);
	setEnabled(LocalFeature::GhostStories, enabled);
	setEnabled(LocalFeature::GhostOnline, enabled);
}

rpl::producer<> LocalFeatures::changes() const {
	return _changes.events();
}

bool LocalFeatures::shadowBanned(PeerId peer) const {
	return _blocked.contains(peer.value);
}

void LocalFeatures::toggleShadowBan(PeerId peer) {
	if (!_blocked.remove(peer.value)) {
		_blocked.insert(peer.value);
	}
	_session->data().refreshLocalVisibility();
	settingsChanged();
	_changes.fire({});
}

QJsonArray LocalFeatures::shadowBans() const {
	auto result = QJsonArray();
	for (const auto id : _blocked) {
		result.append(QString::number(id));
	}
	return result;
}

void LocalFeatures::rememberMessage(not_null<HistoryItem*> item) {
	if (!item->isRegular() || (!enabled(LocalFeature::EditHistory)
		&& !enabled(LocalFeature::KeepDeleted)
		&& !enabled(LocalFeature::KeepExpired))) {
		return;
	}
	const auto id = item->fullId();
	const auto text = item->originalText().text;
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	const auto photo = media ? media->photo() : nullptr;
	const auto mediaId = document ? u"document:"_q + QString::number(document->id)
		: photo ? u"photo:"_q + QString::number(photo->id) : QString();
	const auto entry = findEntry(id);
	const auto wantsFile = enabled(LocalFeature::EditHistory)
		&& (photo || document);
	if (entry
		&& entry->lastHash == LocalMessageIndex::VersionHash(text, mediaId)
		&& (!wantsFile || entry->lastHasFile)) {
		finishVersionMedia();
		return;
	}
	auto record = entry ? loadRecord(id) : QJsonObject();
	if (record.isEmpty()) {
		record[u"peer"] = QString::number(item->history()->peer->id.value);
		record[u"message"] = QString::number(item->id.bare);
		record[u"sender"] = QString::number(item->from()->id.value);
		record[u"chat"] = item->history()->peer->name();
		record[u"date"] = qint64(item->date());
	}
	const auto appended = LocalJournal::AppendVersion(record, text, mediaId, Now(),
		enabled(LocalFeature::EditHistory), kEventLimit);
	const auto before = record;
	if (enabled(LocalFeature::EditHistory)) {
		captureVersionMedia(item, record);
	}
	if (appended || before != record) {
		commitRecord(id, record);
	}
	finishVersionMedia();
}

void LocalFeatures::rememberExpiringMedia(not_null<HistoryItem*> item) {
	const auto id = item->fullId();
	if (!enabled(LocalFeature::KeepExpired) || item->out()
		|| !item->media() || !item->media()->ttlSeconds()) {
		_pendingMedia.remove(id);
		return;
	}
	rememberMessage(item);
	if (const auto entry = findEntry(id); entry && entry->recordHasFile) {
		_pendingMedia.remove(id);
		return;
	}
	auto record = loadRecord(id);
	saveMedia(item, record);
	if (record.contains(u"file"_q)) {
		commitRecord(id, record);
		_pendingMedia.remove(id);
	} else {
		_pendingMedia.emplace(id);
	}
}

void LocalFeatures::saveMedia(not_null<HistoryItem*> item, QJsonObject &record) {
	const auto media = item->media();
	if (!media || record.contains(u"file"_q)) {
		return;
	}
	saveMedia(media->photo(), media->document(), record);
}

void LocalFeatures::saveMedia(PhotoData *photo, DocumentData *document, QJsonObject &record) {
	if (record.contains(u"file"_q)) {
		return;
	}
	auto bytes = QByteArray();
	auto filename = QString();
	if (photo) {
		if (const auto view = photo->activeMediaView()) {
			bytes = view->imageBytes(Data::PhotoSize::Large);
			filename = u"photo.jpg"_q;
		}
	} else if (document) {
		if (const auto view = document->activeMediaView()) {
			bytes = view->bytes();
		}
		filename = document->filename();
		if (filename.isEmpty()) {
			filename = document->isVoiceMessage() ? u"voice.ogg"_q : u"media.bin"_q;
		}
		if (bytes.isEmpty()) {
			const auto &location = document->location(true);
			if (location.accessEnable()) {
				QFile file(location.name());
				if (file.open(QIODevice::ReadOnly) && file.size() <= kMediaLimit) {
					bytes = file.readAll();
				}
				location.accessDisable();
			}
		}
	}
	if (bytes.isEmpty() || bytes.size() > kMediaLimit) {
		return;
	}
	const auto key = QString::fromLatin1(QCryptographicHash::hash(
		bytes, QCryptographicHash::Sha256).toHex());
	if (!_files.contains(key)) {
		_session->local().writeLocalFeatureFile(key, bytes, true);
		_files[key] = QJsonObject{
			{ u"size"_q, qint64(bytes.size()) }, { u"time"_q, Now() },
		};
		filesChanged();
	}
	record[u"file"] = key;
	record[u"filename"] = filename;
}

void LocalFeatures::captureVersionMedia(not_null<HistoryItem*> item, QJsonObject &record) {
	const auto media = item->media();
	auto versions = record[u"versions"].toArray();
	if (!media || versions.isEmpty()) {
		return;
	}
	auto version = versions.last().toObject();
	if (version.contains(u"file"_q)) {
		return;
	}
	saveMedia(item, version);
	if (version.contains(u"file"_q)) {
		LocalJournal::AttachMedia(record, version[u"media"].toString(), version);
		return;
	}
	const auto photo = media->photo();
	const auto document = media->document();
	if ((!photo && !document) || item->forbidsSaving()
		|| (photo && photo->imageByteSize(Data::PhotoSize::Large) > kMediaLimit)
		|| (document && (document->size <= 0 || document->size > kMediaLimit))) {
		return;
	}
	const auto id = item->fullId();
	const auto mediaId = version[u"media"].toString();
	for (const auto &pending : _pendingAttachments) {
		if (pending.id == id && pending.media == mediaId) {
			return;
		}
	}
	if (_pendingAttachments.size() >= 128) {
		return;
	}
	auto pending = PendingAttachment{ .id = id, .media = mediaId };
	if (photo) {
		pending.photo = photo->createMediaView();
	} else {
		pending.document = document->createMediaView();
	}
	_pendingAttachments.push_back(std::move(pending));
	if (photo) {
		photo->load(Data::PhotoSize::Large, id);
	} else if (document->size <= Storage::kMaxFileInMemory) {
		document->save(id, QString());
	}
}

void LocalFeatures::finishVersionMedia() {
	for (auto i = _pendingAttachments.begin(); i != _pendingAttachments.end();) {
		if (!findEntry(i->id)) {
			i = _pendingAttachments.erase(i);
			continue;
		}
		auto attachment = QJsonObject();
		saveMedia(i->photo ? i->photo->owner().get() : nullptr,
			i->document ? i->document->owner().get() : nullptr, attachment);
		if (!attachment.contains(u"file"_q)) {
			++i;
			continue;
		}
		auto record = loadRecord(i->id);
		if (LocalJournal::AttachMedia(record, i->media, attachment)) {
			commitRecord(i->id, record);
		}
		i = _pendingAttachments.erase(i);
	}
}

bool LocalFeatures::keepDeleted(not_null<HistoryItem*> item, bool expired) {
	const auto id = item->fullId();
	if (item->locallyDeleted() && enabled(LocalFeature::KeepExpired)) {
		if (const auto entry = findEntry(id); entry && entry->expired) {
			return true;
		}
	}
	if (!item->isRegular() || item->out()
		|| !enabled(expired ? LocalFeature::KeepExpired : LocalFeature::KeepDeleted)) {
		return false;
	}
	rememberMessage(item);
	item->markLocallyDeleted();
	auto record = loadRecord(id);
	record[u"deleted"] = true;
	record[u"expired"] = expired;
	record[u"updated"] = Now();
	saveMedia(item, record);
	if (expired) {
		item->retainExpiredMediaLocally();
	}
	commitRecord(id, record);
	_session->data().requestItemViewRefresh(item);
	item->invalidateChatListEntry();
	return true;
}

bool LocalFeatures::deleted(FullMsgId id) const {
	const auto entry = findEntry(id);
	return entry && entry->deleted;
}

bool LocalFeatures::hasMessageRecord(FullMsgId id) const {
	return findEntry(id) != nullptr;
}

QJsonObject LocalFeatures::messageRecord(FullMsgId id) {
	return loadRecord(id);
}

int LocalFeatures::messageCount() const {
	return int(_entries.size());
}

int LocalFeatures::messageCount(PeerId chat) const {
	const auto i = _byChat.find(chat);
	return (i != end(_byChat)) ? int(i->second.size()) : 0;
}

auto LocalFeatures::messageEntries(PeerId chat) const
-> std::vector<const LocalMessageEntry*> {
	auto result = std::vector<const LocalMessageEntry*>();
	if (!chat) {
		result.reserve(_byUpdated.size());
		for (auto i = _byUpdated.rbegin(); i != _byUpdated.rend(); ++i) {
			result.push_back(findEntry(i->id));
		}
		return result;
	}
	const auto i = _byChat.find(chat);
	if (i == end(_byChat)) {
		return result;
	}
	result.reserve(i->second.size());
	for (const auto msg : i->second) {
		result.push_back(findEntry(FullMsgId(chat, msg)));
	}
	ranges::sort(result, ranges::greater(), &LocalMessageEntry::updated);
	return result;
}

int LocalFeatures::profileCount() const {
	return int(_profiles.size());
}

QJsonObject LocalFeatures::profileRecord(PeerId id) const {
	return _profiles[QString::number(id.value)].toObject();
}
QJsonArray LocalFeatures::profileRecords() const {
	return Records(_profiles);
}
QByteArray LocalFeatures::mediaBytes(const QString &key) const {
	return _files.contains(key) ? _session->local().readLocalFeatureFile(key) : QByteArray();
}

void LocalFeatures::observePeer(not_null<PeerData*> peer, bool realtime) {
	const auto user = peer->asUser();
	const auto profiles = enabled(LocalFeature::ProfileHistory);
	const auto online = enabled(LocalFeature::OnlineHistory);
	const auto blockHint = enabled(LocalFeature::BlockHint);
	if (!user || user == _session->user() || (!profiles && !online && !blockHint)) {
		return;
	}
	const auto key = QString::number(peer->id.value);
	auto record = _profiles[key].toObject();
	const auto previous = record[u"snapshot"].toObject();
	const auto now = base::unixtime::now();
	const auto lastseen = user->lastseen();
	const auto isOnline = lastseen.isOnline(now);

	// Fields are only compared once they are known, so that data
	// arriving later (like the bio from the full user info) is not
	// mistaken for a change.
	auto snapshot = previous;
	snapshot[u"name"] = user->name();
	snapshot[u"username"] = user->username();
	if (!user->userpicPhotoUnknown()) {
		snapshot[u"photo"] = QString::number(user->userpicPhotoId());
	}
	if (user->wasFullUpdated()) {
		snapshot[u"bio"] = user->about();
		const auto birthday = user->birthday();
		snapshot[u"birthday"] = birthday
			? Data::BirthdayText(birthday)
			: QString();
	}
	if (!user->phone().isEmpty()) {
		snapshot[u"phone"] = user->phone();
	}
	snapshot[u"premium"] = user->isPremium();
	snapshot[u"emojiStatus"] = QString::number(
		user->emojiStatusId().documentId);
	snapshot[u"lastSeenVisible"] = !lastseen.isHidden();
	snapshot[u"longAgo"] = lastseen.isLongAgo();
	snapshot[u"online"] = isOnline;
	snapshot[u"onlineTill"] = qint64(lastseen.onlineTill());

	const auto was = record;
	if (profiles) {
		for (const auto &field : {
				u"name"_q,
				u"username"_q,
				u"bio"_q,
				u"photo"_q,
				u"phone"_q,
				u"premium"_q,
				u"emojiStatus"_q,
				u"birthday"_q,
				u"lastSeenVisible"_q }) {
			if (previous.contains(field)
				&& snapshot.contains(field)
				&& previous[field] != snapshot[field]) {
				AppendEvent(record, {
					{ u"field"_q, field },
					{ u"before"_q, previous[field] },
					{ u"after"_q, snapshot[field] },
				});
			}
		}
	}
	if (online) {
		updateOnlineSessions(record, user, isOnline, realtime);
	}
	const auto photo = snapshot[u"photo"].toString();
	if (!previous.isEmpty() && !photo.isEmpty() && photo != u"0"_q) {
		record[u"hadPhoto"] = true;
	}
	if (blockHint
		&& photo == u"0"_q
		&& lastseen.isLongAgo()
		&& record[u"hadPhoto"].toBool()
		&& !record[u"possibleBlock"].toBool()) {
		record[u"possibleBlock"] = true;
		AppendEvent(record, {
			{ u"field"_q, u"possibleBlock"_q },
			{ u"after"_q, true },
		});
	} else if (!photo.isEmpty() && photo != u"0"_q && !lastseen.isLongAgo()) {
		record[u"possibleBlock"] = false;
	}
	record[u"peer"] = key;
	record[u"name"] = user->name();
	record[u"snapshot"] = snapshot;
	if (record == was) {
		return;
	}
	const auto persistent = [](QJsonObject value) {
		value.remove(u"lastObservedOnline"_q);
		auto snapshot = value[u"snapshot"].toObject();
		snapshot.remove(u"onlineTill"_q);
		value[u"snapshot"] = snapshot;
		return value;
	};
	if (persistent(record) == persistent(was)) {
		_profiles[key] = record;
		_volatileShards.insert(ProfileShard(key));
		if (isOnline && online) {
			_onlineTimer.callOnce(1000);
		}
		return;
	}
	record[u"updated"] = Now();
	setProfile(key, record);
	if (isOnline && online) {
		_onlineTimer.callOnce(1000);
	}
	_profileUpdates.fire_copy(peer->id);
}

void LocalFeatures::updateOnlineSessions(
		QJsonObject &record,
		not_null<UserData*> user,
		bool online,
		bool realtime) {
	const auto now = base::unixtime::now();
	auto sessions = record[u"sessions"].toArray();
	const auto open = !sessions.isEmpty()
		&& !sessions.last().toArray().at(1).toInteger();
	if (online) {
		record[u"lastObservedOnline"] = qint64(now);
		if (!open) {
			sessions.append(QJsonArray{ qint64(now), qint64(0) });
			if (realtime && _watchOnline.contains(user->id.value)) {
				if (const auto window = _session->tryResolveWindow(user)) {
					window->showToast(tr::lng_local_watch_online_toast(
						tr::now,
						lt_user,
						user->name()));
				}
			}
		}
	} else if (open) {
		auto last = sessions.last().toArray();
		const auto from = last.at(0).toInteger();
		const auto till = qint64(user->lastseen().onlineTill());
		const auto seen = record[u"lastObservedOnline"].toInteger();
		// Prefer the exact time Telegram reports, otherwise the last
		// moment the user was still seen online by this client.
		const auto end = (till > from && till <= now)
			? till
			: std::max(seen, from);
		last[1] = std::max(end, from + 1);
		sessions[sessions.size() - 1] = last;
	}
	while (sessions.size() > kSessionLimit) {
		sessions.removeFirst();
	}
	record[u"sessions"] = sessions;
}

void LocalFeatures::migrateProfiles() {
	for (auto i = _profiles.begin(); i != _profiles.end(); ++i) {
		auto record = i.value().toObject();
		if (record.contains(u"sessions"_q)) {
			continue;
		}
		// Older journals logged online changes as separate events.
		auto events = QJsonArray();
		auto sessions = QJsonArray();
		for (const auto &value : record[u"events"].toArray()) {
			const auto event = value.toObject();
			if (event[u"field"].toString() != u"online"_q) {
				events.append(event);
				continue;
			}
			const auto time = event[u"time"].toInteger();
			const auto open = !sessions.isEmpty()
				&& !sessions.last().toArray().at(1).toInteger();
			if (event[u"after"].toBool() && !open) {
				sessions.append(QJsonArray{ time, qint64(0) });
			} else if (!event[u"after"].toBool() && open) {
				auto last = sessions.last().toArray();
				last[1] = std::max(time, last.at(0).toInteger() + 1);
				sessions[sessions.size() - 1] = last;
			}
		}
		record[u"events"] = events;
		record[u"sessions"] = sessions;
		// Older snapshots stored an empty bio before it was loaded.
		auto snapshot = record[u"snapshot"].toObject();
		if (snapshot[u"bio"].toString().isEmpty()) {
			snapshot.remove(u"bio"_q);
			record[u"snapshot"] = snapshot;
		}
		i.value() = record;
		_dirtyShards.insert(ProfileShard(i.key()));
	}
}

TimeId LocalFeatures::lastObservedOnline(PeerId id) const {
	if (!enabled(LocalFeature::OnlineHistory)) {
		return 0;
	}
	const auto record = profileRecord(id);
	const auto sessions = record[u"sessions"].toArray();
	if (!sessions.isEmpty()) {
		const auto last = sessions.last().toArray();
		if (const auto till = last.at(1).toInteger()) {
			return TimeId(till);
		}
	}
	return TimeId(record[u"lastObservedOnline"].toInteger());
}

std::vector<OnlineSession> LocalFeatures::onlineSessions(PeerId id) const {
	auto result = std::vector<OnlineSession>();
	const auto sessions = profileRecord(id)[u"sessions"].toArray();
	result.reserve(sessions.size());
	for (const auto &value : sessions) {
		const auto session = value.toArray();
		result.push_back({
			.from = TimeId(session.at(0).toInteger()),
			.till = TimeId(session.at(1).toInteger()),
		});
	}
	return result;
}

bool LocalFeatures::watchingOnline(PeerId id) const {
	return _watchOnline.contains(id.value);
}

void LocalFeatures::toggleWatchOnline(PeerId id) {
	if (!_watchOnline.remove(id.value)) {
		_watchOnline.insert(id.value);
	}
	settingsChanged();
	_profileUpdates.fire_copy(id);
}

int LocalFeatures::watchedCount() const {
	return int(_watchOnline.size());
}

rpl::producer<PeerId> LocalFeatures::profileUpdates() const {
	return _profileUpdates.events();
}

void LocalFeatures::clearProfiles() {
	for (const auto &value : _profiles) {
		for (const auto &avatar : value.toObject()[u"avatars"].toObject()) {
			const auto key = avatar.toObject()[u"key"].toString();
			if (!key.isEmpty() && _files.contains(key)) {
				_session->local().removeLocalFeatureFile(key);
				_files.remove(key);
			}
		}
	}
	_profiles = QJsonObject();
	for (auto shard = 0; shard != kProfileShards; ++shard) {
		_dirtyShards.insert(shard);
	}
	_watchOnline.clear();
	filesChanged();
	settingsChanged();
	_changes.fire({});
}

void LocalFeatures::captureUserpics() {
	if (!enabled(LocalFeature::ProfileHistory)) { return; }
	auto updates = std::vector<std::pair<QString, QJsonObject>>();
	for (auto i = _profiles.constBegin(); i != _profiles.constEnd(); ++i) {
		const auto peer = _session->data().peerLoaded(PeerId(PeerIdHelper(i.key().toULongLong())));
		if (!peer || peer->userpicPhotoUnknown() || !peer->userpicPhotoId()) { continue; }
		auto record = i.value().toObject();
		auto avatars = record[u"avatars"].toObject();
		const auto photo = QString::number(peer->userpicPhotoId());
		if (avatars.contains(photo)) { continue; }
		const auto view = peer->activeUserpicView();
		if (!view.cloud || view.cloud->isNull()) { continue; }
		auto bytes = QByteArray();
		QBuffer buffer(&bytes);
		buffer.open(QIODevice::WriteOnly);
		view.cloud->save(&buffer, "PNG");
		const auto key = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
		if (!_files.contains(key)) {
			_session->local().writeLocalFeatureFile(key, bytes, true);
			_files[key] = QJsonObject{ { u"size"_q, qint64(bytes.size()) }, { u"time"_q, Now() } };
			filesChanged();
		}
		avatars[photo] = QJsonObject{ { u"key"_q, key }, { u"time"_q, Now() } };
		while (avatars.size() > kEventLimit + 1) {
			auto oldest = avatars.begin();
			for (auto j = avatars.begin(); j != avatars.end(); ++j) {
				if (j.value().toObject()[u"time"].toInteger()
					< oldest.value().toObject()[u"time"].toInteger()) { oldest = j; }
			}
			avatars.erase(oldest);
		}
		record[u"avatars"] = avatars;
		updates.emplace_back(i.key(), record);
	}
	for (const auto &[id, record] : updates) {
		setProfile(id, record);
		_profileUpdates.fire(PeerId(PeerIdHelper(id.toULongLong())));
	}
}

void LocalFeatures::checkOnlineExpiry() {
	if (!enabled(LocalFeature::OnlineHistory)) {
		return;
	}
	_session->data().enumerateUsers([=](not_null<UserData*> user) {
		if (profileRecord(user->id)[u"snapshot"].toObject()[u"online"].toBool()) {
			observePeer(user);
		}
	});
	const auto now = base::unixtime::now();
	auto next = TimeId(0);
	for (const auto &value : _profiles) {
		const auto snapshot = value.toObject()[u"snapshot"].toObject();
		const auto till = TimeId(snapshot[u"onlineTill"].toInteger());
		if (snapshot[u"online"].toBool() && till > now && (!next || till < next)) {
			next = till;
		}
	}
	if (next) {
		_onlineTimer.callOnce((next - now) * 1000);
	}
}

QString LocalFeatures::registrationEstimate(UserId id) const {
	if (!enabled(LocalFeature::RegistrationEstimate)) {
		return {};
	}
	static const auto samples = [] {
		QFile file(u":/gui/local_features/registration_samples.json"_q);
		file.open(QIODevice::ReadOnly);
		return QJsonDocument::fromJson(file.readAll()).array();
	}();
	if (samples.isEmpty()) {
		return {};
	}
	for (auto i = 1; i < samples.size(); ++i) {
		const auto lower = samples[i - 1].toObject();
		const auto upper = samples[i].toObject();
		const auto fromId = lower[u"id"].toString().toULongLong();
		const auto tillId = upper[u"id"].toString().toULongLong();
		if (id.bare >= fromId && id.bare <= tillId && tillId > fromId) {
			const auto from = QDate::fromString(lower[u"date"].toString(), Qt::ISODate);
			const auto till = QDate::fromString(upper[u"date"].toString(), Qt::ISODate);
			const auto date = from.addDays(qint64(double(id.bare - fromId)
				/ double(tillId - fromId) * from.daysTo(till)));
			return u"≈ "_q + date.toString(u"yyyy-MM"_q)
				+ u" ("_q + from.toString(u"yyyy-MM"_q) + u" — "_q
				+ till.toString(u"yyyy-MM"_q) + ')';
		}
	}
	return u"> "_q + samples.last().toObject()[u"date"].toString().left(7);
}

void LocalFeatures::settingsChanged() {
	_metaDirty = true;
	scheduleFlush();
}

void LocalFeatures::filesChanged() {
	_filesDirty = true;
	scheduleFlush();
}

void LocalFeatures::scheduleFlush() {
	if (!_flushTimer.isActive()) {
		_flushTimer.callOnce(kFlushDelay);
	}
}

void LocalFeatures::setProfile(const QString &key, const QJsonObject &record) {
	_profiles[key] = record;
	_dirtyShards.insert(ProfileShard(key));
	scheduleFlush();
}

void LocalFeatures::removeProfile(const QString &key) {
	_profiles.remove(key);
	_dirtyShards.insert(ProfileShard(key));
	scheduleFlush();
}

void LocalFeatures::openArchive() {
	const auto path = _session->local().localFeaturesArchivePath();
	QDir().mkpath(path);
	auto settings = Database::Settings();
	settings.trackEstimatedTime = false;
	settings.totalSizeLimit = 0;
	settings.totalTimeLimit = 0;
	_archive = std::make_unique<Database>(path, settings);
	auto error = Error::NoError();
	auto semaphore = crl::semaphore();
	_archive->open(_session->local().cacheKey(), [&](Error result) {
		error = result;
		semaphore.release();
	});
	semaphore.acquire();
	if (error.type != Error::Type::None) {
		LOG(("Local Features Error: could not open the archive, type %1."
			).arg(int(error.type)));
		_archive = nullptr;
	}
}

QByteArray LocalFeatures::syncGet(const Key &key) {
	if (!_archive) {
		return {};
	}
	auto result = QByteArray();
	auto semaphore = crl::semaphore();
	_archive->get(key, [&](QByteArray &&value) {
		result = std::move(value);
		semaphore.release();
	});
	semaphore.acquire();
	return result;
}

void LocalFeatures::put(const Key &key, QByteArray &&bytes) {
	_archive->put(key, std::move(bytes), [failed = _writeFailed](Error e) {
		if (e.type != Error::Type::None) {
			*failed = true;
		}
	});
}

void LocalFeatures::loadArchive() {
	const auto meta = LocalMessageIndex::ParseRecord(syncGet(kMetaKey));
	if (meta[u"version"].toInt() != kMetaVersion) {
		const auto state = QJsonDocument::fromJson(
			_session->local().readLocalFeatureFile(u"journal"_q)).object();
		if (state[u"version"].toInt() == 1) {
			importJournal(state);
		}
		return;
	}
	_settings = meta[u"settings"].toObject();
	for (const auto &id : meta[u"blocked"].toArray()) {
		_blocked.insert(id.toString().toULongLong());
	}
	for (const auto &id : meta[u"watchOnline"].toArray()) {
		_watchOnline.insert(id.toString().toULongLong());
	}
	_files = LocalMessageIndex::ParseRecord(syncGet(kFilesKey));
	for (auto shard = 0; shard != kProfileShards; ++shard) {
		const auto part = LocalMessageIndex::ParseRecord(
			syncGet(ShardKey(shard)));
		for (auto i = part.begin(); i != part.end(); ++i) {
			_profiles.insert(i.key(), i.value());
		}
	}
	_firstBucket = meta[u"firstBucket"].toInt();
	_tailBucket = std::max(meta[u"tailBucket"].toInt(), _firstBucket);
	for (auto bucket = _firstBucket; bucket <= _tailBucket; ++bucket) {
		auto entries = LocalMessageIndex::ParseBucket(
			syncGet(BucketKey(bucket)),
			bucket);
		for (auto &entry : entries) {
			if (const auto old = findEntry(EntryId(entry))) {
				_dirtyBuckets.insert(old->bucket);
				unindexEntry(EntryId(entry));
			}
			insertEntry(std::move(entry));
		}
	}
}

void LocalFeatures::importJournal(const QJsonObject &state) {
	_settings = state[u"settings"].toObject();
	_profiles = state[u"profiles"].toObject();
	_files = state[u"files"].toObject();
	for (const auto &id : state[u"blocked"].toArray()) {
		_blocked.insert(id.toString().toULongLong());
	}
	for (const auto &id : state[u"watchOnline"].toArray()) {
		_watchOnline.insert(id.toString().toULongLong());
	}
	migrateProfiles();
	auto records = std::vector<QJsonObject>();
	for (const auto &value : state[u"messages"].toObject()) {
		records.push_back(value.toObject());
	}
	ranges::sort(records, ranges::less(), [](const QJsonObject &record) {
		return record[u"updated"].toInteger();
	});
	for (const auto &record : records) {
		const auto id = FullMsgId(
			PeerId(record[u"peer"].toString().toULongLong()),
			MsgId(record[u"message"].toString().toLongLong()));
		if (id.peer && id.msg) {
			commitRecord(id, record);
		}
	}
	for (auto shard = 0; shard != kProfileShards; ++shard) {
		_dirtyShards.insert(shard);
	}
	_metaDirty = _filesDirty = true;
	if (!_archive) {
		return;
	}
	flush();
	_archive->sync();
	if (!*_writeFailed) {
		_session->local().removeLocalFeatureFile(u"journal"_q);
	}
}

const LocalMessageEntry *LocalFeatures::findEntry(FullMsgId id) const {
	const auto i = _entries.find(id);
	return (i != end(_entries)) ? &i->second : nullptr;
}

QJsonObject LocalFeatures::loadRecord(FullMsgId id) {
	if (!findEntry(id)) {
		return {};
	} else if (const auto i = _loaded.find(id); i != end(_loaded)) {
		return i->second;
	}
	auto record = LocalMessageIndex::ParseRecord(syncGet(RecordKey(id)));
	if (!record.isEmpty()) {
		_loaded.emplace(id, record);
	}
	return record;
}

void LocalFeatures::commitRecord(FullMsgId id, const QJsonObject &record) {
	auto entry = LocalMessageIndex::EntryFromRecord(
		record,
		qint32(LocalMessageIndex::SerializeRecord(record).size()));
	entry.peer = id.peer.value;
	entry.msg = id.msg.bare;
	if (const auto old = findEntry(id)) {
		_dirtyBuckets.insert(old->bucket);
		unindexEntry(id);
	}
	if (_buckets[_tailBucket].size() >= kBucketSize) {
		++_tailBucket;
		_metaDirty = true;
	}
	entry.bucket = _tailBucket;
	_dirtyBuckets.insert(_tailBucket);
	insertEntry(std::move(entry));
	_loaded[id] = record;
	_dirtyRecords.insert(id);
	_removedRecords.erase(id);
	scheduleFlush();
}

void LocalFeatures::insertEntry(LocalMessageEntry entry) {
	const auto id = EntryId(entry);
	_byUpdated.insert({ entry.updated, id });
	_byChat[id.peer].insert(id.msg);
	_buckets[entry.bucket].insert(id);
	_archiveSize += entry.size;
	_entries.insert_or_assign(id, std::move(entry));
}

void LocalFeatures::unindexEntry(FullMsgId id) {
	const auto i = _entries.find(id);
	if (i == end(_entries)) {
		return;
	}
	const auto &entry = i->second;
	_byUpdated.erase({ entry.updated, id });
	if (const auto chat = _byChat.find(id.peer); chat != end(_byChat)) {
		chat->second.erase(id.msg);
		if (chat->second.empty()) {
			_byChat.erase(chat);
		}
	}
	if (const auto bucket = _buckets.find(entry.bucket)
		; bucket != end(_buckets)) {
		bucket->second.erase(id);
		if (bucket->second.empty() && entry.bucket != _tailBucket) {
			_buckets.erase(bucket);
		}
	}
	_archiveSize -= entry.size;
	_entries.erase(i);
}

void LocalFeatures::removeEntry(FullMsgId id) {
	if (const auto entry = findEntry(id)) {
		_dirtyBuckets.insert(entry->bucket);
		unindexEntry(id);
		_loaded.erase(id);
		_dirtyRecords.erase(id);
		_removedRecords.insert(id);
		scheduleFlush();
	}
}

void LocalFeatures::trim() {
	while (!_byUpdated.empty()
		&& (int(_entries.size()) > kMessageLimit
			|| _archiveSize > kRecordsLimit)) {
		removeEntry(_byUpdated.begin()->id);
	}
	if (_profiles.size() > kProfileLimit) {
		auto list = std::vector<std::pair<qint64, QString>>();
		list.reserve(_profiles.size());
		for (auto i = _profiles.constBegin(); i != _profiles.constEnd(); ++i) {
			list.emplace_back(
				i.value().toObject()[u"updated"].toInteger(),
				i.key());
		}
		ranges::sort(list, ranges::less(), [](const auto &pair) {
			return pair.first;
		});
		const auto extra = int(list.size()) - kProfileLimit;
		for (auto i = 0; i != extra; ++i) {
			removeProfile(list[i].second);
		}
	}
	auto total = qint64(0);
	for (const auto &value : _files) {
		total += value.toObject()[u"size"].toInteger();
	}
	while (total > kArchiveLimit && !_files.isEmpty()) {
		auto oldest = _files.begin();
		for (auto i = _files.begin(); i != _files.end(); ++i) {
			if (i.value().toObject()[u"time"].toInteger()
				< oldest.value().toObject()[u"time"].toInteger()) {
				oldest = i;
			}
		}
		total -= oldest.value().toObject()[u"size"].toInteger();
		_session->local().removeLocalFeatureFile(oldest.key());
		_files.erase(oldest);
		_filesDirty = true;
	}
}

void LocalFeatures::writeMeta() {
	auto blocked = QJsonArray();
	for (const auto id : _blocked) {
		blocked.append(QString::number(id));
	}
	auto watchOnline = QJsonArray();
	for (const auto id : _watchOnline) {
		watchOnline.append(QString::number(id));
	}
	put(kMetaKey, LocalMessageIndex::SerializeRecord(QJsonObject{
		{ u"version"_q, kMetaVersion },
		{ u"settings"_q, _settings },
		{ u"blocked"_q, blocked },
		{ u"watchOnline"_q, watchOnline },
		{ u"firstBucket"_q, _firstBucket },
		{ u"tailBucket"_q, _tailBucket },
	}));
}

void LocalFeatures::writeFiles() {
	put(kFilesKey, LocalMessageIndex::SerializeRecord(_files));
}

void LocalFeatures::writeBucket(qint32 bucket) {
	const auto i = _buckets.find(bucket);
	if (i == end(_buckets) || i->second.empty()) {
		_archive->remove(BucketKey(bucket));
		return;
	}
	auto entries = std::vector<const LocalMessageEntry*>();
	entries.reserve(i->second.size());
	for (const auto &id : i->second) {
		entries.push_back(findEntry(id));
	}
	ranges::sort(entries, ranges::less(), &LocalMessageEntry::updated);
	put(BucketKey(bucket), LocalMessageIndex::SerializeBucket(entries));
}

void LocalFeatures::writeProfileShard(int shard) {
	auto part = QJsonObject();
	for (auto i = _profiles.constBegin(); i != _profiles.constEnd(); ++i) {
		if (ProfileShard(i.key()) == shard) {
			part.insert(i.key(), i.value());
		}
	}
	if (part.isEmpty()) {
		_archive->remove(ShardKey(shard));
	} else {
		put(ShardKey(shard), LocalMessageIndex::SerializeRecord(part));
	}
}

void LocalFeatures::flush() {
	trim();
	if (!_archive) {
		_dirtyRecords.clear();
		_removedRecords.clear();
		_dirtyBuckets.clear();
		_dirtyShards.clear();
		_metaDirty = _filesDirty = false;
		return;
	}
	for (const auto &id : base::take(_removedRecords)) {
		_archive->remove(RecordKey(id));
	}
	for (const auto &id : base::take(_dirtyRecords)) {
		if (const auto i = _loaded.find(id); i != end(_loaded)) {
			put(RecordKey(id), LocalMessageIndex::SerializeRecord(i->second));
		}
	}
	for (const auto bucket : base::take(_dirtyBuckets)) {
		writeBucket(bucket);
	}
	while (_firstBucket < _tailBucket && !_buckets.contains(_firstBucket)) {
		++_firstBucket;
		_metaDirty = true;
	}
	for (const auto shard : base::take(_dirtyShards)) {
		_volatileShards.erase(shard);
		writeProfileShard(shard);
	}
	if (base::take(_filesDirty)) {
		writeFiles();
	}
	if (base::take(_metaDirty)) {
		writeMeta();
	}
	if (_loaded.size() > kLoadedCache) {
		_loaded.clear();
	}
}

} // namespace Core
