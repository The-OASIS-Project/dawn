#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# By contributing to this project, you agree to license your contributions
# under the GPLv3 (or any later version) or any future licenses chosen by
# the project author(s).
#
# The cloud-to-local fallback ([llm] local_fallback), end to end: a typed turn
# whose cloud provider is a fake that's closed, failing or answering, with a
# fake local LLM beside it.  Each class is one daemon; its tests run in order.

import re
import unittest
import uuid

from fake_llm import FakeLLM, status, text, tool_call
from instance import CLOUD_LLM_PORT, LOCAL_LLM_PORT, LiveTestCase

# Mirrors LLM_LOCAL_FALLBACK_NOTE in include/llm/llm_local_fallback.h
NOTE = "The cloud model isn't answering right now, so this answer is from the local model."
# The tool loop's retry ladder sleeps 1 + 2 + 4 s between attempts.
LADDER_SEC = 7.0
CLOUD_URL = f"http://127.0.0.1:{CLOUD_LLM_PORT}"
SECRETS = '[secrets]\nopenai_api_key = "sk-live-test"\n'


def config(fallback):
    return {
        "llm": {"type": "cloud", "local_fallback": fallback},
        "llm.cloud": {"provider": "openai", "endpoint": CLOUD_URL,
                      "openai_models": ["fake-cloud"], "openai_default_model_idx": 0,
                      "openai_use_responses_api": "never"},
        "llm.local": {"endpoint": f"http://127.0.0.1:{LOCAL_LLM_PORT}", "model": "fake-local"},
    }


class FallbackCase(LiveTestCase):
    fallback = True
    cloud_up = False
    local_up = True

    @classmethod
    def setUpClass(cls):
        cls.cloud = FakeLLM(CLOUD_LLM_PORT, "fake-cloud")
        cls.local = FakeLLM(LOCAL_LLM_PORT, "fake-local")
        fakes = [f for f, up in ((cls.cloud, cls.cloud_up), (cls.local, cls.local_up)) if up]
        cls.start_instance(config(cls.fallback), SECRETS, fakes)
        try:
            cls.client.new_conversation()
        except Exception:
            cls.failures_in_class += 1
            raise

    def ask(self, words="What time is it?"):
        """A turn tagged with a nonce, and the daemon's log during it.  Records how
        many requests each fake had before it (assertNoRequests)."""
        nonce = uuid.uuid4().hex[:12]
        self.before = {f: len(f.chat_requests()) for f in (self.cloud, self.local)}
        mark = self.inst.log_mark()
        t = self.client.turn(f"{words} [{nonce}]")
        return nonce, t, self.inst.log_since(mark)

    def assertNoRequests(self, fake):
        """@p fake got no request at all during the last turn (not just none for its nonce)."""
        self.assertEqual(len(fake.chat_requests()), self.before[fake])

    def assertCloudAttempts(self, log, n):
        """@p n requests went to the cloud fake's address (refused or answered),
        and no name lookup failed (the turn never left for a real host)."""
        self.assertEqual(len(re.findall(r"url=" + re.escape(CLOUD_URL) + r" ", log)), n)
        self.assertNotIn("getaddrinfo", log)


class TestFallbackOff(FallbackCase):
    fallback = False

    def test_cloud_unreachable_fails_as_before(self):
        nonce, t, log = self.ask()
        self.assertTrue(t.errors, "the turn should fail")
        self.assertNoRequests(self.local)
        self.assertCloudAttempts(log, 4)  # the first try and the ladder's three
        self.assertGreaterEqual(t.seconds, LADDER_SEC)


class TestCloudUnreachable(FallbackCase):
    def test_1_first_turn_answers_from_local_with_the_note(self):
        nonce, t, log = self.ask()
        self.assertNoErrorFrames(t)
        self.assertTrue(t.reply.startswith(NOTE), t.reply[:120])
        self.assertIn("fake-local model answering", t.reply)
        self.assertEqual(len(self.local.chat_requests(nonce)), 1)
        self.assertCloudAttempts(log, 1)  # no retry ladder
        self.assertLess(t.seconds, LADDER_SEC)
        self.assertIn("unreachable: answering this turn on the local LLM", log)

    def test_2_next_turn_goes_straight_to_local_without_the_note(self):
        nonce, t, log = self.ask("And the date?")
        self.assertNoErrorFrames(t)
        self.assertNotIn(NOTE, t.reply)
        self.assertEqual(len(self.local.chat_requests(nonce)), 1)
        self.assertCloudAttempts(log, 0)
        self.assertIn("still unreachable: this turn goes to the local LLM", log)

    def test_3_a_tool_runs_once_on_local(self):
        self.local.script(tool_call("calculator", {"action": "evaluate", "value": "2+2"}),
                          text("It's 4."))
        nonce, t, log = self.ask("What is 2+2?")
        self.assertNoErrorFrames(t)
        self.assertIn("It's 4.", t.reply)
        self.assertCloudAttempts(log, 0)
        reqs = self.local.chat_requests(nonce)
        self.assertEqual(len(reqs), 2, "the tool call, then its result")
        tool_rows = [m for m in reqs[-1]["body"]["messages"] if m.get("role") == "tool"]
        self.assertEqual(len(tool_rows), 1, "the tool ran once")
        self.assertIn("4", str(tool_rows[0].get("content")))


class TestCloudFailing(FallbackCase):
    cloud_up = True

    def test_1_cloud_answers_no_local(self):
        nonce, t, log = self.ask()
        self.assertNoErrorFrames(t)
        self.assertIn("fake-cloud model answering", t.reply)
        self.assertNotIn(NOTE, t.reply)
        self.assertNoRequests(self.local)
        self.assertCloudAttempts(log, 1)

    def test_2_529s_fall_back_for_that_turn_only(self):
        self.cloud.script(*[status(529)] * 4)
        nonce, t, log = self.ask()
        self.assertNoErrorFrames(t)
        self.assertTrue(t.reply.startswith(NOTE), t.reply[:120])
        self.assertEqual(len(self.cloud.chat_requests(nonce)), 4, "the full retry ladder")
        self.assertEqual(len(self.local.chat_requests(nonce)), 1)
        # No outage window for a failing (not unreachable) cloud: the next turn tries it.
        nonce, t, log = self.ask("Again?")
        self.assertIn("fake-cloud model answering", t.reply)
        self.assertNoRequests(self.local)

    def test_3_after_a_cloud_tool_a_failure_stays_on_the_cloud(self):
        self.cloud.script(tool_call("calculator", {"action": "evaluate", "value": "2+2"}),
                          *[status(529)] * 4)
        nonce, t, log = self.ask("What is 2+2?")
        self.assertTrue(t.errors, "the turn fails: it can't move once a tool ran")
        self.assertNoRequests(self.local)
        reqs = self.cloud.chat_requests(nonce)
        self.assertEqual(len(reqs), 5, "the tool call, then its result four times")
        tool_rows = [m for m in reqs[-1]["body"]["messages"] if m.get("role") == "tool"]
        self.assertEqual(len(tool_rows), 1, "the tool ran once")


class TestBothDown(FallbackCase):
    local_up = False

    def test_local_down_too_fails_with_the_retries(self):
        nonce, t, log = self.ask()
        self.assertTrue(t.errors, "the turn should fail")
        self.assertIn(f"Local LLM fallback: http://127.0.0.1:{LOCAL_LLM_PORT} isn't answering", log)
        self.assertCloudAttempts(log, 5)  # one quick attempt, then the full retried run
        self.assertGreaterEqual(t.seconds, LADDER_SEC)
        self.assertNotIn("answering this turn on the local LLM", log)
        # Nothing was marked down: the next turn tries the cloud again.
        nonce, t, log = self.ask("Again?")
        self.assertNotIn("still unreachable", log)
        self.assertGreaterEqual(len(re.findall(re.escape(CLOUD_URL), log)), 1)


if __name__ == "__main__":
    unittest.main()
