#!/usr/bin/env bash

###############################################################################
# check_message_kind_confined.sh - CI invariant for request-context rows
#
# A row of `messages` with a `kind` (turn context, memory, directives,
# instructions, loop notes, envelopes) is request context: a model reads it on
# every later request, and nothing else does -- no client, search, export,
# count or memory extraction.  See include/core/message_kind.h.
#
# RULES:
#   1. Every SQL read of `messages` (FROM / JOIN messages) outside the schema
#      migrations names `kind` (a `kind IS NULL` filter, or the replay read
#      selecting it), or says why it reads every row with a "kind-rows:"
#      comment within the four lines above it (garbage collection, an
#      ownership count, change detection).
#   2. The in-memory kind key (MESSAGE_KIND_KEY, "_kind") is used only by the
#      kind helpers and the code that saves or copies histories; everything
#      else asks llm_history_kind_of() / llm_history_is_context().
#   3. Only core/prefix_message.c makes a conversation's frozen prefix message
#      (marks a message MESSAGE_KIND_PREFIX): one shape, for a turn freezing a
#      prompt and a load installing the stored one alike.
#
# Usage:   ./scripts/check_message_kind_confined.sh
# Exit:    0 = clean, 1 = violation found
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

violations=0

report() {
   local rule="$1" file="$2" out="$3"
   echo "VIOLATION ($rule) in $file:"
   printf '%s\n' "$out"
   violations=$((violations + 1))
}

# Rule 1: reads of messages name the kind, or say why not.
while IFS= read -r f; do
   out="$(awk '
      { lines[NR] = $0 }
      END {
         for (n = 1; n <= NR; n++) {
            if (lines[n] !~ /"[^"]*(FROM|JOIN)[[:space:]]+messages([^_a-z]|$)/) continue;
            stmt = "";
            for (k = n - 3; k <= n + 6; k++) if (k > 0 && k <= NR) stmt = stmt " " lines[k];
            if (stmt ~ /(^|[^a-z_.])kind([^a-z_]|$)|m\.kind([^a-z_]|$)/) continue;
            ok = 0;
            for (k = n - 4; k <= n; k++) if (k > 0 && lines[k] ~ /kind-rows:/) ok = 1;
            if (!ok) print "  " n ": " lines[n];
         }
      }' "$f")"
   if [ -n "$out" ]; then
      report "read of messages that neither filters kind nor says kind-rows:" "$f" "$out"
   fi
done < <(find src benchmarks -type f \( -name '*.c' -o -name '*.cpp' \) \
            ! -name 'auth_db_migrations*.c' -print0 |
         xargs -0 grep -lE '(FROM|JOIN)[[:space:]]+messages' | sort)

# Rule 2: the in-memory key stays with its owners.
allow='include/core/message_kind\.h|include/llm/llm_history_kind\.h|include/llm/llm_history_rows\.h|src/llm/llm_history_kind\.c|src/llm/llm_history_rows\.c|src/llm/llm_turn_blocks\.c|src/core/session_voice_save\.c|src/core/session_prefix\.c'
hits="$(grep -rnE 'MESSAGE_KIND_KEY|"_kind"' src include | grep -vE "^(${allow}):" || true)"
if [ -n "$hits" ]; then
   for f in $(printf '%s\n' "$hits" | cut -d: -f1 | sort -u); do
      report "kind key outside the kind helpers" "$f" \
         "$(printf '%s\n' "$hits" | awk -v f="$f:" 'index($0, f) == 1 { print "  " substr($0, length(f) + 1) }')"
   done
fi

# Rule 3: one maker of the frozen prefix message.
hits="$(grep -rnE 'set_kind\([^)]*MESSAGE_KIND_PREFIX|system_message\([^)]*MESSAGE_KIND_PREFIX' src |
        grep -v '^src/core/prefix_message\.c:' || true)"
if [ -n "$hits" ]; then
   report "frozen prefix message made outside core/prefix_message.c" \
      "$(printf '%s\n' "$hits" | cut -d: -f1 | sort -u | tr '\n' ' ')" "$(printf '  %s\n' "$hits")"
fi

if [ "$violations" -gt 0 ]; then
   echo ""
   echo "check_message_kind_confined: FAILED -- $violations violation(s).  Rows with a kind"
   echo "are request context: filter them out (kind IS NULL) of anything but the replay"
   echo "read, and test for them with llm_history_is_context()."
   exit 1
fi

echo "check_message_kind_confined: OK -- request-context rows confined to the replay path."
exit 0
