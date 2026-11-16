/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#include "gift_tracker/gift_tracker_box.h"

#include "base/unixtime.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "gift_tracker/gift_tracker.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace GiftTracker {
namespace {

constexpr auto kMaxShownEvents = 300;

[[nodiscard]] TextWithEntities DescribeGift(const GiftRecord &gift) {
	const auto name = GiftName(gift);
	auto result = gift.slug.isEmpty()
		? TextWithEntities{ name }
		: Ui::Text::Link(name, u"https://t.me/nft/"_q + gift.slug);
	if (gift.stars > 0) {
		result.append(u" · "_q).append(tr::lng_gift_tracker_stars(
			tr::now,
			lt_count,
			int(gift.stars)));
	}
	if (!gift.fromName.isEmpty()) {
		result.append(u" · "_q).append(tr::lng_gift_tracker_from(
			tr::now,
			lt_name,
			gift.fromName));
	}
	return result;
}

[[nodiscard]] TextWithEntities DescribeEvent(const GiftEvent &event) {
	auto result = Ui::Text::Bold(
		langDateTime(base::unixtime::parse(event.when)));
	result.append(u"\n"_q);
	switch (event.type) {
	case EventType::Started:
		result.append(tr::lng_gift_tracker_started(
			tr::now,
			lt_count,
			event.count));
		break;
	case EventType::Added:
		result.append(tr::lng_gift_tracker_added(tr::now)).append(u" "_q);
		result.append(DescribeGift(event.gift));
		break;
	case EventType::Removed:
		result.append(tr::lng_gift_tracker_removed(tr::now)).append(u" "_q);
		result.append(DescribeGift(event.gift));
		break;
	case EventType::Upgraded:
		result.append(tr::lng_gift_tracker_upgraded(tr::now)).append(u" "_q);
		result.append(DescribeGift(event.gift));
		break;
	}
	return result;
}

[[nodiscard]] TextWithEntities DescribeStatus(const TrackedPeer &peer) {
	auto result = Ui::Text::Bold(peer.name);
	result.append(u"\n"_q);
	result.append(peer.lastCheck
		? tr::lng_gift_tracker_last_check(
			tr::now,
			lt_date,
			langDateTime(base::unixtime::parse(peer.lastCheck)))
		: tr::lng_gift_tracker_never_checked(tr::now));
	if (peer.hasSnapshot) {
		result.append(u"\n"_q).append(tr::lng_gift_tracker_count(
			tr::now,
			lt_count,
			int(peer.snapshot.size())));
	}
	if (!peer.lastError.isEmpty()) {
		result.append(u"\n"_q).append(tr::lng_gift_tracker_error(
			tr::now,
			lt_error,
			peer.lastError));
	}
	return result;
}

[[nodiscard]] bool CanTrack(not_null<PeerData*> peer) {
	if (const auto user = peer->asUser()) {
		return !user->isSelf()
			&& !user->isBot()
			&& !user->isServiceUser()
			&& !user->isInaccessible();
	}
	return peer->isBroadcast();
}

void AddLabel(
		not_null<Ui::VerticalLayout*> container,
		TextWithEntities text) {
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			rpl::single(std::move(text)),
			st::boxLabel),
		st::boxRowPadding);
	Ui::AddSkip(container);
}

} // namespace

void HistoryBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		PeerId peerId) {
	const auto session = &controller->session();
	box->setTitle(tr::lng_gift_tracker_title());
	box->setWidth(st::boxWideWidth);

	const auto content = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	const auto fill = [=] {
		content->clear();
		const auto tracker = Find(session);
		const auto peer = tracker ? tracker->find(peerId) : nullptr;
		if (!peer) {
			AddLabel(content, { tr::lng_gift_tracker_not_tracked(tr::now) });
			return;
		}
		AddLabel(content, DescribeStatus(*peer));
		Ui::AddDivider(content);
		Ui::AddSkip(content);
		if (peer->events.empty()) {
			AddLabel(content, { tr::lng_gift_tracker_empty(tr::now) });
			return;
		}
		auto shown = 0;
		for (auto i = rbegin(peer->events); i != rend(peer->events); ++i) {
			AddLabel(content, DescribeEvent(*i));
			if (++shown >= kMaxShownEvents) {
				break;
			}
		}
	};
	fill();

	if (const auto tracker = Find(session)) {
		tracker->updates(
		) | rpl::filter([=](PeerId id) {
			return (id == peerId);
		}) | rpl::on_next([=] {
			fill();
		}, box->lifetime());
	}

	box->addButton(tr::lng_gift_tracker_check_now(), [=] {
		const auto tracker = Find(session);
		const auto scheduled = tracker && tracker->checkNow(peerId);
		controller->showToast(scheduled
			? tr::lng_gift_tracker_check_soon(tr::now)
			: tr::lng_gift_tracker_check_later(tr::now));
	});
	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
}

void TrackedListBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	box->setTitle(tr::lng_gift_tracker_settings_list());
	box->setWidth(st::boxWideWidth);

	const auto content = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	const auto fill = [=] {
		content->clear();
		const auto tracker = Find(session);
		const auto peers = tracker
			? tracker->trackedPeers()
			: std::vector<PeerId>();
		if (peers.empty()) {
			AddLabel(content, { tr::lng_gift_tracker_list_empty(tr::now) });
			return;
		}
		for (const auto peerId : peers) {
			const auto peer = tracker->find(peerId);
			const auto name = peer->name.isEmpty()
				? QString::number(peerId.value)
				: peer->name;
			Settings::AddButtonWithIcon(
				content,
				rpl::single(name),
				st::settingsButtonNoIcon
			)->setClickedCallback([=] {
				controller->show(Box(HistoryBox, controller, peerId));
			});
		}
	};
	fill();

	if (const auto tracker = Find(session)) {
		tracker->updates(
		) | rpl::on_next([=](PeerId) {
			fill();
		}, box->lifetime());
	}

	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
}

void FillPeerMenu(
		not_null<Window::SessionController*> controller,
		PeerData *peer,
		const Window::PeerMenuCallback &addAction) {
	if (!peer || !Enabled() || !CanTrack(peer)) {
		return;
	}
	const auto session = &controller->session();
	const auto tracker = Find(session);
	if (!tracker) {
		return;
	}
	const auto peerId = peer->id;
	if (tracker->tracked(peerId)) {
		addAction(tr::lng_gift_tracker_history(tr::now), [=] {
			controller->show(Box(HistoryBox, controller, peerId));
		}, &st::menuIconGiftPremium);
		addAction(tr::lng_gift_tracker_untrack(tr::now), [=] {
			const auto name = peer->name();
			controller->show(Ui::MakeConfirmBox({
				.text = tr::lng_gift_tracker_untrack_sure(
					tr::now,
					lt_name,
					name),
				.confirmed = [=](Fn<void()> close) {
					if (const auto tracker = Find(session)) {
						tracker->untrack(peerId);
					}
					close();
				},
				.confirmText = tr::lng_gift_tracker_untrack_confirm(),
			}));
		}, &st::menuIconCancel);
	} else {
		addAction(tr::lng_gift_tracker_track(tr::now), [=] {
			if (const auto tracker = Find(session)) {
				tracker->track(peer);
				controller->showToast(
					tr::lng_gift_tracker_tracking_toast(tr::now));
			}
		}, &st::menuIconGiftPremium);
	}
}

void SetupSettings(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, tr::lng_gift_tracker_settings());

	Settings::AddButtonWithIcon(
		container,
		tr::lng_gift_tracker_settings_enabled(),
		st::settingsButtonNoIcon
	)->toggleOn(
		rpl::single(Enabled())
	)->toggledValue(
	) | rpl::filter([](bool enabled) {
		return (enabled != Enabled());
	}) | rpl::on_next([](bool enabled) {
		SetEnabled(enabled);
	}, container->lifetime());

	Settings::AddButtonWithIcon(
		container,
		tr::lng_gift_tracker_settings_notify(),
		st::settingsButtonNoIcon
	)->toggleOn(
		rpl::single(NotifyEnabled())
	)->toggledValue(
	) | rpl::filter([](bool enabled) {
		return (enabled != NotifyEnabled());
	}) | rpl::on_next([](bool enabled) {
		SetNotifyEnabled(enabled);
	}, container->lifetime());

	Settings::AddButtonWithIcon(
		container,
		tr::lng_gift_tracker_settings_list(),
		st::settingsButtonNoIcon
	)->setClickedCallback([=] {
		controller->show(Box(TrackedListBox, controller));
	});

	Ui::AddSkip(container);
	Ui::AddDividerText(container, tr::lng_gift_tracker_settings_about());
}

} // namespace GiftTracker
