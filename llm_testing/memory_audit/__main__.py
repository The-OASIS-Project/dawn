# CLI: python3 -m llm_testing.memory_audit sample|label|classify|compare|report|examples|reextract
#
# An audit of what memory extraction has stored for a user: which facts are worth
# keeping and which kinds of unwanted fact extraction produces.
#
#   sample    stratified sample (or --all) of a user's live facts -> JSONL
#   label     name each sampled fact's kind by hand; the reference for the models
#   classify  a model classifies a sample against the rubric
#   compare   a classification (or two) against the hand labels
#   report    counts by kind, month and use for a classification
#   examples  print facts of one kind (shows fact text: for the user, not for logs)
#   reextract run DAWN's extraction on chosen stored conversations into a scratch
#             database, so a prompt variant can be measured on the same input
#
# Privacy: everything read or written lives under llm_testing/memory_audit/data/
# (ignored by git).  Only `label` and `examples` print fact text; the others
# print counts.  The database is opened read-only.
#
# License: GPLv3, same as DAWN.

import argparse
import json
import os
import random
import re
import sqlite3
import sys
import time
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor

from . import models, reextract, rubric
from .storage import DATA, data_path, private_open, text_sha

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
DEFAULT_DB = "/var/lib/dawn/auth.db"
DEFAULT_SECRETS = os.path.join(ROOT, "secrets.toml")


_data = data_path


def _read_jsonl(path):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def _assistant_name():
    """ai_name from dawn.toml, so the prompt names the assistant as the user does."""
    try:
        for line in open(os.path.join(ROOT, "dawn.toml")):
            m = re.match(r'\s*ai_name\s*=\s*"([^"]+)"', line)
            if m:
                return m.group(1).capitalize()
    except OSError:
        pass
    return "the assistant"


# --------------------------------------------------------------------------- sample

def _live_facts(db, user_id):
    con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    con.row_factory = sqlite3.Row
    rows = con.execute(
        "SELECT id, fact_text, source, created_at, confidence, expires_at, "
        "access_count, last_cited, source_conversation_id FROM memory_facts "
        "WHERE user_id = ? AND superseded_by IS NULL ORDER BY id", (user_id,)).fetchall()
    con.close()
    out = []
    for r in rows:
        out.append({
            "id": r["id"], "text": r["fact_text"], "source": r["source"], "created_at": r["created_at"],
            "date": time.strftime("%Y-%m-%d", time.gmtime(r["created_at"])),
            "confidence": r["confidence"], "expires_at": r["expires_at"],
            "access_count": r["access_count"] or 0, "last_cited": r["last_cited"],
            "conversation": r["source_conversation_id"],
        })
    return out


def _stratum(f):
    return f["date"][:7]


def stratified(facts, n, seed):
    """n facts spread over months in proportion to their size,
    largest remainder first, at least one from each stratum while n allows."""
    groups = defaultdict(list)
    for f in facts:
        groups[_stratum(f)].append(f)
    rng = random.Random(seed)
    for g in groups.values():
        rng.shuffle(g)
    total = len(facts)
    if n >= total:
        return list(facts)
    shares = {k: n * len(v) / total for k, v in groups.items()}
    take = {k: int(s) for k, s in shares.items()}
    for k in sorted(groups, key=lambda k: shares[k] - take[k], reverse=True):
        if sum(take.values()) >= n:
            break
        take[k] += 1
    out = []
    for k, g in groups.items():
        out.extend(g[:take[k]])
    rng.shuffle(out)
    return out


def cmd_sample(a):
    facts = _live_facts(a.db, a.user_id)
    chosen = facts if a.all else stratified(facts, a.n, a.seed)
    path = _data(a.out)
    with private_open(path) as f:
        for x in chosen:
            f.write(json.dumps(x) + "\n")
    print(f"{len(chosen)} of {len(facts)} live facts (user {a.user_id}) -> {path}")


# --------------------------------------------------------------------------- label

def _load_labels(path):
    return json.load(open(path)) if os.path.exists(path) else {}


def _save_labels(path, labels):
    tmp = path + ".tmp"
    with private_open(tmp) as f:
        json.dump(labels, f, indent=1)
    os.replace(tmp, path)


def _kind_menu():
    lines = []
    for i, (k, _, d) in enumerate(rubric.KINDS, 1):
        lines.append(f" {i:2d} {k:17s} {d}")
    return "\n".join(lines)


def _ask(f, n_done, n_total, current):
    print("\033[2J\033[H", end="")
    print(f"[{n_done + 1}/{n_total}]  id {f['id']}  {f['date']}  "
          f"source={f['source']}  used={f['access_count']}  "
          f"cited={'yes' if f['last_cited'] else 'no'}"
          f"{'  expires' if f['expires_at'] else ''}")
    print()
    print("  " + f["text"])
    print()
    print(_kind_menu())
    if current:
        print(f"\n current: {current['kind']}  {current.get('note', '')}")
    print("\n number[, note]   b = back   s = skip   q = quit")
    return input("> ").strip()


def cmd_label(a):
    facts = _read_jsonl(_data(a.set))
    if not facts:
        raise SystemExit(f"no facts in {_data(a.set)}; run `sample` first")
    path = _data(a.labels)
    labels = _load_labels(path)
    if a.redo:
        order = [f for f in facts if f["id"] in a.redo]
    else:
        order = [f for f in facts if str(f["id"]) not in labels]
    i = 0
    history = []
    while 0 <= i < len(order):
        f = order[i]
        ans = _ask(f, len(labels) - (str(f["id"]) in labels), len(facts),
                   labels.get(str(f["id"])))
        if ans == "q":
            break
        if ans == "s":
            i += 1
            continue
        if ans == "b":
            if history:
                i = history.pop()
            continue
        num, _, note = ans.partition(",")
        if not num.strip().isdigit() or not 1 <= int(num) <= len(rubric.KINDS):
            continue
        kind = rubric.KIND_NAMES[int(num) - 1]
        labels[str(f["id"])] = {"kind": kind, "tier": rubric.TIER[kind],
                                "note": note.strip(), "rubric": rubric.RUBRIC_VERSION}
        _save_labels(path, labels)
        history.append(i)
        i += 1
    print(f"{len(labels)} labelled -> {path}")


# --------------------------------------------------------------------------- classify

def cmd_classify(a):
    keys = models.load_keys(a.secrets)
    models.check_ready(a.model, keys, a.endpoint)
    facts = _read_jsonl(_data(a.set))
    if not facts:
        raise SystemExit(f"no facts in {_data(a.set)}; run `sample` first")
    out_path = _data(a.out or f"class_{re.sub(r'[^A-Za-z0-9.-]+', '_', a.model)}"
                     f"_{os.path.splitext(os.path.basename(a.set))[0]}_r{rubric.RUBRIC_VERSION}"
                     f"{'_tier' if a.prompt == 'tier' else ''}.jsonl")
    # A row counts as done only for the same text: scratch databases reuse ids.
    done = {(r["id"], r.get("sha")) for r in _read_jsonl(out_path) if r.get("kind")}
    todo = [f for f in facts if (f["id"], text_sha(f["text"])) not in done]
    make = rubric.tier_system_prompt if a.prompt == "tier" else rubric.system_prompt
    system = make(a.assistant or _assistant_name(), a.today or time.strftime("%Y-%m-%d"))
    tiers = rubric.TIERS if a.prompt == "tier" else None
    batches = [todo[i:i + a.batch] for i in range(0, len(todo), a.batch)]
    print(f"{a.model}: {len(todo)} to classify ({len(done)} already done), "
          f"{len(batches)} batches -> {out_path}", file=sys.stderr)
    usage = Counter()
    failed = []
    t0 = time.time()

    def run(batch):
        ids = [f["id"] for f in batch]
        got, use = {}, Counter()
        try:
            text, u = models.classify_batch(a.model, system, rubric.batch_prompt(batch),
                                            keys, a.endpoint)
            use.update(u)
            got = models.parse_reply(text, ids, rubric.KIND_NAMES, tiers)
        except Exception as e:  # one bad batch must not lose the run
            print(f"  batch at id {ids[0]}: {type(e).__name__}: {e}", file=sys.stderr)
        # A fact the reply skipped or garbled gets one try on its own.
        for f in batch:
            if f["id"] in got:
                continue
            try:
                text, u = models.classify_batch(a.model, system, rubric.batch_prompt([f]),
                                                keys, a.endpoint)
                use.update(u)
                got.update(models.parse_reply(text, [f["id"]], rubric.KIND_NAMES, tiers))
            except Exception as e:
                print(f"  id {f['id']}: {type(e).__name__}: {e}", file=sys.stderr)
        return batch, got, use

    with private_open(out_path, "a") as out, ThreadPoolExecutor(a.workers) as pool:
        n = 0
        for batch, got, use in pool.map(run, batches):
            usage.update(use)
            for f in batch:
                if f["id"] in got:
                    kind, reason, tier = got[f["id"]]
                    out.write(json.dumps({"id": f["id"], "sha": text_sha(f["text"]),
                                          "kind": kind,
                                          "tier": tier or rubric.TIER[kind], "reason": reason,
                                          "model": a.model, "prompt": a.prompt,
                                          "rubric": rubric.RUBRIC_VERSION}) + "\n")
                else:
                    failed.append(f["id"])
            out.flush()
            n += len(batch)
            print(f"  {n}/{len(todo)}  {time.time() - t0:.0f}s", file=sys.stderr)
    print(f"done in {time.time() - t0:.0f}s; tokens in {usage['in']} out {usage['out']}; "
          f"{len(failed)} unclassified (re-run to retry)")


# --------------------------------------------------------------------------- compare

def kappa(pairs, credit=None):
    """Cohen's kappa over (a, b) label pairs.  With credit(a, b) -> 0..1 it is
    weighted kappa: partial agreement counts, in the observed and the chance
    term alike."""
    n = len(pairs)
    if n == 0:
        return float("nan")
    w = credit or (lambda x, y: 1.0 if x == y else 0.0)
    po = sum(w(x, y) for x, y in pairs) / n
    ca, cb = Counter(x for x, _ in pairs), Counter(y for _, y in pairs)
    pe = sum(ca[x] * cb[y] * w(x, y) for x in ca for y in cb) / (n * n)
    return 1.0 if pe == 1 else (po - pe) / (1 - pe)


def _by_id(path, facts=None):
    """Classified rows by id; kinds from an older rubric are left out.  With
    facts, only rows made for that fact's text (the last such row wins)."""
    out = {}
    for r in _read_jsonl(_data(path)):
        if r.get("kind") not in rubric.TIER:
            continue
        if facts is not None:
            f = facts.get(r["id"])
            if not f or (r.get("sha") and r["sha"] != text_sha(f["text"])):
                continue
        out[r["id"]] = r
    return out


def _tier(r):
    """The model's own tier from a tier-first run, else the kind's tier."""
    return r["tier"] if r.get("prompt") == "tier" else rubric.TIER.get(r["kind"], "?")


def _agreement(name, ref, other):
    ids = [i for i in ref if i in other]
    if not ids:
        print(f"{name}: no facts in common")
        return
    v = [(_tier(ref[i]), _tier(other[i])) for i in ids]
    k = [(ref[i]["kind"], other[i]["kind"]) for i in ids]
    agree_v = sum(x == y for x, y in v) / len(v)
    agree_k = sum(x == y for x, y in k) / len(k)
    print(f"{name}  (n={len(ids)})")
    print(f"  tier:      agree {agree_v:.0%}, kappa {kappa(v):.2f}")
    print(f"  kind:      agree {agree_k:.0%}, kappa {kappa(k):.2f}")
    part_k = sum(rubric.credit(x, y) for x, y in k) / len(k)
    print(f"  kind, similar = half: agree {part_k:.0%}, "
          f"kappa {kappa(k, rubric.credit):.2f}")
    tier_pairs = Counter(f"{x}->{y}" for x, y in v if x != y)
    if tier_pairs:
        print("  tier disagreements: " +
              ", ".join(f"{k} {c}" for k, c in tier_pairs.most_common()))
    pairs = Counter(f"{x}->{y}" for x, y in k if x != y)
    if pairs:
        print("  top kind disagreements: " +
              ", ".join(f"{p} {c}" for p, c in pairs.most_common(8)))


def cmd_compare(a):
    labels = {int(i): r for i, r in _load_labels(_data(a.labels)).items()}
    runs = {p: _by_id(p) for p in a.runs}
    for p, run in runs.items():
        _agreement(f"labels vs {os.path.basename(p)}", labels, run)
    if len(a.runs) == 2:
        x, y = a.runs
        _agreement(f"{os.path.basename(x)} vs {os.path.basename(y)}", runs[x], runs[y])


# --------------------------------------------------------------------------- report

def _pct(n, d):
    return f"{n / d:5.1%}" if d else "   - "


def cmd_report(a):
    facts = {f["id"]: f for f in _read_jsonl(_data(a.set))}
    cls = _by_id(a.run, facts)
    ids = [i for i in cls if i in facts]
    if not ids:
        raise SystemExit("no classified facts match the set")
    n = len(ids)
    by_kind = Counter(cls[i]["kind"] for i in ids)
    tiers = Counter(_tier(cls[i]) for i in ids)
    print(f"{n} facts: " + ", ".join(f"{t} {tiers[t]} ({tiers[t] / n:.0%})"
                                     for t in rubric.TIERS if tiers[t]) + "\n")
    print(f"{'kind':18s} {'tier':9s} {'n':>5s} {'share':>6s} {'used':>6s} {'cited':>6s} "
          f"{'expires':>8s}")
    for kind in rubric.KIND_NAMES:
        ks = [i for i in ids if cls[i]["kind"] == kind]
        if not ks:
            continue
        used = sum(1 for i in ks if facts[i]["access_count"] > 0)
        cited = sum(1 for i in ks if facts[i]["last_cited"])
        exp = sum(1 for i in ks if facts[i]["expires_at"])
        kt = {_tier(cls[i]) for i in ks}  # a tier-first run can differ from the kind's
        print(f"{kind:18s} {(kt.pop() if len(kt) == 1 else 'mixed'):9s} {len(ks):5d} "
              f"{_pct(len(ks), n):>6s} "
              f"{_pct(used, len(ks)):>6s} "
              f"{_pct(cited, len(ks)):>6s} {_pct(exp, len(ks)):>8s}")
    print("\ntier by month:")
    print(f"  {'':14s} {'n':>5s} " + " ".join(f"{t:>9s}" for t in rubric.TIERS))
    groups = defaultdict(Counter)
    for i in ids:
        groups[facts[i]["date"][:7]][_tier(cls[i])] += 1
    for g in sorted(groups):
        c, tot = groups[g], sum(groups[g].values())
        print(f"  {g:14s} {tot:5d} " + " ".join(f"{_pct(c[t], tot):>9s}" for t in rubric.TIERS))


def cmd_examples(a):
    facts = {f["id"]: f for f in _read_jsonl(_data(a.set))}
    cls = _by_id(a.run, facts)
    ids = [i for i in cls if cls[i]["kind"] == a.kind and i in facts]
    random.Random(a.seed).shuffle(ids)
    for i in ids[:a.n]:
        print(f"[{i}] {facts[i]['date']}: {facts[i]['text']}")
        print(f"      -> {cls[i]['reason']}")


# --------------------------------------------------------------------------- main

def main(argv=None):
    p = argparse.ArgumentParser(prog="python3 -m llm_testing.memory_audit")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("sample", help="stratified sample of a user's live facts")
    s.add_argument("--db", default=DEFAULT_DB)
    s.add_argument("--user-id", type=int, default=1)
    s.add_argument("--n", type=int, default=150)
    s.add_argument("--all", action="store_true", help="every live fact, not a sample")
    s.add_argument("--seed", type=int, default=42)
    s.add_argument("--out", default="label_set.jsonl")

    s = sub.add_parser("label", help="label a sample by hand")
    s.add_argument("--set", default="label_set.jsonl")
    s.add_argument("--labels", default=f"labels_r{rubric.RUBRIC_VERSION}.json")
    s.add_argument("--redo", type=int, nargs="*", help="relabel these fact ids")

    s = sub.add_parser("classify", help="classify a sample with a model")
    s.add_argument("--model", required=True, help="claude:<model> or local:<name>")
    s.add_argument("--set", default="label_set.jsonl")
    s.add_argument("--out", default="")
    s.add_argument("--batch", type=int, default=20)
    s.add_argument("--workers", type=int, default=4)
    s.add_argument("--endpoint", default="", help="local server, e.g. http://host:8080")
    s.add_argument("--secrets", default=DEFAULT_SECRETS)
    s.add_argument("--assistant", default="", help="assistant name (default: dawn.toml ai_name)")
    s.add_argument("--today", default="", help="date the prompt calls today (default: today)")
    s.add_argument("--prompt", choices=["kind", "tier"], default="kind",
                   help="kind: name the kind, tier follows; tier: decide the tier first")

    s = sub.add_parser("compare", help="classifications against the hand labels")
    s.add_argument("runs", nargs="+", help="one or two classification files")
    s.add_argument("--labels", default=f"labels_r{rubric.RUBRIC_VERSION}.json")

    s = sub.add_parser("report", help="counts by kind, month and use")
    s.add_argument("run")
    s.add_argument("--set", default="facts.jsonl")

    s = sub.add_parser("examples", help="print facts of one kind (shows fact text)")
    s.add_argument("run")
    s.add_argument("kind", choices=rubric.KIND_NAMES)
    s.add_argument("--set", default="facts.jsonl")
    s.add_argument("--n", type=int, default=15)
    s.add_argument("--seed", type=int, default=1)

    s = sub.add_parser("reextract", help="re-extract stored conversations into a scratch DB")
    s.add_argument("--db", default=DEFAULT_DB, help="source auth.db (opened read-only)")
    s.add_argument("--user-id", type=int, default=1)
    s.add_argument("--convs", default="reextract_convs.json",
                   help="conversation ids; picked and saved here on first use")
    s.add_argument("--n", type=int, default=40, help="conversations to pick")
    s.add_argument("--since", default="2026-09-01", help="pick from conversations since")
    s.add_argument("--min-messages", type=int, default=6)
    s.add_argument("--binary", default=os.path.join(ROOT, "build-ci", "tests", "bench_retrieval"))
    s.add_argument("--run-dir", default=ROOT,
                   help="directory with dawn.toml, secrets.toml, models.toml and models/")
    s.add_argument("--provider", default="claude")
    s.add_argument("--model", default="claude-haiku-5-5")
    s.add_argument("--effort", default="medium")
    s.add_argument("--label", default="", help="output name (default: prompt hash, model, effort)")
    s.add_argument("--config", default="",
                   help="dawn.toml for the bench (default: the run dir's); a variant's settings")

    a = p.parse_args(argv)
    if a.cmd == "reextract":
        reextract.run(a, _data)
        return
    {"sample": cmd_sample, "label": cmd_label, "classify": cmd_classify,
     "compare": cmd_compare, "report": cmd_report, "examples": cmd_examples}[a.cmd](a)


if __name__ == "__main__":
    main()
