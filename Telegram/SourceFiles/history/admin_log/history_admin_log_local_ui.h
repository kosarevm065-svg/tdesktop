/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#pragma once

#include "base/object_ptr.h"

namespace Ui {
class RpWidget;
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace AdminLog {

// Per-chat toggle and status of the local history,
// shown under the top bar of the "Recent actions" section.
// Returns nullptr when the feature is disabled in the settings.
[[nodiscard]] object_ptr<Ui::RpWidget> CreateLocalHistoryBar(
	QWidget *parent,
	not_null<ChannelData*> channel);

void SetupLocalHistorySettings(
	not_null<Window::SessionController*> controller,
	not_null<Ui::VerticalLayout*> container);

} // namespace AdminLog
