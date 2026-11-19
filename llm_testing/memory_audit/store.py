# Memory audit: what a re-extraction's store ended up holding, by count.
#
# `store` reads one or more saved scratch databases (reextract_<label>.db) and
# prints, per run: live facts, entities, relations, preferences; facts merged
# and replaced (and how many of those carried no outside text); relations
# closed; near-duplicate live facts (embedding cosine at the memory tool's
# find_duplicates threshold); relations whose subject is the assistant; facts
# without a relation; subjects holding two open values of a one-at-a-time
# relation; and, for a consolidator run, its record: batches by stop and path,
# ops by kind and status, ops per batch, triage's rules, outside-text batches
# by conversation origin.  With the run's usage file, tokens per batch by call
# kind (p50/p90).
#
# `comebacks` asks a judge, for each forgotten fact seeded into a run
# (reextract --forgotten), whether the run's live facts state it again.
# `world` asks a judge how many extraction candidates are claims about the
# outside world whose truth changes and can be checked.
#
# Privacy: databases are opened read-only; fact text goes only to the judge
# model's API; this prints counts.
#
# License: GPLv3, same as DAWN.

import json
import math
import re
import sqlite3
import struct
from collections import Counter, defaultdict

from . import coverage, models
from .storage import data_path

DUP_COSINE = 0.85  # find_duplicates' default
ONE_AT_A_TIME = ("married_to", "born_in", "born_on", "favorite_color", "favorite_food",
                 "primary_language")
# The consolidator's triage reasons (MC_REASON_TRIAGE_* in memory_consolidate_internal.h).
TRIAGE_REASONS = {"nothing similar stored": "new", "the same text is stored": "reinforced",
                  "the same text was forgotten": "forgotten"}


def _open(path):
    con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    con.row_factory = sqlite3.Row
    return con


def _columns(con, table):
    return {r[1] for r in con.execute(f"PRAGMA table_info({table})")}


def _vec(blob):
    if not blob or len(blob) % 4:
        return None
    v = struct.unpack(f"{len(blob) // 4}f", blob)
    n = math.sqrt(sum(x * x for x in v))
    return (v, n) if n > 0 else None


def duplicates(vectors, threshold=DUP_COSINE):
    """(pairs, clusters, facts in clusters) at or above @threshold cosine."""
    parent = list(range(len(vectors)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    pairs = 0
    for i in range(len(vectors)):
        a, na = vectors[i]
        for j in range(i + 1, len(vectors)):
            b, nb = vectors[j]
            if len(a) == len(b) and sum(x * y for x, y in zip(a, b)) / (na * nb) >= threshold:
                pairs += 1
                parent[find(i)] = find(j)
    groups = Counter(find(i) for i in range(len(vectors)))
    clustered = [g for g, n in groups.items() if n > 1]
    return pairs, len(clustered), sum(groups[g] for g in clustered)


def percentile(values, p):
    if not values:
        return 0
    s = sorted(values)
    k = (len(s) - 1) * p
    lo, hi = int(math.floor(k)), int(math.ceil(k))
    return s[lo] if lo == hi else s[lo] + (s[hi] - s[lo]) * (k - lo)


def tainted(outside):
    return outside not in (0, 8, None)  # OUTSIDE_NONE, OUTSIDE_UNKNOWN


def store_counts(path, assistant="friday"):
    """The counts for one scratch database, as a dict (no text)."""
    con = _open(path)
    fc = _columns(con, "memory_facts")
    out = Counter()
    live = con.execute("SELECT id, embedding FROM memory_facts WHERE superseded_by IS NULL"
                       ).fetchall()
    out["facts"] = len(live)
    vecs = [v for v in (_vec(r["embedding"]) for r in live) if v]
    out["dup_pairs"], out["dup_clusters"], out["dup_facts"] = duplicates(vecs)
    kind_col = "superseded_kind" if "superseded_kind" in fc else None
    outside_col = "outside" if "outside" in fc else None
    for r in con.execute("SELECT %s AS k, %s AS o FROM memory_facts WHERE superseded_by IS NOT "
                         "NULL" % (kind_col or "NULL", outside_col or "0")):
        what = "merged" if r["k"] == 0 else "replaced" if r["k"] == 1 else "superseded"
        out[what] += 1
        if not tainted(r["o"]):
            out[f"{what}_not_outside"] += 1
    out["entities"] = con.execute("SELECT COUNT(*) FROM memory_entities").fetchone()[0]
    out["relations"] = con.execute("SELECT COUNT(*) FROM memory_relations").fetchone()[0]
    out["relations_closed"] = con.execute(
        "SELECT COUNT(*) FROM memory_relations WHERE valid_to IS NOT NULL AND "
        "closed_by_fact_id IS NOT NULL").fetchone()[0]
    names = (assistant.lower(), "assistant", "the assistant")
    out["assistant_subject"] = con.execute(
        "SELECT COUNT(*) FROM memory_relations r JOIN memory_entities e ON e.id = "
        "r.subject_entity_id WHERE e.canonical_name IN (?, ?, ?)", names).fetchone()[0]
    out["facts_without_relation"] = con.execute(
        "SELECT COUNT(*) FROM memory_facts f WHERE f.superseded_by IS NULL AND NOT EXISTS "
        "(SELECT 1 FROM memory_relations r WHERE r.fact_id = f.id)").fetchone()[0]
    marks = ",".join("?" * len(ONE_AT_A_TIME))
    out["one_at_a_time_conflicts"] = con.execute(
        f"SELECT COUNT(*) FROM (SELECT subject_entity_id, relation FROM memory_relations WHERE "
        f"valid_to IS NULL AND relation IN ({marks}) GROUP BY subject_entity_id, relation HAVING "
        f"COUNT(*) > 1)", ONE_AT_A_TIME).fetchone()[0]
    out["preferences"] = con.execute("SELECT COUNT(*) FROM memory_preferences").fetchone()[0]
    tables = {r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type = 'table'")}
    record = {}
    if "memory_consolidation_batches" in tables:
        record = record_counts(con)
    con.close()
    return dict(out), record


def record_counts(con):
    """The consolidation record's counts (batches, ops, triage, outside by origin)."""
    rec = {"batches": Counter(), "ops": Counter(), "triage": Counter(),
           "outside_by_origin": defaultdict(Counter)}
    per_batch = []
    for b in con.execute("SELECT b.id, b.stop, b.path, b.outside, b.rounds, c.origin FROM "
                         "memory_consolidation_batches b LEFT JOIN conversations c ON c.id = "
                         "b.conversation_id"):
        rec["batches"][f"{b['path']}/{b['stop']}"] += 1
        origin = (b["origin"] or "?").split(":")[0]
        rec["outside_by_origin"][origin]["tainted" if tainted(b["outside"]) else "clean"] += 1
        rec["batches"][f"rounds={b['rounds']}"] += 1
        n = con.execute("SELECT COUNT(*) FROM memory_consolidation_ops WHERE batch_id = ?",
                        (b["id"],)).fetchone()[0]
        per_batch.append(n)
    for o in con.execute("SELECT op, status, reason FROM memory_consolidation_ops"):
        rec["ops"][f"{o['op']}/{o['status']}"] += 1
        rule = TRIAGE_REASONS.get(o["reason"] or "")
        if rule and o["status"] == "applied":
            rec["triage"][rule] += 1
    rec["ops_per_batch"] = (sum(per_batch) / len(per_batch)) if per_batch else 0.0
    rec["refused_replaces"] = rec["ops"].get("replaced/refused", 0)
    return rec


def usage_counts(path):
    """Tokens per batch by call kind (the consolidator's split into its extraction
    samples and its rounds) from a reextract usage file: {kind: (p50, p90, n)}."""
    per = defaultdict(lambda: defaultdict(lambda: [0, 0, 0]))
    with open(path) as f:
        for line in f:
            if line.strip():
                r = json.loads(line)
                t = per[r["kind"]][r["conversation"]]
                t[0] += r["in"]
                t[1] += r["out"]
                t[2] += r["calls"]
    out = {}
    for kind, convs in per.items():
        ins = [v[0] for v in convs.values()]
        outs = [v[1] for v in convs.values()]
        calls = [v[2] for v in convs.values()]
        out[kind] = {"in_p50": percentile(ins, .5), "in_p90": percentile(ins, .9),
                     "out_p50": percentile(outs, .5), "out_p90": percentile(outs, .9),
                     "calls_per_batch": sum(calls) / len(calls), "batches": len(convs)}
    return out


def run_store(a):
    for label in a.runs:
        db = data_path(label if label.endswith(".db") else f"reextract_{label}.db")
        counts, record = store_counts(db, a.assistant or "friday")
        print(f"== {label}")
        print("  " + "  ".join(f"{k} {v}" for k, v in sorted(counts.items())))
        if record:
            for key in ("batches", "ops", "triage"):
                print(f"  {key}: " + ", ".join(f"{k} {v}" for k, v in sorted(record[key].items())))
            print(f"  ops per batch {record['ops_per_batch']:.1f}, refused replaces "
                  f"{record['refused_replaces']}")
            print("  outside-text batches by origin: " + ", ".join(
                f"{o} {c['tainted']}/{c['tainted'] + c['clean']}"
                for o, c in sorted(record["outside_by_origin"].items())))
        usage = data_path(f"reextract_{label}_usage.jsonl")
        try:
            u = usage_counts(usage)
        except FileNotFoundError:
            u = {}
        for kind, v in sorted(u.items()):
            print(f"  tokens {kind}: in p50 {v['in_p50']:.0f} p90 {v['in_p90']:.0f}, out p50 "
                  f"{v['out_p50']:.0f} p90 {v['out_p90']:.0f}, {v['calls_per_batch']:.2f} "
                  f"calls/batch over {v['batches']}")


def fold(text):
    t = re.sub(r"\s+", " ", (text or "").lower()).strip()
    return t.rstrip(". ")


def load_forgotten(path):
    """Forgotten texts from a JSON list or a JSONL of {"text": ...}."""
    with open(path) as f:
        raw = f.read()
    try:
        items = json.loads(raw)
    except json.JSONDecodeError:
        items = [json.loads(line) for line in raw.splitlines() if line.strip()]
    if isinstance(items, dict):  # a JSONL of one line
        items = [items]
    return [x["text"] if isinstance(x, dict) else x for x in items if x]


def run_comebacks(a):
    """How many seeded forgotten facts each run's live facts state again."""
    forgotten = load_forgotten(data_path(a.forgotten))
    keys = models.load_keys(a.secrets)
    models.check_ready(a.model, keys, a.endpoint)
    a_facts = [{"id": i + 1, "text": t} for i, t in enumerate(forgotten)]
    folded = {fold(t) for t in forgotten}
    for label in a.runs:
        db = data_path(f"reextract_{label}.db")
        con = _open(db)
        live = [{"text": r[0]} for r in con.execute(
            "SELECT fact_text FROM memory_facts WHERE superseded_by IS NULL")]
        con.close()
        exact = sum(1 for f in live if fold(f["text"]) in folded)
        verdicts = Counter()
        for start in range(0, len(a_facts), 20):
            chunk = a_facts[start:start + 20]
            ids = set(range(1, len(chunk) + 1))
            try:
                text, _ = models.classify_batch(a.model, coverage.SYSTEM,
                                                coverage.judge_prompt(chunk, [], live), keys,
                                                a.endpoint)
                got = coverage.parse_verdicts(text, ids)
            except Exception as e:  # one failed call: those are unjudged
                print(f"  judge: {type(e).__name__}")
                got = {}
            for n in ids:
                verdicts[got.get(n, "unjudged")] += 1
        print(f"{label}: {len(forgotten)} forgotten, exact again {exact}, judged again "
              f"{verdicts['covered']} (partly {verdicts['partial']}), not {verdicts['missing']}, "
              f"unjudged {verdicts['unjudged']}")


WORLD_SYSTEM = """You sort facts a personal assistant extracted about its user. For each
numbered fact, say whether it is a WORLD fact: a claim about the outside world (a price,
a score, a schedule, a product, a company, the news, the weather) whose truth changes
over time and could be checked with a search, rather than something about the user, the
people and things in their life, or what they said or did.

Reply with JSON only: an array with one object per fact, in order:
[{"id": <number>, "world": true|false}]"""


def run_world(a):
    """How many of a run's extraction candidates are world facts."""
    keys = models.load_keys(a.secrets)
    models.check_ready(a.model, keys, a.endpoint)
    with open(data_path(a.candidates)) as f:
        facts = [json.loads(line)["text"] for line in f if line.strip()]
    world = judged = 0
    for start in range(0, len(facts), 25):
        chunk = facts[start:start + 25]
        prompt = "\n".join(f"[{n}] {t}" for n, t in enumerate(chunk, 1))
        try:
            text, _ = models.classify_batch(a.model, WORLD_SYSTEM, prompt, keys, a.endpoint)
            m = re.search(r"\[\s*\{.*\}\s*\]", text, re.S)
            items = json.loads(m.group(0)) if m else []
        except Exception as e:  # one failed call: those are unjudged
            print(f"  judge: {type(e).__name__}")
            items = []
        for it in items if isinstance(items, list) else []:
            if isinstance(it, dict) and isinstance(it.get("world"), bool):
                judged += 1
                world += it["world"]
    print(f"{a.candidates}: {len(facts)} candidates, {judged} judged, {world} world facts")
