/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_experimental.h"

#include "settings/settings_common.h"
#include "data/components/passkeys.h"
#include "ui/layers/generic_box.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/search_field_controller.h"
#include "ui/text/text_entity.h"
#include "ui/toast/toast.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/kinetic_scroller.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/vertical_list.h"
#include "ui/gl/gl_detection.h"
#include "ui/chat/chat_style_radius.h"
#include "ui/controls/compose_ai_button_factory.h"
#include "base/options.h"
#include "boxes/moderate_messages_box.h"
#include "core/application.h"
#include "core/launcher.h"
#include "core/sandbox.h"
#include "chat_helpers/tabbed_panel.h"
#include "dialogs/dialogs_entry.h"
#include "dialogs/dialogs_widget.h"
#include "dialogs/dialogs_inner_widget.h"
#include "dialogs/ui/dialogs_layout.h"
#include "ffmpeg/ffmpeg_utility.h"
#include "history/history_item_components.h"
#include "history/view/controls/compose_controls_common.h"
#include "history/view/history_view_message.h"
#include "info/profile/info_profile_actions.h"
#include "info/profile/tabs/adapters/info_profile_tab_media.h"
#include "info/profile/tabs/info_profile_tabs_host.h"
#include "lang/lang_keys.h"
#include "lang/lang_instance.h"
#include "mainwindow.h"
#include "mainwidget.h"
#include "media/player/media_player_instance.h"
#include "mtproto/session_private.h"
#include "webview/webview_embed.h"
#include "window/main_window.h"
#include "window/window_filters_favorite.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "window/window_controller.h"
#include "window/notifications_manager.h"
#include "info/info_flexible_scroll.h"
#include "chat_helpers/stickers_list_widget.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_settings.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QJsonDocument>

#include <array>
#include <QtGui/QGuiApplication>

#include "ui/chat/chat_style.h"

namespace Settings {
namespace {

// Russian texts for the experimental options. The options themselves only
// carry English, and custom keys are not in the cloud language packs, so
// the translation lives here and follows the interface language.
struct RuOptionText {
	const char *id = nullptr;
	const char *name = nullptr;
	const char *description = nullptr;
};

constexpr auto kRuOptionTexts = std::array{
	RuOptionText{ "ai-apply-tone-slug", "Тон ИИ для быстрого применения", "Идентификатор тона ИИ-редактора, привязанного к горячей клавише применения на месте. Пусто — тон не привязан." },
	RuOptionText{ "auto-scroll-inactive-chat", "Читать неактивный чат", "Отмечать новые сообщения прочитанными и прокручивать чат, даже когда окно не в фокусе." },
	RuOptionText{ "classic-profile-scroll", "Классическая прокрутка профиля", "Возвращает прежнюю реализацию прокрутки обложки профиля." },
	RuOptionText{ "ctrl-click-chat-new-window", "Новое окно чата по Ctrl+клику", "Открывать чат в новом окне по Ctrl+клику (Cmd+клику на macOS)." },
	RuOptionText{ "custom-notification", "Разрешить нестандартные уведомления", "Позволяет отключить системные уведомления, даже если собственные уведомления Telegram на этой платформе работают плохо." },
	RuOptionText{ "deadlock-detector", "Детектор зависаний", "Каждые 30 секунд проверять, что главный поток приложения отвечает." },
	RuOptionText{ "dialogs-mute-icon", "Значок «без звука» в списке чатов", "Показывать маленький значок рядом с названием чатов с отключённым звуком." },
	RuOptionText{ "dialogs-unread-on-top", "Непрочитанные чаты сверху", "Ставить чаты с новыми сообщениями (кроме заглушённых) сразу под закреплёнными и держать там, пока вы их не прочитаете." },
	RuOptionText{ "disable-autoplay-next", "Не воспроизводить следующий трек", "Отключить автоматическое воспроизведение следующего аудиофайла, голосового или видеосообщения." },
	RuOptionText{ "enable-vulkan-rhi", "Включить рендеринг через Vulkan", "Использовать Vulkan вместо OpenGL для отрисовки на видеокарте, если он доступен." },
	RuOptionText{ "external-media-viewer", "Внешний просмотрщик медиа", "Открывать фото и видео в системном приложении вместо встроенного просмотрщика." },
	RuOptionText{ "fast-buttons-mode", "Режим быстрых кнопок", "Нажимать кнопки встроенной клавиатуры ботов клавишами 1–9." },
	RuOptionText{ "ffmpeg-multithread", "Многопоточное декодирование видео", "Разрешить FFmpeg декодировать видео в нескольких потоках, обычно по одному на поток процессора." },
	RuOptionText{ "folder-favorite-link", "Кнопка «Избранная папка»", "Кнопка внизу полосы папок, открывающая ссылку tg:// или t.me/." },
	RuOptionText{ "force-angle", "Принудительно использовать ANGLE", "Использовать ANGLE для ускорения медиа и звонков. Главное окно отрисовывается процессором." },
	RuOptionText{ "force-compose-search-one-column", "Встроенный поиск в чатах", "Принудительно использовать встроенный поиск в чатах в одноколоночном режиме." },
	RuOptionText{ "forum-hide-chats-list", "Скрывать список чатов в форумах", "Не показывать узкую колонку со списком чатов." },
	RuOptionText{ "fractional-scaling-enabled", "Точное масштабирование High DPI", "Точно следовать системному масштабу интерфейса." },
	RuOptionText{ "freetype", "Шрифтовой движок FreeType", "Использовать шрифтовой движок из Linux вместо системного." },
	RuOptionText{ "gnotification", "GNotification", "Принудительно включить GNotification из GLib. Если выключено, определяется автоматически." },
	RuOptionText{ "hide-ai-button", "Скрыть кнопку ИИ", "Скрыть кнопку инструментов ИИ в поле ввода сообщения." },
	RuOptionText{ "hide-reply-button", "Скрыть кнопку «Ответить»", "Скрыть кнопку ответа в уведомлениях." },
	RuOptionText{ "high-dpi-downscale", "Уменьшение для High DPI", "Точно следовать системному масштабу интерфейса (другой способ, обычно с лучшим качеством)." },
	RuOptionText{ "kinetic-scroller", "Своя кинетическая прокрутка для тачпадов", "" },
	RuOptionText{ "mac-cmd-reply-immediately", "Mac: мгновенный ответ по Cmd + ↑/↓", "Сразу отвечать на предыдущее или следующее сообщение по Cmd + ↑/↓, а не переносить сначала курсор в начало или конец поля ввода. С Shift курсор перемещается как раньше." },
	RuOptionText{ "mac-modern-notifications", "Современные уведомления macOS", "Использовать фреймворк UserNotifications для системных уведомлений (macOS 10.14+). При первом запуске система спросит разрешение. После включения он остаётся в работе, даже если опцию выключить: macOS перестаёт показывать старые уведомления." },
	RuOptionText{ "moderate-common-groups", "Блокировать пользователей сразу в нескольких группах", "" },
	RuOptionText{ "new-windows-size-as-first", "Размер новых окон чатов", "Открывать новые окна с размером главного окна." },
	RuOptionText{ "prefer-ipv6", "Предпочитать IPv6", "Использовать IPv6, если он доступен. Требует включённой опции «Пробовать подключаться через IPv6»." },
	RuOptionText{ "profile-media-tabs", "Медиа в профиле вкладками", "Заменить кнопки общих медиа в профиле полосой вкладок со списками прямо в профиле. В разработке." },
	RuOptionText{ "profile-media-tabs-expanded", "Раздельные вкладки фото и видео", "Показывать в профиле отдельные вкладки для фото и для видео вместо одной общей вкладки медиа." },
	RuOptionText{ "show-channel-joined-below-about", "Дата вступления в канал в профиле", "Показывать под описанием канала, когда вы в него вступили." },
	RuOptionText{ "show-peer-id-below-about", "ID в профиле", "Показывать ID из API под описанием или «О себе». Добавлять ID контактов в экспорт данных." },
	RuOptionText{ "skip-url-scheme-register", "Не регистрировать схему tg://", "Не перерегистрировать схему ссылок tg:// при автообновлении." },
	RuOptionText{ "tabbed-panel-show-on-click", "Панель эмодзи по клику", "Открывать панель эмодзи, стикеров и GIF только по клику." },
	RuOptionText{ "touchbar-disabled", "Отключить Touch Bar (только macOS)", "" },
	RuOptionText{ "translate-url-template", "Шаблон ссылки переводчика", "Шаблон URL для своего сервиса перевода. Поддерживает %q — текст, %f — исходный язык, %t — язык перевода." },
	RuOptionText{ "uncolored_quote", "Цитаты без цвета", "Не раскрашивать цитаты, превью ссылок, игры и т.п." },
	RuOptionText{ "unlimited-message-width", "Неограниченная ширина сообщений", "Разрешить облачкам с текстом растягиваться шире стандартного максимума." },
	RuOptionText{ "unlimited-recent-stickers", "Неограниченные недавние стикеры", "Показывать столько недавних стикеров, сколько отдаёт сервер." },
	RuOptionText{ "use-modern-chat-view", "Новый вид чата", "Открывать чаты через новый раздел." },
	RuOptionText{ "use-qt-rhi", "Рендеринг через Qt RHI", "Отрисовывать главное окно видеокартой, а не процессором." },
	RuOptionText{ "use-small-msg-bubble-radius", "Маленькое скругление облачков", "Делает большинство облачков сообщений почти квадратными." },
	RuOptionText{ "view-profile-in-chats-list-context-menu", "Пункт «Открыть профиль»", "Добавить «Открыть профиль» в контекстное меню списка чатов." },
	RuOptionText{ "webview-debug-enabled", "Отладка WebView", "Правый клик → «Inspect» в окнах WebView (на macOS запустите Safari и откройте через меню «Разработка»)." },
	RuOptionText{ "webview-legacy-edge", "Принудительно старый Edge WebView", "Пропустить проверку CoreWebView2 и использовать старый Edge WebView в Windows." },
};

[[nodiscard]] bool UseRussian() {
	const auto &instance = Lang::GetInstance();
	return instance.id().startsWith(u"ru"_q)
		|| instance.baseId().startsWith(u"ru"_q);
}

[[nodiscard]] const RuOptionText *LookupRuOption(const QString &id) {
	if (!UseRussian()) {
		return nullptr;
	}
	for (const auto &entry : kRuOptionTexts) {
		if (id == QLatin1String(entry.id)) {
			return &entry;
		}
	}
	return nullptr;
}

[[nodiscard]] QString RuOr(const QString &english, const char *russian) {
	return UseRussian() ? QString::fromUtf8(russian) : english;
}

template <typename Option>
[[nodiscard]] QString OptionName(const Option &option) {
	if (const auto ru = LookupRuOption(option.id())) {
		return QString::fromUtf8(ru->name);
	}
	return option.name().isEmpty() ? option.id() : option.name();
}

template <typename Option>
[[nodiscard]] QString OptionDescription(const Option &option) {
	if (const auto ru = LookupRuOption(option.id())) {
		return QString::fromUtf8(ru->description);
	}
	return option.description();
}

// Both languages stay searchable, so English names still find options.
template <typename Option>
[[nodiscard]] QString OptionSearchable(const Option &option) {
	auto result = OptionName(option) + ' ' + OptionDescription(option);
	if (LookupRuOption(option.id())) {
		result += ' ' + option.name() + ' ' + option.description();
	}
	return result;
}

const auto kOptionsClipboardPrefix = u"tdesktop-flags:"_q;

struct DecodeOptionsResult {
	bool ok = false;
	QString json;
};

[[nodiscard]] QString EncodeOptionsToText(const QString &json) {
	const auto flags = QByteArray::Base64UrlEncoding
		| QByteArray::OmitTrailingEquals;
	return kOptionsClipboardPrefix
		+ qs(qCompress(json.toLatin1(), 9).toBase64(flags));
}

[[nodiscard]] DecodeOptionsResult DecodeOptionsFromText(const QString &text) {
	auto result = DecodeOptionsResult();
	if (!text.startsWith(kOptionsClipboardPrefix)) {
		return result;
	}
	auto encoded = QStringView(text).mid(
		kOptionsClipboardPrefix.size()).toLatin1();
	const auto compressed = QByteArray::fromBase64Encoding(
		std::move(encoded),
		QByteArray::Base64UrlEncoding
			| QByteArray::AbortOnBase64DecodingErrors);
	if (!compressed || (*compressed).isEmpty()) {
		return result;
	}
	const auto decoded = qUncompress(*compressed);
	if (decoded.isEmpty()) {
		return result;
	}

	auto error = QJsonParseError();
	const auto parsed = QJsonDocument::fromJson(decoded, &error);
	if ((error.error != QJsonParseError::NoError) || !parsed.isObject()) {
		return result;
	}
	result.ok = true;
	result.json = QString::fromUtf8(decoded);
	return result;
}

enum class ImportResult {
	Done,
	Invalid,
	Unsupported,
};

[[nodiscard]] ImportResult ImportOptions(const QString &text) {
	const auto decoded = DecodeOptionsFromText(text.trimmed());
	if (!decoded.ok) {
		return ImportResult::Invalid;
	}
	return base::options::deserialize(decoded.json)
		? ImportResult::Done
		: ImportResult::Unsupported;
}

void ImportOptionsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::Controller*> window,
		Fn<void()> imported) {
	box->setTitle(rpl::single(RuOr(
		u"Import experimental settings"_q,
		"Импорт экспериментальных настроек")));

	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::NoNewlines,
		rpl::single(RuOr(
			u"Experimental settings code"_q,
			"Код экспериментальных настроек"))));

	const auto save = [=] {
		const auto result = ImportOptions(field->getLastText());
		if (result == ImportResult::Invalid) {
			field->showError();
			box->showToast(RuOr(
				u"This is not a valid experimental settings code."_q,
				"Это неверный код экспериментальных настроек."));
			return;
		} else if (result == ImportResult::Unsupported) {
			field->showError();
			box->showToast(RuOr(
				u"Experimental settings code is valid"
				", but data format is not supported."_q,
				"Код верный, но формат данных не поддерживается."));
			return;
		}
		imported();
		box->closeBox();
		window->showToast(RuOr(
			u"Experimental settings imported."_q,
			"Экспериментальные настройки импортированы."));
	};
	field->submits() | rpl::on_next(save, field->lifetime());

	box->addButton(tr::lng_settings_save(), save);
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
	box->setFocusCallback([=] {
		field->setFocusFast();
	});
}

void SetupCopyDeepLink(
		not_null<Window::Controller*> window,
		not_null<Button*> button,
		const QString &id) {
	const auto link = u"tg://settings/experimental/"_q + id;
	const auto menu
		= button->lifetime().make_state<base::unique_qptr<Ui::PopupMenu>>();
	button->events(
	) | rpl::filter([](not_null<QEvent*> e) {
		return e->type() == QEvent::ContextMenu;
	}) | rpl::on_next([=](not_null<QEvent*> e) {
		*menu = base::make_unique_q<Ui::PopupMenu>(
			button,
			st::popupMenuWithIcons);
		(*menu)->addAction(RuOr(
			u"Copy deep link"_q,
			"Копировать ссылку на настройку"), [=] {
			TextUtilities::SetClipboardText({ link });
			window->showToast({
				.text = { RuOr(
					u"Deep link copied to clipboard."_q,
					"Ссылка скопирована в буфер обмена.") },
				.iconLottie = u"toast/voip_invite"_q,
				.iconLottieSize = st::toastLottieIconSize,
			});
		}, &st::menuIconCopy);
		(*menu)->popup(QCursor::pos());
		e->accept();
	}, button->lifetime());
}

[[nodiscard]] not_null<Button*> AddOptionRow(
		not_null<Ui::VerticalLayout*> container,
		const QString &name,
		const QString &description,
		const style::SettingsButton &st) {
	if (description.isEmpty()) {
		return container->add(object_ptr<Button>(
			container,
			rpl::single(name),
			st));
	}
	const auto &titlePadding = st::settingsExperimentalTitlePadding;
	const auto &aboutPadding = st::settingsExperimentalAboutPadding;
	const auto button = Ui::CreateChild<Button>(
		container.get(),
		rpl::single(QString()),
		st);
	const auto title = container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			name,
			st::settingsExperimentalTitle),
		titlePadding);
	const auto about = container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			description,
			st::settingsExperimentalAbout),
		aboutPadding);
	title->setAttribute(Qt::WA_TransparentForMouseEvents);
	about->setAttribute(Qt::WA_TransparentForMouseEvents);
	rpl::combine(
		container->widthValue(),
		title->heightValue(),
		about->heightValue()
	) | rpl::on_next([=](int width, int titleHeight, int aboutHeight) {
		button->resize(width, titlePadding.top()
			+ titleHeight
			+ titlePadding.bottom()
			+ aboutPadding.top()
			+ aboutHeight
			+ aboutPadding.bottom());
	}, button->lifetime());
	title->topValue(
	) | rpl::on_next([=](int top) {
		button->moveToLeft(0, top - titlePadding.top());
	}, button->lifetime());
	button->show();
	return button;
}

QString AddOption(
		not_null<Window::Controller*> window,
		not_null<Ui::VerticalLayout*> container,
		base::options::option<bool> &option,
		rpl::producer<> resetClicks,
		rpl::producer<> reloadOptionsRequests,
		rpl::producer<QString> query,
		Fn<void(const QString&, not_null<QWidget*>)> registerHighlight) {
	const auto name = OptionName(option);
	const auto description = OptionDescription(option);

	const auto wrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)),
		style::margins(),
		style::al_justify);
	const auto inner = wrap->entity();

	auto &lifetime = inner->lifetime();
	const auto toggles = lifetime.make_state<rpl::event_stream<bool>>();
	std::move(
		resetClicks
	) | rpl::map_to(
		option.defaultValue()
	) | rpl::start_to_stream(*toggles, lifetime);
	std::move(reloadOptionsRequests) | rpl::on_next([=, &option] {
		toggles->fire_copy(option.value());
	}, lifetime);

	const auto button = AddOptionRow(
		inner,
		name,
		description,
		(option.relevant()
			? st::settingsButtonNoIcon
			: st::settingsOptionDisabled)
	)->toggleOn(toggles->events_starting_with(option.value()));

	if (registerHighlight) {
		registerHighlight(u"experimental/"_q + option.id(), button);
	}

	SetupCopyDeepLink(window, button, option.id());

	const auto restarter = (option.relevant() && option.restartRequired())
		? button->lifetime().make_state<base::Timer>()
		: nullptr;
	if (restarter) {
		restarter->setCallback([=] {
			window->show(Ui::MakeConfirmBox({
				.text = tr::lng_settings_need_restart(),
				.confirmed = [] { Core::Restart(); },
				.confirmText = tr::lng_settings_restart_now(),
				.cancelText = tr::lng_settings_restart_later(),
			}));
		});
	}
	button->toggledChanges(
	) | rpl::on_next([=, &option](bool toggled) {
		if (!option.relevant() && toggled != option.defaultValue()) {
			toggles->fire_copy(option.defaultValue());
			window->showToast(
				tr::lng_settings_experimental_irrelevant(tr::now));
			return;
		}
		option.set(toggled);
		if (restarter) {
			restarter->callOnce(st::settingsButtonNoIcon.toggle.duration);
		}
	}, inner->lifetime());

	const auto searchable = OptionSearchable(option);
	const auto terms = SearchWords(searchable);
	std::move(
		query
	) | rpl::on_next([=](const QString &text) {
		wrap->toggle(
			MatchesWords(terms, SearchWords(text)),
			anim::type::instant);
	}, wrap->lifetime());

	return searchable;
}

QString AddFavoriteLinkButton(
		not_null<Window::Controller*> window,
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> query,
		Fn<void(const QString&, not_null<QWidget*>)> registerHighlight) {
	const auto option = &base::options::lookup<QString>(
		Window::kOptionFolderFavoriteLink);
	const auto name = OptionName(*option);
	const auto searchable = OptionSearchable(*option);

	const auto wrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)),
		style::margins(),
		style::al_justify);
	const auto inner = wrap->entity();

	auto label = rpl::single(
		rpl::empty
	) | rpl::then(
		option->changes()
	) | rpl::map([option] {
		return option->value();
	});
	const auto button = AddButtonWithLabel(
		inner,
		rpl::single(name),
		std::move(label),
		st::settingsButtonNoIcon);
	button->setClickedCallback([=] {
		window->show(Box(Window::EditFolderFavoriteLinkBox));
	});

	if (registerHighlight) {
		registerHighlight(u"experimental/"_q + option->id(), button);
	}

	SetupCopyDeepLink(window, button, option->id());

	const auto terms = SearchWords(searchable);
	std::move(
		query
	) | rpl::on_next([=](const QString &text) {
		wrap->toggle(
			MatchesWords(terms, SearchWords(text)),
			anim::type::instant);
	}, wrap->lifetime());

	return searchable;
}

void SetupExperimental(
		not_null<Window::Controller*> window,
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<> reloadOptionsRequests,
		rpl::producer<QString> query,
		Fn<void(const QString&, not_null<QWidget*>)> registerHighlight) {
	const auto headerWrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	const auto header = headerWrap->entity();

	Ui::AddSkip(header, st::settingsCheckboxesSkip);

	header->add(
		object_ptr<Ui::FlatLabel>(
			header,
			tr::lng_settings_experimental_about(),
			st::boxLabel),
		st::defaultBoxDividerLabelPadding);

	auto reset = (Button*)nullptr;
	if (base::options::changed()) {
		const auto wrap = header->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				header,
				object_ptr<Ui::VerticalLayout>(header)));
		const auto inner = wrap->entity();
		Ui::AddDivider(inner);
		Ui::AddSkip(inner, st::settingsCheckboxesSkip);
		reset = inner->add(object_ptr<Button>(
			inner,
			tr::lng_settings_experimental_restore(),
			st::settingsButtonNoIcon));
		reset->addClickHandler([=] {
			base::options::reset();
			wrap->hide(anim::type::normal);
		});
		Ui::AddSkip(inner, st::settingsCheckboxesSkip);
	}

	Ui::AddDivider(header);

	rpl::duplicate(
		query
	) | rpl::on_next([=](const QString &text) {
		headerWrap->toggle(text.trimmed().isEmpty(), anim::type::instant);
	}, headerWrap->lifetime());

	struct Category {
		QString title;
		std::vector<const char*> options;
	};
	const auto categories = std::vector<Category>{
		{
			RuOr(u"Chats"_q, "Чаты"),
			{
				Dialogs::kOptionForumHideChatsList,
				Dialogs::kOptionDialogsUnreadOnTop,
				Dialogs::Ui::kOptionDialogsMuteIcon,
				kOptionUseNewChatView,
				kOptionAutoScrollInactiveChat,
				kModerateCommonGroups,
				Info::kClassicProfileScroll,
			}
		},
		{
			RuOr(u"Messages"_q, "Сообщения"),
			{
				Ui::kOptionUseSmallMsgBubbleRadius,
				HistoryView::kOptionUnlimitedMessageWidth,
				HistoryView::Controls::kOptionMacCmdReplyImmediately,
				Ui::kOptionHideAiButton,
				kForceComposeSearchOneColumn,
				Ui::kOptionUncoloredQuote,
			}
		},
		{
			RuOr(u"Profile"_q, "Профиль"),
			{
				Window::kOptionViewProfileInChatsListContextMenu,
				Info::Profile::kOptionShowPeerIdBelowAbout,
				Info::Profile::kOptionShowChannelJoinedBelowAbout,
				Info::Profile::kOptionProfileMediaTabs,
				Info::Profile::kOptionProfileMediaTabsExpanded,
			}
		},
		{
			RuOr(u"Stickers and emoji"_q, "Стикеры и эмодзи"),
			{
				ChatHelpers::kOptionTabbedPanelShowOnClick,
				ChatHelpers::kOptionUnlimitedRecentStickers,
			}
		},
		{
			RuOr(u"Media"_q, "Медиа"),
			{
				Media::Player::kOptionDisableAutoplayNext,
				Window::kOptionExternalMediaViewer,
				FFmpeg::kOptionFFmpegMultiThread,
			}
		},
		{
			RuOr(u"Notifications"_q, "Уведомления"),
			{
				Window::Notifications::kOptionHideReplyButton,
				Window::Notifications::kOptionCustomNotification,
				Window::Notifications::kOptionGNotification,
				Window::Notifications::kOptionMacModernNotifications,
			}
		},
		{
			RuOr(u"Interface"_q, "Интерфейс"),
			{
				Core::kOptionFractionalScalingEnabled,
				Core::kOptionHighDpiDownscale,
				Ui::GL::kOptionUseQtRhi,
				Ui::GL::kOptionForceANGLE,
				Ui::GL::kOptionEnableVulkanRhi,
				Core::kOptionFreeType,
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && !defined DESKTOP_APP_DISABLE_PRIVATE_QT_API
				Ui::kOptionKineticScroller,
#endif
				Window::kOptionDisableTouchbar,
				Window::kOptionNewWindowsSizeAsFirst,
			}
		},
		{
			RuOr(u"System"_q, "Система"),
			{
				MTP::details::kOptionPreferIPv6,
				Core::kOptionSkipUrlSchemeRegister,
				Core::kOptionDeadlockDetector,
				Webview::kOptionWebviewDebugEnabled,
				Webview::kOptionWebviewLegacyEdge,
			}
		},
	};

	const auto addOption = [&](
			not_null<Ui::VerticalLayout*> inner,
			const char name[]) {
		return AddOption(
			window,
			inner,
			base::options::lookup<bool>(name),
			(reset
				? (reset->clicks() | rpl::to_empty)
				: rpl::producer<>()),
			rpl::duplicate(reloadOptionsRequests),
			rpl::duplicate(query),
			registerHighlight);
	};
	const auto addCategory = [&](
			const QString &title,
			auto &&fill) {
		const auto wrap = container->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				container,
				object_ptr<Ui::VerticalLayout>(container)));
		const auto inner = wrap->entity();
		Ui::AddSkip(inner);
		Ui::AddSubsectionTitle(inner, rpl::single(title));
		auto searchable = std::vector<QString>();
		fill(inner, searchable);
		Ui::AddSkip(inner);
		Ui::AddDivider(inner);

		auto terms = std::vector<QStringList>();
		for (const auto &entry : searchable) {
			terms.push_back(SearchWords(entry));
		}
		rpl::duplicate(
			query
		) | rpl::on_next([=](const QString &text) {
			const auto words = SearchWords(text);
			const auto matches = words.isEmpty()
				|| ranges::any_of(terms, [&](const QStringList &entry) {
					return MatchesWords(entry, words);
				});
			wrap->toggle(matches, anim::type::instant);
		}, wrap->lifetime());
	};

	for (const auto &category : categories) {
		addCategory(category.title, [&](
				not_null<Ui::VerticalLayout*> inner,
				std::vector<QString> &searchable) {
			for (const auto name : category.options) {
				searchable.push_back(addOption(inner, name));
			}
		});
	}

	addCategory(RuOr(u"Other"_q, "Другое"), [&](
			not_null<Ui::VerticalLayout*> inner,
			std::vector<QString> &searchable) {
		if (base::options::lookup<bool>(kOptionFastButtonsMode).value()) {
			searchable.push_back(addOption(inner, kOptionFastButtonsMode));
		}
		searchable.push_back(AddFavoriteLinkButton(
			window,
			inner,
			rpl::duplicate(query),
			registerHighlight));
	});
}

} // namespace

Experimental::Experimental(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

Experimental::~Experimental() = default;

rpl::producer<QString> Experimental::title() {
	return tr::lng_settings_experimental();
}

void Experimental::fillTopBarMenu(const Ui::Menu::MenuCallback &addAction) {
	const auto window = &controller()->window();
	addAction(
		RuOr(u"Export"_q, "Экспорт"),
		[=] {
			TextUtilities::SetClipboardText(
				{ EncodeOptionsToText(base::options::serialize()) });
			window->showToast({
				.text = { RuOr(
					u"Experimental settings code copied to clipboard."_q,
					"Код экспериментальных настроек скопирован.") },
				.iconLottie = u"toast/copy"_q,
				.iconLottieSize = st::toastLottieIconSize,
			});
		},
		&st::menuIconCopy);
	const auto reload = [=] {
		_reloadOptionsRequests.fire({});
	};
	addAction(
		RuOr(u"Import"_q, "Импорт"),
		[=] {
			const auto result = ImportOptions(
				QGuiApplication::clipboard()->text());
			if (result == ImportResult::Invalid) {
				window->show(Box(ImportOptionsBox, window, reload));
				return;
			} else if (result == ImportResult::Unsupported) {
				window->showToast(RuOr(
					u"Experimental settings code is valid"
					", but data format is not supported."_q,
					"Код верный, но формат данных не поддерживается."));
				return;
			}
			reload();
			window->showToast(RuOr(
				u"Experimental settings imported "
				"from code in clipboard."_q,
				"Экспериментальные настройки импортированы из буфера обмена."));
		},
		&st::menuIconImportTheme);
}

void Experimental::setInnerFocus() {
	if (_searchField) {
		_searchField->setFocus();
	} else {
		setFocus();
	}
}

void Experimental::showFinished() {
	AbstractSection::showFinished();
	for (const auto &[id, widget] : _highlights) {
		if (widget) {
			controller()->checkHighlightControl(id, widget);
		}
	}
}

base::weak_qptr<Ui::RpWidget> Experimental::createPinnedToTop(
		not_null<QWidget*> parent) {
	auto search = CreateSectionSearchRow(parent, _query.current());
	_searchController = std::move(search.controller);
	const auto row = search.row;
	_searchField = search.field;

	_searchController->queryValue(
	) | rpl::on_next([=](QString text) {
		_query = std::move(text);
	}, row->lifetime());

	return base::make_weak(row);
}

void Experimental::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	SetupExperimental(
		&controller()->window(),
		content,
		_reloadOptionsRequests.events(),
		_query.value(),
		[this](const QString &id, not_null<QWidget*> widget) {
			_highlights.push_back({ id, widget.get() });
		});

	Ui::ResizeFitChild(this, content);
}

} // namespace Settings
