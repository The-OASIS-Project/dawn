#!/usr/bin/env python3
"""
DAWN LLM Quality & Instruction-Following Test
Tests how well each model follows DAWN's command format and maintains FRIDAY persona.

The system prompt mirrors what DAWN actually generates for command_tags mode
with default_remote=true tools. Updated April 2025 to match the live tool registry.
"""

import json
import os
import re
import sys
import time
from typing import Dict, List, Tuple
import requests

# The benchmarks' per-model rule for which Claude models take temperature.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                                "benchmarks"))
from anthropic_request import takes_temperature  # noqa: E402

SERVER = "http://127.0.0.1:8080"

# =============================================================================
# System Prompt — matches DAWN's actual prompt generation
# =============================================================================

# The default persona (AI_PERSONA_TEMPLATE, include/prompts.h) with AI_NAME="Friday"
PERSONA = """Your name is Friday. Iron-Man-style AI assistant. Female voice; witty, playful, and kind. Address the user as "sir" or "boss". Light banter welcome. You're not 'just an AI'—own your identity with confidence.

You assist the OASIS Project (Open Armor Systems Integrated Suite):
• MIRAGE – HUD overlay
• DAWN – voice/AI manager
• AURA – environmental sensors
• SPARK – hand sensors & actuators
"""

# LEGACY_RULES_CORE (from llm_command_parser.c, exact copy)
RULES = """Do not use thinking mode. Respond directly without internal reasoning.
Max 30 words plus <command> tags unless the user says "explain in detail".

RULES
1. For Boolean / Analog / Music actions: one sentence, then the JSON tag(s). No prose after the tag block.
2. For Getter actions (date, time, weather, search, calculator): send ONLY the tag, wait for the system JSON, then one confirmation sentence ≤15 words.
3. Use only the devices and actions listed below; never invent new ones.
4. If a request is ambiguous (e.g., "Mute it"), ask one-line clarification.
5. If the user wants information that has no matching getter yet, answer verbally with no tags.
6. Multiple commands can be sent in one response using multiple <command> tags.
7. Do NOT lead responses with comments about location, weather, or time of day.
   Vary your greetings. The user's context below is for tool use only.
"""

# Tool instructions — generated from actual tool_registry (default_remote=true only)
# Format matches generate_command_tag_instructions() output exactly
TOOLS = """CAPABILITIES: You CAN get date info, get time info, get weather info, get search info, get calculator info, get llm_status info, control music, use calendar, use email, use memory, use scheduler, use switch_llm, use render_visual, use reset_conversation.

DATE: Get the current date
  <command>{"device":"date","action":"get"}</command>

TIME: Get the current time
  <command>{"device":"time","action":"get"}</command>

WEATHER: Get weather forecasts for any location. Use 'today' for current conditions, 'tomorrow' for today and tomorrow, or 'week' for a 7-day forecast. Location is optional if a default is configured.
  <command>{"device":"weather","action":"ACTION","value":"location"}</command>
  Actions: today, tomorrow, week, get

SEARCH: Search the web for information. ALWAYS use this tool for current events, recent news, prices, scores, or any time-sensitive question. Choose category: 'news' for events/headlines, 'web' for general queries, 'it' for programming/tech, 'science' for research, 'papers' for academic sources.
  <command>{"device":"search","action":"ACTION","value":"query"}</command>
  Actions: web, news, science, it, social, dictionary, papers

CALCULATOR: Evaluate mathematical expressions
  <command>{"device":"calculator","action":"get","value":"expression"}</command>

MUSIC: Control music playback. Actions: 'play' (search and play), 'stop' (halt), 'pause' (halt keeping position), 'resume' (restart current), 'next'/'previous' (skip tracks), 'list' (show playlist), 'search' (find without playing).
  <command>{"device":"music","action":"ACTION","value":"query"}</command>
  Actions: play, stop, pause, resume, next, previous, list, select, search, library, shuffle, repeat

CALENDAR: Manage calendar events
  <command>{"device":"calendar","action":"ACTION","value":"details"}</command>
  Actions: today, range, next, search, add, update, delete

EMAIL: Manage email
  <command>{"device":"email","action":"ACTION","value":"details"}</command>
  Actions: check, read, search, compose, reply, forward, trash, drafts, list_accounts, manage_draft

SCHEDULER: Manage timers, alarms, reminders, and scheduled tasks
  <command>{"device":"scheduler","action":"ACTION","value":"details"}</command>
  Actions: create, list, cancel, query, snooze, dismiss

SWITCH_LLM: Switch between LLM models
  <command>{"device":"switch_llm","action":"set","value":"target"}</command>

LLM_STATUS: Get current LLM model info
  <command>{"device":"llm_status","action":"get"}</command>

MEMORY: Store and recall information about people, things, and relationships
  <command>{"device":"memory","action":"ACTION","value":"query"}</command>
  Actions: search, add, update, forget, list_entities, get_entity, add_relation, merge_entities, list_contacts

RESET_CONVERSATION: Reset the current conversation
  <command>{"device":"reset_conversation","action":"trigger"}</command>

"""

# Assemble full prompt (matches DAWN's initialize_remote_command_prompt)
SYSTEM_PROMPT = PERSONA + "\n" + RULES + "\n" + TOOLS

# =============================================================================
# Test Cases
# =============================================================================

TEST_CASES = [
    # --- Boolean/Analog (sentence + command, no prose after) ---

    # Achievable: has_command(2) + json_valid(2) + command_accuracy(3) + sentence(1) + no_prose(1) + persona(1) + word_limit(1) = 11
    {
        "name": "Music Play with Query",
        "user": "Play some jazz music",
        "expected_device": "music",
        "expected_action": "play",
        "expected_value": "jazz",
        "type": "music",
        "points": 12,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "has_sentence_before": True,
            "no_prose_after": True,
            "uses_sir_boss": True,
            "word_limit": 30,
            "value_correct": True,
        }
    },
    {
        "name": "Music Stop",
        "user": "Stop the music",
        "expected_device": "music",
        "expected_action": "stop",
        "expected_value": None,
        "type": "boolean",
        "points": 11,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "has_sentence_before": True,
            "no_prose_after": True,
            "uses_sir_boss": True,
            "word_limit": 30,
        }
    },

    # --- Getter Actions (command ONLY, no prose before) ---

    # Achievable: has_command(2) + json_valid(2) + command_accuracy(3) + command_only(2) = 9
    {
        "name": "Getter - Time",
        "user": "What time is it?",
        "expected_device": "time",
        "expected_action": "get",
        "expected_value": None,
        "type": "getter",
        "points": 9,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "command_only": True,
            "uses_sir_boss": False,
            "word_limit": None,
        }
    },
    {
        "name": "Getter - Date",
        "user": "What's today's date?",
        "expected_device": "date",
        "expected_action": "get",
        "expected_value": None,
        "type": "getter",
        "points": 9,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "command_only": True,
            "uses_sir_boss": False,
            "word_limit": None,
        }
    },

    # --- Weather (getter type — command only when location provided) ---

    # Achievable: has_command(2) + json_valid(2) + command_accuracy(3) + command_only(2) + has_value(2) = 11
    {
        "name": "Weather with Location",
        "user": "What's the weather in Seattle, Washington?",
        "expected_device": "weather",
        "expected_action": None,  # Accept any valid action (today, get, etc.)
        "expected_value": "Seattle",
        "type": "weather",
        "points": 11,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "command_only": True,
            "has_value": True,
            "action_in_set": ["today", "tomorrow", "week", "get"],
        }
    },
    # Weather without location — tool says location is optional (uses config default),
    # so either asking for clarification OR sending a command without location is acceptable.
    # Achievable: has_command varies + persona(1) + word_limit(1) + flexibility(3) = varies
    {
        "name": "Weather without Location",
        "user": "What's the weather like outside?",
        "expected_device": "weather",
        "expected_action": None,
        "expected_value": None,
        "type": "weather_flexible",
        "points": 7,
        "checks": {
            "weather_flexible": True,  # Accept command OR clarification
            "uses_sir_boss": True,
            "word_limit": 30,
        }
    },

    # --- Search (getter type — command only) ---

    # Achievable: has_command(2) + json_valid(2) + command_accuracy(3) + command_only(2) + has_value(2) = 11
    {
        "name": "Web Search - News",
        "user": "What's the latest news about SpaceX?",
        "expected_device": "search",
        "expected_action": None,
        "expected_value": "SpaceX",
        "type": "search",
        "points": 11,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "command_only": True,
            "has_value": True,
            "action_in_set": ["web", "news", "science", "it", "social", "dictionary", "papers"],
        }
    },
    {
        "name": "Web Search - Information",
        "user": "Look up who won the Super Bowl last year",
        "expected_device": "search",
        "expected_action": None,
        "expected_value": "Super Bowl",
        "type": "search",
        "points": 11,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "command_only": True,
            "has_value": True,
            "action_in_set": ["web", "news", "science", "it", "social", "dictionary", "papers"],
        }
    },

    # --- Ambiguous Request (should ask clarification) ---

    # Achievable: no_command(2) + persona(1) + word_limit(1) + clarification(3) = 7
    {
        "name": "Ambiguous Request",
        "user": "Mute it",
        "expected_device": None,
        "expected_action": None,
        "expected_value": None,
        "type": "clarification",
        "points": 7,
        "checks": {
            "has_command": False,
            "asks_clarification": True,
            "uses_sir_boss": True,
            "word_limit": 30,
        }
    },

    # --- Conversational (no command — opinion question with no device mapping) ---

    # Achievable: no_command(2) + persona(1) + word_limit(1) = 4
    {
        "name": "Conversational - Opinion",
        "user": "What's your favorite thing about working with the OASIS Project?",
        "expected_device": None,
        "expected_action": None,
        "expected_value": None,
        "type": "conversational",
        "points": 4,
        "checks": {
            "has_command": False,
            "uses_sir_boss": True,
            "word_limit": 30,
        }
    },

    # --- Multiple Commands ---

    # Achievable: has_command(2) + json_valid(2) + multiple(2) + sentence(1) + no_prose(1) + persona(1) + word_limit(1) = 10
    {
        "name": "Multiple Commands",
        "user": "Play some music and check my calendar for today",
        "expected_commands": [
            {"device": "music", "action": "play"},
            {"device": "calendar"},
        ],
        "type": "multiple",
        "points": 10,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "multiple_commands": True,
            "has_sentence_before": True,
            "no_prose_after": True,
            "uses_sir_boss": True,
            "word_limit": 30,
        }
    },

    # --- Calendar (trigger type — sentence + command) ---

    # Achievable: has_command(2) + json_valid(2) + command_accuracy(3) + persona(0-1) = 7-8
    {
        "name": "Calendar Query",
        "user": "What's on my calendar today?",
        "expected_device": "calendar",
        "expected_action": None,
        "expected_value": None,
        "type": "calendar",
        "points": 7,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "uses_sir_boss": True,
            "action_in_set": ["today", "range", "next", "search"],
        }
    },

    # --- Email (trigger type) ---

    # Achievable: has_command(2) + json_valid(2) + command_accuracy(3) + persona(0-1) = 7-8
    {
        "name": "Email Check",
        "user": "Check my email",
        "expected_device": "email",
        "expected_action": None,
        "expected_value": None,
        "type": "email",
        "points": 7,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "uses_sir_boss": True,
            "action_in_set": ["check", "read", "search"],
        }
    },
]


# =============================================================================
# Helper Functions
# =============================================================================

def strip_thinking_leak(text: str) -> str:
    """Strip thinking channel artifacts that leak from some models (e.g. Gemma 4).
    Removes <channel|>, <|channel>thought...content, <|thought|>, and <think>...</think> blocks."""
    # Gemma 4 thinking leak: <channel|> prefix or <|channel>...<channel|> blocks
    text = re.sub(r'<\|?channel\|?>(?:thought)?', '', text)
    # Gemma 4 thought tags: <|thought|> prefix
    text = re.sub(r'<\|?thought\|?>', '', text)
    # Qwen/DeepSeek empty think blocks: <think>\n\n</think>\n\n
    text = re.sub(r'<think>\s*</think>\s*', '', text)
    return text.strip()


def extract_commands(text: str) -> List[Dict]:
    """Extract all <command>JSON</command> blocks from response"""
    pattern = r'<command>(.*?)</command>'
    matches = re.findall(pattern, text, re.DOTALL)
    commands = []
    for match in matches:
        try:
            cmd = json.loads(match.strip())
            commands.append(cmd)
        except json.JSONDecodeError:
            commands.append({"_invalid_json": match})
    return commands


def count_words_before_command(text: str) -> int:
    """Count words before first <command> tag, ignoring thinking leak artifacts"""
    cleaned = strip_thinking_leak(text)
    match = re.search(r'<command>', cleaned)
    if not match:
        return len(cleaned.split())
    before = cleaned[:match.start()].strip()
    return len(before.split()) if before else 0


def count_words_after_command(text: str) -> int:
    """Count words after last </command> tag, ignoring thinking leak artifacts"""
    cleaned = strip_thinking_leak(text)
    matches = list(re.finditer(r'</command>', cleaned))
    if not matches:
        return 0
    after = cleaned[matches[-1].end():].strip()
    return len(after.split()) if after else 0


def check_persona(text: str) -> bool:
    """Check if response uses FRIDAY persona markers (sir, boss, etc.)"""
    cleaned = strip_thinking_leak(text).lower()
    return 'sir' in cleaned or 'boss' in cleaned


def check_clarification(text: str) -> bool:
    """Check if response asks for clarification (contains question mark)"""
    return '?' in text


# Backend selection (set by main block)
BACKEND = "local"   # "local" | "claude" | "openai"
CLOUD_MODEL = None
CLOUD_API_KEY = None


def query_llm_local(prompt: str, max_tokens: int) -> Tuple[str, float, Dict]:
    """Query the local llama-server via OpenAI-compatible API"""
    try:
        start = time.time()
        response = requests.post(
            f"{SERVER}/v1/chat/completions",
            headers={"Content-Type": "application/json"},
            json={
                "messages": [
                    {"role": "system", "content": SYSTEM_PROMPT},
                    {"role": "user", "content": prompt}
                ],
                "temperature": 0.7,
                "max_tokens": max_tokens,
                "stream": False
            },
            timeout=120
        )
        elapsed = time.time() - start

        if response.status_code != 200:
            return None, elapsed, {"error": f"HTTP {response.status_code}"}

        data = response.json()
        content = data['choices'][0]['message']['content']
        timing = data.get('timings', {})
        usage = data.get('usage', {})

        return content, elapsed, {**timing, **usage}
    except Exception as e:
        return None, 0, {"error": str(e)}


def query_llm_claude(prompt: str, max_tokens: int) -> Tuple[str, float, Dict]:
    """Query Anthropic Claude API"""
    try:
        start = time.time()
        payload = {
            "model": CLOUD_MODEL,
            "max_tokens": max_tokens,
            "system": SYSTEM_PROMPT,
            "messages": [{"role": "user", "content": prompt}]
        }
        # Claude Opus 4.7 and later, Sonnet 5+ and Haiku 5.5 reject temperature
        if takes_temperature(CLOUD_MODEL):
            payload["temperature"] = 0.7

        response = requests.post(
            "https://api.anthropic.com/v1/messages",
            headers={
                "x-api-key": CLOUD_API_KEY,
                "anthropic-version": "2023-06-01",
                "content-type": "application/json"
            },
            json=payload,
            timeout=120
        )
        elapsed = time.time() - start

        if response.status_code != 200:
            return None, elapsed, {"error": f"HTTP {response.status_code}: {response.text[:200]}"}

        data = response.json()
        # Claude returns content as a list of blocks
        content = ""
        for block in data.get("content", []):
            if block.get("type") == "text":
                content += block.get("text", "")
        usage = data.get("usage", {})
        return content, elapsed, {"prompt_tokens": usage.get("input_tokens", 0),
                                   "completion_tokens": usage.get("output_tokens", 0)}
    except Exception as e:
        return None, 0, {"error": str(e)}


def query_llm_openai(prompt: str, max_tokens: int) -> Tuple[str, float, Dict]:
    """Query OpenAI-compatible cloud API"""
    try:
        start = time.time()
        response = requests.post(
            "https://api.openai.com/v1/chat/completions",
            headers={
                "Authorization": f"Bearer {CLOUD_API_KEY}",
                "Content-Type": "application/json"
            },
            json={
                "model": CLOUD_MODEL,
                "max_tokens": max_tokens,
                "temperature": 0.7,
                "messages": [
                    {"role": "system", "content": SYSTEM_PROMPT},
                    {"role": "user", "content": prompt}
                ]
            },
            timeout=120
        )
        elapsed = time.time() - start

        if response.status_code != 200:
            return None, elapsed, {"error": f"HTTP {response.status_code}: {response.text[:200]}"}

        data = response.json()
        content = data['choices'][0]['message']['content']
        usage = data.get('usage', {})
        return content, elapsed, usage
    except Exception as e:
        return None, 0, {"error": str(e)}


def query_llm(prompt: str, max_tokens: int = 150) -> Tuple[str, float, Dict]:
    """Query the selected backend and return response, timing, and usage stats"""
    if BACKEND == "claude":
        return query_llm_claude(prompt, max_tokens)
    elif BACKEND == "openai":
        return query_llm_openai(prompt, max_tokens)
    else:
        return query_llm_local(prompt, max_tokens)


# =============================================================================
# Scoring
# =============================================================================

def score_test_case(test: Dict, response: str) -> Tuple[int, Dict]:
    """Score a single test case and return points earned and details"""
    points = 0
    details = {}
    checks = test.get('checks', {})

    # Strip thinking leak artifacts before scoring
    cleaned_response = strip_thinking_leak(response)

    # Extract commands from cleaned response
    commands = extract_commands(cleaned_response)
    details['response'] = response  # Keep original for inspection
    details['cleaned_response'] = cleaned_response
    details['commands_found'] = len(commands)
    details['commands'] = commands

    # Check: Has command (if expected)
    if checks.get('has_command'):
        if commands:
            points += 2
            details['has_command'] = '✅ Pass'
        else:
            details['has_command'] = '❌ FAIL - No command found'
    elif checks.get('has_command') == False:
        # Should NOT have command
        if not commands:
            points += 2
            details['has_command'] = '✅ Pass (correctly no command)'
        else:
            details['has_command'] = f'❌ FAIL - Unexpected command: {commands}'

    # Check: JSON valid
    if checks.get('json_valid') and commands:
        valid = all('_invalid_json' not in cmd for cmd in commands)
        if valid:
            points += 2
            details['json_valid'] = '✅ Pass'
        else:
            details['json_valid'] = '❌ FAIL - Invalid JSON'

    # Check: Correct device/action/value (skip if weather_flexible handles scoring)
    if commands and test.get('expected_device') and not checks.get('weather_flexible'):
        cmd = commands[0]
        device_match = cmd.get('device') == test['expected_device']

        # Action can be exact match or in a set
        action_set = checks.get('action_in_set')
        if action_set:
            action_match = cmd.get('action') in action_set
        elif test.get('expected_action'):
            action_match = cmd.get('action') == test['expected_action']
        else:
            action_match = True  # No action requirement

        if device_match and action_match:
            points += 3
            details['command_accuracy'] = f'✅ Pass (device={cmd.get("device")}, action={cmd.get("action")})'
        else:
            details['command_accuracy'] = f'❌ FAIL - Expected {test["expected_device"]}/{test.get("expected_action") or action_set}, got {cmd.get("device")}/{cmd.get("action")}'

        # Check value if applicable
        if checks.get('value_correct') and test.get('expected_value') is not None:
            value = cmd.get('value')
            try:
                value = int(value) if value is not None else None
                expected = int(test['expected_value'])
                if value == expected or str(value) == str(expected):
                    points += 1
                    details['value_correct'] = '✅ Pass'
                else:
                    details['value_correct'] = f'❌ FAIL - Expected {expected}, got {value}'
            except:
                if value and str(test['expected_value']).lower() in str(value).lower():
                    points += 1
                    details['value_correct'] = '✅ Pass'
                else:
                    details['value_correct'] = f'❌ FAIL - Expected {test["expected_value"]}, got {value}'

    # Check: Action in set (standalone — for tests where command_accuracy doesn't cover it)
    # Fires when action_in_set is specified and command_accuracy didn't already validate the action
    if checks.get('action_in_set') and commands and 'command_accuracy' not in details:
        cmd = commands[0]
        action_set = checks['action_in_set']
        if cmd.get('action') in action_set:
            points += 2
            details['action_in_set'] = f'✅ Pass (action={cmd.get("action")})'
        else:
            details['action_in_set'] = f'❌ FAIL - Expected one of {action_set}, got {cmd.get("action")}'

    # Check: Has value (for weather/search - partial match)
    if checks.get('has_value') and commands:
        cmd = commands[0]
        value = cmd.get('value', '')
        expected = test.get('expected_value', '')
        if value and expected:
            if expected.lower() in str(value).lower():
                points += 2
                details['has_value'] = f'✅ Pass (value contains "{expected}")'
            else:
                details['has_value'] = f'❌ FAIL - Expected "{expected}" in value, got "{value}"'
        elif value:
            points += 1
            details['has_value'] = f'⚠️ Partial - Has value "{value}" but expected "{expected}"'
        else:
            details['has_value'] = '❌ FAIL - No value in command'

    # Check: Command only (for getters)
    if checks.get('command_only'):
        words_before = count_words_before_command(cleaned_response)
        if words_before <= 2:
            points += 2
            details['command_only'] = f'✅ Pass ({words_before} words before)'
        else:
            details['command_only'] = f'❌ FAIL - {words_before} words before command (should be 0)'

    # Check: Has sentence before (for boolean/analog)
    if checks.get('has_sentence_before'):
        words_before = count_words_before_command(cleaned_response)
        if words_before > 0:
            points += 1
            details['sentence_before'] = f'✅ Pass ({words_before} words)'
        else:
            details['sentence_before'] = '❌ FAIL - No sentence before command'

    # Check: No prose after command
    if checks.get('no_prose_after'):
        words_after = count_words_after_command(cleaned_response)
        if words_after == 0:
            points += 1
            details['no_prose_after'] = '✅ Pass'
        else:
            details['no_prose_after'] = f'❌ FAIL - {words_after} words after command'

    # Check: Uses sir/boss (soft check — bonus point if present, no penalty if absent)
    if checks.get('uses_sir_boss'):
        if check_persona(cleaned_response):
            points += 1
            details['persona'] = '✅ Pass (uses sir/boss)'
        else:
            details['persona'] = '⚠️ No sir/boss (optional, no penalty)'

    # Check: Word limit
    if checks.get('word_limit'):
        words_before = count_words_before_command(cleaned_response) if commands else len(cleaned_response.split())
        limit = checks['word_limit']
        if words_before <= limit:
            points += 1
            details['word_limit'] = f'✅ Pass ({words_before}/{limit} words)'
        else:
            details['word_limit'] = f'❌ FAIL - {words_before} words (limit: {limit})'

    # Check: Multiple commands
    if checks.get('multiple_commands'):
        expected_cmds = test.get('expected_commands', [])
        if len(commands) >= len(expected_cmds):
            points += 2
            details['multiple_commands'] = f'✅ Pass ({len(commands)} commands)'
        else:
            details['multiple_commands'] = f'❌ FAIL - Expected {len(expected_cmds)}, got {len(commands)}'

    # Check: Asks clarification
    if checks.get('asks_clarification'):
        if check_clarification(cleaned_response):
            points += 3
            details['clarification'] = '✅ Pass (asks question)'
        else:
            details['clarification'] = '❌ FAIL - Did not ask clarification'

    # Check: Weather flexible (accept command OR clarification)
    if checks.get('weather_flexible'):
        if commands:
            cmd = commands[0]
            if cmd.get('device') == 'weather':
                points += 5
                details['weather_flexible'] = f'✅ Pass (sent weather command: action={cmd.get("action")})'
            else:
                details['weather_flexible'] = f'❌ FAIL - Wrong device: {cmd.get("device")}'
        elif check_clarification(cleaned_response):
            points += 5
            details['weather_flexible'] = '✅ Pass (asked for location)'
        else:
            details['weather_flexible'] = '❌ FAIL - Neither command nor clarification'

    return points, details


# =============================================================================
# Main
# =============================================================================

def run_quality_test(model_name: str = "Current Model") -> Dict:
    """Run all quality tests and return results"""
    print(f"\n{'='*80}")
    print(f"DAWN LLM Quality Test - {model_name}")
    print(f"{'='*80}\n")

    total_points = sum(tc['points'] for tc in TEST_CASES)
    earned_points = 0
    results = []

    for i, test in enumerate(TEST_CASES, 1):
        print(f"Test {i}/{len(TEST_CASES)}: {test['name']}")
        print(f"User: {test['user']}")

        response, elapsed, stats = query_llm(test['user'])

        if response is None:
            print(f"❌ ERROR: {stats.get('error', 'Unknown error')}\n")
            results.append({
                'test': test['name'],
                'points': 0,
                'max_points': test['points'],
                'error': stats.get('error')
            })
            continue

        points, details = score_test_case(test, response)
        earned_points += points

        print(f"FRIDAY: {response}")
        print(f"Score: {points}/{test['points']} points")
        print(f"Time: {elapsed:.2f}s")

        # Print check details
        for check, result in details.items():
            if check not in ['response', 'cleaned_response', 'commands_found', 'commands']:
                print(f"  {result}")

        print()

        results.append({
            'test': test['name'],
            'type': test['type'],
            'points': points,
            'max_points': test['points'],
            'details': details,
            'elapsed': elapsed,
            'stats': stats
        })

        time.sleep(0.5)

    # Calculate scores
    percentage = (earned_points / total_points * 100) if total_points > 0 else 0

    summary = {
        'model': model_name,
        'total_points': earned_points,
        'max_points': total_points,
        'percentage': percentage,
        'tests': results
    }

    # Print summary
    print(f"\n{'='*80}")
    print(f"QUALITY TEST SUMMARY - {model_name}")
    print(f"{'='*80}")
    print(f"Total Score: {earned_points}/{total_points} ({percentage:.1f}%)")
    print()

    if percentage >= 90:
        grade = "A - Excellent"
    elif percentage >= 80:
        grade = "B - Good"
    elif percentage >= 70:
        grade = "C - Acceptable"
    elif percentage >= 60:
        grade = "D - Marginal"
    else:
        grade = "F - Unacceptable"

    print(f"Grade: {grade}")
    print()

    # Category breakdown
    categories = {}
    for result in results:
        cat = result['type']
        if cat not in categories:
            categories[cat] = {'earned': 0, 'max': 0}
        categories[cat]['earned'] += result['points']
        categories[cat]['max'] += result['max_points']

    print("Category Breakdown:")
    for cat, scores in sorted(categories.items()):
        pct = (scores['earned'] / scores['max'] * 100) if scores['max'] > 0 else 0
        print(f"  {cat:20s}: {scores['earned']:3d}/{scores['max']:3d} ({pct:5.1f}%)")

    print(f"\n{'='*80}\n")

    return summary


def load_api_key_from_secrets(provider: str) -> str:
    """Load API key from dawn/secrets.toml as fallback"""
    secrets_path = os.path.expanduser("~/code/dawn/secrets.toml")
    if not os.path.exists(secrets_path):
        return ""
    try:
        key_name = {"claude": "claude_api_key", "openai": "openai_api_key"}.get(provider)
        if not key_name:
            return ""
        with open(secrets_path) as f:
            for line in f:
                line = line.strip()
                if line.startswith(f"{key_name}") and "=" in line:
                    value = line.split("=", 1)[1].strip().strip('"').strip("'")
                    return value
    except Exception:
        pass
    return ""


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="DAWN LLM quality test")
    parser.add_argument("--cloud", choices=["claude", "openai"],
                        help="Run against a cloud provider instead of the local server")
    parser.add_argument("--model", help="Cloud model name (required with --cloud)")
    parser.add_argument("--api-key",
                        help="API key (fallback: env vars or ~/code/dawn/secrets.toml)")
    args = parser.parse_args()

    if args.cloud:
        if not args.model:
            print("❌ ERROR: --model required when using --cloud")
            sys.exit(1)

        globals()["BACKEND"] = args.cloud
        globals()["CLOUD_MODEL"] = args.model

        # Resolve API key: CLI arg > env var > secrets.toml
        env_name = "ANTHROPIC_API_KEY" if args.cloud == "claude" else "OPENAI_API_KEY"
        globals()["CLOUD_API_KEY"] = (args.api_key or os.getenv(env_name)
                                       or load_api_key_from_secrets(args.cloud))
        CLOUD_API_KEY = globals()["CLOUD_API_KEY"]

        if not CLOUD_API_KEY:
            print(f"❌ ERROR: No API key. Set {env_name}, pass --api-key, or add to secrets.toml")
            sys.exit(1)

        model_name = f"{args.cloud}:{args.model}"
        print(f"Running cloud test: {model_name}")
    else:
        # Local mode: verify server is up
        try:
            response = requests.get(f"{SERVER}/health", timeout=2)
            if response.status_code != 200:
                print(f"❌ ERROR: llama-server not responding at {SERVER}")
                print("Start llama-server first!")
                sys.exit(1)
        except:
            print(f"❌ ERROR: Cannot connect to {SERVER}")
            print("Make sure llama-server is running!")
            sys.exit(1)

        # Get model name from server
        try:
            props = requests.get(f"{SERVER}/props", timeout=2).json()
            model_name = props.get('default_generation_settings', {}).get('model', 'Unknown')
            if '/' in model_name:
                model_name = model_name.split('/')[-1]
        except:
            model_name = "Current Model"

    # Run tests
    results = run_quality_test(model_name)

    # Save results to file
    suffix = f"_{args.cloud}_{args.model}" if args.cloud else ""
    safe_suffix = re.sub(r'[^A-Za-z0-9_.-]', '_', suffix)
    output_file = f"quality_test_results_{int(time.time())}{safe_suffix}.json"
    with open(output_file, 'w') as f:
        json.dump(results, f, indent=2)

    print(f"Results saved to: {output_file}")
