# SAGE — Salience-Aware Gating Engine (Proactive Attention Layer)

**Status**: **PRE + P0 + WebUI Watches panel SHIPPED — PAUSED here (2026-07-10).** PRE
(2026-07-06) and P0 (2026-07-10, commit `805bc22`) are done and live-verified on real hardware;
the one deferred P0 surface, the **WebUI Watches panel**, is built + reviewed + live-verified
and committed (`ee84e14`). **SAGE work is now deliberately paused at the end of P0** — the
proactive layer is functional end-to-end (voice- and WebUI-managed watches → gate → real-time
spoken/banner alerts → `attention_log`); **P1 onward remain PROPOSED** and resume on the trigger
in §11 (P0 has run in the field, precision reviewed). Per the design-doc commit policy this file
is committable. Shipped commits: dawn `d495feb`/`805bc22`/`ee84e14`, mirage `4342147`, aura
`1463ecd`. See "PRE — as-built" (§11.1) and "P0 — as-built" (§11.2) for what was actually built
and the deviations.
**Date**: 2026-07-03 (design); PRE implemented 2026-07-06
**Provenance**: Deep-research session (Claude Fable 5): survey of proactive-LLM papers,
open-source, and commercial systems, followed by architecture synthesis. Claim verification
status is noted throughout — 2 claims adversarially verified (3-0 votes) in the original
harness run, and targeted spot-verification passes (2026-07-03) confirmed **all four
pillar sources** directly against the papers (R1 F1-score; R3 all numbers; R4 heuristics/
82%/dimensions; R6 windows/FTR/local-beats-frontier — one nuance at F5). Three of the four
pillars are peer-reviewed (ICLR 2025, CHI 2025, ACL 2026). A journal audit of the
early-terminated research harness recovered the dropped sources worth having (Hunches
architecture → R13, bounded deferral → R14). Remaining minor claims are direct extractions
from primary sources; 0 refuted anywhere.
**Working name**: SAGE is a placeholder; final OASIS component name TBD by the developer.
**Supersedes/absorbs**: the `docs/TODO.md` item "Proactive background observation (memory
Phase 2)" — that item framed proactivity as a memory feature; this design reframes it as an
ecosystem-level subsystem that the memory graph *feeds* (see §9.4). Update TODO.md when this
ships.

---

## 1. Problem Statement

DAWN is architecturally reactive: every pipeline (voice, WebUI, satellites, messaging) is
request→response. Nothing in the OASIS ecosystem has the job of watching the world and
deciding *whether, when, where, and how urgently* the assistant should speak or act without
being asked. This is the single largest gap between OASIS and the JARVIS/FRIDAY fiction
(session analysis, 2026-07-03): every reactive capability exists and works; every proactive
moment ("power at 15%, sir", "CO2 is climbing in your helmet", "you asked me to watch this —
it changed") is impossible.

**Goal**: ONE subsystem — an attention/salience layer — that ALL event sources plug into.
Not per-feature proactive hacks. Event sources include: STAT telemetry, AURA helmet sensors,
SPARK armor telemetry, ECHO cellular events, Home Assistant, scheduler, messaging channels,
memory-extraction outputs, calendar, and (future) vision/perception events.

**Non-goals**:
- Not a replacement for the scheduler (timed/recurring tasks stay in `scheduler.c`; the
  scheduler becomes an *event source* and a *delivery consumer*).
- Not autonomous action without authority: anything physical or dangerous routes through the
  existing tool-authority model (`TOOL_CAP_DANGEROUS`, two-step confirmation).
- Not a conversation feature: SAGE decides to *initiate*; the existing session/LLM machinery
  handles what happens after.
- **Not conversational interjection (v1)**: initiative *inside* a live dialogue the assistant
  is already party to (adding an unprompted relevant point mid-conversation — Inner Thoughts'
  [R4] native domain) is a distinct future mode. It shares the judge rubric but triggers on
  conversation turns instead of external events, and interacts with always-on voice mode.
  Deliberately out of scope for SAGE v1; revisit at P2+ when the judge exists.

**Relationship to the durable-background-jobs keystone** (Odysseus roadmap Tier 1, TODO.md):
complementary, not competing — watch-condition jobs *terminate in SAGE events* ("watch the
build; if it breaks, interrupt me"), and SAGE's DIGEST delivery reuses the same briefing
infra jobs deliver through. Neither blocks the other: PRE+P0 are small and can land before or
alongside the jobs work. TODO.md should state which program leads when both go active.

### 1.1 Position among the six JARVIS gaps (session analysis, 2026-07-03)

The gap analysis that produced this design identified six gaps between OASIS and the
JARVIS/FRIDAY fiction. SAGE is the unifying one — but it is the *socket*, not the whole
toolbox. Each other gap has a defined relationship to SAGE; three remain independent
subsystems that must be built on their own:

| # | Gap | Relationship to SAGE | Status |
|---|-----|----------------------|--------|
| 1 | **Suit-state ingestion** (DAWN blind to AURA sensors; SPARK absent; MIRAGE a load-bearing serial↔MQTT router) | **Absorbed** — SAGE's PRE phase (`suit_service.c` + §9.2 transport decision). Ships standalone value before any attention logic | **SHIPPED (PRE, 2026-07-06)** |
| 2 | **Continuous perception** (MIRAGE detection hard-disabled/deprecated; vision = snapshot-on-request only; zero home cameras) | **Bolt-on at the interface** — a future low-fps local VLM/detector loop's entire output contract is "emit `sage_event_t`s" (person entered view, vehicle approaching, text visible). The loop itself (camera pipeline, model on Orin, frame budget) is independent infrastructure with its own design pass | not designed |
| 3 | **Durable background jobs** ("research this and get back to me"; DAWN's LLM model is interrupt+rollback, the opposite) | **Peer trunk** — its own subsystem (registry, detached workers, reconnect-replay, cancel; see `project_durable_background_jobs_idea.md`). Composes at both ends: watch-jobs terminate in SAGE events; job completions deliver through SAGE policy tiers. SAGE = when to speak; jobs = how to work unattended | concept only |
| 4 | **Conversational fluidity** (no barge-in — DAP2 stub, AEC-blocked; no post-response hot-mic window; no speaker ID; flat TTS) | **Supplier** — voice-pipeline work SAGE consumes: INTERRUPT tier is degraded (queue-front + chime) until TTS preemption exists (§13 Q5); audience gating sharpens with speaker ID. All three wanted for the reactive experience regardless. Hot-mic window is cheap (the SMS `active_window_sec` pattern applied to voice); barge-in is the hard one | on TODO (speaker ID, barge-in §3) |
| 5 | **Presence/room model** (which human is in which room; paused `satellite_mappings` work, 2026-04-21) | **Supplier** — SAGE created its 2nd and 3rd consumers: warmest-surface routing (§8) and privacy/audience gating (deliberately conservative until presence exists). Phone-ring routing (TODO) was the 1st. Three customers now strengthen the unpause case | paused |
| 6 | **Attention/salience** | **This document** | designed |

Net: nothing on the JARVIS list requires a second decision-making layer — every gap
either lives inside SAGE, supplies it, or composes with it at a named interface. See §15
for the dependency-ordered roadmap across all six.

---

## 2. Research Survey — What the Field Converged On

Two independent research traditions converged on the same architecture:

**LLM proactive-agent papers (2024–2026)** attack "should I speak/act now?" as a learnable
decision, benchmarked and rewarded. **Pre-LLM HCI/ubicomp interruptibility research
(2005–2018)** attacks "when/where may I interrupt?" with deployed, validated systems. SAGE
composes both: the LLM tradition supplies the judge; the HCI tradition supplies the
anti-noise policy.

### 2.1 Key findings (each with source; see §12 for full references)

**F1 — The dual-process funnel is universal.** Every strong system is a variant of:
many events → cheap always-on filter → expensive judge on survivors → rate-limited,
context-routed delivery. The 2018 attention-management survey [R7] formalizes it as
*sensing → processing → inferring → modeling → managing*; the LLM papers rediscover it as
classifier-gated generation [R3].

**F2 — The gate must be small and trained; prompting a big LLM "should I speak?" is the
worst option.** [R3, VERIFIED against paper Table 1, 2026-07-03]: RoBERTa-base classifier
gating a Llama-3-8B generator = 5.90 ms / 0.47 GB per decision vs 30.12 ms / 15.47 GB
end-to-end (RTX 3090), at 93.18% vs 96.59% interruption accuracy — while **zero-shot
Llama-3-8B scores only 81.72%**. Caveats from the paper itself: evaluated on their own
synthetic (Yahoo-Answers-derived) test set with proxy metrics; NEC Labs preprint (May
2026), not yet peer-reviewed. Corroborated by [R5]: frontier LLMs (DeepSeek-V3.2 best)
reach only 64.4% accuracy / 71.3% F1 zero-shot on when-to-assist. Implication:
deterministic rules first, distilled classifier later, LLM judge only *behind* the gate.

**F3 — "Inner Thoughts" is the best conceptual frame for the judge.** [R4, **CHI 2025**,
VERIFIED against paper 2026-07-03]: the agent continuously forms covert candidate
*thoughts* on triggers (`on_new_message`, `on_pause` at 10 s), scores each 1–5 via an LLM
chain-of-thought evaluator against **eight heuristics — Relevance, Information Gap,
Expected Impact, Urgency, Coherence, Originality, Balance, Dynamics** (exact list
verified) — and speaks only when a thought's score crosses a configurable threshold.
Human raters preferred it 82% of the time over a next-speaker-prediction baseline, with
significant wins on all seven dimensions (turn appropriateness, coherence,
anthropomorphism, intelligence, engagement, initiative, adaptability; all p<0.05).
Proactivity as *suppressed speech*, not *predicted turns*.

**F4 — When-to-help is learnable from accept/reject labels.** [R1, VERIFIED 3-0]:
ProactiveBench pipeline — real-world activity events → proactive task predictions → human
accept/reject labels → **reward model that simulates human judgment**. 6,790-event
benchmark [VERIFIED 3-0]. Their fine-tuned model hit F1 66.47% on the when-decision,
beating all open- and closed-source models tested [unverified]. Implication: SAGE's
feedback log (§8) is simultaneously the anti-noise lever and a private training set.

**F5 — Opportunity WINDOWS, not instants; false positives as a first-class metric.** [R6]
(ProActor, **ACL 2026**, VERIFIED against paper Eq. 4/6 + Table 1, 2026-07-03): reference
actions are time *windows* (reference range ℛ, Eq. 4); a **Fault Trigger Rate (FTR)**
metric (Eq. 6) explicitly penalizes proactive actions fired outside valid windows; the
stage-aware reward deliberately shifts to a conservative phase late in training. Also
verified: **a 4-bit quantized LoRA-tuned Qwen2.5-14B achieved the best proactive-timing
scores on BOTH test datasets** (PT 0.2347/0.0846 vs strongest baselines' ≤0.2023/0.0811)
against GPT-5.1, Gemini-2.5-flash, and Claude-Sonnet-4, with comparable action
consistency — and ranked #1 on the composite PRI on ABCD+ (with-action-observations).
**Nuance**: on the Home Loan dataset (no action observations) frontier models still led
the composite ranking — local wins on *timing* specifically, not across the board. The
judge does not need the cloud for the when-decision. Implication: every SAGE event carries
a TTL; stale candidates die unspoken; FTR is a tracked metric from day one.

**F6 — False-positive interjections are THE failure mode.** [R5]: low precision causes
alert fatigue → users ignore or disable the feature entirely. Design priority is
precision-first, recall second. A missed interjection costs little (the user can still
ask); a wrong interjection spends trust.

**F7 — The user's own reaction history beats context sensing 5×.** [R8] (Pielot, UbiComp
2017, real-device deployment): an XGBoost classifier on 197 passively-sensed phone features
predicted engagement at precision 0.07 vs baseline 0.043; adding just **4 past-behavior
features** (per-user mean accept rate, last response, fast+slow exponential rolling means
of past engagement) took precision to 0.218 — **5× baseline** — F1 0.113→0.311. Also [R8]:
**interruptibility ≠ engagement** — "can I get attention" and "will they act on it" are
different predictions; a proactive system must predict engagement. Implication: the
feedback log (§8) is the highest-leverage component in the whole design.

**F8 — Defer-to-breakpoint is the validated timing strategy.** [R7]: interrupting at task
boundaries/breakpoints has both theory (memory-for-problem-state, threaded cognition) and
field deployment (Attelia II) behind it. The survey's delivery taxonomy: **mediating**
(defer to breakpoint), **mitigating** (choose a less intrusive device/modality),
**indicating** (show status). Cascading classification (cheap classifiers feed
higher-abstraction ones) is the standard staged inference chain [R7]; the pre-LLM
interruptibility pipeline convention is scenario→context-collection→predictive-model [R9].

**F9 — Proactivity decomposes into planning + guidance.** [R2] (ProactiveEval): *target
planning* (what proactive goal) vs *dialogue guidance* (how to steer). 328 environments,
22 LLMs evaluated; no single model dominated both; reasoning/CoT capability measurably
influences proactive quality [unverified]. Implication: keep judge scoring (planning) and
utterance generation (guidance) as separable steps — they may want different models/efforts.

**F10 — Nobody has shipped the unified version.** Confirmed by a targeted survey pass
(2026-07-03), now with named evidence:
- **LangChain "ambient agents"** [R10] is the closest open-source concept: event-stream or
  scheduled LangGraph agents with human-in-the-loop **notify / question / review**
  checkpoints and an `interrupt` primitive. But HITL triggers on *task ambiguity*, not on
  modeled user attention — there is no salience scoring, no interruption-timing model, no
  anti-noise learning. It's the durable-jobs half, not the attention half.
- **OVOS + Home Assistant** (open-source voice): the proactive path is "HA automation →
  push TTS announcement to the speaker" — rule-triggered delivery with no judgment layer
  at all. Delivery exists everywhere; deciding-whether-to-speak exists nowhere.
- **Alexa Hunches** (shipped, most mature commercial — architecture IS published,
  amazon.science "The science behind Hunches"): encoder-decoder **deep device embeddings**
  (device history + home configuration) feed a neural device-state predictor; embeddings
  k-means-cluster into ~60 usage archetypes for generalization; and — the key detail — a
  **separate acceptance model predicts whether a given Hunch will be accepted** before it
  fires. That's the ProactiveBench reward-model pattern [F4] and the reaction-history
  lever [F7] running in production at scale. Scope is still a single domain (smart-home
  device state). Validates three SAGE choices: presence/activity states as anchor
  context, the opt-in auto-execute tier (§8 tier 1), and acceptance-prediction as the
  P3 learned-gate objective.
- **ChatGPT Pulse** (shipped Sept 2025): once-daily overnight research → 5–10 scannable
  cards from memory/chat-history/feedback + optional Gmail/Calendar. Pure DIGEST tier at
  fixed cadence — no event-driven interruption — but its per-card thumbs feedback is the
  Pielot reaction-history lever productized; SAGE's digest items should carry the same
  per-item rating affordance.
Every shipped system implements exactly ONE of SAGE's delivery tiers in one domain
(Hunches = auto-act, Pulse = digest, HA/OVOS = rule-announce). A unified, cross-domain,
event-driven, local-first attention layer over an embodied assistant **does not exist in
the open**. OASIS extends the state of the art here (§10).

---

## 3. Design Principles (each derived from evidence)

P1. **One funnel, all sources.** Every proactive impulse in OASIS flows through SAGE. No
    other module gets its own "decide to speak" logic. (F1, F10)
P2. **Precision over recall, always.** Ship conservative; loosen with evidence. Every phase
    gate below has an FTR/precision criterion. (F5, F6)
P3. **Cheap before expensive.** Deterministic C rules → (later) distilled classifier → LLM
    judge only on gate survivors. Target: judge invocations ≤ a few dozen/day. (F2, F8)
P4. **Windows, not instants.** Every event has a TTL; every candidate expires. (F5)
P5. **Separate WHAT from WHEN/WHERE.** Judge scores content; policy owns timing, surface,
    and budget. (F8, F9)
P6. **The feedback log is a first-class product.** Log every interjection + outcome from
    P0 onward, even before anything learns from it. (F4, F7)
P7. **Local-capable by design.** Judge runs through `llm_interface` so it can be cloud
    (Haiku-class) or local (Qwen-class); nothing assumes cloud. (F5/R6)
P8. **Authority unchanged.** SAGE may *propose* actions; execution goes through the
    existing command executor + capability flags + confirmation flows.

---

## 4. Architecture Overview

```
 STAT ─┐                                         ┌→ INTERRUPT   (speak now, warmest surface)
 AURA ─┤   ┌─────────┐   ┌──────────┐   ┌─────┐  ├→ BREAKPOINT  (speak at next idle boundary)
 ECHO ─┤   │ INGEST  │   │   GATE   │   │JUDGE│  ├→ AMBIENT     (HUD badge / WebUI card, no voice)
 HA ───┼──→│normalize│──→│ rules +  │──→│ LLM │→─┤→ DIGEST      (fold into next briefing)
 sched─┤   │ to event│   │ novelty/ │   │8-heu│  ├→ DROP        (logged, not delivered)
 msgs ─┤   │ schema  │   │hysteresis│   │ CoT │  └───────┬──────
 mem ──┤   └─────────┘   │(→learned)│   └──▲──┘          │
 vision┘                 └──────────┘      │        ┌────▼─────┐
 (future)                                  │        │ FEEDBACK │  accept/dismiss/ignore per
                            context pack:  │        │   LOG    │  category → retunes GATE
                            memory graph,──┘        └──────────┘  thresholds & JUDGE prompt
                            presence, calendar,                   (Pielot 4-feature loop, F7)
                            conversation recency, DND
```

Five stages, mapping 1:1 onto the [R7] pipeline (sensing→processing→inferring→modeling→
managing). Stages are independent modules behind one internal header (project split
pattern, ARCHITECTURE.md §File Organization).

---

## 5. Event Schema (Ingest)

One canonical struct; all sources normalize into it. Sketch (final field sizes per
CODING_STYLE_GUIDE; static allocation, fixed-size strings):

```c
typedef struct {
   char     event_key[64];      /* stable dedup key, e.g. "stat.battery.low"          */
   char     source[16];         /* "stat" | "aura" | "echo" | "ha" | "sched" | ...    */
   char     type[32];           /* source-scoped type, e.g. "threshold_cross"         */
   char     summary[256];       /* one-line human-readable event description          */
   char     entities[3][64];    /* memory-graph entity names touched, if any          */
   double   magnitude;          /* source-normalized severity 0.0–1.0                 */
   int64_t  observed_at;        /* epoch ms                                           */
   int64_t  ttl_ms;             /* opportunity window (P4); expired ⇒ auto-DROP       */
   char     raw_json[1024];     /* original payload for the judge's context           */
} sage_event_t;
```

**Source → event mapping (initial set):**

| Source | Transport (already exists?) | Example events |
|---|---|---|
| STAT | `stat/telemetry` MQTT — **exists** (`src/core/stat_service.c`) | battery SoC crossing, discharge-rate anomaly, thermal threshold, fan fault, BMS fault |
| AURA | `helmet/telemetry` MQTT — **EXISTS (PRE shipped 2026-07-06)**: MIRAGE `suit_telemetry.c` republishes the parsed Enviro/Motion/GPS; DAWN `suit_service.c` ingests | CO2 slope, air-quality band change, heat index, GPS geofence enter/leave |
| SPARK | `armor/telemetry` MQTT — **EXISTS (PRE shipped)**: per-piece temp/voltage republished (SPARK reaches MIRAGE via AURA's ESP-NOW→serial relay); DAWN keeps a bounded armor roster | armor-piece voltage low, temp high, telemetry timeout (piece offline) |
| ECHO | `echo/events` — **exists** (`src/tools/phone_service.c`) | missed call, SMS from VIP contact, modem lost, signal degraded |
| Home Assistant | REST polling / webhooks (`homeassistant_service.c`). The `state_changed` WS subscription that would feed this is also **signal-map #3** — it doubles as the real-time push for the shipped interactive HA board (see `PROACTIVE_ALERTS_SCOPE.md` §3 + `DAWN_UI_SIGNAL_MAP.md §9.4`) | entity state changes matching watch rules (door open, device offline) |
| Scheduler | in-process (`src/core/scheduler.c`) | overdue task, upcoming event T-15min (calendar via CalDAV cache) |
| Messaging | in-process (`messaging_engine_inbound.c`) | unanswered inbound past threshold on a channel the user isn't watching |
| Memory | extraction pipeline (`memory_extraction.c`) | new fact contradicting an active plan; commitment detected ("I need to X by Friday") |
| Component status | `*/status` LWT topics (`component_status.c`) | MIRAGE/STAT/ECHO went offline/online |
| Vision (future) | perception loop (not built) | person/vehicle detected, text visible |

Ingest is a thin adapter per source (~30–80 LOC each) publishing into a single
mutex-protected ring buffer. Per lock-ordering rules the SAGE queue mutex is a **leaf
lock** (copy out, release, process).

---

## 6. Gate (cheap, always-on filter)

**v1 (P0): deterministic C rules, config-driven.** Rule types:
- **Threshold/band**: fire when metric crosses a band edge (with hysteresis — re-arm only
  after re-entering the band by a margin, so a noisy signal can't machine-gun events).
- **Slope**: fire on rate-of-change over a window (e.g. CO2 ppm/min).
- **Match**: fire on event pattern (source+type+entity) — e.g. SMS from contact tagged VIP.
- **Absence**: fire when an expected event does NOT arrive (armor telemetry timeout).

**Novelty suppression** (per `event_key`): exponential backoff — after an event fires,
identical keys are suppressed for `backoff_initial` and the window doubles per repeat up
to `backoff_max`, resetting after quiet period. Prevents "battery at 15%… battery at 15%…".

**Output**: a *candidate* = event + rule that fired + suggested urgency floor/ceiling.
Rules can mark a candidate `bypass_judge` (P0 mode, or trivially-urgent classes like
modem-lost) or `judge_required`.

**v2 (P3): learned gate.** Distill a classifier from the feedback log once it has enough
labels (F2, F4). Design the gate behind a function-pointer interface so the learned
version slots in without touching neighbors. Target: ONNX runtime already linked (Piper) —
a tiny classifier (RoBERTa-class or smaller, even logistic regression on engineered
features) runs in the existing `embedding_engine`/ONNX infrastructure.

**Config sketch** (`dawn.toml`):

```toml
[attention]
enabled = false                  # master switch, default OFF
mode = "digest"                  # "digest" (P0) | "full" (P1+)
max_interjections_per_hour = 4   # global budget
quiet_hours = "23:00-07:00"      # DIGEST-only during quiet hours (except INTERRUPT class)
judge_enabled = false            # P2+
judge_threshold = 3.5            # Inner-Thoughts-style 1–5 score floor

[[attention.rule]]
name = "battery_low"
source = "stat"
metric = "battery.soc"
below = 20
hysteresis = 5
urgency_floor = "breakpoint"
privacy = "public"               # public | personal | private (see §8 audience gating)
ttl_min = 30
```

Every setting surfaces in the WebUI `SETTINGS_SCHEMA` per the configuration architecture.

---

## 7. Judge (LLM salience scoring) — P2

For each surviving candidate, build a *thought* and score it. Modeled on Inner Thoughts
[R4] with the rubric adapted to an embodied assistant:

**Context pack assembled per invocation** (all existing infrastructure):
- Current presence/activity: which sessions are active, TTS state, voice state machine
  state, last-interaction timestamps (session manager).
- Conversation recency: is this topic already live in an active conversation?
- **Information gap** (OASIS-unique, F3+memory graph): query memory for whether the user
  already knows / recently discussed this. `memory_db` + focus-injection retrieval already
  provide the machinery.
- Calendar next-N-hours (CalDAV cache).
- Feedback stats for this category (§8): acceptance rate, last reaction.
- The event's `raw_json` + rule that fired.

**Scoring rubric** (CoT, one call): relevance to current context; information gap; urgency;
expected impact (what can the user *do* about it now?); novelty vs feedback log;
appropriateness (quiet hours, guests present, driving). Output JSON:
`{score: 1–5, urgency: interrupt|breakpoint|ambient|digest|drop, surface_hint, utterance}`.
Below `judge_threshold` ⇒ demote to DIGEST or DROP (never silently discard — log it).

**Model routing**: through `llm_interface` with a dedicated purpose slot (same pattern as
extraction's `memory_extraction_resolve_config()` — reuse that shape). Default: Haiku-class
cloud at low effort; design validated for a local Qwen-class judge (F5). Keep score-then-
generate separable (F9) — P2 can generate the utterance in the same call; if quality
demands, split later.

**Cost control**: judge only sees gate survivors (target ≤ dozens/day); batch near-
simultaneous candidates into one call; `bypass_judge` classes skip it entirely.

**Fixture bench — seed from ProactiveBench (checked 2026-07-03).** The R1 artifacts are
public: dataset (136 instances / 6,790 events across coding/writing/daily-life scenarios,
collected via ActivityWatch desktop traces) + trained reward model, Apache-2.0, at
github.com/thunlp/ProactiveAgent and huggingface.co/YancyLee/ProactiveAgent. Uses for P2:
(a) copy their event→prediction→accept/reject JSON shapes for our fixture format;
(b) run our judge prompt against a sample of their labeled events as a sanity bench
(does our rubric's speak/stay-quiet correlate with their human labels?); (c) their
reward-model training recipe is the template for P3's learned gate on our own logs.
**Caveat — domain mismatch**: their events are desktop-activity traces, not home/suit
telemetry; treat as calibration-shape reference, never as gold for SAGE's domains, and
bench their reward model before trusting any transfer (DAWN bench-honesty discipline).
Apache-2.0 borrowing follows the established mem0 pattern: per-file adaptation comments +
NOTICE entry (CLAUDE.md §Third-party attribution).

---

## 8. Policy + Feedback (delivery and learning)

**Urgency classes → delivery behavior:**

| Class | Behavior | Reuses |
|---|---|---|
| INTERRUPT | Speak now on warmest surface; may preempt TTS (once barge-in infra exists; until then, queue-front) | `session_broadcast_system_message()` pattern (Layer 1, shipped for phone ring); TTS queue |
| BREAKPOINT | Hold until breakpoint (voice state == SILENCE, TTS idle, no in-flight LLM turn, ≥N s since last user interaction) — but **bounded deferral** [R14]: each urgency band has a max-deferral time; when it elapses, deliver anyway even mid-activity. Bound interacts with the event TTL: if TTL < max-deferral remaining, demote to AMBIENT/DIGEST instead of interrupting | state machine observation; scheduler-style pending queue |
| AMBIENT | No voice. HUD badge (MQTT `hud` notification path), WebUI card (WS broadcast weak-symbol pattern) | `webui_broadcast_*` weak symbols; `hud_tools.c` / notification path |
| DIGEST | Fold into next scheduled briefing | scheduler briefing delivery incl. `deliver_to` messaging channels |
| DROP | Log only | — |

**Surface routing (mitigating, F8)**: warmest-surface heuristic v1 — HUD online && recent
suit activity ⇒ HUD+voice; else most-recently-active session's surface; else DIGEST.
Full presence-based routing depends on the paused `satellite_mappings` work
(memory: `project_local_speaker_satellite.md`) — design accommodates it, doesn't block on it.

**Budgets**: global/hour cap, per-category/day cap, quiet hours (INTERRUPT exempt),
minimum spacing between voice interjections.

**Privacy / audience gating.** A wrong-audience interjection ("your doctor called about
the biopsy results" spoken with guests present) destroys more trust than any false
positive. Every rule/category carries a `privacy` class:
- `public` — any surface, any audience (weather, suit telemetry, component status).
- `personal` — voice OK only when audience is believed to be owner-only; otherwise demote
  to AMBIENT on a private surface (calendar, messages summary).
- `private` — **never voice-initiated**; deliver to private surfaces only (WebUI card,
  HUD-when-worn, owner's messaging channel); voice only names the *existence* of a
  private item on request ("you have a private notification"), fiction-canonical
  ("shall I discuss this privately, sir?").
v1 audience detection is deliberately conservative: without reliable presence, `personal`
behaves like `private` for voice unless the user opts a category up. Real audience
inference (speaker ID, HA occupancy, multiple-voices-recently) folds in with the presence
work. `privacy` is a column in the rule config and `attention_log`.

**Action candidates (proactive DOING, not just saying).** The judge may attach an action
proposal to a candidate: `{tool, action, value, rationale}`. Policy tiers:
1. **Auto-execute + inform** — only for categories the user has explicitly pre-authorized
   in config (`auto_actions = ["..."]`), AND the tool is reversible AND not
   `TOOL_CAP_DANGEROUS`. Executes via the normal `command_execute()` path, then delivers
   an AMBIENT/DIGEST notice of what was done. (Fiction: "I've muted notifications for
   your call.")
2. **Suggest + confirm** — the default for everything else: the interjection *offers*
   ("the garage has been open 40 minutes — want me to close it?"); execution happens only
   through the ensuing normal conversation turn, inheriting all existing confirmation
   flows (email two-step, etc.).
3. **Never auto** — `TOOL_CAP_DANGEROUS` and anything physical-actuation (faceplate,
   future repulsor/SPARK commands) is always tier 2 regardless of config.
Outcomes land in `attention_log.outcome` (`executed` / `confirmed` / `declined`), which
feeds the same feedback loop — declined suggestions downweight the category exactly like
dismissed speech.

**Feedback log** (new table, P0/P1; single-writer via existing auth_db discipline):

```sql
CREATE TABLE attention_log (
   id INTEGER PRIMARY KEY,
   user_id INTEGER NOT NULL,
   event_key TEXT NOT NULL, category TEXT NOT NULL,
   source TEXT, summary TEXT,
   gate_rule TEXT, judge_score REAL, urgency TEXT, privacy TEXT,
   surface TEXT, delivered_at INTEGER,          -- NULL = dropped/expired
   outcome TEXT,        -- engaged | acknowledged | dismissed | ignored | suppressed_by_user
   outcome_at INTEGER,
   fired_within_window INTEGER                  -- for FTR computation
);
```

Outcome capture: explicit ("stop telling me about X" — a new tool action that writes
`suppressed_by_user` and downweights the category), behavioral (user responds to the
interjection = engaged; says nothing next turn = ignored; "not now" = dismissed).
Per-category rolling stats (Pielot's 4 features: mean accept rate, last response,
fast/slow EMAs) feed back into gate thresholds (P1: displayed only; P2: judge prompt
context; P3: training labels).

**Metrics tracked from P0** (bench-honesty discipline, same spirit as the memory bench):
FTR [R6], per-category precision (engaged+acknowledged / delivered), volume/day,
expiry rate, judge-invocation count.

---

## 9. DAWN Integration Map (for the implementing agent)

9.1 **New module**: `src/core/attention/` (Layer 2 service) — split from day one per the
    module-split pattern: `attention_core.c` (init/shutdown/state/ring), `attention_ingest.c`
    (source adapters), `attention_gate.c`, `attention_judge.c` (P2), `attention_policy.c`,
    `attention_db.c` (log + stats), behind `include/core/attention/attention.h` +
    `attention_internal.h`. GPL header on every file. `SUCCESS`/`FAILURE` codes, no
    negative returns. New leaf mutex registered in ARCHITECTURE.md lock-ordering section.
9.2 **Prerequisite (separate PR, do first)**: `src/core/suit_service.c` — AURA/SPARK
    telemetry ingest modeled exactly on `src/core/stat_service.c` (MQTT subscribe →
    in-memory cache + optional history → tool query surface). This closes the "DAWN is
    blind to the suit" gap independently of SAGE and gives SAGE its AURA/SPARK source
    for free.

    **Transport decision: OPTION A — DECIDED 2026-07-03 (developer).** Rationale: speed
    of implementation and HUD/UI responsiveness — the serial path into MIRAGE stays
    untouched so HUD widgets keep their current latency, and the deployed ESP-NOW/serial
    helmet setup is not disturbed. Background (verified against mirage source,
    2026-07-03: `mirage/src/comm/command_processing.c:467/508/582` parses AURA's
    Motion/Enviro/GPS JSON from serial and drives HUD widgets; MIRAGE's only MQTT
    publishes are TTS/SFX/snapshot/status — sensor telemetry is never republished, so
    the data dead-ends inside MIRAGE's process).

    **Option A (CHOSEN)**: MIRAGE republishes parsed sensor JSON to `helmet/telemetry`
    (retained-optional, OCP v1.4 envelope), following the existing `stat/telemetry` /
    `echo/telemetry` component convention. Small MIRAGE change at the point it already
    holds the parsed data; helmet stays WiFi-free; SPARK armor telemetry (already
    relayed AURA→serial→MIRAGE, see `registerArmor()` in `mirage/src/hardware/armor.c`)
    republishes the same way (e.g. `armor/telemetry`). DAWN then subscribes like any
    component feed. Implementation notes for the PRE session: publish from where the
    parse already succeeds (don't re-parse), rate-limit to the sensor's natural cadence,
    OCP v1.4 envelope + `./format_code.sh` in the mirage repo too, and verify OCP
    compliance before commit (project preference). **Accepted caveat** (document in
    THREAT/failure model): MIRAGE is now a required hop for suit telemetry — HUD process
    dies ⇒ brain loses the body. Acceptable for v1; SAGE's §9.9 self-monitoring will
    surface a silent helmet feed.

    **Option B (NOT chosen, kept for reference)**: switch AURA to WIFI_MODE/MQTT
    (`aura-v2.5/config.h` — MQTT + TLS already implemented) so AURA publishes `helmet`
    directly. Removes the MIRAGE dependency but adds a WiFi radio dependency inside the
    helmet, adds MQTT latency to the HUD's own sensor widgets, and diverges from the
    deployed ESP-NOW default. Revisit only if the MIRAGE-as-hop caveat bites in
    practice; `suit_service.c` is identical under either option, so switching later
    wastes no DAWN-side work.
9.3 **Delivery reuse**: `session_broadcast_system_message()` (Layer 1, in
    `session_manager`) for context fan-out; scheduler briefing infra for DIGEST;
    `webui_broadcast_*` weak-symbol pattern for WebUI cards (consider this the 7th weak
    symbol — strengthens the case for the planned `scheduler_broadcasts_t` consolidation,
    TODO.md); MQTT `hud` notification path for HUD badges.
9.4 **Memory graph**: judge context queries via existing `memory_db` retrieval; the
    "commitment detected" source hooks `memory_extraction.c` post-extraction (new facts
    tagged as commitments/watch-items feed ingest).
9.5 **Config**: `[attention]` section in `dawn.toml` + `SETTINGS_SCHEMA` entries in
    `www/js/ui/settings.js`. Master default OFF.
9.6 **Schema**: `attention_log` (+ later `attention_category_stats`) via the standard
    migration path — dependent indexes in the migration file, never the base schema
    (invariant: `project_schema_base_vs_migration_ordering.md`).
9.7 **Testing**: Unity tests for gate hysteresis/backoff/TTL expiry (pure logic, very
    unit-testable), policy breakpoint detection, DB round-trip. Judge prompt gets a small
    fixture bench (feed synthetic candidates, assert score ordering) before any live use.
9.8 **No new threads if avoidable**: prefer advancing SAGE on the main loop's existing
    1-second heartbeat (the OTA `ota_rollout_tick` precedent) for gate evaluation + policy
    queue; judge calls go to the existing worker pool. If profiling shows heartbeat
    pressure, one dedicated thread with the documented leaf mutex.
9.9 **Self-monitoring**: a wedged attention layer is indistinguishable from "nothing worth
    saying" — silent failure is the failure mode. Keep per-interval counters
    (events_ingested, gate_passed, judged, delivered, expired) in SAGE state; expose via
    a `attention` action on the `system_status`/debug tool surface and the WebUI panel.
    Cheap liveness rule on the heartbeat: if a source's MQTT feed is alive (component
    status online) but SAGE ingested zero events from it for N minutes where its baseline
    is nonzero, LOG_WARNING + WebUI badge (never a voice interjection about itself —
    a broken SAGE must not be trusted to announce that it's broken).

---

## 10. Where This Extends Beyond the State of the Art

1. **Unified cross-domain salience layer** — papers are single-domain (dialogue [R4],
   screen activity [R5]); commercial systems are siloed features (F10). Suit + home +
   comms + memory through one funnel is novel.
2. **Persistent-memory-informed information-gap scoring** — no surveyed system asks "does
   the user already know this?" against a real user model. DAWN's bitemporal graph makes
   [R4]'s information-gap heuristic real instead of guessed.
3. **Embodied multi-surface mitigation** — HCI's modality-choice strategy [R7] has never
   had helmet-HUD vs room-satellite vs phone vs chat-channel to route across.
4. **Fully-local end-to-end** — 0.47 GB gate [R3] + 4-bit 14B judge beating frontier on
   timing [R6] both fit a Jetson Orin beside Whisper. No commercial system attempts this.
5. **Private ProactiveBench** — the feedback log reproduces [R1]'s labeling pipeline on
   the user's own data, on-device.

## 11. Phasing (each independently shippable, each with a precision gate)

| Phase | Scope | Ships when | Effort |
|---|---|---|---|
| **PRE** ✅ **SHIPPED 2026-07-06** | Option A (DECIDED §9.2): MIRAGE `helmet/telemetry` + `armor/telemetry` republish, then `suit_service.c` AURA/SPARK ingest + `suit_status` tool in DAWN | ~~standalone value: "Friday, what's my helmet CO2?"~~ **DONE — live-verified on hardware** (dawn `d495feb`, mirage `4342147`, aura `1463ecd`) | done |
| **P0** ✅ **SHIPPED 2026-07-10** (`805bc22` + `ee84e14`) | Event schema + poll ingest (STAT/suit/component) + rules gate + hysteresis/backoff/TTL + **conversationally-managed DB watches** + real-time **spoken/banner delivery** + `attention_log` + metrics. **Two deliberate reframes from this row**: watches are DB-backed + voice/WebUI-managed (NOT static config), and delivery is real-time "now" alerts (NOT digest-only — no regular briefing exists in this deployment). See §11.2. | ~~zero interruption risk; digest-only~~ **DONE — live-verified on the suit** ("watch the CO2", spoken ATTENTION alerts, silent banners, `list`/`ignore`/`set` all working). **WebUI Watches panel** (the one deferred P0 surface) also shipped + live-verified 2026-07-10 (see §11.2). | done |
| **P1** ← **PAUSED (next when resumed)** | Urgency taxonomy + BREAKPOINT/AMBIENT/INTERRUPT delivery + budgets/quiet-hours + outcome capture + "stop telling me about X" tool | P0 ran ≥2 weeks; per-category precision on digest items reviewed; start with 2-3 rule categories only | agent ~2-3 days · api $0 · 3 ckpt |
| **P2** | LLM judge (rubric, context pack incl. memory info-gap) + judge bench fixtures | P1 FTR/precision acceptable; judge must beat rules-only on the fixture bench before going live | agent ~2-3 days · api ~$5-15 (bench) · 3 ckpt |
| **P2.5 (optional, training-free)** | PRIME-style [R11] experience retrieval: feed the judge k past interjection outcomes (accepted/dismissed exemplars for this category) retrieved from `attention_log` into its prompt — personalization without any training | P2 live; log has ≥ dozens of outcomes | agent ~2-3h · api $0 · 1 ckpt |
| **P3** | Learned gate distilled from `attention_log` (ONNX or feature-based) | log has ≥ hundreds of labeled outcomes; learned gate beats rules AND P2.5 retrieval on held-out precision | research-shaped; gate on P2 telemetry |

**⏸ PAUSED at the end of P0 (2026-07-10).** PRE + P0 + the WebUI Watches panel are shipped and
live-verified; SAGE now delivers proactive alerts end-to-end. Work is **deliberately paused here**
rather than continuing straight into P1. Rationale: P1's value (urgency tiers, budgets,
quiet-hours, outcome capture, "stop telling me about X") is best tuned against **real field
signal** — the P1 gate is explicitly "P0 ran ≥2 weeks, per-category precision reviewed," which
requires the current build to accrue `attention_log` outcomes first. Nothing about P0 blocks the
pause: the gate/policy/log seams already carry the P1/P2 hooks with no rework (see "What P1/P2
inherit" after §11.2). **Resume trigger for P1**: P0 has run in the field long enough to review
false-trigger rate and per-category precision from `attention_log`, OR a concrete need surfaces
(a watch category that's too noisy → needs budgets/quiet-hours; a user asking to permanently
mute a signal → the "stop telling me about X" tool). Until then this doc is the durable record of
where SAGE stands.

**Future direction (out of scope, noted for the roadmap): anticipation.** ProAct [R12]
shows agents using *idle-time compute* to predict upcoming needs and pre-fetch
information before the user asks (−14.8% task turns, −28.1% hallucinations vs reactive).
That's the other half of JARVIS proactivity — *prepare* unprompted, not just *speak*
unprompted. It composes with durable background jobs, not with SAGE's interjection
pipeline; revisit after both exist.

**Future direction (out of scope, the real destination): dynamic telemetry awareness —
retire the curated catalog.** The P0 metric *catalog* (`src/core/attention/attention_catalog.c`
— a hand-maintained table of watchable signals + per-metric reader/label/unit/defaults) is a
**scaffold, not the end state**. JARVIS/FRIDAY does not consult a list of things they're
*allowed* to watch — they are aware of everything flowing through the suit and can watch *any*
of it on request ("Friday, keep an eye on the coolant flow rate" works even though no one
pre-registered "coolant flow rate"). The catalog is a finite enumeration standing in for that
awareness because P0 needs concrete readers, sensible defaults, and a bounded vocabulary to
ship safely. **The destination is a self-describing telemetry model**: a generic ingester
subscribes to *all* `*/telemetry` topics, flattens each OCP envelope to dotted paths
(`device.type.field`), and maintains a runtime map of `path → {last value, timestamp, unit?}`.
Any numeric field any component publishes — including a *new* component added later — becomes
watchable with **zero code change**; the "catalog" becomes discovered-from-the-wire, not
compile-time. The per-metric `read_*` functions collapse into one generic map lookup. What the
catalog gives us that a raw wire-map does not — human labels, sensible default thresholds,
per-metric loudness, and validation — must be recovered another way: (a) components
*self-describing* their fields (units/label/nominal-range in the telemetry envelope or a
published schema — an OCP spec extension), (b) small heuristics (a % field defaults to an
`above 90` band; a `*_faults` count to `above 0`), and (c) LLM-inferred defaults at watch-create
time when the field is otherwise undescribed. Trade-offs to design through: loss of compile-time
safety, unit/scale ambiguity (the P0 `air_quality` direction question generalizes to every
discovered field), and naming stability across firmware versions. **Guiding principle for all
future work here: DAWN's awareness should be dynamic and derived from what's actually on the
bus, not a human-maintained enumeration — build toward "she knows what's flowing," not "she has
a longer list."** Slots naturally alongside the SAGE ingest layer (it already samples the same
telemetry snapshots); the catalog stays as the labeled/curated *overlay* on top of the
discovered map (so the common signals keep good defaults) rather than the gate on what's
watchable.

### 11.1 PRE — as-built (shipped 2026-07-06)

Committed: dawn `d495feb`, mirage `4342147`, aura `1463ecd`. Live-verified on the real suit
(helmet CO2/heading/position queries, staleness detection with MIRAGE down, self-heal on
helmet re-enumeration, real 5-piece armor roster). Reviewed — DAWN suit (4 agents): 0
crit/high after fixes (an `append_armor` snprintf-truncation OOB and a `#ifdef` include-guard
coupling); MIRAGE serial (3 agents): 0 crit/high, 1 med (runtime config-reload divergence)
fixed.

**What shipped:**
- **MIRAGE** `src/comm/suit_telemetry.c` — republishes the already-parsed AURA Enviro/Motion/
  GPS to `helmet/telemetry` and SPARK armor temp/voltage to `armor/telemetry`, OCP v1.4
  envelope `{device:"aura"|"spark", msg_type:"telemetry", type:"Enviro"|"Motion"|"GPS"|"Armor",
  timestamp, …}`. Helmet feed rate-limited to ~1 Hz per type (keeps the compass-rate Motion
  stream off the broker); `mqttSendMessageQuiet` for the streaming path.
- **DAWN** `src/core/suit_service.c` + `suit_status` tool — MQTT ingest → mutex-guarded live
  cache, **per-subsystem** TTL staleness (enviro/motion/gps each carry their own last-seen so
  a stalled Enviro can't hide behind fresh Motion), bounded 16-piece armor roster. Tool
  actions: environment/orientation/position/armor/all.

**Deviations from the design (intentional):**
- **Live cache only — NO history DB.** §9.2 said "optional history"; deferred. Unlike the
  `stat_service`/`stat_db` sibling, `suit_service` does not persist. SAGE's `attention_log`
  (P0) becomes the real event store; suit trends can get their own `suit_db` later if wanted.
- **No `SETTINGS_SCHEMA`/WebUI entry** — config lives in `dawn.toml` `[suit]` only, matching
  the `stat` sibling (topics/paths are excluded from the WebUI panel).

**Live-test learnings (carry into P0):**
- The helmet is registered as an *armor element* in MIRAGE's config (for the HUD), so it
  matched the armor-republish hook on every high-rate Motion message → armor republish is now
  gated on `have_temp || have_voltage` (a bare presence event carries nothing for the roster).
- GPS position ships as `latitudeDegrees`/`longitudeDegrees` (AURA never emits plain
  `latitude`/`longitude`); publish those under the wire `latitude`/`longitude` keys.
- AURA's heat index was double-broken (guarded behind `humidity>=40` → N/A at 39.5%, *and* a
  misderived formula over-predicting ~17 °C); replaced with the NWS Rothfusz Celsius regression,
  always emitted (air-temp fallback below 26.7 °C).
- **The feed's transport is a load-bearing dependency.** The helmet USB kept dropping (ESP32-S3
  native USB, 3 hubs deep on a flaky RTS5411). PRE hardened MIRAGE's serial link: configurable
  port (`config.json` `Serial Port`/`Serial Enable`, CLI `-d`/`-u` > config > default), a
  24→256-byte buffer fix, and a `/dev/serial/by-id/…*-if00` **glob mask** so the config is
  portable across identical boards with no per-board serial. Move the helmet off the shared hub
  onto a powered/root port for stability.

**Deferred PRE follow-ups (none block P0):** suit history DB; register the suit feed with
`component_status` so a wedged helmet feed is observable (folds naturally into §9.9
self-monitoring at P0); wire AURA's GGA `quality` field to a real fix-type; MIRAGE serial
single-source-of-truth cleanup; a `STAT=OFF/SUIT=ON` CI matrix leg.

**What P0 inherits:** the suit is now a ready event source. P0's ingest has five feeds
available immediately — STAT, ECHO, scheduler, component-status, and suit.

### 11.2 P0 — as-built (shipped 2026-07-10: core `805bc22` + Watches panel `ee84e14`, both live-verified) — PAUSED here

Live-verified on the real suit (set watches by voice, spoken ATTENTION alerts fired on
threshold crossings, silent banners for ambient watches, `list`/`ignore`/`set` all working).
Plan: `~/.claude/plans/sparkling-purring-pony.md`. Reviewed by 6 agents across two passes
(architecture / embedded-efficiency / security on the core; ui-design-architect /
reuse-pattern-reviewer on the delivery channel) — all findings applied.

**What shipped:**
- **Module** `src/core/attention/` (Layer-2 leaf service): `attention_core.c` (lifecycle, watch
  cache + reload, leaf-mutex event queue, the heartbeat `attention_tick`, metrics),
  `attention_catalog.c` (the metric catalog — single source of truth), `attention_ingest.c`
  (poll adapters), `attention_gate.c` (pure threshold/slope/absence + hysteresis + backoff +
  TTL), `attention_policy.c` (notify→mode + per-hour budget — the P1/P2 seam), `attention_db.c`.
- **`attention` LLM tool** (`src/tools/attention_tool.c`) — watch/list/ignore/set/remove, the
  primary management surface. Metric param is a free-form string whose description is generated
  from the catalog at registration (no hardcoded enum, no 16-cap, single source of truth).
- **DB** — migration **v71** (`attention_rules` + `attention_log`, P1/P2 columns nullable) +
  `src/auth/auth_db_attention.c` CRUD (ad-hoc prepared statements, cold path).
- **Delivery** — a new `scheduler_emit_alert()` (voice, off the heartbeat thread) + SAGE's own
  `attention_alert` WebUI channel (ATTENTION badge, chime-free) rather than the scheduler's
  notification path. `[attention]` config (master switch default OFF) + WebUI settings scalars.
- **Heartbeat** wired into both `dawn.c` main loops; `attention_init` seeds 3 safety watches.

**Deviations from the P0 row (deliberate, dev-directed):**
- **Watches are DB-backed + conversationally managed, NOT `[[attention.rule]]` TOML.** The JARVIS
  principle: you *tell* Friday what to watch ("watch the CO2", "ignore that temp"), you don't
  edit a config file. TOML `[attention]` keeps only operator scalars.
- **Real-time spoken/banner delivery, NOT digest-only.** This deployment has no regular briefing,
  so digest-only would deliver into a void. Alerts speak (off-thread, chime-free) + show an
  ATTENTION banner; each watch carries a loudness level (`alert`/`ambient`). Digest remains a
  log-only forward-compat placeholder.
- **Poll-only ingest** (STAT/suit/component snapshots on the tick) — ECHO discrete events deferred
  to P1 (they need push hooks; `on_message` dispatch is single-consumer). The event queue exists
  now so P1 push slots in with no restructuring.
- **Catalog is a scaffold, not the destination** — see §11 "Future direction: dynamic telemetry
  awareness". ~24 STAT+AURA signals enumerated; the end state derives the watchable set from the
  wire, not a hand-maintained table.

**Bugs found + fixed during live testing (worth remembering):**
- Loudness ignored the user's wording — "*alert* me" produced a silent watch because the catalog
  default won. Fixed: the tool's `level` param now maps phrasing ("alert/tell/let me know" →
  spoken; "quietly/just show" → silent) so the user's words drive loudness.
- Reusing the scheduler notification path mislabeled alerts as "REMINDER" **and** made the browser
  chime on every banner (breaking "ambient = silent"). Fixed by the dedicated `attention_alert`
  channel; `scheduler_emit_alert` no longer emits a banner.
- `set` silently dropped the `direction` argument (updated threshold/level but not direction) —
  fixed; the callback now forwards it.
- Editing a watch's threshold/direction preserved **stale gate state** (hysteresis/backoff/slope
  computed against the old condition) — fixed: reload preserves state only when the firing config
  is unchanged (`same_gate_config`); an edit starts fresh.
- The tool returned no current value, so the LLM **confabulated** the live reading — fixed:
  watch/set confirmations append the real value (+ "already past the threshold") from the live
  snapshot.

**Deferred P0 follow-ups (none blocking):** ~~WebUI Watches panel~~ **SHIPPED 2026-07-10 (`ee84e14`, live-verified)** — added as a 2nd tab inside the Scheduler popover (renamed "Scheduler & Watches", widened to 504px, tabs wrap to two rows). Live-verified: create/fire/edit/pause/remove + live metric display all confirmed. `src/webui/webui_attention.c` (WS `watch_list/add/update/set_enabled/remove`, user-scoped, driving the same `attention_watch_*` core) + `www/js/ui/watches.js` (`DawnWatches`) + `www/css/components/watches.css`; list/toggle/edit/delete/add + live values + a "master switch off" note. Enum↔string serialization + `clamp_seconds` extracted to the attention core (`sage_*_to_str`/`_from_str`/`attention_clamp_seconds`) so the tool + panel can't drift on the wire vocab. 5-agent review applied (sec 0 findings; fixed modal-vs-outside-click, Remove-button styling, add-dropdown clobber, a11y). Deferred within the panel: threshold reset-to-default (needs a server sentinel), and two efficiency LOWs (per-watch snapshot + full re-render on the 5s poll) with triggers. Still open: §9.9 per-source
liveness self-monitoring (counters exist, the "silent source" warn is deferred); `broadcast_json_to_user`
already extracted (8 fan-out broadcasters de-duplicated) during review; slope-rate can't yet be set
via the tool (no slope metric in the catalog — latent until one is added); `STAT=OFF/SUIT=ON` CI leg.

**What P1/P2 inherit (no rework):** the `attention_policy_decide` seam (P2 judge inserts before it,
P1 budgets/quiet-hours/demotion inside it); `sage_event_t`'s unused `raw_json`/`entities`/`privacy`
seams; `attention_log`'s nullable `judge_score`/`urgency`/`outcome` columns; the event queue for P1
push sources; `attention_rules.muted_until` for P1 timed snooze ("ignore that for now").

## 12. References

| # | Work | Link | Role in design | Verification |
|---|---|---|---|---|
| R1 | *Proactive Agent: Shifting LLM Agents from Reactive Responses to Active Assistance* + **ProactiveBench** (6,790 events) — **ICLR 2025**. **Artifacts PUBLIC (checked 2026-07-03)**: code+data at github.com/thunlp/ProactiveAgent (**Apache-2.0**); trained agent + reward model at huggingface.co/YancyLee/ProactiveAgent (reward model 0.918 F1 on their test set) | arxiv.org/abs/2410.12361 | reward-model-from-accept/reject-labels pipeline; F4; P2 fixture-bench seed (§7) | pipeline + benchmark claims **VERIFIED 3-0**; F1 66.47% **VERIFIED against abstract 2026-07-03** |
| R2 | **ProactiveEval**: unified proactive-dialogue evaluation (target planning vs dialogue guidance; 328 envs, 22 LLMs) | arxiv.org/pdf/2508.20973 | F9, judge/generation separation | unverified extraction |
| R3 | **DiscussLLM: Teaching Large Language Models When to Speak** (NEC Labs, preprint May 2026) — RoBERTa gate + Llama-3-8B; silent-token training; code github.com/necla-ml/DiscussLLM | arxiv.org/pdf/2508.18167 | F2, gate sizing numbers | **all numbers VERIFIED against Table 1, 2026-07-03**; synthetic-data caveat noted at F2 |
| R4 | **Inner Thoughts**: intrinsic-motivation proactive conversation (8-heuristic CoT scoring) — **CHI 2025** | arxiv.org/html/2501.00383v2 | F3, judge rubric | **VERIFIED against paper 2026-07-03** (heuristic list, thresholds, 82%, 7 dimensions) |
| R5 | **ProAgentBench**: when-to-assist / how-to-assist decomposition; alert-fatigue framing | arxiv.org/html/2602.04482 | F6, frontier-LLM when-accuracy ceiling | unverified extraction |
| R6 | **ProActor: Timing-Aware Reinforcement Learning for Proactive Task Scheduling Agents** (UC Santa Cruz + Zillow, **ACL 2026**) — opportunity windows, FTR, GRPO turn-level RL; ART-F + annotation pipelines "to be open-sourced" | arxiv.org/pdf/2605.24900 | F5, P4/TTL, FTR metric, local-judge feasibility | **windows/FTR/timing claims VERIFIED against Eq. 4/6 + Table 1, 2026-07-03**; Home-Loan nuance at F5 |
| R7 | Survey: attention-management systems (sensing→…→managing; mediating/mitigating/indicating; Attelia II; cascading classifiers) | arxiv.org/pdf/1806.06771 | F1, F8, pipeline + policy taxonomy | unverified extraction |
| R8 | Pielot et al., *Beyond Interruptibility: Predicting Opportune Moments to Engage Mobile Phone Users*, UbiComp 2017 | pielot.org/pubs/Pielot2017-UbiComp-Engagement.pdf | F7 — 4 past-behavior features ⇒ 5× precision; interruptibility≠engagement | unverified extraction |
| R9 | Interruptibility-prediction pipeline convention | dl.acm.org/doi/10.1145/2750858.2807514 | F8 (methodological ancestor) | unverified extraction |
| R10 | LangChain *Introducing ambient agents* (+ LangGraph interrupt/persistence, executive email assistant example) | langchain.com/blog/introducing-ambient-agents | F10; notify/question/review HITL taxonomy (mirrors §8 tiers) | checked 2026-07-03 |
| R11 | **PRIME**: training-free proactive reasoning via iterative memory evolution (3 experience zones: strategies / failure patterns / user prefs, RAG-guided) | arxiv.org/abs/2604.07645 | P2.5 training-free personalization option | abstract-level only |
| R12 | **ProAct / ProActEval**: idle-time compute for anticipation (−14.8% turns, −28.1% hallucination vs reactive; 200 scenarios / 40 domains) | arxiv.org/abs/2605.25971 | future anticipation direction (§11 note) | abstract-level only |
| R13 | Commercial ships (checked 2026-07-03): Alexa Hunches — architecture at amazon.science *"The science behind Hunches: deep device embeddings"* (encoder-decoder device embeddings, ~60 k-means archetypes, separate Hunch-**acceptance model**) + developer.amazon.com Hunches posts (opt-in autonomous actions, Smart Home Skill / AutomationManagement APIs); ChatGPT Pulse (openai.com/index/introducing-chatgpt-pulse — nightly research → rated digest cards) | see F10 | F10; acceptance-model precedent for P3; per-item rating UX for DIGEST | checked 2026-07-03 |
| R14 | Horvitz et al., **bounded deferral** lineage: *Principles of Bounded Deferral* (microsoft.com/research tr-2005-87), *Balancing Awareness and Interruption* (Interact 2005), *Attention-Sensitive Alerting* (arxiv.org/abs/1301.6707, utility-directed defer-vs-interrupt), *BusyBody* (CSCW, personalized cost-of-interruption models) | see links | §8 BREAKPOINT max-deferral mechanism | checked 2026-07-03 (abstract level) |

Internal references: session gap-analysis (2026-07-03, this repo's Claude session "Fable's
Take on JARVIS"); agent deep-read reports on MIRAGE/AURA/SPARK/STAT/ECHO integration seams
(same session); `docs/TODO.md` §"Proactive background observation (memory Phase 2)";
`atlas/dawn/memory/DYNAMIC_CONTEXT_INJECTION.md` (Phase 1 focus injection — the reactive
sibling of this work); `src/core/stat_service.c` (the ingest pattern to copy).

## 13. Open Questions (resolve during PRE/P0)

1. Component name — SAGE is a placeholder.
2. Judge model default: Haiku-class cloud vs local Qwen — decide at P2 with the fixture
   bench; the [R6] result argues local is viable, but DAWN's extraction-model sweep
   precedent (Haiku won) says bench it, don't assume.
3. Does the scheduler's briefing engine absorb DIGEST delivery as-is, or does SAGE need
   its own digest formatter? (Inspect `briefing_thread_func` capacity during P0.)
4. Multi-user: `attention_log` is per-user from day one, but v1 delivery assumes the
   single-owner deployment; per-user routing folds in with speaker-ID/presence work.
5. INTERRUPT-class preemption depends on barge-in/TTS-preemption not yet built — until
   then INTERRUPT = front-of-queue + chime, which is acceptable for v1.

## 14. Follow-Up Queue — Not Fully Investigated

The research harness was terminated early (rate limits); a journal audit recovered the
dropped sources worth having (Hunches architecture, bounded deferral, Inner Thoughts
verification). What remains genuinely uninvestigated, for follow-up when token budget
allows — none of it is believed load-bearing:

1. **Four unidentified papers** the search phase surfaced but never fetched:
   arxiv.org/abs/1712.07120, 2106.02077, 2505.14668, 2605.30152. Unknown content; three
   stray search hits vs. eight convergent sources = low risk, but unexamined.
2. **Abstract-level-only sources**: PRIME [R11], ProAct [R12], and the Horvitz bounded-
   deferral lineage [R14] were read at abstract/summary depth only. Before implementing
   P2.5 (PRIME-style retrieval) or the §8 max-deferral bounds, read the method sections.
3. **~20 minor corroborating claims** from the original harness remain extraction-level
   (quoted from papers, never adversarially voted). All four pillar papers ARE verified;
   the residue is supporting detail (e.g. R2 ProactiveEval's 328-env/22-LLM specifics,
   R7 survey internals).
4. **Skipped open-source deep dives**: Letta's background/"sleeptime" agent features,
   Home Assistant's official AI direction (home-assistant.io/blog/2025/09/11/ai-in-home-
   assistant), the HA `ai_automation_suggester` community integration, Apple's App
   Intents donation/ranking docs. Expected to confirm F10, might contain borrowable
   implementation details.
5. **External-validity caveats to re-check at P2**: DiscussLLM's numbers are on its own
   synthetic test set (noted at F2); ProActor's composite-ranking win holds only on the
   with-action-observations dataset (noted at F5). Neither weakens the architecture;
   both matter when picking the P2 judge default (§13 Q2 says bench it regardless).
6. **Watch item**: ProActor's ART-F RL framework + annotation pipeline are stated
   "to be open-sourced" — if that lands, it's the P3 training recipe off the shelf.
7. **The perception loop and durable-jobs designs** (§1.1 gaps 2 and 3) are cross-
   referenced but undesigned — each needs its own design doc before implementation.

## 15. OASIS Proactivity Roadmap (all six gaps, dependency-ordered)

Four tracks. A and B are the trunks; C is substrate both trunks consume; D is the
late-arriving source. Within a track, order is strict; across tracks, work can
interleave. Effort in agent terms per project convention.

**Track A — SAGE (this document)**
| Step | What | Gate |
|---|---|---|
| A1 (PRE) ✅ | Option A (DECIDED 2026-07-03): MIRAGE `helmet/telemetry`/`armor/telemetry` republish + `suit_service.c` + `suit_status` tool | ~~ready to start~~ **SHIPPED 2026-07-06** (see §11.1) |
| A2 (P0) ✅ | Event schema, poll ingest (STAT/suit/component), rules gate, **conversationally-managed DB watches + real-time spoken/banner delivery** (reframed from digest-only), `attention_log`, metrics, **+ WebUI Watches panel** | ~~A1 shipped~~ **SHIPPED 2026-07-10** (`805bc22` + `ee84e14`; see §11.2) |
| A3 (P1) ← **PAUSED (resume next)** | Urgency tiers + bounded deferral, budgets, privacy gating, outcome capture, "stop telling me about X"; ECHO discrete-event push ingest | Track A **paused after P0** (2026-07-10) — resume when P0 has run in the field ≥2 weeks and per-category precision from `attention_log` is reviewed, or a noisy-watch / permanent-mute need surfaces |
| A4 (P2) | LLM judge (Inner-Thoughts rubric + memory info-gap), fixture bench seeded from ProactiveBench | P1 FTR acceptable; judge beats rules on fixtures |
| A5 (P2.5) | Training-free personalization: feedback-log exemplar retrieval into judge prompt | P2 live, ≥dozens of outcomes |
| A6 (P3) | Learned gate distilled from `attention_log` | ≥hundreds of outcomes; beats rules AND P2.5 |

**Track B — Durable background jobs** (peer trunk; concept → needs its own design doc)
B1 design pass (registry, detached worker, reconnect-replay, cancel, briefing delivery) →
B2 v1 + first consumer (deep research) → B3 watch-condition jobs terminating in SAGE
events (the A×B integration; requires A2+).

**Track C — Substrate** (suppliers; each independently valuable)
- C1 Speaker ID (sherpa-onnx, on TODO) — feeds C2, memory attribution, per-user auth.
- C2 Presence/room model (unpause `satellite_mappings`) — now has 3 consumers: phone-ring
  routing, SAGE surface routing, SAGE audience gating. Sharpens A3's privacy tiers.
- C3 Hot-mic conversation window (SMS `active_window_sec` pattern on voice) — cheap,
  reactive-UX win, independent of SAGE.
- C4 Barge-in (AEC; TODO §3) — upgrades SAGE INTERRUPT from queue-front to true preemption.

**Track D — Perception** (own design doc first)
D1 suit VLM/detector event loop (low-fps, on already-flowing MIRAGE frames, emits
`sage_event_t`) → D2 home cameras on the same loop. Requires A2 (the socket) and realistic
GPU budget analysis alongside Whisper.

**Suggested interleaving** (rationale: start the feedback-data clock early — the
`attention_log` accrues value with calendar time and is the moat no one else has):
A1 → A2 → B1 (parallelizable with A) → A3 → C1/C2 → A4/A5 → B2/B3 → C3/C4 → D1 → A6 → D2.

Anticipation (ProAct-style idle-time prepare-before-asked, §11 note) slots after B2 as a
jobs-side feature. TODO.md should absorb this section as the program-of-record when SAGE
work starts, replacing the "Proactive background observation (memory Phase 2)" item.

---

## 16. Multi-named watches per metric — SHIPPED 2026-08-25 (post-P0 evolution)

P0 stored **one watch per metric**. That was pure app convention — `attention_rules`
already stored multi-row with a `name` column and the gate already keyed identity per-watch
(`event_key = "metric#id"`), so nothing in storage or evaluation enforced it; the constraint
lived only in `attention_watch_find_by_metric` (find-then-update) and in keeping conversational
addressing ("ignore the temperature") unambiguous. It blocked three real patterns: **band
alerts** (min AND max), **tiered thresholds** ("warm at 75, *we're going to die* at 120"), and
**named, specifically-spoken alerts**. This evolution lifts it to **N named watches per metric**.

Full design + the plan-/arch-review resolution log lived in the working doc
`docs/ATTENTION_MULTI_WATCH_DESIGN.md` (untracked; retired once this section landed). Summary:

- **Model.** `name` becomes the user-facing identity and primary conversational address,
  **unique per user** (case-insensitive). A metric is now a *group*. `event_key` stays per-watch
  so two watches on one metric never cross-suppress.
- **Naming.** A `named` bool column (**schema v80**) distinguishes a user-chosen name (spoken in
  alerts, never regenerated) from a system **auto-name** (condition-derived, e.g. `"CO2 above
  1200"`, regenerated on edit while unnamed so it never lies). User-name uniqueness is enforced at
  **one core seam** (`attention_watch_add`/`_update`, surfaced as `ATTENTION_NAME_TAKEN`); the
  auto-namer dedups system names.
- **Tool grammar** (`attention_tool.c`), chosen to match DAWN's dominant tool pattern (survey of
  scheduler/memory/HA/calendar/email/document_manage): `watch` (additive; dedups only on an
  *identical* condition via `same_gate_config`) / `list` (name + `[id]`) / `set` (edit one) /
  `ignore` / `resume` (new enable verb) / `remove`. Selectors: **name** (specific), **metric or
  component-prefix** (group, e.g. `stat.cpu`), **id** (exact, from `list`); resolution order
  id > name > metric/prefix. `ignore`/`resume` act on **all** matches (reversible → group is the
  intent); `set`/`remove` **list-and-ask** on >1 (never a silent bulk mutation).
- **Slope creation folded in.** Rising/falling watches (§6 slope rules) are now creatable by
  voice/chat — `direction` extended to {above,below,rising,falling} + a `rate` param; `rule_type=slope`
  inferred. (Previously the gate evaluated slope but nothing could create one.)
- **Name-aware alerts.** `build_summary` speaks the user name when set — *"you're over the
  '<name>' threshold — <value>"* — else P0's exact format. Kept actionable (name **+** value).
- **Shared validators.** Rule-kind + trigger validation extracted from the WebUI into the core
  (`attention_resolve_rule_kind`/`attention_validate_watch_trigger`), so the tool and panel share
  one definition and can't drift.
- **Wire (additive):** `watch_add`/`watch_update` accept `name`; `watch_list_response` emits
  `named` + catalog `rule_type`/`default_direction`/`default_threshold`; add no longer dedups by
  metric. The WebUI panel (multi-row + name + slope selector) is built off this in lockstep.
- **Hardening:** mutating tool actions are refused for callers with no session context
  (unauth-MQTT-as-user-1 backstop, mirroring `job_tool`); user names are C0-sanitized at one shared
  ingest helper.

**Alert budget clarification** (§8 policy). `[attention] max_alerts_per_hour` (default **4**) is a
**global** spoken-alert budget: over-budget ALERTs demote to a silent AMBIENT banner
(`attention_policy_decide`). **`0` disables it** (every alert always spoken; guarded by
`s_max_per_hour > 0`). Multi-watch makes the global cap easier to hit, which surfaces the natural
P1+ refinement below.

**Deferreds** (none blocking): voice **rename** via the tool (`new_name` on `set` — today `name` is
the selector there; the panel is the rename surface); a `UNIQUE(user_id, name COLLATE NOCASE)` DB
index (needs a dedup backfill + provably collision-free auto-namer first — uniqueness is
core-app-enforced today); and a **per-priority / per-level alert budget** so a life-safety watch is
never silenced by chatty ones consuming the global quota — newly relevant now that tiered/named
alerts make the priority distinction real. Fold the per-priority budget into P1 (§11 A3 urgency
tiers + budgets), where it belongs.

**Code map:** `attention_core.c` (naming, uniqueness, finders, shared validators), `attention_tool.c`
(grammar), `attention_gate.c` (name-aware summary), `webui_attention.c` (wire), `auth_db_attention.c`
+ v71/v80 migrations (`named` column), `attention_catalog.c` (catalog wire accessors).
