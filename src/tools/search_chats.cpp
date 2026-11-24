//
// Created by alex2772 on 5/9/26.
//

#include "search_chats.h"

#include "llmui/telegram.h"

#include <range/v3/range/conversion.hpp>
#include <range/v3/view/transform.hpp>

OpenAITools::Tool tools::searchChats(_<ITelegramClient> telegram) {
    return {
        .name = "search_chats",
        .description = "Searches for chats by @username or name.",
        .parameters =
            {
                .properties =
                    {
                        {"query", {.type = "string", .description = "The username or name of the "
                            "chat. Examples: \n"
                            "- @alex2772sc\n"
                            "- Alex2772\n"
                        }},
                    },
                .required = {"query"},
            },
        .handler = [telegram = std::move(telegram)](OpenAITools::Ctx ctx) -> AFuture<AString> {
            auto query = ctx.args["query"].asStringOpt().valueOrException("query string is required");
            query = query.trim(" \t\r\n");
            if (query.startsWith("@")) {
                query = query.substr(1);
            }

            if (query.empty()) {
                co_return "Enter a chat name or @username.";
            }
            auto queryResult = co_await telegram->sendQueryWithResult(ITelegramClient::toPtr(td::td_api::searchChatsOnServer(query, 50)));
            td::td_api::object_ptr<td::td_api::chat> publicChat;
            const auto text = query.toStdString();
            if (text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos) {
                try {
                    publicChat = co_await telegram->sendQueryWithResult(ITelegramClient::toPtr(td::td_api::searchPublicChat(query)));
                } catch (const AException& e) {
                    const auto reason = e.getMessage();
                    if (!reason.endsWith("USERNAME_INVALID") && !reason.endsWith("USERNAME_NOT_OCCUPIED")) {
                        throw;
                    }
                }
            }
            if (queryResult->chat_ids_.empty() && (!publicChat || !publicChat->id_)) {
                co_return "No chats found satisfying your query.";
            }

            AString result;
            AVector<_<td::td_api::chat>> chats;
            try {
                auto chatFutures =
                    queryResult->chat_ids_ | ranges::view::transform([&](td::td_api::int53 chatId) {
                        return telegram->getChat(chatId);
                    }) |
                    ranges::to_vector;
                chats.reserve(chatFutures.size());
                for (const auto& chat : chatFutures) {
                    chats.push_back(co_await chat);
                }
            } catch (const AException& e) {
                ALogger::err("search_chats") << "chatIdsToChats(queryResult->chat_ids_) failed: " << e;
            }
            result += "<existing_chats comment=\"Chats that you participate already\">\n";
            co_await llmui::formatChatList(*telegram, result, chats);
            result += "</existing_chats>\n";
            result += "<global_search comment=\"Chats that don't know about you\">\n";
            if (publicChat && publicChat->id_) {
                co_await llmui::formatChatSingle(*telegram, result, *publicChat);
            }
            result += "</global_search>\n";

            co_return result;
        },
    };
}
