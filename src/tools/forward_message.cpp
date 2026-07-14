//
// Created by kuni on 6/24/26.
//

#include "forward_message.h"

#include "util/json_utils.h"

static constexpr auto LOG_TAG = "tools::forwardMessage";

OpenAITools::Tool tools::forwardMessage(_<ITelegramClient> telegram, _<td::td_api::chat> fromChat) {
    return {
        .name = "forward_message",
        .description = "Forwards one or more messages from \"{}\" to another chat (e.g., to a friend, "
                       "a group, or to Saved Messages). Use this when you find a post in a channel/a message in PM "
                       "interesting enough to share. You can optionally add a comment that will be sent as a separate "
                       "interesting after the forward."_format(fromChat->title_),
        .parameters = {
            .properties = {
                {"message_id", {
                    .type = "integer",
                    .description = "ID of the message to forward. Taken from message_id attribute in <message> tag.",
                }},
                {"to_chat_id", {
                    .type = "integer",
                    .description = "ID of the destination chat. Use #get_telegram_chats to find chat IDs. "
                                   "Use your own chat ID (Saved Messages) to forward to yourself.",
                }},
                {"comment", {
                    .type = "string",
                    .description = "Optional comment to send after the forwarded message. "
                                   "Express your reaction, thoughts, or why you found this interesting.",
                }},
            },
            .required = {"message_id", "to_chat_id"},
        },
        .handler = [telegram, fromChat](OpenAITools::Ctx ctx) -> AFuture<AString> {
            auto toChatId  = util::jsonAsLongInt(ctx.args["to_chat_id"]).valueOrException("to_chat_id integer required");
            auto comment   = ctx.args["comment"].asStringOpt();

            auto messageId = util::jsonAsLongInt(ctx.args["message_id"]).valueOrException("message_id integer required");
            std::vector<std::int64_t> messageIds = { messageId };
            auto messageCount = 1;

            ALogger::info(LOG_TAG) << "Forwarding " << messageCount << " message(s)"
                                   << " from chat " << fromChat->id_
                                   << " to chat " << toChatId;

            for (auto messageId : messageIds) {
                try {
                    // just check if provided messages are
                    co_await telegram->getMessage(fromChat->id_, messageId);
                } catch (const AException& e) {
                    co_return "Error: message {} was not found in \"{}\" chat."_format(messageId, fromChat->title_);
                }
            }

            auto fwd = td::td_api::make_object<td::td_api::forwardMessages>();
            fwd->chat_id_      = toChatId;
            fwd->from_chat_id_ = fromChat->id_;
            fwd->message_ids_  = std::move(messageIds);
            fwd->send_copy_    = false;
            fwd->remove_caption_ = false;

            co_await telegram->sendQueryWithResult(std::move(fwd));

            if (comment && !comment->empty()) {
                auto sendMsg = td::td_api::make_object<td::td_api::sendMessage>();
                sendMsg->chat_id_ = toChatId;
                auto content = td::td_api::make_object<td::td_api::inputMessageText>();
                auto formattedText = td::td_api::make_object<td::td_api::formattedText>();
                formattedText->text_ = comment->toStdString();
                content->text_ = std::move(formattedText);
                sendMsg->input_message_content_ = std::move(content);
                co_await telegram->sendQueryWithResult(std::move(sendMsg));
            }

            co_return "Forwarded {} message(s) successfully to chat_id={}"_format(messageCount, toChatId);
        },
    };
}
