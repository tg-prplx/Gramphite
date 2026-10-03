#include "../../Telegram/SourceFiles/core/local_profile_export.h"

#include <QtCore/QJsonDocument>

void Check(bool passed, const char *description);

void RunProfileExportChecks() {
	auto record = QJsonObject();
	record[u"name"] = QString::fromUtf16(u"Новое Имя 😀");
	record[u"peer"] = QString::fromUtf16(u"562958543355209");
	record[u"userId"] = QString::fromUtf16(u"9007199254740993");
	auto snapshot = QJsonObject();
	snapshot[u"username"] = QString::fromUtf16(u"new_user");
	record[u"snapshot"] = snapshot;
	auto event = QJsonObject();
	event[u"field"] = QString::fromUtf16(u"username");
	event[u"before"] = QString::fromUtf16(u"old_alias");
	event[u"after"] = QString::fromUtf16(u"new_user");
	event[u"time"] = qint64(123);
	record[u"events"] = QJsonArray{ event };
	using Core::LocalProfileExport::MatchesQuery;
	Check(MatchesQuery(record, QString::fromUtf16(u"  ")), "empty search shows all profiles");
	Check(MatchesQuery(record, QString::fromUtf16(u"НОВОЕ @NEW_USER")), "search joins Unicode name and username tokens");
	Check(MatchesQuery(record, QString::fromUtf16(u"@OLD_ALIAS")), "old usernames remain searchable after rename");
	Check(MatchesQuery(record, QString::fromUtf16(u"9007199254740993")), "large IDs are searchable without numeric rounding");
	Check(!MatchesQuery(record, QString::fromUtf16(u"new_user missing")), "all search tokens must match");
	auto avatars = QJsonObject();
	auto avatar = QJsonObject();
	avatar[u"key"] = QString::fromUtf16(u"available");
	avatar[u"time"] = qint64(100);
	avatars[u"photo1"] = avatar;
	avatar[u"key"] = QString::fromUtf16(u"evicted");
	avatar[u"time"] = qint64(200);
	avatars[u"photo2"] = avatar;
	record[u"avatars"] = avatars;
	const auto bytes = QByteArray::fromHex("89504e4700ff0011");
	const auto exported = Core::LocalProfileExport::Create(
		record,
		record[u"userId"].toString(),
		qint64(300),
		[&](const QString &key) {
			return key == u"available" ? bytes : QByteArray();
		});
	const auto restored = QJsonDocument::fromJson(QJsonDocument(exported).toJson()).object();
	const auto history = restored[u"history"].toObject();
	Check(restored[u"userId"].toString() == record[u"userId"].toString(), "export keeps IDs as exact strings");
	Check(history[u"events"] == record[u"events"], "export preserves old values and event times");
	const auto exportedAvatars = history[u"avatars"].toObject();
	const auto saved = exportedAvatars[u"photo1"].toObject();
	Check(QByteArray::fromBase64(saved[u"dataBase64"].toString().toLatin1()) == bytes, "export embeds portable avatar bytes");
	Check(saved[u"time"].toInteger() == 100, "avatar timestamp survives export");
	Check(!exportedAvatars[u"photo2"].toObject()[u"available"].toBool(), "evicted avatar is marked unavailable");
	Check(!record[u"avatars"].toObject()[u"photo1"].toObject().contains(u"dataBase64"), "export never mutates the encrypted journal record");
}
