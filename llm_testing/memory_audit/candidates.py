# Memory audit: conversations worth turning into memory eval cases.
#
# Finds, per signal, the conversations where memory may have gone wrong, so the
# owner can open them and write a case for llm_testing/memory_eval/ himself:
#
#   forgetting          facts the user had forgotten (memory_forgotten)
#   staleness           facts filed as replaced by a newer value
#   correction          user turns right after a reply that read as a correction
#                       ("no,", "that's wrong", "actually", ...): staleness or
#                       an answer that agreed with something false
#   unused memory       turns that were given memories and cited none
#   dismissed alerts    proactive alerts the user dismissed (over-interruption)
#
# Privacy: prints counts and ids only, never a fact, a message or an alert.
# Text is matched inside SQLite and never read into Python.  The database is
# opened read-only.  A table an older database lacks is skipped with a note.
#
# License: GPLv3, same as DAWN.

import pathlib
import sqlite3

MEMORY_SUPERSEDED_REPLACED = 1  # memory_facts.superseded_kind (include/memory/memory_db.h)

# (cue, at the start only).  Lower-cased; matched against the trimmed message.
CORRECTION_CUES = [("no,", True), ("no.", True), ("nope", True), ("that's wrong", False),
                   ("that's not right", False), ("that is wrong", False), ("actually", False),
                   ("not anymore", False), ("i told you", False), ("not true", False)]

# attention_log.outcome values that mean the user turned an alert away.
DISMISSED_OUTCOMES = ("dismissed", "suppressed_by_user")


def _has_table(con, name):
    return con.execute("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?",
                       (name,)).fetchone() is not None


def _has_column(con, table, column):
    return any(r[1] == column for r in con.execute(f"PRAGMA table_info({table})"))


def _signal(name, rows=None, note=""):
    """rows: (id,) tuples, one per occurrence; ids are deduplicated for printing."""
    rows = rows or []
    ids = sorted({r[0] for r in rows if r[0]})
    return {"name": name, "count": len(rows), "ids": ids, "note": note}


def forgotten(con, user):
    if not _has_table(con, "memory_forgotten"):
        return _signal("forgetting", note="no memory_forgotten table (database before v110)")
    rows = con.execute(
        "SELECT (SELECT c.id FROM messages m JOIN conversations c ON c.id = m.conversation_id "
        "        WHERE m.id = f.forget_msg_id AND c.user_id = f.user_id) "
        "FROM memory_forgotten f WHERE f.user_id = ?", (user,)).fetchall()
    return _signal("forgetting", rows,
                   "ids: the conversation of the latest message at the forget, when it is "
                   "this user's (where the forget was said, if said in chat)")


def replaced(con, user):
    if not _has_column(con, "memory_facts", "superseded_kind"):
        return _signal("staleness: replaced facts",
                       note="no superseded_kind column (database before v104)")
    rows = con.execute(
        "SELECT n.source_conversation_id FROM memory_facts o "
        "LEFT JOIN memory_facts n ON n.id = o.superseded_by "
        "WHERE o.user_id = ? AND o.superseded_by IS NOT NULL AND o.superseded_kind = ?",
        (user, MEMORY_SUPERSEDED_REPLACED)).fetchall()
    return _signal("staleness: replaced facts", rows,
                   "ids: the conversation the newer value came from")


def corrections(con, user):
    plain = "m.kind IS NULL" if _has_column(con, "messages", "kind") else "1"
    prev_plain = plain.replace("m.", "p.")
    text = "lower(ltrim(m.content))"
    cues, args = [], []
    for cue, anchored in CORRECTION_CUES:
        cues.append(f"substr({text}, 1, ?) = ?" if anchored else f"instr({text}, ?) > 0")
        args.extend([len(cue), cue] if anchored else [cue])
    q_plain = plain.replace("m.", "q.")
    # The reply the user answered: the latest earlier user or assistant message.
    prev = ("(SELECT p.id FROM messages p WHERE p.conversation_id = m.conversation_id "
            f"AND p.id < m.id AND {prev_plain} AND p.role IN ('user', 'assistant') "
            "ORDER BY p.id DESC LIMIT 1)")
    # memory_citation.c files a reply's audit row under the user question it answered.
    question = ("(SELECT q.id FROM messages q WHERE q.conversation_id = m.conversation_id "
                f"AND q.id < {prev} AND {q_plain} AND q.role = 'user' "
                "ORDER BY q.id DESC LIMIT 1)")
    audit = _has_table(con, "memory_citation_audit")
    with_memory = ("EXISTS (SELECT 1 FROM memory_citation_audit a WHERE "
                   f"a.message_id = {question} AND a.conversation_id = m.conversation_id "
                   "AND a.user_id = c.user_id)") if audit else "0"
    rows = con.execute(
        f"SELECT m.conversation_id, {with_memory} FROM messages m "
        "JOIN conversations c ON c.id = m.conversation_id "
        f"WHERE c.user_id = ? AND m.role = 'user' AND {plain} AND ({' OR '.join(cues)}) "
        f"AND (SELECT r.role FROM messages r WHERE r.id = {prev}) = 'assistant'",
        (user, *args)).fetchall()
    note = (f"{sum(1 for r in rows if r[1])} after a reply that was given memories"
            if audit else "no memory_citation_audit table: memory-bearing replies unknown")
    return _signal("staleness/sycophancy: correction after a reply", rows, note)


def unused_memory(con, user):
    if not _has_table(con, "memory_citation_audit"):
        return _signal("injected, none cited",
                       note="no memory_citation_audit table (database before v77)")
    rows = con.execute(
        "SELECT conversation_id FROM memory_citation_audit WHERE user_id = ? "
        "AND COALESCE(injected_ids, '') != '' AND COALESCE(cited_ids, '') = ''",
        (user,)).fetchall()
    return _signal("injected, none cited", rows)


def dismissed_alerts(con, user):
    if not _has_table(con, "attention_log"):
        return _signal("over-interruption: dismissed alerts",
                       note="no attention_log table (database before v71)")
    marks = ",".join("?" * len(DISMISSED_OUTCOMES))
    rows = con.execute(f"SELECT id FROM attention_log WHERE user_id = ? AND outcome IN ({marks})",
                       (user, *DISMISSED_OUTCOMES)).fetchall()
    delivered = con.execute("SELECT COUNT(*) FROM attention_log WHERE user_id = ? "
                            "AND delivered_at IS NOT NULL", (user,)).fetchone()[0]
    return _signal("over-interruption: dismissed alerts", rows,
                   f"ids: attention_log rows (alerts have no conversation); {delivered} "
                   "delivered alerts in all, outcomes recorded only once outcome capture exists")


SIGNALS = (forgotten, replaced, corrections, unused_memory, dismissed_alerts)


def collect(db, user):
    con = sqlite3.connect(pathlib.Path(db).resolve().as_uri() + "?mode=ro", uri=True)
    try:
        return [s(con, user) for s in SIGNALS]
    finally:
        con.close()


def render(signals, max_ids):
    lines = []
    for s in signals:
        lines.append(f"{s['name']}: {s['count']}")
        if s["ids"]:
            shown = ", ".join(str(i) for i in s["ids"][:max_ids])
            more = len(s["ids"]) - max_ids
            lines.append(f"  ids ({len(s['ids'])}): {shown}"
                         + (f" ... +{more}" if more > 0 else ""))
        if s["note"]:
            lines.append(f"  {s['note']}")
    return "\n".join(lines)


def run(a):
    print(f"user {a.user_id}, {a.db} (read-only)")
    print(render(collect(a.db, a.user_id), a.max_ids))
