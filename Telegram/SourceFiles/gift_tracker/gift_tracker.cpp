/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#include "gift_tracker/gift_tracker.h"

#include "api/api_premium.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/unixtime.h"
#include "core/enhanced_settings.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_star_gift.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mtproto/mtproto_response.h"
#include "settings.h"
#include "storage/details/storage_file_utilities.h"
#include "storage/storage_account.h"
#include "window/window_session_controller.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace GiftTracker {
namespace {

constexpr auto kFormatVersion = quint32(1);
constexpr auto kPerPage = 100;
constexpr auto kMaxPages = 30;
constexpr auto kMaxEvents = 2000;
constexpr auto kCheckInterval = TimeId(4 * 3600);
constexpr auto kCheckJitter = TimeId(3600);
constexpr auto kManualInterval = TimeId(5 * 60);
constexpr auto kFirstTick = 60 * crl::time(1000);
constexpr auto kPeerGap = 90 * crl::time(1000);
constexpr auto kPageDelay = 3 * crl::time(1000);
constexpr auto kIdleTick = 10 * 60 * crl::time(1000);
constexpr auto kFloodExtra = TimeId(60);

const auto kDisabledKey = u"gift_tracker_off"_q;
const auto kSilentKey = u"gift_tracker_silent"_q;
const auto kFileName = u"data"_q;

base::flat_map<not_null<Main::Session*>, std::unique_ptr<Tracker>> Trackers;

[[nodiscard]] QString Folder(not_null<Main::Session*> session) {
	return session->local().basePath() + u"gift_tracker/"_q;
}

[[nodiscard]] QString IdToString(uint64 value) {
	return QString::number(value);
}

[[nodiscard]] uint64 IdFromString(const QJsonValue &value) {
	return value.toString().toULongLong();
}

[[nodiscard]] uint64 AccessHash(not_null<PeerData*> peer) {
	if (const auto user = peer->asUser()) {
		return user->accessHash();
	} else if (const auto channel = peer->asChannel()) {
		return channel->accessHash();
	}
	return 0;
}

[[nodiscard]] MTPInputPeer InputPeer(
		not_null<Main::Session*> session,
		not_null<TrackedPeer*> entry) {
	if (const auto peer = session->data().peerLoaded(entry->id)) {
		if (const auto hash = AccessHash(peer)) {
			entry->accessHash = hash;
			return peer->input();
		}
	}
	if (const auto userId = peerToUser(entry->id)) {
		return MTP_inputPeerUser(
			MTP_long(userId.bare),
			MTP_long(entry->accessHash));
	} else if (const auto channelId = peerToChannel(entry->id)) {
		return MTP_inputPeerChannel(
			MTP_long(channelId.bare),
			MTP_long(entry->accessHash));
	}
	return MTP_inputPeerEmpty();
}

[[nodiscard]] QJsonObject SerializeGift(const GiftRecord &gift) {
	auto result = QJsonObject();
	result.insert(u"key"_q, gift.key);
	result.insert(u"gift"_q, IdToString(gift.giftId));
	result.insert(u"title"_q, gift.title);
	result.insert(u"slug"_q, gift.slug);
	result.insert(u"stars"_q, IdToString(uint64(gift.stars)));
	result.insert(u"from"_q, IdToString(SerializePeerId(gift.fromId)));
	result.insert(u"fromName"_q, gift.fromName);
	result.insert(u"date"_q, gift.date);
	result.insert(u"number"_q, gift.number);
	result.insert(u"unique"_q, gift.unique);
	return result;
}

[[nodiscard]] GiftRecord DeserializeGift(const QJsonObject &object) {
	return {
		.key = object.value(u"key"_q).toString(),
		.giftId = IdFromString(object.value(u"gift"_q)),
		.title = object.value(u"title"_q).toString(),
		.slug = object.value(u"slug"_q).toString(),
		.stars = int64(IdFromString(object.value(u"stars"_q))),
		.fromId = DeserializePeerId(IdFromString(object.value(u"from"_q))),
		.fromName = object.value(u"fromName"_q).toString(),
		.date = TimeId(object.value(u"date"_q).toDouble()),
		.number = object.value(u"number"_q).toInt(),
		.unique = object.value(u"unique"_q).toBool(),
	};
}

[[nodiscard]] QJsonObject SerializePeer(const TrackedPeer &peer) {
	auto snapshot = QJsonArray();
	for (const auto &gift : peer.snapshot) {
		snapshot.append(SerializeGift(gift));
	}
	auto events = QJsonArray();
	for (const auto &event : peer.events) {
		auto object = QJsonObject();
		object.insert(u"when"_q, event.when);
		object.insert(u"type"_q, int(event.type));
		object.insert(u"count"_q, event.count);
		object.insert(u"gift"_q, SerializeGift(event.gift));
		events.append(object);
	}
	auto result = QJsonObject();
	result.insert(u"id"_q, IdToString(SerializePeerId(peer.id)));
	result.insert(u"hash"_q, IdToString(peer.accessHash));
	result.insert(u"name"_q, peer.name);
	result.insert(u"added"_q, peer.added);
	result.insert(u"lastCheck"_q, peer.lastCheck);
	result.insert(u"lastError"_q, peer.lastError);
	result.insert(u"hasSnapshot"_q, peer.hasSnapshot);
	result.insert(u"snapshot"_q, snapshot);
	result.insert(u"events"_q, events);
	return result;
}

[[nodiscard]] std::optional<TrackedPeer> DeserializePeer(
		const QJsonObject &object) {
	auto result = TrackedPeer{
		.id = DeserializePeerId(IdFromString(object.value(u"id"_q))),
		.accessHash = IdFromString(object.value(u"hash"_q)),
		.name = object.value(u"name"_q).toString(),
		.added = TimeId(object.value(u"added"_q).toDouble()),
		.lastCheck = TimeId(object.value(u"lastCheck"_q).toDouble()),
		.lastError = object.value(u"lastError"_q).toString(),
		.hasSnapshot = object.value(u"hasSnapshot"_q).toBool(),
	};
	if (!result.id) {
		return std::nullopt;
	}
	for (const auto value : object.value(u"snapshot"_q).toArray()) {
		result.snapshot.push_back(DeserializeGift(value.toObject()));
	}
	for (const auto value : object.value(u"events"_q).toArray()) {
		const auto event = value.toObject();
		const auto type = event.value(u"type"_q).toInt();
		if (type < int(EventType::Started) || type > int(EventType::Upgraded)) {
			continue;
		}
		result.events.push_back({
			.when = TimeId(event.value(u"when"_q).toDouble()),
			.type = EventType(type),
			.gift = DeserializeGift(event.value(u"gift"_q).toObject()),
			.count = event.value(u"count"_q).toInt(),
		});
	}
	return result;
}

[[nodiscard]] TimeId FloodSeconds(const QString &type) {
	const auto index = type.lastIndexOf('_');
	const auto seconds = type.mid(index + 1).toInt();
	return std::max(seconds, 1);
}

void AssignUniqueKeys(std::vector<GiftRecord> &gifts) {
	auto counts = base::flat_map<QString, int>();
	for (auto &gift : gifts) {
		const auto index = counts[gift.key]++;
		if (index) {
			gift.key += u"#"_q + QString::number(index);
		}
	}
}

} // namespace

bool Enabled() {
	return !GetEnhancedBool(kDisabledKey);
}

void SetEnabled(bool enabled) {
	SetEnhancedValue(kDisabledKey, !enabled);
	EnhancedSettings::Write();
}

bool NotifyEnabled() {
	return !GetEnhancedBool(kSilentKey);
}

void SetNotifyEnabled(bool enabled) {
	SetEnhancedValue(kSilentKey, !enabled);
	EnhancedSettings::Write();
}

QString GiftName(const GiftRecord &gift) {
	if (gift.unique) {
		return Data::UniqueGiftName(gift.title, gift.number);
	} else if (!gift.title.isEmpty()) {
		return gift.title;
	}
	return tr::lng_gift_tracker_gift(tr::now);
}

Tracker::Tracker(not_null<Main::Session*> session)
: _session(session)
, _timer([=] { tick(); }) {
	load();
	scheduleTick(kFirstTick);
}

Tracker::~Tracker() = default;

bool Tracker::tracked(PeerId peerId) const {
	return find(peerId) != nullptr;
}

const TrackedPeer *Tracker::find(PeerId peerId) const {
	const auto i = ranges::find(_peers, peerId, &TrackedPeer::id);
	return (i != end(_peers)) ? &*i : nullptr;
}

TrackedPeer *Tracker::entry(PeerId peerId) {
	const auto i = ranges::find(_peers, peerId, &TrackedPeer::id);
	return (i != end(_peers)) ? &*i : nullptr;
}

std::vector<PeerId> Tracker::trackedPeers() const {
	return _peers | ranges::views::transform(
		&TrackedPeer::id
	) | ranges::to_vector;
}

rpl::producer<PeerId> Tracker::updates() const {
	return _updates.events();
}

void Tracker::track(not_null<PeerData*> peer) {
	if (tracked(peer->id)) {
		return;
	}
	_peers.push_back({
		.id = peer->id,
		.accessHash = AccessHash(peer),
		.name = peer->name(),
		.added = base::unixtime::now(),
	});
	save();
	_updates.fire_copy(peer->id);
	_forced.emplace(peer->id);
	if (!_fetch) {
		scheduleTick(crl::time(2000));
	}
}

void Tracker::untrack(PeerId peerId) {
	const auto i = ranges::find(_peers, peerId, &TrackedPeer::id);
	if (i == end(_peers)) {
		return;
	}
	_peers.erase(i);
	_forced.remove(peerId);
	if (_fetch && _fetch->peerId == peerId) {
		_session->api().request(base::take(_requestId)).cancel();
		_fetch = std::nullopt;
		scheduleTick(kPeerGap);
	}
	save();
	_updates.fire_copy(peerId);
}

bool Tracker::checkNow(PeerId peerId) {
	const auto found = entry(peerId);
	if (!found || (_fetch && _fetch->peerId == peerId)) {
		return false;
	}
	const auto now = base::unixtime::now();
	if (found->lastCheck && now - found->lastCheck < kManualInterval) {
		return false;
	}
	_forced.emplace(peerId);
	if (!_fetch) {
		scheduleTick(crl::time(1000));
	}
	return true;
}

void Tracker::scheduleTick(crl::time delay) {
	_timer.callOnce(std::max(delay, crl::time(1000)));
}

TimeId Tracker::interval(PeerId peerId) const {
	return kCheckInterval + TimeId(SerializePeerId(peerId) % kCheckJitter);
}

TrackedPeer *Tracker::nextDue(TimeId now, TimeId &nearest) {
	while (!_forced.empty()) {
		const auto peerId = *_forced.begin();
		_forced.remove(peerId);
		if (const auto found = entry(peerId)) {
			return found;
		}
	}
	TrackedPeer *result = nullptr;
	auto best = TimeId();
	for (auto &peer : _peers) {
		const auto due = peer.lastCheck
			? (peer.lastCheck + interval(peer.id))
			: TimeId(0);
		if (!result || due < best) {
			result = &peer;
			best = due;
		}
	}
	if (result && best <= now) {
		return result;
	}
	nearest = best;
	return nullptr;
}

void Tracker::tick() {
	if (_fetch) {
		return;
	} else if (!Enabled() || _peers.empty()) {
		scheduleTick(kIdleTick);
		return;
	}
	const auto now = crl::now();
	if (now < _floodUntil) {
		scheduleTick(_floodUntil - now);
		return;
	}
	const auto unixNow = base::unixtime::now();
	auto nearest = TimeId();
	if (const auto due = nextDue(unixNow, nearest)) {
		start(due);
	} else {
		scheduleTick(std::max(
			kPeerGap,
			crl::time(nearest - unixNow) * crl::time(1000)));
	}
}

void Tracker::start(not_null<TrackedPeer*> entry) {
	static auto generation = 0;
	_fetch = Fetch{ .peerId = entry->id, .generation = ++generation };
	requestPage();
}

void Tracker::requestPage() {
	if (!_fetch) {
		return;
	}
	const auto found = entry(_fetch->peerId);
	if (!found) {
		_fetch = std::nullopt;
		scheduleTick(kPeerGap);
		return;
	}
	const auto peerId = _fetch->peerId;
	const auto generation = _fetch->generation;
	_requestId = _session->api().request(MTPpayments_GetSavedStarGifts(
		MTP_flags(0),
		InputPeer(_session, found),
		MTPint(), // collection_id
		MTP_string(_fetch->offset),
		MTP_int(kPerPage)
	)).done(crl::guard(this, [=](const MTPpayments_SavedStarGifts &result) {
		_requestId = 0;
		if (!_fetch || _fetch->generation != generation) {
			return;
		}
		const auto &data = result.data();
		const auto owner = &_session->data();
		owner->processUsers(data.vusers());
		owner->processChats(data.vchats());
		const auto peer = owner->peer(peerId);
		for (const auto &gift : data.vgifts().v) {
			if (const auto parsed = Api::FromTL(peer, gift)) {
				_fetch->gifts.push_back(parse(*parsed));
			}
		}
		if (const auto next = data.vnext_offset()) {
			_fetch->offset = qs(*next);
		} else {
			_fetch->offset = QString();
		}
		++_fetch->pages;
		if (_fetch->offset.isEmpty()) {
			finishFetch();
		} else if (_fetch->pages >= kMaxPages) {
			_fetch->truncated = true;
			finishFetch();
		} else {
			base::call_delayed(kPageDelay, this, [=] {
				if (_fetch && _fetch->generation == generation) {
					requestPage();
				}
			});
		}
	})).fail(crl::guard(this, [=](const MTP::Error &error) {
		_requestId = 0;
		if (_fetch && _fetch->generation == generation) {
			failFetch(error);
		}
	})).handleFloodErrors().send();
}

void Tracker::finishFetch() {
	auto fetch = base::take(_fetch);
	if (const auto found = fetch ? entry(fetch->peerId) : nullptr) {
		apply(found, std::move(*fetch));
	}
	scheduleTick(kPeerGap);
}

void Tracker::failFetch(const MTP::Error &error) {
	const auto fetch = base::take(_fetch);
	const auto found = fetch ? entry(fetch->peerId) : nullptr;
	const auto type = error.type();
	if (MTP::IsFloodError(type)) {
		const auto wait = FloodSeconds(type) + kFloodExtra;
		_floodUntil = crl::now() + crl::time(wait) * crl::time(1000);
		if (found) {
			_forced.emplace(found->id);
		}
		scheduleTick(crl::time(wait) * crl::time(1000));
		return;
	}
	if (found) {
		found->lastError = type;
		found->lastCheck = base::unixtime::now();
		save();
		_updates.fire_copy(found->id);
	}
	scheduleTick(kPeerGap);
}

GiftRecord Tracker::parse(const Data::SavedStarGift &gift) const {
	auto result = GiftRecord{
		.giftId = gift.info.id,
		.stars = gift.info.stars,
		.fromId = gift.fromId,
		.date = gift.date,
	};
	if (result.fromId) {
		if (const auto from = _session->data().peerLoaded(result.fromId)) {
			result.fromName = from->name();
		}
	}
	if (const auto unique = gift.info.unique.get()) {
		result.unique = true;
		result.giftId = unique->initialGiftId
			? unique->initialGiftId
			: gift.info.id;
		result.title = unique->title;
		result.slug = unique->slug;
		result.number = unique->number;
		result.key = u"u:"_q + QString::number(unique->id);
	} else {
		result.title = gift.info.resellTitle;
		result.key = u"g:%1:%2:%3"_q
			.arg(gift.info.id)
			.arg(SerializePeerId(gift.fromId))
			.arg(gift.date);
	}
	return result;
}

void Tracker::apply(not_null<TrackedPeer*> entry, Fetch &&fetch) {
	const auto now = base::unixtime::now();
	auto gifts = std::move(fetch.gifts);
	AssignUniqueKeys(gifts);

	if (const auto peer = _session->data().peerLoaded(entry->id)) {
		if (!peer->name().isEmpty()) {
			entry->name = peer->name();
		}
		if (const auto hash = AccessHash(peer)) {
			entry->accessHash = hash;
		}
	}
	entry->lastCheck = now;
	entry->lastError = QString();

	if (!entry->hasSnapshot) {
		entry->events.push_back({
			.when = now,
			.type = EventType::Started,
			.count = int(gifts.size()),
		});
		entry->snapshot = std::move(gifts);
		entry->hasSnapshot = true;
		save();
		_updates.fire_copy(entry->id);
		return;
	}

	auto oldKeys = base::flat_set<QString>();
	for (const auto &gift : entry->snapshot) {
		oldKeys.emplace(gift.key);
	}
	auto newKeys = base::flat_set<QString>();
	for (const auto &gift : gifts) {
		newKeys.emplace(gift.key);
	}
	auto added = std::vector<GiftRecord>();
	for (const auto &gift : gifts) {
		if (!oldKeys.contains(gift.key)) {
			added.push_back(gift);
		}
	}
	auto removed = std::vector<GiftRecord>();
	if (!fetch.truncated) {
		for (const auto &gift : entry->snapshot) {
			if (!newKeys.contains(gift.key)) {
				removed.push_back(gift);
			}
		}
	}
	auto upgraded = std::vector<GiftRecord>();
	for (auto i = begin(added); i != end(added);) {
		const auto j = i->unique
			? ranges::find_if(removed, [&](const GiftRecord &gift) {
				return !gift.unique
					&& (gift.giftId == i->giftId)
					&& (gift.fromId == i->fromId);
			})
			: end(removed);
		if (j != end(removed)) {
			upgraded.push_back(*i);
			removed.erase(j);
			i = added.erase(i);
		} else {
			++i;
		}
	}
	for (const auto &gift : removed) {
		entry->events.push_back({ now, EventType::Removed, gift });
	}
	for (const auto &gift : upgraded) {
		entry->events.push_back({ now, EventType::Upgraded, gift });
	}
	for (const auto &gift : added) {
		entry->events.push_back({ now, EventType::Added, gift });
	}
	if (entry->events.size() > kMaxEvents) {
		entry->events.erase(
			begin(entry->events),
			end(entry->events) - kMaxEvents);
	}

	if (fetch.truncated) {
		for (const auto &gift : entry->snapshot) {
			if (!newKeys.contains(gift.key)) {
				gifts.push_back(gift);
			}
		}
	}
	entry->snapshot = std::move(gifts);
	save();
	_updates.fire_copy(entry->id);

	const auto changed = int(added.size() + upgraded.size());
	if ((changed || !removed.empty()) && NotifyEnabled()) {
		notify(entry, changed, int(removed.size()));
	}
}

void Tracker::notify(not_null<TrackedPeer*> entry, int added, int removed) {
	const auto window = _session->tryResolveWindow();
	if (!window) {
		return;
	}
	window->showToast(tr::lng_gift_tracker_changed_toast(
		tr::now,
		lt_name,
		entry->name,
		lt_added,
		QString::number(added),
		lt_removed,
		QString::number(removed)));
}

void Tracker::load() {
	const auto key = _session->local().peekLegacyLocalKey();
	if (!key) {
		return;
	}
	using namespace Storage::details;
	auto file = FileReadDescriptor();
	if (!ReadEncryptedFile(file, kFileName, Folder(_session), key)) {
		return;
	}
	auto version = quint32();
	auto owner = quint64();
	auto bytes = QByteArray();
	file.stream >> version >> owner >> bytes;
	if (!CheckStreamStatus(file.stream)
		|| version != kFormatVersion
		|| owner != _session->uniqueId()) {
		return;
	}
	const auto document = QJsonDocument::fromJson(bytes);
	for (const auto value : document.object().value(u"peers"_q).toArray()) {
		if (auto peer = DeserializePeer(value.toObject())) {
			if (!tracked(peer->id)) {
				_peers.push_back(std::move(*peer));
			}
		}
	}
}

void Tracker::save() {
	const auto key = _session->local().peekLegacyLocalKey();
	if (!key) {
		return;
	}
	auto peers = QJsonArray();
	for (const auto &peer : _peers) {
		peers.append(SerializePeer(peer));
	}
	auto object = QJsonObject();
	object.insert(u"peers"_q, peers);
	const auto bytes = QJsonDocument(object).toJson(
		QJsonDocument::Compact);

	using namespace Storage::details;
	auto data = EncryptedDescriptor(
		uint32(sizeof(quint32) * 3 + sizeof(quint64) + bytes.size()));
	data.stream
		<< quint32(kFormatVersion)
		<< quint64(_session->uniqueId())
		<< bytes;
	auto file = FileWriteDescriptor(kFileName, Folder(_session));
	file.writeEncrypted(data, key);
}

void Start(not_null<Main::Session*> session) {
	if (Trackers.contains(session)) {
		return;
	}
	Trackers.emplace(session, std::make_unique<Tracker>(session));
	session->lifetime().add([=] {
		Trackers.remove(session);
	});
}

Tracker *Find(not_null<Main::Session*> session) {
	const auto i = Trackers.find(session);
	return (i != end(Trackers)) ? i->second.get() : nullptr;
}

} // namespace GiftTracker
