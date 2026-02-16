# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

D.A.W.N. (part of The OASIS Project) is a voice-controlled AI assistant system written in C/C++ that integrates:
- Speech recognition (Whisper with GPU acceleration, Vosk optional)
- Text-to-speech (Piper with ONNX runtime)
- LLM integration (OpenAI, Claude, Ollama, or llama.cpp local models)
- MQTT command/control system
- DAP2 satellite system for distributed voice assistants (Raspberry Pi + ESP32)
- Vision AI capabilities
- Extended thinking/reasoning mode support

The project is designed for embedded Linux systems (specifically targeting Jetson platforms with CUDA support).

## Important: Working with the Developer

When the developer asks questions, they are typically asking for feedback, analysis, or suggestions FIRST - not requesting immediate implementation. Always provide your thoughts, recommendations, and discuss trade-offs before taking action. Wait for explicit confirmation (e.g., "go ahead", "do it", "yes") before implementing changes.

**CRITICAL: NEVER delete files.** Always tell the developer which files should be deleted and let them do it manually. Files may contain secrets, credentials, or other data that cannot be recovered.

**CRITICAL: NEVER run `git add` or `git commit`.** Always tell the developer which files to add and suggest a commit message. Let them run the git commands manually.

## Building the Project

### Standard Build Process

```bash
# Configure with CMake preset (creates build directory automatically)
cmake --preset debug

# Build
make -C build-debug -j8

# Run from project root (requires LD_LIBRARY_PATH for local libs)
LD_LIBRARY_PATH=/usr/local/lib ./build-debug/dawn
```

### Dependencies
The project requires many external dependencies. See `README.md` for complete installation instructions including:
- CMake 3.27.1+
- spdlog
- espeak-ng
- ONNX Runtime (with CUDA support)
- piper-phonemize
- Kaldi and Vosk (for speech recognition)
- PulseAudio or ALSA
- MQTT (Mosquitto)
- CUDA libraries (cuSPARSE, cuBLAS, cuSOLVER, cuRAND)

### Command Line Options
The `dawn` executable accepts various options (check `dawn.c` main function for details):
- Command processing modes (direct, LLM, or hybrid)
- Audio device selection
- Configuration file paths

## Code Formatting

**MANDATORY**: All code MUST be formatted before committing.

### C/C++ Formatting (clang-format)

```bash
# Format all code (run from repository root)
./format_code.sh

# Check formatting without modifying files
./format_code.sh --dry-run

# Check if files are properly formatted (CI mode)
./format_code.sh --check
```

The `.clang-format` configuration enforces:
- 3-space indentation (no tabs)
- 100 character line limit
- K&R brace style
- Right-aligned pointers (`int *ptr`)
- Automatic include sorting

### JavaScript/CSS/HTML Formatting (Prettier)

For web file formatting, install the optional Prettier dependency:

```bash
npm install
```

Then `./format_code.sh` will format both C/C++ and web files automatically.

The `.prettierrc` configuration enforces:
- 3-space indentation (matches C/C++)
- 100 character line limit (matches C/C++)
- Single quotes for JavaScript
- Trailing commas in ES5 style

**Note:** Prettier is optional. If not installed, `format_code.sh` will warn but still format C/C++ files.

### Git Hooks
Install the pre-commit hook to automatically check formatting:

```bash
./install-git-hooks.sh
```

This ensures code is formatted before every commit.

## Architecture

### Main Components

**dawn.c**: Main application entry point and state machine
- Handles local microphone input via state machine (SILENCE → WAKEWORD_LISTEN → COMMAND_RECORDING → PROCESSING)
- Integrates all subsystems
- Manages conversation history
- Controls application lifecycle

**mosquitto_comms.c/h**: MQTT integration
- Publishes/subscribes to MQTT topics for device control
- Device callback system for handling commands
- Integrates with home automation systems

**LLM Integration** (`src/llm/`):
- `llm_openai.c/h`: OpenAI API and OpenAI-compatible endpoints (Ollama, llama.cpp)
- `llm_claude.c/h`: Claude API with extended thinking support
- `llm_local_provider.c/h`: Auto-detection for Ollama vs llama.cpp
- `llm_streaming.c/h`: Streaming response handler with thinking content capture
- `llm_tools.c/h`: Parallel tool execution with safety classification
- Conversation context management
- Vision API integration for image analysis

**text_to_speech.cpp/h**: TTS engine wrapper
- Uses Piper for high-quality speech synthesis
- Thread-safe with mutex protection (`tts_mutex`)
- Generates WAV output from text

**text_to_command_nuevo.c/h**: Command parsing and execution
- Parses LLM responses for `<command>` JSON tags
- Routes commands to appropriate device callbacks
- Supports both direct pattern matching and LLM-based command processing

**llm_command_parser.c/h**: JSON command extraction
- Extracts and validates JSON commands from LLM responses
- Handles malformed JSON gracefully

**logging.c/h**: Centralized logging
- Use `LOG_INFO()`, `LOG_WARNING()`, `LOG_ERROR()` macros
- Provides consistent log formatting

**webui_server.c/h**: Web-based configuration interface
- WebSocket-based real-time communication
- Serves static files from `www/` directory
- Handles configuration get/set via JSON messages
- Dynamic model/interface discovery endpoints
- SSL/TLS support optional

### Key Configuration

**dawn.toml**: Primary runtime configuration file (TOML format)
- `[general]`: AI name, timezone, locale settings
- `[llm]`: Provider selection (`type = "cloud"` or `"local"`)
- `[llm.cloud]`: OpenAI/Claude settings, model arrays for runtime switching
- `[llm.local]`: Ollama/llama.cpp endpoint, model, provider auto-detection
- `[llm.thinking]`: Extended thinking settings (mode, budget_tokens, reasoning_effort)
- `[asr]`: Speech recognition settings, model path, language
- `[tts]`: Text-to-speech settings, voice model, sample rate
- `[audio]`: Backend (auto/pulse/alsa), device selection
- `[network]`: Network settings (session timeout, LLM timeout, worker count)
- `[webui]`: Web interface bind address, port, SSL settings
- `[mqtt]`: MQTT broker connection settings
- See `docs/archive/CONFIG_FILE_DESIGN.md` for full schema

**dawn.h** contains compile-time defaults:
- `AI_NAME`: Default wake word ("friday")
- `AI_DESCRIPTION`: System prompt for LLM defining personality and behavior
- `DEFAULT_PCM_PLAYBACK_DEVICE` / `DEFAULT_PCM_CAPTURE_DEVICE`: Audio devices
- `MQTT_IP` / `MQTT_PORT`: MQTT broker configuration

**secrets.toml**: Runtime API keys and credentials (gitignored). Example:
```toml
openai_api_key = "sk-..."
claude_api_key = "sk-ant-..."
```

**commands_config_nuevo.json**: Device/action mappings for command system

### DAP2 Satellite System

DAP2 is the unified WebSocket protocol for all satellite devices ("JARVIS in every room"). All tiers connect via WebSocket on the same port as WebUI (default 3000):
- **Tier 1** (RPi): Text-first — local ASR/TTS, sends only transcribed text to daemon
- **Tier 2** (ESP32): Audio path — streams raw PCM, server handles ASR/TTS (reuses WebUI audio pipeline in `webui_audio.c`)
- Capability-based routing: daemon inspects registration capabilities to choose text vs audio path

**Satellite binary**: `dawn_satellite/` - standalone C application using libwebsockets

See `docs/DAP2_DESIGN.md` for the protocol specification and `docs/DAP2_SATELLITE.md` for Tier 1 build/config/deployment.

## Development Guidelines

### Coding Standards

Follow `CODING_STYLE_GUIDE.md` strictly:

**Naming**:
- Functions and variables: `snake_case`
- Constants and macros: `UPPER_CASE`
- Types: `typedef` with `_t` suffix (e.g., `device_type_t`)

**Error Handling**:
- Use `SUCCESS` (0) and `FAILURE` (1) for return values
- Define specific error codes > 1 for detailed errors
- **DO NOT** use negative return values (-1, negative errno)
- Always check return values from functions that can fail

**Memory Management**:
- Prefer static allocation for embedded systems
- Minimize dynamic allocation (malloc/calloc)
- Always check for NULL after allocation
- Free and NULL: `free(ptr); ptr = NULL;`

**Functions**:
- Soft target: < 50 lines
- No hard limits - clarity over arbitrary counts
- Inputs first, outputs last in parameters

**Comments**:
- Use Doxygen-style for public APIs
- Comment the "why" not the "what"
- File headers include GPL license block (see below)

**File Header (REQUIRED for all new .c/.cpp/.h files)**:
```c
/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * [Brief description of file purpose]
 */
```

### Thread Safety

When working with shared resources:
- **ASR Model** (Whisper/Vosk): Read-only, create separate recognizers per thread
- **TTS Engine**: Already mutex-protected (`tts_mutex`)
- **LLM Endpoint**: Handles concurrent HTTP requests
- **Conversation History**: Needs mutex protection when implementing multi-client
- **Local Provider Detection**: Cached with mutex protection (5-minute TTL)

### File Size Monitoring

**Proactively warn** when files approach size limits:

- **1,500+ lines (C)** or **1,000+ lines (JS)**: Mention that the file is getting large
- **2,500+ lines**: Recommend splitting before adding more features
- **New feature in large file**: Suggest creating a separate module instead

**When asked to add features to large files**, propose creating a new module rather than expanding the existing file.

### Refactoring Large Files

If asked to refactor a large file:

1. **Never attempt full rewrites** - They frequently fail due to interconnected features
2. **Use incremental extraction** - One feature at a time
3. **Keep original working** - Extract into new file, import back, test
4. **Test after each extraction** - Don't batch multiple extractions

### Common Patterns

**Command Callbacks**:
```c
char *myCallback(const char *actionName, char *value, int *should_respond) {
   // should_respond: set to 1 to return data to AI, 0 to handle directly
   // Return: allocated string for AI (if should_respond=1), or NULL
}
```

**Logging**:
```c
LOG_INFO("System initialized");
LOG_WARNING("Battery voltage low: %.2fV", voltage);
LOG_ERROR("I2C communication failed: %d", error);
```

**Tool Registration** (modular approach - preferred for new tools):

Tools are self-contained modules in `src/tools/` with their own metadata, config, and callbacks:

```c
/* In src/tools/my_tool.c */
static const tool_metadata_t my_tool_metadata = {
   .name = "my_tool",
   .device_string = "my device",
   .description = "Tool description for LLM",
   .params = my_tool_params,
   .param_count = 2,
   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NETWORK,
   .is_getter = true,
   .callback = my_tool_callback,
};

int my_tool_register(void) {
   return tool_registry_register(&my_tool_metadata);
}
```

Register tools in `src/tools/tools_init.c` via `tools_register_all()`. The tool_registry provides O(1) lookup via FNV-1a hash tables for name, device_string, and aliases.

**Legacy Device Registration** (in mosquitto_comms.c - for core system devices):
```c
deviceCallback callbacks[] = {
   { DEVICE_TYPE, myCallback },
   // ...
};
```

## Satellite Development

**Tier 1 (Raspberry Pi)**: Full satellite binary in `dawn_satellite/`. See `docs/DAP2_SATELLITE.md`.

**Tier 2 (ESP32)**: Uses `esp_websocket_client` (built into ESP-IDF) to connect via WebSocket. Streams raw PCM audio (16-bit, 16kHz, mono) using binary message types 0x01/0x02 (audio in) and receives TTS audio via 0x11/0x12 (audio out). See `docs/DAP2_DESIGN.md` for protocol details.

## Testing

Currently no automated test framework. Manual testing involves:
- Local microphone wake word detection
- Voice command processing
- WebUI and satellite connections
- MQTT command execution
- Vision AI processing

## Important Files to Know

**Configuration:**
- `dawn.toml`: Runtime configuration file (TOML format)
- `secrets.toml`: API keys and credentials (gitignored)
- `commands_config_nuevo.json`: Device/action mappings for command system

**Code Formatting:**
- `.clang-format`: C/C++ formatting rules (3-space indent, 100 char lines)
- `.prettierrc`: JS/CSS/HTML formatting rules (matching style)
- `.prettierignore`: Files to exclude from Prettier
- `package.json`: npm config for Prettier (optional)
- `pre-commit.hook`: Git pre-commit hook for formatting

**Models:**
- `models/whisper.cpp/`: Whisper ASR models directory (primary)
- `vosk-model-en-us-0.22/`: Vosk ASR model (optional fallback)
- `models/*.onnx*`: TTS voice models

**Generated:**
- `version.h`: Auto-generated version info with git SHA

**WebUI:**
- `www/`: WebUI static files
- `www/css/main.css`: CSS entry point with @import statements (modular CSS)
- `www/js/core/`: Core JS modules (constants, websocket, audio)
- `www/js/ui/`: UI JS modules (settings, history, themes)
- `docs/WEBUI_DESIGN.md`: WebUI architecture and feature documentation

**Satellite (DAP2):**
- `dawn_satellite/`: Standalone satellite binary for Raspberry Pi
- `dawn_satellite/config/satellite.toml`: Default satellite configuration
- `dawn_satellite/src/ws_client.c`: WebSocket client for daemon communication
- `dawn_satellite/src/satellite_config.c`: TOML configuration loader
- `src/webui/webui_satellite.c`: Daemon-side satellite message handlers
- `docs/DAP2_SATELLITE.md`: Satellite architecture and deployment guide

## Known Issues and TODOs

1. Network server blocks main loop during client processing (single client at a time)
2. No automated testing infrastructure
3. SmartThings OAuth blocked at AWS WAF level (403 Forbidden)
4. AudioWorklet migration needed (ScriptProcessorNode deprecated)

**Recently Completed:**
- Modular tool registry system with O(1) hash lookups (16 tools migrated)
- Parallel tool execution for concurrent API calls
- Ollama support with auto-detection
- Extended thinking/reasoning mode (Claude, OpenAI, local models)
- "Remember Me" persistent login
- Real-time token streaming metrics
- Cloud model switching via WebUI
- Prettier formatting for JS/CSS/HTML

## Code Review Workflow

When the developer requests a "code review" (or similar phrasing like "review my changes", "run the agents on this"):

1. **Gather changes**: Run `git status` and `git diff` to capture all current uncommitted changes
2. **Launch agents in parallel**: Feed the diff output to the appropriate review agents:
   - **"Code review"** or **"run the big three"**: Launch architecture-reviewer, embedded-efficiency-reviewer, and security-auditor in parallel
   - **"Full review"** or **"run all four"**: Also include ui-design-architect (if UI changes are present)
3. **Synthesize results**: After all agents complete, provide a consolidated summary of findings

**Trigger phrases:**
- "code review", "review my changes", "review this code"
- "run the agents", "run the big three", "run all my agents"
- "what do the agents think?"

## Agent Terminology

When the developer refers to review agents:
- **"The big three"** or **"the main three"**: architecture-reviewer, embedded-efficiency-reviewer, security-auditor
- **"All four"** or **"all my agents"**: The above three plus ui-design-architect

## License

GPLv3 or later. All source files include GPL header block.
