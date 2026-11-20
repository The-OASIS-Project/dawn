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
# The harness itself: an instance answers a typed turn from a fake LLM and
# saves it, and the fence holds (no network beyond loopback, real state masked).

import os
import socket
import unittest

from fake_llm import FakeLLM
from instance import LOCAL_LLM_PORT, LiveTestCase


class TestInstance(LiveTestCase):
    @classmethod
    def setUpClass(cls):
        cls.local = FakeLLM(LOCAL_LLM_PORT, "fake-local")
        cls.start_instance(
            {"llm": {"type": "local"},
             "llm.local": {"endpoint": f"http://127.0.0.1:{LOCAL_LLM_PORT}",
                           "model": "fake-local"}},
            fakes=[cls.local])

    def test_a_turn_is_answered_and_saved(self):
        conv = self.client.new_conversation()
        t = self.client.turn("hello there")
        self.assertNoErrorFrames(t)
        self.assertIn("fake-local model answering", t.reply)
        self.assertEqual(self.shown_roles(conv), ["user", "assistant"])

    def test_the_network_is_fenced(self):
        s = socket.socket()
        s.settimeout(2)
        with self.assertRaises(OSError):
            s.connect(("192.168.1.1", 80))  # any LAN or internet address
        s.close()
        with self.assertRaises(OSError):
            socket.getaddrinfo("api.openai.com", 443)

    def test_real_dawn_state_is_masked(self):
        for d in ("/var/lib/dawn", "/etc/dawn", os.path.expanduser("~/.config/dawn")):
            if os.path.isdir(d):
                self.assertEqual(os.listdir(d), [], d)


if __name__ == "__main__":
    unittest.main()
