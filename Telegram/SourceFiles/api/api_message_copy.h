/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "api/api_common.h"

namespace Data {
struct ResolvedForwardDraft;
} // namespace Data

namespace Api {

void SendMessageCopies(
	Data::ResolvedForwardDraft &&draft,
	SendAction action,
	FnMut<void()> &&prepared);

} // namespace Api
