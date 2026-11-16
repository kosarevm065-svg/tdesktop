/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#pragma once

class HistoryItem;
class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace ChatArchive {

inline constexpr auto kMasterOption = "chat_archive";
inline constexpr auto kMediaLimitOption = "chat_archive_media_mb";

struct ChatInfo {
	PeerId peerId = 0;
	QString name;
	TimeId updated = 0;
	bool enabled = false;
};

struct MediaInfo {
	QString type;
	QString name;
	QString mime;
	QString description;
	QString file;
	int64 size = 0;
};

struct Message {
	MsgId id = 0;
	TimeId date = 0;
	TimeId edited = 0;
	TimeId deleted = 0;
	PeerId fromId = 0;
	QString fromName;
	QString forwardFrom;
	TextWithEntities text;
	MsgId replyTo = 0;
	std::optional<MediaInfo> media;
	bool out = false;
	bool service = false;
};

[[nodiscard]] bool MasterEnabled();
void SetMasterEnabled(bool enabled);
[[nodiscard]] int MediaLimitMb();
void SetMediaLimitMb(int limit);

void Start(not_null<Main::Session*> session);
void Registered(not_null<HistoryItem*> item);

[[nodiscard]] bool IsEnabled(not_null<PeerData*> peer);
void SetEnabled(not_null<PeerData*> peer, bool enabled);
[[nodiscard]] bool HasArchive(not_null<Main::Session*> session, PeerId peerId);

[[nodiscard]] std::vector<ChatInfo> Chats(
	not_null<Main::Session*> session);
[[nodiscard]] std::vector<Message> LoadMessages(
	not_null<Main::Session*> session,
	PeerId peerId);
[[nodiscard]] QString ChatFolder(
	not_null<Main::Session*> session,
	PeerId peerId);
void RemoveArchive(not_null<Main::Session*> session, PeerId peerId);

[[nodiscard]] QString RootFolder(not_null<Main::Session*> session);

} // namespace ChatArchive
