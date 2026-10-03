/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_message_copy.h"

#include "apiwrap.h"
#include "base/timer.h"
#include "chat_helpers/message_field.h"
#include "core/local_features.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "storage/file_download.h"
#include "storage/localimageloader.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/image/image.h"

#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>

namespace Api {
namespace {

constexpr auto kCopyIdleTimeout = 10 * 60 * crl::time(1000);
constexpr auto kMaxVoiceCopyInMemory = 64 * 1024 * 1024;

class MessageCopyBatch final
	: public std::enable_shared_from_this<MessageCopyBatch> {
public:
	MessageCopyBatch(
		Data::ResolvedForwardDraft &&draft,
		SendAction action,
		FnMut<void()> &&prepared);
	void start();

private:
	struct Entry {
		FullMsgId id;
		MessageGroupId groupId;
		TextWithTags caption;
		std::shared_ptr<Data::PhotoMedia> photo;
		std::shared_ptr<Data::DocumentMedia> document;
		QString downloadPath;
		bool supported = true;
		bool started = false;
		bool spoiler = false;
	};

	void check();
	void dispatch();
	void fail();
	void stop();
	[[nodiscard]] bool ready(const Entry &entry) const;
	[[nodiscard]] QByteArray photoBytes(const Entry &entry) const;

	const not_null<Main::Session*> _session;
	SendAction _action;
	Data::ForwardOptions _options;
	std::vector<Entry> _entries;
	FnMut<void()> _prepared;
	std::shared_ptr<QTemporaryDir> _downloads;
	std::shared_ptr<MessageCopyBatch> _keepAlive;
	bool _checking = false;
	bool _finished = false;
	base::Timer _timeout;
	rpl::lifetime _lifetime;

};

MessageCopyBatch::MessageCopyBatch(
		Data::ResolvedForwardDraft &&draft,
		SendAction action,
		FnMut<void()> &&prepared)
: _session(&action.history->session())
, _action(action)
, _options(draft.options)
, _prepared(std::move(prepared))
, _timeout([=] {
	const auto keep = shared_from_this();
	fail();
}) {
	_action.clearDraft = false;
	_entries.reserve(draft.items.size());
	for (const auto item : draft.items) {
		const auto media = item->media();
		const auto text = item->originalText();
		auto entry = Entry{
			.id = item->fullId(),
			.groupId = item->groupId(),
		};
		if (!media || media->webpage()
			|| _options != Data::ForwardOptions::NoNamesAndCaptions) {
			entry.caption = {
				text.text,
				TextUtilities::ConvertEntitiesToTextTags(text.entities),
			};
		}
		if (const auto photo = media ? media->photo() : nullptr) {
			entry.photo = photo->createMediaView();
		} else if (const auto document = media ? media->document() : nullptr) {
			entry.document = document->createMediaView();
		} else if (media && !media->webpage()) {
			entry.supported = false;
		}
		entry.spoiler = media && media->hasSpoiler();
		_entries.push_back(std::move(entry));
	}
}

void MessageCopyBatch::start() {
	_keepAlive = shared_from_this();
	const auto weak = weak_from_this();
	_session->lifetime().add([weak] {
		if (const auto strong = weak.lock()) {
			strong->stop();
		}
	});
	_session->localFeatures().changes(
	) | rpl::on_next([weak] {
		if (const auto strong = weak.lock()) {
			strong->check();
		}
	}, _lifetime);
	_session->downloaderTaskFinished(
	) | rpl::on_next([weak] {
		if (const auto strong = weak.lock()) {
			strong->check();
		}
	}, _lifetime);
	_session->data().documentLoadProgress(
	) | rpl::on_next([weak](not_null<DocumentData*> document) {
		if (const auto strong = weak.lock()) {
			for (const auto &entry : strong->_entries) {
				if (entry.document && entry.document->owner() == document) {
					strong->_timeout.callOnce(kCopyIdleTimeout);
					strong->check();
					break;
				}
			}
		}
	}, _lifetime);
	_timeout.callOnce(kCopyIdleTimeout);
	check();
}

bool MessageCopyBatch::ready(const Entry &entry) const {
	if (entry.photo) {
		return entry.photo->loaded();
	} else if (entry.document) {
		return entry.document->loaded(true);
	}
	return true;
}

void MessageCopyBatch::check() {
	if (_finished || _checking) {
		return;
	}
	_checking = true;
	const auto reset = gsl::finally([&] { _checking = false; });
	if (!_session->localFeatures().enabled(Core::LocalFeature::CopyProtected)
		|| ranges::any_of(_entries, [](const Entry &entry) {
			return !entry.supported;
		})) {
		fail();
		return;
	}
	for (auto &entry : _entries) {
		if (ready(entry)) {
			continue;
		} else if (entry.started) {
			if ((entry.photo && entry.photo->owner()->failed(Data::PhotoSize::Large))
				|| (entry.document && entry.document->owner()->cancelled())) {
				fail();
				return;
			}
			continue;
		}
		entry.started = true;
		if (entry.photo) {
			const auto photo = entry.photo->owner();
			photo->clearFailed(Data::PhotoSize::Large);
			photo->load(Data::PhotoSize::Large, entry.id);
		} else if (entry.document) {
			const auto document = entry.document->owner();
			if (document->forbidsFileSave()) {
				fail();
				return;
			}
			if (document->loading()) {
				continue;
			}
			if (document->size > Storage::kMaxFileInMemory) {
				if (!_downloads) {
					_downloads = std::make_shared<QTemporaryDir>();
					_session->lifetime().add([keep = _downloads] {});
				}
				if (!_downloads->isValid()) {
					fail();
					return;
				}
				entry.downloadPath = _downloads->filePath(
					QString::number(document->id)
						+ u"_"_q
						+ QFileInfo(document->filename()).fileName());
			}
			document->save(entry.id, entry.downloadPath);
		}
	}
	if (ranges::all_of(_entries, [=](const Entry &entry) {
		return ready(entry);
	})) {
		dispatch();
	}
}

QByteArray MessageCopyBatch::photoBytes(const Entry &entry) const {
	auto bytes = entry.photo->imageBytes(Data::PhotoSize::Large);
	if (bytes.isEmpty()) {
		const auto image = entry.photo->image(Data::PhotoSize::Large);
		auto buffer = QBuffer(&bytes);
		if (!image || !buffer.open(QIODevice::WriteOnly)
			|| !image->original().save(&buffer, "JPEG", 97)) {
			return {};
		}
	}
	return bytes;
}

void MessageCopyBatch::dispatch() {
	auto files = Ui::PreparedList();
	for (const auto &entry : _entries) {
		if (!entry.photo && !entry.document) {
			continue;
		}
		auto file = Ui::PreparedFile(QString());
		file.caption = entry.caption;
		file.spoiler = entry.spoiler;
		if (entry.photo) {
			file.content = photoBytes(entry);
			file.type = Ui::PreparedFile::Type::Photo;
		} else {
			const auto document = entry.document->owner();
			file.path = document->filepath(true);
			file.content = file.path.isEmpty() ? entry.document->bytes() : QByteArray();
			file.displayName = QFileInfo(document->filename()).fileName();
			if (document->isVoiceMessage() || document->isVideoMessage()) {
				if (file.content.isEmpty()
					&& QFileInfo(file.path).size() <= kMaxVoiceCopyInMemory) {
					auto input = QFile(file.path);
					if (input.open(QIODevice::ReadOnly)) {
						file.content = input.readAll();
					}
				}
				if (!file.content.isEmpty()) {
					file.path = QString();
				}
			}
			file.type = document->isVideoFile()
				? Ui::PreparedFile::Type::Video
				: document->isSong()
				? Ui::PreparedFile::Type::Music
				: Ui::PreparedFile::Type::File;
		}
		if (file.path.isEmpty() && file.content.isEmpty()) {
			fail();
			return;
		}
		files.files.push_back(std::move(file));
	}
	auto nextFile = files.files.begin();
	for (auto i = size_t(0); i != _entries.size();) {
		const auto &entry = _entries[i++];
		if (!entry.photo && !entry.document) {
			auto message = MessageToSend(_action);
			message.textWithTags = entry.caption;
			_session->api().sendMessage(std::move(message));
		} else if (entry.document
			&& (entry.document->owner()->isVoiceMessage()
				|| entry.document->owner()->isVideoMessage())
			&& !nextFile->content.isEmpty()) {
			const auto document = entry.document->owner();
			const auto voice = document->voice();
			_session->api().sendVoiceMessage(
				nextFile->content,
				voice ? voice->waveform : VoiceWaveform(),
				document->duration(),
				document->isVideoMessage(),
				_action,
				entry.caption);
			++nextFile;
		} else {
			auto one = Ui::PreparedList();
			const auto albumType = nextFile->albumType(true);
			one.files.push_back(std::move(*nextFile++));
			while (entry.groupId
				&& i != _entries.size()
				&& _entries[i].groupId == entry.groupId
				&& (_entries[i].photo || _entries[i].document)
				&& nextFile->canBeInAlbumType(albumType)
				&& one.files.size() < Ui::MaxAlbumItems()) {
				one.files.push_back(std::move(*nextFile++));
				++i;
			}
			const auto album = (one.files.size() > 1)
				? std::make_shared<SendingAlbum>()
				: nullptr;
			_session->api().sendFiles(
				std::move(one),
				(entry.photo || albumType == Ui::AlbumType::PhotoVideo)
					? SendMediaType::Photo
					: SendMediaType::File,
				album,
				_action);
		}
	}

	if (_prepared) {
		_prepared();
	}
	stop();
}

void MessageCopyBatch::fail() {
	const auto history = _action.history;
	const auto topic = _action.replyTo.topicRootId;
	const auto sublist = _action.replyTo.monoforumPeerId;
	if (history->forwardDraft(topic, sublist).ids.empty()) {
		auto draft = Data::ForwardDraft{ .options = _options };
		for (const auto &entry : _entries) {
			draft.ids.push_back(entry.id);
		}
		history->setForwardDraft(topic, sublist, std::move(draft));
	}
	_session->api().sendMessageFail(
		tr::lng_local_copy_failed(tr::now),
		history->peer);
	stop();
}

void MessageCopyBatch::stop() {
	_finished = true;
	_timeout.cancel();
	_lifetime.destroy();
	_keepAlive = nullptr;
}

} // namespace

void SendMessageCopies(
		Data::ResolvedForwardDraft &&draft,
		SendAction action,
		FnMut<void()> &&prepared) {
	const auto batch = std::make_shared<MessageCopyBatch>(
		std::move(draft),
		action,
		std::move(prepared));
	batch->start();
}

} // namespace Api
