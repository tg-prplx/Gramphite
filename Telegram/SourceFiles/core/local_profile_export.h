/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QStringList>

namespace Core::LocalProfileExport {

[[nodiscard]] inline bool MatchesQuery(
		const QJsonObject &record,
		const QString &query) {
	const auto tokens = query.simplified().toCaseFolded().split(' ', Qt::SkipEmptyParts);
	if (tokens.isEmpty()) {
		return true;
	}
	auto values = QStringList{
		record[u"name"].toString(),
		record[u"peer"].toString(),
		record[u"userId"].toString(),
	};
	const auto snapshot = record[u"snapshot"].toObject();
	values.push_back(snapshot[u"name"].toString());
	values.push_back(snapshot[u"username"].toString());
	for (const auto &value : record[u"events"].toArray()) {
		const auto event = value.toObject();
		const auto field = event[u"field"].toString();
		if (field == u"name" || field == u"username") {
			values.push_back(event[u"before"].toString());
			values.push_back(event[u"after"].toString());
		}
	}
	const auto haystack = values.join(' ').toCaseFolded();
	for (auto token : tokens) {
		if (token.startsWith('@')) {
			token.remove(0, 1);
		}
		if (!haystack.contains(token)) {
			return false;
		}
	}
	return true;
}

template <typename ReadMedia>
[[nodiscard]] QJsonObject Create(
		QJsonObject record,
		const QString &userId,
		qint64 exportedAt,
		ReadMedia &&readMedia) {
	auto avatars = record[u"avatars"].toObject();
	for (auto i = avatars.begin(); i != avatars.end(); ++i) {
		auto avatar = i.value().toObject();
		const auto bytes = readMedia(avatar[u"key"].toString());
		avatar[u"available"] = !bytes.isEmpty();
		if (!bytes.isEmpty()) {
			avatar[u"mimeType"] = QString::fromUtf16(u"image/png");
			avatar[u"dataBase64"] = QString::fromLatin1(bytes.toBase64());
		}
		i.value() = avatar;
	}
	record[u"avatars"] = avatars;
	auto result = QJsonObject();
	result[u"format"] = QString::fromUtf16(u"telegram-local-profile-history");
	result[u"version"] = 1;
	result[u"exportedAt"] = exportedAt;
	result[u"userId"] = userId;
	result[u"history"] = record;
	return result;
}

} // namespace Core::LocalProfileExport
