//
// Created by alex2772 on 5/9/26.
//

#include "stickers.h"

#include <range/v3/all.hpp>

#include "llmui/audio.h"
#include "llmui/image.h"
#include "llmui/malicious_payloads.h"
#include "llmui/telegram.h"
#include "util/is_accessible_from_lockdown.h"
#include "util/json_utils.h"


static constexpr auto LOG_TAG = "tools::stickers";

AMap<int64_t, td::td_api::object_ptr<td::td_api::sticker>>& tools::stickers::knownStickers() {
    static AMap<int64_t, td::td_api::object_ptr<td::td_api::sticker>> result;
    return result;
}

static AFuture<AVector<td::td_api::object_ptr<td::td_api::sticker>>> getSavedStickers(ITelegramClient& telegram) {
    // a combination of favorite + recent stickers gives total storage for 30-40 stickers.
    auto favoriteStickers = co_await telegram.sendQueryWithResult(ITelegramClient::toPtr(td::td_api::getFavoriteStickers()));
    auto recentStickers = co_await telegram.sendQueryWithResult(ITelegramClient::toPtr(td::td_api::getRecentStickers()));
    AVector<td::td_api::object_ptr<td::td_api::sticker>> out;
    out << std::move(favoriteStickers->stickers_);
    out << std::move(recentStickers->stickers_);
    co_return out;
}

AFuture<AString> llmui::listFavoriteStickers(ITelegramClient& telegram, IOpenAIChat& openAI) {
    AString out;
    for (auto& stickerPtr : co_await getSavedStickers(telegram)) {
        // register the sticker in knownStickers() so that #sticker_save can resolve this sticker_id later;
        // previously only chat history rendering filled knownStickers(), so #sticker_save always failed with
        // "no such element" for stickers listed by #sticker_list.
        auto& known = tools::stickers::knownStickers()[stickerPtr->id_];
        if (!known) {
            known = std::move(stickerPtr);
        }
        auto& sticker = *known; // non-const: fetchMedia takes object_ptr<file>& and reassigns it while downloading
        llmui::checkForMaliciousPayloads(sticker.emoji_);
        const auto xmlTag =
            "sticker sticker_id=\"{}\" emoji=\"{}\""_format(sticker.id_, sticker.emoji_);
        // we rely on cache in llmui::image.
        out += co_await llmui::image({}, openAI, co_await llmui::fetchMedia(telegram, sticker.sticker_), xmlTag);
        out += "\n";
    }
    co_return out;
}

OpenAITools::Tool tools::stickers::list(_<ITelegramClient> telegram, _<IOpenAIChat> openAI) {
    return {
        .name = "sticker_list",
        .description = "Returns list of saved stickers. Use this before #sticker_send",
        .parameters = {},
        .handler = [telegram = std::move(telegram), openAI = std::move(openAI)](OpenAITools::Ctx ctx) -> AFuture<AString> {
            return llmui::listFavoriteStickers(*telegram, *openAI);
        },
    };
}

OpenAITools::Tool tools::stickers::save(_<ITelegramClient> telegram) {
    return {
        .name = "sticker_save",
        .description = "Saves sticker so you can use them later. Use this if you liked a sticker",
        .parameters = {
            .properties = {
                {"sticker_id", {.type = "integer", .description = "sticker_id of the sticker you would like to save"}},
            },
            .required = {"sticker_id"},
        },
        .handler = [telegram = std::move(telegram)](OpenAITools::Ctx ctx) -> AFuture<AString> {
            const auto stickerId = util::jsonAsLongInt(ctx.args["sticker_id"]).valueOrException("sticker_id integer required");
            if (stickerId == 0) {
                co_return "Error: sticker_id can't be 0";
            }

            // don't use .at(): an unknown sticker_id used to throw AException("no such element") and produce a huge
            // stack trace in the log instead of a recoverable tool error for the LLM.
            auto& known = knownStickers();
            auto it = known.find(stickerId);
            if (it == known.end() || !it->second) {
                co_return "Error: unknown sticker_id \"{}\". sticker_id values come from sticker_id=\"...\" "
                          "attributes in the chat history (requires capabilities.use_stickers) or from "
                          "#sticker_list output; re-read them and call #sticker_save again with a valid "
                          "sticker_id."_format(stickerId);
            }
            auto& sticker = *it->second;
            if (!sticker.sticker_ || !sticker.sticker_->remote_) {
                co_return "Error: sticker \"{}\" has no remote file id"_format(stickerId);
            }
            // extract before co_await: knownStickers() may be mutated by other coroutines meanwhile.
            const auto remoteFileId = sticker.sticker_->remote_->id_;

            // save sticker to tg.
            // this allows the account holder to manage/add/remove/list stickers that Kuni has saved.
            // we don't need to implement CRUD shit by ourselves, Pasha Durov did that for us.
            // also, telegram apparently has a native limit of up to 10 favorite stickers. the oldest one is removed.
            // this kinda suits our goal because (1) we don't want to have too many stickers (2) there are so many
            // stickers out there so sticker rotation is a good thing for us.
            co_await telegram->sendQueryWithResult(ITelegramClient::toPtr(td::td_api::addFavoriteSticker(ITelegramClient::toPtr(td::td_api::inputFileRemote(remoteFileId)))));
            co_return "Sticker \"{}\" saved successfully. You should acknowledge your participant you liked the sticker and saved it."_format(stickerId);
        },
    };
}

OpenAITools::Tool tools::stickers::send(_<ITelegramClient> telegram, _<td::td_api::chat> chat) {
    return {
        .name = "sticker_send",
        .description = "Sends specified sticker to \"{}\" chat"_format(chat->title_),
        .parameters = {
            .properties = {
                {"sticker_id", {.type = "integer", .description = "sticker_id of the sticker you would like to send"}},
                {"reply_to_message_id", {
                    .type = "integer",
                    .description = "If specified, the message will be rendered as a reply to the "
                    "message with given message id. You must use it if there are multiple messages "
                    "or to clearly address specific message."},
                },
            },
            .required = {"sticker_id"},
        },
        .handler = [telegram = std::move(telegram), chat = std::move(chat)](OpenAITools::Ctx ctx) -> AFuture<AString> {
            const auto stickerId = util::jsonAsLongInt(ctx.args["sticker_id"]).valueOrException("sticker_id integer required");
            const auto replyTo = util::jsonAsLongInt(ctx.args["reply_to_message_id"]).valueOr(0);
            if (stickerId == 0) {
                co_return "Error: sticker_id can't be 0";
            }

            if (! co_await util::isAccessibleFromLockdown(*telegram, chat->id_)) {
                ALogger::err(LOG_TAG) << "Lockdown mode is enabled. You can only send messages to chat with ID {} (PAPIK_CHAT_ID)."_format(
                    config().papikChatId);
                co_return "Error: lockdown mode is enabled.";
            }

            auto favoriteStickers = co_await getSavedStickers(*telegram);
            auto stickerIt = ranges::find_if(favoriteStickers, [&](const td::td_api::object_ptr<td::td_api::sticker>& sticker) {
                return sticker->id_ == stickerId;
            });
            if (stickerIt == favoriteStickers.end()) {
                co_return "Error: was not saved; you should have used #sticker_save beforehand";
            }
            td::td_api::sticker& sticker = **stickerIt;

            co_await telegram->sendQueryWithResult([&] {
                auto msg = td::td_api::make_object<td::td_api::sendMessage>();
                msg->chat_id_ = chat->id_;
                msg->input_message_content_ = [&]() -> td::td_api::object_ptr<td::td_api::InputMessageContent> {
                    auto inputMessageSticker = td::td_api::make_object<td::td_api::inputMessageSticker>();
                    inputMessageSticker->sticker_ = ITelegramClient::toPtr(td::td_api::inputFileRemote(sticker.sticker_->remote_->id_));
                    inputMessageSticker->emoji_ = sticker.emoji_;
                    inputMessageSticker->width_ = sticker.width_;
                    inputMessageSticker->height_ = sticker.height_;
                    return inputMessageSticker;
                }();
                if (replyTo != 0) {
                    msg->reply_to_ = ITelegramClient::toPtr(td::td_api::inputMessageReplyToMessage(replyTo, nullptr, 0));
                }
                return msg;
            }());

            co_return "Sticker \"{}\" sent successfuly to {}"_format(stickerId, chat->title_);
        },
    };
}