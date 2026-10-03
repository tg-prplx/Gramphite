/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_type.h"
#include "data/data_msg_id.h"

class HistoryItem;
class PeerData;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Settings {

[[nodiscard]] Type LocalFeaturesId();
void ShowLocalMessageHistory(
	not_null<Window::SessionController*> controller,
	FullMsgId id);
void ShowLocalMessagesArchive(
	not_null<Window::SessionController*> controller,
	PeerId chat);
void ShowLocalProfileInsights(
	not_null<Window::SessionController*> controller,
	PeerId id);
void ToggleLocalHidden(
	not_null<Window::SessionController*> controller,
	PeerId id);

[[nodiscard]] bool HasLocalInsights(not_null<PeerData*> peer);
void AddLocalSenderActions(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item);

} // namespace Settings
