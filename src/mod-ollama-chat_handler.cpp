#include "Log.h"
#include "Language.h"
#include "Player.h"
#include "Chat.h"
#include "Channel.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Config.h"
#include "Common.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "ObjectAccessor.h"
#include "World.h"
#include "AiFactory.h"
#include "ChannelMgr.h"
#include <sstream>
#include <vector>
#include <fmt/core.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <algorithm>
#include <random>
#include <cctype>
#include <chrono>
#include <ctime>
#include "DatabaseEnv.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat-utilities.h"
#include "mod-ollama-chat_sentiment.h"
#include "mod-ollama-chat_rag.h"
#include "ChatHelper.h"
#include <iomanip>
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "SharedDefines.h"
#include "Group.h"
#include "Creature.h"
#include "GameObject.h"
#include "TravelMgr.h"
#include "TravelNode.h"
#include "ObjectMgr.h"
#include "QuestDef.h"

// For AzerothCore range checks
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Map.h"
#include "GridNotifiers.h"
#include "PathGenerator.h"

// Forward declarations for internal helper functions.
enum class NaturalCommand;
static bool IsBotEligibleForChatChannelLocal(Player* bot, Player* player,
                                             ChatChannelSourceLocal source, Channel* channel = nullptr, Player* receiver = nullptr);
static std::string GenerateBotPrompt(Player* bot, std::string playerMessage, Player* player);
static void AppendBotConversation(uint64_t botGuid, uint64_t playerGuid, const std::string& playerMessage, const std::string& botReply);
static bool ExecutePlayerbotCommand(Player* bot, Player* commandInitiator,
    NaturalCommand cmd, std::string const& argument = "", std::string const& castTargetHint = "",
    std::string const& attackTargetHint = "", ChatChannelSourceLocal sourceLocal = SRC_UNDEFINED_LOCAL);
static void QueueBotRepliesForCandidates(std::vector<ObjectGuid> const& botGuids,
    ObjectGuid senderGuid, std::string const& msg, ChatChannelSourceLocal sourceLocal,
    uint32 channelId, std::string const& channelName);

// Natural language command detection and execution
enum class NaturalCommand {
    NONE,
    FOLLOW,
    STAY,
    ATTACK,
    FLEE,
    AVOID_AOE,
    AVOID_AOE_OFF,
    TRADE,
    CAST
};

struct ParsedNaturalCommand
{
    NaturalCommand command = NaturalCommand::NONE;
    std::string argument;
    std::string castTarget;
    std::string attackTarget;
};

static std::string TrimWhitespace(std::string value)
{
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch)
    {
        return !std::isspace(ch);
    }));

    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch)
    {
        return !std::isspace(ch);
    }).base(), value.end());

    return value;
}

static std::string NormalizeSpellText(std::string text)
{
    for (char& c : text)
        c = std::tolower(static_cast<unsigned char>(c));

    std::string normalized;
    normalized.reserve(text.size());

    bool previousWasSpace = true;
    for (char const c : text)
    {
        if (std::isalnum(static_cast<unsigned char>(c)))
        {
            normalized.push_back(c);
            previousWasSpace = false;
        }
        else if (!previousWasSpace)
        {
            normalized.push_back(' ');
            previousWasSpace = true;
        }
    }

    if (!normalized.empty() && normalized.back() == ' ')
        normalized.pop_back();

    return normalized;
}

static std::string ToLowerAscii(std::string value)
{
    for (char& c : value)
        c = std::tolower(static_cast<unsigned char>(c));

    return value;
}

static bool ContainsWholeWordCaseInsensitive(std::string const& message, std::string const& keyword)
{
    if (message.empty() || keyword.empty())
        return false;

    std::string lowerMessage = ToLowerAscii(message);
    std::string lowerKeyword = ToLowerAscii(keyword);

    size_t pos = 0;
    while ((pos = lowerMessage.find(lowerKeyword, pos)) != std::string::npos)
    {
        bool validStart = (pos == 0 || !std::isalnum(static_cast<unsigned char>(lowerMessage[pos - 1])));
        size_t endPos = pos + lowerKeyword.length();
        bool validEnd = (endPos >= lowerMessage.length() || !std::isalnum(static_cast<unsigned char>(lowerMessage[endPos])));

        if (validStart && validEnd)
            return true;

        ++pos;
    }

    return false;
}

static std::string NormalizeCommandTargetHint(std::string targetHint)
{
    std::string trimmed = TrimWhitespace(targetHint);
    std::string normalized = NormalizeSpellText(trimmed);

    auto stripPrefix = [&normalized](std::string const& prefix)
    {
        if (normalized.rfind(prefix, 0) == 0)
            normalized = TrimWhitespace(normalized.substr(prefix.length()));
    };

    stripPrefix("to ");
    stripPrefix("on ");
    stripPrefix("at ");
    stripPrefix("raid ");
    stripPrefix("raid marker ");
    stripPrefix("marker ");
    stripPrefix("marked ");
    stripPrefix("mark ");

    if (normalized == "the target")
        normalized = "target";

    if (normalized == "my target" || normalized == "mytarget" ||
        normalized == "my current target" || normalized == "current target" ||
        normalized == "target")
        return "target";

    if (normalized == "bot target" || normalized == "your target")
        return "bot target";

    if (normalized == "me" || normalized == "myself" || normalized == "self")
        return "me";

    if (normalized == "you" || normalized == "yourself" || normalized == "bot")
        return "you";

    static std::vector<std::string> const raidIcons = {
        "star", "circle", "diamond", "triangle", "moon", "square", "cross", "x", "skull"
    };
    for (std::string const& icon : raidIcons)
    {
        if (normalized == icon)
            return icon;
    }

    return trimmed;
}

static bool IsKeywordCastTargetHint(std::string const& targetHint)
{
    std::string normalized = NormalizeSpellText(TrimWhitespace(targetHint));
    if (normalized.empty())
        return false;

    static std::vector<std::string> const keywords = {
        "target", "my target", "current target", "my current target",
        "bot target", "your target",
        "me", "myself", "self",
        "you", "yourself", "bot",
        "star", "circle", "diamond", "triangle", "moon", "square", "cross", "x", "skull"
    };

    for (std::string const& keyword : keywords)
    {
        if (normalized == keyword)
            return true;
    }

    return false;
}

static bool IsDuplicateRecentCommandMessage(Player* player, ChatChannelSourceLocal source, std::string const& message)
{
    if (!player)
        return false;

    static std::mutex sRecentCommandsMutex;
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> sRecentCommands;

    std::string key = std::to_string(player->GetGUID().GetRawValue()) + ":" + std::to_string(static_cast<uint32>(source)) + ":" + ToLowerAscii(TrimWhitespace(message));
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(sRecentCommandsMutex);

    if (sRecentCommands.size() > 256)
    {
        for (auto it = sRecentCommands.begin(); it != sRecentCommands.end();)
        {
            auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
            if (ageMs > 5000)
                it = sRecentCommands.erase(it);
            else
                ++it;
        }
    }

    auto existing = sRecentCommands.find(key);
    if (existing != sRecentCommands.end())
    {
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - existing->second).count();
        if (elapsedMs <= 1500)
            return true;
    }

    sRecentCommands[key] = now;
    return false;
}

struct CastApproachState
{
    uint32 spellId = 0;
    ObjectGuid targetGuid = ObjectGuid::Empty;
    std::chrono::steady_clock::time_point expiresAt{};
};

static std::mutex sCastApproachMutex;
static std::unordered_map<uint64, CastApproachState> sCastApproachByBot;

static bool TryBeginCastApproach(ObjectGuid botGuid, uint32 spellId, ObjectGuid targetGuid)
{
    if (botGuid.IsEmpty() || !spellId || targetGuid.IsEmpty())
        return false;

    auto now = std::chrono::steady_clock::now();
    uint64 botKey = botGuid.GetRawValue();

    std::lock_guard<std::mutex> lock(sCastApproachMutex);

    auto it = sCastApproachByBot.find(botKey);
    if (it != sCastApproachByBot.end())
    {
        bool notExpired = now < it->second.expiresAt;
        bool sameFlow = (it->second.spellId == spellId && it->second.targetGuid == targetGuid);
        if (notExpired && sameFlow)
            return false;
    }

    sCastApproachByBot[botKey] = { spellId, targetGuid, now + std::chrono::seconds(30) };
    return true;
}

static void EndCastApproach(ObjectGuid botGuid)
{
    if (botGuid.IsEmpty())
        return;

    std::lock_guard<std::mutex> lock(sCastApproachMutex);
    sCastApproachByBot.erase(botGuid.GetRawValue());
}

static uint32 ComputeLevenshteinDistance(std::string const& first, std::string const& second)
{
    if (first == second)
        return 0;

    if (first.empty())
        return static_cast<uint32>(second.size());

    if (second.empty())
        return static_cast<uint32>(first.size());

    std::vector<uint32> previous(second.size() + 1);
    std::vector<uint32> current(second.size() + 1);

    for (size_t col = 0; col <= second.size(); ++col)
        previous[col] = static_cast<uint32>(col);

    for (size_t row = 1; row <= first.size(); ++row)
    {
        current[0] = static_cast<uint32>(row);

        for (size_t col = 1; col <= second.size(); ++col)
        {
            uint32 substitutionCost = first[row - 1] == second[col - 1] ? 0u : 1u;
            uint32 deletion = previous[col] + 1;
            uint32 insertion = current[col - 1] + 1;
            uint32 substitution = previous[col - 1] + substitutionCost;
            current[col] = std::min({ deletion, insertion, substitution });
        }

        previous.swap(current);
    }

    return previous.back();
}

static std::string ResolveBotSpellName(Player* bot, std::string const& requestedSpell)
{
    if (!bot || requestedSpell.empty())
        return "";

    std::string requestedNormalized = NormalizeSpellText(requestedSpell);
    if (requestedNormalized.empty())
        return "";

    std::string requestedCompact;
    requestedCompact.reserve(requestedNormalized.size());
    for (char const c : requestedNormalized)
    {
        if (c != ' ')
            requestedCompact.push_back(c);
    }

    std::string exactMatch;
    std::string containsMatch;
    std::string fuzzyMatch;
    uint32 fuzzyDistance = 9999;

    for (auto const& spellPair : bot->GetSpellMap())
    {
        uint32 spellId = spellPair.first;
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        char const* spellName = spellInfo->SpellName[0];
        if (!spellName || !*spellName)
            continue;

        std::string candidate = spellName;
        std::string candidateNormalized = NormalizeSpellText(candidate);
        if (candidateNormalized.empty())
            continue;

        std::string candidateCompact;
        candidateCompact.reserve(candidateNormalized.size());
        for (char const c : candidateNormalized)
        {
            if (c != ' ')
                candidateCompact.push_back(c);
        }

        if (candidateNormalized == requestedNormalized)
        {
            exactMatch = candidate;
            break;
        }

        if (candidateNormalized.find(requestedNormalized) != std::string::npos && containsMatch.empty())
            containsMatch = candidate;

        uint32 distance = ComputeLevenshteinDistance(candidateCompact, requestedCompact);
        if (distance < fuzzyDistance)
        {
            fuzzyDistance = distance;
            fuzzyMatch = candidate;
        }
    }

    if (!exactMatch.empty())
        return exactMatch;

    if (!containsMatch.empty())
        return containsMatch;

    if (!fuzzyMatch.empty() && fuzzyDistance <= 2)
        return fuzzyMatch;

    return "";
}

static std::string GenerateUnknownSpellPrompt(Player* bot, Player* player, std::string const& playerMessage, std::string const& requestedSpell)
{
    if (!bot || !player)
        return "";

    return SafeFormat(
        "You are {} in World of Warcraft. "
        "The player {} asked you: \"{}\". "
        "They asked for spell \"{}\", but you do not know that spell. "
        "Reply naturally in one short sentence, in-character, politely saying you don't know that spell and asking for a different spell request. "
        "Do not mention teaching or learning from the player. "
        "Do not use markdown.",
        bot->GetName(), player->GetName(), playerMessage, requestedSpell.empty() ? "that spell" : requestedSpell);
}

static void SendNaturalCommandReply(Player* bot, Player* player, ChatChannelSourceLocal sourceLocal, std::string const& response)
{
    if (!bot || !player || response.empty())
        return;

    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    if (!botAI)
        return;

    switch (sourceLocal)
    {
        case SRC_WHISPER_LOCAL:
            botAI->Whisper(response, player->GetName());
            break;
        case SRC_PARTY_LOCAL:
            botAI->SayToParty(response);
            break;
        default:
            botAI->Say(response);
            break;
    }
}
static void SendNaturalCommandWhisper(Player* bot, Player* player, std::string const& response)
{
    if (!bot || !player || response.empty())
        return;

    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    if (!botAI)
        return;

    botAI->Whisper(response, player->GetName());
}

static void QueueUnknownSpellNaturalReply(ObjectGuid botGuid, ObjectGuid playerGuid,
    ChatChannelSourceLocal sourceLocal, std::string const& playerMessage, std::string const& requestedSpell)
{
    std::thread([botGuid, playerGuid, sourceLocal, playerMessage, requestedSpell]()
    {
        Player* bot = ObjectAccessor::FindPlayer(botGuid);
        Player* player = ObjectAccessor::FindPlayer(playerGuid);
        if (!bot || !player)
            return;

        std::string prompt = GenerateUnknownSpellPrompt(bot, player, playerMessage, requestedSpell);
        std::string response;

        try
        {
            auto future = SubmitQuery(prompt);
            if (future.valid())
                response = TrimWhitespace(future.get());
        }
        catch (const std::exception&)
        {
        }

        if (response.empty())
        {
            static std::vector<std::string> const fallbacks = {
                "I don't know that spell. Ask me for another spell and I'll try.",
                "That spell isn't in my spellbook right now. Try a different spell.",
                "I can't cast that one. Give me another spell name."
            };
            response = fallbacks[urand(0, fallbacks.size() - 1)];
        }

        bot = ObjectAccessor::FindPlayer(botGuid);
        player = ObjectAccessor::FindPlayer(playerGuid);
        if (!bot || !player)
            return;

        SendNaturalCommandReply(bot, player, sourceLocal, response);
        AppendBotConversation(botGuid.GetRawValue(), playerGuid.GetRawValue(), playerMessage, response);

        if (g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] CAST_UNKNOWN_SPELL_REPLY_SENT: bot='{}' requested='{}' reply='{}'",
                bot->GetName(), requestedSpell, response);
        }
    }).detach();
}

static void SendCastExecutionFeedback(Player* bot, Player* player, ChatChannelSourceLocal sourceLocal,
    uint32 spellId, Unit* target)
{
    if (!bot || !player || !spellId)
        return;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return;

    std::ostringstream msg;
    msg << "Casting " << ChatHelper::FormatSpell(spellInfo) << " on ";
    if (!target || target == bot)
        msg << "self";
    else
        msg << target->GetName();

    SendNaturalCommandReply(bot, player, sourceLocal, msg.str());
}

static uint32 ResolveBotSpellId(Player* bot, std::string const& requestedSpell)
{
    if (!bot || requestedSpell.empty())
        return 0;

    std::string resolvedSpellName = ResolveBotSpellName(bot, requestedSpell);
    if (resolvedSpellName.empty())
        return 0;

    std::string resolvedNormalized = NormalizeSpellText(resolvedSpellName);
    if (resolvedNormalized.empty())
        return 0;

    for (auto const& spellPair : bot->GetSpellMap())
    {
        uint32 spellId = spellPair.first;
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        char const* spellName = spellInfo->SpellName[0];
        if (!spellName || !*spellName)
            continue;

        std::string candidateNormalized = NormalizeSpellText(spellName);
        if (candidateNormalized == resolvedNormalized)
            return spellId;
    }

    return 0;
}

static ParsedNaturalCommand ParseCommandFromLLM(const std::string& llmResponse)
{
    ParsedNaturalCommand parsed;

    std::string lower = llmResponse;
    for (char& c : lower) {
        c = std::tolower(static_cast<unsigned char>(c));
    }

    auto parseCastPayload = [](std::string const& payload, ParsedNaturalCommand& output)
    {
        std::string castPayload = TrimWhitespace(payload);
        if (castPayload.empty())
            return;

        size_t separatorPos = castPayload.find('|');
        if (separatorPos != std::string::npos)
        {
            output.argument = TrimWhitespace(castPayload.substr(0, separatorPos));
            output.castTarget = NormalizeCommandTargetHint(castPayload.substr(separatorPos + 1));
        }
        else
        {
            std::string loweredPayload = castPayload;
            for (char& c : loweredPayload)
                c = std::tolower(static_cast<unsigned char>(c));

            size_t onPos = loweredPayload.rfind(" on ");
            size_t toPos = loweredPayload.rfind(" to ");
            size_t atPos = loweredPayload.rfind(" at ");
            size_t separatorPos = std::string::npos;
            if (onPos != std::string::npos)
                separatorPos = onPos;
            if (toPos != std::string::npos && (separatorPos == std::string::npos || toPos > separatorPos))
                separatorPos = toPos;
            if (atPos != std::string::npos && (separatorPos == std::string::npos || atPos > separatorPos))
                separatorPos = atPos;

            if (separatorPos != std::string::npos)
            {
                size_t targetStart = separatorPos + 4;
                std::string potentialSpell = TrimWhitespace(castPayload.substr(0, separatorPos));
                std::string potentialTarget = TrimWhitespace(castPayload.substr(targetStart));

                if (!potentialSpell.empty() && !potentialTarget.empty())
                {
                    output.argument = potentialSpell;
                    output.castTarget = NormalizeCommandTargetHint(potentialTarget);
                    return;
                }
            }

            output.argument = castPayload;
        }
    };

    auto parseAttackPayload = [](std::string const& payload, ParsedNaturalCommand& output)
    {
        std::string attackPayload = NormalizeCommandTargetHint(payload);
        if (attackPayload.empty())
            return;

        std::string normalized = NormalizeSpellText(attackPayload);
        if (normalized == "target")
            return;

        output.attackTarget = attackPayload;
    };

    std::string response = TrimWhitespace(llmResponse);
    std::string responseLower = TrimWhitespace(lower);

    if (responseLower.rfind("cast:", 0) == 0)
    {
        parsed.command = NaturalCommand::CAST;
        parseCastPayload(response.substr(5), parsed);
        return parsed;
    }

    if (responseLower.rfind("cast ", 0) == 0)
    {
        parsed.command = NaturalCommand::CAST;
        parseCastPayload(response.substr(5), parsed);
        return parsed;
    }

    if (responseLower.rfind("attack:", 0) == 0)
    {
        parsed.command = NaturalCommand::ATTACK;
        parseAttackPayload(response.substr(7), parsed);
        return parsed;
    }

    if (responseLower.rfind("attack ", 0) == 0)
    {
        parsed.command = NaturalCommand::ATTACK;
        parseAttackPayload(response.substr(7), parsed);
        return parsed;
    }
    
    // Simple parsing - look for command keywords in the LLM response
    if (ContainsWholeWordCaseInsensitive(response, "follow")) parsed.command = NaturalCommand::FOLLOW;
    else if (ContainsWholeWordCaseInsensitive(response, "stay")) parsed.command = NaturalCommand::STAY;
    else if (ContainsWholeWordCaseInsensitive(response, "attack") ||
             ContainsWholeWordCaseInsensitive(response, "pull") ||
             ContainsWholeWordCaseInsensitive(response, "engage")) parsed.command = NaturalCommand::ATTACK;
    else if (ContainsWholeWordCaseInsensitive(response, "flee") || ContainsWholeWordCaseInsensitive(response, "retreat")) parsed.command = NaturalCommand::FLEE;
    else if (ContainsWholeWordCaseInsensitive(response, "avoid_aoe_off") ||
             ContainsWholeWordCaseInsensitive(response, "disable avoid aoe") ||
             ContainsWholeWordCaseInsensitive(response, "stop avoiding aoe") ||
             ContainsWholeWordCaseInsensitive(response, "normal aoe")) parsed.command = NaturalCommand::AVOID_AOE_OFF;
    else if ((ContainsWholeWordCaseInsensitive(response, "avoid") &&
              (ContainsWholeWordCaseInsensitive(response, "aoe") || ContainsWholeWordCaseInsensitive(response, "area of effect"))) ||
             ContainsWholeWordCaseInsensitive(response, "avoid_aoe") ||
             ContainsWholeWordCaseInsensitive(response, "avoid aoe")) parsed.command = NaturalCommand::AVOID_AOE;
    else if (ContainsWholeWordCaseInsensitive(response, "trade")) parsed.command = NaturalCommand::TRADE;
    else if (ContainsWholeWordCaseInsensitive(response, "cast")) parsed.command = NaturalCommand::CAST;
    
    return parsed;
}

static std::string GenerateCommandExtractionPrompt(const std::string& playerMessage)
{
    return SafeFormat(
        "The player just said: \"{}\"\n\n"
        "Based on this message, determine what action command the player is asking the bot to perform. "
        "Available commands are: FOLLOW, STAY, ATTACK (to engage/pull an enemy, usually the player's selected target), FLEE (retreat/run to leader), AVOID_AOE (enable avoid aoe movement strategy), AVOID_AOE_OFF (disable avoid aoe movement strategy), TRADE (open trade), CAST (to use magic/abilities), or NONE (if no command is being given).\n\n"
        "If it is a cast request, respond exactly as CAST:<spell name>|<target>. "
        "Use target values like me, you, target, bot target, moon, skull, star, or a character/creature name. "
        "If no specific target is requested, use target. "
        "Example: CAST:Flash Heal|me or CAST:Fireball|target. "
        "If it is an attack request with an explicit target, respond as ATTACK:<target>. "
        "Example: ATTACK:skull or ATTACK:moon. If no explicit target is provided, respond ATTACK. "
        "Map phrases like 'pull' or 'engage' to ATTACK. "
        "Map phrases like 'normal aoe' or 'stop avoiding aoe' to AVOID_AOE_OFF. "
        "For other commands, respond with only FOLLOW, STAY, ATTACK, FLEE, AVOID_AOE, AVOID_AOE_OFF, TRADE, or NONE. Do not explain.",
        playerMessage
    );
}

static std::string InferCastTargetHintFromMessage(std::string const& playerMessage)
{
    std::string normalized = NormalizeSpellText(playerMessage);
    if (normalized.empty())
        return "";

    std::string padded = " " + normalized + " ";
    auto findPhrase = [&padded](std::string const& phrase) -> bool
    {
        return padded.find(" " + phrase + " ") != std::string::npos;
    };

    if (findPhrase("moon")) return "moon";
    if (findPhrase("skull")) return "skull";
    if (findPhrase("star")) return "star";
    if (findPhrase("cross") || findPhrase("x")) return "cross";
    if (findPhrase("square")) return "square";
    if (findPhrase("triangle")) return "triangle";
    if (findPhrase("diamond")) return "diamond";
    if (findPhrase("circle")) return "circle";

    if (findPhrase("me") || findPhrase("myself") || findPhrase("self")) return "me";
    if (findPhrase("you") || findPhrase("yourself") || findPhrase("bot")) return "you";
    if (findPhrase("my target") || findPhrase("current target") || findPhrase("target")) return "target";

    return "";
}

struct PendingNaturalCommandResult
{
    ObjectGuid senderGuid;
    ChatChannelSourceLocal sourceLocal = SRC_UNDEFINED_LOCAL;
    std::string playerMessage;
    uint32 channelId = 0;
    std::string channelName;
    std::vector<ObjectGuid> finalCandidateGuids;
    std::vector<ObjectGuid> commandTargetGuids;
    ParsedNaturalCommand parsedCommand;
    NaturalCommand detectedCmd = NaturalCommand::NONE;
};

static std::mutex sPendingNaturalCommandsMutex;
static std::vector<PendingNaturalCommandResult> sPendingNaturalCommands;

static void EnqueuePendingNaturalCommandResult(PendingNaturalCommandResult&& result)
{
    std::lock_guard<std::mutex> lock(sPendingNaturalCommandsMutex);
    sPendingNaturalCommands.push_back(std::move(result));
}

void ProcessPendingNaturalCommandResults()
{
    std::vector<PendingNaturalCommandResult> pending;
    {
        std::lock_guard<std::mutex> lock(sPendingNaturalCommandsMutex);
        if (sPendingNaturalCommands.empty())
            return;

        pending.swap(sPendingNaturalCommands);
    }

    for (PendingNaturalCommandResult& item : pending)
    {
        Player* senderPtr = ObjectAccessor::FindPlayer(item.senderGuid);
        if (!senderPtr)
            continue;

        std::vector<Player*> commandTargetPtrs;
        commandTargetPtrs.reserve(item.commandTargetGuids.size());
        for (ObjectGuid const& guid : item.commandTargetGuids)
        {
            Player* bot = ObjectAccessor::FindPlayer(guid);
            if (bot)
                commandTargetPtrs.push_back(bot);
        }

        if (item.detectedCmd != NaturalCommand::NONE)
        {
            if (g_DebugEnabled)
                LOG_INFO("server.loading", "[Ollama Chat] EXECUTING_COMMAND(main-thread): bots_count={}, parser=llm", commandTargetPtrs.size());

            uint32_t executedCount = 0;
            for (Player* bot : commandTargetPtrs)
            {
                if (!bot)
                    continue;

                std::string castTargetHint = item.parsedCommand.castTarget;
                std::string castTargetSource = "llm";

                if (item.detectedCmd == NaturalCommand::CAST)
                {
                    std::string resolvedSpellName = ResolveBotSpellName(bot, item.parsedCommand.argument);
                    if (resolvedSpellName.empty())
                    {
                        QueueUnknownSpellNaturalReply(bot->GetGUID(), item.senderGuid, item.sourceLocal,
                            item.playerMessage, item.parsedCommand.argument);
                        executedCount++;
                        if (g_DebugEnabled)
                        {
                            LOG_INFO("server.loading", "[Ollama Chat] CAST_UNKNOWN_SPELL_REPLY_QUEUED(main-thread): bot='{}' requested='{}'",
                                bot->GetName(), item.parsedCommand.argument);
                        }
                        continue;
                    }
                }

                if (item.detectedCmd == NaturalCommand::CAST)
                {
                    std::string normalizedMsg = NormalizeSpellText(item.playerMessage);
                    std::string normalizedHint = NormalizeSpellText(castTargetHint);
                    if (normalizedHint == "me" &&
                        (normalizedMsg.find(" yourself") != std::string::npos || normalizedMsg.find("yourself ") != std::string::npos))
                    {
                        castTargetHint = "you";
                        castTargetSource = "llm_corrected_from_message(yourself->you)";
                    }
                }

                if (item.detectedCmd == NaturalCommand::CAST && castTargetHint.empty())
                {
                    castTargetHint = InferCastTargetHintFromMessage(item.playerMessage);
                    if (castTargetHint.empty())
                    {
                        castTargetHint = "target";
                        castTargetSource = "default(player-target)";
                    }
                    else
                    {
                        castTargetSource = "fallback_from_player_message";
                    }
                }

                if (g_DebugEnabled && item.detectedCmd == NaturalCommand::CAST)
                {
                    LOG_INFO("server.loading", "[Ollama Chat] CAST_EXECUTE_REQUEST(main-thread): bot='{}' spell='{}' target_hint='{}' target_source='{}'",
                        bot->GetName(), item.parsedCommand.argument,
                        castTargetHint.empty() ? "(empty)" : castTargetHint,
                        castTargetSource);
                }

                senderPtr = ObjectAccessor::FindPlayer(item.senderGuid);
                if (!senderPtr)
                    continue;

                if (ExecutePlayerbotCommand(bot, senderPtr, item.detectedCmd, item.parsedCommand.argument,
                        castTargetHint, item.parsedCommand.attackTarget, item.sourceLocal))
                    executedCount++;
            }

            if (g_DebugEnabled)
                LOG_INFO("server.loading", "[Ollama Chat] COMMAND_EXECUTED(main-thread): bots={}", executedCount);

            if (executedCount > 0)
                continue;

            if (g_DebugEnabled)
                LOG_INFO("server.loading", "[Ollama Chat] COMMAND_EXECUTION_EMPTY(main-thread): continuing_to_chat");
        }

        if (g_DebugEnabled)
            LOG_INFO("server.loading", "[Ollama Chat] NO_COMMAND_DETECTED(main-thread): continuing_to_chat");

        QueueBotRepliesForCandidates(item.finalCandidateGuids, item.senderGuid, item.playerMessage,
            item.sourceLocal, item.channelId, item.channelName);
    }
}

static void QueueSingleBotReply(ObjectGuid botGuid, ObjectGuid senderGuid, std::string const& msg,
    ChatChannelSourceLocal sourceLocal, uint32 channelId, std::string const& channelName)
{
    std::thread([botGuid, senderGuid, msg, sourceLocal, channelId, channelName]() {
        try {
            Player* botPtr = ObjectAccessor::FindPlayer(botGuid);
            Player* senderPtr = ObjectAccessor::FindPlayer(senderGuid);
            if (!botPtr || !senderPtr)
                return;

            std::string prompt = GenerateBotPrompt(botPtr, msg, senderPtr);

            auto responseFuture = SubmitQuery(prompt);
            if (!responseFuture.valid())
                return;

            std::string response = responseFuture.get();

            botPtr = ObjectAccessor::FindPlayer(botGuid);
            senderPtr = ObjectAccessor::FindPlayer(senderGuid);
            if (!botPtr)
            {
                if (g_DebugEnabled)
                    LOG_ERROR("server.loading", "[Ollama Chat] Failed to reacquire bot from GUID {}", botGuid.GetRawValue());
                return;
            }
            if (!senderPtr)
            {
                if (g_DebugEnabled)
                    LOG_ERROR("server.loading", "[Ollama Chat] Failed to reacquire sender from GUID {}", senderGuid.GetRawValue());
                return;
            }
            if (response.empty())
            {
                if (g_DebugEnabled)
                    LOG_INFO("server.loading", "[OllamaChat] Bot {} skipped reply due to API error", botPtr->GetName());
                return;
            }

            PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(botPtr);
            if (!botAI)
            {
                if (g_DebugEnabled)
                    LOG_ERROR("server.loading", "[Ollama Chat] No PlayerbotAI found for bot {}", botPtr->GetName());
                return;
            }

            if (g_EnableTypingSimulation)
            {
                uint32_t delay = g_TypingSimulationBaseDelay + (response.length() * g_TypingSimulationDelayPerChar);
                if (g_DebugEnabled)
                    LOG_INFO("server.loading", "[OllamaChat] Bot {} simulating typing delay: {}ms for {} characters",
                        botPtr->GetName(), delay, response.length());
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));

                botPtr = ObjectAccessor::FindPlayer(botGuid);
                if (!botPtr)
                    return;
                botAI = PlayerbotsMgr::instance().GetPlayerbotAI(botPtr);
                if (!botAI)
                    return;
                senderPtr = ObjectAccessor::FindPlayer(senderGuid);
                if (!senderPtr)
                    return;
            }

            if (channelId != 0 && !channelName.empty())
            {
                ChannelMgr* cMgr = ChannelMgr::forTeam(botPtr->GetTeamId());
                if (cMgr)
                {
                    Channel* targetChannel = cMgr->GetChannel(channelName, botPtr);
                    if (targetChannel)
                    {
                        if (g_DebugEnabled)
                            LOG_INFO("server.loading", "[Ollama Chat] Bot {} found channel '{}' (ID: {}), checking membership...",
                                botPtr->GetName(), channelName, targetChannel->GetChannelId());

                        if (botPtr->IsInChannel(targetChannel))
                        {
                            if (g_DebugEnabled)
                                LOG_INFO("server.loading", "[Ollama Chat] Bot {} is confirmed in channel '{}', sending message...",
                                    botPtr->GetName(), channelName);

                            targetChannel->Say(botPtr->GetGUID(), response, LANG_UNIVERSAL);
                            ProcessBotChatMessage(botPtr, response, SRC_GENERAL_LOCAL, targetChannel);
                            if (g_DebugEnabled)
                                LOG_INFO("server.loading", "[Ollama Chat] Bot {} responded in channel {}: {}",
                                    botPtr->GetName(), channelName, response);
                        }
                        else if (g_DebugEnabled)
                        {
                            LOG_ERROR("server.loading", "[Ollama Chat] Bot {} NOT in channel '{}' according to IsInChannel check - skipping reply",
                                botPtr->GetName(), channelName);
                        }
                    }
                    else if (g_DebugEnabled)
                    {
                        LOG_ERROR("server.loading", "[Ollama Chat] Bot {} cannot find channel '{}' (ID: {}) for team {} - skipping reply",
                            botPtr->GetName(), channelName, channelId, static_cast<int>(botPtr->GetTeamId()));
                    }
                }
            }
            else
            {
                switch (sourceLocal)
                {
                    case SRC_GUILD_LOCAL:
                        botAI->SayToGuild(response);
                        ProcessBotChatMessage(botPtr, response, SRC_GUILD_LOCAL, nullptr);
                        break;
                    case SRC_OFFICER_LOCAL:
                        botAI->SayToGuild(response);
                        ProcessBotChatMessage(botPtr, response, SRC_OFFICER_LOCAL, nullptr);
                        break;
                    case SRC_PARTY_LOCAL:
                        botAI->SayToParty(response);
                        ProcessBotChatMessage(botPtr, response, SRC_PARTY_LOCAL, nullptr);
                        break;
                    case SRC_RAID_LOCAL:
                        botAI->SayToRaid(response);
                        ProcessBotChatMessage(botPtr, response, SRC_RAID_LOCAL, nullptr);
                        break;
                    case SRC_SAY_LOCAL:
                    {
                        bool someoneCanHear = false;
                        if (botPtr->IsInWorld())
                        {
                            for (auto const& pair : ObjectAccessor::GetPlayers())
                            {
                                Player* nearbyPlayer = pair.second;
                                if (nearbyPlayer && nearbyPlayer != botPtr && nearbyPlayer->IsInWorld())
                                {
                                    if (botPtr->GetDistance(nearbyPlayer) <= g_SayDistance)
                                    {
                                        someoneCanHear = true;
                                        break;
                                    }
                                }
                            }
                        }

                        if (someoneCanHear)
                        {
                            botAI->Say(response);
                            ProcessBotChatMessage(botPtr, response, SRC_SAY_LOCAL, nullptr);
                        }
                        else if (g_DebugEnabled)
                        {
                            LOG_INFO("server.loading", "[Ollama Chat] Bot {} skipping Say reply - no one within {} yards to hear it",
                                botPtr->GetName(), g_SayDistance);
                        }
                        break;
                    }
                    case SRC_YELL_LOCAL:
                    {
                        bool someoneCanHear = false;
                        if (botPtr->IsInWorld())
                        {
                            for (auto const& pair : ObjectAccessor::GetPlayers())
                            {
                                Player* nearbyPlayer = pair.second;
                                if (nearbyPlayer && nearbyPlayer != botPtr && nearbyPlayer->IsInWorld())
                                {
                                    if (botPtr->GetDistance(nearbyPlayer) <= g_YellDistance)
                                    {
                                        someoneCanHear = true;
                                        break;
                                    }
                                }
                            }
                        }

                        if (someoneCanHear)
                        {
                            botAI->Yell(response);
                            ProcessBotChatMessage(botPtr, response, SRC_YELL_LOCAL, nullptr);
                        }
                        else if (g_DebugEnabled)
                        {
                            LOG_INFO("server.loading", "[Ollama Chat] Bot {} skipping Yell reply - no one within {} yards to hear it",
                                botPtr->GetName(), g_YellDistance);
                        }
                        break;
                    }
                    case SRC_WHISPER_LOCAL:
                    {
                        Player* originalSender = ObjectAccessor::FindPlayer(senderGuid);
                        if (originalSender)
                        {
                            if (g_DebugEnabled)
                                LOG_INFO("server.loading", "[Ollama Chat] Bot {} whispering response '{}' to {}",
                                    botPtr->GetName(), response, originalSender->GetName());
                            botAI->Whisper(response, originalSender->GetName());
                        }
                        else if (g_DebugEnabled)
                        {
                            LOG_ERROR("server.loading", "[Ollama Chat] Cannot whisper response - original sender not found for GUID {}", senderGuid.GetRawValue());
                        }
                        break;
                    }
                    default:
                        botAI->Say(response);
                        ProcessBotChatMessage(botPtr, response, SRC_SAY_LOCAL, nullptr);
                        break;
                }
            }

            UpdateBotPlayerSentiment(botPtr, senderPtr, msg);

            AppendBotConversation(botGuid.GetRawValue(), senderGuid.GetRawValue(), msg, response);
            if (botPtr->IsInWorld() && senderPtr->IsInWorld())
            {
                float respDistance = senderPtr->GetDistance(botPtr);
                if (g_DebugEnabled)
                    LOG_INFO("server.loading", "[Ollama Chat] Bot {} (distance: {}) responded: {}", botPtr->GetName(), respDistance, response);
            }
            else if (g_DebugEnabled)
            {
                LOG_INFO("server.loading", "[Ollama Chat] Bot {} responded: {} (distance not calculated - players not in world)", botPtr->GetName(), response);
            }
        }
        catch (const std::exception& ex)
        {
            if (g_DebugEnabled)
                LOG_ERROR("server.loading", "[Ollama Chat] Exception in bot response thread: {}", ex.what());
        }
    }).detach();
}

static void QueueBotRepliesForCandidates(std::vector<ObjectGuid> const& botGuids,
    ObjectGuid senderGuid, std::string const& msg, ChatChannelSourceLocal sourceLocal,
    uint32 channelId, std::string const& channelName)
{
    for (ObjectGuid const& botGuid : botGuids)
        QueueSingleBotReply(botGuid, senderGuid, msg, sourceLocal, channelId, channelName);
}

static bool IsTargetWithinSpellRange(Player* bot, Unit* target, uint32 spellId)
{
    if (!bot || !target || !spellId)
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return false;

    float maxRange = bot->GetSpellMaxRangeForTarget(target, spellInfo);
    if (maxRange > 0.0f && !bot->IsWithinDistInMap(target, maxRange))
        return false;

    float minRange = bot->GetSpellMinRangeForTarget(target, spellInfo);
    if (minRange > 0.0f && bot->GetDistance(target) < minRange)
        return false;

    return true;
}

static float GetSpellApproachDistance(Player* bot, Unit* target, uint32 spellId)
{
    if (!bot || !target || !spellId)
        return 0.0f;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return 0.0f;

    float maxRange = bot->GetSpellMaxRangeForTarget(target, spellInfo);
    if (maxRange <= 0.0f)
        return 0.0f;

    float minRange = bot->GetSpellMinRangeForTarget(target, spellInfo);
    float desired = std::max(0.0f, maxRange - 1.5f);
    if (minRange > 0.0f)
        desired = std::max(desired, minRange + 0.5f);

    return desired;
}

static bool StartSpellApproachLikeAttackPathing(Player* bot, Unit* target, uint32 spellId)
{
    if (!bot || !target || !spellId)
        return false;

    if (bot->GetMapId() != target->GetMapId())
        return false;

    float tx = target->GetPositionX();
    float ty = target->GetPositionY();
    float tz = target->GetPositionZ();
    float targetOrientation = target->GetOrientation();

    float deltaAngle = Position::NormalizeOrientation(targetOrientation - target->GetAngle(bot));
    if (deltaAngle > M_PI)
        deltaAngle -= 2.0f * M_PI;

    bool behind = std::fabs(deltaAngle) > M_PI_2;
    if (target->HasUnitMovementFlag(MOVEMENTFLAG_FORWARD) && behind)
    {
        float predictDis = std::min(3.0f, target->GetObjectSize() * 2.0f);
        tx += std::cos(targetOrientation) * predictDis;
        ty += std::sin(targetOrientation) * predictDis;

        if (!target->GetMap()->CheckCollisionAndGetValidCoords(target, target->GetPositionX(), target->GetPositionY(),
            target->GetPositionZ(), tx, ty, tz))
        {
            tx = target->GetPositionX();
            ty = target->GetPositionY();
            tz = target->GetPositionZ();
        }
    }

    float desiredDistance = GetSpellApproachDistance(bot, target, spellId);
    desiredDistance += bot->GetCombatReach() + target->GetCombatReach();

    if (bot->GetExactDist(tx, ty, tz) <= desiredDistance)
        return false;

    PathGenerator path(bot);
    path.CalculatePath(tx, ty, tz, false);

    PathType pathType = path.GetPathType();
    int typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_SHORTCUT;
    if (!(pathType & typeOk) || path.GetPath().empty())
        return false;

    float shortenTo = desiredDistance;
    float disToGo = bot->GetExactDist(tx, ty, tz) - desiredDistance;
    if (disToGo >= 6.0f)
        shortenTo = disToGo / 2.0f + desiredDistance;

    path.ShortenPathUntilDist(G3D::Vector3(tx, ty, tz), shortenTo);
    if (path.GetPath().empty())
        return false;

    auto endPos = path.GetPath().back();

    bot->AttackStop();
    bot->CombatStop(false);
    bot->GetMotionMaster()->Clear(false);
    bot->GetMotionMaster()->MovePoint(0, endPos.x, endPos.y, endPos.z,
        FORCED_MOVEMENT_NONE, 0.0f, 0.0f, true, true);
    return true;
}

static std::string DescribeUnitForDebug(Unit* unit)
{
    if (!unit)
        return "none";

    if (unit->ToPlayer())
        return SafeFormat("{} (player)", unit->GetName());

    if (unit->ToCreature())
        return SafeFormat("{} (creature:{})", unit->GetName(), unit->GetEntry());

    return unit->GetName();
}

static Unit* ResolveCastTargetUnit(Player* bot, Player* commandInitiator, std::string const& targetText, std::string* resolutionSource = nullptr)
{
    if (!bot || !commandInitiator)
        return nullptr;

    auto setResolution = [&resolutionSource](std::string const& value)
    {
        if (resolutionSource)
            *resolutionSource = value;
    };

    Unit* selectedUnit = commandInitiator->GetSelectedUnit();
    std::string normalizedTarget = NormalizeSpellText(TrimWhitespace(targetText));

    auto resolveRaidIconTarget = [&](std::string const& iconKey) -> Unit*
    {
        if (!commandInitiator->GetGroup())
            return nullptr;

        auto hasIconWord = [&iconKey](std::string const& word)
        {
            return iconKey == word || iconKey.find(word) != std::string::npos;
        };

        int32 iconIndex = -1;
        if (hasIconWord("star")) iconIndex = 0;
        else if (hasIconWord("circle")) iconIndex = 1;
        else if (hasIconWord("diamond")) iconIndex = 2;
        else if (hasIconWord("triangle")) iconIndex = 3;
        else if (hasIconWord("moon")) iconIndex = 4;
        else if (hasIconWord("square")) iconIndex = 5;
        else if (hasIconWord("cross") || iconKey == "x") iconIndex = 6;
        else if (hasIconWord("skull")) iconIndex = 7;

        if (iconIndex < 0)
            return nullptr;

        ObjectGuid iconTargetGuid = commandInitiator->GetGroup()->GetTargetIcon(iconIndex);
        if (iconTargetGuid.IsEmpty())
            return nullptr;

        Unit* iconTarget = ObjectAccessor::GetUnit(*commandInitiator, iconTargetGuid);
        if (iconTarget)
            setResolution(SafeFormat("raid_icon:{}", iconKey));

        return iconTarget;
    };

    if (normalizedTarget.empty() ||
        normalizedTarget == "target" ||
        normalizedTarget == "my target" ||
        normalizedTarget == "current target" ||
        normalizedTarget == "player target")
    {
        setResolution("player_selected_target");
        return selectedUnit;
    }

    if (normalizedTarget == "bot target" ||
        normalizedTarget == "your target")
    {
        setResolution("bot_victim");
        return bot->GetVictim();
    }

    if (normalizedTarget == "me" ||
        normalizedTarget == "myself" ||
        normalizedTarget == "self" ||
        normalizedTarget == "player" ||
        normalizedTarget == NormalizeSpellText(commandInitiator->GetName()))
    {
        setResolution("command_initiator");
        return commandInitiator;
    }

    if (normalizedTarget == "you" ||
        normalizedTarget == "yourself" ||
        normalizedTarget == "bot" ||
        normalizedTarget == NormalizeSpellText(bot->GetName()))
    {
        setResolution("bot_self");
        return bot;
    }

    if (Unit* iconTarget = resolveRaidIconTarget(normalizedTarget))
        return iconTarget;

    auto nameMatches = [&normalizedTarget](std::string const& candidateName)
    {
        std::string normalizedCandidate = NormalizeSpellText(candidateName);
        if (normalizedCandidate.empty())
            return false;

        return normalizedCandidate == normalizedTarget ||
               normalizedCandidate.find(normalizedTarget) != std::string::npos;
    };

    Unit* partialMatch = nullptr;

    auto considerUnit = [&nameMatches, &normalizedTarget, &partialMatch](Unit* unit)
    {
        if (!unit)
            return static_cast<Unit*>(nullptr);

        if (!unit->IsAlive())
            return static_cast<Unit*>(nullptr);

        std::string normalizedCandidate = NormalizeSpellText(unit->GetName());
        if (normalizedCandidate.empty())
            return static_cast<Unit*>(nullptr);

        if (normalizedCandidate == normalizedTarget)
            return unit;

        if (!partialMatch && nameMatches(unit->GetName()))
            partialMatch = unit;

        return static_cast<Unit*>(nullptr);
    };

    if (Group* group = commandInitiator->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member->GetMap() != commandInitiator->GetMap())
                continue;

            if (Unit* exact = considerUnit(member))
            {
                setResolution("group_member_exact");
                return exact;
            }
        }
    }

    if (Unit* exact = considerUnit(selectedUnit))
    {
        setResolution("selected_target_name_match");
        return exact;
    }

    Map* map = commandInitiator->GetMap();
    if (map)
    {
        std::list<Unit*> nearbyUnits;
        Acore::AnyUnitInObjectRangeCheck unitCheck(commandInitiator, 60.0f);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> unitSearcher(commandInitiator, nearbyUnits, unitCheck);
        Cell::VisitObjects(commandInitiator, unitSearcher, 60.0f);

        for (Unit* nearbyUnit : nearbyUnits)
        {
            if (!nearbyUnit || nearbyUnit == commandInitiator)
                continue;

            if (Unit* exact = considerUnit(nearbyUnit))
            {
                setResolution("nearby_unit_exact");
                return exact;
            }
        }

        for (auto const& pair : map->GetCreatureBySpawnIdStore())
        {
            Creature* creature = pair.second;
            if (!creature)
                continue;

            if (!commandInitiator->IsWithinDistInMap(creature, 50.0f))
                continue;

            if (!commandInitiator->IsWithinLOS(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ()))
                continue;

            if (Unit* exact = considerUnit(creature))
            {
                setResolution("nearby_creature_exact");
                return exact;
            }
        }
    }

    if (partialMatch)
    {
        setResolution("partial_name_match");
        return partialMatch;
    }

    setResolution("fallback_player_selected_target");
    return selectedUnit;
}

static Unit* FindNearestCastableMatchingUnit(Player* bot, PlayerbotAI* botAI, std::string const& targetText, uint32 spellId)
{
    if (!bot || !botAI || !spellId)
        return nullptr;

    std::string normalizedTarget = NormalizeSpellText(TrimWhitespace(targetText));
    if (normalizedTarget.empty())
        return nullptr;

    std::list<Unit*> nearbyUnits;
    Acore::AnyUnitInObjectRangeCheck unitCheck(bot, 60.0f);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> unitSearcher(bot, nearbyUnits, unitCheck);
    Cell::VisitObjects(bot, unitSearcher, 60.0f);

    Unit* nearest = nullptr;
    float nearestDistance = std::numeric_limits<float>::max();

    for (Unit* candidate : nearbyUnits)
    {
        if (!candidate || candidate == bot || !candidate->IsAlive())
            continue;

        std::string normalizedCandidate = NormalizeSpellText(candidate->GetName());
        if (normalizedCandidate.empty())
            continue;

        bool nameMatches = normalizedCandidate == normalizedTarget ||
                           normalizedCandidate.find(normalizedTarget) != std::string::npos ||
                           normalizedTarget.find(normalizedCandidate) != std::string::npos;
        if (!nameMatches)
            continue;

        if (!IsTargetWithinSpellRange(bot, candidate, spellId))
            continue;

        float distance = bot->GetDistance(candidate);
        if (distance < nearestDistance)
        {
            nearestDistance = distance;
            nearest = candidate;
        }
    }

    return nearest;
}

static Unit* FindNearestMatchingUnit(Player* bot, std::string const& targetText)
{
    if (!bot)
        return nullptr;

    std::string normalizedTarget = NormalizeSpellText(TrimWhitespace(targetText));
    if (normalizedTarget.empty())
        return nullptr;

    std::list<Unit*> nearbyUnits;
    Acore::AnyUnitInObjectRangeCheck unitCheck(bot, 60.0f);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> unitSearcher(bot, nearbyUnits, unitCheck);
    Cell::VisitObjects(bot, unitSearcher, 60.0f);

    Unit* nearest = nullptr;
    float nearestDistance = std::numeric_limits<float>::max();

    for (Unit* candidate : nearbyUnits)
    {
        if (!candidate || candidate == bot || !candidate->IsAlive())
            continue;

        std::string normalizedCandidate = NormalizeSpellText(candidate->GetName());
        if (normalizedCandidate.empty())
            continue;

        bool nameMatches = normalizedCandidate == normalizedTarget ||
                           normalizedCandidate.find(normalizedTarget) != std::string::npos ||
                           normalizedTarget.find(normalizedCandidate) != std::string::npos;
        if (!nameMatches)
            continue;

        float distance = bot->GetDistance(candidate);
        if (distance < nearestDistance)
        {
            nearestDistance = distance;
            nearest = candidate;
        }
    }

    return nearest;
}

static bool ExecutePlayerbotCommand(Player* bot, Player* commandInitiator,
                                    NaturalCommand cmd,
                                    std::string const& castSpellName,
                                    std::string const& castTargetHint,
                                    std::string const& attackTargetHint,
                                    ChatChannelSourceLocal sourceLocal)
{
    if (!bot || !commandInitiator || cmd == NaturalCommand::NONE)
        return false;

    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    if (!botAI)
        return false;

    std::string command;
    bool listInventoryBeforeTrade = false;
    std::string resolvedSpellName;
    uint32 resolvedSpellId = 0;
    switch (cmd)
    {
        case NaturalCommand::FOLLOW:
            command = "follow";
            break;
        case NaturalCommand::STAY:
            command = "stay";
            break;
        case NaturalCommand::ATTACK:
            command = "attack my target";
            break;
        case NaturalCommand::FLEE:
            command = "flee";
            break;
        case NaturalCommand::AVOID_AOE:
            command = "co +avoid aoe";
            break;
        case NaturalCommand::AVOID_AOE_OFF:
            command = "co -avoid aoe";
            break;
        case NaturalCommand::TRADE:
            command = "t";
            listInventoryBeforeTrade = true;
            break;
        case NaturalCommand::CAST:
        {
            resolvedSpellName = ResolveBotSpellName(bot, castSpellName);
            if (resolvedSpellName.empty())
                return false;

            resolvedSpellId = ResolveBotSpellId(bot, castSpellName);
            if (!resolvedSpellId)
            {
                LOG_ERROR("server.loading", "[Ollama Chat] CAST_ABORT: bot='{}' spell_request='{}' resolved name '{}' but no spell id found",
                    bot->GetName(), castSpellName, resolvedSpellName);
                return false;
            }

            command = "cast " + resolvedSpellName;
            break;
        }
        default:
            return false;
    }

    Unit* previousSelectedUnit = commandInitiator->GetSelectedUnit();

    // Execute the command through the PlayerbotAI system
    if (g_DebugEnabled)
    {
        if (cmd == NaturalCommand::CAST)
        {
            LOG_INFO("server.loading", "[Ollama Chat] CAST_PREP: bot='{}' from='{}' spell_request='{}' spell_resolved='{}' target_hint='{}' player_selected_before='{}'",
                bot->GetName(), commandInitiator->GetName(), castSpellName, resolvedSpellName,
                castTargetHint.empty() ? "(empty->player target)" : castTargetHint,
                DescribeUnitForDebug(previousSelectedUnit));
        }
        else
        {
            LOG_INFO("server.loading", "[Ollama Chat] Executing natural language command '{}' for bot {} from {}",
                command, bot->GetName(), commandInitiator->GetName());
        }
    }

    ObjectGuid previousSelection = ObjectGuid::Empty;
    if (previousSelectedUnit)
    {
        Unit* selected = previousSelectedUnit;
        previousSelection = selected->GetGUID();
    }

    ObjectGuid previousBotSelection = ObjectGuid::Empty;
    if (Unit* botSelected = bot->GetSelectedUnit())
        previousBotSelection = botSelected->GetGUID();

    bool retryCastAfterApproach = false;
    ObjectGuid retryCastTargetGuid = ObjectGuid::Empty;
    std::string retryCastCommand;
    bool skipImmediateCastBecauseApproachInProgress = false;

    bool selectionTemporarilyChanged = false;
    if (cmd == NaturalCommand::ATTACK && !attackTargetHint.empty())
    {
        std::string resolutionSource;
        if (Unit* requestedTarget = ResolveCastTargetUnit(bot, commandInitiator, attackTargetHint, &resolutionSource))
        {
            commandInitiator->SetSelection(requestedTarget->GetGUID());
            bot->SetSelection(requestedTarget->GetGUID());
            selectionTemporarilyChanged = true;

            if (g_DebugEnabled)
            {
                LOG_INFO("server.loading", "[Ollama Chat] ATTACK_TARGET_RESOLVED: bot='{}' hint='{}' source='{}' resolved='{}'",
                    bot->GetName(), attackTargetHint, resolutionSource, DescribeUnitForDebug(requestedTarget));
            }
        }
        else if (g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] ATTACK_TARGET_RESOLVED: bot='{}' hint='{}' source='none' resolved='none' (keeping player selection)",
                bot->GetName(), attackTargetHint);
        }
    }
    else if (cmd == NaturalCommand::CAST && !castTargetHint.empty())
    {
        std::string resolutionSource;
        if (Unit* requestedTarget = ResolveCastTargetUnit(bot, commandInitiator, castTargetHint, &resolutionSource))
        {
            if (!requestedTarget->IsPlayer() && !IsTargetWithinSpellRange(bot, requestedTarget, resolvedSpellId))
            {
                bool allowNameMatchSearch = !IsKeywordCastTargetHint(castTargetHint);

                Unit* nearestCastable = allowNameMatchSearch
                    ? FindNearestCastableMatchingUnit(bot, botAI, castTargetHint, resolvedSpellId)
                    : nullptr;
                if (nearestCastable)
                {
                    requestedTarget = nearestCastable;
                    resolutionSource = "nearest_castable_match";
                }
                else
                {
                    Unit* nearestMatch = allowNameMatchSearch ? FindNearestMatchingUnit(bot, castTargetHint) : nullptr;
                    if (nearestMatch)
                    {
                        requestedTarget = nearestMatch;
                        resolutionSource = "nearest_name_match_uncastable_precheck";
                    }
                    else if (allowNameMatchSearch)
                    {
                        LOG_ERROR("server.loading", "[Ollama Chat] CAST_TARGET_WARNING: bot='{}' spell='{}' target_hint='{}' has no nearby matching alive unit; trying original resolved target '{}'",
                            bot->GetName(), resolvedSpellName, castTargetHint, DescribeUnitForDebug(requestedTarget));
                    }
                }
            }

            commandInitiator->SetSelection(requestedTarget->GetGUID());
            bot->SetSelection(requestedTarget->GetGUID());
            selectionTemporarilyChanged = true;

            if (requestedTarget->IsPlayer())
            {
                command = "cast " + resolvedSpellName + " on " + requestedTarget->GetName();
            }
            else
            {
                command = "cast " + ChatHelper::FormatWorldobject(requestedTarget) + " " + resolvedSpellName;
            }

            if (!IsTargetWithinSpellRange(bot, requestedTarget, resolvedSpellId))
            {
                if (TryBeginCastApproach(bot->GetGUID(), resolvedSpellId, requestedTarget->GetGUID()))
                {
                    retryCastAfterApproach = true;
                    retryCastTargetGuid = requestedTarget->GetGUID();
                    retryCastCommand = command;

                    LOG_ERROR("server.loading", "[Ollama Chat] CAST_OUT_OF_RANGE: bot='{}' spell='{}' target='{}' scheduling approach+retry",
                        bot->GetName(), resolvedSpellName, DescribeUnitForDebug(requestedTarget));
                }
                else
                {
                    skipImmediateCastBecauseApproachInProgress = true;
                    if (g_DebugEnabled)
                    {
                        LOG_INFO("server.loading", "[Ollama Chat] CAST_OUT_OF_RANGE: bot='{}' spell='{}' target='{}' approach already in progress",
                            bot->GetName(), resolvedSpellName, DescribeUnitForDebug(requestedTarget));
                    }
                }
            }

            LOG_ERROR("server.loading", "[Ollama Chat] CAST_COMMAND_BUILT: bot='{}' spell='{}' target_hint='{}' target_resolved='{}' command='{}' selection_mode='bot_selected_target'",
                bot->GetName(), resolvedSpellName, castTargetHint, DescribeUnitForDebug(requestedTarget), command);

            if (g_DebugEnabled)
            {
                LOG_INFO("server.loading", "[Ollama Chat] CAST_TARGET_RESOLVED: bot='{}' hint='{}' source='{}' resolved='{}' bot_selected_now='{}' player_selected_now='{}'",
                    bot->GetName(), castTargetHint, resolutionSource, DescribeUnitForDebug(requestedTarget),
                    DescribeUnitForDebug(bot->GetSelectedUnit()), DescribeUnitForDebug(commandInitiator->GetSelectedUnit()));
            }
        }
        else if (g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] CAST_TARGET_RESOLVED: bot='{}' hint='{}' source='none' resolved='none' (keeping player selection)",
                bot->GetName(), castTargetHint);
        }
    }
    else if (cmd == NaturalCommand::CAST)
    {
        LOG_ERROR("server.loading", "[Ollama Chat] CAST_COMMAND_BUILT: bot='{}' spell='{}' target_hint='(empty)' command='{}'",
            bot->GetName(), resolvedSpellName, command);
    }

    try {
        bool const deferSelectionRestore =
            (cmd == NaturalCommand::CAST && retryCastAfterApproach && selectionTemporarilyChanged) ||
            (cmd == NaturalCommand::ATTACK && selectionTemporarilyChanged);

        if (cmd == NaturalCommand::CAST && retryCastAfterApproach && !retryCastCommand.empty() && !retryCastTargetGuid.IsEmpty())
        {
            ObjectGuid botGuid = bot->GetGUID();
            ObjectGuid initiatorGuid = commandInitiator->GetGUID();
            bool hadFollowNonCombat = botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT);
            bool hadStayNonCombat = botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT);
            std::thread([botGuid, initiatorGuid, retryCastTargetGuid, retryCastCommand, resolvedSpellId, previousSelection, previousBotSelection,
                         sourceLocal,
                         hadFollowNonCombat, hadStayNonCombat]()
            {
                constexpr uint32_t maxAttempts = 200;
                constexpr uint32_t sleepMs = 250;
                bool hasIssuedApproach = false;

                auto restoreSelections = [&](Player* botPtr, Player* initiatorPtr)
                {
                    if (initiatorPtr)
                        initiatorPtr->SetSelection(previousSelection);
                    if (botPtr)
                        botPtr->SetSelection(previousBotSelection);
                };

                auto finish = [&, hadFollowNonCombat, hadStayNonCombat](Player* botPtr, Player* initiatorPtr)
                {
                    if (botPtr)
                    {
                        if (PlayerbotAI* finishBotAI = PlayerbotsMgr::instance().GetPlayerbotAI(botPtr))
                        {
                            std::string restoreCmd;
                            if (hadFollowNonCombat)
                                restoreCmd += "+follow";
                            if (hadStayNonCombat)
                            {
                                if (!restoreCmd.empty())
                                    restoreCmd += ",";
                                restoreCmd += "+stay";
                            }

                            if (!restoreCmd.empty())
                                finishBotAI->ChangeStrategy(restoreCmd, BOT_STATE_NON_COMBAT);
                        }
                    }

                    restoreSelections(botPtr, initiatorPtr);
                    EndCastApproach(botGuid);
                };

                bool nonCombatStateTemporarilyRelaxed = false;

                for (uint32_t attempt = 0; attempt < maxAttempts; ++attempt)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));

                    Player* botPtr = ObjectAccessor::FindPlayer(botGuid);
                    Player* initiatorPtr = ObjectAccessor::FindPlayer(initiatorGuid);
                    if (!botPtr || !initiatorPtr || !botPtr->IsInWorld())
                    {
                        finish(botPtr, initiatorPtr);
                        return;
                    }

                    PlayerbotAI* botAIPtr = PlayerbotsMgr::instance().GetPlayerbotAI(botPtr);
                    if (!botAIPtr)
                    {
                        finish(botPtr, initiatorPtr);
                        return;
                    }

                    if (!nonCombatStateTemporarilyRelaxed)
                    {
                        botAIPtr->ChangeStrategy("-follow,-stay", BOT_STATE_NON_COMBAT);
                        nonCombatStateTemporarilyRelaxed = true;
                    }

                    Unit* targetPtr = ObjectAccessor::GetUnit(*botPtr, retryCastTargetGuid);
                    if (!targetPtr || !targetPtr->IsAlive())
                    {
                        finish(botPtr, initiatorPtr);
                        return;
                    }

                    if (!IsTargetWithinSpellRange(botPtr, targetPtr, resolvedSpellId))
                    {
                        bool shouldNudgeApproach = !hasIssuedApproach || (!botPtr->isMoving() && (attempt % 8 == 0));
                        if (shouldNudgeApproach)
                        {
                            bool moved = StartSpellApproachLikeAttackPathing(botPtr, targetPtr, resolvedSpellId);

                            if (!hasIssuedApproach)
                            {
                                LOG_ERROR("server.loading", "[Ollama Chat] CAST_APPROACH_START: bot='{}' target='{}' mode='reach_combat_pathing' moved={}",
                                    botPtr->GetName(), DescribeUnitForDebug(targetPtr), moved ? 1 : 0);
                            }

                            hasIssuedApproach = hasIssuedApproach || moved;
                        }

                        continue;
                    }

                    constexpr uint32_t castAttempts = 4;
                    for (uint32_t castAttempt = 0; castAttempt < castAttempts; ++castAttempt)
                    {
                        botPtr->AttackStop();
                        botPtr->CombatStop(false);
                        botPtr->StopMoving();
                        botPtr->SetFacingToObject(targetPtr);
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));

                        initiatorPtr->SetSelection(retryCastTargetGuid);
                        botPtr->SetSelection(retryCastTargetGuid);
                        LOG_ERROR("server.loading", "[Ollama Chat] CAST_RETRY_EXECUTE: bot='{}' command='{}' attempt={}",
                            botPtr->GetName(), retryCastCommand, castAttempt + 1);

                        bool castOk = botAIPtr->CastSpell(resolvedSpellId, targetPtr, nullptr);
                        if (castOk)
                        {
                            SendCastExecutionFeedback(botPtr, initiatorPtr, sourceLocal, resolvedSpellId, targetPtr);

                            if (g_DebugEnabled)
                                LOG_INFO("server.loading", "[Ollama Chat] CAST_RETRY_SUCCESS: bot='{}' spellId={} target='{}'",
                                    botPtr->GetName(), resolvedSpellId, DescribeUnitForDebug(targetPtr));
                            break;
                        }

                        std::this_thread::sleep_for(std::chrono::milliseconds(300));

                        if (!targetPtr->IsAlive())
                            break;
                    }

                    finish(botPtr, initiatorPtr);
                    return;
                }

                Player* botPtrTimeout = ObjectAccessor::FindPlayer(botGuid);
                LOG_ERROR("server.loading", "[Ollama Chat] CAST_RETRY_TIMEOUT: bot='{}' command='{}'",
                    botPtrTimeout ? botPtrTimeout->GetName() : "unknown", retryCastCommand);

                Player* botPtr = ObjectAccessor::FindPlayer(botGuid);
                Player* initiatorPtr = ObjectAccessor::FindPlayer(initiatorGuid);
                finish(botPtr, initiatorPtr);
            }).detach();
        }
        else if (cmd == NaturalCommand::CAST && skipImmediateCastBecauseApproachInProgress)
        {
            if (selectionTemporarilyChanged)
            {
                commandInitiator->SetSelection(previousSelection);
                bot->SetSelection(previousBotSelection);
            }
            return true;
        }
        else
        {
            // Send command via the PlayerbotAI HandleCommand method
            // The command will be interpreted as a whispered command from the initiator
            if (cmd == NaturalCommand::CAST)
            {
                Unit* castTarget = bot->GetSelectedUnit();
                if (!castTarget)
                    castTarget = commandInitiator->GetSelectedUnit();
                if (!castTarget)
                    castTarget = bot;

                if (!botAI->CastSpell(resolvedSpellId, castTarget, nullptr))
                    botAI->HandleCommand(CHAT_MSG_WHISPER, command, commandInitiator);
                else
                    SendCastExecutionFeedback(bot, commandInitiator, sourceLocal, resolvedSpellId, castTarget);
            }
            else
            {
                if (cmd == NaturalCommand::TRADE && listInventoryBeforeTrade)
                {
                    botAI->HandleCommand(CHAT_MSG_WHISPER, "c", commandInitiator);
                    botAI->HandleCommand(CHAT_MSG_WHISPER, command, commandInitiator);
                }
                else
                {
                    botAI->HandleCommand(CHAT_MSG_WHISPER, command, commandInitiator);
                }

                if (cmd == NaturalCommand::AVOID_AOE)
                    SendNaturalCommandWhisper(bot, commandInitiator, "Avoiding AoE.");
                else if (cmd == NaturalCommand::AVOID_AOE_OFF)
                    SendNaturalCommandWhisper(bot, commandInitiator, "Returning to normal AoE behavior.");
            }
        }

        if (cmd == NaturalCommand::ATTACK && selectionTemporarilyChanged)
        {
            ObjectGuid botGuid = bot->GetGUID();
            ObjectGuid initiatorGuid = commandInitiator->GetGUID();
            std::thread([botGuid, initiatorGuid, previousSelection, previousBotSelection]()
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1800));

                Player* botPtr = ObjectAccessor::FindPlayer(botGuid);
                Player* initiatorPtr = ObjectAccessor::FindPlayer(initiatorGuid);
                if (!botPtr || !initiatorPtr)
                    return;

                initiatorPtr->SetSelection(previousSelection);
                botPtr->SetSelection(previousBotSelection);
            }).detach();
        }

        if (selectionTemporarilyChanged && !deferSelectionRestore)
        {
            commandInitiator->SetSelection(previousSelection);
            bot->SetSelection(previousBotSelection);
        }

        if (g_DebugEnabled && cmd == NaturalCommand::CAST)
        {
            LOG_INFO("server.loading", "[Ollama Chat] CAST_DONE: bot='{}' command='{}' restored_player_selection='{}' restored_bot_selection='{}'",
                bot->GetName(), command, DescribeUnitForDebug(commandInitiator->GetSelectedUnit()), DescribeUnitForDebug(bot->GetSelectedUnit()));
        }

        return true;
    }
    catch (const std::exception& e)
    {
        if (selectionTemporarilyChanged)
        {
            commandInitiator->SetSelection(previousSelection);
            bot->SetSelection(previousBotSelection);
        }

        if (g_DebugEnabled)
        {
            LOG_ERROR("server.loading", "[Ollama Chat] Error executing command '{}' for bot {}: {}",
                    command, bot->GetName(), e.what());
        }
        return false;
    }
}

// Helper function to format class name for any player
static std::string FormatPlayerClass(uint8_t classId)
{
    switch (classId)
    {
        case CLASS_WARRIOR:      return "Warrior";
        case CLASS_PALADIN:      return "Paladin";
        case CLASS_HUNTER:       return "Hunter";
        case CLASS_ROGUE:        return "Rogue";
        case CLASS_PRIEST:       return "Priest";
        case CLASS_DEATH_KNIGHT: return "Death Knight";
        case CLASS_SHAMAN:       return "Shaman";
        case CLASS_MAGE:         return "Mage";
        case CLASS_WARLOCK:      return "Warlock";
        case CLASS_DRUID:        return "Druid";
        default:                 return "Unknown";
    }
}

// Helper function to format race name for any player
static std::string FormatPlayerRace(uint8_t raceId)
{
    switch (raceId)
    {
        case RACE_HUMAN:         return "Human";
        case RACE_ORC:           return "Orc";
        case RACE_DWARF:         return "Dwarf";
        case RACE_NIGHTELF:      return "Night Elf";
        case RACE_UNDEAD_PLAYER: return "Undead";
        case RACE_TAUREN:        return "Tauren";
        case RACE_GNOME:         return "Gnome";
        case RACE_TROLL:         return "Troll";
        case RACE_BLOODELF:      return "Blood Elf";
        case RACE_DRAENEI:       return "Draenei";
        default:                 return "Unknown";
    }
}

const char* ChatChannelSourceLocalStr[] =
{
    "Undefined",  // 0
    "Say",        // 1
    "Party",      // 2
    "Raid",       // 3
    "Guild",      // 4
    "Officer",    // 5
    "Yell",       // 6
    "Whisper",    // 7
    "Unknown8",   // 8
    "Unknown9",   // 9
    "Unknown10",  // 10
    "Unknown11",  // 11
    "Unknown12",  // 12
    "Unknown13",  // 13
    "Unknown14",  // 14
    "Unknown15",  // 15
    "Unknown16",  // 16
    "General"     // 17
};

std::string GetConversationEntryKey(uint64_t botGuid, uint64_t playerGuid, const std::string& playerMessage, const std::string& botReply)
{
    // Use a combination that guarantees uniqueness
    return SafeFormat("{}:{}:{}:{}", botGuid, playerGuid, playerMessage, botReply);
}

std::string rtrim(const std::string& s)
{
    const std::string whitespace = " \t\n\r,.!?;:";
    size_t end = s.find_last_not_of(whitespace);
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

ChatChannelSourceLocal GetChannelSourceLocal(uint32_t type)
{
    switch (type)
    {
        case CHAT_MSG_SAY:
            return SRC_SAY_LOCAL;
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
            return SRC_PARTY_LOCAL;
        case CHAT_MSG_RAID:
        case CHAT_MSG_RAID_LEADER:
        case CHAT_MSG_RAID_WARNING:
            return SRC_RAID_LOCAL;
        case CHAT_MSG_GUILD:
            return SRC_GUILD_LOCAL;
        case CHAT_MSG_OFFICER:
            return SRC_OFFICER_LOCAL;
        case CHAT_MSG_YELL:
            return SRC_YELL_LOCAL;
        case CHAT_MSG_WHISPER:
        case CHAT_MSG_WHISPER_FOREIGN:
        case CHAT_MSG_WHISPER_INFORM:
            return SRC_WHISPER_LOCAL;
        case CHAT_MSG_CHANNEL:
            return SRC_GENERAL_LOCAL;
        default:
            return SRC_UNDEFINED_LOCAL;
    }
}

Channel* GetValidChannel(uint32_t teamId, const std::string& channelName, Player* player)
{
    ChannelMgr* cMgr = ChannelMgr::forTeam(static_cast<TeamId>(teamId));
    Channel* channel = cMgr->GetChannel(channelName, player);
    if (!channel)
    {
        if(g_DebugEnabled)
        {
            LOG_ERROR("server.loading", "[Ollama Chat] Channel '{}' not found for team {}", channelName, teamId);
        }
    }
    return channel;
}

bool PlayerBotChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg)
{
    if (!g_Enable)
        return true;

    ChatChannelSourceLocal sourceLocal = GetChannelSourceLocal(type);
    ProcessChat(player, type, lang, msg, sourceLocal, nullptr, nullptr);
    return true;
}

bool PlayerBotChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Group* /*group*/)
{
    if (!g_Enable)
        return true;

    ChatChannelSourceLocal sourceLocal = GetChannelSourceLocal(type);
    ProcessChat(player, type, lang, msg, sourceLocal, nullptr, nullptr);
    return true;
}

bool PlayerBotChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Guild* /*guild*/)
{
    if (!g_Enable)
        return true;

    ChatChannelSourceLocal sourceLocal = GetChannelSourceLocal(type);
    ProcessChat(player, type, lang, msg, sourceLocal, nullptr, nullptr);
    return true;
}

bool PlayerBotChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel)
{
    if (!g_Enable)
        return true;

    ChatChannelSourceLocal sourceLocal = GetChannelSourceLocal(type);
    ProcessChat(player, type, lang, msg, sourceLocal, channel, nullptr);
    return true;
}

bool PlayerBotChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Player* receiver)
{
    // Only process if our module is enabled
    if (!g_Enable)
        return true;

    if (type == CHAT_MSG_WHISPER)
    {
        // Check if this is a valid whisper to a bot
        if (!receiver || !player || player == receiver)
            return true;

        // Check if sender is a bot - if so, don't trigger Ollama responses for bot-to-bot whispers
        PlayerbotAI* senderAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);
        if (senderAI && senderAI->IsBotAI())
        {
            return true;
        }

        PlayerbotAI* receiverAI = PlayerbotsMgr::instance().GetPlayerbotAI(receiver);
        if (!receiverAI || !receiverAI->IsBotAI())
            return true;
    }

    if (g_DebugEnabled)
    {
        LOG_INFO("server.loading", "[Ollama Chat] OnPlayerCanUseChat called: player={}, type={}, receiver={}",
            player->GetName(), type, receiver ? receiver->GetName() : "null");
    }

    // Process the chat immediately in OnPlayerCanUseChat to prevent double processing
    ChatChannelSourceLocal sourceLocal = GetChannelSourceLocal(type);
    ProcessChat(player, type, lang, msg, sourceLocal, nullptr, receiver);

    // Return false to prevent the message from being processed again in OnPlayerChat
    return true;
}

void AppendBotConversation(uint64_t botGuid, uint64_t playerGuid, const std::string& playerMessage, const std::string& botReply)
{
    std::lock_guard<std::mutex> lock(g_ConversationHistoryMutex);
    auto& playerHistory = g_BotConversationHistory[botGuid][playerGuid];
    playerHistory.push_back({ playerMessage, botReply });
    while (playerHistory.size() > g_MaxConversationHistory)
    {
        playerHistory.pop_front();
    }

}

void SaveBotConversationHistoryToDB()
{
    std::lock_guard<std::mutex> lock(g_ConversationHistoryMutex);

    for (const auto& [botGuid, playerMap] : g_BotConversationHistory) {
        for (const auto& [playerGuid, history] : playerMap) {
            for (const auto& pair : history) {
                const std::string& playerMessage = pair.first;
                const std::string& botReply = pair.second;

                std::string escPlayerMsg = playerMessage;
                CharacterDatabase.EscapeString(escPlayerMsg);

                std::string escBotReply = botReply;
                CharacterDatabase.EscapeString(escBotReply);

                CharacterDatabase.Execute(SafeFormat(
                    "INSERT IGNORE INTO mod_ollama_chat_history (bot_guid, player_guid, timestamp, player_message, bot_reply) "
                    "VALUES ({}, {}, NOW(), '{}', '{}')",
                    botGuid, playerGuid, escPlayerMsg, escBotReply));
            }
        }
    }

    // Cleanup: keep only the N most recent entries per bot/player pair
    std::string cleanupQuery = R"SQL(
        DELETE h1 FROM mod_ollama_chat_history h1
        JOIN (
            SELECT bot_guid, player_guid, timestamp,
                   ROW_NUMBER() OVER(PARTITION BY bot_guid, player_guid ORDER BY timestamp DESC) as rn
            FROM mod_ollama_chat_history
        ) h2 ON h1.bot_guid = h2.bot_guid
             AND h1.player_guid = h2.player_guid
             AND h1.timestamp = h2.timestamp
        WHERE h2.rn > {};
    )SQL";
    CharacterDatabase.Execute(SafeFormat(cleanupQuery, g_MaxConversationHistory));
}

// Called when a bot sends a message (random chatter or other bot-initiated messages)
// This triggers other bots to potentially reply
void ProcessBotChatMessage(Player* bot, const std::string& msg, ChatChannelSourceLocal sourceLocal, Channel* channel)
{
    if (!bot || msg.empty())
        return;
        
    // If channel is nullptr but this is a channel-type message, try to find the channel
    if (!channel && sourceLocal == SRC_GENERAL_LOCAL)
    {
        // Look up the General channel for this bot's faction
        std::string channelName = "General";
        ChannelMgr* cMgr = ChannelMgr::forTeam(bot->GetTeamId());
        if (cMgr)
        {
            channel = cMgr->GetChannel(channelName, bot);
            if (g_DebugEnabled)
            {
                if (channel)
                    LOG_INFO("server.loading", "[Ollama Chat] ProcessBotChatMessage: Found General channel for bot {}", bot->GetName());
                else
                    LOG_ERROR("server.loading", "[Ollama Chat] ProcessBotChatMessage: Could not find General channel for bot {}", bot->GetName());
            }
        }
    }
    
    // Validate that bot is actually in the relevant chat group before triggering replies
    bool canSendMessage = false;
    switch (sourceLocal)
    {
        case SRC_SAY_LOCAL:
        case SRC_YELL_LOCAL:
            // Distance checks will be applied during eligibility filtering
            canSendMessage = true;
            break;
            
        case SRC_GENERAL_LOCAL:
            // Must have a channel object
            canSendMessage = (channel != nullptr);
            if (!canSendMessage && g_DebugEnabled)
                LOG_ERROR("server.loading", "[Ollama Chat] Bot {} cannot send to General - no channel found", bot->GetName());
            break;
            
        case SRC_GUILD_LOCAL:
        case SRC_OFFICER_LOCAL:
            // Must be in a guild with at least one real player online
            if (bot->GetGuildId() != 0)
            {
                Guild* guild = sGuildMgr->GetGuildById(bot->GetGuildId());
                if (guild)
                {
                    // Check if any real (non-bot) players are online in this guild
                    bool hasRealPlayer = false;
                    for (auto const& pair : ObjectAccessor::GetPlayers())
                    {
                        Player* member = pair.second;
                        if (member && member->GetGuildId() == bot->GetGuildId())
                        {
                            if (!PlayerbotsMgr::instance().GetPlayerbotAI(member))
                            {
                                hasRealPlayer = true;
                                break;
                            }
                        }
                    }
                    canSendMessage = hasRealPlayer;
                    if (!canSendMessage && g_DebugEnabled)
                        LOG_INFO("server.loading", "[Ollama Chat] Bot {} cannot send to Guild - no real players online in guild", bot->GetName());
                }
                else
                {
                    canSendMessage = false;
                    if (g_DebugEnabled)
                        LOG_ERROR("server.loading", "[Ollama Chat] Bot {} cannot send to Guild - guild not found", bot->GetName());
                }
            }
            else
            {
                canSendMessage = false;
                if (g_DebugEnabled)
                    LOG_ERROR("server.loading", "[Ollama Chat] Bot {} cannot send to Guild - not in a guild", bot->GetName());
            }
            break;
            
        case SRC_PARTY_LOCAL:
        case SRC_RAID_LOCAL:
            // Must be in a group with at least one real player
            if (bot->GetGroup())
            {
                Group* group = bot->GetGroup();
                bool hasRealPlayer = false;
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* member = ref->GetSource();
                    if (member && !PlayerbotsMgr::instance().GetPlayerbotAI(member))
                    {
                        hasRealPlayer = true;
                        break;
                    }
                }
                canSendMessage = hasRealPlayer;
                if (!canSendMessage && g_DebugEnabled)
                    LOG_INFO("server.loading", "[Ollama Chat] Bot {} cannot send to Party - no real players in group", bot->GetName());
            }
            else
            {
                canSendMessage = false;
                if (g_DebugEnabled)
                    LOG_ERROR("server.loading", "[Ollama Chat] Bot {} cannot send to Party - not in a group", bot->GetName());
            }
            break;
            
        case SRC_WHISPER_LOCAL:
            // Whispers are handled separately
            canSendMessage = true;
            break;
            
        default:
            canSendMessage = true;
            break;
    }
    
    if (!canSendMessage)
    {
        if (g_DebugEnabled)
            LOG_ERROR("server.loading", "[Ollama Chat] Bot {} cannot send message to {} - validation failed", 
                    bot->GetName(), ChatChannelSourceLocalStr[sourceLocal]);
        return;
    }
        
    // Convert ChatChannelSourceLocal back to chat type for ProcessChat
    uint32_t type = 0;
    switch (sourceLocal)
    {
        case SRC_SAY_LOCAL: type = CHAT_MSG_SAY; break;
        case SRC_YELL_LOCAL: type = CHAT_MSG_YELL; break;
        case SRC_PARTY_LOCAL: type = CHAT_MSG_PARTY; break;
        case SRC_RAID_LOCAL: type = CHAT_MSG_RAID; break;
        case SRC_GUILD_LOCAL: type = CHAT_MSG_GUILD; break;
        case SRC_OFFICER_LOCAL: type = CHAT_MSG_OFFICER; break;
        case SRC_WHISPER_LOCAL: type = CHAT_MSG_WHISPER; break;
        case SRC_GENERAL_LOCAL: type = CHAT_MSG_CHANNEL; break;
        default: type = CHAT_MSG_SAY; break;
    }
    
    std::string mutableMsg = msg; // ProcessChat takes non-const reference
    uint32_t lang = bot->GetTeamId() == TEAM_ALLIANCE ? LANG_COMMON : LANG_ORCISH;
    
    // Call the main ProcessChat function with bot as sender
    PlayerBotChatHandler::ProcessChat(bot, type, lang, mutableMsg, sourceLocal, channel, nullptr);
}

std::string GetBotHistoryPrompt(uint64_t botGuid, uint64_t playerGuid, std::string playerMessage)
{
    if(!g_EnableChatHistory)
    {
        return "";
    }
    
    std::lock_guard<std::mutex> lock(g_ConversationHistoryMutex);

    std::string result;
    const auto botIt = g_BotConversationHistory.find(botGuid);
    if (botIt == g_BotConversationHistory.end())
        return result;
    const auto playerIt = botIt->second.find(playerGuid);
    if (playerIt == botIt->second.end())
        return result;

    Player* player = ObjectAccessor::FindPlayer(ObjectGuid(playerGuid));
    std::string playerName = player ? player->GetName() : "The player";

    result += SafeFormat(g_ChatHistoryHeaderTemplate, fmt::arg("player_name", playerName));

    for (const auto& entry : playerIt->second) {
        result += SafeFormat(g_ChatHistoryLineTemplate,
            fmt::arg("player_name", playerName),
            fmt::arg("player_message", entry.first),
            fmt::arg("bot_reply", entry.second)
        );
    }

    result += SafeFormat(g_ChatHistoryFooterTemplate,
        fmt::arg("player_name", playerName),
        fmt::arg("player_message", playerMessage)
    );

    return result;
}

// --- Helper: Spells ---
std::string ChatHandler_GetBotSpellInfo(Player* bot)
{
    // Map to store highest rank of each spell: spell name -> (spellId, rank, costText)
    std::map<std::string, std::tuple<uint32, uint32, std::string>> uniqueSpells;
    
    for (const auto& spellPair : bot->GetSpellMap())
    {
        uint32 spellId = spellPair.first;
        const SpellInfo* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || spellInfo->Attributes & SPELL_ATTR0_PASSIVE)
            continue;
        if (spellInfo->SpellFamilyName == SPELLFAMILY_GENERIC)
            continue;
        if (bot->HasSpellCooldown(spellId))
            continue;
        
        const char* name = spellInfo->SpellName[0];
        if (!name || !*name)
            continue;
        
        std::string costText;
        if (spellInfo->ManaCost || spellInfo->ManaCostPercentage)
        {
            switch (spellInfo->PowerType)
            {
                case POWER_MANA: costText = std::to_string(spellInfo->ManaCost) + " mana"; break;
                case POWER_RAGE: costText = std::to_string(spellInfo->ManaCost) + " rage"; break;
                case POWER_FOCUS: costText = std::to_string(spellInfo->ManaCost) + " focus"; break;
                case POWER_ENERGY: costText = std::to_string(spellInfo->ManaCost) + " energy"; break;
                case POWER_RUNIC_POWER: costText = std::to_string(spellInfo->ManaCost) + " runic power"; break;
                default: costText = std::to_string(spellInfo->ManaCost) + " unknown resource"; break;
            }
        }
        else
        {
            costText = "no cost";
        }
        
        // Get base spell name (without rank)
        std::string spellName = name;
        uint32 rank = spellInfo->GetRank();
        
        // Check if we already have this spell, and if so, only keep the highest rank
        auto it = uniqueSpells.find(spellName);
        if (it == uniqueSpells.end())
        {
            // First time seeing this spell
            uniqueSpells[spellName] = std::make_tuple(spellId, rank, costText);
        }
        else
        {
            // We've seen this spell before, check if this is a higher rank
            uint32 existingRank = std::get<1>(it->second);
            if (rank > existingRank)
            {
                // Replace with higher rank
                uniqueSpells[spellName] = std::make_tuple(spellId, rank, costText);
            }
        }
    }
    
    // Build the output string from unique spells
    std::ostringstream spellSummary;
    for (const auto& [spellName, spellData] : uniqueSpells)
    {
        uint32 rank = std::get<1>(spellData);
        const std::string& costText = std::get<2>(spellData);
        
        spellSummary << "**" << spellName << "**";
        if (rank > 0)
        {
            spellSummary << " (Rank " << rank << ")";
        }
        spellSummary << " - Costs " << costText << "\n";
    }
    return spellSummary.str();
}

// --- Helper: Group info ---
std::vector<std::string> ChatHandler_GetGroupStatus(Player* bot)
{
    std::vector<std::string> info;
    if (!bot || !bot->GetGroup()) return info;
    Group* group = bot->GetGroup();
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->GetMap()) continue;
        if(bot == member) continue;
        float dist = bot->GetDistance(member);
        std::string beingAttacked = "";
        if (Unit* attacker = member->GetVictim())
        {
            beingAttacked = " [Under Attack by " + attacker->GetName() +
                            ", Level: " + std::to_string(attacker->GetLevel()) + ", HP: " + std::to_string(attacker->GetHealth()) +
                            "/" + std::to_string(attacker->GetMaxHealth()) + ")]";
        }
        std::string className = FormatPlayerClass(member->getClass());
        std::string raceName = FormatPlayerRace(member->getRace());
        info.push_back(
            member->GetName() +
            " (Level: " + std::to_string(member->GetLevel()) +
            ", Class: " + className +
            ", Race: " + raceName +
            ", HP: " + std::to_string(member->GetHealth()) + "/" + std::to_string(member->GetMaxHealth()) +
            ", Dist: " + std::to_string(dist) + ")" + beingAttacked
        );

    }
    return info;
}

// --- Helper: Visible players ---
std::vector<std::string> ChatHandler_GetVisiblePlayers(Player* bot, float radius = 40.0f)
{
    std::vector<std::string> players;
    if (!bot || !bot->GetMap()) return players;
    for (auto const& pair : ObjectAccessor::GetPlayers())
    {
        Player* player = pair.second;
        if (!player || player == bot) continue;
        if (!player->IsInWorld() || player->IsGameMaster()) continue;
        if (player->GetMap() != bot->GetMap()) continue;
        if (!bot->IsWithinDistInMap(player, radius)) continue;
        if (!bot->IsWithinLOS(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ())) continue;
        float dist = bot->GetDistance(player);
        std::string faction = (player->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");
        std::string className = FormatPlayerClass(player->getClass());
        std::string raceName = FormatPlayerRace(player->getRace());
        players.push_back(
            "Player: " + player->GetName() +
            " (Level: " + std::to_string(player->GetLevel()) +
            ", Class: " + className +
            ", Race: " + raceName +
            ", Faction: " + faction +
            ", Distance: " + std::to_string(dist) + ")"
        );

    }
    return players;
}

// --- Helper: Visible locations/objects (creatures and gameobjects) ---
std::vector<std::string> ChatHandler_GetVisibleLocations(Player* bot, float radius = 40.0f)
{
    std::vector<std::string> visible;
    if (!bot || !bot->GetMap()) return visible;
    Map* map = bot->GetMap();
    for (auto const& pair : map->GetCreatureBySpawnIdStore())
    {
        Creature* c = pair.second;
        if (!c) continue;
        if (c->GetGUID() == bot->GetGUID()) continue;
        if (!bot->IsWithinDistInMap(c, radius)) continue;
        if (!bot->IsWithinLOS(c->GetPositionX(), c->GetPositionY(), c->GetPositionZ())) continue;
        if (c->IsPet() || c->IsTotem()) continue;
        std::string type;
        if (c->isDead()) type = "DEAD";
        else if (c->IsHostileTo(bot)) type = "ENEMY";
        else if (c->IsFriendlyTo(bot)) type = "FRIENDLY";
        else type = "NEUTRAL";
        float dist = bot->GetDistance(c);
        visible.push_back(
            type + ": " + c->GetName() +
            ", Level: " + std::to_string(c->GetLevel()) +
            ", HP: " + std::to_string(c->GetHealth()) + "/" + std::to_string(c->GetMaxHealth()) +
            ", Distance: " + std::to_string(dist) + ")"
        );
    }
    for (auto const& pair : map->GetGameObjectBySpawnIdStore())
    {
        GameObject* go = pair.second;
        if (!go) continue;
        if (!bot->IsWithinDistInMap(go, radius)) continue;
        if (!bot->IsWithinLOS(go->GetPositionX(), go->GetPositionY(), go->GetPositionZ())) continue;
        float dist = bot->GetDistance(go);
        visible.push_back(
            go->GetName() +
            ", Type: " + std::to_string(go->GetGoType()) +
            ", Distance: " + std::to_string(dist) + ")"
        );
    }
    return visible;
}

// --- Helper: Combat summary ---
std::string ChatHandler_GetCombatSummary(Player* bot)
{
    std::ostringstream oss;
    bool inCombat = bot->IsInCombat();
    Unit* victim = bot->GetVictim();

    // Class-specific resource reporting
    auto classId = bot->getClass();

    auto printResource = [&](std::ostringstream& oss) {
        switch (classId)
        {
            case CLASS_WARRIOR:
                oss << ", Rage: " << bot->GetPower(POWER_RAGE) << "/" << bot->GetMaxPower(POWER_RAGE);
                break;
            case CLASS_ROGUE:
                oss << ", Energy: " << bot->GetPower(POWER_ENERGY) << "/" << bot->GetMaxPower(POWER_ENERGY);
                break;
            case CLASS_DEATH_KNIGHT:
                oss << ", Runic Power: " << bot->GetPower(POWER_RUNIC_POWER) << "/" << bot->GetMaxPower(POWER_RUNIC_POWER);
                break;
            case CLASS_HUNTER:
                oss << ", Focus: " << bot->GetPower(POWER_FOCUS) << "/" << bot->GetMaxPower(POWER_FOCUS);
                break;
            default: // Mana classes
                if (bot->GetMaxPower(POWER_MANA) > 0)
                    oss << ", Mana: " << bot->GetPower(POWER_MANA) << "/" << bot->GetMaxPower(POWER_MANA);
                break;
        }
    };

    if (inCombat)
    {
        oss << "IN COMBAT: ";
        if (victim)
        {
            oss << "Target: " << victim->GetName()
                << ", Level: " << victim->GetLevel()
                << ", HP: " << victim->GetHealth() << "/" << victim->GetMaxHealth();
        }
        else
        {
            oss << "No current target";
        }
        oss << ". ";
        printResource(oss);
    }
    else
    {
        oss << "NOT IN COMBAT. ";
        printResource(oss);
    }
    return oss.str();
}


static std::string GenerateBotGameStateSnapshot(Player* bot)
{
    // Prepare each section
    std::string combat = ChatHandler_GetCombatSummary(bot);

    std::string group;
    std::vector<std::string> groupInfo = ChatHandler_GetGroupStatus(bot);
    if (!groupInfo.empty()) {
        group += "Group members:\n";
        for (const auto& entry : groupInfo) group += " - " + entry + "\n";
    }

    std::string spells = ChatHandler_GetBotSpellInfo(bot);

    std::string quests;
    for (auto const& [questId, qsd] : bot->getQuestStatusMap())
    {
        // look up the template
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        // get the English title as a fallback
        std::string title = quest->GetTitle();

        // then, if we have a locale record, overwrite it
        if (auto const* locale = sObjectMgr->GetQuestLocale(questId))
        {
            int locIdx = bot->GetSession()->GetSessionDbLocaleIndex();
            if (locIdx >= 0)
                ObjectMgr::GetLocaleString(locale->Title, locIdx, title);
        }

        // Convert quest status to readable string
        std::string statusText;
        switch (qsd.Status)
        {
            case QUEST_STATUS_NONE:       statusText = "not started"; break;
            case QUEST_STATUS_COMPLETE:   statusText = "complete (ready to turn in)"; break;
            case QUEST_STATUS_INCOMPLETE: statusText = "in progress"; break;
            case QUEST_STATUS_FAILED:     statusText = "failed"; break;
            case QUEST_STATUS_REWARDED:   statusText = "completed and rewarded"; break;
            default:                      statusText = "unknown"; break;
        }

        quests += "Quest \"" + title + "\" is " + statusText + "\n";
    }

    std::string los;
    std::vector<std::string> losLocs = ChatHandler_GetVisibleLocations(bot);
    if (!losLocs.empty()) {
        for (const auto& entry : losLocs) los += " - " + entry + "\n";
    }

    std::string players;
    std::vector<std::string> nearbyPlayers = ChatHandler_GetVisiblePlayers(bot);
    if (!nearbyPlayers.empty()) {
        for (const auto& entry : nearbyPlayers) players += " - " + entry + "\n";
    }

    // Use template
    return SafeFormat(
        g_ChatBotSnapshotTemplate,
        fmt::arg("combat", combat),
        fmt::arg("group", group),
        fmt::arg("spells", spells),
        fmt::arg("quests", quests),
        fmt::arg("los", los),
        fmt::arg("players", players)
    );
}


void PlayerBotChatHandler::ProcessChat(Player* player, uint32_t /*type*/, uint32_t lang, std::string& msg, ChatChannelSourceLocal sourceLocal, Channel* channel, Player* receiver)
{
    if (player == nullptr) {
        LOG_ERROR("server.loading", "[Ollama Chat] ProcessChat: player is null");
        return;
    }
    if (msg.empty()) {
        return;
    }
    if (lang == LANG_ADDON) return;
    std::string chanName = (channel != nullptr) ? channel->GetName() : "Unknown";
    uint32_t channelId = (channel != nullptr) ? channel->GetChannelId() : 0;
    std::string receiverName = (receiver != nullptr) ? receiver->GetName() : "None";
    if(g_DebugEnabled)
    {
        LOG_INFO("server.loading",
                "[Ollama Chat] Player {} sent msg: '{}' | Source: {} | Channel Name: {} | Channel ID: {} | Receiver: {}",
                player->GetName(), msg, (int)sourceLocal, chanName, channelId, receiverName);
    }


    auto startsWithWord = [](const std::string& text, const std::string& word) {
        if (text.size() < word.size()) return false;
        if (text.compare(0, word.size(), word) != 0) return false;
        // If exact length match or next char is whitespace/punctuation, it's a word
        return text.size() == word.size() || !std::isalnum((unsigned char)text[word.size()]);
    };

    std::string trimmedMsg = rtrim(msg);
    for (const std::string& blacklist : g_BlacklistCommands)
    {
        if (startsWithWord(trimmedMsg, blacklist))
        {
            if (g_DebugEnabled)
                LOG_INFO("server.loading",
                         "[Ollama Chat] Message starts with '{}' (blacklisted). Skipping bot responses.",
                         blacklist);
            return;
        }
    }
    
    // Check if this channel type is disabled
    if (sourceLocal == SRC_GENERAL_LOCAL && g_DisableForCustomChannels)
    {
        if (g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] Custom channels are disabled, skipping");
        }
        return;
    }
    
    if ((sourceLocal == SRC_SAY_LOCAL || sourceLocal == SRC_YELL_LOCAL) && g_DisableForSayYell)
    {
        if (g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] Say/Yell channels are disabled, skipping");
        }
        return;
    }
    
    if ((sourceLocal == SRC_GUILD_LOCAL || sourceLocal == SRC_OFFICER_LOCAL) && g_DisableForGuild)
    {
        if (g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] Guild channels are disabled, skipping");
        }
        return;
    }
    
    if ((sourceLocal == SRC_PARTY_LOCAL || sourceLocal == SRC_RAID_LOCAL) && g_DisableForParty)
    {
        if (g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] Party/Raid channels are disabled, skipping");
        }
        return;
    }
             
    PlayerbotAI* senderAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);
    bool senderIsBot = (senderAI && senderAI->IsBotAI());
    
    std::vector<Player*> eligibleBots;
    
    // Handle different chat sources differently
    if (sourceLocal == SRC_WHISPER_LOCAL && receiver != nullptr)
    {
        // Check if whisper replies are disabled
        if (!g_EnableWhisperReplies)
        {
            if(g_DebugEnabled)
            {
                LOG_INFO("server.loading", "[Ollama Chat] Whisper replies are disabled, skipping");
            }
            return;
        }
        
        if(g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] Processing whisper from {} to {}", 
                    player->GetName(), receiver->GetName());
        }
        
        // Skip bot-to-bot whispers to prevent Ollama responses
        if (senderIsBot)
        {
            return;
        }
        
        // For whispers, only the receiver bot can respond (if it's a bot)
        PlayerbotAI* receiverAI = PlayerbotsMgr::instance().GetPlayerbotAI(receiver);
        if (receiverAI && receiverAI->IsBotAI())
        {
            eligibleBots.push_back(receiver);
            if(g_DebugEnabled)
            {
                LOG_INFO("server.loading", "[Ollama Chat] Found eligible bot {} for whisper", receiver->GetName());
            }
        }
        else if(g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] Whisper target {} is not a bot or has no AI", receiver->GetName());
        }
    }
    else if (channel != nullptr)
    {
        // For channel chat, find all bots that are in the same channel instance
        if(g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] Processing channel message in '{}' (ID: {})", 
                    channel->GetName(), channel->GetChannelId());
        }
        
        // Verify the original channel is valid before proceeding
        if (!channel)
        {
            if(g_DebugEnabled)
            {
                LOG_ERROR("server.loading", "[Ollama Chat] Channel is null, cannot process channel message");
            }
            return;
        }
        
        // For channel chat, simply find all bots in the same zone as the player
        auto const& allPlayers = ObjectAccessor::GetPlayers();
        for (auto const& itr : allPlayers)
        {
            Player* candidate = itr.second;
            if (!candidate || candidate == player)
                continue;
                
            // Skip non-bots early
            PlayerbotAI* candidateAI = PlayerbotsMgr::instance().GetPlayerbotAI(candidate);
            if (!candidateAI || !candidateAI->IsBotAI())
                continue;
            
            // Check if this is a local or global channel
            bool isLocalChannel = (channel->GetName().find("General -") != std::string::npos || 
                                  channel->GetName().find("Trade -") != std::string::npos ||
                                  channel->GetName().find("LocalDefense -") != std::string::npos);
            
            bool isGlobalChannel = (channel->GetName().find("World") != std::string::npos || channel->GetName().find("LookingForGroup") != std::string::npos);
        
            // For local channels, bot must be in same zone as player
            if (isLocalChannel)
            {
                // ZONE CHECK: Bot must be in exact same zone as player
                if (candidate->GetZoneId() != player->GetZoneId())
                {
                    if(g_DebugEnabled)
                    {
                        //LOG_ERROR("server.loading", "[Ollama Chat] Bot {} FAILED zone check - Bot zone: {}, Player zone: {}, Channel: '{}'", candidate->GetName(), candidate->GetZoneId(), player->GetZoneId(), channel->GetName());
                    }
                    continue; // SKIP this bot - wrong zone
                }
            }
            // For global channels like World, no zone restriction
            
            // CHANNEL MEMBERSHIP CHECK: Bot must actually be in the channel
            if (!candidate->IsInChannel(channel))
            {
                if(g_DebugEnabled)
                {
                    //LOG_INFO("server.loading", "[Ollama Chat] Bot {} not in channel '{}', skipping", candidate->GetName(), channel->GetName());
                }
                continue;
            }
            
            // FACTION CHECK: For non-global channels, ensure same faction
            if (candidate->GetTeamId() != player->GetTeamId())
            {
                if (!isGlobalChannel)
                {
                    if(g_DebugEnabled)
                    {
                        //LOG_ERROR("server.loading", "[Ollama Chat] Bot {} FAILED faction check - Bot: {}, Player: {}, Channel: '{}'", candidate->GetName(), (int)candidate->GetTeamId(), (int)player->GetTeamId(), channel->GetName());
                    }
                    continue; // SKIP this bot - wrong faction
                }
            }
            
            // CHANNEL MEMBERSHIP CHECK: Verify bot is actually in the channel
            if (!candidate->IsInChannel(channel))
            {
                if(g_DebugEnabled)
                {
                    //LOG_ERROR("server.loading", "[Ollama Chat] Bot {} FAILED channel membership check - Not in channel '{}'", candidate->GetName(), channel->GetName());
                }
                continue; // SKIP this bot - not in the channel
            }
            
            // REAL PLAYER CHECK: Channel must have at least one real player
            bool hasRealPlayerInChannel = false;
            for (auto const& playerItr : allPlayers)
            {
                Player* potentialRealPlayer = playerItr.second;
                if (potentialRealPlayer && potentialRealPlayer->IsInChannel(channel))
                {
                    PlayerbotAI* realPlayerAI = PlayerbotsMgr::instance().GetPlayerbotAI(potentialRealPlayer);
                    if (!realPlayerAI || !realPlayerAI->IsBotAI())
                    {
                        hasRealPlayerInChannel = true;
                        break;
                    }
                }
            }
            
            if (!hasRealPlayerInChannel)
            {
                if(g_DebugEnabled)
                {
                    //LOG_INFO("server.loading", "[Ollama Chat] Bot {} skipped - no real players in channel '{}'", candidate->GetName(), channel->GetName());
                }
                continue;
            }
            
            // ONLY add bots that passed ALL verifications
            eligibleBots.push_back(candidate);
            if(g_DebugEnabled)
            {
                // LOG_INFO("server.loading", "[Ollama Chat] VERIFIED eligible bot {} in channel '{}' - Distance: {:.2f}, Zone match: {}", candidate->GetName(), channel->GetName(), candidate->GetDistance(player), (candidate->GetZoneId() == player->GetZoneId()));
            }
        }
        
        if(g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] Found {} bots in channel instance '{}'", 
                    eligibleBots.size(), channel->GetName());
        }
    }
    else
    {
        // For other chat types (say, yell, guild, party, etc.), use all players and filter by eligibility
        auto const& allPlayers = ObjectAccessor::GetPlayers();
        for (auto const& itr : allPlayers)
        {
            Player* candidate = itr.second;
            if (candidate->IsInWorld() && candidate != player)
            {
                PlayerbotAI* candidateAI = PlayerbotsMgr::instance().GetPlayerbotAI(candidate);
                if (candidateAI && candidateAI->IsBotAI())
                {
                    if ((sourceLocal == SRC_PARTY_LOCAL || sourceLocal == SRC_RAID_LOCAL) &&
                        (!player->GetGroup() || candidate->GetGroup() != player->GetGroup()))
                        continue;

                    // For Guild/Party, verify there's a real player in that guild/party
                    if (sourceLocal == SRC_GUILD_LOCAL || sourceLocal == SRC_OFFICER_LOCAL)
                    {
                        if (candidate->GetGuildId() != 0)
                        {
                            // Check if any real player is online in this guild
                            bool hasRealPlayerInGuild = false;
                            for (auto const& guildPlayerItr : allPlayers)
                            {
                                Player* guildMember = guildPlayerItr.second;
                                if (guildMember && guildMember->GetGuildId() == candidate->GetGuildId())
                                {
                                    PlayerbotAI* memberAI = PlayerbotsMgr::instance().GetPlayerbotAI(guildMember);
                                    if (!memberAI || !memberAI->IsBotAI())
                                    {
                                        hasRealPlayerInGuild = true;
                                        break;
                                    }
                                }
                            }
                            if (!hasRealPlayerInGuild)
                                continue; // Skip bot - no real players in guild
                        }
                    }
                    else if (sourceLocal == SRC_PARTY_LOCAL || sourceLocal == SRC_RAID_LOCAL)
                    {
                        Group* group = candidate->GetGroup();
                        if (group)
                        {
                            // Check if any real player is in this group
                            bool hasRealPlayerInGroup = false;
                            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                            {
                                Player* member = ref->GetSource();
                                if (member)
                                {
                                    PlayerbotAI* memberAI = PlayerbotsMgr::instance().GetPlayerbotAI(member);
                                    if (!memberAI || !memberAI->IsBotAI())
                                    {
                                        hasRealPlayerInGroup = true;
                                        break;
                                    }
                                }
                            }
                            if (!hasRealPlayerInGroup)
                                continue; // Skip bot - no real players in group
                        }
                    }
                    else if (sourceLocal == SRC_SAY_LOCAL || sourceLocal == SRC_YELL_LOCAL)
                    {
                        // For Say/Yell, require a real player within hearing distance
                        float threshold = (sourceLocal == SRC_SAY_LOCAL) ? g_SayDistance : g_YellDistance;
                        bool hasRealPlayerNearby = false;
                        
                        if (candidate->IsInWorld() && threshold > 0.0f)
                        {
                            for (auto const& nearbyPlayerItr : allPlayers)
                            {
                                Player* nearbyPlayer = nearbyPlayerItr.second;
                                if (nearbyPlayer && nearbyPlayer->IsInWorld())
                                {
                                    PlayerbotAI* nearbyAI = PlayerbotsMgr::instance().GetPlayerbotAI(nearbyPlayer);
                                    if (!nearbyAI || !nearbyAI->IsBotAI())
                                    {
                                        if (candidate->GetDistance(nearbyPlayer) <= threshold)
                                        {
                                            hasRealPlayerNearby = true;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                        
                        if (!hasRealPlayerNearby)
                            continue; // Skip bot - no real player can hear Say/Yell
                    }
                    
                    eligibleBots.push_back(candidate);
                }
            }
        }
    }
    
    std::vector<Player*> candidateBots;
    int notEligibleCount = 0;
    for (Player* bot : eligibleBots)
    {
        if (!bot)
        {
            continue;
        }
        
        // For channel messages, bots in eligibleBots have already passed STRICT channel checks
        // Only run additional eligibility checks for non-channel sources
        // EXCEPTION: If channel is nullptr but sourceLocal is a channel type (like GENERAL), 
        // treat it as a channel message (happens with bot-initiated messages)
        bool isChannelSource = (sourceLocal == SRC_GENERAL_LOCAL);
        
        if (channel != nullptr || isChannelSource)
        {
            // Channel bots have already been verified to be in EXACT same channel instance
            // OR this is a channel-type source (General) even without channel object
            candidateBots.push_back(bot);
        }
        else
        {
            // For non-channel sources (Say/Yell/Guild/Party/Whisper), run the full eligibility check
            if (IsBotEligibleForChatChannelLocal(bot, player, sourceLocal, channel, receiver))
                candidateBots.push_back(bot);
            else
                notEligibleCount++;
        }
    }
    
    if (g_DebugEnabled && notEligibleCount > 0)
    {
        LOG_INFO("server.loading", "[Ollama Chat] {} bots not eligible for {} (distance/guild/party checks failed)", 
                notEligibleCount, ChatChannelSourceLocalStr[sourceLocal]);
    }
    
    // Determine reply chance based on channel type
    uint32_t chance;
    if (sourceLocal == SRC_SAY_LOCAL || sourceLocal == SRC_YELL_LOCAL)
    {
        // Say/Yell channel type
        chance = senderIsBot ? g_BotReplyChance_Say : g_PlayerReplyChance_Say;
    }
    else if (sourceLocal == SRC_PARTY_LOCAL || sourceLocal == SRC_RAID_LOCAL)
    {
        // Party/Raid channel type
        chance = senderIsBot ? g_BotReplyChance_Party : g_PlayerReplyChance_Party;
    }
    else if (sourceLocal == SRC_GUILD_LOCAL || sourceLocal == SRC_OFFICER_LOCAL)
    {
        // Guild/Officer channel type
        chance = senderIsBot ? g_BotReplyChance_Guild : g_PlayerReplyChance_Guild;
    }
    else if (sourceLocal == SRC_GENERAL_LOCAL)
    {
        // General/Trade/Custom channel type
        chance = senderIsBot ? g_BotReplyChance_Channel : g_PlayerReplyChance_Channel;
    }
    else
    {
        // Default fallback (whispers, etc.) - use Say chances
        chance = senderIsBot ? g_BotReplyChance_Say : g_PlayerReplyChance_Say;
    }
    
    if(g_DebugEnabled)
    {
        LOG_INFO("server.loading", "[Ollama Chat] Sender: {} ({}), Channel: {}, Reply Chance: {}%, Candidate Bots: {}",
                player->GetName(), senderIsBot ? "BOT" : "PLAYER", ChatChannelSourceLocalStr[sourceLocal], chance, candidateBots.size());
    }
    
    std::vector<Player*> finalCandidates;
    
    // For whispers, handle directly - there should only be one receiver bot
    if (sourceLocal == SRC_WHISPER_LOCAL)
    {
        if (!candidateBots.empty())
        {
            Player* whisperBot = candidateBots[0]; // Should only be one bot for whispers
            if (!(g_DisableRepliesInCombat && whisperBot->IsInCombat()))
            {
                finalCandidates.push_back(whisperBot);
                if(g_DebugEnabled)
                {
                    LOG_INFO("server.loading", "[Ollama Chat] Whisper: Bot {} selected to respond", whisperBot->GetName());
                }
            }
        }
    }
    else
    {
        // Handle non-whisper chats with normal multi-bot logic
        std::vector<std::pair<size_t, Player*>> mentionedBots;

        // Helper to convert string to lowercase safely
        auto toLowerStr = [](const std::string& str) -> std::string {
            std::string result = str;
            for (char& c : result)
            {
                c = std::tolower(static_cast<unsigned char>(c));
            }
            return result;
        };

        // Helper to check if a bot name is mentioned as a complete word
        auto isBotNameMentioned = [&trimmedMsg, &toLowerStr](const std::string& botName) -> size_t {
            std::string lowerMsg = toLowerStr(trimmedMsg);
            std::string lowerBotName = toLowerStr(botName);
            
            size_t pos = 0;
            while ((pos = lowerMsg.find(lowerBotName, pos)) != std::string::npos)
            {
                // Check if it's a word boundary before the name
                bool validStart = (pos == 0 || !std::isalnum(static_cast<unsigned char>(lowerMsg[pos - 1])));
                // Check if it's a word boundary after the name
                size_t endPos = pos + lowerBotName.length();
                bool validEnd = (endPos >= lowerMsg.length() || !std::isalnum(static_cast<unsigned char>(lowerMsg[endPos])));
                
                if (validStart && validEnd)
                {
                    return pos; // Found a valid word-boundary match
                }
                pos++; // Continue searching
            }
            return std::string::npos;
        };

        for (Player* bot : candidateBots)
        {
            if (!bot)
            {
                continue;
            }
            if (g_DisableRepliesInCombat && bot->IsInCombat())
            {
                continue;
            }
            
            size_t pos = isBotNameMentioned(bot->GetName());
            if (pos != std::string::npos)
            {
                mentionedBots.emplace_back(pos, bot);
                if(g_DebugEnabled)
                {
                    LOG_INFO("server.loading", "[Ollama Chat] Bot {} mentioned at position {} in message", bot->GetName(), pos);
                }
            }
        }

        if (!mentionedBots.empty())
        {
            // Sort by position to get the first mentioned bot
            std::sort(mentionedBots.begin(), mentionedBots.end(),
                      [](const std::pair<size_t, Player*> &a, const std::pair<size_t, Player*> &b) { return a.first < b.first; });
            Player* chosen = mentionedBots.front().second;
            if (!(g_DisableRepliesInCombat && chosen->IsInCombat()))
            {
                finalCandidates.push_back(chosen);
                if(g_DebugEnabled)
                {
                    LOG_INFO("server.loading", "[Ollama Chat] Bot {} selected (mentioned first at position {})", 
                            chosen->GetName(), mentionedBots.front().first);
                }
            }
        }
        else
        {
            for (Player* bot : candidateBots)
            {
                if (g_DisableRepliesInCombat && bot->IsInCombat())
                {
                    if(g_DebugEnabled)
                    {
                        LOG_INFO("server.loading", "[Ollama Chat] Bot {} skipped - in combat", bot->GetName());
                    }
                    continue;
                }
                uint32_t roll = urand(0, 99);
                if (roll < chance)
                {
                    finalCandidates.push_back(bot);
                    if(g_DebugEnabled)
                    {
                        LOG_INFO("server.loading", "[Ollama Chat] Bot {} PASSED chance roll ({} < {}%)", bot->GetName(), roll, chance);
                    }
                }
                else if(g_DebugEnabled)
                {
                    LOG_INFO("server.loading", "[Ollama Chat] Bot {} FAILED chance roll ({} >= {}%)", bot->GetName(), roll, chance);
                }
            }
        }
    }

    
    if (finalCandidates.empty())
    {
        if(g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] *** NO BOTS RESPONDING *** to {} from {} in {} channel. "
                    "Eligible: {}, Candidates: {}, Final: 0, Chance: {}%",
                    senderIsBot ? "BOT" : "PLAYER", player->GetName(), ChatChannelSourceLocalStr[sourceLocal],
                    eligibleBots.size(), candidateBots.size(), chance);
            LOG_INFO("server.loading", "[Ollama Chat] No eligible bots found to respond to message '{}'. "
                    "Source: {}, Eligible bots: {}, Candidate bots: {}, Combat disabled: {}",
                    msg, ChatChannelSourceLocalStr[sourceLocal], eligibleBots.size(), 
                    candidateBots.size(), g_DisableRepliesInCombat);
        }
        return;
    }
    
    if (finalCandidates.size() > g_MaxBotsToPick)
    {
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(finalCandidates.begin(), finalCandidates.end(), g);
        uint32_t countToPick = urand(1, g_MaxBotsToPick);
        if(g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] Limiting {} bots to {} (MaxBotsToPick)", finalCandidates.size(), countToPick);
        }
        finalCandidates.resize(countToPick);
    }
    
    if(g_DebugEnabled && !finalCandidates.empty())
    {
        std::string botNames;
        for (Player* bot : finalCandidates)
        {
            if (!botNames.empty()) botNames += ", ";
            botNames += bot->GetName();
        }
        LOG_INFO("server.loading", "[Ollama Chat] *** {} BOTS RESPONDING *** to {} from {} in {}: [{}]",
                finalCandidates.size(), senderIsBot ? "BOT" : "PLAYER", player->GetName(),
                ChatChannelSourceLocalStr[sourceLocal], botNames);
    }
    
    // Check for natural language commands from player using LLM
    // Only check if the sender is a real player (not a bot)
    bool const isCommandChannel =
        sourceLocal == SRC_WHISPER_LOCAL || sourceLocal == SRC_PARTY_LOCAL;

    bool shouldAttemptCommandExtraction = g_EnableNaturalLanguageCommands;

    if (g_DebugEnabled)
    {
        LOG_INFO("server.loading", "[Ollama Chat] COMMAND_CHECK: senderIsBot={}, finalCandidates={}, isCommandChannel={}, shouldAttempt={}",
            senderIsBot, finalCandidates.size(), isCommandChannel, shouldAttemptCommandExtraction);
    }

    if (!senderIsBot && !finalCandidates.empty() && isCommandChannel && shouldAttemptCommandExtraction)
    {
        if (IsDuplicateRecentCommandMessage(player, sourceLocal, msg))
        {
            if (g_DebugEnabled)
                LOG_INFO("server.loading", "[Ollama Chat] SKIPPING_COMMAND_CHECK: duplicate_recent_command");
            return;
        }

        ObjectGuid senderGuidObj = player->GetGUID();
        uint32 channelId = channel ? channel->GetChannelId() : 0;
        std::string channelName = channel ? channel->GetName() : "";

        std::vector<ObjectGuid> finalCandidateGuids;
        finalCandidateGuids.reserve(finalCandidates.size());
        for (Player* candidateBot : finalCandidates)
            finalCandidateGuids.push_back(candidateBot->GetGUID());

        std::vector<Player*> commandTargets = finalCandidates;

        if (sourceLocal == SRC_PARTY_LOCAL && commandTargets.size() > 1)
        {
            auto toLowerStr = [](const std::string& str) -> std::string {
                std::string result = str;
                for (char& c : result)
                    c = std::tolower(static_cast<unsigned char>(c));
                return result;
            };

            auto findBotMentionPosition = [&msg, &toLowerStr](const std::string& botName) -> size_t {
                std::string lowerMsg = toLowerStr(msg);
                std::string lowerBotName = toLowerStr(botName);

                size_t pos = 0;
                while ((pos = lowerMsg.find(lowerBotName, pos)) != std::string::npos)
                {
                    bool validStart = (pos == 0 || !std::isalnum(static_cast<unsigned char>(lowerMsg[pos - 1])));
                    size_t endPos = pos + lowerBotName.length();
                    bool validEnd = (endPos >= lowerMsg.length() || !std::isalnum(static_cast<unsigned char>(lowerMsg[endPos])));

                    if (validStart && validEnd)
                        return pos;

                    ++pos;
                }

                return std::string::npos;
            };

            Player* mentionedBot = nullptr;
            size_t firstMentionPos = std::string::npos;
            for (Player* bot : commandTargets)
            {
                size_t pos = findBotMentionPosition(bot->GetName());
                if (pos != std::string::npos && (mentionedBot == nullptr || pos < firstMentionPos))
                {
                    mentionedBot = bot;
                    firstMentionPos = pos;
                }
            }

            if (mentionedBot)
            {
                commandTargets = { mentionedBot };
                if (g_DebugEnabled)
                    LOG_INFO("server.loading", "[Ollama Chat] PARTY_COMMAND_TARGET: using mentioned bot '{}'", mentionedBot->GetName());
            }
            else
            {
                if (g_DebugEnabled)
                    LOG_INFO("server.loading", "[Ollama Chat] PARTY_COMMAND_TARGET: no bot name mentioned, using all {} candidate bots", commandTargets.size());
            }
        }

        std::vector<ObjectGuid> commandTargetGuids;
        commandTargetGuids.reserve(commandTargets.size());
        for (Player* commandBot : commandTargets)
            commandTargetGuids.push_back(commandBot->GetGUID());

        std::thread([senderGuidObj, sourceLocal, msg, channelId, channelName,
            finalCandidateGuids, commandTargetGuids]()
        {
            ParsedNaturalCommand parsedCommand;
            NaturalCommand detectedCmd = NaturalCommand::NONE;

            if (g_DebugEnabled)
                LOG_INFO("server.loading", "[Ollama Chat] EXTRACTING_COMMAND(async): player_message='{}'", msg);

            std::string commandExtractionPrompt = GenerateCommandExtractionPrompt(msg);

            if (g_DebugEnabled)
                LOG_INFO("server.loading", "[Ollama Chat] SUBMITTING_TO_LLM(async): prompt='{}'", commandExtractionPrompt);

            try {
                auto commandFuture = SubmitQuery(commandExtractionPrompt);
                if (commandFuture.valid())
                {
                    if (g_DebugEnabled)
                        LOG_INFO("server.loading", "[Ollama Chat] WAITING_FOR_LLM_RESPONSE(async)");
                    std::string llmResponse = commandFuture.get();
                    if (g_DebugEnabled)
                        LOG_INFO("server.loading", "[Ollama Chat] LLM_RESPONSE(async): '{}'", llmResponse);

                    if (!llmResponse.empty())
                    {
                        parsedCommand = ParseCommandFromLLM(llmResponse);
                        detectedCmd = parsedCommand.command;
                    }
                }
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("server.loading", "[Ollama Chat] EXCEPTION_IN_COMMAND_EXTRACTION(async): {}", e.what());
            }

            PendingNaturalCommandResult pending;
            pending.senderGuid = senderGuidObj;
            pending.sourceLocal = sourceLocal;
            pending.playerMessage = msg;
            pending.channelId = channelId;
            pending.channelName = channelName;
            pending.finalCandidateGuids = finalCandidateGuids;
            pending.commandTargetGuids = commandTargetGuids;
            pending.parsedCommand = parsedCommand;
            pending.detectedCmd = detectedCmd;

            EnqueuePendingNaturalCommandResult(std::move(pending));
        }).detach();

        return;
    }
    else
    {
        if (g_DebugEnabled)
        {
            if (senderIsBot)
                LOG_INFO("server.loading", "[Ollama Chat] SKIPPING_COMMAND_CHECK: sender_is_bot");
            if (finalCandidates.empty())
                LOG_INFO("server.loading", "[Ollama Chat] SKIPPING_COMMAND_CHECK: no_final_candidates");
            if (!isCommandChannel)
                LOG_INFO("server.loading", "[Ollama Chat] SKIPPING_COMMAND_CHECK: unsupported_channel={} (commands allowed only in whisper/party)",
                    ChatChannelSourceLocalStr[sourceLocal]);
            if (!shouldAttemptCommandExtraction)
                LOG_INFO("server.loading", "[Ollama Chat] SKIPPING_COMMAND_CHECK: natural_language_commands_disabled");
            if (!g_EnableNaturalLanguageCommands)
                LOG_INFO("server.loading", "[Ollama Chat] SKIPPING_COMMAND_CHECK: feature_disabled");
        }
    }
    
    ObjectGuid senderGuidObj = player->GetGUID();

    std::vector<ObjectGuid> finalCandidateGuids;
    finalCandidateGuids.reserve(finalCandidates.size());
    for (Player* bot : finalCandidates)
    {
        if (!bot)
            continue;

        float distance = player->GetDistance(bot);
        if (g_DebugEnabled)
            LOG_INFO("server.loading", "[Ollama Chat] Bot {} (distance: {}) is set to respond.", bot->GetName(), distance);

        finalCandidateGuids.push_back(bot->GetGUID());
    }

    QueueBotRepliesForCandidates(finalCandidateGuids, senderGuidObj, msg, sourceLocal, channelId, chanName);
}

static bool IsBotEligibleForChatChannelLocal(Player* bot, Player* player, ChatChannelSourceLocal source, Channel* channel, Player* receiver)
{
    if (!bot || !player || bot == player)
    {
        if (g_DebugEnabled)
            LOG_INFO("server.loading", "[Ollama Chat] IsBotEligible: FAILED basic check - bot={}, player={}, same={}", 
                    (void*)bot, (void*)player, (bot == player));
        return false;
    }
    if (!PlayerbotsMgr::instance().GetPlayerbotAI(bot))
    {
        if (g_DebugEnabled)
            LOG_INFO("server.loading", "[Ollama Chat] IsBotEligible: Bot {} FAILED - no PlayerbotAI", bot->GetName());
        return false;
    }
        
    // For whispers, only the specific receiver should respond
    if (source == SRC_WHISPER_LOCAL)
    {
        // Don't allow bot-to-bot whisper responses
        PlayerbotAI* senderAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);
        if (senderAI && senderAI->IsBotAI())
        {
            return false;
        }
        
        return (receiver && bot == receiver);
    }
    
    // Check team compatibility for non-proximity chats (except channels which can be cross-faction)
    // Say and Yell are proximity-based and don't require same faction
    bool isProximityChatSource = (source == SRC_SAY_LOCAL || source == SRC_YELL_LOCAL);
    if (!channel && !isProximityChatSource && bot->GetTeamId() != player->GetTeamId())
        return false;
    
    // For channels, check if bot is in the specific channel instance
    if (channel)
    {
        // Verify the channel is valid before proceeding
        if (!channel)
        {
            if(g_DebugEnabled)
            {
                LOG_ERROR("server.loading", "[Ollama Chat] IsBotEligibleForChatChannelLocal: Channel is null");
            }
            return false;
        }
            
        // ONLY use exact channel instance check - NO Player::IsInChannel() anymore
        ChannelMgr* candidateCMgr = ChannelMgr::forTeam(bot->GetTeamId());
        if (!candidateCMgr)
            return false;
            
        Channel* candidateChannel = candidateCMgr->GetChannel(channel->GetName(), bot);
        // Verify both channels are valid and are the exact same instance
        if (!candidateChannel || candidateChannel != channel)
        {
            if(g_DebugEnabled)
            {
                LOG_INFO("server.loading", "[Ollama Chat] IsBotEligibleForChatChannelLocal: Bot {} not in same channel instance '{}' - Bot team: {}, Channel ptr: {} vs {}", 
                        bot->GetName(), channel->GetName(), (int)bot->GetTeamId(),
                        (void*)candidateChannel, (void*)channel);
            }
            return false;
        }
        
        // Additional team check for cross-faction channels - only allow same faction unless it's a global channel
        if (bot->GetTeamId() != player->GetTeamId())
        {
            // Allow cross-faction only for specific global channels
            bool isGlobalChannel = (channel->GetName().find("World") != std::string::npos || 
                                   channel->GetName().find("LookingForGroup") != std::string::npos);
            if (!isGlobalChannel)
            {
                if(g_DebugEnabled)
                {
                    LOG_INFO("server.loading", "[Ollama Chat] IsBotEligibleForChatChannelLocal: Bot {} different faction from player - Bot: {}, Player: {}, Channel: '{}'", bot->GetName(), (int)bot->GetTeamId(), (int)player->GetTeamId(), channel->GetName());
                }
                return false;
            }
        }
    }
    
    bool isInParty = (player->GetGroup() && bot->GetGroup() && (player->GetGroup() == bot->GetGroup()));
    float threshold = 0.0f;
    
    switch (source)
    {
        case SRC_SAY_LOCAL:    
            threshold = g_SayDistance;
            if (threshold > 0.0f)
            {
                if (!bot->IsInWorld() || !player->IsInWorld())
                    return false;
                    
                float distance = bot->GetDistance(player);
                return distance <= threshold;
            }
            return false;
            
        case SRC_YELL_LOCAL:   
            threshold = g_YellDistance;
            return (threshold > 0.0f && player->GetDistance(bot) <= threshold);
            
        case SRC_GUILD_LOCAL:
        case SRC_OFFICER_LOCAL:
            return (player->GetGuild() && bot->GetGuildId() == player->GetGuildId());
            
        case SRC_PARTY_LOCAL:
        case SRC_RAID_LOCAL:
            return isInParty;
            
        case SRC_WHISPER_LOCAL:
            // For whispers, the bot should only respond if it's the specific receiver
            return (receiver && bot == receiver);
            
        case SRC_GENERAL_LOCAL:
            // For channels like General, Trade, etc., no distance check - only channel membership matters
            // Channel membership was already checked above
            return true;
            
        default:
            return false;
    }
}

std::string GenerateBotPrompt(Player* bot, std::string playerMessage, Player* player)
{  
    if (!bot || !player) {
        return "";
    }
    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    if (botAI == nullptr) {
        return "";
    }
    ChatHelper* helper = botAI->GetChatHelper();
    if (helper == nullptr) {
        return "";
    }
    if (g_ChatPromptTemplate.empty()) {
        LOG_ERROR("server.loading", "[Ollama Chat] GenerateBotPrompt: template is empty");
        return "";
    }

    AreaTableEntry const* botCurrentArea = botAI->GetCurrentArea();
    AreaTableEntry const* botCurrentZone = botAI->GetCurrentZone();

    uint64_t botGuid                = bot->GetGUID().GetRawValue();
    uint64_t playerGuid             = player->GetGUID().GetRawValue();

    std::string personality         = GetBotPersonality(bot);
    std::string personalityPrompt   = GetPersonalityPromptAddition(personality);
    std::string botName             = bot->GetName();
    uint32_t botLevel               = bot->GetLevel();
    uint8_t botGenderByte           = bot->getGender();
    std::string botAreaName         = botCurrentArea ? botAI->GetLocalizedAreaName(botCurrentArea): "UnknownArea";
    std::string botZoneName         = botCurrentZone ? botAI->GetLocalizedAreaName(botCurrentZone): "UnknownZone";
    std::string botMapName          = bot->GetMap() ? bot->GetMap()->GetMapName() : "UnknownMap";
    std::string botClass            = botAI->GetChatHelper()->FormatClass(bot->getClass());
    std::string botRace             = botAI->GetChatHelper()->FormatRace(bot->getRace());
    std::string botRole             = ChatHelper::FormatClass(bot, AiFactory::GetPlayerSpecTab(bot));
    std::string botGender           = (botGenderByte == 0 ? "Male" : "Female");
    std::string botFaction          = (bot->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");
    std::string botGuild            = (bot->GetGuild() ? bot->GetGuild()->GetName() : "No Guild");
    std::string botGroupStatus      = (bot->GetGroup() ? "In a group" : "Solo");
    uint32_t botGold                = bot->GetMoney() / 10000;

    std::string playerName          = player->GetName();
    uint32_t playerLevel            = player->GetLevel();
    std::string playerClass         = botAI->GetChatHelper()->FormatClass(player->getClass());
    std::string playerRace          = botAI->GetChatHelper()->FormatRace(player->getRace());
    std::string playerRole          = ChatHelper::FormatClass(player, AiFactory::GetPlayerSpecTab(player));
    uint8_t playerGenderByte        = player->getGender();
    std::string playerGender        = (playerGenderByte == 0 ? "Male" : "Female");
    std::string playerFaction       = (player->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");
    std::string playerGuild         = (player->GetGuild() ? player->GetGuild()->GetName() : "No Guild");
    std::string playerGroupStatus   = (player->GetGroup() ? "In a group" : "Solo");
    uint32_t playerGold             = player->GetMoney() / 10000;
    float playerDistance            = player->IsInWorld() && bot->IsInWorld() ? player->GetDistance(bot) : -1.0f;

    std::string chatHistory         = GetBotHistoryPrompt(botGuid, playerGuid, playerMessage);
    std::string sentimentInfo       = GetSentimentPromptAddition(bot, player);

    // Retrieve RAG information if enabled
    std::string ragInfo;
    if (g_EnableRAG && g_RAGSystem) {
        auto ragResults = g_RAGSystem->RetrieveRelevantInfo(playerMessage, g_RAGMaxRetrievedItems, g_RAGSimilarityThreshold);
        std::string ragContent = g_RAGSystem->GetFormattedRAGInfo(ragResults);
        if (!ragContent.empty()) {
            ragInfo = SafeFormat(g_RAGPromptTemplate, fmt::arg("rag_info", ragContent));
        }
        if (g_DebugEnabled) {
            LOG_INFO("server.loading", "[Ollama Chat] RAG Debug - Enabled: {}, System: {}, Message: '{}', Results: {}, Content length: {}",
                g_EnableRAG, (void*)g_RAGSystem, playerMessage, ragResults.size(), ragContent.length());
        }
    } else if (g_DebugEnabled) {
        LOG_INFO("server.loading", "[Ollama Chat] RAG Debug - Not enabled or no system - Enabled: {}, System: {}",
            g_EnableRAG, (void*)g_RAGSystem);
    }

    std::string extraInfo = SafeFormat(
        g_ChatExtraInfoTemplate,
        fmt::arg("bot_race", botRace),
        fmt::arg("bot_gender", botGender),
        fmt::arg("bot_role", botRole),
        fmt::arg("bot_faction", botFaction),
        fmt::arg("bot_guild", botGuild),
        fmt::arg("bot_group_status", botGroupStatus),
        fmt::arg("bot_gold", botGold),
        fmt::arg("player_race", playerRace),
        fmt::arg("player_gender", playerGender),
        fmt::arg("player_role", playerRole),
        fmt::arg("player_faction", playerFaction),
        fmt::arg("player_guild", playerGuild),
        fmt::arg("player_group_status", playerGroupStatus),
        fmt::arg("player_gold", playerGold),
        fmt::arg("player_distance", playerDistance),
        fmt::arg("bot_area", botAreaName),
        fmt::arg("bot_zone", botZoneName),
        fmt::arg("bot_map", botMapName)
    );
    
    std::string prompt = SafeFormat(
        g_ChatPromptTemplate,
        fmt::arg("bot_name", botName),
        fmt::arg("bot_level", botLevel),
        fmt::arg("bot_class", botClass),
        fmt::arg("bot_personality", personalityPrompt),
        fmt::arg("bot_personality_name", personality),
        fmt::arg("player_level", playerLevel),
        fmt::arg("player_class", playerClass),
        fmt::arg("player_name", playerName),
        fmt::arg("player_message", playerMessage),
        fmt::arg("extra_info", extraInfo),
        fmt::arg("chat_history", chatHistory),
        fmt::arg("sentiment_info", sentimentInfo)
    );

    // Add RAG information to the prompt if available
    if (!ragInfo.empty()) {
        prompt += ragInfo + "\n";
    }

    if(g_EnableChatBotSnapshotTemplate)
    {
        prompt += GenerateBotGameStateSnapshot(bot);
    }

    // Debug logging for full prompt including RAG information
    if (g_DebugEnabled && g_DebugShowFullPrompt) {
        LOG_INFO("server.loading", "[Ollama Chat] Full prompt sent to bot {} for player {}: {}", botName, playerName, prompt);
    }

    return prompt;
}
