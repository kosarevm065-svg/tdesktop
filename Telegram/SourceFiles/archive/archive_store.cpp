/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#include "archive/archive_store.h"

#include "base/timer.h"
#include "base/unixtime.h"
#include "core/enhanced_settings.h"
#include "data/data_changes.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_web_page.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/view/history_view_element.h"
#include "main/main_session.h"
#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>

namespace ChatArchive {
namespace {

constexpr auto kFlushTimeout = crl::time(1500);
constexpr auto kIndexWriteTimeout = crl::time(3000);
constexpr auto kMaxPendingDownloads = 64;

struct PendingDownload {
	PeerId peerId = 0;
	MsgId msgId = 0;
	QString target;
	DocumentData *document = nullptr;
	std::shared_ptr<Data::PhotoMedia> photo;
	bool started = false;
};

struct ChatState {
	QString name;
	TimeId updated = 0;
	bool enabled = false;
	bool hashesLoaded = false;
	base::flat_map<MsgId, size_t> hashes;
	base::flat_set<MsgId> deleted;
	base::flat_set<MsgId> withFile;
};

struct Store {
	explicit Store(not_null<Main::Session*> session);
	~Store();

	const not_null<Main::Session*> session;
	const QString root;
	base::flat_map<PeerId, ChatState> chats;
	base::flat_set<FullMsgId> queued;
	std::vector<PendingDownload> downloads;
	base::Timer flushTimer;
	base::Timer indexTimer;
	bool clearing = false;
	bool checkingDownloads = false;
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

[[nodiscard]] PeerId ParsePeer(const QJsonValue &value) {
	return PeerId(PeerIdHelper(value.toString().toULongLong()));
}

[[nodiscard]] QString ChatFile(const QString &root, PeerId peerId) {
	return root + u"chats/"_q + PeerKey(peerId) + u".jsonl"_q;
}

[[nodiscard]] QString MediaFolder(const QString &root, PeerId peerId) {
	return root + u"media/"_q + PeerKey(peerId) + '/';
}

[[nodiscard]] QString EntityName(EntityType type) {
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
	case EntityType::StrikeOut: return u"strike"_q;
	case EntityType::Code: return u"code"_q;
	case EntityType::Pre: return u"pre"_q;
	case EntityType::Blockquote: return u"blockquote"_q;
	case EntityType::Spoiler: return u"spoiler"_q;
	default: return QString();
	}
}

[[nodiscard]] EntityType EntityFromName(const QString &name) {
	static const auto map = base::flat_map<QString, EntityType>{
		{ u"url"_q, EntityType::Url },
		{ u"text_url"_q, EntityType::CustomUrl },
		{ u"email"_q, EntityType::Email },
		{ u"hashtag"_q, EntityType::Hashtag },
		{ u"cashtag"_q, EntityType::Cashtag },
		{ u"mention"_q, EntityType::Mention },
		{ u"mention_name"_q, EntityType::MentionName },
		{ u"bot_command"_q, EntityType::BotCommand },
		{ u"phone"_q, EntityType::Phone },
		{ u"bold"_q, EntityType::Bold },
		{ u"italic"_q, EntityType::Italic },
		{ u"underline"_q, EntityType::Underline },
		{ u"strike"_q, EntityType::StrikeOut },
		{ u"code"_q, EntityType::Code },
		{ u"pre"_q, EntityType::Pre },
		{ u"blockquote"_q, EntityType::Blockquote },
		{ u"spoiler"_q, EntityType::Spoiler },
	};
	const auto i = map.find(name);
	return (i != end(map)) ? i->second : EntityType::Invalid;
}

[[nodiscard]] QJsonArray SerializeEntities(const EntitiesInText &entities) {
	auto result = QJsonArray();
	for (const auto &entity : entities) {
		const auto name = EntityName(entity.type());
		if (name.isEmpty()) {
			continue;
		}
		auto item = QJsonArray{ name, entity.offset(), entity.length() };
		if (!entity.data().isEmpty()) {
			item.append(entity.data());
		}
		result.append(item);
	}
	return result;
}

[[nodiscard]] EntitiesInText ParseEntities(const QJsonArray &array) {
	auto result = EntitiesInText();
	for (const auto &value : array) {
		const auto item = value.toArray();
		if (item.size() < 3) {
			continue;
		}
		const auto type = EntityFromName(item[0].toString());
		if (type == EntityType::Invalid) {
			continue;
		}
		result.push_back(EntityInText(
			type,
			item[1].toInt(),
			item[2].toInt(),
			(item.size() > 3) ? item[3].toString() : QString()));
	}
	return result;
}

[[nodiscard]] QString DocumentType(not_null<DocumentData*> document) {
	if (document->sticker()) {
		return u"sticker"_q;
	} else if (document->isVoiceMessage()) {
		return u"voice"_q;
	} else if (document->isVideoMessage()) {
		return u"video_message"_q;
	} else if (document->isAnimation()) {
		return u"gif"_q;
	} else if (document->isVideoFile()) {
		return u"video"_q;
	} else if (document->isAudioFile()) {
		return u"audio"_q;
	}
	return u"file"_q;
}

[[nodiscard]] QString SafeFileName(QString name) {
	static const auto bad = QRegularExpression(u"[\\\\/:*?\"<>|\\x00-\\x1F]"_q);
	name.replace(bad, u"_"_q);
	return name.left(80);
}

[[nodiscard]] QJsonObject SerializeMedia(not_null<Data::Media*> media) {
	auto result = QJsonObject();
	if (const auto photo = media->photo()) {
		result.insert(u"type"_q, u"photo"_q);
		result.insert(
			u"size"_q,
			photo->imageByteSize(Data::PhotoSize::Large));
	} else if (const auto document = media->document()) {
		result.insert(u"type"_q, DocumentType(document));
		result.insert(u"name"_q, document->filename());
		result.insert(u"mime"_q, document->mimeString());
		result.insert(u"size"_q, double(document->size));
	} else if (const auto webpage = media->webpage()) {
		result.insert(u"type"_q, u"webpage"_q);
		result.insert(u"description"_q, webpage->url);
	} else {
		result.insert(u"type"_q, u"other"_q);
	}
	if (!result.contains(u"description"_q)) {
		const auto text = media->notificationText().text;
		if (!text.isEmpty()) {
			result.insert(u"description"_q, text);
		}
	}
	return result;
}

[[nodiscard]] QJsonObject SerializeItem(not_null<const HistoryItem*> item) {
	auto result = QJsonObject();
	result.insert(u"t"_q, u"m"_q);
	result.insert(u"id"_q, double(item->id.bare));
	result.insert(u"date"_q, item->date());
	const auto from = item->from();
	result.insert(u"from"_q, PeerKey(from->id));
	result.insert(u"from_name"_q, from->name());
	if (item->out()) {
		result.insert(u"out"_q, true);
	}
	const auto service = item->isService();
	const auto text = service
		? item->notificationText()
		: item->originalText();
	if (service) {
		result.insert(u"service"_q, true);
	}
	if (!text.text.isEmpty()) {
		result.insert(u"text"_q, text.text);
		const auto entities = SerializeEntities(text.entities);
		if (!entities.isEmpty()) {
			result.insert(u"entities"_q, entities);
		}
	}
	if (const auto replyTo = item->replyToId()) {
		result.insert(u"reply_to"_q, double(replyTo.bare));
	}
	if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
		const auto name = forwarded->originalSender
			? forwarded->originalSender->name()
			: forwarded->originalHiddenSenderInfo
			? forwarded->originalHiddenSenderInfo->name
			: QString();
		if (!name.isEmpty()) {
			result.insert(u"forward_from"_q, name);
		}
	}
	if (const auto edited = item->Get<HistoryMessageEdited>()) {
		if (edited->date) {
			result.insert(u"edited"_q, edited->date);
		}
	}
	if (const auto media = item->media()) {
		result.insert(u"media"_q, SerializeMedia(media));
	}
	return result;
}

[[nodiscard]] QString Root(not_null<Main::Session*> session) {
	return cWorkingDir()
		+ u"tdata/64gram_archive/"_q
		+ QString::number(session->userId().bare)
		+ '/';
}

void AppendLines(
		const QString &path,
		const std::vector<QJsonObject> &records) {
	if (records.empty()) {
		return;
	}
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
		LOG(("Chat Archive Error: could not open %1").arg(path));
		return;
	}
	for (const auto &record : records) {
		file.write(QJsonDocument(record).toJson(QJsonDocument::Compact));
		file.write("\n", 1);
	}
}

template <typename Callback>
void ReadLines(const QString &path, Callback &&callback) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	while (!file.atEnd()) {
		const auto line = file.readLine().trimmed();
		if (line.isEmpty()) {
			continue;
		}
		auto error = QJsonParseError();
		const auto document = QJsonDocument::fromJson(line, &error);
		if (error.error == QJsonParseError::NoError && document.isObject()) {
			callback(document.object());
		}
	}
}

[[nodiscard]] size_t RecordHash(const QJsonObject &record) {
	const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
	return size_t(qHash(bytes));
}

void LoadHashes(not_null<Store*> store, PeerId peerId, ChatState &state) {
	if (state.hashesLoaded) {
		return;
	}
	state.hashesLoaded = true;
	ReadLines(ChatFile(store->root, peerId), [&](const QJsonObject &record) {
		const auto id = MsgId(int64(record.value(u"id"_q).toDouble()));
		const auto type = record.value(u"t"_q).toString();
		if (type == u"m"_q) {
			state.hashes[id] = RecordHash(record);
		} else if (type == u"d"_q) {
			state.deleted.emplace(id);
		} else if (type == u"f"_q) {
			state.withFile.emplace(id);
		}
	});
}

void WriteIndex(not_null<Store*> store) {
	store->indexTimer.cancel();
	auto chats = QJsonArray();
	for (const auto &[peerId, state] : store->chats) {
		chats.append(QJsonObject{
			{ u"peer"_q, PeerKey(peerId) },
			{ u"name"_q, state.name },
			{ u"updated"_q, state.updated },
			{ u"enabled"_q, state.enabled },
		});
	}
	QDir().mkpath(store->root);
	auto file = QFile(store->root + u"index.json"_q);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(QJsonObject{
			{ u"version"_q, 1 },
			{ u"chats"_q, chats },
		}).toJson(QJsonDocument::Indented));
	}
}

void ReadIndex(not_null<Store*> store) {
	auto file = QFile(store->root + u"index.json"_q);
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	for (const auto &value : document.object().value(u"chats"_q).toArray()) {
		const auto object = value.toObject();
		const auto peerId = ParsePeer(object.value(u"peer"_q));
		if (!peerId) {
			continue;
		}
		auto &state = store->chats[peerId];
		state.name = object.value(u"name"_q).toString();
		state.updated = object.value(u"updated"_q).toInt();
		state.enabled = object.value(u"enabled"_q).toBool();
	}
}

[[nodiscard]] bool Archiving(not_null<Store*> store, PeerId peerId) {
	if (!MasterEnabled()) {
		return false;
	}
	const auto i = store->chats.find(peerId);
	return (i != end(store->chats)) && i->second.enabled;
}

void CheckDownloads(not_null<Store*> store);

void ScheduleMedia(
		not_null<Store*> store,
		not_null<const HistoryItem*> item,
		ChatState &state) {
	const auto limit = int64(MediaLimitMb()) * 1024 * 1024;
	const auto media = item->media();
	if (!limit
		|| !media
		|| state.withFile.contains(item->id)
		|| int(store->downloads.size()) >= kMaxPendingDownloads) {
		return;
	}
	const auto peerId = item->history()->peer->id;
	for (const auto &pending : store->downloads) {
		if (pending.peerId == peerId && pending.msgId == item->id) {
			return;
		}
	}
	const auto folder = MediaFolder(store->root, peerId);
	const auto prefix = QString::number(item->id.bare);
	if (const auto photo = media->photo()) {
		if (photo->imageByteSize(Data::PhotoSize::Large) > limit) {
			return;
		}
		auto view = photo->createMediaView();
		photo->load(
			Data::PhotoSize::Large,
			item->fullId(),
			LoadFromCloudOrLocal,
			true);
		store->downloads.push_back({
			.peerId = peerId,
			.msgId = item->id,
			.target = folder + prefix + u".jpg"_q,
			.photo = std::move(view),
		});
	} else if (const auto document = media->document()) {
		if (document->size > limit || document->sticker()) {
			return;
		}
		auto name = SafeFileName(document->filename());
		if (name.isEmpty()) {
			name = u"file"_q;
		}
		store->downloads.push_back({
			.peerId = peerId,
			.msgId = item->id,
			.target = folder + prefix + '_' + name,
			.document = document,
		});
	} else {
		return;
	}
	CheckDownloads(store);
}

void FileSaved(
		not_null<Store*> store,
		PeerId peerId,
		MsgId msgId,
		const QString &target) {
	auto &state = store->chats[peerId];
	state.withFile.emplace(msgId);
	auto record = QJsonObject{
		{ u"t"_q, u"f"_q },
		{ u"id"_q, double(msgId.bare) },
		{ u"file"_q, QFileInfo(target).fileName() },
	};
	AppendLines(ChatFile(store->root, peerId), { record });
}

void CheckDownloads(not_null<Store*> store) {
	if (store->checkingDownloads) {
		return;
	}
	store->checkingDownloads = true;
	const auto guard = gsl::finally([&] {
		store->checkingDownloads = false;
	});
	auto &list = store->downloads;
	for (auto i = begin(list); i != end(list);) {
		auto done = false;
		if (i->photo) {
			const auto owner = i->photo->owner();
			if (i->photo->loaded()) {
				QDir().mkpath(QFileInfo(i->target).absolutePath());
				if (i->photo->saveToFile(i->target)) {
					FileSaved(store, i->peerId, i->msgId, i->target);
				}
				done = true;
			} else if (owner->failed(Data::PhotoSize::Large)) {
				done = true;
			}
		} else if (const auto document = i->document) {
			const auto existing = document->filepath(true);
			if (!existing.isEmpty() && existing != i->target) {
				QDir().mkpath(QFileInfo(i->target).absolutePath());
				QFile::remove(i->target);
				if (QFile::copy(existing, i->target)) {
					FileSaved(store, i->peerId, i->msgId, i->target);
				}
				done = true;
			} else if (!existing.isEmpty()) {
				FileSaved(store, i->peerId, i->msgId, i->target);
				done = true;
			} else if (document->loading()) {
				done = false;
			} else if (i->started) {
				if (QFileInfo(i->target).size() == document->size) {
					FileSaved(store, i->peerId, i->msgId, i->target);
				}
				done = true;
			} else {
				i->started = true;
				QDir().mkpath(QFileInfo(i->target).absolutePath());
				const auto item = store->session->data().message(
					i->peerId,
					i->msgId);
				const auto origin = item
					? Data::FileOrigin(item->fullId())
					: Data::FileOrigin();
				document->save(
					origin,
					i->target,
					LoadFromCloudOrLocal,
					true);
			}
		} else {
			done = true;
		}
		i = done ? list.erase(i) : (i + 1);
	}
}

void ArchiveItems(
		not_null<Store*> store,
		const std::vector<not_null<const HistoryItem*>> &items) {
	auto byPeer = base::flat_map<PeerId, std::vector<QJsonObject>>();
	for (const auto &item : items) {
		if (!item->isRegular()
			|| item->isScheduled()
			|| !IsServerMsgId(item->id)) {
			continue;
		}
		const auto peer = item->history()->peer;
		if (!Archiving(store, peer->id)) {
			continue;
		}
		auto &state = store->chats[peer->id];
		LoadHashes(store, peer->id, state);
		auto record = SerializeItem(item);
		const auto hash = RecordHash(record);
		const auto i = state.hashes.find(item->id);
		if (i == end(state.hashes) || i->second != hash) {
			state.hashes[item->id] = hash;
			byPeer[peer->id].push_back(std::move(record));
		}
		ScheduleMedia(store, item, state);
	}
	const auto now = base::unixtime::now();
	for (const auto &[peerId, records] : byPeer) {
		AppendLines(ChatFile(store->root, peerId), records);
		auto &state = store->chats[peerId];
		state.updated = now;
		if (const auto peer = store->session->data().peerLoaded(peerId)) {
			state.name = peer->name();
		}
	}
	if (!byPeer.empty() && !store->indexTimer.isActive()) {
		store->indexTimer.callOnce(kIndexWriteTimeout);
	}
}

void Flush(not_null<Store*> store) {
	store->flushTimer.cancel();
	if (store->queued.empty()) {
		return;
	}
	auto items = std::vector<not_null<const HistoryItem*>>();
	items.reserve(store->queued.size());
	for (const auto &id : base::take(store->queued)) {
		if (const auto item = store->session->data().message(id)) {
			items.push_back(item);
		}
	}
	ArchiveItems(store, items);
}

void Enqueue(not_null<Store*> store, FullMsgId id) {
	if (!Archiving(store, id.peer) || !IsServerMsgId(id.msg)) {
		return;
	}
	store->queued.emplace(id);
	if (!store->flushTimer.isActive()) {
		store->flushTimer.callOnce(kFlushTimeout);
	}
}

void MarkDeleted(
		not_null<Store*> store,
		const std::vector<not_null<HistoryItem*>> &items) {
	if (store->clearing) {
		return;
	}
	Flush(store);
	ArchiveItems(store, std::vector<not_null<const HistoryItem*>>(
		begin(items),
		end(items)));
	auto byPeer = base::flat_map<PeerId, std::vector<QJsonObject>>();
	const auto now = base::unixtime::now();
	for (const auto &item : items) {
		const auto peerId = item->history()->peer->id;
		if (!IsServerMsgId(item->id) || !Archiving(store, peerId)) {
			continue;
		}
		auto &state = store->chats[peerId];
		if (!state.hashes.contains(item->id)
			|| state.deleted.contains(item->id)) {
			continue;
		}
		state.deleted.emplace(item->id);
		byPeer[peerId].push_back(QJsonObject{
			{ u"t"_q, u"d"_q },
			{ u"id"_q, double(item->id.bare) },
			{ u"date"_q, now },
		});
	}
	for (const auto &[peerId, records] : byPeer) {
		AppendLines(ChatFile(store->root, peerId), records);
	}
}

Store::Store(not_null<Main::Session*> session)
: session(session)
, root(Root(session))
, flushTimer([=] { Flush(this); })
, indexTimer([=] { WriteIndex(this); }) {
	ReadIndex(this);
}

Store::~Store() {
	if (indexTimer.isActive()) {
		WriteIndex(this);
	}
}

} // namespace

bool MasterEnabled() {
	return GetEnhancedBool(kMasterOption);
}

void SetMasterEnabled(bool enabled) {
	SetEnhancedValue(kMasterOption, enabled);
	EnhancedSettings::Write();
}

int MediaLimitMb() {
	return std::max(GetEnhancedInt(kMediaLimitOption), 0);
}

void SetMediaLimitMb(int limit) {
	SetEnhancedValue(kMediaLimitOption, std::max(limit, 0));
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

	session->data().itemIdChanged(
	) | rpl::on_next([=](const Data::Session::IdChange &change) {
		Enqueue(store, change.newId);
	}, store->lifetime);

	session->changes().messageUpdates(
		Data::MessageUpdate::Flag::Edited
	) | rpl::on_next([=](const Data::MessageUpdate &update) {
		Enqueue(store, update.item->fullId());
	}, store->lifetime);

	session->data().itemsAboutToBeDestroyed(
	) | rpl::on_next([=](std::vector<not_null<HistoryItem*>> items) {
		MarkDeleted(store, items);
	}, store->lifetime);

	session->data().itemRemoved(
	) | rpl::on_next([=](not_null<const HistoryItem*> item) {
		const auto id = item->fullId();
		if (store->queued.remove(id)) {
			ArchiveItems(store, { item });
		}
	}, store->lifetime);

	session->data().sessionDataAboutToBeCleared(
	) | rpl::on_next([=] {
		Flush(store);
		store->clearing = true;
		store->downloads.clear();
	}, store->lifetime);

	session->downloaderTaskFinished(
	) | rpl::filter([=] {
		return !store->downloads.empty();
	}) | rpl::on_next([=] {
		CheckDownloads(store);
	}, store->lifetime);

	session->lifetime().add([=] {
		Stores().remove(session);
	});
}

void Registered(not_null<HistoryItem*> item) {
	const auto session = &item->history()->session();
	if (const auto store = Find(session)) {
		Enqueue(store, FullMsgId(item->history()->peer->id, item->id));
	}
}

bool IsEnabled(not_null<PeerData*> peer) {
	const auto store = Find(&peer->session());
	return store && Archiving(store, peer->id);
}

void SetEnabled(not_null<PeerData*> peer, bool enabled) {
	const auto store = Find(&peer->session());
	if (!store) {
		return;
	}
	auto &state = store->chats[peer->id];
	state.enabled = enabled;
	state.name = peer->name();
	if (!state.updated) {
		state.updated = base::unixtime::now();
	}
	WriteIndex(store);
	if (!enabled) {
		return;
	}
	auto items = std::vector<not_null<const HistoryItem*>>();
	if (const auto history = peer->owner().historyLoaded(peer)) {
		for (const auto &block : history->blocks) {
			for (const auto &view : block->messages) {
				items.push_back(view->data());
			}
		}
	}
	ArchiveItems(store, items);
}

bool HasArchive(not_null<Main::Session*> session, PeerId peerId) {
	return QFile::exists(ChatFile(Root(session), peerId));
}

std::vector<ChatInfo> Chats(not_null<Main::Session*> session) {
	auto result = std::vector<ChatInfo>();
	if (const auto store = Find(session)) {
		Flush(store);
		for (const auto &[peerId, state] : store->chats) {
			result.push_back({
				.peerId = peerId,
				.name = state.name,
				.updated = state.updated,
				.enabled = state.enabled,
			});
		}
	}
	ranges::sort(result, ranges::greater(), &ChatInfo::updated);
	return result;
}

std::vector<Message> LoadMessages(
		not_null<Main::Session*> session,
		PeerId peerId) {
	if (const auto store = Find(session)) {
		Flush(store);
	}
	const auto root = Root(session);
	const auto folder = MediaFolder(root, peerId);
	auto map = base::flat_map<MsgId, Message>();
	ReadLines(ChatFile(root, peerId), [&](const QJsonObject &record) {
		const auto id = MsgId(int64(record.value(u"id"_q).toDouble()));
		const auto type = record.value(u"t"_q).toString();
		if (type == u"d"_q) {
			const auto i = map.find(id);
			if (i != end(map)) {
				i->second.deleted = record.value(u"date"_q).toInt();
			}
			return;
		} else if (type == u"f"_q) {
			const auto i = map.find(id);
			if (i != end(map) && i->second.media) {
				i->second.media->file = folder
					+ record.value(u"file"_q).toString();
			}
			return;
		} else if (type != u"m"_q) {
			return;
		}
		auto &message = map[id];
		const auto deleted = message.deleted;
		auto file = message.media ? message.media->file : QString();
		message = Message{
			.id = id,
			.date = record.value(u"date"_q).toInt(),
			.edited = record.value(u"edited"_q).toInt(),
			.deleted = deleted,
			.fromId = ParsePeer(record.value(u"from"_q)),
			.fromName = record.value(u"from_name"_q).toString(),
			.forwardFrom = record.value(u"forward_from"_q).toString(),
			.text = TextWithEntities{
				record.value(u"text"_q).toString(),
				ParseEntities(record.value(u"entities"_q).toArray()),
			},
			.replyTo = MsgId(int64(record.value(u"reply_to"_q).toDouble())),
			.out = record.value(u"out"_q).toBool(),
			.service = record.value(u"service"_q).toBool(),
		};
		if (record.contains(u"media"_q)) {
			const auto media = record.value(u"media"_q).toObject();
			message.media = MediaInfo{
				.type = media.value(u"type"_q).toString(),
				.name = media.value(u"name"_q).toString(),
				.mime = media.value(u"mime"_q).toString(),
				.description = media.value(u"description"_q).toString(),
				.file = std::move(file),
				.size = int64(media.value(u"size"_q).toDouble()),
			};
		}
	});
	auto result = std::vector<Message>();
	result.reserve(map.size());
	for (auto &[id, message] : map) {
		result.push_back(std::move(message));
	}
	return result;
}

QString ChatFolder(not_null<Main::Session*> session, PeerId peerId) {
	return MediaFolder(Root(session), peerId);
}

void RemoveArchive(not_null<Main::Session*> session, PeerId peerId) {
	const auto root = Root(session);
	QFile::remove(ChatFile(root, peerId));
	QDir(MediaFolder(root, peerId)).removeRecursively();
	if (const auto store = Find(session)) {
		store->chats.remove(peerId);
		WriteIndex(store);
	}
}

QString RootFolder(not_null<Main::Session*> session) {
	return Root(session);
}

} // namespace ChatArchive
