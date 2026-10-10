# Offline tests for the quality suite's own logic: capture substitution, mocks,
# scoring and the judge's kappa.  No network.  Run:
#   python3 -m llm_testing.quality.test_offline
#
# License: GPLv3, same as DAWN.

import json
import os
import re
import sys
import tempfile
import unittest

from . import capture, cases, score
from .judge import _kappa
from .loop import Step, Trajectory, TurnResult
from . import mocks
from .mocks import World, _safe_eval

NOW = capture.clock("2026-10-13T09:30:00", "America/Los_Angeles")
CTX = ("--- TURN CONTEXT (t1) ---\n[system_time] Current time: Thursday, 2026-10-08 02:00 UTC "
       "(ISO: 2026-10-08T02:00:00Z).  stale\n--- END TURN CONTEXT (t1) ---\n")


def _capture(provider="claude", question="Hey there?"):
    msgs_key = capture.messages_key(provider)
    body = {"model": "m", "max_tokens": 16384, msgs_key: [
        {"role": "user", "content": [{"type": "text", "text": CTX},
                                     {"type": "text", "text": question}]}]}
    return capture.Capture("x", provider, "https://x", [], body, "webui-text", "m", question,
                           [question])


def _traj(world, answer="", steps=()):
    t = Trajectory(world=world)
    tr = TurnResult(user="u", answer=answer)
    tr.steps = list(steps)
    t.turns.append(tr)
    return t


class CaptureTests(unittest.TestCase):
    def test_question_and_time_replaced_rest_untouched(self):
        cap = _capture()
        body = capture.instantiate(cap, "What's the weather?", NOW)
        text = json.dumps(body)
        self.assertIn("What's the weather?", text)
        self.assertNotIn("Hey there?", text)
        self.assertIn("Tuesday, 2026-10-13 09:30 PDT", text)
        self.assertIn("(ISO: 2026-10-13T09:30:00-07:00)", text)
        self.assertNotIn("2026-10-08", text)
        self.assertTrue(body["stream"])
        self.assertEqual(body["max_tokens"], 16384)
        self.assertNotIn("stream", cap.body)  # the template itself is untouched

    def test_load_capture_set_matches_turns_by_text(self):
        d = tempfile.mkdtemp()
        cap = _capture(question="Good morning.")
        follow = json.loads(json.dumps(cap.body))
        follow["messages"].append({"role": "assistant", "content": "ok"})
        files = [cap.body, {"messages": [{"role": "user", "content": "tool follow-up"}]}, follow]
        for i, b in enumerate(files, 1):
            json.dump({"provider": "claude", "url": "u", "headers": [], "body": b},
                      open(f"{d}/{i:03d}-claude.json", "w"))
        with open(f"{d}/manifest.jsonl", "w") as m:
            m.write(json.dumps({"surface": "webui-text", "model": "m",
                                "turns": ["Good morning."]}) + "\n")
        caps = capture.load_capture_set(d)
        self.assertEqual(len(caps), 1)
        self.assertTrue(caps[0].path.endswith("001-claude.json"))


class MockTests(unittest.TestCase):
    def test_calculator_is_arithmetic_only(self):
        self.assertEqual(_safe_eval("47 * 89"), 4183)
        self.assertEqual(_safe_eval("5!"), 120)
        self.assertIsNone(_safe_eval("__import__('os').system('true')"))

    def test_confirm_in_the_prepares_own_turn_is_flagged(self):
        w = World(NOW)
        w.turn = 0
        w.call("home_assistant", {"action": "unlock", "device": "front door"})
        w.call("home_assistant", {"action": "confirm", "device": "p1"})
        self.assertEqual(w.same_turn_acts, ["ha_unlock"])

    def test_confirm_on_a_later_turn_acts(self):
        w = World(NOW)
        w.call("email", {"action": "send", "arguments": json.dumps({"to": "b@x.com"})})
        w.turn = 1
        w.call("email", {"action": "confirm_send", "arguments": json.dumps({"draft_id": "p1"})})
        self.assertEqual(w.sent[0]["to"], "b@x.com")
        self.assertEqual(w.same_turn_acts, [])

    def test_email_read_and_trash_need_a_yes(self):
        w = World(NOW, {"inbox": [{"id": "m2", "from": "Newsletter", "subject": "Digest",
                                   "body": "This week..."}]})
        self.assertIn("This week", w.call("email", {"action": "read", "message_id": "m2"}))
        self.assertIn("pending_id p1", w.call("email", {"action": "trash", "message_id": "m2"}))
        w.turn = 1
        self.assertEqual(w.call("email", {"action": "confirm_trash", "pending_id": "p1"}),
                         "Moved to trash.")
        self.assertEqual((w.inbox, w.trashed[0]["message_id"], w.same_turn_acts), ([], "m2", []))

    def test_url_fetch_without_a_page_says_so(self):
        w = World(NOW, {"pages": {"example.org": "Page text."}})
        self.assertIn("Page text.", w.call("url_fetch", {"url": "https://example.org/a"}))
        self.assertIn("isn't available", w.call("url_fetch", {"url": "https://other.net"}))
        self.assertEqual(w.unmocked, [])

    def test_unmocked_tool_is_recorded_not_failed(self):
        w = World(NOW)
        self.assertIn("isn't available", w.call("stocks", {"action": "quote"}))
        self.assertEqual(w.unmocked, ["stocks.quote"])

    def test_unhandled_action_on_a_mocked_tool_is_recorded(self):
        w = World(NOW)
        self.assertIn("isn't available", w.call("calendar", {"action": "delete"}))
        self.assertEqual(w.unmocked, ["calendar.delete"])

    def test_fire_at_in_utc_is_local_time(self):
        w = World(NOW)
        w.call("scheduler", {"action": "create", "arguments": json.dumps(
            {"type": "reminder", "fire_at": "2026-10-14T22:00:00Z"})})
        self.assertEqual(w.reminders[0]["fire_at"], "2026-10-14T15:00:00")


def _prompts_h_string(name):
    """A string macro's value in include/prompts.h (adjacent literals joined)."""
    path = os.path.join(os.path.dirname(__file__), "..", "..", "include", "prompts.h")
    with open(path, encoding="utf-8") as f:
        src = f.read()
    m = re.search(r"#define " + name + r"\b((?:[^\n]*\\\n)*[^\n]*)", src)
    if not m:
        raise AssertionError(f"{name} not in prompts.h")
    lits = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
    return "".join(lits).encode().decode("unicode_escape")


def _c_framed_actions():
    """{(tool, action or "*"): frame} as the tools declare them in src/tools: a
    tool-wide .third_party, or an action-kinds entry's frame."""
    root = os.path.join(os.path.dirname(__file__), "..", "..")
    with open(os.path.join(root, "include", "tools", "tool_registry.h"), encoding="utf-8") as f:
        names = dict(re.findall(r'#define (TOOL_FRAME_\w+) "([^"]+)"', f.read()))
    out = {}
    tools_dir = os.path.join(root, "src", "tools")
    for fn in sorted(os.listdir(tools_dir)):
        if not fn.endswith(".c"):
            continue
        with open(os.path.join(tools_dir, fn), encoding="utf-8") as f:
            src = f.read()
        tool = re.search(r'tool_metadata_t \w+ = \{\s*\.name = "([^"]+)",', src)
        if not tool:
            continue
        for frame in re.findall(r"^\s*\.third_party = (TOOL_FRAME_\w+)", src, re.M):
            out[(tool.group(1), "*")] = names[frame]
        for action, frame in re.findall(
                r'^\s*\{ "(\w+)", TOOL_KIND_\w+, [^,]+, (TOOL_FRAME_\w+) \}', src, re.M):
            out[(tool.group(1), action)] = names[frame]
    return out


class ThirdPartyFrameTests(unittest.TestCase):
    TAG = "dawn-ctx-0123abcd"

    def test_framed_actions_match_the_tools(self):
        self.assertEqual(_c_framed_actions(), mocks._THIRD_PARTY_ACTIONS)

    def test_text_matches_prompts_h(self):
        lead = _prompts_h_string("THIRD_PARTY_FRAME_LEAD_TEMPLATE").replace("%s", "{what}")
        self.assertEqual(lead, mocks.THIRD_PARTY_FRAME_LEAD)
        self.assertEqual(_prompts_h_string("THIRD_PARTY_WHAT_EMAIL"),
                         mocks.THIRD_PARTY_WHAT["EMAIL CONTENT"])
        self.assertEqual(_prompts_h_string("THIRD_PARTY_WHAT_WEB"),
                         mocks.THIRD_PARTY_WHAT["WEB CONTENT"])

    def test_email_read_is_framed(self):
        args = {"arguments": json.dumps({"action": "read", "message_id": "m1"})}
        out = mocks.frame_third_party("email", args, "From: a@x.com\nHi", self.TAG)
        self.assertTrue(out.startswith(f"--- EMAIL CONTENT ({self.TAG}) ---\nThird-party content "
                                       "(email, as its sender wrote it)."))
        self.assertIn("\nFrom: a@x.com\nHi\n", out)
        self.assertTrue(out.endswith(f"--- END EMAIL CONTENT ({self.TAG}) ---\n"))

    def test_web_tools_are_framed(self):
        for tool in ("search", "url_fetch"):
            out = mocks.frame_third_party(tool, {"query": "q"}, "page", self.TAG)
            self.assertTrue(out.startswith(f"--- WEB CONTENT ({self.TAG}) ---\n"), tool)

    def test_errors_gaps_and_other_tools_are_not(self):
        send = {"action": "send"}
        read = {"action": "read"}
        self.assertEqual(mocks.frame_third_party("email", send, "Draft ready", self.TAG),
                         "Draft ready")
        self.assertEqual(mocks.frame_third_party("email", read, "Error: no such message",
                                                 self.TAG), "Error: no such message")
        self.assertEqual(mocks.frame_third_party("search", {}, mocks.UNAVAILABLE, self.TAG),
                         mocks.UNAVAILABLE)
        self.assertEqual(mocks.frame_third_party("weather", {}, "Sunny", self.TAG), "Sunny")


class ScoreTests(unittest.TestCase):
    def _case(self, checks):
        return cases.Case(id="c", category="x", turns=["u"], checks=checks)

    def test_answer_numbers_ignore_separators(self):
        s, res, _ = score.score_case(self._case({"answer_contains": [["4183"]]}),
                                     _traj(World(NOW), answer="That's 4,183, boss."))
        self.assertEqual(s, 1.0)

    def test_world_and_forbidden_args(self):
        w = World(NOW)
        w.call("scheduler", {"action": "create", "arguments": json.dumps(
            {"type": "reminder", "fire_at": "2026-10-14T15:00:00"})})
        steps = [Step("phone", {"action": "call", "arguments": json.dumps({"target": "him"})}, "")]
        checks = {"world": {"reminders": [{"match": {"fire_at": ["~2026-10-14T15:00"]}}]},
                  "forbidden_args": [{"tool": "phone", "match": {"target": "~him"}}]}
        s, res, _ = score.score_case(self._case(checks), _traj(w, steps=steps))
        self.assertTrue(res["world:reminders#0"])
        self.assertFalse(res["forbidden_args:phone#0"])

    def test_speakable_flags_markdown_tables_and_urls(self):
        s, res, why = score.score_case(self._case({"speakable": True}),
                                       _traj(World(NOW), answer="| a | b |\nsee https://x.y"))
        self.assertFalse(res["speakable"])
        self.assertIn("URL", why["speakable"])

    def test_run_error_scores_zero(self):
        t = _traj(World(NOW))
        t.turns[0].error = "HTTP 400"
        self.assertEqual(score.score_case(self._case({"no_tools": True}), t)[0], 0.0)


class CaseTests(unittest.TestCase):
    def test_symbolic_dates_resolve(self):
        self.assertEqual(cases._resolve("{tomorrow} {weekday} {date+6}", NOW),
                         "2026-10-14 Tuesday 2026-10-19")

    def test_shipped_cases_load(self):
        self.assertGreaterEqual(len(cases.load_cases(NOW)), 120)


class ProviderTests(unittest.TestCase):
    def test_claude_breakpoint_follows_the_newest_user_block(self):
        from . import providers
        cap = _capture()
        body = capture.instantiate(cap, "Q?", NOW)
        reply = providers.Reply(assistant_items=[{"role": "assistant", "content": [
            {"type": "tool_use", "id": "t1", "name": "date", "input": {}}]}],
            calls=[providers.ToolCall("t1", "date", {})])
        providers.append_results(cap, body, reply, {"t1": "Tuesday"})
        marked = [b for m in body["messages"] if m["role"] == "user"
                  for b in m["content"] if "cache_control" in b]
        self.assertEqual(len(marked), 1)
        self.assertEqual(marked[0]["type"], "tool_result")

    def test_key_name_follows_the_carrier(self):
        from .providers import key_name
        local = capture.Capture("x", "openai-chat", "http://10.0.0.2:8080/v1/chat/completions",
                                ["Content-Type: application/json"], {}, "webui-text", "m", "q")
        self.assertIsNone(key_name(local))
        claude = capture.Capture("x", "claude", "https://api.anthropic.com/v1/messages",
                                 ["x-api-key: [REDACTED]"], {}, "webui-text", "m", "q")
        self.assertEqual(key_name(claude), "claude_api_key")

    def test_reasoning_details_merge_like_dawn(self):
        from .providers import merge_reasoning_details
        merged = merge_reasoning_details([{"type": "r", "index": 0, "text": "a"},
                                          {"type": "r", "index": 0, "text": "b"},
                                          {"type": "r", "index": 1, "text": "c"}])
        self.assertEqual([m["text"] for m in merged], ["ab", "c"])

    def test_errors_split_transport_from_model(self):
        c = cases.Case(id="c", category="x", turns=["u"], checks={"no_tools": True})
        t = _traj(World(NOW))
        t.turns[0].error = "HTTP 503: overloaded"
        self.assertIn("run", score.score_case(c, t)[1])
        t.turns[0].error = ("SSLError: HTTPSConnectionPool(host='x', port=443): Max retries "
                            "exceeded (UNEXPECTED_EOF_WHILE_READING)")
        self.assertIn("run", score.score_case(c, t)[1])
        t.turns[0].error = "HTTP 400: tool_use ids must be unique"
        self.assertIn("model_error", score.score_case(c, t)[1])

    def test_fire_at_follows_dawns_parser(self):
        w = World(NOW)
        self.assertEqual(w._local_time("2026-10-14 15:00"), "2026-10-14T00:00:00")
        self.assertEqual(w._local_time("15:00"), "2026-10-13T15:00:00")
        self.assertEqual(w._local_time("08:00"), "2026-10-14T08:00:00")


class DeadlineTests(unittest.TestCase):
    """A stream that trickles past its deadline ends; retries stop at the budget."""

    def test_trickling_stream_hits_deadline(self):
        from . import providers

        class Resp:
            def iter_lines(self, decode_unicode=True):
                while True:  # a keep-alive line forever: never a read timeout
                    yield ": ping"

        with self.assertRaises(providers.StreamDeadline):
            for _ in providers._sse(Resp(), deadline=0.0):
                pass

    def test_retries_stop_at_budget(self):
        from unittest import mock
        from . import providers
        calls = []

        def failing(cap, body, keys):
            calls.append(1)
            return providers.Reply(error="HTTP 529: overloaded")

        clock = iter(range(0, 10_000, 200))  # each look at the clock is 200s later
        with mock.patch.object(providers, "_send_once", failing), \
                mock.patch.object(providers.time, "time", lambda: next(clock)), \
                mock.patch.object(providers.time, "sleep", lambda s: None):
            reply = providers.send(_capture(), {}, {})
        self.assertLess(len(calls), providers.RETRY_ATTEMPTS)
        self.assertIn("gave up after", reply.error)


class InstructionTests(unittest.TestCase):
    def _cap(self, system):
        return capture.Capture("x", "claude", "u", [], {"system": system, "messages": []},
                               "webui-text", "m", "q")

    def test_old_and_new_rules(self):
        old = self._cap("Your name is F.\n\nWhen writing a mathematical factorial, spell it.\n"
                        "RULES\n1. Keep responses concise and conversational.\n"
                        "2. Use tools.\n3. If a request is ambiguous, ask for clarification.\n")
        i = capture.instructions(old)
        self.assertEqual(i["persona"], "Your name is F.")
        self.assertEqual(i["length"], "Keep responses concise and conversational.")
        self.assertEqual(i["ask"], "If a request is ambiguous, ask for clarification.")
        new = self._cap("P\nRULES\n1. Match the length to the request. A quick one.\n"
                        "2. If a request is missing something you need, ask.\n")
        i = capture.instructions(new)
        self.assertTrue(i["length"].startswith("Match the length"))
        self.assertIn("missing something you need", i["ask"])
        bullets = self._cap("P\nRULES\n- Match the length to the request. A quick one.\n"
                            "- If a request is missing something you need, ask.\n"
                            "- Search results include snippets.\n"
                            "  Only fetch a URL if asked.\n")
        i = capture.instructions(bullets)
        self.assertTrue(i["length"].startswith("Match the length"))
        self.assertIn("missing something you need", i["ask"])
        self.assertEqual(capture._rules(bullets.body["system"])[-1],
                         "Search results include snippets. Only fetch a URL if asked.")

    def test_replace_persona_and_unstated_criterion(self):
        from .judge import definition
        cap = self._cap("## Your Identity\nBe Ada.\n\nIMPORTANT: Use the identity above. Ignore."
                        "\n\nYour name is F.\nRULES\n1. Use tools.\n")
        i = capture.instructions(cap)
        self.assertEqual(i["persona"], "Be Ada.")
        self.assertIsNone(i["length"])
        row = {"surface": "webui-text", "_instructions": i}
        self.assertIsNone(definition(row, "concise"))  # not told: not graded
        self.assertIn("Be Ada.", definition(row, "persona"))
        alone = self._cap("## Your Identity\nBe Ada.\n\nRULES\n1. Use tools.\n")
        self.assertEqual(capture.instructions(alone)["persona"], "Be Ada.")


class PatchTests(unittest.TestCase):
    def _cap(self, system, provider="claude"):
        body = ({"instructions": system} if provider == "openai-responses"
                else {"system": system, "messages": []})
        return capture.Capture("x", provider, "u", [], body, "webui-text", "m", "q")

    def test_patches_once_and_shares_a_follow_up(self):
        cap = self._cap([{"type": "text", "text": "A\nRULES\n1. Old rule.\n"}])
        cap.followup = self._cap("RULES\n1. Old rule.\n", "openai-responses")
        other = self._cap([{"type": "text", "text": "RULES\n1. Old rule.\n"}])
        other.followup = cap.followup  # one follow-up, two templates
        done = set()
        for c in (cap, other):
            capture.patch_system(c, [["Old rule.", "New rule."]], done)
        self.assertIn("New rule.", cap.body["system"][0]["text"])
        self.assertIn("New rule.", cap.followup.body["instructions"])

    def test_missing_or_split_text_refuses(self):
        with self.assertRaises(ValueError):
            capture.patch_system(self._cap("RULES\n1. Other.\n", "openai-responses"),
                                 [["Old rule.", "x"]])
        split = self._cap([{"type": "text", "text": "AAA BB"}, {"type": "text", "text": "B CCC"}])
        with self.assertRaises(ValueError):
            capture.patch_system(split, [["BB\nB", "ZZ"]])

    def test_rules_heading_is_a_line_of_its_own(self):
        cap = self._cap("Be F.\nHOUSE RULES\n- tidy\nRULES\n1. Match the length to it.\n")
        i = capture.instructions(cap)
        self.assertEqual(i["persona"], "Be F.\nHOUSE RULES\n- tidy")
        self.assertTrue(i["length"].startswith("Match the length"))

    def test_spoken_direction_comes_from_the_note(self):
        cap = self._cap("RULES\n1. Match the length.\n")
        cap.body["messages"] = [{"role": "user", "content": [{"type": "text", "text":
            "[Operator note dawn-x] HUD list.\n\nYour reply is read to the user by "
            "text-to-speech. Keep it short."}]}]
        self.assertEqual(capture.instructions(cap)["spoken"],
                         "Your reply is read to the user by text-to-speech. Keep it short.")


class KappaTests(unittest.TestCase):
    def test_kappa(self):
        self.assertAlmostEqual(_kappa([(0, 0), (1, 1), (2, 2), (2, 2)]), 1.0)
        k = _kappa([(2, 2)] * 5)
        self.assertNotEqual(k, k)  # NaN: an always-pass judge proves nothing


if __name__ == "__main__":
    sys.exit(unittest.main())
