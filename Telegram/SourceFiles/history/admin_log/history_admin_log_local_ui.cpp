/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#include "history/admin_log/history_admin_log_local_ui.h"

#include "history/admin_log/history_admin_log_local.h"
#include "base/unixtime.h"
#include "data/data_channel.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_widgets.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace AdminLog {
namespace {

constexpr auto kShownGaps = 3;

[[nodiscard]] QString FormatDate(TimeId date) {
	return base::unixtime::parse(date).date().toString(u"dd.MM.yyyy"_q);
}

[[nodiscard]] QString StatusText(const LocalHistoryState &state) {
	if (!state.enabled && !state.count) {
		return tr::lng_admin_log_local_about(tr::now);
	} else if (!state.since) {
		return tr::lng_admin_log_local_waiting(tr::now);
	}
	auto result = tr::lng_admin_log_local_since(
		tr::now,
		lt_date,
		FormatDate(state.since));
	if (!state.enabled) {
		result += QChar('\n') + tr::lng_admin_log_local_paused(tr::now);
	}
	if (!state.gaps.empty()) {
		auto list = QStringList();
		const auto from = std::max(
			int(state.gaps.size()) - kShownGaps,
			0);
		for (auto i = from; i != int(state.gaps.size()); ++i) {
			const auto &[start, till] = state.gaps[i];
			list.push_back(FormatDate(start) + QChar(0x2013) + FormatDate(till));
		}
		if (from > 0) {
			list.push_front(QString(QChar(0x2026)));
		}
		result += QChar('\n') + tr::lng_admin_log_local_gaps(
			tr::now,
			lt_list,
			list.join(u", "_q));
	}
	return result;
}

[[nodiscard]] QString LimitLabel(int value) {
	return (value > 0)
		? QString::number(value)
		: tr::lng_settings_admin_log_local_unlimited(tr::now);
}

void LimitBox(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<rpl::variable<int>> limit) {
	box->setTitle(tr::lng_settings_admin_log_local_limit());
	const auto group = std::make_shared<Ui::RadiobuttonGroup>(
		LocalHistory::EventsLimitSetting());
	for (const auto value : LocalHistory::EventsLimitOptions()) {
		box->addRow(object_ptr<Ui::Radiobutton>(
			box,
			group,
			value,
			LimitLabel(value),
			st::defaultBoxCheckbox));
	}
	box->addButton(tr::lng_settings_save(), [=] {
		LocalHistory::SetEventsLimitSetting(group->current());
		*limit = group->current();
		box->closeBox();
	});
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

} // namespace

object_ptr<Ui::RpWidget> CreateLocalHistoryBar(
		QWidget *parent,
		not_null<ChannelData*> channel) {
	const auto local = LocalHistory::Find(&channel->session());
	if (!local || !LocalHistory::MasterEnabled()) {
		return { nullptr };
	}
	auto result = object_ptr<Ui::VerticalLayout>(parent);
	const auto raw = result.data();
	raw->paintRequest() | rpl::on_next([=](QRect clip) {
		QPainter(raw).fillRect(clip, st::windowBg);
	}, raw->lifetime());

	const auto toggle = raw->add(object_ptr<Ui::SettingsButton>(
		raw,
		tr::lng_admin_log_local_toggle(),
		st::settingsButtonNoIcon));
	toggle->toggleOn(rpl::single(local->enabled(channel)));
	toggle->toggledChanges(
	) | rpl::on_next([=](bool enabled) {
		if (const auto local = LocalHistory::Find(&channel->session())) {
			local->setEnabled(channel, enabled);
		}
	}, toggle->lifetime());

	raw->add(
		object_ptr<Ui::FlatLabel>(
			raw,
			local->stateValue(channel) | rpl::map(StatusText),
			st::boxDividerLabel),
		st::defaultBoxDividerLabelPadding);
	return result;
}

void SetupLocalHistorySettings(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, tr::lng_settings_admin_log_local());

	Settings::AddButtonWithIcon(
		container,
		tr::lng_settings_admin_log_local_enable(),
		st::settingsButtonNoIcon
	)->toggleOn(
		rpl::single(LocalHistory::MasterEnabled())
	)->toggledChanges(
	) | rpl::filter([](bool enabled) {
		return (enabled != LocalHistory::MasterEnabled());
	}) | rpl::on_next([](bool enabled) {
		LocalHistory::SetMasterEnabled(enabled);
	}, container->lifetime());

	const auto limit = std::make_shared<rpl::variable<int>>(
		LocalHistory::EventsLimitSetting());
	Settings::AddButtonWithLabel(
		container,
		tr::lng_settings_admin_log_local_limit(),
		limit->value() | rpl::map(LimitLabel),
		st::settingsButtonNoIcon
	)->addClickHandler([=] {
		controller->show(Box(LimitBox, limit));
	});

	Settings::AddButtonWithIcon(
		container,
		tr::lng_settings_admin_log_local_clear(),
		st::settingsAttentionButton
	)->addClickHandler([=] {
		const auto session = &controller->session();
		controller->show(Ui::MakeConfirmBox({
			.text = tr::lng_settings_admin_log_local_clear_sure(),
			.confirmed = [=](Fn<void()> close) {
				if (const auto local = LocalHistory::Find(session)) {
					local->clearAll();
				}
				close();
				controller->showToast(
					tr::lng_settings_admin_log_local_cleared(tr::now));
			},
			.confirmText = tr::lng_box_delete(),
			.confirmStyle = &st::attentionBoxButton,
		}));
	});

	Ui::AddSkip(container);
	Ui::AddDividerText(container, tr::lng_settings_admin_log_local_about());
}

} // namespace AdminLog
