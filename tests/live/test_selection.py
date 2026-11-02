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
# Which LLM a chat runs on: a key is used only for the provider dawn.toml
# selects.  No provider selected runs on the local LLM even with keys present;
# a selected provider without its key is refused with the reason.

import time
import unittest
import uuid

from fake_llm import FakeLLM
from instance import CLOUD_LLM_PORT, LOCAL_LLM_PORT, LiveTestCase

CLOUD_URL = f"http://127.0.0.1:{CLOUD_LLM_PORT}"


def config(provider):
    return {
        "llm": {"type": "cloud"},
        "llm.cloud": {"provider": provider, "endpoint": CLOUD_URL,
                      "openai_models": ["fake-cloud"], "openai_default_model_idx": 0,
                      "openai_use_responses_api": "never"},
        "llm.local": {"endpoint": f"http://127.0.0.1:{LOCAL_LLM_PORT}", "model": "fake-local"},
    }


class SelectionCase(LiveTestCase):
    provider = ""
    secrets = ""

    @classmethod
    def setUpClass(cls):
        cls.cloud = FakeLLM(CLOUD_LLM_PORT, "fake-cloud")
        cls.local = FakeLLM(LOCAL_LLM_PORT, "fake-local")
        cls.start_instance(config(cls.provider), cls.secrets, [cls.cloud, cls.local])
        try:
            cls.conv = cls.client.new_conversation()
        except Exception:
            cls.failures_in_class += 1
            raise

    def ask(self, words="What time is it?"):
        nonce = uuid.uuid4().hex[:12]
        self.before = {f: len(f.chat_requests()) for f in (self.cloud, self.local)}
        return nonce, self.client.turn(f"{words} [{nonce}]")

    def assertNoRequests(self, fake):
        self.assertEqual(len(fake.chat_requests()), self.before[fake])


class TestNoProviderSelected(SelectionCase):
    """Keys present, no provider picked: the local LLM answers; no key is used."""

    secrets = '[secrets]\nopenai_api_key = "sk-live-test"\nclaude_api_key = "sk-ant-live"\n'

    def test_local_answers_and_the_cloud_gets_nothing(self):
        nonce, t = self.ask()
        self.assertNoErrorFrames(t)
        self.assertIn("fake-local model answering", t.reply)
        self.assertEqual(len(self.local.chat_requests(nonce)), 1)
        self.assertNoRequests(self.cloud)


class TestProviderWithoutKey(SelectionCase):
    """OpenAI selected, only another provider's key: refused, with the reason."""

    provider = "openai"
    secrets = '[secrets]\nclaude_api_key = "sk-ant-live"\n'

    def test_refused_with_the_reason_and_nothing_sent_or_saved(self):
        nonce, t = self.ask()
        self.assertTrue(t.errors, "the turn should be refused")
        self.assertIn("OpenAI is selected, but it has no API key", t.errors[0].get("message", ""))
        self.assertNoRequests(self.cloud)
        self.assertNoRequests(self.local)
        self.assertEqual(self.shown_roles(self.conv), [], "the refused question isn't saved")


class TestFollowsTheDefault(SelectionCase):
    """The live session of a conversation with no selection of its own moves when the
    default does (a reloaded conversation: with the conversation rows)."""

    secrets = '[secrets]\nopenai_api_key = "sk-live-test"\n'

    def save_settings(self, payload, timeout=10):
        self.client.send("set_config", payload)
        deadline = time.monotonic() + timeout
        while True:
            f = self.client._recv(deadline)
            if f is None:
                raise TimeoutError("no set_config_response")
            if f.get("type") == "set_config_response":
                return f["payload"]

    def test_a_settings_save_moves_the_next_turn(self):
        nonce, t = self.ask()
        self.assertIn("fake-local model answering", t.reply)
        self.assertNoRequests(self.cloud)

        reply = self.save_settings({"llm": {"cloud": {"provider": "openai"}}})
        self.assertTrue(reply.get("success"), reply)

        nonce, t = self.ask("And now?")
        self.assertNoErrorFrames(t)
        self.assertIn("fake-cloud model answering", t.reply)
        self.assertEqual(len(self.cloud.chat_requests(nonce)), 1)
        self.assertNoRequests(self.local)


if __name__ == "__main__":
    unittest.main()
