/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#include "archive/archive_profiles.h"

#include "base/timer.h"
#include "base/unixtime.h"
#include "core/enhanced_settings.h"
#include "core/file_utilities.h"
#include "data/data_changes.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/userpic_view.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtGui/QImageReader>

namespace ProfileSnapshots {
namespace {

constexpr auto kWriteTimeout = crl::time(5000);
constexpr auto kRefreshDateAfter = TimeId(24 * 60 * 60);
constexpr auto kMaxPendingPhotos = 32;
constexpr auto kPendingTimeout = crl::time(120000);
constexpr auto kFullPhotoMinWidth = 320;

struct Field {
	QString value;
	TimeId date = 0;
};

struct Snapshot {
	Field name;
	Field username;
	Field about;
	Field photo;
	PhotoId photoId = 0;
};

struct PendingPhoto {
	not_null<PeerData*> peer;
	PhotoId photoId = 0;
	Ui::PeerUserpicView view;
	std::shared_ptr<Data::PhotoMedia> large;
	crl::time added = 0;
};

struct Store {
	explicit Store(not_null<Main::Session*> session);
	~Store();

	const not_null<Main::Session*> session;
	const QString root;
	base::flat_map<PeerId, Snapshot> snapshots;
	std::vector<PendingPhoto> pending;
	base::Timer writeTimer;
	rpl::lifetime lifetime;
};

base::flat_map<not_null<Main::Session*>, std::unique_ptr<Store>> &Stores() {
	static auto result = base::flat_map<
		not_null<Main::Session*>,
		std::unique_ptr<Store>>();
	return result;
}

[[nodiscard]] Store *Find(not_null<Main::Session*> session) {
	auto &stores = Stores();
	const auto i = stores.find(session);
	return (i != end(stores)) ? i->second.get() : nullptr;
}

[[nodiscard]] QString PeerKey(PeerId peerId) {
	return QString::number(peerId.value);
}

[[nodiscard]] QString PhotoPath(not_null<Store*> store, PeerId peerId) {
	return store->root + u"photos/"_q + PeerKey(peerId) + u".jpg"_q;
}

[[nodiscard]] QJsonObject SerializeField(const Field &field) {
	return QJsonObject{
		{ u"value"_q, field.value },
		{ u"date"_q, field.date },
	};
}

[[nodiscard]] Field ParseField(const QJsonValue &value) {
	const auto object = value.toObject();
	return {
		.value = object.value(u"value"_q).toString(),
		.date = object.value(u"date"_q).toInt(),
	};
}

void Write(not_null<Store*> store) {
	store->writeTimer.cancel();
	auto result = QJsonObject();
	for (const auto &[peerId, snapshot] : store->snapshots) {
		result.insert(PeerKey(peerId), QJsonObject{
			{ u"name"_q, SerializeField(snapshot.name) },
			{ u"username"_q, SerializeField(snapshot.username) },
			{ u"about"_q, SerializeField(snapshot.about) },
			{ u"photo"_q, SerializeField(snapshot.photo) },
			{ u"photo_id"_q, QString::number(snapshot.photoId) },
		});
	}
	QDir().mkpath(store->root);
	auto file = QFile(store->root + u"profiles.json"_q);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(result).toJson(QJsonDocument::Compact));
	}
}

void Read(not_null<Store*> store) {
	auto file = QFile(store->root + u"profiles.json"_q);
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	const auto object = document.object();
	for (auto i = object.begin(); i != object.end(); ++i) {
		const auto peerId = PeerId(PeerIdHelper(i.key().toULongLong()));
		if (!peerId) {
			continue;
		}
		const auto data = i.value().toObject();
		store->snapshots.emplace(peerId, Snapshot{
			.name = ParseField(data.value(u"name"_q)),
			.username = ParseField(data.value(u"username"_q)),
			.about = ParseField(data.value(u"about"_q)),
			.photo = ParseField(data.value(u"photo"_q)),
			.photoId = data.value(u"photo_id"_q).toString().toULongLong(),
		});
	}
}

void ScheduleWrite(not_null<Store*> store) {
	if (!store->writeTimer.isActive()) {
		store->writeTimer.callOnce(kWriteTimeout);
	}
}

bool UpdateField(Field &field, const QString &value) {
	if (value.isEmpty()) {
		return false;
	}
	const auto now = base::unixtime::now();
	if (field.value == value && now - field.date < kRefreshDateAfter) {
		return false;
	}
	field.value = value;
	field.date = now;
	return true;
}

[[nodiscard]] bool Tracked(not_null<PeerData*> peer) {
	if (peer->isSelf() || peer->isServiceUser()) {
		return false;
	}
	const auto history = peer->owner().historyLoaded(peer);
	return history && history->inChatList();
}

void CheckPending(not_null<Store*> store) {
	auto &list = store->pending;
	for (auto i = begin(list); i != end(list);) {
		const auto peer = i->peer;
		const auto path = PhotoPath(store, peer->id);
		auto done = false;
		if (i->large && i->large->loaded()) {
			QDir().mkpath(QFileInfo(path).absolutePath());
			i->large->saveToFile(path);
			done = true;
		} else if (!i->large
			|| i->large->owner()->failed(Data::PhotoSize::Large)) {
			if (const auto image = peer->userpicCloudImage(i->view)) {
				QDir().mkpath(QFileInfo(path).absolutePath());
				image->save(path, "JPG");
				done = true;
			}
		}
		if (!done && crl::now() - i->added > kPendingTimeout) {
			i = list.erase(i);
		} else if (done) {
			auto &snapshot = store->snapshots[peer->id];
			snapshot.photoId = i->photoId;
			snapshot.photo = { .value = path, .date = base::unixtime::now() };
			ScheduleWrite(store);
			i = list.erase(i);
		} else {
			++i;
		}
	}
}

void CapturePhoto(
		not_null<Store*> store,
		not_null<PeerData*> peer,
		bool full) {
	const auto photoId = peer->userpicPhotoId();
	if (!photoId || !peer->hasUserpic()) {
		return;
	}
	auto &snapshot = store->snapshots[peer->id];
	const auto path = PhotoPath(store, peer->id);
	const auto upgrade = full
		&& (QImageReader(path).size().width() < kFullPhotoMinWidth);
	if (snapshot.photoId == photoId && QFile::exists(path) && !upgrade) {
		if (UpdateField(snapshot.photo, snapshot.photo.value)) {
			ScheduleWrite(store);
		}
		return;
	}
	for (const auto &pending : store->pending) {
		if (pending.peer == peer) {
			return;
		}
	}
	if (int(store->pending.size()) >= kMaxPendingPhotos) {
		return;
	}
	auto large = std::shared_ptr<Data::PhotoMedia>();
	const auto photo = full ? peer->owner().photo(photoId).get() : nullptr;
	if (photo
		&& !photo->isNull()
		&& photo->location(Data::PhotoSize::Large).valid()) {
		large = photo->createMediaView();
		photo->load(Data::PhotoSize::Large, peer->userpicPhotoOrigin());
	}
	store->pending.push_back({
		.peer = peer,
		.photoId = photoId,
		.view = peer->createUserpicView(),
		.large = std::move(large),
		.added = crl::now(),
	});
	peer->loadUserpic();
	CheckPending(store);
}

void Capture(
		not_null<Store*> store,
		not_null<PeerData*> peer,
		bool full = false) {
	if (!Enabled() || !Tracked(peer)) {
		return;
	}
	auto &snapshot = store->snapshots[peer->id];
	auto changed = false;
	const auto user = peer->asUser();
	if (!user || !user->isInaccessible()) {
		changed |= UpdateField(snapshot.name, peer->name());
	}
	changed |= UpdateField(snapshot.username, peer->username());
	changed |= UpdateField(snapshot.about, peer->about());
	if (changed) {
		ScheduleWrite(store);
	}
	CapturePhoto(store, peer, full);
}

Store::Store(not_null<Main::Session*> session)
: session(session)
, root(cWorkingDir()
	+ u"tdata/64gram_profiles/"_q
	+ QString::number(session->userId().bare)
	+ '/')
, writeTimer([=] { Write(this); }) {
	Read(this);
}

Store::~Store() {
	if (writeTimer.isActive()) {
		Write(this);
	}
}

[[nodiscard]] QString FormatDate(TimeId date) {
	return base::unixtime::parse(date).date().toString(u"dd.MM.yyyy"_q);
}

[[nodiscard]] QString SavedLabel(const QString &label, TimeId date) {
	return label
		+ u" · "_q
		+ tr::lng_profile_snapshot_saved(
			tr::now,
			lt_date,
			FormatDate(date));
}

[[nodiscard]] const Snapshot *Lookup(not_null<PeerData*> peer) {
	const auto store = Find(&peer->session());
	if (!store) {
		return nullptr;
	}
	const auto i = store->snapshots.find(peer->id);
	return (i != end(store->snapshots)) ? &i->second : nullptr;
}

void SavedPhotoBox(
		not_null<Ui::GenericBox*> box,
		QString path,
		QString label) {
	const auto image = QImage(path);
	box->setTitle(rpl::single(label));
	const auto width = st::boxWideWidth
		- st::boxRowPadding.left()
		- st::boxRowPadding.right();
	const auto scaled = image.isNull()
		? QImage()
		: image.scaled(
			width * style::DevicePixelRatio(),
			width * style::DevicePixelRatio(),
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	const auto size = scaled.isNull()
		? QSize(width, 0)
		: (scaled.size() / style::DevicePixelRatio());
	const auto widget = box->addRow(object_ptr<Ui::RpWidget>(box));
	widget->resize(size);
	widget->paintRequest() | rpl::on_next([=] {
		if (scaled.isNull()) {
			return;
		}
		auto p = QPainter(widget);
		auto copy = scaled;
		copy.setDevicePixelRatio(style::DevicePixelRatio());
		p.drawImage((widget->width() - size.width()) / 2, 0, copy);
	}, widget->lifetime());
	widget->widthValue() | rpl::on_next([=](int) {
		widget->resize(widget->width(), size.height());
	}, widget->lifetime());
	box->setWidth(st::boxWideWidth);
	box->addButton(tr::lng_profile_snapshot_show_file(), [=] {
		File::ShowInFolder(path);
	});
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace

bool Enabled() {
	return GetEnhancedBool(kOption);
}

void SetEnabled(bool enabled) {
	SetEnhancedValue(kOption, enabled);
	EnhancedSettings::Write();
}

void Start(not_null<Main::Session*> session) {
	auto &stores = Stores();
	if (stores.contains(session)) {
		return;
	}
	const auto store = stores.emplace(
		session,
		std::make_unique<Store>(session)).first->second.get();

	using Flag = Data::PeerUpdate::Flag;
	session->changes().peerUpdates(
		Flag::Name | Flag::Username | Flag::Usernames | Flag::About | Flag::Photo
	) | rpl::on_next([=](const Data::PeerUpdate &update) {
		Capture(store, update.peer);
	}, store->lifetime);

	session->downloaderTaskFinished(
	) | rpl::filter([=] {
		return !store->pending.empty();
	}) | rpl::on_next([=] {
		CheckPending(store);
	}, store->lifetime);

	session->data().sessionDataAboutToBeCleared(
	) | rpl::on_next([=] {
		store->pending.clear();
		if (store->writeTimer.isActive()) {
			Write(store);
		}
	}, store->lifetime);

	session->lifetime().add([=] {
		Stores().remove(session);
	});
}

void AddSavedInfoLines(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		const AddLine &addLine) {
	if (const auto store = Find(&peer->session())) {
		Capture(store, peer, true);
	}
	using Flag = Data::PeerUpdate::Flag;
	const auto changes = [=](Flag flags) {
		return peer->session().changes().peerFlagsValue(peer, flags);
	};
	const auto user = peer->asUser();
	if (user) {
		addLine(
			changes(Flag::Name) | rpl::map([=](const auto &) {
				const auto snapshot = Lookup(peer);
				return snapshot
					? SavedLabel(
						tr::lng_profile_snapshot_name(tr::now),
						snapshot->name.date)
					: QString();
			}),
			changes(Flag::Name) | rpl::map([=](const auto &) {
				const auto snapshot = Lookup(peer);
				return (snapshot
					&& user->isInaccessible()
					&& !snapshot->name.value.isEmpty())
					? TextWithEntities{ snapshot->name.value }
					: TextWithEntities();
			}));
	}
	addLine(
		changes(Flag::Username | Flag::Usernames) | rpl::map([=](const auto &) {
			const auto snapshot = Lookup(peer);
			return snapshot
				? SavedLabel(
					tr::lng_info_username_label(tr::now),
					snapshot->username.date)
				: QString();
		}),
		changes(Flag::Username | Flag::Usernames) | rpl::map([=](const auto &) {
			const auto snapshot = Lookup(peer);
			return (snapshot
				&& peer->username().isEmpty()
				&& !snapshot->username.value.isEmpty())
				? TextWithEntities{ QChar('@') + snapshot->username.value }
				: TextWithEntities();
		}));
	addLine(
		changes(Flag::About) | rpl::map([=](const auto &) {
			const auto snapshot = Lookup(peer);
			return snapshot
				? SavedLabel(
					(user && !user->isBot())
						? tr::lng_info_bio_label(tr::now)
						: tr::lng_info_about_label(tr::now),
					snapshot->about.date)
				: QString();
		}),
		changes(Flag::About) | rpl::map([=](const auto &) {
			const auto snapshot = Lookup(peer);
			return (snapshot
				&& peer->about().isEmpty()
				&& !snapshot->about.value.isEmpty())
				? TextWithEntities{ snapshot->about.value }
				: TextWithEntities();
		}));
	const auto photoLabel = [=] {
		const auto snapshot = Lookup(peer);
		return snapshot
			? SavedLabel(
				tr::lng_profile_snapshot_photo(tr::now),
				snapshot->photo.date)
			: QString();
	};
	const auto photo = addLine(
		changes(Flag::Photo) | rpl::map([=](const auto &) {
			return photoLabel();
		}),
		changes(Flag::Photo) | rpl::map([=](const auto &) {
			const auto snapshot = Lookup(peer);
			return (snapshot
				&& !peer->hasUserpic()
				&& !snapshot->photo.value.isEmpty()
				&& QFile::exists(snapshot->photo.value))
				? tr::link(tr::lng_profile_snapshot_open_photo(tr::now))
				: TextWithEntities();
		}));
	photo->setClickHandlerFilter([=](const auto &...) {
		if (const auto snapshot = Lookup(peer)) {
			controller->show(Box(
				SavedPhotoBox,
				snapshot->photo.value,
				photoLabel()));
		}
		return false;
	});
}

} // namespace ProfileSnapshots
