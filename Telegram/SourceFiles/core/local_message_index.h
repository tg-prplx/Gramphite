/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDataStream>
#include <QtCore/QIODevice>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <vector>

namespace Core::LocalMessageIndex {

inline constexpr auto kPreviewLength = 100;
inline constexpr auto kBucketVersion = qint32(1);

enum class MediaKind : quint8 {
	None,
	Photo,
	Document,
};

struct Entry {
	quint64 peer = 0;
	qint64 msg = 0;
	quint64 sender = 0;
	QString chat;
	QString preview;
	qint64 date = 0;
	qint64 updated = 0;
	quint64 lastHash = 0;
	qint32 versions = 0;
	qint32 size = 0;
	qint32 bucket = 0;
	MediaKind media = MediaKind::None;
	bool deleted = false;
	bool expired = false;
	bool recordHasFile = false;
	bool lastHasFile = false;
};

[[nodiscard]] inline quint64 VersionHash(
		const QString &text,
		const QString &media) {
	auto hash = QCryptographicHash(QCryptographicHash::Sha1);
	hash.addData(text.toUtf8());
	hash.addData(QByteArray(1, '\0'));
	hash.addData(media.toUtf8());
	const auto result = hash.result();
	auto value = quint64(0);
	for (auto i = 0; i != 8; ++i) {
		value = (value << 8) | quint8(result[i]);
	}
	return value;
}

[[nodiscard]] inline MediaKind KindFromMedia(const QString &media) {
	return media.isEmpty()
		? MediaKind::None
		: media.startsWith(u"photo:")
		? MediaKind::Photo
		: MediaKind::Document;
}

[[nodiscard]] inline QByteArray SerializeRecord(const QJsonObject &record) {
	return QJsonDocument(record).toJson(QJsonDocument::Compact);
}

[[nodiscard]] inline QJsonObject ParseRecord(const QByteArray &bytes) {
	return bytes.isEmpty() ? QJsonObject() : QJsonDocument::fromJson(bytes).object();
}

// Everything the archive lists and checks without reading the record.
[[nodiscard]] inline Entry EntryFromRecord(
		const QJsonObject &record,
		qint32 size) {
	const auto versions = record[u"versions"].toArray();
	const auto last = versions.isEmpty()
		? QJsonObject()
		: versions.last().toObject();
	const auto text = last[u"text"].toString();
	const auto media = last[u"media"].toString();
	auto result = Entry();
	result.peer = record[u"peer"].toString().toULongLong();
	result.msg = record[u"message"].toString().toLongLong();
	result.sender = record[u"sender"].toString().toULongLong();
	result.chat = record[u"chat"].toString();
	result.preview = text.left(kPreviewLength);
	result.date = record[u"date"].toInteger();
	result.updated = record[u"updated"].toInteger();
	result.lastHash = VersionHash(text, media);
	result.versions = qint32(versions.size());
	result.size = size;
	result.media = KindFromMedia(media);
	result.deleted = record[u"deleted"].toBool();
	result.expired = record[u"expired"].toBool();
	result.recordHasFile = record.contains(u"file");
	result.lastHasFile = last.contains(u"file");
	return result;
}

[[nodiscard]] inline QByteArray SerializeBucket(
		const std::vector<const Entry*> &entries) {
	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << kBucketVersion << qint32(entries.size());
	for (const auto entry : entries) {
		auto flags = quint8(entry->media);
		flags |= entry->deleted ? 0x04 : 0;
		flags |= entry->expired ? 0x08 : 0;
		flags |= entry->recordHasFile ? 0x10 : 0;
		flags |= entry->lastHasFile ? 0x20 : 0;
		stream
			<< entry->peer
			<< entry->msg
			<< entry->sender
			<< entry->chat
			<< entry->preview
			<< entry->date
			<< entry->updated
			<< entry->lastHash
			<< entry->versions
			<< entry->size
			<< flags;
	}
	return result;
}

[[nodiscard]] inline std::vector<Entry> ParseBucket(
		const QByteArray &bytes,
		qint32 bucket) {
	auto result = std::vector<Entry>();
	if (bytes.isEmpty()) {
		return result;
	}
	auto stream = QDataStream(bytes);
	stream.setVersion(QDataStream::Qt_5_1);
	auto version = qint32();
	auto count = qint32();
	stream >> version >> count;
	if (version != kBucketVersion
		|| count < 0
		|| stream.status() != QDataStream::Ok) {
		return result;
	}
	result.reserve(count);
	for (auto i = 0; i != count; ++i) {
		auto entry = Entry();
		auto flags = quint8();
		stream
			>> entry.peer
			>> entry.msg
			>> entry.sender
			>> entry.chat
			>> entry.preview
			>> entry.date
			>> entry.updated
			>> entry.lastHash
			>> entry.versions
			>> entry.size
			>> flags;
		if (stream.status() != QDataStream::Ok) {
			return {};
		}
		entry.media = MediaKind(flags & 0x03);
		entry.deleted = (flags & 0x04);
		entry.expired = (flags & 0x08);
		entry.recordHasFile = (flags & 0x10);
		entry.lastHasFile = (flags & 0x20);
		entry.bucket = bucket;
		result.push_back(std::move(entry));
	}
	return result;
}

} // namespace Core::LocalMessageIndex
