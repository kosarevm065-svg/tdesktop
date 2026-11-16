/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#pragma once

class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class FlatLabel;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace ProfileSnapshots {

inline constexpr auto kOption = "profile_snapshots";
inline constexpr auto kTmeOption = "profile_tme_fallback";

[[nodiscard]] bool Enabled();
void SetEnabled(bool enabled);
[[nodiscard]] bool TmeEnabled();
void SetTmeEnabled(bool enabled);

void Start(not_null<Main::Session*> session);

using AddLine = Fn<not_null<Ui::FlatLabel*>(
	rpl::producer<QString> label,
	rpl::producer<TextWithEntities> text)>;

void AddSavedInfoLines(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	const AddLine &addLine);

} // namespace ProfileSnapshots
