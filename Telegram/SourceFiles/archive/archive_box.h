/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#pragma once

class PeerData;

namespace Ui {
class VerticalLayout;
} // namespace Ui

namespace Ui::Menu {
struct MenuCallback;
} // namespace Ui::Menu

namespace Window {
class SessionController;
} // namespace Window

namespace ChatArchive {

void FillPeerMenu(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	const Ui::Menu::MenuCallback &addAction);

void ShowArchiveList(not_null<Window::SessionController*> controller);

void SetupSettings(
	not_null<Window::SessionController*> controller,
	not_null<Ui::VerticalLayout*> container);

} // namespace ChatArchive
