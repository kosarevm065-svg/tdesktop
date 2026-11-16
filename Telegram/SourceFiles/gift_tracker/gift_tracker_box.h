/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#pragma once

#include "window/window_peer_menu.h"

namespace Ui {
class GenericBox;
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace GiftTracker {

void HistoryBox(
	not_null<Ui::GenericBox*> box,
	not_null<Window::SessionController*> controller,
	PeerId peerId);

void TrackedListBox(
	not_null<Ui::GenericBox*> box,
	not_null<Window::SessionController*> controller);

void FillPeerMenu(
	not_null<Window::SessionController*> controller,
	PeerData *peer,
	const Window::PeerMenuCallback &addAction);

void SetupSettings(
	not_null<Window::SessionController*> controller,
	not_null<Ui::VerticalLayout*> container);

} // namespace GiftTracker
