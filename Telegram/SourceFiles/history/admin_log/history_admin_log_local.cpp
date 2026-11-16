/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#include "history/admin_log/history_admin_log_local.h"

#include "history/admin_log/history_admin_log_filter_value.h"
#include "base/unixtime.h"
#include "core/enhanced_settings.h"
#include "data/data_channel.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_session.h"
#include "storage/details/storage_file_utilities.h"
#include "storage/storage_account.h"

namespace AdminLog {
namespace {

constexpr auto kFileVersion = quint32(1);
constexpr auto kPerPage = 100;
constexpr auto kMaxPagesPerSync = 200;
constexpr auto kServerKeepsSeconds = TimeId(48 * 60 * 60);
constexpr auto kSyncInterval = crl::time(2 * 60 * 60 * 1000);
constexpr auto kFirstSyncDelay = crl::time(60 * 1000);
constexpr auto kBetweenChannelsDelay = crl::time(1500);
constexpr auto kMinSyncNowInterval = TimeId(60);
constexpr auto kDefaultEventsLimit = 20000;
constexpr auto kMaxGaps = 1000;

const auto kMasterKey = u"admin_log_local_history"_q;
const auto kLimitKey = u"admin_log_local_limit"_q;
const auto kIndexName = u"alog_index"_q;

using Registry = base::flat_map<
	not_null<Main::Session*>,
	std::unique_ptr<LocalHistory>>;

[[nodiscard]] Registry &Instances() {
	static auto result = Registry();
	return result;
}

template <typename T>
[[nodiscard]] QByteArray SerializeTL(const T &value) {
	auto buffer = mtpBuffer();
	value.write(buffer);
	return QByteArray(
		reinterpret_cast<const char*>(buffer.constData()),
		buffer.size() * sizeof(mtpPrime));
}

[[nodiscard]] uint64 UserKey(const MTPUser &user) {
	return user.match([](const auto &data) {
		return peerFromUser(UserId(data.vid().v)).value;
	});
}

[[nodiscard]] uint64 ChatKey(const MTPChat &chat) {
	return chat.match([](const MTPDchannel &data) {
		return peerFromChannel(ChannelId(data.vid().v)).value;
	}, [](const MTPDchannelForbidden &data) {
		return peerFromChannel(ChannelId(data.vid().v)).value;
	}, [](const auto &data) {
		return peerFromChat(ChatId(data.vid().v)).value;
	});
}

[[nodiscard]] FilterValue::Flags EventFlags(
		const MTPChannelAdminLogEventAction &action) {
	using Flag = FilterValue::Flag;
	switch (action.type()) {
	case mtpc_channelAdminLogEventActionParticipantJoin:
	case mtpc_channelAdminLogEventActionParticipantJoinByInvite:
	case mtpc_channelAdminLogEventActionParticipantJoinByRequest:
		return Flag::Join;
	case mtpc_channelAdminLogEventActionParticipantLeave:
		return Flag::Leave;
	case mtpc_channelAdminLogEventActionParticipantInvite:
		return Flag::Invite;
	case mtpc_channelAdminLogEventActionParticipantToggleBan:
		return Flag::Ban | Flag::Unban | Flag::Kick | Flag::Unkick;
	case mtpc_channelAdminLogEventActionParticipantToggleAdmin:
		return Flag::Promote | Flag::Demote;
	case mtpc_channelAdminLogEventActionChangeTitle:
	case mtpc_channelAdminLogEventActionChangeAbout:
	case mtpc_channelAdminLogEventActionChangeUsername:
	case mtpc_channelAdminLogEventActionChangeUsernames:
	case mtpc_channelAdminLogEventActionChangePhoto:
	case mtpc_channelAdminLogEventActionChangeStickerSet:
	case mtpc_channelAdminLogEventActionChangeEmojiStickerSet:
	case mtpc_channelAdminLogEventActionChangeLinkedChat:
	case mtpc_channelAdminLogEventActionChangeLocation:
	case mtpc_channelAdminLogEventActionChangePeerColor:
	case mtpc_channelAdminLogEventActionChangeProfilePeerColor:
	case mtpc_channelAdminLogEventActionChangeWallpaper:
	case mtpc_channelAdminLogEventActionChangeEmojiStatus:
		return Flag::Info;
	case mtpc_channelAdminLogEventActionToggleInvites:
	case mtpc_channelAdminLogEventActionToggleSignatures:
	case mtpc_channelAdminLogEventActionToggleSignatureProfiles:
	case mtpc_channelAdminLogEventActionTogglePreHistoryHidden:
	case mtpc_channelAdminLogEventActionDefaultBannedRights:
	case mtpc_channelAdminLogEventActionToggleSlowMode:
	case mtpc_channelAdminLogEventActionChangeHistoryTTL:
	case mtpc_channelAdminLogEventActionToggleNoForwards:
	case mtpc_channelAdminLogEventActionChangeAvailableReactions:
	case mtpc_channelAdminLogEventActionToggleAntiSpam:
	case mtpc_channelAdminLogEventActionToggleAutotranslation:
		return Flag::Settings;
	case mtpc_channelAdminLogEventActionUpdatePinned:
		return Flag::Pinned;
	case mtpc_channelAdminLogEventActionEditMessage:
	case mtpc_channelAdminLogEventActionStopPoll:
		return Flag::Edit;
	case mtpc_channelAdminLogEventActionDeleteMessage:
		return Flag::Delete;
	case mtpc_channelAdminLogEventActionStartGroupCall:
	case mtpc_channelAdminLogEventActionDiscardGroupCall:
	case mtpc_channelAdminLogEventActionParticipantMute:
	case mtpc_channelAdminLogEventActionParticipantUnmute:
	case mtpc_channelAdminLogEventActionParticipantVolume:
	case mtpc_channelAdminLogEventActionToggleGroupCallSetting:
		return Flag::GroupCall;
	case mtpc_channelAdminLogEventActionExportedInviteDelete:
	case mtpc_channelAdminLogEventActionExportedInviteRevoke:
	case mtpc_channelAdminLogEventActionExportedInviteEdit:
		return Flag::Invites;
	case mtpc_channelAdminLogEventActionToggleForum:
	case mtpc_channelAdminLogEventActionCreateTopic:
	case mtpc_channelAdminLogEventActionEditTopic:
	case mtpc_channelAdminLogEventActionDeleteTopic:
	case mtpc_channelAdminLogEventActionPinTopic:
		return Flag::Topics;
	case mtpc_channelAdminLogEventActionParticipantSubExtend:
		return Flag::SubExtend;
	case mtpc_channelAdminLogEventActionParticipantEditRank:
		return Flag::EditRank;
	}
	return FilterValue::Flags();
}

[[nodiscard]] QString MessageText(const MTPMessage &message) {
	return message.match([](const MTPDmessage &data) {
		return qs(data.vmessage());
	}, [](const auto &) {
		return QString();
	});
}

[[nodiscard]] QString SearchText(const MTPChannelAdminLogEventAction &action) {
	const auto pair = [](const MTPstring &a, const MTPstring &b) {
		return qs(a) + QChar('\n') + qs(b);
	};
	return action.match([&](
			const MTPDchannelAdminLogEventActionChangeTitle &data) {
		return pair(data.vprev_value(), data.vnew_value());
	}, [&](const MTPDchannelAdminLogEventActionChangeAbout &data) {
		return pair(data.vprev_value(), data.vnew_value());
	}, [&](const MTPDchannelAdminLogEventActionChangeUsername &data) {
		return pair(data.vprev_value(), data.vnew_value());
	}, [](const MTPDchannelAdminLogEventActionUpdatePinned &data) {
		return MessageText(data.vmessage());
	}, [](const MTPDchannelAdminLogEventActionEditMessage &data) {
		return MessageText(data.vprev_message())
			+ QChar('\n')
			+ MessageText(data.vnew_message());
	}, [](const MTPDchannelAdminLogEventActionDeleteMessage &data) {
		return MessageText(data.vmessage());
	}, [](const MTPDchannelAdminLogEventActionSendMessage &data) {
		return MessageText(data.vmessage());
	}, [](const MTPDchannelAdminLogEventActionStopPoll &data) {
		return MessageText(data.vmessage());
	}, [](const auto &) {
		return QString();
	});
}

} // namespace

struct LocalHistory::Channel {
	ChannelId id = 0;
	bool enabled = false;
	bool loaded = false;
	std::map<uint64, MTPChannelAdminLogEvent> events;
	std::map<uint64, MTPUser> users;
	std::map<uint64, MTPChat> chats;
	uint64 syncedId = 0;
	TimeId since = 0;
	TimeId lastSync = 0;
	std::vector<std::pair<TimeId, TimeId>> gaps;
};

LocalHistory::LocalHistory(not_null<Main::Session*> session)
: _session(session)
, _api(&session->mtp())
, _timer([=] { syncAll(); })
, _nextTimer([=] { syncNext(); }) {
	readIndex();
	scheduleSync(kFirstSyncDelay);
}

LocalHistory::~LocalHistory() = default;

LocalHistory *LocalHistory::Find(not_null<Main::Session*> session) {
	const auto &instances = Instances();
	const auto i = instances.find(session);
	return (i != end(instances)) ? i->second.get() : nullptr;
}

bool LocalHistory::MasterEnabled() {
	return GetEnhancedBool(kMasterKey);
}

void LocalHistory::SetMasterEnabled(bool enabled) {
	SetEnhancedValue(kMasterKey, enabled);
	EnhancedSettings::Write();
	if (enabled) {
		for (const auto &[session, instance] : Instances()) {
			instance->scheduleSync(kFirstSyncDelay);
		}
	}
}

int LocalHistory::EventsLimitSetting() {
	const auto value = GetEnhancedInt(kLimitKey);
	return value ? value : kDefaultEventsLimit;
}

int LocalHistory::EventsLimit() {
	const auto value = EventsLimitSetting();
	return (value > 0) ? value : 0;
}

void LocalHistory::SetEventsLimitSetting(int value) {
	SetEnhancedValue(kLimitKey, value);
	EnhancedSettings::Write();
	for (const auto &[session, instance] : Instances()) {
		for (auto &[id, data] : instance->_channels) {
			if (data->loaded) {
				const auto was = data->events.size();
				instance->prune(*data);
				if (data->events.size() != was) {
					instance->writeChannel(*data);
					instance->notify(id);
				}
			}
		}
	}
}

std::vector<int> LocalHistory::EventsLimitOptions() {
	return { 1000, 5000, kDefaultEventsLimit, 100000, -1 };
}

QString LocalHistory::folder() const {
	auto result = _session->local().basePath();
	if (!result.endsWith('/')) {
		result += '/';
	}
	return result + u"admin_log/"_q;
}

QString LocalHistory::ChannelFileName(ChannelId id) {
	return u"alog_"_q + QString::number(id.bare, 16);
}

LocalHistory::Channel &LocalHistory::channel(ChannelId id) {
	auto i = _channels.find(id);
	if (i == end(_channels)) {
		auto data = std::make_unique<Channel>();
		data->id = id;
		i = _channels.emplace(id, std::move(data)).first;
	}
	return *i->second;
}

LocalHistory::Channel *LocalHistory::loadedChannel(ChannelId id) {
	const auto i = _channels.find(id);
	if (i == end(_channels)) {
		return nullptr;
	}
	ensureLoaded(*i->second);
	return i->second.get();
}

void LocalHistory::ensureLoaded(Channel &data) {
	if (!data.loaded) {
		data.loaded = true;
		readChannel(data);
	}
}

void LocalHistory::readIndex() {
	const auto key = _session->local().peekLegacyLocalKey();
	if (!key) {
		return;
	}
	using namespace Storage::details;
	auto file = FileReadDescriptor();
	if (!ReadEncryptedFile(file, kIndexName, folder(), key)) {
		return;
	}
	auto version = quint32();
	auto count = qint32();
	file.stream >> version >> count;
	if (version != kFileVersion || count < 0 || count > 100000) {
		return;
	}
	for (auto i = 0; i != count; ++i) {
		auto id = quint64();
		auto enabled = qint32();
		file.stream >> id >> enabled;
		if (!CheckStreamStatus(file.stream)) {
			return;
		}
		channel(ChannelId(id)).enabled = (enabled != 0);
	}
}

void LocalHistory::writeIndex() {
	const auto key = _session->local().peekLegacyLocalKey();
	if (!key) {
		return;
	}
	using namespace Storage::details;
	auto data = EncryptedDescriptor(uint32(
		sizeof(quint32) * 2
		+ _channels.size() * (sizeof(quint64) + sizeof(qint32))));
	data.stream << kFileVersion << qint32(_channels.size());
	for (const auto &[id, channel] : _channels) {
		data.stream << quint64(id.bare) << qint32(channel->enabled ? 1 : 0);
	}
	auto file = FileWriteDescriptor(kIndexName, folder());
	file.writeEncrypted(data, key);
}

void LocalHistory::readChannel(Channel &data) {
	const auto key = _session->local().peekLegacyLocalKey();
	if (!key) {
		return;
	}
	using namespace Storage::details;
	auto file = FileReadDescriptor();
	if (!ReadEncryptedFile(file, ChannelFileName(data.id), folder(), key)) {
		return;
	}
	auto version = quint32();
	auto channelId = quint64();
	auto syncedId = quint64();
	auto since = qint32();
	auto lastSync = qint32();
	auto gapsCount = qint32();
	file.stream
		>> version
		>> channelId
		>> syncedId
		>> since
		>> lastSync
		>> gapsCount;
	if (version != kFileVersion
		|| ChannelId(channelId) != data.id
		|| gapsCount < 0
		|| gapsCount > kMaxGaps) {
		return;
	}
	auto gaps = std::vector<std::pair<TimeId, TimeId>>();
	gaps.reserve(gapsCount);
	for (auto i = 0; i != gapsCount; ++i) {
		auto from = qint32();
		auto till = qint32();
		file.stream >> from >> till;
		gaps.emplace_back(from, till);
	}
	auto serialized = QByteArray();
	file.stream >> serialized;
	if (!CheckStreamStatus(file.stream)) {
		return;
	}
	auto parsed = MTPchannels_AdminLogResults();
	auto from = reinterpret_cast<const mtpPrime*>(serialized.constData());
	const auto till = from + (serialized.size() / sizeof(mtpPrime));
	if (!parsed.read(from, till)) {
		LOG(("Admin Log Local: could not parse %1.").arg(data.id.bare));
		return;
	}
	const auto &results = parsed.c_channels_adminLogResults();
	for (const auto &event : results.vevents().v) {
		data.events.emplace(event.data().vid().v, event);
	}
	for (const auto &user : results.vusers().v) {
		data.users.insert_or_assign(UserKey(user), user);
	}
	for (const auto &chat : results.vchats().v) {
		data.chats.insert_or_assign(ChatKey(chat), chat);
	}
	data.syncedId = syncedId;
	data.since = since;
	data.lastSync = lastSync;
	data.gaps = std::move(gaps);
}

void LocalHistory::writeChannel(const Channel &data) {
	const auto key = _session->local().peekLegacyLocalKey();
	if (!key) {
		return;
	}
	auto events = QVector<MTPChannelAdminLogEvent>();
	events.reserve(data.events.size());
	for (const auto &[id, event] : data.events) {
		events.push_back(event);
	}
	auto users = QVector<MTPUser>();
	users.reserve(data.users.size());
	for (const auto &[id, user] : data.users) {
		users.push_back(user);
	}
	auto chats = QVector<MTPChat>();
	chats.reserve(data.chats.size());
	for (const auto &[id, chat] : data.chats) {
		chats.push_back(chat);
	}
	const auto serialized = SerializeTL(MTP_channels_adminLogResults(
		MTP_vector<MTPChannelAdminLogEvent>(std::move(events)),
		MTP_vector<MTPChat>(std::move(chats)),
		MTP_vector<MTPUser>(std::move(users))));

	using namespace Storage::details;
	auto encrypted = EncryptedDescriptor(uint32(
		sizeof(quint32)
		+ sizeof(quint64) * 2
		+ sizeof(qint32) * 3
		+ data.gaps.size() * sizeof(qint32) * 2
		+ sizeof(quint32)
		+ serialized.size()));
	encrypted.stream
		<< kFileVersion
		<< quint64(data.id.bare)
		<< quint64(data.syncedId)
		<< qint32(data.since)
		<< qint32(data.lastSync)
		<< qint32(data.gaps.size());
	for (const auto &[from, till] : data.gaps) {
		encrypted.stream << qint32(from) << qint32(till);
	}
	encrypted.stream << serialized;
	auto file = FileWriteDescriptor(ChannelFileName(data.id), folder());
	file.writeEncrypted(encrypted, key);
}

bool LocalHistory::enabled(not_null<ChannelData*> channel) const {
	const auto i = _channels.find(peerToChannel(channel->id));
	return (i != end(_channels)) && i->second->enabled;
}

void LocalHistory::setEnabled(not_null<ChannelData*> channel, bool enabled) {
	const auto id = peerToChannel(channel->id);
	auto &data = this->channel(id);
	if (data.enabled == enabled) {
		return;
	}
	data.enabled = enabled;
	writeIndex();
	notify(id);
	if (enabled) {
		syncNow(channel);
	}
}

LocalHistoryState LocalHistory::state(not_null<ChannelData*> channel) {
	const auto data = loadedChannel(peerToChannel(channel->id));
	if (!data) {
		return {};
	}
	return {
		.enabled = data->enabled,
		.count = int(data->events.size()),
		.since = data->since,
		.lastSync = data->lastSync,
		.gaps = data->gaps,
	};
}

rpl::producer<LocalHistoryState> LocalHistory::stateValue(
		not_null<ChannelData*> channel) {
	const auto id = peerToChannel(channel->id);
	return rpl::single(rpl::empty) | rpl::then(
		_changes.events(
		) | rpl::filter([=](ChannelId changed) {
			return (changed == id);
		}) | rpl::to_empty
	) | rpl::map([=] {
		return state(channel);
	});
}

bool LocalHistory::hasEvents(not_null<ChannelData*> channel) {
	const auto data = loadedChannel(peerToChannel(channel->id));
	return data && !data->events.empty();
}

rpl::producer<> LocalHistory::clearedEvents() const {
	return _cleared.events();
}

void LocalHistory::notify(ChannelId id) {
	_changes.fire_copy(id);
}

void LocalHistory::registerPeers(Channel &data) {
	const auto owner = &_session->data();
	for (const auto &[key, user] : data.users) {
		if (!owner->peerLoaded(PeerId(key))) {
			owner->processUser(user);
		}
	}
	for (const auto &[key, chat] : data.chats) {
		if (!owner->peerLoaded(PeerId(key))) {
			owner->processChat(chat);
		}
	}
}

QVector<MTPChannelAdminLogEvent> LocalHistory::query(
		not_null<ChannelData*> channel,
		uint64 maxId,
		const FilterValue &filter,
		const QString &search,
		int limit) {
	auto result = QVector<MTPChannelAdminLogEvent>();
	const auto data = loadedChannel(peerToChannel(channel->id));
	if (!data || data->events.empty() || limit <= 0) {
		return result;
	}
	registerPeers(*data);

	const auto query = search.trimmed();
	const auto matches = [&](const MTPDchannelAdminLogEvent &event) {
		const auto userId = UserId(event.vuser_id().v);
		if (filter.admins) {
			const auto &admins = *filter.admins;
			const auto peerId = peerFromUser(userId);
			if (!ranges::contains(admins, peerId, &UserData::id)) {
				return false;
			}
		}
		if (filter.flags) {
			if (!(*filter.flags & EventFlags(event.vaction()))) {
				return false;
			}
		}
		if (!query.isEmpty()) {
			auto text = SearchText(event.vaction());
			if (const auto user = _session->data().userLoaded(userId)) {
				text += QChar('\n') + user->name();
			}
			if (!text.contains(query, Qt::CaseInsensitive)) {
				return false;
			}
		}
		return true;
	};
	auto i = maxId ? data->events.lower_bound(maxId) : data->events.end();
	while (i != data->events.begin() && result.size() < limit) {
		--i;
		if (matches(i->second.data())) {
			result.push_back(i->second);
		}
	}
	return result;
}

void LocalHistory::scheduleSync(crl::time delay) {
	_timer.callOnce(delay);
}

void LocalHistory::syncAll() {
	scheduleSync(kSyncInterval);
	if (!MasterEnabled()) {
		return;
	}
	for (const auto &[id, data] : _channels) {
		if (data->enabled
			&& _syncing != id
			&& !ranges::contains(_queue, id)) {
			_queue.push_back(id);
		}
	}
	syncNext();
}

void LocalHistory::syncNow(not_null<ChannelData*> channel) {
	const auto id = peerToChannel(channel->id);
	if (!MasterEnabled() || !enabled(channel)) {
		return;
	} else if (_syncing == id || ranges::contains(_queue, id)) {
		return;
	}
	const auto data = loadedChannel(id);
	const auto now = base::unixtime::now();
	if (data && data->lastSync && (now - data->lastSync) < kMinSyncNowInterval) {
		return;
	}
	_queue.push_front(id);
	syncNext();
}

void LocalHistory::syncNext() {
	if (_syncing || _requestId) {
		return;
	}
	const auto now = base::unixtime::now();
	if (_floodUntil > now) {
		_nextTimer.callOnce((_floodUntil - now) * crl::time(1000) + 1000);
		return;
	}
	while (!_queue.empty()) {
		const auto id = _queue.front();
		_queue.pop_front();
		const auto i = _channels.find(id);
		if (i == end(_channels) || !i->second->enabled) {
			continue;
		}
		const auto channel = _session->data().channelLoaded(id);
		if (!channel
			|| !channel->amIn()
			|| !(channel->hasAdminRights() || channel->amCreator())) {
			continue;
		}
		ensureLoaded(*i->second);
		_syncing = id;
		_pendingEvents.clear();
		_pendingUsers.clear();
		_pendingChats.clear();
		requestPage(channel, 0, kMaxPagesPerSync);
		return;
	}
}

void LocalHistory::requestPage(
		not_null<ChannelData*> channel,
		uint64 maxId,
		int pagesLeft) {
	const auto id = peerToChannel(channel->id);
	const auto minId = this->channel(id).syncedId;
	_requestId = _api.request(MTPchannels_GetAdminLog(
		MTP_flags(0),
		channel->inputChannel(),
		MTP_string(QString()),
		MTP_channelAdminLogEventsFilter(MTP_flags(0)),
		MTP_vector<MTPInputUser>(),
		MTP_long(maxId),
		MTP_long(minId),
		MTP_int(kPerPage)
	)).done([=](const MTPchannels_AdminLogResults &result) {
		_requestId = 0;
		const auto &data = result.c_channels_adminLogResults();
		const auto &events = data.vevents().v;
		_pendingEvents.append(events);
		_pendingUsers.append(data.vusers().v);
		_pendingChats.append(data.vchats().v);
		if (events.size() >= kPerPage && pagesLeft > 1) {
			requestPage(channel, events.back().data().vid().v, pagesLeft - 1);
		} else {
			applySync(channel);
			finishSync(id);
		}
	}).fail([=](const MTP::Error &error) {
		_requestId = 0;
		const auto type = error.type();
		if (type.startsWith(u"FLOOD_WAIT_"_q)) {
			const auto seconds = std::max(type.mid(11).toInt(), 60);
			_floodUntil = base::unixtime::now() + seconds;
			_queue.push_front(id);
		} else {
			LOG(("Admin Log Local: sync of %1 failed: %2."
				).arg(id.bare
				).arg(type));
		}
		finishSync(id);
	}).send();
}

void LocalHistory::applySync(not_null<ChannelData*> channel) {
	auto &data = this->channel(peerToChannel(channel->id));
	ensureLoaded(data);

	const auto now = base::unixtime::now();
	const auto serverFrom = now - kServerKeepsSeconds;
	if (!data.lastSync) {
		data.since = serverFrom;
	} else if (data.lastSync < serverFrom) {
		// The app didn't sync for longer than the server keeps the log.
		data.gaps.emplace_back(data.lastSync, serverFrom);
	}
	if (_pendingEvents.size() >= kPerPage * kMaxPagesPerSync) {
		// Too many events at once, older ones could have been skipped.
		const auto oldest = _pendingEvents.back().data().vdate().v;
		data.gaps.emplace_back(std::max(data.lastSync, serverFrom), oldest);
	}
	while (int(data.gaps.size()) > kMaxGaps) {
		data.gaps.erase(data.gaps.begin());
	}
	for (const auto &event : std::as_const(_pendingEvents)) {
		const auto id = event.data().vid().v;
		data.events.insert_or_assign(id, event);
		data.syncedId = std::max(data.syncedId, id);
	}
	const auto owner = &_session->data();
	for (const auto &user : std::as_const(_pendingUsers)) {
		const auto key = UserKey(user);
		data.users.insert_or_assign(key, user);
		if (!owner->peerLoaded(PeerId(key))) {
			owner->processUser(user);
		}
	}
	for (const auto &chat : std::as_const(_pendingChats)) {
		const auto key = ChatKey(chat);
		data.chats.insert_or_assign(key, chat);
		if (!owner->peerLoaded(PeerId(key))) {
			owner->processChat(chat);
		}
	}
	data.lastSync = now;
	prune(data);
	writeChannel(data);
	notify(data.id);
}

void LocalHistory::finishSync(ChannelId id) {
	if (_syncing == id) {
		_syncing = 0;
	}
	_pendingEvents.clear();
	_pendingUsers.clear();
	_pendingChats.clear();
	if (!_queue.empty()) {
		_nextTimer.callOnce(kBetweenChannelsDelay);
	}
}

void LocalHistory::prune(Channel &data) {
	const auto limit = EventsLimit();
	if (limit <= 0 || int(data.events.size()) <= limit) {
		return;
	}
	while (int(data.events.size()) > limit) {
		data.events.erase(data.events.begin());
	}
	const auto oldest = data.events.begin()->second.data().vdate().v;
	data.since = std::max(data.since, oldest);
	data.gaps.erase(ranges::remove_if(data.gaps, [&](const auto &gap) {
		return gap.second <= data.since;
	}), data.gaps.end());
}

void LocalHistory::clearAll() {
	_api.request(base::take(_requestId)).cancel();
	_queue.clear();
	_syncing = 0;
	_pendingEvents.clear();
	_pendingUsers.clear();
	_pendingChats.clear();
	for (auto &[id, data] : _channels) {
		const auto enabled = data->enabled;
		*data = Channel();
		data->id = id;
		data->enabled = enabled;
		data->loaded = true;
	}
	Storage::details::ScheduleRemoveFolder(folder());
	writeIndex();
	for (const auto &[id, data] : _channels) {
		notify(id);
	}
	_cleared.fire({});
}

void StartLocalHistory(not_null<Main::Session*> session) {
	auto &instances = Instances();
	if (instances.contains(session)) {
		return;
	}
	instances.emplace(session, std::make_unique<LocalHistory>(session));
	session->lifetime().add([=] {
		Instances().remove(session);
	});
}

} // namespace AdminLog
