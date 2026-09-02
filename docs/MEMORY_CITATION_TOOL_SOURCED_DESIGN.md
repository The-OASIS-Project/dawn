# Memory Citation — Tool-Sourced Facts (Option B)

**Status:** design / unstarted. Working doc — do **not** commit until in-flight.
**Date:** 2026-08-23 · **Rev 2** (folds in master-plan-reviewer + architecture-reviewer)
**Related:** `MEMORY_CITATION_DESIGN.md` (Phase 1/2), `atlas/dawn/memory/`, TODO.md "Memory citation signal — Phase 2".

> **Rev-2 review verdict:** both reviewers say **build it** — the spine is solid and field-current, Approach
> B is sealed, the B1/B2 phasing is right. Two HIGH gaps (the `recall` tool marker; the empty-stash capture
> gate) were the doc's own §14 risk already realized in the plan; both are cheap on paper, expensive in data,
> and are now **B1 scope**. All findings are folded below and tagged `[plan]` / `[arch]`.

---

## 1. Problem

The citation signal only covers memory surfaced by the **focus-block injection** (the numbered
`[M1]…[Mk]` items in the volatile prompt block). It is **blind to memory the model pulls in via a
`recall`/`memory.search` tool call during the turn** — the *dominant* path for exactly the queries
where citation matters most.

Observed live (conv 1177, 2026-08-23):

```
user:      "Any word on colors I've set them to in the past?"
assistant: "Let me search your memory…"            → memory.search tool call
tool:      - [ID:5881] *User* adjusted office lights 5434K→5400K …
           - [ID:5939] Office Light #1 at 99% …
assistant: "You stick with cool daylight—5400K…"   → answer grounded in the TOOL results
cited:     <cited>M1</cited>  →  fact:10074  ("Creality 3D-printer collab email")
```

The facts the answer used (`5881`, `5939`) arrived as `[ID:xxxx]` from the tool and were never numbered
`[M#]`, so they are un-citeable. Told to "cite the `[M#]` you used," the model had no correct target and
reflexively cited `[M1]` — an unrelated email retrieval ranked top. The salience fix (2026-08-23) raised
compliance, making this **more** visible.

### Why it matters beyond a cosmetic wrong-highlight
Citation feeds **Phase-2 reinforcement** (cited fact → confidence bump → better ranking / slower decay).
Reinforcing off *today's* focus-only signal would, on every memory-search turn, **credit the wrong facts**
and **ignore the ones actually used** — so completing the signal is a near-**prerequisite** for Phase 2 to
help rather than mislearn.

**Not a bug in the capture/strip/mapping code** — that path is correct. This is a scope limitation of the
citation *design*.

`[plan]` **RMM fidelity:** the Phase-1 signal is adapted from RMM (arXiv 2503.08026), whose reward set is
*the turn's retrieved memories*. RMM has one retrieval channel; DAWN has two (focus + tool). Phase 1 covered
one, so Option B makes DAWN **more** RMM-faithful, not a departure.

---

## 2. Current architecture (grounded, verified against HEAD)

- **Focus stash** (`citation_stash_t`, `session_manager.h:264-286`): per-turn `entries[64]` of
  `{item_id[64], final_score}` + `count`, indexed by ordinal-1. Built in `build_focus_block.c` as items
  render `[M# source]`; published onto `session->citation_stash` under `history_mutex`; **cleared at
  dispatch entry** (`session_dispatch_user_turn`, `session_manager.c:2337`).
- **Capture** (`memory_citation.c` `memory_citation_capture`): runs inside `llm_response_finalize`
  (`:59`), **once per turn** on the final response (from `dawn.c:2662`, `session_manager_llm.c:310`).
  Snapshots the stash under `history_mutex`, parses every `<cited>…</cited>` for **ordinals**, validates
  `1 ≤ ord ≤ stash.count`, dedups (`seen[MAX_CITATION_STASH+1]`), writes a `memory_citation_audit` row
  (`injected_ids`, `cited_ids`, `injected_scores`, `dropped_count`) + broadcasts
  `webui_broadcast_context_citations(user_id, conv_id, msg_id, cited)`.
  - `[plan]` **Two gotchas the parser has today:** (a) it skips *all* non-digits, so `ID:6432` currently
    parses as **ordinal 6432** — the grammar extension is a real tokenizer rewrite, not a patch. (b) it
    **early-returns on `stash.count <= 0`** (~line 115) — see HIGH-2.
  - `[arch]` The single `cited` buffer feeds **both** the audit row and the Aurora broadcast — see the
    two-list requirement in §6.
- **Tool result rendering — TWO markers, TWO files** `[plan HIGH-1]`:
  - `memory_callback.c` renders `- [ID:%lld] <fact>` at the **retrieval** paths
    (`682, 694, 1052, 1360, 1444, 1576, 1588`) and a **creation confirmation** `Remembered: "…" (ID:x)`
    at `1039` (NOT retrieval).
  - `recall_format.c` (the **unified `recall` fronting tool** over memory+notes+docs+calendar) renders
    facts as `[memory id 6432]` — a *different* marker. It also flags focus-duplicates ("already in
    current context") **while still printing the id**, so cross-provenance dedup (§6.4) fires on real turns.
- **Concurrency** `[arch M3][plan]`: `session_get_command_context()` is **thread-local, set per worker
  task** (`llm_tools.c:269`); tool calls run on **parallel worker threads**, so the tool-surfaced set is a
  genuine **multi-writer** structure (unlike the single-writer focus stash) — `history_mutex` on every
  record is *required*, not defensive.
- **No double-credit** `[plan]`: verified there is **no** retrieval-time confidence bump on the
  recall/search path (the `+0.1` at `memory_callback.c:947/970` is remember-duplicate storage only), so
  the Phase-2 tie-in (§9) has no hidden interaction.

---

## 3. Goals / non-goals

**Goals**
1. Make a fact the model pulled in via a memory tool **citeable**, validated against what was **actually
   surfaced this turn** (not "any fact that exists").
2. Record tool-sourced citations in the audit with **provenance** so analysis separates the two
   populations and can measure grammar adoption + ID mis-copy.
3. **Log/audit-only first** (B1) — no reinforcement, no wire change — so the signal is measured before
   it's acted on.
4. Leave the focus-block `[M#]` path byte-for-byte unchanged.

**Non-goals (this design)**
- Phase-2 reinforcement itself (separate, gated on B1's numbers).
- Citing tool-sourced **summaries/entities** — tool results are facts today; carry a `kind` seam, don't
  build it.
- Retrieval-quality fixes (an unrelated email ranking `[M1]` is a separate issue).

---

## 4. Design decision — how the model cites a tool fact

### Approach A — extend `[M#]` into tool results (renumber)
Continue the ordinal sequence into tool output. **Rejected.**

### Approach B — cite the id the model already sees  ★ chosen (both reviewers sealed it)
Tool results carry a stable id; teach the model to cite it, e.g. `<cited>M1,ID:6432</cited>`. Capture
validates `M#` against the focus stash (unchanged) and `ID:6432` against a **per-turn tool-surfaced set**,
resolving both to canonical `fact:6432`.

**Why B wins** (the two un-obvious ones are decisive):
- `[arch]` A turns the **build-once/read-only** focus stash into a **mid-turn multi-writer** structure
  written by parallel tool workers, with cross-search renumbering — and `MAX_CITATION_STASH=64` is sized
  to the focus `top_k` ceiling, so tool results overflow it and mis-score real cites as hallucinations.
  B confines the new concurrent-write pattern to a *new* structure with no invariant to violate.
- `[plan]` **Failure-direction asymmetry:** a garbled **ID** almost always fails validation → **drops**
  (under-credits, safe). A garbled **ordinal** usually lands on *another valid ordinal* → **mis-credits a
  wrong fact** (poisons Phase 2 — the worst outcome). For a signal whose entire value is honesty, A's
  compact grammar buys convenience at the cost of the one property that matters.
- B matches current agentic-RAG practice (Anthropic search-result blocks, OpenAI file-search
  annotations, LlamaIndex citation engines all cite by stable in-context ids; renumbering is the
  known-fragile multi-iteration variant).

---

## 5. New state + population

### The per-turn tool-surfaced set (session-scoped, mirrors the focus stash)
```c
/* include/core/session_manager.h — near citation_stash_t */
#define MAX_TOOL_CITED_FACTS 96   /* multi-search × ~10; cap + drop-with-log past it */

typedef struct {
   int64_t fact_id;   /* the surfaced memory fact id (model cites the bare int) */
   int     kind;      /* 0=fact (only value today). [plan/arch] carry the seam now; populate facts-only. */
} tool_cited_entry_t;

typedef struct {
   tool_cited_entry_t entries[MAX_TOOL_CITED_FACTS];
   int count;
} tool_cited_set_t;    /* session->tool_cited_set, guarded by history_mutex */
```

- **Clear:** at **dispatch entry**, the *same* seam that clears `citation_stash`
  (`session_dispatch_user_turn`) — `[arch]` verified to cover **all** turn types incl. jobs/research.
- **Record helper lives in `memory_citation.c`, NOT `memory_callback.c`** `[arch M1/M2]`:
  `memory_callback.c` is **2,238 lines** (over the 1,500 soft limit); `memory_citation.c` is 204. Expose
  `void memory_citation_record_tool_fact(session_t *s, int64_t fact_id)` (dedup by id, cap-and-log,
  record **under `history_mutex`**, null-check `s`) in `memory_citation.c` — the producer half of state it
  already consumes. Call sites become one-liners.
- **Population rule (state it, don't list lines)** `[plan MED-3]`: **retrieval renders record; creation /
  confirmation renders do NOT.** Record at `memory_callback.c` `682, 694, 1052, 1360, 1444, 1576, 1588`
  **and** the `recall_format.c` fact path; **exclude** `memory_callback.c:1039` (`Remembered:` — recording
  it would let the model "cite" a fact it just stored and hand Phase 2 a same-turn self-cite bump).
- **Marker unification (HIGH-1 — do in B1, or you do this twice)** `[plan]`: `recall_format.c` prints
  `[memory id N]` while `memory_callback.c` prints `[ID:N]`. Two markers = teaching the model two grammars
  = the confusion B exists to avoid. **Unify to one marker via a shared macro** (`SURFACED_ID_FMT` beside
  `CITED_TAG_*` in `text_filter.h`, §7) used by both renderers, and record from both. This is the single
  biggest do-it-twice risk in the plan.

---

## 6. Capture changes (`memory_citation.c`)

1. **Gate fix (HIGH-2)** `[plan]`: the current `if (stash.count <= 0) return;` skips **tool-only turns** —
   the exact population B1 must measure. Change to *proceed when the focus stash **or** the tool set is
   non-empty*. One line; invisible if missed (rows just never exist).
2. **Grammar / real tokenizer** `[plan LOW]`: extend the digit-only scan to two token shapes — `M<digits>`
   / bare `<digits>` → focus ordinal (existing, back-compat kept), and `ID:<digits>` → tool fact id.
   Requires an actual tokenizer (today's "skip non-digits" would read `ID:6432` as ordinal 6432). Fuzz
   vectors: `M1,ID:6432`, `ID:6432,M1`, `id:6432`, `ID :6432`, `ID6432`, orphaned closer, mixed dedup.
3. **Validation:** `M#` → focus stash. `ID:n` → present in `tool_cited_set` → `fact:n`; else `dropped`.
   ID-token dedup uses a resolved-id set, **not** the ordinal `seen[]` array (id space is unbounded)
   `[plan LOW]`.
4. **Cross-provenance dedup** `[arch/plan]`: a fact both focus-injected and tool-surfaced, cited once, must
   dedup on the resolved `fact:x` (not the token). Fires on real turns (recall flags focus-dupes).
5. **Two cited lists — the B1 wire-clean requirement (HIGH, arch)**: build **`cited_all`** (focus + tool →
   the audit row) and **`cited_focus`** (focus only → `webui_broadcast_context_citations`). Broadcasting a
   tool `fact:x` — which has **no Context-panel row** — would silently change the reviewed Aurora frame.
   This fork is what makes "B1 changes no wire frame" true; state it as a hard requirement, not an impl
   detail.
6. **Audit schema (one migration)** `[arch M4 + plan MED-2]`: add a parallel **`tool_surfaced_ids`** column
   (the tool universe, stored **canonical `fact:x`** so summaries later slot in as `summary:x` with no
   schema change) **and** **`dropped_tool_count`** (so B1 can measure the ID mis-copy rate). Keep
   `cited_ids` a pure canonical-id list; **derive provenance by set membership** (`cited ∩ injected` =
   focus; `cited ∩ (tool_surfaced \ injected)` = tool). No positional-tag column. `memory_citation_audit`
   has no `dawn.toml` round-trip hazard — plain migration.
7. **Concurrent snapshot** `[arch M3]`: in capture, snapshot `citation_stash` **and** `tool_cited_set` in
   the **same** `history_mutex` critical section (shared lock, no ordering concern) so validation sees a
   consistent pair.

---

## 7. Prompt changes

- **Adjacency (MED-1 — apply the Phase-1 salience lesson)** `[plan]`: the `[ID:x]` items live in **tool
  results**, even further from `k_citation_footer` than the focus items were. Put the one-line cite hint in
  the **tool-result footer itself** — `recall_format.c` already has a footer ("When you answer from this,
  KEEP the specific facts…"); the `memory_callback` search/recent renders append one line. Adjacency is the
  cheapest compliance lever and we already paid to learn that.
- Focus-block reminder + `k_citation_footer` gain a short "you may also cite a surfaced fact by id"
  clause, but the **load-bearing** hint is the tool-result footer.
- **Single-source the grammar** `[plan]`: `SURFACED_ID_FMT` + a `CITED_TAG_ID_EXAMPLE` beside `CITED_TAG_*`
  in `text_filter.h`, referenced by render, prompt example, and parser — mirror-with-sync-comment
  discipline so they can't drift.
- **Kill-switch** `[plan LOW]`: the ID grammar, set population, and prompt clause **all gate on the
  existing `g_config.memory.citation_enabled`** — no new config key (avoids the 9-file path).

---

## 8. Aurora / `context_citations` impact

- **B1:** with the §6.5 two-list fork, `context_citations` stays **focus-only** — **no wire change, Aurora
  untouched**, tool-cited facts are audit-only.
- **B2 (later, coordinate with Aurora):** extend the frame with `tool` provenance so the client highlights
  the cited fact **in the tool transcript**, or a distinct `tool_citations` frame. The tool result text
  (`[ID:x]`) persists, so the client can anchor either way. Gate on coordination, exactly like the Phase-1
  gold-frame.
- The `<cited>` **strip** is unaffected — a tool-sourced `<cited>ID:x</cited>` is removed by the same
  `text_filter_cited_strip` (strip keys on `<cited>`/`</cited>`, grammar unchanged for it).

---

## 9. Reinforcement tie-in (Phase 2 — not built here)

Phase-2 reads a **cited fact regardless of provenance** and applies the confidence bump via the existing
decay/ranking machinery. B is what makes that bump land on the facts the model **actually used** on the
search path. No reinforcement code ships here. Tie-in verified clean (no retrieval-time double-credit, §2).

---

## 10. Phasing

- **B1 — complete the signal (log/audit only).** Tool-surfaced set + shared clear seam + record helper +
  population (incl. `recall_format.c`, HIGH-1) + capture gate fix (HIGH-2) + tokenizer + two-list fork +
  audit migration (`tool_surfaced_ids` + `dropped_tool_count`) + tool-result footer hint. **No wire change,
  no Aurora change, no reinforcement.** Measurable immediately via `citation_audit_summary.py` (add
  tool-cite precision + per-provenance drop split). **This is the whole "do it once" ask.**
  - **B1 exit criteria (write them now — Phase 2 is gated on these)** `[plan]`: tool-cite rate on
    memory-search turns ≥ target; hallucinated-ID drop rate ≤ target; spurious focus-cites on
    tool-answered turns materially down vs the conv-1177 baseline.
- **B2 — surface + reinforce (separate, coordinated).** `context_citations` tool provenance (Aurora
  coordination) and/or fold tool-cited facts into Phase-2 reinforcement.

---

## 11. Open decisions — RESOLVED by review

| # | Decision | Resolution |
|---|---|---|
| 1 | A vs B | **B**, sealed by both reviewers (§4). |
| 2 | Audit shape | **Parallel `tool_surfaced_ids`** (canonical `fact:x`) **+ `dropped_tool_count`, one migration**; provenance by set membership. `[arch M4 / plan MED-2]` |
| 3 | Kind seam | **Carry a `kind` field now**, populate facts-only; summary/entity is additive later. `[plan]` |
| 4 | Cap | **96 + drop-with-log**; cluster render is the overflow path to watch. |
| 5 | Bare-number back-compat | **Yes** — bare digits stay focus ordinals (what the shipped parser/model already do). |

---

## 12. Touch points

| File | Change |
|---|---|
| `include/core/session_manager.h` | `tool_cited_set_t` + `session->tool_cited_set` + `MAX_TOOL_CITED_FACTS` |
| dispatch clear seam (`session_dispatch_user_turn`) | clear `tool_cited_set` beside `citation_stash` |
| `include/core/text_filter.h` | `SURFACED_ID_FMT` + `CITED_TAG_ID_EXAMPLE` (single-source the marker/grammar) |
| `src/memory/memory_citation.c` | record helper (producer); gate fix; tokenizer; two validation sets; `cited_all`/`cited_focus` fork; cross-provenance dedup; audit migration binds |
| `src/memory/memory_callback.c` | unify marker to `SURFACED_ID_FMT`; one-line record calls at the 7 **retrieval** sites (exclude `:1039`) |
| `src/tools/recall_format.c` | unify fact marker to `SURFACED_ID_FMT`; record; add the tool-result footer cite hint |
| `memory_citation_audit` schema | `tool_surfaced_ids` (canonical CSV) + `dropped_tool_count` — one migration |
| `benchmarks/citation_audit_summary.py` | tool-cite precision, per-provenance split, ID-drop rate |
| tests | set populate/clear/cap; capture parses+validates `ID:` tokens (+ fuzz vectors); gate fires on tool-only turns; focus path byte-identical; `cited_focus` excludes tool ids |

---

## 13. Before-you-build checklist (from the reviews)

- [ ] Unify the fact marker across `memory_callback.c` + `recall_format.c` via `SURFACED_ID_FMT`; record
      from both (facts only). **(HIGH-1)**
- [ ] Capture gate: proceed when focus stash **or** tool set is non-empty. **(HIGH-2)**
- [ ] Two cited lists: `cited_all` → audit, `cited_focus` → broadcast. **(arch HIGH)**
- [ ] Record helper in `memory_citation.c`; one-liners at retrieval sites; exclude `:1039`; null-check
      session; record under `history_mutex`.
- [ ] Audit migration: `tool_surfaced_ids` (canonical `fact:x`) + `dropped_tool_count`, one bump.
- [ ] Tool-result footer cite hint (adjacency); grammar single-sourced; gate on `citation_enabled`.
- [ ] `tool_cited_entry_t` carries `kind` (facts-only populated).
- [ ] Real tokenizer + fuzz vectors; ID dedup separate from the ordinal `seen[]`.
- [ ] Snapshot both sets in one `history_mutex` critical section in capture.
- [ ] Write B1 exit criteria into §10.

---

## 14. Risks

- **Signal honesty depends on the surfaced-set being complete** — every **retrieval** render path (both
  files) must record, or a real citation looks hallucinated. Mitigate with the single record helper + the
  stated inclusion rule (retrieval records, creation doesn't) so the invariant is *checkable*, not vibes.
  This is a small instance of the "hand-carried invariant across N sites" debt already in TODO.md.
- **Grammar creep** — the `<cited>` tokenizer must stay tolerant and not let a malformed `ID:` corrupt
  adjacent focus ordinals. Fuzz it.
- **Cross-provenance double-count** if dedup is on the token instead of the resolved `fact:x`.
- **Aurora** — only B2 touches a wire frame; the §6.5 fork keeps B1 clean. Keep them separate so B1 can't
  regress the reviewed contract.

---

## 15. Live validation — preliminary spot-check (2026-08-24)

**Not thorough, not conclusive, single-query — a first smoke, logged so the trend has a starting point.**
One query, *"Tell me what you know about my Mark 47 build"*, run per model; all surfaced the same ~10 tool
facts, so it's a clean compliance comparison. The adjacent tool-result cite hint (added after the first
`memory.search` turn cited focus `[M#]` and ignored the tool facts) moved Haiku's `cited_tool` **0 → 5**.

Day-level after the fix (`citation_audit_summary.py --days 1`): citation rate **31%** (up from the ~13%
focus-only baseline), tool facts surfaced **82**, tool-cited **10**, tool-cite precision **12.2%**,
**`dropped_tool = 0`** (no mis-copied ids across every tool cite observed).

Per-model **citation counts** (identical query; 10 tool facts surfaced each except where noted):

| Model | cited_tool | cited_focus | dropped (focus) | dropped_tool | Read |
|---|---|---|---|---|---|
| gpt-5.6-sol | 9 | 0 | 0 | 0 | surgical — most tool cites, selective |
| gpt-5.6-luna | 8 | 0 | 0 | 0 | selective + spotless |
| gpt-5.4-mini | 10 | 0 | 0 | 0 | clean, but cited *every* tool fact |
| Sonnet 5 | 8 | 0 | 0 | 0 | surgical — but only on a forced-tool re-run; skipped the plain-query search first |
| Opus 5 | 4 | 9 | 0 | 0 | comprehensive cross-channel (13 total cites) |
| Haiku 4.5 | 5 | 2 | 0 | 0 | selective, clean |
| Opus 4.8 | 0 | 8 | 0 | 0 | **skipped the tool** — answered from focus injection |
| gemini-3.5-flash | 1 | 7 | 11 | 0 | cited, but hallucinated 11 `[M#]` ordinals |
| Local (Qwen3.6-27B Q4) | 0 | 0 | 0 | 0 | emitted no `<cited>` tag at all |

Count-level reads: the tool path works on every capable model; only the local Qwen doesn't engage the
grammar, and Opus 4.8 opted out by answering from the focus injection instead of searching. **`dropped_tool = 0`
across all eight models** — nobody has ever fabricated an `[ID:x]`. Design confirmation: **gemini hallucinated
11 `[M#]` ordinals but 0 `ID:` numbers** — the explicit `[ID:x]` marker is harder to fabricate than a bare
ordinal, so the tool path is structurally more robust to sloppy models than the focus-`[M#]` path (the
failure-direction asymmetry that sealed Approach B, §4).

#### Post-reorder re-verification (2026-08-25) — Responses prompt-cache reorder did NOT regress citation

Re-ran the same query (*"Tell me what you know about my Mark 47 build"*) on **gpt-5.6-luna** after the
OpenAI Responses prompt-cache reorder (volatile block moved out of `instructions` to a user item just
before the question — see `docs/RESPONSES_CACHE_REORDER_PLAN.md`). The concern was that repositioning the
`[M#]`-bearing block could corrupt the ordinal→item mapping and spike hallucinated ordinals. It did not:

| | cited_tool | cited_focus | dropped (focus) | dropped_tool |
|---|---|---|---|---|
| luna baseline (above) | 8 | 0 | 0 | 0 |
| luna **post-reorder** (audit row 130) | 7 | 0 | 0 | 0 |

Same shape: all-tool cites, zero focus, **zero drops of either kind** — "selective + spotless," identical
to baseline (7-vs-8 is single-query selectivity noise). Verified on the *same* turns that triggered the new
cross-turn cache (0 → ~93% cached), so citation quality was measured *with* the reorder active. The reorder
is request-side; capture/audit is response-side and untouched.

*UI aside:* the cite tags did not light up the WebUI — expected and pre-existing, not the reorder. This
turn cited via the tool path (`cited_focus=0`), and `webui_broadcast_context_citations` only carries focus
cites (early-returns on empty); more fundamentally the client has **no** `context_citations` renderer at
all (`www/` has no handler; `format.js` strips `<cited>`). Citation is Phase-1 audit-only today — a
user-facing cite-highlight (client renderer + extending the broadcast to tool cites) is unbuilt future work.

### 15.1 Answer-vs-cite deep read (5 models)

Read each model's actual answer against the facts it cited (the count table above can't distinguish genuine
selection from cite-everything). **Headline: cite *count* ≠ cite *quality*, and answer quality is decoupled
from citation compliance.**

| Model | Answer | Cite honesty | Notes |
|---|---|---|---|
| gpt-5.6-sol | A | **A** | Excellent answer; every one of its 9 cites is earned (incl. hard ones like `9949` carrier-board gate). Cleanest grounding. |
| gpt-5.6-luna | A- | A- | Clean, honest cites; a touch less *current* than the others. |
| Sonnet 5 | A | A- | Surgical tool run — 8 clean cites, no spurious; best temporal structure, and framed the stale MomoCon fact *correctly* (a past delay) where Opus 5 cited it as current. Needed a tool hint to search at all. |
| Opus 5 | **A (best answer)** | B- | Richest/most actionable answer (Aug 12-16 progression, phone #, "~25h to go/no-go"), but its 13 cites are the *least precise*: cited a **stale fact its own answer contradicts** (`6997` "has not resumed work"), and **used** but didn't cite `10256`/`10306`. |
| Haiku 4.5 | A- | B | Accurate cites but **under-cited** — used ~9 facts (sponsorship, full workflow), cited 5. |
| Qwen3.6-27B (local) | **A (answer)** | F | Top-tier *answer* — alone surfaced the panel specifics (Galleria 8, 2:30 PM), satellite prints, MIRAGE carrier-board gate, no hallucinations — but **zero `<cited>` tags**. |

Findings:
- **More cites is not better cites.** Opus 5 cited the most (13) and graded worst on precision (a spurious
  stale cite + two used-but-uncited facts); sol cited fewer (9) and every one was earned. A raw `cited_tool`
  count overstates a cite-a-lot model.
- **Under-citing is the common failure**, not over-citing: every citer omitted the paint-colors / sponsorship
  facts it clearly used.
- **The local model's gap is citation *compliance*, not comprehension.** Qwen's answer matched or beat the
  frontier models on completeness/accuracy; it simply won't emit the `<cited>` grammar. That's a
  prompt-engineering lever for small models, not a capability ceiling — encouraging for local-model use.
- **Triggering the tool is its own reliability axis.** 2 of 4 Claude models (Opus 4.8, Sonnet 5) *skipped*
  `memory.search` on the plain query and answered from the focus injection — no tool cites at all; every
  OpenAI model plus Haiku and Opus 5 searched unprompted. A model that won't fire the tool yields no tool
  citations regardless of how good it'd be at them.

**Price/quality takeaway (short-context pricing, ~1 query):** `cited_tool`-per-dollar ranks
**gpt-5.6-luna** first by a wide margin (A-/A cites at $1.20/MTok output). **Sonnet 5** ($10) matches luna's
cite quality at ~8× the cost, so luna dominates it on pure value — but Sonnet 5 is the **premium/Claude-tier
pick**: it beats **Opus 5** ($25) on *both* price and cite precision (surgical/clean vs comprehensive/stale),
retiring the count-ranking's illusion that Opus 5 was on top. Order within the paid tier: Sonnet 5 > sol >
Opus 5 on value.

Caveat: still one query, qualitative judgment against the stored fact set — a real precision/recall read
needs many queries and a rubric.
