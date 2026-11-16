/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "mtproto/sender.h"

namespace Main {
class Session;
} // namespace Main

namespace AdminLog {

struct FilterValue;

// Telegram servers return only the last 48 hours of the admin log, so the
// log of chosen channels is fetched periodically and kept in an encrypted
// per-account file to let "Recent actions" scroll past that limit.
struct LocalHistoryState {
	bool enabled = false;
	int count = 0;
	// Since when the local history is complete (except the gaps).
	TimeId since = 0;
	TimeId lastSync = 0;
	// Periods the app was offline longer than the server keeps the log.
	std::vector<std::pair<TimeId, TimeId>> gaps;
};

class LocalHistory final {
public:
	explicit LocalHistory(not_null<Main::Session*> session);
	~LocalHistory();

	[[nodiscard]] static LocalHistory *Find(
		not_null<Main::Session*> session);

	[[nodiscard]] static bool MasterEnabled();
	static void SetMasterEnabled(bool enabled);
	// Max events per chat, 0 means unlimited.
	[[nodiscard]] static int EventsLimit();
	[[nodiscard]] static int EventsLimitSetting();
	static void SetEventsLimitSetting(int value);
	[[nodiscard]] static std::vector<int> EventsLimitOptions();

	[[nodiscard]] bool enabled(not_null<ChannelData*> channel) const;
	void setEnabled(not_null<ChannelData*> channel, bool enabled);

	[[nodiscard]] LocalHistoryState state(not_null<ChannelData*> channel);
	[[nodiscard]] rpl::producer<LocalHistoryState> stateValue(
		not_null<ChannelData*> channel);
	[[nodiscard]] bool hasEvents(not_null<ChannelData*> channel);

	// Stored events with id < maxId (any id if maxId == 0), newest first.
	// Users and chats they mention get registered in the data session.
	[[nodiscard]] QVector<MTPChannelAdminLogEvent> query(
		not_null<ChannelData*> channel,
		uint64 maxId,
		const FilterValue &filter,
		const QString &search,
		int limit);

	void syncNow(not_null<ChannelData*> channel);
	void clearAll();

	[[nodiscard]] rpl::producer<> clearedEvents() const;

private:
	struct Channel;

	[[nodiscard]] Channel &channel(ChannelId id);
	[[nodiscard]] Channel *loadedChannel(ChannelId id);
	void ensureLoaded(Channel &data);
	void readIndex();
	void writeIndex();
	void readChannel(Channel &data);
	void writeChannel(const Channel &data);
	[[nodiscard]] QString folder() const;
	[[nodiscard]] static QString ChannelFileName(ChannelId id);

	void scheduleSync(crl::time delay);
	void syncAll();
	void syncNext();
	void requestPage(
		not_null<ChannelData*> channel,
		uint64 maxId,
		int pagesLeft);
	void applySync(not_null<ChannelData*> channel);
	void finishSync(ChannelId id);
	void prune(Channel &data);
	void registerPeers(Channel &data);
	void notify(ChannelId id);

	const not_null<Main::Session*> _session;
	MTP::Sender _api;
	base::Timer _timer;
	base::Timer _nextTimer;

	base::flat_map<ChannelId, std::unique_ptr<Channel>> _channels;
	std::deque<ChannelId> _queue;
	ChannelId _syncing = 0;
	mtpRequestId _requestId = 0;
	TimeId _floodUntil = 0;

	// Accumulated while paging through a single sync.
	QVector<MTPChannelAdminLogEvent> _pendingEvents;
	QVector<MTPUser> _pendingUsers;
	QVector<MTPChat> _pendingChats;

	rpl::event_stream<ChannelId> _changes;
	rpl::event_stream<> _cleared;

	rpl::lifetime _lifetime;

};

void StartLocalHistory(not_null<Main::Session*> session);

} // namespace AdminLog
