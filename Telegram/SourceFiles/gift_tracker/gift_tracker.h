/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_peer_id.h"

namespace Main {
class Session;
} // namespace Main

namespace Data {
struct SavedStarGift;
} // namespace Data

namespace MTP {
class Error;
} // namespace MTP

namespace GiftTracker {

struct GiftRecord {
	QString key;
	uint64 giftId = 0;
	QString title;
	QString slug;
	int64 stars = 0;
	PeerId fromId = 0;
	QString fromName;
	TimeId date = 0;
	int number = 0;
	bool unique = false;
};

enum class EventType {
	Started,
	Added,
	Removed,
	Upgraded,
};

struct GiftEvent {
	TimeId when = 0;
	EventType type = EventType::Added;
	GiftRecord gift;
	int count = 0;
};

struct TrackedPeer {
	PeerId id = 0;
	uint64 accessHash = 0;
	QString name;
	TimeId added = 0;
	TimeId lastCheck = 0;
	QString lastError;
	bool hasSnapshot = false;
	std::vector<GiftRecord> snapshot;
	std::vector<GiftEvent> events;
};

[[nodiscard]] bool Enabled();
void SetEnabled(bool enabled);
[[nodiscard]] bool NotifyEnabled();
void SetNotifyEnabled(bool enabled);

[[nodiscard]] QString GiftName(const GiftRecord &gift);

class Tracker final : public base::has_weak_ptr {
public:
	explicit Tracker(not_null<Main::Session*> session);
	~Tracker();

	[[nodiscard]] bool tracked(PeerId peerId) const;
	[[nodiscard]] const TrackedPeer *find(PeerId peerId) const;
	[[nodiscard]] std::vector<PeerId> trackedPeers() const;

	void track(not_null<PeerData*> peer);
	void untrack(PeerId peerId);
	bool checkNow(PeerId peerId);

	[[nodiscard]] rpl::producer<PeerId> updates() const;

private:
	struct Fetch {
		PeerId peerId = 0;
		std::vector<GiftRecord> gifts;
		QString offset;
		int pages = 0;
		int generation = 0;
		bool truncated = false;
	};

	void load();
	void save();
	void scheduleTick(crl::time delay);
	void tick();
	[[nodiscard]] TrackedPeer *nextDue(TimeId now, TimeId &nearest);
	[[nodiscard]] TimeId interval(PeerId peerId) const;
	void start(not_null<TrackedPeer*> entry);
	void requestPage();
	void finishFetch();
	void failFetch(const MTP::Error &error);
	void apply(not_null<TrackedPeer*> entry, Fetch &&fetch);
	void notify(not_null<TrackedPeer*> entry, int added, int removed);
	[[nodiscard]] TrackedPeer *entry(PeerId peerId);
	[[nodiscard]] GiftRecord parse(const Data::SavedStarGift &gift) const;

	const not_null<Main::Session*> _session;
	std::vector<TrackedPeer> _peers;
	std::optional<Fetch> _fetch;
	mtpRequestId _requestId = 0;
	base::Timer _timer;
	base::flat_set<PeerId> _forced;
	crl::time _floodUntil = 0;
	rpl::event_stream<PeerId> _updates;

};

void Start(not_null<Main::Session*> session);
[[nodiscard]] Tracker *Find(not_null<Main::Session*> session);

} // namespace GiftTracker
