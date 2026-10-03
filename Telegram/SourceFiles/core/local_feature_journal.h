/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

namespace Core::LocalJournal {

inline bool AttachMedia(QJsonObject &record, const QString &media, const QJsonObject &attachment) {
	if (media.isEmpty() || !attachment.contains(u"file")) {
		return false;
	}
	auto versions = record[u"versions"].toArray();
	auto changed = false;
	for (auto i = 0; i < versions.size(); ++i) {
		auto version = versions[i].toObject();
		if (version[u"media"].toString() != media || version.contains(u"file")) {
			continue;
		}
		version[u"file"] = attachment[u"file"];
		version[u"filename"] = attachment[u"filename"];
		versions[i] = version;
		changed = true;
	}
	if (changed) {
		record[u"versions"] = versions;
	}
	return changed;
}

inline bool AppendVersion(
		QJsonObject &record,
		const QString &text,
		const QString &media,
		qint64 timestamp,
		bool preserve,
		int limit) {
	auto versions = record[u"versions"].toArray();
	if (!versions.isEmpty()
		&& versions.last().toObject()[u"text"].toString() == text
		&& versions.last().toObject()[u"media"].toString() == media) {
		return false;
	}
	if (!preserve) {
		versions = {};
	}
	auto version = QJsonObject();
	version[u"text"] = text;
	version[u"media"] = media;
	version[u"time"] = timestamp;
	versions.append(version);
	while (versions.size() > limit) {
		versions.removeFirst();
	}
	record[u"versions"] = versions;
	record[u"updated"] = timestamp;
	return true;
}

} // namespace Core::LocalJournal
