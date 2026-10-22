# Offline tests for the memory audit (no database, no network).
#   python3 -m llm_testing.memory_audit.test_offline
#
# License: GPLv3, same as DAWN.

import contextlib
import io
import os
import sqlite3
import tempfile
import unittest

from . import candidates, models, rubric, storage
from .__main__ import kappa, main, stratified


def _fact(i, month):
    return {"id": i, "date": f"2026-{month:02d}-01"}


class Stratified(unittest.TestCase):
    def test_size_and_spread(self):
        facts = [_fact(i, 2 + i % 5 if i % 4 else 1) for i in range(600)]
        got = stratified(facts, 60, seed=1)
        self.assertEqual(len(got), 60)
        self.assertEqual(len({f["id"] for f in got}), 60)
        jan = sum(1 for f in got if f["date"].startswith("2026-01"))
        self.assertTrue(13 <= jan <= 17, jan)  # a quarter of the facts

    def test_n_above_total_returns_all(self):
        facts = [_fact(i, 1) for i in range(5)]
        self.assertEqual(len(stratified(facts, 10, seed=1)), 5)

    def test_seeded(self):
        facts = [_fact(i, 1 + i % 3) for i in range(100)]
        self.assertEqual([f["id"] for f in stratified(facts, 20, 7)],
                         [f["id"] for f in stratified(facts, 20, 7)])


class Parse(unittest.TestCase):
    def test_fenced_and_partial(self):
        text = 'Here:\n```json\n[{"id": 3, "kind": "preference", "reason": "likes tea"},' \
               ' {"id": "4", "kind": "work_log", "reason": "a commit"},' \
               ' {"id": 5, "kind": "nonsense"}, {"id": 99, "kind": "identity"}]\n```'
        got = models.parse_reply(text, [3, 4, 5], rubric.KIND_NAMES)
        self.assertEqual(got, {3: ("preference", "likes tea", None),
                               4: ("work_log", "a commit", None)})

    def test_tier_reply(self):
        text = '[{"id": 1, "tier": "episodic", "kind": "work_log", "reason": "r"},' \
               ' {"id": 2, "tier": "bogus", "kind": "identity"}]'
        got = models.parse_reply(text, [1, 2], rubric.KIND_NAMES, rubric.TIERS)
        self.assertEqual(got, {1: ("work_log", "r", "episodic")})
        self.assertIn("{kinds}", rubric.TIER_SYSTEM)
        rubric.tier_system_prompt("friday", "2026-10-11")  # formats without error

    def test_garbage(self):
        self.assertEqual(models.parse_reply("no json here", [1], rubric.KIND_NAMES), {})
        self.assertEqual(models.parse_reply("[{bad", [1], rubric.KIND_NAMES), {})

    def test_spec(self):
        self.assertEqual(models.parse_spec("claude:claude-haiku-5-5"),
                         ("claude", "claude-haiku-5-5"))
        with self.assertRaises(SystemExit):
            models.parse_spec("haiku")


class Kappa(unittest.TestCase):
    def test_values(self):
        self.assertEqual(kappa([("a", "a"), ("b", "b")]), 1.0)
        self.assertAlmostEqual(kappa([("a", "a"), ("a", "b"), ("b", "a"), ("b", "b")]), 0.0)

    def test_weighted_kappa(self):
        pairs = [("identity", "reference"), ("reference", "identity"),
                 ("ongoing", "ongoing"), ("speculative", "identity")]
        self.assertGreater(kappa(pairs, rubric.credit), kappa(pairs))
        self.assertEqual(rubric.credit("upcoming", "ongoing"), 0.5)
        self.assertEqual(rubric.credit("identity", "too_vague"), 0.0)
        for pair in rubric.SIMILAR:
            self.assertTrue(pair <= set(rubric.KIND_NAMES), pair)


class Rubric(unittest.TestCase):
    def test_kinds(self):
        self.assertEqual(len(set(rubric.KIND_NAMES)), len(rubric.KIND_NAMES))
        self.assertEqual(set(rubric.TIER.values()), set(rubric.TIERS))
        prompt = rubric.system_prompt("Friday", "2026-10-09")
        self.assertIn("Today is 2026-10-09", prompt)
        for k in rubric.KIND_NAMES:
            self.assertIn(k, prompt)



class TestStorage(unittest.TestCase):
    def test_paths_stay_under_data(self):
        self.assertTrue(storage.data_path("x.jsonl").startswith(os.path.realpath(storage.DATA)))
        with self.assertRaises(SystemExit):
            storage.data_path("./escape.jsonl")
        with self.assertRaises(SystemExit):
            storage.data_path(os.path.join(storage.DATA, "..", "escape.jsonl"))

    def test_private_files_are_0600(self):
        path = storage.data_path("_test_private.tmp")
        old = os.umask(0o022)
        try:
            with storage.private_open(path) as f:
                f.write("x")
            self.assertEqual(os.stat(path).st_mode & 0o777, 0o600)
            with storage.private_open(path, "a") as f:
                f.write("y")
            self.assertEqual(open(path).read(), "xy")
        finally:
            os.umask(old)
            os.unlink(path)

    def test_fingerprint_tracks_text(self):
        self.assertEqual(storage.text_sha("a"), storage.text_sha("a"))
        self.assertNotEqual(storage.text_sha("a"), storage.text_sha("b"))


SECRET = "zz-secret-"  # every text written below starts with it; none may be printed

_CANDIDATE_SCHEMA = """
CREATE TABLE conversations (id INTEGER PRIMARY KEY, user_id INTEGER);
CREATE TABLE messages (id INTEGER PRIMARY KEY, conversation_id INTEGER, role TEXT,
                       content TEXT, kind TEXT);
CREATE TABLE memory_facts (id INTEGER PRIMARY KEY, user_id INTEGER, fact_text TEXT,
                           superseded_by INTEGER, superseded_kind INTEGER NOT NULL DEFAULT 0,
                           source_conversation_id INTEGER);
CREATE TABLE memory_forgotten (id INTEGER PRIMARY KEY, user_id INTEGER, fact_text TEXT,
                               forget_msg_id INTEGER, created_at INTEGER);
CREATE TABLE memory_citation_audit (id INTEGER PRIMARY KEY, conversation_id INTEGER,
                                    message_id INTEGER, user_id INTEGER, injected_ids TEXT,
                                    cited_ids TEXT);
"""


class Candidates(unittest.TestCase):
    def setUp(self):
        fd, self.db = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        con = sqlite3.connect(self.db)
        con.executescript(_CANDIDATE_SCHEMA)
        s = SECRET
        con.executemany("INSERT INTO conversations VALUES (?, ?)", [(10, 1), (11, 1), (20, 2)])
        con.executemany("INSERT INTO messages VALUES (?, ?, ?, ?, ?)", [
            (1, 10, "user", s + "what's my number", None),
            (2, 10, "assistant", s + "it is 555", None),
            (3, 10, "user", "No, " + s + "it changed", None),        # correction, memory reply
                                                                     # (audited under msg 1)
            (4, 11, "user", "Actually " + s + "hi", None),           # no reply before it
            (5, 11, "assistant", s + "hello", None),
            (6, 11, "user", s + "context: actually", "context"),     # a context row
            (7, 11, "user", s + "thanks, that's wrong", None),       # correction, no memory
            (8, 20, "assistant", s + "x", None),
            (9, 20, "user", "no, " + s + "other user", None)])
        con.executemany("INSERT INTO memory_facts VALUES (?, ?, ?, ?, ?, ?)", [
            (1, 1, s + "old", 2, 1, 10), (2, 1, s + "new", None, 0, 11),
            (3, 1, s + "dup", 2, 0, 10), (4, 2, s + "theirs", 5, 1, 20)])
        con.executemany("INSERT INTO memory_forgotten VALUES (?, ?, ?, ?, ?)", [
            (1, 1, s + "gone", 3, 0), (2, 1, s + "panel", 9, 0), (3, 2, s + "theirs", 9, 0)])
        con.executemany("INSERT INTO memory_citation_audit VALUES (?, ?, ?, ?, ?, ?)", [
            (1, 10, 1, 1, "fact:1", ""), (2, 11, 99, 1, "fact:2", "fact:2"),
            (3, 11, 0, 1, "", ""), (4, 20, 8, 2, "fact:4", "")])
        con.commit()
        con.close()

    def tearDown(self):
        os.unlink(self.db)

    def test_counts_and_ids(self):
        got = {s["name"].split(":")[0]: s for s in candidates.collect(self.db, 1)}
        self.assertEqual((got["forgetting"]["count"], got["forgetting"]["ids"]), (2, [10]))
        self.assertEqual((got["staleness"]["count"], got["staleness"]["ids"]), (1, [11]))
        corr = got["staleness/sycophancy"]
        self.assertEqual((corr["count"], corr["ids"]), (2, [10, 11]))
        self.assertTrue(corr["note"].startswith("1 after"))
        self.assertEqual((got["injected, none cited"]["count"],
                          got["injected, none cited"]["ids"]), (1, [10]))
        self.assertIn("no attention_log table", got["over-interruption"]["note"])

    def test_prints_no_text(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            main(["candidates", "--db", self.db])
        self.assertNotIn(SECRET, out.getvalue())
        self.assertIn("forgetting: 2", out.getvalue())

    def test_read_only(self):
        con = sqlite3.connect(f"file:{self.db}?mode=ro", uri=True)
        with self.assertRaises(sqlite3.OperationalError):
            con.execute("DELETE FROM messages")
        con.close()
        candidates.collect(self.db, 1)
        con = sqlite3.connect(self.db)
        self.assertEqual(con.execute("SELECT COUNT(*) FROM messages").fetchone()[0], 9)
        con.close()


if __name__ == "__main__":
    unittest.main()
