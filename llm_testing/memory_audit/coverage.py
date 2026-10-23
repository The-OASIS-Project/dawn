# Memory audit: how much of one extraction another one kept.
#
# A judge model reads run A's facts (by tier, from A's classification) one
# conversation at a time, with run B's facts, and says for each A fact whether
# B covers it, covers part of it (a lost date, name or number counts as
# partial), or misses it.  B's facts from the same conversation come first, then
# the rest of B: extraction dedups across conversations, so a fact B already
# stored from an earlier conversation isn't stored again under this one.
#
# A prompt variant compared with the baseline this way separates lost facts
# from facts that are only worded differently.  Prints counts only.
#
# License: GPLv3, same as DAWN.

import json
import os
import re
import sys
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor

from . import models, rubric
from .storage import data_path, private_open, text_sha

VERDICTS = ["covered", "partial", "missing"]

SYSTEM = """You compare two sets of facts a personal assistant extracted about its user
from the same conversations, by two versions of its extraction prompt.

For each numbered fact in A, say whether set B carries the same information:
- covered: B states it, in any wording, in one fact or across several
- partial: B states only part of it, or a less specific version (a date, name,
  number or qualifier A has is missing from B)
- missing: B doesn't state it

Reply with JSON only, no other text: an array with one object per A fact, in order:
[{"id": <A number>, "v": "covered|partial|missing"}]"""


def judge_prompt(a_facts, b_same, b_other):
    lines = ["B, from the same conversation:"]
    lines += [f"- {f['text']}" for f in b_same] or ["(none)"]
    lines += ["", "B, from other conversations:"]
    lines += [f"- {f['text']}" for f in b_other] or ["(none)"]
    lines += ["", "A:"]
    lines += [f"[{n}] {f['text']}" for n, f in enumerate(a_facts, 1)]
    lines += ["", f"Reply with the JSON array only, one object for each of ids 1-{len(a_facts)}."]
    return "\n".join(lines)


def parse_verdicts(text, ids):
    """{id: verdict}; ids the reply skipped or garbled are left out."""
    m = re.search(r"\[\s*\{.*\}\s*\]", text, re.S)
    try:
        items = json.loads(m.group(0)) if m else []
    except json.JSONDecodeError:
        return {}
    out = {}
    for it in items if isinstance(items, list) else []:
        if not isinstance(it, dict):
            continue
        fid = it.get("id")
        if isinstance(fid, str) and fid.isdigit():
            fid = int(fid)
        if fid in ids and it.get("v") in VERDICTS:
            out[fid] = it["v"]
    return out


def _read(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def run(a, by_id, tier_of):
    """The coverage command.  by_id(class_path, facts) and tier_of(row) come from
    __main__ so the tier rules stay in one place."""
    keys = models.load_keys(a.secrets)
    models.check_ready(a.model, keys, a.endpoint)
    a_facts = {f["id"]: f for f in _read(data_path(a.a_set))}
    b_facts = _read(data_path(a.b_set))
    cls = by_id(a.a_class, a_facts)
    tiers = set(a.tiers.split(","))
    chosen = [f for i, f in a_facts.items() if i in cls and tier_of(cls[i]) in tiers]
    if not chosen:
        raise SystemExit("no classified A facts in those tiers")
    unclassified = sum(1 for i in a_facts if i not in cls)
    stem = [os.path.splitext(os.path.basename(p))[0] for p in (a.a_set, a.b_set)]
    out_path = data_path(a.out or f"coverage_{stem[0]}__{stem[1]}.jsonl")
    # Verdicts are cached per A fact and its text, against this B set, judge
    # model, its endpoint (a local judge is named by where it runs) and judge
    # instructions.
    b_sha = text_sha("\n".join([a.model, a.endpoint or "", text_sha(SYSTEM)] +
                               sorted(f["text"] for f in b_facts)))
    done = {}
    if os.path.exists(out_path):
        for r in _read(out_path):
            if r.get("b") == b_sha:
                done[(r["id"], r["sha"])] = r["v"]
    by_conv = defaultdict(list)
    for f in chosen:
        if (f["id"], text_sha(f["text"])) not in done:
            by_conv[f["conversation"]].append(f)
    usage = Counter()

    def judge(conv):
        facts = by_conv[conv]
        same = [f for f in b_facts if f["conversation"] == conv]
        other = [f for f in b_facts if f["conversation"] != conv]
        got, use = {}, Counter()
        for _ in range(2):  # one retry for facts the reply skipped
            todo = [f for f in facts if f["id"] not in got]
            if not todo:
                break
            try:
                text, u = models.classify_batch(a.model, SYSTEM, judge_prompt(todo, same, other),
                                                keys, a.endpoint)
                use.update(u)
                # The prompt numbers A's facts 1..n; map back to fact ids.
                v = parse_verdicts(text, set(range(1, len(todo) + 1)))
                got.update({todo[n - 1]["id"]: x for n, x in v.items()})
            except Exception as e:  # one bad conversation must not lose the run
                print(f"  conv {conv}: {type(e).__name__}", file=sys.stderr)
        return facts, got, use

    with private_open(out_path, "a") as out, ThreadPoolExecutor(a.workers) as pool:
        for facts, got, use in pool.map(judge, sorted(by_conv, key=lambda c: (c is None, c or 0))):
            usage.update(use)
            for f in facts:
                if f["id"] in got:
                    done[(f["id"], text_sha(f["text"]))] = got[f["id"]]
                    out.write(json.dumps({"id": f["id"], "sha": text_sha(f["text"]),
                                          "b": b_sha, "v": got[f["id"]]}) + "\n")

    per_conv = defaultdict(Counter)
    per_tier = defaultdict(Counter)
    missing_kind = Counter()
    for f in chosen:
        v = done.get((f["id"], text_sha(f["text"])), "unjudged")
        per_conv[f["conversation"]][v] += 1
        per_tier[tier_of(cls[f["id"]])][v] += 1
        if v != "covered":
            missing_kind[f"{cls[f['id']]['kind']} {v}"] += 1
    cols = VERDICTS + ["unjudged"]
    print(f"{'conv':>6s} " + " ".join(f"{c:>8s}" for c in cols))
    for conv in sorted(per_conv, key=lambda c: (c is None, c or 0)):
        print(f"{conv!s:>6s} " + " ".join(f"{per_conv[conv][c]:8d}" for c in cols))
    total = Counter()
    for t in sorted(per_tier):
        total.update(per_tier[t])
        n = sum(per_tier[t].values())
        print(f"{t:9s} n={n:4d}  " + "  ".join(f"{c} {per_tier[t][c]}" for c in cols
                                               if per_tier[t][c]))
    n = sum(total.values())
    # Loss is over the facts the judge answered for; unjudged ones (a failed
    # request, an unreadable reply) are neither lost nor kept.
    judged = n - total["unjudged"]
    if judged:
        loss = (f"lost {(total['missing'] + 0.5 * total['partial']) / judged:.1%} of {judged} "
                "judged, partial = half")
    else:
        loss = "nothing judged"
    if total["unjudged"]:
        loss += f"; {total['unjudged']} unjudged, so incomplete"
    print(f"{'total':9s} n={n:4d}  " + "  ".join(f"{c} {total[c]}" for c in cols if total[c]) +
          f"  ({loss})")
    if missing_kind:
        print("not covered by kind: " + ", ".join(f"{k} {c}" for k, c in missing_kind.most_common()))
    if unclassified:
        print(f"{unclassified} A facts unclassified (classify A first)")
    print(f"judge tokens in {usage['in']} out {usage['out']}")
