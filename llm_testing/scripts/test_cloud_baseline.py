#!/usr/bin/env python3
"""
DAWN Cloud LLM Baseline Test
Tests Claude and ChatGPT to establish peak accuracy baseline
"""

import json
import os
import re
import sys
import time
from typing import Dict, List, Tuple

# API Keys - check environment first, then secrets.toml
ANTHROPIC_API_KEY = os.getenv("ANTHROPIC_API_KEY") or ""
OPENAI_API_KEY = os.getenv("OPENAI_API_KEY") or ""

# DAWN System Prompt (from dawn.h AI_DESCRIPTION)
SYSTEM_PROMPT = """FRIDAY, Iron-Man AI assistant. Female voice; witty yet kind. Address the user as "sir" or "boss". Never reveal model identity.
Max 30 words plus <command> tags unless the user says "explain in detail".

You assist the OASIS Project (Open Armor Systems Integrated Suite):
• MIRAGE – HUD overlay
• DAWN – voice/AI manager
• AURA – environmental sensors
• SPARK – hand sensors & actuators

CRITICAL: ALL device commands MUST be wrapped in <command></command> XML tags containing JSON. NEVER output raw JSON!

RULES
1. For Boolean / Analog / Music actions: one sentence, then the JSON wrapped in <command> tags. No prose after the tag block.
2. For Getter actions (date, time, suit_status): send ONLY the <command> tag with JSON inside, wait for the system JSON, then one confirmation sentence ≤15 words.
3. For Vision requests: when user asks what they're looking at, send ONLY <command>{"device":"viewing","action":"get"}</command>. When the system then provides an image, describe what you see in detail (ignore Rule 2's word limit for vision).
4. Use only the devices and actions listed below; never invent new ones.
5. If a request is ambiguous (e.g., "Mute it"), ask one-line clarification.
6. If the user wants information that has no matching getter yet, answer verbally with no tags.
7. Device "info" supports ENABLE / DISABLE only—never use "get" with it.
8. To mute playback after clarification, use <command>{"device":"volume","action":"set","value":0}</command>.

COMMAND FORMAT (MANDATORY):
✅ CORRECT: HUD online, boss. <command>{"device":"armor_display","action":"enable"}</command>
❌ WRONG: {"device":"armor_display","action":"enable"}
❌ WRONG: <command>enable armor display</command>

=== EXAMPLES ===
User: Turn on the armor display.
FRIDAY: HUD online, boss. <command>{"device":"armor_display","action":"enable"}</command>
System→ {"response":"armor display enabled"}
FRIDAY: Display confirmed, sir.

User: What time is it?
FRIDAY: <command>{"device":"time","action":"get"}</command>
System→ {"response":"The time is 4:07 PM."}
FRIDAY: Time confirmed, sir.

User: Mute it.
FRIDAY: Need specifics, sir—audio playback or mic?

User: Mute playback.
FRIDAY: Volume to zero, boss. <command>{"device":"volume","action":"set","value":0}</command>
System→ {"response":"volume set"}
FRIDAY: Muted, sir.

Available devices and actions:
- armor_display: enable/disable (boolean)
- detect: enable/disable (object detection, boolean)
- map: enable/disable (minimap, boolean)
- faceplate: enable/disable (helmet, boolean)
- info: enable/disable (logging, boolean)
- record: enable/disable (boolean)
- stream: enable/disable (boolean)
- hud: set (analog, 0-100)
- volume: set (analog, 0-100)
- visual_offset: set (analog, pixels)
- date: get (getter)
- time: get (getter)
- viewing: get (vision getter)
- flac: play/next/previous/stop (music)
- local_llm: enable/disable (boolean)
- cloud_llm: enable/disable (boolean)
"""

# Test cases (same as test_llm_quality.py)
TEST_CASES = [
    {
        "id": 1,
        "category": "boolean",
        "name": "Boolean Enable - Armor Display",
        "prompt": "Turn on the armor display",
        "expected_device": "armor_display",
        "expected_action": "enable",
        "max_points": 10
    },
    {
        "id": 2,
        "category": "boolean",
        "name": "Boolean Disable - Object Detection",
        "prompt": "Turn off object detection",
        "expected_device": "detect",
        "expected_action": "disable",
        "max_points": 10
    },
    {
        "id": 3,
        "category": "analog",
        "name": "Analog Set - Volume",
        "prompt": "Set the volume to 75",
        "expected_device": "volume",
        "expected_action": "set",
        "expected_value": 75,
        "max_points": 10
    },
    {
        "id": 4,
        "category": "getter",
        "name": "Getter - Time",
        "prompt": "What time is it?",
        "expected_device": "time",
        "expected_action": "get",
        "max_points": 10
    },
    {
        "id": 5,
        "category": "getter",
        "name": "Getter - Date",
        "prompt": "What's today's date?",
        "expected_device": "date",
        "expected_action": "get",
        "max_points": 10
    },
    {
        "id": 6,
        "category": "vision",
        "name": "Vision Request",
        "prompt": "What am I looking at?",
        "expected_device": "viewing",
        "expected_action": "get",
        "max_points": 10
    },
    {
        "id": 7,
        "category": "clarification",
        "name": "Ambiguous Request",
        "prompt": "Mute it",
        "should_clarify": True,
        "max_points": 10
    },
    {
        "id": 8,
        "category": "music",
        "name": "Music Play",
        "prompt": "Play some jazz music",
        "expected_device": "flac",
        "expected_action": "play",
        "max_points": 10
    },
    {
        "id": 9,
        "category": "multiple",
        "name": "Multiple Commands",
        "prompt": "Turn on the armor display and enable object detection",
        "expected_count": 2,
        "max_points": 15
    },
    {
        "id": 10,
        "category": "conversational",
        "name": "Conversational Question",
        "prompt": "How are the systems looking today?",
        "should_have_command": False,
        "max_points": 10
    },
]


def call_claude(messages: List[Dict], model="claude-sonnet-4-5-20250929") -> str:
    """Call Anthropic Claude API"""
    import anthropic

    client = anthropic.Anthropic(api_key=ANTHROPIC_API_KEY)

    response = client.messages.create(
        model=model,
        max_tokens=200,
        system=SYSTEM_PROMPT,
        messages=messages
    )

    return response.content[0].text


def call_chatgpt(messages: List[Dict], model="gpt-4o") -> str:
    """Call OpenAI ChatGPT API"""
    import openai

    client = openai.OpenAI(api_key=OPENAI_API_KEY)

    # Add system message
    full_messages = [{"role": "system", "content": SYSTEM_PROMPT}] + messages

    response = client.chat.completions.create(
        model=model,
        max_tokens=200,
        messages=full_messages
    )

    return response.choices[0].message.content


def extract_commands(text: str) -> List[Dict]:
    """Extract JSON commands from <command> tags"""
    commands = []
    pattern = r'<command>(.*?)</command>'
    matches = re.findall(pattern, text, re.DOTALL)

    for match in matches:
        try:
            # Clean up the JSON string
            json_str = match.strip()
            cmd = json.loads(json_str)
            commands.append(cmd)
        except json.JSONDecodeError:
            continue

    return commands


def count_words(text: str) -> int:
    """Count words, excluding <command> tags and their contents"""
    # Remove <command> tags and contents
    cleaned = re.sub(r'<command>.*?</command>', '', text, flags=re.DOTALL)
    return len(cleaned.split())


def grade_response(test: Dict, response: str) -> Tuple[int, List[str]]:
    """Grade a response based on test criteria"""
    points = 0
    feedback = []
    max_points = test["max_points"]

    commands = extract_commands(response)
    has_sir_boss = "sir" in response.lower() or "boss" in response.lower()

    category = test["category"]

    if category in ["boolean", "analog", "music"]:
        # Check for command
        if commands and len(commands) > 0:
            cmd = commands[0]
            points += 4
            feedback.append("✅ Pass")

            # Check device and action
            if cmd.get("device") == test.get("expected_device") and \
               cmd.get("action") == test.get("expected_action"):
                points += 4
                feedback.append("✅ Pass (device & action correct)")
            else:
                feedback.append(f"❌ FAIL - Expected {test.get('expected_device')}/{test.get('expected_action')}, got {cmd.get('device')}/{cmd.get('action')}")
        else:
            feedback.append("❌ FAIL - No command found")

        # Check word count before command
        before_cmd = response.split('<command>')[0] if '<command>' in response else response
        words_before = count_words(before_cmd)
        if words_before <= 30:
            feedback.append(f"✅ Pass ({words_before} words)")
        else:
            feedback.append(f"❌ FAIL - {words_before} words (limit: 30)")

        # Check no prose after command
        if '<command>' in response:
            parts = response.split('</command>')
            if len(parts) > 1:
                after = parts[1].strip()
                words_after = count_words(after)
                if words_after == 0:
                    points += 1
                    feedback.append("✅ Pass")
                else:
                    feedback.append(f"❌ FAIL - {words_after} words after command")
            else:
                points += 1
                feedback.append("✅ Pass")

        # Check for sir/boss
        if has_sir_boss:
            points += 1
            feedback.append("✅ Pass (uses sir/boss)")
        else:
            feedback.append("❌ FAIL - Missing sir/boss")

        # Total word count
        total_words = count_words(response)
        if total_words <= 30:
            feedback.append(f"✅ Pass ({total_words}/30 words)")
        else:
            feedback.append(f"❌ FAIL - {total_words} words (limit: 30)")

    elif category == "getter":
        # Check for command
        if commands and len(commands) > 0:
            cmd = commands[0]
            points += 5
            feedback.append("✅ Pass")

            # Check device and action
            if cmd.get("device") == test.get("expected_device") and \
               cmd.get("action") == test.get("expected_action"):
                points += 5
                feedback.append("✅ Pass (device & action correct)")
            else:
                feedback.append(f"❌ FAIL - Expected {test.get('expected_device')}/{test.get('expected_action')}, got {cmd.get('device')}/{cmd.get('action')}")
        else:
            feedback.append("❌ FAIL - No command found")

        # Check words before command
        before_cmd = response.split('<command>')[0] if '<command>' in response else response
        words_before = count_words(before_cmd)
        if words_before == 0:
            feedback.append(f"✅ Pass (0 words before)")
        else:
            feedback.append(f"❌ FAIL - {words_before} words before command (should be 0)")

    elif category == "vision":
        # Same as getter
        if commands and len(commands) > 0:
            cmd = commands[0]
            points += 5
            feedback.append("✅ Pass")

            if cmd.get("device") == test.get("expected_device") and \
               cmd.get("action") == test.get("expected_action"):
                points += 5
                feedback.append("✅ Pass (device & action correct)")
            else:
                feedback.append(f"❌ FAIL - Expected {test.get('expected_device')}/{test.get('expected_action')}, got {cmd.get('device')}/{cmd.get('action')}")
        else:
            feedback.append("❌ FAIL - No command found")

        before_cmd = response.split('<command>')[0] if '<command>' in response else response
        words_before = count_words(before_cmd)
        if words_before == 0:
            feedback.append(f"✅ Pass (0 words before)")
        else:
            feedback.append(f"❌ FAIL - {words_before} words before command (should be 0)")

    elif category == "clarification":
        # Should NOT have command, should ask question
        if not commands:
            points += 7
            feedback.append("✅ Pass (correctly no command)")
        else:
            feedback.append(f"❌ FAIL - Unexpected command: {commands}")

        if has_sir_boss:
            points += 2
            feedback.append("✅ Pass (uses sir/boss)")
        else:
            feedback.append("❌ FAIL - Missing sir/boss)")

        total_words = count_words(response)
        if total_words <= 30:
            feedback.append(f"✅ Pass ({total_words}/30 words)")
        else:
            feedback.append(f"❌ FAIL - {total_words} words (limit: 30)")

        if "?" in response:
            points += 1
            feedback.append("✅ Pass (asks question)")
        else:
            feedback.append("❌ FAIL - Did not ask clarification")

    elif category == "multiple":
        # Check for correct number of commands
        expected_count = test.get("expected_count", 2)
        if commands and len(commands) >= expected_count:
            points += 5
            feedback.append("✅ Pass")
        else:
            feedback.append(f"❌ FAIL - Expected {expected_count}, got {len(commands)}")

        words = count_words(response)
        if words <= 30:
            points += 3
            feedback.append(f"✅ Pass ({words} words)")
        else:
            feedback.append(f"❌ FAIL - {words} words (limit: 30)")

        if '<command>' in response:
            parts = response.split('</command>')
            if len(parts) > 1:
                after = parts[-1].strip()
                words_after = count_words(after)
                if words_after == 0:
                    points += 2
                    feedback.append("✅ Pass")
                else:
                    feedback.append(f"❌ FAIL - {words_after} words after command")

        if has_sir_boss:
            points += 2
            feedback.append("✅ Pass (uses sir/boss)")
        else:
            feedback.append("❌ FAIL - Missing sir/boss")

        # Check if we got the right number of commands
        if len(commands) == expected_count:
            points += 3
            feedback.append(f"✅ Pass (exactly {expected_count} commands)")
        else:
            feedback.append(f"❌ FAIL - Expected {expected_count}, got {len(commands)}")

    elif category == "conversational":
        # Should NOT have command
        if not commands:
            points += 5
            feedback.append("✅ Pass (correctly no command)")
        else:
            feedback.append(f"❌ FAIL - Unexpected command: {commands}")

        if has_sir_boss:
            points += 3
            feedback.append("✅ Pass (uses sir/boss)")
        else:
            feedback.append("❌ FAIL - Missing sir/boss")

        words = count_words(response)
        if words <= 30:
            points += 2
            feedback.append(f"✅ Pass ({words}/30 words)")
        else:
            feedback.append(f"❌ FAIL - {words} words (limit: 30)")

    return points, feedback


def run_test_suite(provider: str, model: str):
    """Run complete test suite for a provider"""
    print(f"\n{'='*80}")
    print(f"DAWN Cloud Baseline Test - {provider} ({model})")
    print(f"{'='*80}\n")

    results = []
    total_points = 0
    max_total = sum(test["max_points"] for test in TEST_CASES)

    for test in TEST_CASES:
        print(f"Test {test['id']}/10: {test['name']}")
        print(f"User: {test['prompt']}")

        try:
            # Call API
            start_time = time.time()
            messages = [{"role": "user", "content": test['prompt']}]

            if provider == "claude":
                response = call_claude(messages, model)
            else:  # chatgpt
                response = call_chatgpt(messages, model)

            elapsed = time.time() - start_time

            print(f"FRIDAY: {response}")

            # Grade response
            points, feedback = grade_response(test, response)
            total_points += points

            print(f"Score: {points}/{test['max_points']} points")
            print(f"Time: {elapsed:.2f}s")
            for line in feedback:
                print(f"  {line}")

            results.append({
                "test": test['name'],
                "response": response,
                "points": points,
                "max_points": test['max_points'],
                "time": elapsed,
                "feedback": feedback
            })

        except Exception as e:
            print(f"❌ ERROR: {e}")
            results.append({
                "test": test['name'],
                "error": str(e),
                "points": 0,
                "max_points": test['max_points']
            })

        print()

    # Calculate grade
    percentage = (total_points / max_total) * 100
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

    print(f"\n{'='*80}")
    print(f"QUALITY TEST SUMMARY - {provider.upper()} ({model})")
    print(f"{'='*80}")
    print(f"Total Score: {total_points}/{max_total} ({percentage:.1f}%)\n")
    print(f"Grade: {grade}\n")

    # Category breakdown
    categories = {}
    for i, test in enumerate(TEST_CASES):
        cat = test["category"]
        if cat not in categories:
            categories[cat] = {"points": 0, "max": 0}
        categories[cat]["points"] += results[i].get("points", 0)
        categories[cat]["max"] += test["max_points"]

    print("Category Breakdown:")
    for cat, scores in sorted(categories.items()):
        pct = (scores["points"] / scores["max"] * 100) if scores["max"] > 0 else 0
        print(f"  {cat:20s}: {scores['points']:3d}/{scores['max']:3d} ({pct:5.1f}%)")

    print(f"\n{'='*80}\n")

    return results, total_points, max_total


if __name__ == "__main__":
    # Parse secrets.toml for API keys (try multiple locations)
    secrets_paths = [
        "secrets.toml",
        "../secrets.toml",
        "../../secrets.toml",
        os.path.expanduser("~/.config/dawn/secrets.toml"),
    ]

    for secrets_path in secrets_paths:
        if os.path.exists(secrets_path):
            try:
                with open(secrets_path, "r") as f:
                    content = f.read()

                    # Simple TOML parsing for key = "value" format
                    match = re.search(r'claude_api_key\s*=\s*"([^"]+)"', content)
                    if match and not ANTHROPIC_API_KEY:
                        ANTHROPIC_API_KEY = match.group(1)
                        os.environ["ANTHROPIC_API_KEY"] = ANTHROPIC_API_KEY

                    match = re.search(r'openai_api_key\s*=\s*"([^"]+)"', content)
                    if match and not OPENAI_API_KEY:
                        OPENAI_API_KEY = match.group(1)
                        os.environ["OPENAI_API_KEY"] = OPENAI_API_KEY

                print(f"Loaded API keys from {secrets_path}")
                break
            except Exception as e:
                print(f"Warning: Could not parse {secrets_path}: {e}")

    # Test Claude
    if ANTHROPIC_API_KEY:
        print("\n🤖 Testing Claude Sonnet 3.7...")
        claude_results, claude_score, claude_max = run_test_suite("claude", "claude-sonnet-4-5-20250929")
    else:
        print("⚠️  Skipping Claude (no API key)")

    # Test ChatGPT
    if OPENAI_API_KEY:
        print("\n🤖 Testing ChatGPT GPT-4o...")
        gpt_results, gpt_score, gpt_max = run_test_suite("chatgpt", "gpt-4o")
    else:
        print("⚠️  Skipping ChatGPT (no API key)")

    # Summary
    print(f"\n{'='*80}")
    print("BASELINE COMPARISON")
    print(f"{'='*80}")
    if ANTHROPIC_API_KEY:
        print(f"Claude Sonnet 3.7: {claude_score}/{claude_max} ({claude_score/claude_max*100:.1f}%)")
    if OPENAI_API_KEY:
        print(f"ChatGPT GPT-4o:    {gpt_score}/{gpt_max} ({gpt_score/gpt_max*100:.1f}%)")
    print(f"{'='*80}\n")
