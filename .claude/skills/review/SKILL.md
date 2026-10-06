---
name: review
description: Run the six review agents on changes, synthesize findings, triage based on developer thinking, and fix approved items. Trigger when the user says "review", "run the agents", "run the big three", "run all four", "run all five", "run all six", "full review", "code review", or similar.
disable-model-invocation: false
user-invocable: true
allowed-tools: Read, Grep, Glob, Bash, Edit, Write, Agent
argument-hint: "[scope description]"
---

# Code Review + Fix Skill

You are performing a full review cycle: launch agents, synthesize results, validate, triage, and fix.

## Scope

The user may specify scope in any way they like — a file path, "the last commit", "everything uncommitted", "the OAuth changes", "just the C files", or nothing at all. Interpret their intent and gather the right diff.

If no argument is given, default to all uncommitted changes (`git diff` + `git diff --cached` + new untracked source files from `git status`).

For a whole-branch or pre-merge review ("review the branch", "before I push/merge"), use `git diff main...HEAD` plus `git log main..HEAD --oneline`, and tell the correctness-reviewer it is in holistic-diff mode so it hunts cross-commit interactions.

The user may also say something like "and fix everything" or "auto-fix" — this means skip the confirmation step and proceed directly to fixes after triage. The default is to wait for confirmation.

## Step 1: Gather Changes

1. Run the appropriate git commands for the scope
2. Note which file types are involved — this determines which agents are relevant

## Step 2: Launch Review Agents

Launch the agents **in parallel** (single message, multiple Agent tool calls). Feed each one the actual diff content and the project context (embedded C on Jetson, CLAUDE.md standards).

The agents already know their domains and how to structure their output:

1. **architecture-reviewer**
2. **embedded-efficiency-reviewer**
3. **security-auditor**
4. **correctness-reviewer**: logic and correctness, the lens the specialists structurally miss. It covers state-ordering hazards, contract vs. implementation, boundaries, error paths and cross-commit interactions. It also checks **non-literal format strings**: prompt templates and other named formats passed to `snprintf`/logging, which `-Wformat` cannot check, including a stray `%` in prompt prose.
5. **ui-design-architect**: relevant for ANY UI changes: web (JS/CSS/HTML), SDL, LVGL, or any other display/interaction layer
6. **coding-standards-auditor**: audits compliance with CLAUDE.md and CODING_STYLE_GUIDE.md (return codes, naming, Doxygen, formatting, include hygiene)

**Which agents per trigger phrase** (correctness-reviewer is in every set; it's the general-logic lens, not a specialist):

| Phrase | Agents |
|---|---|
| "review", "code review", "run the agents", "run the big three" | 1–4 |
| "run all four" | 1–4 + ui-design-architect |
| "run all five", "run all six", "full review" | all six (mandatory for large refactors, new modules, pre-release audits) |

If the changes clearly don't touch any UI surface at all, you may skip the ui-design-architect and note why. Do NOT substitute or add `master-code-reviewer`; the specialized set above replaces it. Use it only if the developer asks for it by name.

## Step 3: Synthesize and Validate

After all agents complete, review each finding yourself before including it in the table. Read the relevant code and confirm the issue is real — agents sometimes misread context, flag code that's actually correct, or raise concerns already handled elsewhere. Drop false positives and note if a finding is valid but already mitigated.

Create a consolidated findings table of validated issues:

| # | Severity | Agent | File | Finding | Action |
|---|----------|-------|------|---------|--------|

## Step 4: Triage

The goal is to fix things that genuinely improve the code — real bugs, real security issues, real architectural problems, real efficiency wins on constrained hardware. Not to chase perfection or pad a list.

**How the developer thinks about this:**

- If it's broken, unsafe, or violates the project's own standards (CLAUDE.md coding style, error handling patterns, memory management rules) — fix it. That's why we have standards.
- If it's a real efficiency problem on embedded (unnecessary allocations in hot paths, unbounded buffers, things that'll hurt on a Jetson with limited RAM) — fix it. Abstract "could be more efficient" observations on cold paths aren't worth touching.
- If it's a genuine code quality improvement that makes the code clearer or more correct, and the fix is localized and low-risk — fix it. Good engineering.
- If it's a UI consistency issue — mismatched styling between parallel components, accessibility gaps, broken patterns that work in one module but not the mirrored one — fix it. UI quality matters as much as backend quality. Don't dismiss visual or interaction bugs just because they're "only UI."
- If it's a real bug — even low-severity — and the fix is easy and localized, fix it. Don't skip real issues just because they're low priority. The cost of a one-line fix is near zero; the cost of a known bug sitting around is not.
- **Pre-existing issues: fix them.** If an agent surfaces a real issue — bug, missing validation, inaccurate docs, security gap — fix it regardless of whether this diff introduced it. The review found it; that's the opportunity. If we don't fix it now, when will we? The only valid skip reason is if the fix is genuinely invasive (touches many files outside the diff, changes public APIs, high risk of regression). Never use "pre-existing" as an automatic skip. Triage on merit: severity + fix effort, not on when it was introduced.
- If it's a nitpick, a style opinion beyond what CLAUDE.md specifies, or a "nice to have" future improvement unrelated to the code being changed — skip it. Mention it in the table for awareness, but don't touch working code to satisfy a preference.
- If fixing something would change a public API, ripple across multiple modules, or feel like scope creep — flag it but don't fix it without asking.

Use your judgment. The developer trusts you to tell the difference between "this matters" and "this is technically correct but not worth the diff noise."

## Step 5: Present Triage

Show the developer:
1. The findings table with your **Action** column filled in (Fix / Skip / Ask)
2. A brief summary: "X findings total, fixing Y, skipping Z"
3. Which items you're about to fix and a short rationale

**Default: wait for confirmation before proceeding.** The developer may override triage decisions. If the user indicated auto-fix (e.g., "fix everything", "don't ask"), proceed directly to Step 6.

## Step 6: Execute Fixes

For each approved fix:
1. Read the file first (always)
2. Make the minimal change needed — don't refactor surrounding code

Then verify in the project's commit order. Format **before** building, so the bytes you compiled and tested are the bytes that get committed:
1. `./format_code.sh --changed` (never the bare full-tree run, which is slow and touches unrelated files)
2. `make -C build-debug -j8 2>&1 | grep -E "warning:|error:"`: the build must be clean, **zero warnings** (no warnings get checked in)
3. `make -C build-debug tests-ci` then run the unit tests relevant to the touched modules (`./build-debug/tests/test_<name>`). Plain `make` does not rebuild test binaries, and some tests bare-link a single `.c`, so a new cross-module call only fails to link here.
4. `./format_code.sh --check --changed`
5. If any step fails, fix it before continuing

## Step 6b: Re-review Post-Review Changes

Code written after the agent pass (the fixes themselves, or new work added during a redirect) was seen by no reviewer. If a fix is more than a trivial localized edit (new logic, new branches, touched lock/lifetime/ordering, or a multi-file change), re-run the relevant lens(es) on just that delta before calling the review done. A bug introduced while applying feedback is invisible to the original pass; that is how the reconnect-state bug on the music branch reached a PR.

## Step 7: Summary

1. List what was fixed (file:line + one-line description)
2. List what was skipped and why
3. Confirm build (zero warnings) + tests + format check pass, and note any post-review re-review from Step 6b
4. Do NOT run `git add`/`commit`/`push`; the developer handles git. Offer a single `git add <files>` command and a terse commit message (no Claude trailers, no real names, no internal planning labels)
