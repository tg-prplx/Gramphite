#include "../../Telegram/SourceFiles/core/local_message_index.h"

void Check(bool passed, const char *description);

void RunMessageIndexChecks() {
	using namespace Core::LocalMessageIndex;
	auto version = QJsonObject();
	version[u"text"] = QString(kPreviewLength + 20, QChar(u'я'));
	version[u"media"] = QString::fromUtf16(u"photo:42");
	version[u"time"] = qint64(50);
	version[u"file"] = QString::fromUtf16(u"hash");
	auto record = QJsonObject();
	record[u"peer"] = QString::fromUtf16(u"9223372036854775000");
	record[u"message"] = QString::fromUtf16(u"777");
	record[u"sender"] = QString::fromUtf16(u"12345");
	record[u"chat"] = QString::fromUtf16(u"Чат 😀");
	record[u"date"] = qint64(40);
	record[u"updated"] = qint64(50);
	record[u"deleted"] = true;
	record[u"versions"] = QJsonArray{ version, version };
	const auto bytes = SerializeRecord(record);
	Check(ParseRecord(bytes) == record, "record survives serialization");
	const auto entry = EntryFromRecord(record, qint32(bytes.size()));
	Check(entry.peer == 9223372036854775000ULL, "large peer ids are kept exactly");
	Check(entry.preview.size() == kPreviewLength, "preview is truncated");
	Check(entry.versions == 2 && entry.deleted && !entry.expired, "flags read from record");
	Check(entry.media == MediaKind::Photo && entry.lastHasFile, "media kind and file flag read");
	Check(entry.lastHash == VersionHash(version[u"text"].toString(), version[u"media"].toString()),
		"latest version hash lets unchanged updates skip the record");
	Check(VersionHash(u"a"_qs, u"b"_qs) != VersionHash(u"ab"_qs, QString()),
		"text and media are separated in the hash");
	auto other = entry;
	other.msg = 778;
	other.deleted = false;
	other.expired = true;
	const auto bucket = SerializeBucket({ &entry, &other });
	const auto parsed = ParseBucket(bucket, 7);
	Check(parsed.size() == 2, "bucket keeps all entries");
	Check(parsed[0].chat == entry.chat && parsed[0].preview == entry.preview,
		"bucket keeps Unicode text");
	Check(parsed[0].lastHash == entry.lastHash && parsed[0].size == entry.size,
		"bucket keeps hash and size");
	Check(parsed[1].msg == 778 && parsed[1].expired && !parsed[1].deleted,
		"bucket keeps per-entry flags");
	Check(parsed[0].bucket == 7 && parsed[1].bucket == 7, "bucket id assigned on parse");
	Check(ParseBucket(bucket.left(bucket.size() - 3), 1).empty(),
		"truncated bucket is rejected");
	Check(ParseBucket(QByteArray(), 1).empty(), "missing bucket is empty");
}
