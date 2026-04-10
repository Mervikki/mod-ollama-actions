# AzerothCore + Playerbots Module: mod-ollama-actions

> **Note:** This is a fork and enhancement of the original [mod-ollama-chat by DustinHendrickson](https://github.com/DustinHendrickson/mod-ollama-chat). While the original module focused solely on generating immersive chat responses using local LLMs, this modification adds **Natural Language Action Commands**, allowing playerbots to intelligently understand your intent and execute commands (like attacking, casting specific spells, or trading) based on natural conversational cues.

> [!CAUTION]
> **LLM/AI Disclaimer:** Large Language Models (LLMs) such as those used by this module do not possess intelligence, reasoning, or true understanding. They generate text by predicting the most likely next word based on patterns in their training data. Results may vary, and sometimes the output may fall back to default behavior. Use with realistic expectations.
>
> This module is also in development and can require substantial system resources depending on the local LLM model you run.

> [!IMPORTANT]
> To fully disable Playerbots normal chatter and random chatter that might interfere with this module, set the following settings in your `playerbots.conf`:
> - `AiPlayerbot.EnableBroadcasts = 0` (disables loot/quest/kill broadcasts)
> - `AiPlayerbot.RandomBotTalk = 0` (disables random talking in say/yell/general channels)
> - `AiPlayerbot.RandomBotEmote = 0` (disables random emoting)
> - `AiPlayerbot.RandomBotSuggestDungeons = 0` (disables dungeon suggestions)
> - `AiPlayerbot.EnableGreet = 0` (disables greeting when invited)
> - `AiPlayerbot.GuildFeedback = 0` (disables guild event chatting)
> - `AiPlayerbot.RandomBotSayWithoutMaster = 0` (disables bots talking without a master)

## Overview

***mod-ollama-actions*** transforms your AzerothCore Playerbots from static scripted followers into intelligent companions! By integrating an external language model (LLM) via the Ollama API, bots can now not only hold dynamic, in-character conversations, but also interpret your natural speech to execute appropriate in-game commands.

Tell a bot, *"Pull that group!"*, *"Heal me quickly!"*, or *"Let's see what you have to trade"*, and the LLM will map your request into backend actions (Attack, Cast, Trade, etc.).

## Key Features

- **Natural Language Bot Actions:**  
  Bots analyze your chat messages to determine intent. Without needing strict prefixes or syntax, you can ask bots to:
  - **Follow / Stay**
  - **Attack** (can resolve specific targets like "skull" or "moon")
  - **Flee** (retreat to leader)
  - **Cast Spells** (dynamically determines the spell and target, e.g., "Cast Flash Heal on me")
  - **Toggle AoE Avoidance**
  - **Open Trade**

- **Extensible Command List (`bot_commands.txt`):**  
  You can provide an external command list file to teach the LLM additional Playerbot command mappings (for example formations, strategies, loot rules, and utility commands) without writing new C++ handlers for each command.
  - Commands from this list are parsed as `COMMAND:<raw playerbot command>`.
  - Core fast-path commands (follow/stay/attack/cast/trade/aoe/flee) are detected first; the extended command list is only consulted as a fallback.
  
- **Context-Aware Prompt Generation:**  
  The module gathers extensive context about both the bot and the interacting player—including class, race, role, faction, guild, and more.
  
- **Player Bot Personalities:**  
  Each bot can be assigned a personality type (e.g., Gamer, Roleplayer, Trickster) that influences how they speak when acknowledging your commands or bantering.

- **Conversation Memory:**  
  Bots maintain short-term chat memory. Recent interactions with specific players are passed to the model to provide conversational continuity.

- **Event-Based Chatter & Random Banter:**  
  Playerbots comment on key in-game events such as quest completions, rare loot drops, deaths, leveling up, and more. When idle, bots may randomly generate environment-aware chatter.

- **Think Mode Support:**  
  Leverage advanced reasoning models (like DeepSeek) by enabling `ThinkModeEnableForModule = 1`. Internal reasoning is sent but "thinking" output is filtered out before chatting.

- **Live Reload & Configuration:**  
  Hot-reload config and bot personality packs on the fly via the `.ollama reload` GM command without a server restart. No messy reboots after tuning your bot's behavior.

## Installation

> [!IMPORTANT]
> **Cross-Platform Support**: This module uses cpp-httplib, meaning there are no curl dependencies and it natively compiles smoothly on Windows, Linux, and macOS.

1. **Prerequisites:**
   - Active [AzerothCore](https://github.com/azerothcore/azerothcore-wotlk) environment with Playerbots installed.
   - **fmtlib** (e.g. `vcpkg install fmt` / `sudo apt install libfmt-dev`)
   - **Ollama**: A running [Ollama server](https://ollama.com/) (local or remote).

2. **Clone the Module:**
   ```bash
   cd /path/to/azerothcore/modules
   git clone https://github.com/Mervikki/mod-ollama-actions.git mod-ollama-chat
   ```
   *(Note: Keeping the folder name as `mod-ollama-chat` is recommended for CMake to discover it smoothly, or follow your server's module structure).*

3. **Recompile AzerothCore:**
   ```bash
   cd /path/to/azerothcore && make -j$(nproc) && make install
   ```

4. **Configuration:**
   Copy the default configuration file to your server configuration directory and adjust it to fit your setup:
   ```bash
   cp /path/to/azerothcore/modules/mod-ollama-chat/conf/mod_ollama_chat.conf.dist /path/to/azerothcore/env/dist/etc/mod_ollama_chat.conf
   ```

5. **Optional: Add an Extended Command List**
   - Create a command list file in your server install, for example:
     - `/path/to/server/data/bot_commands.txt`
   - Set the config value in `mod_ollama_chat.conf`:
     ```ini
     OllamaChat.BotCommandsPath = "../data/bot_commands.txt"
     ```
   - Recommended format per line:
     ```text
     command | intent tags | short meaning
     ```
     Example:
     ```text
     co +avoid aoe | movement defensive aoe | avoid harmful area effects
     formation line | formation spread positioning | arrange bots in a line
     ```

## Setting up Ollama Server

Download and install Ollama from [ollama.com](https://ollama.com). 

**Pulling a Model:**  
Before using the module, pull the specific model your bots will use (we recommend a fast computing model for prompt action execution):
```bash
ollama pull llama3.2:1b
```

If your Ollama server is hosted on a different device than your AzerothCore server, be sure to bind Ollama universally:
```bash
export OLLAMA_HOST=0.0.0.0
ollama serve
```
Then update the `OllamaChat.ApiEndpoint` in your `.conf` file to point to its IP (e.g., `http://192.168.1.100:11434`).

## Available In-Game Commands

Administrators (GM level 3+) can use the following commands to manage the system on the fly:

- `.ollama reload` - Hot-reloads all config values and personality SQL packs.
- `.ollama personality set <bot_name> <personality>` - Override a bot's randomly generated personality.
- `.ollama personality get <bot_name>` - See a bot's current personality.
- `.ollama personality list` - List available personalities.

Review the configuration file for extensive customization over trigger chances, intent extraction formatting, LLM contexts, and chat rate limiting!

## Credits / License

- Originally built by: [DustinHendrickson](https://github.com/DustinHendrickson/mod-ollama-chat) (Chat system & core pipeline)
- Forked & Expanded by: **Mervikki** (Natural Language Actions & Intent Resolution)
- Released under GNU GPL v3 license (consistent with AzerothCore).
