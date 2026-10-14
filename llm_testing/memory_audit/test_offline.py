# Offline tests for the memory audit (no database, no network).
#   python3 -m llm_testing.memory_audit.test_offline
#
# License: GPLv3, same as DAWN.

import unittest

from . import models, rubric
from .__main__ import kappa, stratified


def _fact(i, cat, month):
    return {"id": i, "category": cat, "date": f"2026-{month:02d}-01"}


class Stratified(unittest.TestCase):
    def test_size_and_spread(self):
        facts = [_fact(i, "work" if i % 4 else "home", 1 + i % 6) for i in range(600)]
        got = stratified(facts, 60, seed=1)
        self.assertEqual(len(got), 60)
        self.assertEqual(len({f["id"] for f in got}), 60)
        home = sum(1 for f in got if f["category"] == "home")
        self.assertTrue(13 <= home <= 17, home)  # a quarter of the facts

    def test_n_above_total_returns_all(self):
        facts = [_fact(i, "x", 1) for i in range(5)]
        self.assertEqual(len(stratified(facts, 10, seed=1)), 5)

    def test_seeded(self):
        facts = [_fact(i, "x" if i % 2 else "y", 1 + i % 3) for i in range(100)]
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


if __name__ == "__main__":
    unittest.main()
