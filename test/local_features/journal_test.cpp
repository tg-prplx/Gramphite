#include "../../Telegram/SourceFiles/core/local_feature_journal.h"
#include <QtCore/QJsonDocument>
#include <iostream>
#include <cstdlib>

void RunProfileExportChecks();
void RunMessageIndexChecks();

void Check(bool passed, const char *description) {
	if (!passed) {
		std::cerr << description << '\n';
		std::exit(1);
	}
}

int main() {
	RunProfileExportChecks();
	RunMessageIndexChecks();
	using Core::LocalJournal::AppendVersion;
	auto record = QJsonObject();
	const auto original = QString::fromUtf16(u"original 😀 текст");
	Check(AppendVersion(record, original, {}, 10, true, 3), "original saved");
	Check(!AppendVersion(record, original, {}, 20, true, 3), "repeated server update deduplicated");
	Check(record[u"updated"].toInteger() == 10, "duplicate keeps original timestamp");
	Check(AppendVersion(record, QString::fromUtf16(u"edited"), {}, 30, true, 3), "edit saved");
	Check(record[u"versions"].toArray().first().toObject()[u"text"].toString() == original,
		"old text retained after edit");
	Check(AppendVersion(record, QString::fromUtf16(u"edited"), QString::fromUtf16(u"photo:1"), 40, true, 3),
		"media replacement with same caption saved");
	AppendVersion(record, QString::fromUtf16(u"latest"), {}, 50, true, 3);
	Check(record[u"versions"].toArray().size() == 3, "version limit enforced");
	Check(record[u"versions"].toArray().first().toObject()[u"time"].toInteger() == 30,
		"oldest version evicted first");
	record[u"peer"] = QString::fromUtf16(u"562958543355209");
	record[u"message"] = QString::fromUtf16(u"2147483647");
	record[u"deleted"] = true;
	const auto restored = QJsonDocument::fromJson(QJsonDocument(record).toJson()).object();
	Check(restored == record, "versions, deletion marker and large IDs survive round trip");
	AppendVersion(record, QString::fromUtf16(u"without history"), {}, 60, false, 3);
	Check(record[u"versions"].toArray().size() == 1, "disabled history retains only current content");
	Check(record[u"deleted"].toBool(), "snapshot does not clear deletion marker");
	auto attachments = QJsonObject();
	AppendVersion(attachments, original, QString::fromUtf16(u"photo:1"), 10, true, 10);
	AppendVersion(attachments, original, QString::fromUtf16(u"document:2"), 20, true, 10);
	const auto oldPhoto = QJsonObject{
		{ QString::fromUtf16(u"file"), QString::fromUtf16(u"photo_hash") },
		{ QString::fromUtf16(u"filename"), QString::fromUtf16(u"photo.jpg") },
	};
	Check(Core::LocalJournal::AttachMedia(attachments, QString::fromUtf16(u"photo:1"), oldPhoto),
		"late download attaches to old version");
	auto versions = attachments[u"versions"].toArray();
	Check(versions.first().toObject()[u"file"].toString() == QString::fromUtf16(u"photo_hash"),
		"original attachment retained");
	Check(!versions.last().toObject().contains(u"file"), "replacement does not inherit old attachment");
	Check(!Core::LocalJournal::AttachMedia(attachments, QString::fromUtf16(u"photo:1"), oldPhoto),
		"attachment retry is idempotent");
	Check(versions.first().toObject()[u"time"].toInteger() == 10,
		"download does not alter version timestamp");
	Check(QJsonDocument::fromJson(QJsonDocument(attachments).toJson()).object() == attachments,
		"per-version attachments survive archive round trip");
	std::cout << "All local journal checks passed\n";
}
