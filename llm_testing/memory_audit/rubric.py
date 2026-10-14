# Memory audit: the kinds of stored fact, and the classifier prompt.
#
# The labeller and the models name a fact's kind only.  Its tier, where it
# belongs in memory, follows from the kind:
#   semantic  true about the user now; injected into every turn
#   episodic  what happened and when; kept with its time and source conversation,
#             retrieved when a question is about the past
#   pointer   lives in a document; memory should point to it
#   discard   not worth storing anywhere
#   other     fits no kind; the note says what it is
# The kinds are a draft; the hand labels and their notes decide them.
#
# License: GPLv3, same as DAWN.

RUBRIC_VERSION = 4

# (kind, tier, definition).  Order is the labelling menu's numbering.
KINDS = [
    ("identity", "semantic",
     "who the user is: background, work role, skills, health, home, household"),
    ("preference", "semantic",
     "a standing like, dislike, opinion, or way the user wants things done"),
    ("relationship", "semantic",
     "a person or pet in the user's life and how they are connected"),
    ("ongoing", "semantic",
     "a project, goal, plan, commitment or routine that lasts weeks or more"),
    ("upcoming", "semantic",
     "an event or plan of the user's on a known date (a trip, a conference, an "
     "appointment)"),
    ("reference", "semantic",
     "a stable detail worth recalling later: something they own, a birthday, an "
     "account or service they use, a place they go"),
    ("one_off_event", "episodic",
     "something that happened once (a meal, an errand, a visit, one conversation's "
     "topic)"),
    ("transient_state", "episodic",
     "a state that changes on its own: a device on or off, a door locked, the "
     "weather, what's playing, a battery level, a current reading"),
    ("work_log", "episodic",
     "a step in a task: a bug fixed, a commit made, a file edited, a test run, a "
     "setting changed"),
    ("assistant_said", "discard",
     "the assistant's own answer, suggestion, opinion or action recorded as a "
     "fact about the user"),
    ("conversation_meta", "discard",
     "about the conversation itself: the user asked about X, wanted to know Y, "
     "tested the assistant, was told Z"),
    ("speculative", "discard",
     "a guess or hedge: may want, might be interested, probably, seems to"),
    ("outside_content", "discard",
     "content from a web page, email, document, search result or the news that "
     "isn't about the user"),
    ("test_artifact", "discard",
     "from testing the assistant, a benchmark, or made-up test data"),
    ("too_vague", "discard",
     "no specific content anyone could use"),
    ("other", "other",
     "fits none of the kinds above (the reason says what it is)"),
    ("in_document", "pointer",
     "a detail kept in one of the user's documents or notes"),
]

KIND_NAMES = [k for k, _, _ in KINDS]
TIER = {k: t for k, t, _ in KINDS}
TIERS = ["semantic", "episodic", "pointer", "discard", "other"]


# Kinds close enough that picking one for the other earns half credit when
# scoring a classifier against the labels: a fact often fits both.
SIMILAR = {frozenset(p) for p in [
    ("identity", "reference"), ("identity", "relationship"),
    ("relationship", "reference"), ("identity", "preference"),
    ("preference", "ongoing"), ("ongoing", "upcoming"), ("ongoing", "work_log"),
    ("upcoming", "one_off_event"), ("one_off_event", "work_log"),
    ("one_off_event", "transient_state"), ("one_off_event", "conversation_meta"),
    ("reference", "in_document"), ("outside_content", "in_document"),
    ("assistant_said", "conversation_meta"), ("conversation_meta", "too_vague"),
    ("speculative", "too_vague"), ("conversation_meta", "test_artifact"),
]}


def credit(a: str, b: str) -> float:
    """1 for the same kind, 0.5 for similar kinds, else 0."""
    if a == b:
        return 1.0
    return 0.5 if frozenset((a, b)) in SIMILAR else 0.0


def kinds_text() -> str:
    return "\n".join(f"- {k}: {d}" for k, _, d in KINDS)


SYSTEM = """You review the long-term memory of a personal voice assistant named {assistant}.
After each conversation the assistant stores short facts about its user.  Some
are lasting facts about the user, some record what happened, and some are text
that came from somewhere else.

Today is {today}.  Each fact shows the date it was stored.  Judge each fact as
of the day it was stored.  Use today's date only to tell which dates are already
past, never to guess at the current year.

Name each fact's kind:

{kinds}

Pick the kind that describes what the fact is.  A plan of the user's on a known
date is upcoming, even when that date is soon or already past.  When two kinds
fit, pick the more specific one.

Reply with JSON only: an array with one object per fact, in order:
[{{"id": <id>, "kind": "<kind>", "reason": "<under 15 words>"}}]
"kind" is one of these names exactly: {names}."""


def system_prompt(assistant: str, today: str) -> str:
    return SYSTEM.format(assistant=assistant, today=today, kinds=kinds_text(),
                         names=", ".join(KIND_NAMES))


# Tier first: the model answers the questions the tier depends on, in order,
# then names the kind as a secondary label.
TIER_SYSTEM = """You review the long-term memory of a personal voice assistant named {assistant}.
After each conversation the assistant stores short facts about its user.  Some
are lasting facts about the user, some record what happened, and some are text
that came from somewhere else.

Today is {today}.  Each fact shows the date it was stored.  Judge each fact as
of the day it was stored.  Use today's date only to tell which dates are already
past, never to guess at the current year.

Decide where each fact belongs by asking these questions in order and stopping
at the first yes:

1. discard: Is it not really about the user's life?  That is: the assistant's
   own words or actions, content from a web page, email, document or the news,
   talk about the conversation itself (the user asked, wanted to know, was
   told), testing the assistant, a guess or hedge, or too vague to use.
2. pointer: Is it a detail kept in one of the user's documents or notes, better
   looked up there than remembered?
3. semantic: Read without its date, would it still be true and useful a month
   after it was stored?  Who the user is, people and pets in their life, standing
   preferences, things they own, projects and routines that last, and a plan on
   a known date (a trip, an appointment) all count.  A state that changes on its
   own (a device, the weather, a reading, what is running or broken right now)
   does not, even when it looks like a stable detail.
4. episodic: Is it what happened, or how things were, at a moment?  A one-time
   event, a step in a task, a state at the time.  These are kept with their date
   as context.
5. other: none of the above (the reason says what it is).

Then name the fact's kind from this list:

{kinds}

Reply with JSON only: an array with one object per fact, in order:
[{{"id": <id>, "tier": "<tier>", "kind": "<kind>", "reason": "<under 15 words>"}}]
"tier" is one of: {tiers}.  "kind" is one of these names exactly: {names}."""


def tier_system_prompt(assistant: str, today: str) -> str:
    return TIER_SYSTEM.format(assistant=assistant, today=today, kinds=kinds_text(),
                              tiers=", ".join(TIERS), names=", ".join(KIND_NAMES))


def batch_prompt(facts) -> str:
    """The user message for one batch: id, date, category and text per fact."""
    lines = ["Facts:"]
    for f in facts:
        lines.append(f"[{f['id']}] ({f['date']}, {f['category']}) {f['text']}")
    return "\n".join(lines)
