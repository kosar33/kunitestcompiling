#pragma once
#include "IStableDiffusionClient.h"
#include "OpenAITools.h"
#include "telegram/ITelegramClient.h"

namespace tools {
using VoiceSynthesis = std::function<AFuture<APath>(AString)>;
VoiceSynthesis configuredVoiceSynthesis();

OpenAITools::Tool sendTelegramMessage(
    _<ITelegramClient> telegram,
    _<IOpenAIChat> openAI,
    _<td::td_api::chat> chat,
    _<td::td_api::array<td::td_api::object_ptr<td::td_api::message>>> messages,
    VoiceSynthesis synthesize = configuredVoiceSynthesis());
}
