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

import json
import sqlite3
import time
import unittest
import uuid

from fake_llm import FakeLLM, text, tool_call
from dawn_client import Client
from instance import ADMIN_USER, CLOUD_LLM_PORT, LOCAL_LLM_PORT, WEBUI_PORT, LiveTestCase

CLOUD_URL = f"http://127.0.0.1:{CLOUD_LLM_PORT}"


def config(provider, model="fake-cloud", llm_type="cloud", local_model="fake-local"):
    return {
        "llm": {"type": llm_type},
        "llm.cloud": {"provider": provider, "endpoint": CLOUD_URL,
                      "openai_models": [model], "openai_default_model_idx": 0,
                      "openai_use_responses_api": "never"},
        "llm.local": {"endpoint": f"http://127.0.0.1:{LOCAL_LLM_PORT}", "model": local_model},
    }


class SelectionCase(LiveTestCase):
    provider = ""
    secrets = ""
    model = "fake-cloud"
    llm_type = "cloud"
    local_model = "fake-local"

    @classmethod
    def setUpClass(cls):
        cls.cloud = FakeLLM(CLOUD_LLM_PORT, "fake-cloud")
        cls.local = FakeLLM(LOCAL_LLM_PORT, "fake-local")
        cls.start_instance(config(cls.provider, cls.model, cls.llm_type, cls.local_model), cls.secrets,
                           [cls.cloud, cls.local])
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
    """A conversation with no selection of its own moves when the default does:
    its open session at the next turn, and when it's opened again later."""

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

    def test_1_a_settings_save_moves_the_next_turn(self):
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

    def test_2_reopened_it_runs_on_the_new_default(self):
        rows = self.inst.db("SELECT llm_type, cloud_provider, model FROM conversations "
                            "WHERE id = ?", (self.conv,))
        self.assertEqual(rows, [("", "", "")], "no selection stored for it")

        other = Client(WEBUI_PORT, ADMIN_USER, self.inst.password)
        self.addCleanup(other.close)
        loaded = other.load_conversation(self.conv)
        self.assertTrue(loaded["llm_settings"]["follows_default"], loaded.get("llm_settings"))

        # Created on local, opened again after the default moved to the cloud.
        nonce = uuid.uuid4().hex[:12]
        before = len(self.local.chat_requests())
        t = other.turn(f"Reopened? [{nonce}]")
        self.assertNoErrorFrames(t)
        self.assertIn("fake-cloud model answering", t.reply)
        self.assertEqual(len(self.local.chat_requests()), before)


class TestPicksAreChecked(SelectionCase):
    """A pick without its provider's key is refused when made; "cloud" on a local
    default means the provider dawn.toml names."""

    llm_type = "local"
    provider = "openai"
    secrets = '[secrets]\nopenai_api_key = "sk-live-test"\n'

    def set_llm(self, payload, timeout=10):
        self.client.send("set_session_llm", payload)
        deadline = time.monotonic() + timeout
        while True:
            f = self.client._recv(deadline)
            if f is None:
                raise TimeoutError("no set_session_llm_response")
            if f.get("type") == "set_session_llm_response":
                return f["payload"]

    def test_1_a_provider_without_its_key_is_refused(self):
        reply = self.set_llm({"type": "cloud", "provider": "gemini"})
        self.assertFalse(reply.get("success"), reply)
        self.assertIn("Gemini is selected, but it has no API key", reply.get("error", ""))
        nonce, t = self.ask()
        self.assertIn("fake-local model answering", t.reply)  # nothing changed

    def test_2_switch_to_cloud_uses_the_configured_provider(self):
        self.local.script(tool_call("switch_llm", {"target": "cloud"}), text("Switched."))
        nonce, t = self.ask("Switch to the cloud.")
        self.assertNoErrorFrames(t)
        nonce, t = self.ask("And now?")
        self.assertIn("fake-cloud model answering", t.reply)
        self.assertEqual(len(self.cloud.chat_requests(nonce)), 1)

    def test_3_a_pick_for_another_conversation_changes_nothing(self):
        # A late follow-up for a conversation the user has left (test_2 left this
        # session on the cloud): nothing in it applies here.
        reply = self.set_llm({"type": "local", "model": "fake-local",
                              "conversation_id": self.conv + 1000})
        self.assertEqual(reply.get("type"), "cloud", reply)
        nonce, t = self.ask("Still the cloud?")
        self.assertIn("fake-cloud model answering", t.reply)
        self.assertNoRequests(self.local)



class TestSwitchInAPrivateConversation(SelectionCase):
    """A switch the user asks for lasts in a private conversation too, as a pick in
    the picker does."""

    llm_type = "local"
    provider = "openai"
    secrets = '[secrets]\nopenai_api_key = "sk-live-test"\n'

    def recv_type(self, kind, timeout=10):
        deadline = time.monotonic() + timeout
        while True:
            f = self.client._recv(deadline)
            if f is None:
                raise TimeoutError(f"no {kind}")
            if f.get("type") == kind:
                return f["payload"]

    def test_it_sticks(self):
        self.client.send("set_private", {"conversation_id": self.conv, "is_private": True})
        self.recv_type("set_private_response")
        self.local.script(tool_call("switch_llm", {"target": "cloud"}), text("Switched."))
        nonce, t = self.ask("Switch to the cloud.")
        self.assertNoErrorFrames(t)
        nonce, t = self.ask("And now?")
        self.assertIn("fake-cloud model answering", t.reply)
        self.assertNoRequests(self.local)
        rows = self.inst.db("SELECT llm_type, cloud_provider, is_private FROM conversations "
                            "WHERE id = ?", (self.conv,))
        self.assertEqual(rows, [("cloud", "openai", 1)])


class TestSwitchToLocalMidTurn(SelectionCase):
    """A switch to local mid-turn sends the rest of the turn with the local model
    (here the server's own: none configured), never the cloud model's name."""

    provider = "openai"
    local_model = ""
    secrets = '[secrets]\nopenai_api_key = "sk-live-test"\n'

    def test_2_reopened_it_reports_no_model(self):
        # test_1 left it local with no model of its own: a restore must not send a
        # placeholder back as a model name.
        rows = self.inst.db("SELECT llm_type, model FROM conversations WHERE id = ?", (self.conv,))
        self.assertEqual(rows, [("local", "")])
        other = Client(WEBUI_PORT, ADMIN_USER, self.inst.password)
        self.addCleanup(other.close)
        settings = other.load_conversation(self.conv)["llm_settings"]
        self.assertEqual(settings.get("llm_type"), "local", settings)
        self.assertEqual(settings.get("model"), "", settings)

    def test_1_the_rest_of_the_turn_names_no_cloud_model(self):
        self.cloud.script(tool_call("switch_llm", {"target": "local"}))
        self.local.script(text("Local now."))
        nonce, t = self.ask("Switch to local.")
        self.assertNoErrorFrames(t)
        reqs = self.local.chat_requests()[self.before[self.local]:]
        self.assertTrue(reqs, "the turn went on to the local server")
        self.assertNotIn(reqs[0]["body"].get("model"), ("fake-cloud",), reqs[0]["body"].get("model"))


class TestBackToTheDefault(SelectionCase):
    """"Use the default" clears a conversation's own pick: saved, so it follows the
    default from then on."""

    provider = "openai"
    secrets = '[secrets]\nopenai_api_key = "sk-live-test"\n'

    def set_llm(self, payload, timeout=10):
        self.client.send("set_session_llm", payload)
        deadline = time.monotonic() + timeout
        while True:
            f = self.client._recv(deadline)
            if f is None:
                raise TimeoutError("no set_session_llm_response")
            if f.get("type") == "set_session_llm_response":
                return f["payload"]

    def row(self):
        return self.inst.db("SELECT llm_type, cloud_provider, model FROM conversations "
                            "WHERE id = ?", (self.conv,))

    def test_it_follows_the_default_again(self):
        nonce, t = self.ask("Hello.")  # saves the conversation
        self.assertTrue(self.set_llm({"type": "local", "model": "fake-local"}).get("success"))
        self.assertEqual(self.row()[0][0], "local")
        reply = self.set_llm({"type": "reset"})
        self.assertTrue(reply.get("follows_default"), reply)
        self.assertEqual(self.row(), [("", "", "")])
        nonce, t = self.ask("And now?")
        self.assertIn("fake-cloud model answering", t.reply)

class TestALocalModelNamedLikeACloudOne(SelectionCase):
    """A model no dawn.toml cloud list names stays local, whether or not the local
    server's list was ever fetched (Ollama's gpt-oss is named like an OpenAI
    model); a cloud vendor nobody picked never gets the conversation."""

    llm_type = "local"
    provider = "openai"
    secrets = '[secrets]\nopenai_api_key = "sk-live-test"\n'

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.local.name = "gpt-oss-20b"  # what the local server lists

    def set_llm(self, payload, timeout=10):
        self.client.send("set_session_llm", payload)
        deadline = time.monotonic() + timeout
        while True:
            f = self.client._recv(deadline)
            if f is None:
                raise TimeoutError("no set_session_llm_response")
            if f.get("type") == "set_session_llm_response":
                return f["payload"]

    def assert_stays_local(self, reply):
        self.assertTrue(reply.get("success"), reply)
        self.assertEqual(reply.get("type"), "local", reply)
        nonce, t = self.ask()
        self.assertNoRequests(self.cloud)
        self.assertEqual(len(self.local.chat_requests(nonce)), 1)

    def test_picking_it_alone_keeps_the_session_local(self):
        self.assert_stays_local(self.set_llm({"model": "gpt-oss-20b"}))

    def test_restoring_it_keeps_it_local(self):
        # A restore echoes back the provider the server sent: "none" for local
        self.assert_stays_local(self.set_llm({"type": "local", "provider": "none",
                                              "model": "gpt-oss-20b", "from_restore": True}))

    def test_a_reasoning_change_keeps_it_local(self):
        self.assertTrue(self.set_llm({"type": "local", "model": "gpt-oss-20b"}).get("success"))
        self.assert_stays_local(self.set_llm({"thinking_mode": "disabled"}))


class TestRetiredModelOnACustomEndpoint(SelectionCase):
    """A retired model's replacement (models.toml [retired]) is for the vendor's own
    URL: behind a proxy or on Azure the id is a deployment name the user chose."""

    provider = "openai"
    secrets = '[secrets]\nopenai_api_key = "sk-live-test"\n'
    model = "gpt-4"

    def test_the_request_keeps_its_model(self):
        nonce, t = self.ask()
        self.assertNoErrorFrames(t)
        reqs = self.cloud.chat_requests(nonce)
        self.assertEqual(len(reqs), 1)
        self.assertEqual(reqs[0]["body"].get("model"), "gpt-4")


class TestAJobRunsOnItsConversationsLLM(SelectionCase):
    """A job runs on the LLM of the conversation that started it, not on the
    default; Resume checks that LLM again and says why it can't run."""

    llm_type = "local"
    provider = "openai"
    secrets = '[secrets]\nopenai_api_key = "sk-live-test"\n'

    def recv_type(self, kind, timeout=10):
        deadline = time.monotonic() + timeout
        while True:
            f = self.client._recv(deadline)
            if f is None:
                raise TimeoutError(f"no {kind}")
            if f.get("type") == kind:
                return f["payload"]

    def job_row(self):
        rows = self.inst.db("SELECT id, job_status, llm_type, cloud_provider FROM conversations "
                            "WHERE parent_id = ? AND job_status IS NOT NULL", (self.conv,))
        return rows[0] if rows else None

    def test_1_spawned_from_a_cloud_conversation_it_runs_there(self):
        self.client.send("set_session_llm", {"type": "cloud", "provider": "openai",
                                             "model": "fake-cloud"})
        self.assertTrue(self.recv_type("set_session_llm_response").get("success"))
        job_nonce = uuid.uuid4().hex[:12]
        goal = {"goal": f"Count to three [{job_nonce}]", "on_complete": "none"}
        self.cloud.script(tool_call("job", {"action": "spawn", "arguments": json.dumps(goal)}),
                          text("Started."))
        nonce, t = self.ask("Start a job.")
        self.assertNoErrorFrames(t)

        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and not self.cloud.chat_requests(job_nonce):
            time.sleep(0.25)
        self.assertTrue(self.cloud.chat_requests(job_nonce), "the job's turn went to the cloud")
        self.assertEqual(self.local.chat_requests(job_nonce), [])
        row = self.job_row()
        self.assertIsNotNone(row)
        self.assertEqual(row[2:], ("cloud", "openai"))

    def test_2_resume_says_why_it_cant_run(self):
        # The job test_1 started (tests run in name order)
        row = self.job_row()
        self.assertIsNotNone(row)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and self.job_row()[1] in ("queued", "running"):
            time.sleep(0.25)
        self.assertNotIn(self.job_row()[1], ("queued", "running"), "the job finished")
        con = sqlite3.connect(f"{self.inst.run}/data/auth.db", timeout=10)
        try:
            con.execute("UPDATE conversations SET job_status = 'failed', cloud_provider = 'gemini' "
                        "WHERE id = ?", (row[0],))
            con.commit()
        finally:
            con.close()
        # Its worker frees the job's slot just after writing its status: retry
        # while Resume still reads it as live.
        for _ in range(20):
            self.client.send("job_action", {"action": "resume", "conversation_id": row[0]})
            reply = self.recv_type("job_action_response")
            if "can't be resumed now" not in reply.get("message", ""):
                break
            time.sleep(0.25)
        self.assertFalse(reply.get("ok"), reply)
        self.assertIn("Gemini is selected, but it has no API key", reply.get("message", ""))
        self.assertEqual(self.job_row()[1], "failed", "nothing was claimed")


if __name__ == "__main__":
    unittest.main()
