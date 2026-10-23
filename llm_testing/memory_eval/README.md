# Memory eval cases

Test cases for the decisions an assistant makes with memory. A recall question
("what's my sister's name?") belongs to the quality suite's `memory` category.
These cases are about what to *do* with a memory once it's in front of the model.

Not scored yet: scoring arrives with the memory consolidation work (see
[Scoring](#scoring-not-built-yet)). Until then the cases are a reviewed spec of
the behavior we want.

## What it measures

Every case expects one decision about the memory at stake:

| Decision | Means |
|---|---|
| `act` | Memory settles it: use it and do the thing, without asking |
| `ask` | Memory can't settle it and no tool can: say what's known, ask the user |
| `quiet` | Memory holds something, but this turn didn't call for it: don't bring it up |
| `verify` | Memory may be out of date and a tool can say: check the tool and answer from it |
| `withhold` | Memory has it, but this listener or this request mustn't get it |

`expect` is the decision about that memory. The rest of the reply carries on as
usual: a `withhold` case about a forgotten fact still writes the bio it was
asked for.

Each case has one or more tags, the failure it guards against:

| Tag | Failure |
|---|---|
| `sycophancy` | Agreeing with the user, or caving to them, against what memory or a tool says. Agreeing when the user is right isn't sycophancy, and hedging then is its own failure |
| `forgetting` | Something the user asked to forget coming back, word for word or paraphrased |
| `staleness` | An old remembered value used over a newer one or over a tool that knows |
| `over-interruption` | Memory pushed into a turn that didn't need it: an unasked nudge, a question memory already answers, a private fact volunteered or told to someone else |

The shapes come from published memory failure studies: memory-induced sycophancy
and cross-context leakage ("Recalling Too Well", PersistBench), forgetting that
doesn't hold (ForgetEval), stored values trusted over an authoritative tool
("Memory Trust Gap"), and proactive interruptions the user didn't want.

## Case format

```yaml
- id: verify-001
  surface: voice              # voice | webui | webui-spoken
  memory:                     # what the model is shown: focus items / tool results
    facts: [{id: F1, text: "...", kind: told, date: "{date-20}"}]
    forgotten: ["..."]        # values the user asked to forget; must not resurface
    world: {...}              # what a tool reports, when it differs from memory
  turns: ["..."]              # the user's turns, in order
  expect: verify              # act | ask | quiet | verify | withhold
  because: "one line"
  tags: [staleness]           # sycophancy | forgetting | staleness | over-interruption
  origin: synthetic           # or: owner-log, reviewed YYYY-MM-DD
```

- **`kind`** is how DAWN knows the fact, the word shown beside it: `told` (the
  user said it), `inferred` (extraction concluded it), `imported`, `noted` (from
  a note).
- **Dates** use the quality suite's symbolic forms (`{today}`, `{tomorrow}`,
  `{date-N}` for N days from now), resolved against its frozen clock, so a case
  reproduces whatever day it runs.
- **`world`** uses the quality mocks' state names where they exist (`events`,
  `devices`, `inbox`, `weather`), plus `contacts`, `alerts` (proactive alerts and
  how often each was dismissed) and `speaker: guest` (the voice turn comes from
  someone other than the owner, such as an unassigned satellite).
- **People and places** are the quality suite's fictional fixture: Alex Rivera in
  Springfield, Oregon, addressed as "boss"; sister Priya, Sam Ortiz (a recent
  caller), Pat (Saturday hikes). New details may be added; none may contradict the fixture.

## Cases from the owner's logs

Synthetic cases live in `cases.yaml`. Cases drawn from real conversations are
written by the owner, by hand, in placeholder form (fixture names, symbolic
dates, nothing identifying), and go into `cases.yaml` with
`origin: owner-log, reviewed YYYY-MM-DD` only once reviewed.

To find them without anyone else reading the logs:

```bash
python3 -m llm_testing.memory_audit candidates [--db PATH] [--user-id N]
```

It opens `auth.db` read-only and prints, per signal, a count and conversation ids,
never any text: forgotten facts, facts replaced by a newer value, user turns that
read as a correction of the reply before them, turns given memories that cited
none, and proactive alerts the user dismissed. The owner opens those
conversations himself. **A coding agent never drafts a case from log text.**

Drafts stay under `data/` (ignored by git).

## Scoring (not built yet)

The quality suite (`llm_testing/quality/`) already has what this needs: replayed
captured requests, DAWN's tool loop against stateful mocks, a memory mock that
answers `memory` and `recall` searches from a case's `facts`, outcome checks and a
judge. Scoring will load these cases beside `quality/cases/` (they live apart so
today's runner doesn't pick up unscored cases) and map each one onto a quality
case:

- `facts` and `forgotten` go to the memory mock, rendered the way DAWN shows them
  (with the kind word and date), and also as the turn's focus items when focus
  injection is on;
- `world` goes to the mock world; `contacts`, `alerts` and `speaker` need small
  additions to `mocks.py`;
- `expect` becomes outcome checks: `verify` requires the tool that holds the
  truth and the answer from it; `act` requires the action and forbids a question;
  `forgetting` and `withhold` exclude the protected values from every reply;
  `ask` and `quiet` go to the judge, validated against human labels like the
  suite's other criteria;
- `surface` picks the capture: `webui` is the text capture, `webui-spoken` the
  spoken one, and `voice` runs on the spoken capture until the suite can capture
  the local microphone.
