/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "window/notifications_manager.h"

namespace Platform {
namespace Notifications {

[[nodiscard]] bool SkipToastForCustom();
void MaybePlaySoundForCustom(Fn<void()> playSound);
void MaybeFlashBounceForCustom(Fn<void()> flashBounce);
[[nodiscard]] bool WaitForInputForCustom();

[[nodiscard]] bool Supported();
[[nodiscard]] bool Enforced();
[[nodiscard]] bool ByDefault();
[[nodiscard]] std::unique_ptr<Window::Notifications::Manager> Create(
	Window::Notifications::System *system);

} // namespace Notifications
} // namespace Platform

// Platform dependent implementations.

#ifdef Q_OS_WIN
#include "platform/win/notifications_manager_win.h"
#elif defined Q_OS_MAC // Q_OS_MAC
#include "platform/mac/notifications_manager_mac.h"
#else // Q_OS_WIN || Q_OS_MAC
#include "platform/linux/notifications_manager_linux.h"
#endif // else for Q_OS_WIN || Q_OS_MAC
