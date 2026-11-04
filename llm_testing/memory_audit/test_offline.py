# Offline tests for the memory audit (no database, no network).
#   python3 -m llm_testing.memory_audit.test_offline
#
# License: GPLv3, same as DAWN.

import contextlib
import io
import json
import os
import re
import sqlite3
import tempfile
import unittest

from . import candidates, coverage, models, reextract, rubric, storage, store
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


class Coverage(unittest.TestCase):
    """coverage with a stubbed judge: counts, the verdict cache, and no text."""

    def setUp(self):
        s = SECRET
        self.paths = []

        def write(name, rows):
            path = storage.data_path(name)
            with storage.private_open(path) as f:
                for r in rows:
                    f.write(json.dumps(r) + "\n")
            self.paths.append(path)
            return name

        a = [{"id": i, "text": f"{s}a{i}", "conversation": c} for i, c in
             [(1, 10), (2, 10), (3, 11), (4, 11)]]
        self.a = write("_test_cov_a.jsonl", a)
        self.b = write("_test_cov_b.jsonl", [{"id": 9, "text": s + "b", "conversation": 10}])
        kinds = {1: "identity", 2: "preference", 3: "work_log", 4: "assistant_said"}
        self.cls = write("_test_cov_class.jsonl", [
            {"id": i, "sha": storage.text_sha(f"{s}a{i}"), "kind": k, "tier": rubric.TIER[k]}
            for i, k in kinds.items()])
        self.out = storage.data_path("_test_cov_out.jsonl")
        self.paths.append(self.out)
        self.calls = []
        reply = {1: "covered", 2: "partial", 3: "missing"}

        self.fail = None  # an A fact text whose request fails

        def judge(spec, system, user, keys, endpoint):
            self.calls.append(user)
            if self.fail and self.fail in user:
                raise RuntimeError("judge request failed")
            n = len(re.findall(r"^\[(\d+)\]", user, re.M))  # A facts are numbered 1..n
            first = {f"{SECRET}a{i}": i for i in reply}
            ids = [first[t] for t in re.findall(r"^\[\d+\] (\S+)", user, re.M) if t in first]
            text = "Sure: " + json.dumps([{"id": k, "v": reply[i]}
                                          for k, i in enumerate(ids, 1)][:n])
            return text, {"in": 1, "out": 1}

        self._saved = (models.classify_batch, models.check_ready, models.load_keys)
        models.classify_batch = judge
        models.check_ready = lambda *a: None
        models.load_keys = lambda p: {}

    def tearDown(self):
        models.classify_batch, models.check_ready, models.load_keys = self._saved
        for p in self.paths:
            if os.path.exists(p):
                os.unlink(p)

    def _run(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            main(["coverage", self.a, self.b, "--a-class", self.cls, "--out", self.out,
                  "--workers", "1"])
        return out.getvalue()

    def test_counts_cache_and_no_text(self):
        got = self._run()
        self.assertIn("semantic  n=   2  covered 1  partial 1", got)
        self.assertIn("    10        1        1        0        0", got)  # per conversation
        self.assertIn("episodic  n=   1  missing 1", got)   # discard-tier fact 4 not judged
        self.assertIn("lost 50.0%", got)                   # missing 1 + partial half, of 3
        self.assertNotIn(SECRET, got)
        self.assertEqual(len(self.calls), 2)                 # one call per conversation
        self.assertIn(SECRET + "b", self.calls[0].split("other conversations")[0])
        again = self._run()                                  # verdicts come from the cache
        self.assertEqual(again.splitlines()[:-1], got.splitlines()[:-1])
        self.assertIn("judge tokens in 0", again)
        self.assertEqual(len(self.calls), 2)

    def test_loss_counts_judged_facts_only(self):
        self.fail = SECRET + "a3"  # conversation 11's request fails, fact 3 stays unjudged
        got = self._run()
        self.assertIn("episodic  n=   1  unjudged 1", got)
        self.assertIn("lost 25.0% of 2 judged, partial = half; 1 unjudged, so incomplete", got)
        self.assertNotIn(SECRET, got)

    def test_parse_verdicts(self):
        text = 'I see [1] and [2]: [{"id": "1", "v": "covered"}, {"id": 2, "v": "maybe"}, ' \
               '{"id": 7, "v": "missing"}]'
        self.assertEqual(coverage.parse_verdicts(text, {1, 2}), {1: "covered"})
        self.assertEqual(coverage.parse_verdicts("[{bad", {1}), {})


class Reextract(unittest.TestCase):
    def test_bench_command_dumps_candidates_when_asked(self):
        plain = reextract.bench_command("b", "claude", "m", "medium")
        self.assertNotIn("--dump-candidates", plain)
        cmd = reextract.bench_command("b", "claude", "m", "medium", candidates="c.jsonl")
        i = cmd.index("--dump-candidates")
        self.assertEqual(cmd[i + 1], os.path.abspath("c.jsonl"))

    def test_candidates_file_is_a_coverage_set(self):
        """The bench's candidate lines carry what coverage reads of a B set."""
        path = storage.data_path("_test_candidates.jsonl")
        try:
            with storage.private_open(path) as f:
                for i, conv in [(1, 10), (2, 10), (3, 12)]:
                    f.write(json.dumps({"id": i, "text": f"t{i}", "conversation": conv,
                                        "kind": "fact", "source": "explicit"}) + "\n")
            self.assertEqual(reextract.count_candidates(path), (3, 2))
            rows = coverage._read(path)
            self.assertTrue(all({"text", "conversation"} <= set(r) for r in rows))
        finally:
            os.unlink(path)

    def test_bench_command_sets_the_consolidator(self):
        self.assertNotIn("--consolidator", reextract.bench_command("b", "claude", "m", ""))
        cmd = reextract.bench_command("b", "claude", "m", "", consolidator="agent")
        self.assertEqual(cmd[cmd.index("--consolidator") + 1], "agent")

    def test_bench_command_sets_the_samples(self):
        self.assertNotIn("--samples", reextract.bench_command("b", "claude", "m", ""))
        cmd = reextract.bench_command("b", "claude", "m", "", consolidator="agent", samples=2)
        self.assertEqual(cmd[cmd.index("--samples") + 1], "2")

    def test_usage_lines_carry_the_call_kind_and_step(self):
        line = (b"LLM cache: provider=claude model=m kind=consolidation conv=0 iter=2 prompt=1200 "
                b"read=1000 write=0 uncached=200 coverage=83% expected=- state=warm output=300")
        m = reextract.USAGE_RE.search(line)
        self.assertEqual((m.group(1), m.group(2), m.group(3), m.group(4)),
                         (b"consolidation", b"2", b"1200", b"300"))
        self.assertEqual(reextract.usage_step("consolidation", 2), "consolidation/round")
        self.assertEqual(reextract.usage_step("consolidation", 0), "consolidation/sample")
        self.assertEqual(reextract.usage_step("extraction", -1), "extraction")


def _vec(*xs):
    import struct as _s
    return _s.pack(f"{len(xs)}f", *xs)


class Store(unittest.TestCase):
    def test_duplicates_and_percentile(self):
        vecs = [store._vec(_vec(1, 0)), store._vec(_vec(0.99, 0.1)), store._vec(_vec(0, 1))]
        self.assertEqual(store.duplicates(vecs), (1, 1, 2))
        self.assertEqual(store.percentile([1, 2, 3, 4], 0.5), 2.5)
        self.assertEqual(store.percentile([], 0.9), 0)

    def test_counts_from_a_scratch_database(self):
        path = storage.data_path("_test_store.db")
        if os.path.exists(path):
            os.unlink(path)
        con = sqlite3.connect(path)
        con.executescript("""
            CREATE TABLE memory_facts (id INTEGER PRIMARY KEY, fact_text TEXT, embedding BLOB,
              superseded_by INTEGER, superseded_kind INTEGER, outside INTEGER DEFAULT 0);
            CREATE TABLE memory_entities (id INTEGER PRIMARY KEY, canonical_name TEXT);
            CREATE TABLE memory_relations (id INTEGER PRIMARY KEY, subject_entity_id INTEGER,
              relation TEXT, fact_id INTEGER, valid_to INTEGER, closed_by_fact_id INTEGER);
            CREATE TABLE memory_preferences (id INTEGER PRIMARY KEY);
            CREATE TABLE conversations (id INTEGER PRIMARY KEY, origin TEXT);
            CREATE TABLE memory_consolidation_batches (id INTEGER PRIMARY KEY,
              conversation_id INTEGER, path TEXT, stop TEXT, outside INTEGER, rounds INTEGER);
            CREATE TABLE memory_consolidation_ops (id INTEGER PRIMARY KEY, batch_id INTEGER,
              op TEXT, status TEXT, reason TEXT);
        """)
        con.execute("INSERT INTO memory_facts VALUES (1, 'a', ?, NULL, NULL, 0)", (_vec(1, 0),))
        con.execute("INSERT INTO memory_facts VALUES (2, 'b', ?, NULL, NULL, 0)", (_vec(1, 0),))
        con.execute("INSERT INTO memory_facts VALUES (3, 'c', NULL, 1, 1, 0)")
        con.execute("INSERT INTO memory_entities VALUES (1, 'friday'), (2, 'jonathan smith')")
        con.execute("INSERT INTO memory_relations VALUES (1, 1, 'owns', 1, NULL, NULL), "
                    "(2, 2, 'married_to', 2, NULL, NULL), (3, 2, 'married_to', 2, NULL, NULL)")
        con.execute("INSERT INTO conversations VALUES (5, 'messaging:sms'), (6, 'webui')")
        con.execute("INSERT INTO memory_consolidation_batches VALUES (1, 5, 'agent', 'finished', "
                    "2, 1), (2, 6, 'agent_partial', 'tokens', 0, 0)")
        con.execute("INSERT INTO memory_consolidation_ops VALUES (1, 1, 'added', 'applied', "
                    "'nothing similar stored'), (2, 1, 'replaced', 'refused', 'x'), "
                    "(3, 2, 'reinforced', 'applied', 'the same text is stored')")
        con.commit()
        con.close()
        try:
            counts, record = store.store_counts(path, "Friday")
            self.assertEqual(counts["facts"], 2)
            self.assertEqual(counts["dup_pairs"], 1)
            self.assertEqual(counts["replaced"], 1)
            self.assertEqual(counts["replaced_not_outside"], 1)
            self.assertEqual(counts["assistant_subject"], 1)
            self.assertEqual(counts["one_at_a_time_conflicts"], 1)
            self.assertEqual(record["refused_replaces"], 1)
            self.assertEqual(record["triage"], {"new": 1, "reinforced": 1})
            self.assertEqual(record["outside_by_origin"]["messaging"]["tainted"], 1)
            self.assertEqual(record["ops_per_batch"], 1.5)
        finally:
            os.unlink(path)

    def test_usage_by_kind(self):
        path = storage.data_path("_test_usage.jsonl")
        with storage.private_open(path) as f:
            for conv, kind, i, o in [(1, "consolidation", 100, 10), (1, "consolidation", 50, 5),
                                     (2, "consolidation", 300, 30)]:
                f.write(json.dumps({"conversation": conv, "kind": kind, "calls": 1, "in": i,
                                    "out": o}) + "\n")
        try:
            u = store.usage_counts(path)["consolidation"]
            self.assertEqual(u["batches"], 2)
            self.assertEqual(u["in_p50"], 225)
            self.assertEqual(u["calls_per_batch"], 1.5)
        finally:
            os.unlink(path)

    def test_forgotten_seed_formats(self):
        path = storage.data_path("_test_forgotten.json")
        try:
            with storage.private_open(path) as f:
                json.dump(["one", {"text": "two"}], f)
            self.assertEqual(store.load_forgotten(path), ["one", "two"])
            with storage.private_open(path) as f:
                f.write(json.dumps({"text": "three"}) + "\n")
            self.assertEqual(store.load_forgotten(path), ["three"])
            self.assertEqual(store.fold("  Three  apples. "), "three apples")
        finally:
            os.unlink(path)


if __name__ == "__main__":
    unittest.main()
