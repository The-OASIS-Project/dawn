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
# A logged-in client's request with a field set to null, or a list sent as
# something else, gets a reply and leaves the daemon running.
#
# Not reached here: music_search {"query": null} and music_library
# {"type": null} / {"type": "tracks_by_artist", "artist": null}.  --server
# never opens the music database, so both answer "Music database not available"
# before they read the field.

import subprocess

from fake_llm import FakeLLM
from instance import LOCAL_LLM_PORT, LiveTestCase

# Never an account here: a request whose null is ignored must not find one.
NO_USER = "no-such-user"
PASSWORD = "xxxxxxxx-not-used"


class TestNullFields(LiveTestCase):
    @classmethod
    def setUpClass(cls):
        cls.local = FakeLLM(LOCAL_LLM_PORT, "fake-local")
        cls.start_instance(
            {"llm": {"type": "local"},
             "llm.local": {"endpoint": f"http://127.0.0.1:{LOCAL_LLM_PORT}",
                           "model": "fake-local"}},
            fakes=[cls.local])

    def ask(self, msg_type, payload, reply_type, match=None):
        """Send one frame; the daemon must still be running and must answer with
        @p reply_type (any frame of it when @p reply_type is None: just alive)."""
        try:
            self.client.send(msg_type, payload)
            reply = self.client.wait_for(reply_type, 10, match) if reply_type else None
        except (ConnectionError, TimeoutError, OSError) as e:
            try:
                rc = self.inst.proc.wait(5)  # a closed socket can beat the exit
            except subprocess.TimeoutExpired:
                rc = None
            if rc is not None:
                self.fail(f"{msg_type} {payload!r}: the daemon died (exit {rc})")
            raise AssertionError(f"{msg_type} {payload!r}: {e}") from e
        self.assertIsNone(self.inst.proc.poll(), f"{msg_type} {payload!r}: the daemon died")
        return reply

    def refused(self, msg_type, payload):
        reply = self.ask(msg_type, payload, f"{msg_type}_response")
        self.assertFalse(reply.get("success"), reply)
        return reply

    @staticmethod
    def code(c):
        return lambda p: p.get("code") == c

    # -- the null string fields ------------------------------------------------

    def test_change_password_null_username(self):
        self.refused("change_password", {"username": None, "new_password": PASSWORD})

    def test_change_password_null_new_password(self):
        self.refused("change_password", {"username": NO_USER, "new_password": None})

    def test_delete_user_null_username(self):
        self.refused("delete_user", {"username": None})

    def test_get_audio_devices_null_backend(self):
        self.ask("get_audio_devices", {"backend": None}, "get_audio_devices_response")

    def test_contacts_add_null_value(self):
        self.refused("contacts_add", {"entity_id": 1, "field_type": "email", "value": None})

    def test_contacts_update_null_value(self):
        self.refused("contacts_update", {"contact_id": 1, "field_type": "email", "value": None})

    def test_continue_conversation_null_summary(self):
        conv = self.client.new_conversation()
        self.client.turn("hello there")
        reply = self.ask("continue_conversation", {"conversation_id": conv, "summary": None},
                         "continue_conversation_response")
        self.assertTrue(reply.get("success"), reply)
        self.assertEqual(reply.get("summary"), "")

    def test_delete_all_memories_null_confirm(self):
        self.refused("delete_all_memories", {"confirm": None})

    def test_export_memories_null_format(self):
        self.ask("export_memories", {"format": None}, "export_memories_response")

    def test_import_memories_null_format(self):
        self.refused("import_memories", {"format": None})

    def test_set_channel_llm_null_fields(self):
        for key in ("llm_type", "cloud_provider", "model", "thinking_mode", "reasoning_effort"):
            with self.subTest(key=key):
                # Read as absent, the request goes on to look the conversation up.
                self.ask("set_channel_llm", {"conversation_id": 999999, key: None}, "error",
                         self.code("NOT_FOUND"))

    def test_music_control_null_action(self):
        self.ask("music_control", {"action": None}, "music_error", self.code("INVALID_REQUEST"))

    def test_music_queue_null_action(self):
        # music_control first: it creates the connection's music state, which
        # music_queue needs before it reads the action.
        self.ask("music_control", {"action": "no-such-action"}, "music_error",
                 self.code("UNKNOWN_ACTION"))
        self.ask("music_queue", {"action": None}, "music_error", self.code("INVALID_REQUEST"))

    # -- lists that aren't lists -----------------------------------------------

    def test_import_memories_facts_not_a_list(self):
        # A null list reads as none: nothing to import.
        reply = self.ask("import_memories", {"format": "json", "data": {"facts": None}},
                         "import_memories_response")
        self.assertEqual(reply.get("imported_facts"), 0, reply)
        for facts in ("a fact", {"fact_text": "a fact"}):
            with self.subTest(facts=facts):
                self.refused("import_memories", {"format": "json", "data": {"facts": facts}})

    def test_import_memories_preferences_not_a_list(self):
        for prefs in ("a preference", {"category": "a", "value": "b"}):
            with self.subTest(prefs=prefs):
                self.refused("import_memories",
                             {"format": "json", "data": {"preferences": prefs}})

    def test_set_tools_config_tools_not_a_list(self):
        for tools in (None, "search"):
            with self.subTest(tools=tools):
                self.refused("set_tools_config", {"tools": tools})

    # -- the null that must not widen a request --------------------------------

    def test_undo_memory_batch_null_op_id_survives(self):
        # Only that the daemon survives and answers: no batch exists on a fresh
        # instance, so the reply is "Not found" with or without the guard.  That
        # a null op_id never undoes the whole batch is proven by the unit test
        # in tests/test_webui_memory_batches.c.
        self.ask("undo_memory_batch", {"batch_id": 1, "op_id": None},
                 "undo_memory_batch_response")

    # -- settings sent with the wrong type -------------------------------------

    def test_set_config_wrong_types_leave_settings(self):
        def current():
            self.client.send("get_config", {})
            cfg = self.client.wait_for("get_config_response", 10)["config"]
            return (cfg["webui"]["max_clients"], cfg["webui"]["export_max_messages"],
                    cfg["llm"]["local_fallback"])

        before = current()
        reply = self.ask("set_config", {"webui": {"max_clients": "7", "export_max_messages": None},
                                        "llm": {"local_fallback": "yes" if not before[2] else 0}},
                         "set_config_response")
        self.assertTrue(reply.get("success"), reply)
        self.assertEqual(current(), before)

    # -- a payload that isn't an object ----------------------------------------

    def test_payload_not_an_object(self):
        for payload in (None, 5, "x", []):
            with self.subTest(payload=payload):
                self.ask("change_password", payload,
                         None if payload is None else "change_password_response")
                self.ask("import_memories", payload,
                         None if payload is None else "import_memories_response")
                self.ask("get_audio_devices", payload, "get_audio_devices_response")


if __name__ == "__main__":
    import unittest

    unittest.main()
