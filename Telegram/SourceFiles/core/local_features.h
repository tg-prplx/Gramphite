/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include <rpl/event_stream.h>
#include "core/local_message_index.h"
#include "data/data_msg_id.h"
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QSet>

#include <set>
#include <unordered_map>
#include <unordered_set>

class HistoryItem;
class PeerData;
class PhotoData;
class DocumentData;

namespace Data {
class PhotoMedia;
class DocumentMedia;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace Storage::Cache {
class Database;
struct Key;
} // namespace Storage::Cache

namespace Core {

enum class LocalFeature {
	GhostMessages,
	GhostStories,
	GhostOnline,
	EditHistory,
	KeepDeleted,
	KeepExpired,
	CopyProtected,
	ProfileHistory,
	OnlineHistory,
	BlockHint,
	RegistrationEstimate,
};

using LocalMessageEntry = LocalMessageIndex::Entry;

struct OnlineSession {
	TimeId from = 0;
	TimeId till = 0; // Zero while the session is still going on.
};

class LocalFeatures final {
public:
	explicit LocalFeatures(not_null<Main::Session*> session);
	~LocalFeatures();

	[[nodiscard]] bool enabled(LocalFeature feature) const;
	void setEnabled(LocalFeature feature, bool enabled);
	[[nodiscard]] bool ghostMode() const;
	void setGhostMode(bool enabled);
	[[nodiscard]] rpl::producer<> changes() const;
	[[nodiscard]] bool shadowBanned(PeerId peer) const;
	void toggleShadowBan(PeerId peer);
	[[nodiscard]] QJsonArray shadowBans() const;
	void rememberMessage(not_null<HistoryItem*> item);
	void rememberExpiringMedia(not_null<HistoryItem*> item);
	[[nodiscard]] bool keepDeleted(not_null<HistoryItem*> item, bool expired = false);
	[[nodiscard]] bool deleted(FullMsgId id) const;
	[[nodiscard]] bool hasMessageRecord(FullMsgId id) const;
	[[nodiscard]] QJsonObject messageRecord(FullMsgId id);
	[[nodiscard]] int messageCount() const;
	[[nodiscard]] int messageCount(PeerId chat) const;
	[[nodiscard]] std::vector<const LocalMessageEntry*> messageEntries(
		PeerId chat = PeerId()) const;
	[[nodiscard]] QJsonObject profileRecord(PeerId id) const;
	[[nodiscard]] int profileCount() const;
	[[nodiscard]] QJsonArray profileRecords() const;
	[[nodiscard]] QByteArray mediaBytes(const QString &key) const;
	[[nodiscard]] QString registrationEstimate(UserId id) const;
	[[nodiscard]] TimeId lastObservedOnline(PeerId id) const;
	[[nodiscard]] std::vector<OnlineSession> onlineSessions(PeerId id) const;
	[[nodiscard]] bool watchingOnline(PeerId id) const;
	void toggleWatchOnline(PeerId id);
	[[nodiscard]] int watchedCount() const;
	[[nodiscard]] rpl::producer<PeerId> profileUpdates() const;
	void clearProfiles();
	void flush();

private:
	void observePeer(not_null<PeerData*> peer, bool realtime = false);
	void updateOnlineSessions(
		QJsonObject &record,
		not_null<UserData*> user,
		bool online,
		bool realtime);
	void migrateProfiles();
	void checkOnlineExpiry();
	void captureUserpics();
	void saveMedia(not_null<HistoryItem*> item, QJsonObject &record);
	void saveMedia(PhotoData *photo, DocumentData *document, QJsonObject &record);
	void captureVersionMedia(not_null<HistoryItem*> item, QJsonObject &record);
	void finishVersionMedia();
	void trim();

	void openArchive();
	void loadArchive();
	void importJournal(const QJsonObject &state);
	[[nodiscard]] QByteArray syncGet(const Storage::Cache::Key &key);
	void put(const Storage::Cache::Key &key, QByteArray &&bytes);
	[[nodiscard]] const LocalMessageEntry *findEntry(FullMsgId id) const;
	[[nodiscard]] QJsonObject loadRecord(FullMsgId id);
	void commitRecord(FullMsgId id, const QJsonObject &record);
	void insertEntry(LocalMessageEntry entry);
	void unindexEntry(FullMsgId id);
	void removeEntry(FullMsgId id);
	void setProfile(const QString &key, const QJsonObject &record);
	void removeProfile(const QString &key);
	void settingsChanged();
	void filesChanged();
	void scheduleFlush();
	void writeMeta();
	void writeFiles();
	void writeBucket(qint32 bucket);
	void writeProfileShard(int shard);

	struct EntryKey {
		qint64 updated = 0;
		FullMsgId id;

		friend inline auto operator<=>(EntryKey, EntryKey) = default;
		friend inline bool operator==(EntryKey, EntryKey) = default;
	};

	not_null<Main::Session*> _session;
	std::unique_ptr<Storage::Cache::Database> _archive;
	QJsonObject _settings;
	QJsonObject _profiles;
	QJsonObject _files;
	QSet<quint64> _blocked;
	QSet<quint64> _watchOnline;

	std::unordered_map<FullMsgId, LocalMessageEntry> _entries;
	std::set<EntryKey> _byUpdated;
	std::unordered_map<PeerId, std::set<MsgId>> _byChat;
	std::unordered_map<qint32, std::unordered_set<FullMsgId>> _buckets;
	std::unordered_map<FullMsgId, QJsonObject> _loaded;
	qint64 _archiveSize = 0;
	qint32 _firstBucket = 0;
	qint32 _tailBucket = 0;

	std::unordered_set<FullMsgId> _dirtyRecords;
	std::unordered_set<FullMsgId> _removedRecords;
	std::unordered_set<qint32> _dirtyBuckets;
	std::unordered_set<int> _dirtyShards;
	std::unordered_set<int> _volatileShards;
	bool _metaDirty = false;
	bool _filesDirty = false;
	std::shared_ptr<std::atomic<bool>> _writeFailed
		= std::make_shared<std::atomic<bool>>(false);

	base::flat_set<FullMsgId> _pendingMedia;
	struct PendingAttachment {
		FullMsgId id;
		QString media;
		std::shared_ptr<Data::PhotoMedia> photo;
		std::shared_ptr<Data::DocumentMedia> document;
	};
	std::vector<PendingAttachment> _pendingAttachments;
	base::Timer _flushTimer;
	base::Timer _onlineTimer;
	rpl::event_stream<> _changes;
	rpl::event_stream<PeerId> _profileUpdates;
	rpl::lifetime _lifetime;

};

} // namespace Core
