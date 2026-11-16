/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#include "archive/archive_box.h"

#include "archive/archive_profiles.h"
#include "archive/archive_store.h"
#include "base/unixtime.h"
#include "core/file_utilities.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "ui/text/format_values.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <QtCore/QUrl>

namespace ChatArchive {
namespace {

constexpr auto kPageSize = 200;
constexpr auto kMediaLimits = std::array{ 0, 1, 5, 20, 100 };

[[nodiscard]] QString FormatDateTime(TimeId date) {
	return base::unixtime::parse(date).toString(u"dd.MM.yyyy hh:mm"_q);
}

[[nodiscard]] QString FormatDate(TimeId date) {
	return base::unixtime::parse(date).date().toString(u"dd.MM.yyyy"_q);
}

[[nodiscard]] QString MediaLimitText(int limit) {
	return limit
		? tr::lng_chat_archive_media_mb(
			tr::now,
			lt_size,
			QString::number(limit))
		: tr::lng_chat_archive_media_off(tr::now);
}

[[nodiscard]] bool ChatGone(not_null<Main::Session*> session, PeerId id) {
	const auto history = session->data().historyLoaded(id);
	return !history || !history->inChatList();
}

[[nodiscard]] QString MediaTitle(const MediaInfo &media) {
	auto result = u"["_q + media.type + ']';
	if (!media.name.isEmpty()) {
		result += ' ' + media.name;
	}
	if (media.size > 0) {
		result += u" ("_q + Ui::FormatSizeText(media.size) + ')';
	}
	if (!media.description.isEmpty() && media.type != u"photo"_q) {
		result += u" — "_q + media.description;
	}
	return result;
}

[[nodiscard]] QString EntityTypeName(EntityType type) {
	switch (type) {
	case EntityType::Url: return u"url"_q;
	case EntityType::CustomUrl: return u"text_url"_q;
	case EntityType::Email: return u"email"_q;
	case EntityType::Hashtag: return u"hashtag"_q;
	case EntityType::Cashtag: return u"cashtag"_q;
	case EntityType::Mention: return u"mention"_q;
	case EntityType::MentionName: return u"mention_name"_q;
	case EntityType::BotCommand: return u"bot_command"_q;
	case EntityType::Phone: return u"phone"_q;
	case EntityType::Bold: return u"bold"_q;
	case EntityType::Italic: return u"italic"_q;
	case EntityType::Underline: return u"underline"_q;
	case EntityType::StrikeOut: return u"strikethrough"_q;
	case EntityType::Code: return u"code"_q;
	case EntityType::Pre: return u"pre"_q;
	case EntityType::Blockquote: return u"blockquote"_q;
	case EntityType::Spoiler: return u"spoiler"_q;
	default: return u"unknown"_q;
	}
}

[[nodiscard]] QByteArray ExportJson(
		const QString &name,
		PeerId peerId,
		const std::vector<Message> &messages) {
	auto list = QJsonArray();
	for (const auto &message : messages) {
		auto object = QJsonObject{
			{ u"id"_q, double(message.id.bare) },
			{ u"date"_q, message.date },
			{ u"date_text"_q, FormatDateTime(message.date) },
			{ u"from_id"_q, QString::number(message.fromId.value) },
			{ u"from"_q, message.fromName },
			{ u"text"_q, message.text.text },
		};
		if (message.out) {
			object.insert(u"out"_q, true);
		}
		if (message.service) {
			object.insert(u"service"_q, true);
		}
		if (message.edited) {
			object.insert(u"edited"_q, message.edited);
		}
		if (message.deleted) {
			object.insert(u"deleted"_q, message.deleted);
		}
		if (message.replyTo) {
			object.insert(u"reply_to"_q, double(message.replyTo.bare));
		}
		if (!message.forwardFrom.isEmpty()) {
			object.insert(u"forwarded_from"_q, message.forwardFrom);
		}
		if (!message.text.entities.isEmpty()) {
			auto entities = QJsonArray();
			for (const auto &entity : message.text.entities) {
				auto e = QJsonObject{
					{ u"type"_q, EntityTypeName(entity.type()) },
					{ u"offset"_q, entity.offset() },
					{ u"length"_q, entity.length() },
				};
				if (!entity.data().isEmpty()) {
					e.insert(u"data"_q, entity.data());
				}
				entities.append(e);
			}
			object.insert(u"entities"_q, entities);
		}
		if (const auto &media = message.media) {
			auto m = QJsonObject{ { u"type"_q, media->type } };
			if (!media->name.isEmpty()) {
				m.insert(u"name"_q, media->name);
			}
			if (!media->mime.isEmpty()) {
				m.insert(u"mime"_q, media->mime);
			}
			if (media->size > 0) {
				m.insert(u"size"_q, double(media->size));
			}
			if (!media->description.isEmpty()) {
				m.insert(u"description"_q, media->description);
			}
			if (!media->file.isEmpty()) {
				m.insert(u"file"_q, media->file);
			}
			object.insert(u"media"_q, m);
		}
		list.append(object);
	}
	return QJsonDocument(QJsonObject{
		{ u"name"_q, name },
		{ u"peer_id"_q, QString::number(peerId.value) },
		{ u"exported"_q, base::unixtime::now() },
		{ u"messages"_q, list },
	}).toJson(QJsonDocument::Indented);
}

[[nodiscard]] QString HtmlText(const TextWithEntities &text) {
	struct Event {
		int position = 0;
		int order = 0;
		QString tag;
	};
	auto events = std::vector<Event>();
	auto index = 0;
	for (const auto &entity : text.entities) {
		const auto from = entity.offset();
		const auto till = from + entity.length();
		if (from < 0 || till > text.text.size() || from >= till) {
			continue;
		}
		auto open = QString();
		auto close = QString();
		switch (entity.type()) {
		case EntityType::Bold: open = u"<b>"_q; close = u"</b>"_q; break;
		case EntityType::Italic: open = u"<i>"_q; close = u"</i>"_q; break;
		case EntityType::Underline:
			open = u"<u>"_q; close = u"</u>"_q; break;
		case EntityType::StrikeOut:
			open = u"<s>"_q; close = u"</s>"_q; break;
		case EntityType::Code:
			open = u"<code>"_q; close = u"</code>"_q; break;
		case EntityType::Pre:
			open = u"<pre>"_q; close = u"</pre>"_q; break;
		case EntityType::Blockquote:
			open = u"<blockquote>"_q; close = u"</blockquote>"_q; break;
		case EntityType::Spoiler:
			open = u"<span class=\"spoiler\">"_q; close = u"</span>"_q; break;
		case EntityType::Url:
			open = u"<a href=\""_q
				+ text.text.mid(from, till - from).toHtmlEscaped()
				+ u"\">"_q;
			close = u"</a>"_q;
			break;
		case EntityType::CustomUrl:
			open = u"<a href=\""_q + entity.data().toHtmlEscaped() + u"\">"_q;
			close = u"</a>"_q;
			break;
		default: continue;
		}
		++index;
		events.push_back({ from, 2 * 100000 - index, open });
		events.push_back({ till, index, close });
	}
	ranges::sort(events, [](const Event &a, const Event &b) {
		return (a.position != b.position)
			? (a.position < b.position)
			: (a.order < b.order);
	});
	auto result = QString();
	auto position = 0;
	const auto append = [&](int till) {
		result += text.text.mid(position, till - position).toHtmlEscaped();
		position = till;
	};
	for (const auto &event : events) {
		append(event.position);
		result += event.tag;
	}
	append(text.text.size());
	return result.replace('\n', u"<br>"_q);
}

[[nodiscard]] QByteArray ExportHtml(
		const QString &name,
		const std::vector<Message> &messages) {
	auto result = QString();
	result += u"<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">"_q
		u"<title>"_q + name.toHtmlEscaped() + u"</title><style>"_q
		u"body{font-family:sans-serif;background:#f4f4f5;margin:0;"_q
		u"padding:16px;}"_q
		u".m{background:#fff;border-radius:8px;padding:8px 12px;"_q
		u"margin:6px auto;max-width:720px;}"_q
		u".m.out{background:#effdde;}.m.deleted{border-left:3px solid #e53935;}"_q
		u".h{font-size:12px;color:#888;margin-bottom:4px;}"_q
		u".h b{color:#3a6d99;}.s{text-align:center;color:#777;"_q
		u"font-size:13px;}.f{color:#888;font-size:12px;}"_q
		u".spoiler{background:#ccc;}.media{color:#555;margin-top:4px;}"_q
		u"img{max-width:320px;border-radius:6px;display:block;}"_q
		u"</style></head><body><h2>"_q + name.toHtmlEscaped() + u"</h2>\n"_q;
	for (const auto &message : messages) {
		if (message.service) {
			result += u"<div class=\"s\">"_q
				+ FormatDateTime(message.date)
				+ u" — "_q
				+ message.text.text.toHtmlEscaped()
				+ u"</div>\n"_q;
			continue;
		}
		auto classes = u"m"_q;
		if (message.out) {
			classes += u" out"_q;
		}
		if (message.deleted) {
			classes += u" deleted"_q;
		}
		result += u"<div class=\""_q + classes + u"\" id=\"m"_q
			+ QString::number(message.id.bare) + u"\"><div class=\"h\"><b>"_q
			+ message.fromName.toHtmlEscaped() + u"</b> · "_q
			+ FormatDateTime(message.date);
		if (message.edited) {
			result += u" · "_q
				+ tr::lng_chat_archive_edited(tr::now).toHtmlEscaped();
		}
		if (message.deleted) {
			result += u" · "_q + tr::lng_chat_archive_deleted(
				tr::now,
				lt_date,
				FormatDateTime(message.deleted)).toHtmlEscaped();
		}
		result += u"</div>"_q;
		if (!message.forwardFrom.isEmpty()) {
			result += u"<div class=\"f\">"_q + tr::lng_chat_archive_forwarded(
				tr::now,
				lt_user,
				message.forwardFrom).toHtmlEscaped() + u"</div>"_q;
		}
		if (message.replyTo) {
			result += u"<div class=\"f\"><a href=\"#m"_q
				+ QString::number(message.replyTo.bare) + u"\">"_q
				+ tr::lng_chat_archive_reply(
					tr::now,
					lt_id,
					QString::number(message.replyTo.bare)).toHtmlEscaped()
				+ u"</a></div>"_q;
		}
		if (!message.text.text.isEmpty()) {
			result += u"<div>"_q + HtmlText(message.text) + u"</div>"_q;
		}
		if (const auto &media = message.media) {
			result += u"<div class=\"media\">"_q;
			if (!media->file.isEmpty() && QFile::exists(media->file)) {
				const auto url = QUrl::fromLocalFile(media->file).toString();
				if (media->type == u"photo"_q) {
					result += u"<a href=\""_q + url.toHtmlEscaped()
						+ u"\"><img src=\""_q + url.toHtmlEscaped()
						+ u"\"></a>"_q;
				} else {
					result += u"<a href=\""_q + url.toHtmlEscaped() + u"\">"_q
						+ MediaTitle(*media).toHtmlEscaped() + u"</a>"_q;
				}
			} else {
				result += MediaTitle(*media).toHtmlEscaped();
			}
			result += u"</div>"_q;
		}
		result += u"</div>\n"_q;
	}
	result += u"</body></html>\n"_q;
	return result.toUtf8();
}

void Export(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		const QString &name,
		PeerId peerId,
		bool html) {
	auto fileName = name;
	fileName.replace(
		QRegularExpression(u"[\\\\/:*?\"<>|\\x00-\\x1F]"_q),
		u"_"_q);
	if (fileName.isEmpty()) {
		fileName = u"chat"_q;
	}
	const auto extension = html ? u".html"_q : u".json"_q;
	const auto folder = QStandardPaths::writableLocation(
		QStandardPaths::DownloadLocation);
	const auto initial = QDir(folder).filePath(fileName + extension);
	const auto session = &controller->session();
	const auto weak = base::make_weak(controller);
	FileDialog::GetWritePath(
		box.get(),
		tr::lng_chat_archive_export(tr::now),
		html ? u"HTML (*.html)"_q : u"JSON (*.json)"_q,
		initial,
		[=](QString &&path) {
			if (path.isEmpty()) {
				return;
			}
			const auto messages = LoadMessages(session, peerId);
			auto file = QFile(path);
			const auto ok = file.open(QIODevice::WriteOnly)
				&& file.write(html
					? ExportHtml(name, messages)
					: ExportJson(name, peerId, messages)) >= 0;
			file.close();
			if (const auto strong = weak.get()) {
				strong->showToast(ok
					? tr::lng_chat_archive_exported(tr::now)
					: tr::lng_chat_archive_export_failed(tr::now));
			}
		});
}

void FillMessages(
		not_null<Ui::VerticalLayout*> container,
		const std::vector<Message> &messages,
		int count,
		Fn<void()> showMore) {
	container->clear();
	const auto from = std::max(int(messages.size()) - count, 0);
	if (from > 0) {
		const auto more = container->add(
			object_ptr<Ui::LinkButton>(
				container,
				tr::lng_chat_archive_show_earlier(tr::now)),
			st::boxRowPadding);
		more->setClickedCallback(showMore);
		Ui::AddSkip(container);
	}
	if (messages.empty()) {
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				tr::lng_chat_archive_empty(),
				st::boxDividerLabel),
			st::boxRowPadding);
	}
	for (auto i = from; i != int(messages.size()); ++i) {
		const auto &message = messages[i];
		auto text = TextWithEntities();
		if (message.service) {
			text.append(tr::italic(message.text.text));
		} else {
			text.append(tr::bold(message.fromName));
		}
		auto info = u" · "_q + FormatDateTime(message.date);
		if (message.edited) {
			info += u" · "_q + tr::lng_chat_archive_edited(tr::now);
		}
		text.append(info);
		if (message.deleted) {
			text.append(u" · "_q).append(tr::bold(
				tr::lng_chat_archive_deleted(
					tr::now,
					lt_date,
					FormatDateTime(message.deleted))));
		}
		if (!message.forwardFrom.isEmpty()) {
			text.append('\n').append(tr::italic(
				tr::lng_chat_archive_forwarded(
					tr::now,
					lt_user,
					message.forwardFrom)));
		}
		if (message.replyTo) {
			text.append('\n').append(tr::italic(
				tr::lng_chat_archive_reply(
					tr::now,
					lt_id,
					QString::number(message.replyTo.bare))));
		}
		if (!message.service && !message.text.text.isEmpty()) {
			text.append('\n').append(message.text);
		}
		const auto label = container->add(
			object_ptr<Ui::FlatLabel>(container, st::boxLabel),
			st::boxRowPadding);
		label->setMarkedText(text);
		label->setSelectable(true);
		if (const auto &media = message.media) {
			const auto path = media->file;
			const auto exists = !path.isEmpty() && QFile::exists(path);
			const auto line = container->add(
				object_ptr<Ui::FlatLabel>(container, st::boxDividerLabel),
				st::boxRowPadding);
			line->setMarkedText(exists
				? tr::link(MediaTitle(*media))
				: TextWithEntities{ MediaTitle(*media) });
			if (exists) {
				line->setClickHandlerFilter([=](const auto &...) {
					File::Launch(path);
					return false;
				});
			}
		}
		Ui::AddSkip(container);
	}
}

void ChatBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		PeerId peerId,
		QString name) {
	const auto session = &controller->session();
	box->setTitle(rpl::single(name));
	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxWideWidth * 3 / 2);

	struct State {
		std::vector<Message> messages;
		Fn<void()> refill;
		int count = kPageSize;
	};
	const auto state = box->lifetime().make_state<State>();
	state->messages = LoadMessages(session, peerId);

	const auto container = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	state->refill = [=] {
		FillMessages(container, state->messages, state->count, [=] {
			state->count += kPageSize;
			crl::on_main(box.get(), [=] { state->refill(); });
		});
		container->resizeToWidth(st::boxWideWidth);
	};
	state->refill();
	box->setInitScrollCallback([=] {
		box->scrollToY(std::numeric_limits<int>::max());
	});

	box->addButton(tr::lng_chat_archive_export_html(), [=] {
		Export(box, controller, name, peerId, true);
	});
	box->addButton(tr::lng_chat_archive_export_json(), [=] {
		Export(box, controller, name, peerId, false);
	});
	box->addLeftButton(tr::lng_chat_archive_delete(), [=] {
		controller->show(Ui::MakeConfirmBox({
			.text = tr::lng_chat_archive_delete_sure(),
			.confirmed = [=](Fn<void()> close) {
				RemoveArchive(session, peerId);
				close();
				box->closeBox();
			},
			.confirmText = tr::lng_box_delete(),
		}));
	});
}

void ListBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	box->setTitle(tr::lng_chat_archive_title());
	box->setWidth(st::boxWideWidth);
	const auto container = box->verticalLayout();
	const auto chats = Chats(session);
	if (chats.empty()) {
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_chat_archive_list_empty(),
				st::boxDividerLabel),
			st::boxRowPadding);
	}
	for (const auto &chat : chats) {
		auto status = QStringList();
		if (chat.updated) {
			status.push_back(FormatDate(chat.updated));
		}
		if (ChatGone(session, chat.peerId)) {
			status.push_back(tr::lng_chat_archive_chat_gone(tr::now));
		} else if (!chat.enabled) {
			status.push_back(tr::lng_chat_archive_paused(tr::now));
		}
		const auto peerId = chat.peerId;
		const auto name = chat.name.isEmpty()
			? QString::number(peerId.value)
			: chat.name;
		Settings::AddButtonWithLabel(
			container,
			rpl::single(name),
			rpl::single(status.join(u" · "_q)),
			st::settingsButtonNoIcon
		)->setClickedCallback([=] {
			controller->show(Box(ChatBox, controller, peerId, name));
		});
	}
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	box->addLeftButton(tr::lng_chat_archive_open_folder(), [=] {
		const auto root = RootFolder(session);
		QDir().mkpath(root);
		File::ShowInFolder(root + u"index.json"_q);
	});
}

} // namespace

void FillPeerMenu(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		const Ui::Menu::MenuCallback &addAction) {
	if (!MasterEnabled()) {
		return;
	}
	const auto enabled = IsEnabled(peer);
	addAction(
		(enabled
			? tr::lng_chat_archive_disable(tr::now)
			: tr::lng_chat_archive_enable(tr::now)),
		[=] {
			SetEnabled(peer, !enabled);
			controller->showToast(enabled
				? tr::lng_chat_archive_disabled_toast(tr::now)
				: tr::lng_chat_archive_enabled_toast(tr::now));
		},
		enabled ? &st::menuIconUnarchive : &st::menuIconArchive);
	if (enabled || HasArchive(&peer->session(), peer->id)) {
		const auto peerId = peer->id;
		const auto name = peer->name();
		addAction(
			tr::lng_chat_archive_open(tr::now),
			[=] { controller->show(Box(ChatBox, controller, peerId, name)); },
			&st::menuIconExport);
	}
}

void ShowArchiveList(not_null<Window::SessionController*> controller) {
	controller->show(Box(ListBox, controller));
}

void SetupSettings(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, tr::lng_chat_archive_settings());

	Settings::AddButtonWithIcon(
		container,
		tr::lng_chat_archive_master(),
		st::settingsButtonNoIcon
	)->toggleOn(
		rpl::single(MasterEnabled())
	)->toggledValue(
	) | rpl::filter([](bool enabled) {
		return (enabled != MasterEnabled());
	}) | rpl::on_next([=](bool enabled) {
		SetMasterEnabled(enabled);
	}, container->lifetime());

	const auto limitChanges = container->lifetime().make_state<
		rpl::event_stream<int>>();
	Settings::AddButtonWithLabel(
		container,
		tr::lng_chat_archive_media_limit(),
		rpl::single(
			MediaLimitMb()
		) | rpl::then(
			limitChanges->events()
		) | rpl::map(MediaLimitText),
		st::settingsButtonNoIcon
	)->setClickedCallback([=] {
		auto options = std::vector<QString>();
		auto selected = 0;
		for (auto i = 0; i != int(kMediaLimits.size()); ++i) {
			options.push_back(MediaLimitText(kMediaLimits[i]));
			if (kMediaLimits[i] == MediaLimitMb()) {
				selected = i;
			}
		}
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			SingleChoiceBox(box, {
				.title = tr::lng_chat_archive_media_limit(),
				.options = options,
				.initialSelection = selected,
				.callback = [=](int index) {
					SetMediaLimitMb(kMediaLimits[index]);
					limitChanges->fire_copy(kMediaLimits[index]);
				},
			});
		}));
	});

	Settings::AddButtonWithIcon(
		container,
		tr::lng_chat_archive_open_list(),
		st::settingsButtonNoIcon
	)->setClickedCallback([=] {
		ShowArchiveList(controller);
	});

	Ui::AddDividerText(container, tr::lng_chat_archive_about());
	Ui::AddSkip(container);

	Settings::AddButtonWithIcon(
		container,
		tr::lng_profile_snapshot_setting(),
		st::settingsButtonNoIcon
	)->toggleOn(
		rpl::single(ProfileSnapshots::Enabled())
	)->toggledValue(
	) | rpl::filter([](bool enabled) {
		return (enabled != ProfileSnapshots::Enabled());
	}) | rpl::on_next([=](bool enabled) {
		ProfileSnapshots::SetEnabled(enabled);
	}, container->lifetime());

	Ui::AddDividerText(container, tr::lng_profile_snapshot_about());
}

} // namespace ChatArchive
