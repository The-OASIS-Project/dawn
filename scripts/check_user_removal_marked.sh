#!/usr/bin/env bash

###############################################################################
# check_user_removal_marked.sh - CI invariant for a user's removals
#
# What a user removes (a forgotten memory, a deleted document, a deleted
# account) is withdrawn from the conversations it was sent into.  The database
# records a deleted item for that only while the delete is marked as the
# user's removal (conv_db_withdraw_intent_begin / _end, auth_db_withdraw.h):
# the same delete functions also serve maintenance (decay, dedup rollback,
# re-indexing), which must record nothing.
#
# RULES, everywhere in src/ except the modules that implement these deletes:
#   1. Every call to a delete function below sits between
#      conv_db_withdraw_intent_begin and conv_db_withdraw_intent_end in the
#      same function, or says why it isn't a removal with a "not-a-removal:"
#      comment on its line or the one above.
#   2. A function that marks a removal also calls session_withdraw_forgotten
#      (or its _async form): marking records the removal, the call withdraws it.
# Comments are ignored, except for the "not-a-removal:" marker.
#
# Usage:   ./scripts/check_user_removal_marked.sh
# Exit:    0 = clean, 1 = violation found
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

DELETES='memory_db_fact_delete|memory_db_summary_delete|memory_db_entity_delete|memory_db_relation_delete|memory_db_pref_delete|memory_db_delete_user_memories|memory_db_facts_delete_by_patterns|memory_db_conversations_forget|memory_db_admin_reset_derived|memory_forget_conversation|document_db_delete_indexed|document_db_delete|memory_note_bridge_delete_gloss|auth_db_delete_user'

# The modules that implement the deletes are the deletes, not their callers.
# CHECK_USER_REMOVAL_FILES (space-separated) checks those files instead: for
# testing the guard itself.
FILES=${CHECK_USER_REMOVAL_FILES:-$(find src -name '*.c' ! -name 'memory_db*.c' \
   ! -name 'document_db*.c' ! -name 'auth_db*.c' ! -name 'memory_forget.c' \
   ! -name 'memory_note_bridge.c' | sort)}

violations=0
for f in $FILES; do
   [ -f "$f" ] || continue
   out=$(awk -v re="(^|[^A-Za-z0-9_])(${DELETES})[(]" '
      function flush() {
         if (fn != "" && intent && !withdraw) {
            printf "  %d: %s  (marks a removal, never withdraws it)\n", fn_line, fn
         }
      }
      {
         raw = $0
         line = $0
         # Comments are not code: a block comment spanning lines, then any on this one.
         if (in_comment) {
            if (index(line, "*/")) { line = substr(line, index(line, "*/") + 2); in_comment = 0 }
            else { line = "" }
         }
         while (match(line, /\/\*/)) {
            rest = substr(line, RSTART + 2)
            if (index(rest, "*/")) { line = substr(line, 1, RSTART - 1) substr(rest, index(rest, "*/") + 2) }
            else { line = substr(line, 1, RSTART - 1); in_comment = 1 }
         }
         sub(/\/\/.*/, "", line)
      }
      # A function starts at a line opening in column 0 with a type and "(".
      line ~ /^[A-Za-z_].*\(/ && line !~ /;[[:space:]]*$/ {
         flush(); fn = raw; fn_line = NR; marked = 0; intent = 0; withdraw = 0
      }
      line ~ /conv_db_withdraw_intent_begin\(/ { marked = 1; intent = 1 }
      line ~ /conv_db_withdraw_intent_end\(/ { marked = 0 }
      line ~ /session_withdraw_forgotten(_async)?\(/ { withdraw = 1 }
      {
         if (line ~ re && line !~ /^[A-Za-z_]/ && !marked && raw !~ /not-a-removal:/ &&
             prev !~ /not-a-removal:/) {
            printf "  %d: %s\n", NR, raw
         }
         prev = raw
      }
      END { flush() }' "$f")
   if [ -n "$out" ]; then
      echo "VIOLATION (a user-facing delete not marked as a removal) in $f:"
      printf '%s\n' "$out"
      violations=$((violations + 1))
   fi
done

if [ "$violations" -gt 0 ]; then
   echo
   echo "check_user_removal_marked: FAILED -- $violations file(s).  Wrap a user's removal in"
   echo "conv_db_withdraw_intent_begin/_end (then session_withdraw_forgotten[_async]), or mark a"
   echo "maintenance delete with a \"not-a-removal:\" comment saying why."
   exit 1
fi
echo "check_user_removal_marked: OK"
