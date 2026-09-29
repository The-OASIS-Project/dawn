#!/usr/bin/env bash

###############################################################################
# check_llm_blocks_confined.sh - CI invariant for stored assistant turn blocks
#
# `messages.llm_blocks` holds an assistant turn as the model produced it,
# including reasoning a vendor signed or encrypted for itself.  It is read only
# to rebuild an LLM context replayed to that vendor, and must never reach a
# client, a search index, an export, or another model.
#
# RULES (C comments stripped first, so prose can't false-positive):
#   1. SQL naming llm_blocks (a string literal) appears only in the auth files
#      that own the column: the schema, its migrations (v92 on), the message
#      module.
#   2. The replay read, conv_db_get_messages_for_llm(), is called only by the
#      history loader.
#   3. The loader's replay entry points (memory_history_load_for_llm,
#      memory_history_load_rows) are called only by surfaces that replay a
#      context to the LLM.
#   4. The loader's temporary key (_blocks_raw) stays inside the loader.
#   5. An assistant row written through the legacy add functions, which can't
#      carry blocks, is DAWN-authored text (a briefing, a research report, a
#      channel post): the call says so with a "no-blocks:" comment within the
#      four lines above it.  A reply the model produced goes through
#      conv_db_add_row() with its blocks, and an assistant row built for it
#      sets .llm_blocks (or says no-blocks: why not).
#   6. The in-memory keys that hold blocks (_blocks on a history message,
#      _stored on a row object) are used only where turns are captured,
#      rendered, saved or reloaded; anything else goes through
#      llm_history_strip_internal / llm_history_wire_copy first.
#   7. No SELECT * over messages: it would carry llm_blocks along.
#
# Usage:   ./scripts/check_llm_blocks_confined.sh
# Exit:    0 = clean, 1 = violation found
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Strip C comments (multi-line /* */ and // line) from every file named after
# the output directory, in one pass, keeping line numbers: $1/<path> gets <path>
# stripped.  String and character literals are kept whole.
strip_all() {
   local outdir="$1"
   shift
   awk -v outdir="$outdir" '
      FNR == 1 {
         if (outfile != "") close(outfile);
         outfile = outdir "/" FILENAME; inblk = 0;
      }
      {
         line = $0;
         # Nothing that can open a comment or literal: the line as it is.
         if (!inblk && line !~ /[\/"\047]/) { print line > outfile; next }
         out = ""; i = 1; L = length(line); inq = 0;
         while (i <= L) {
            c1 = substr(line, i, 1);
            c2 = substr(line, i, 2);
            if (inq) {
               out = out c1;
               if (c1 == "\\") { out = out substr(line, i + 1, 1); i += 2; continue }
               if (c1 == "\"") inq = 0;
               i++;
            } else if (inblk) {
               if (c2 == "*/") { inblk = 0; i += 2 } else { i++ }
            } else if (c1 == "\047") {
               # a character literal: copied whole, never a string or comment start
               j = i + 1;
               if (substr(line, j, 1) == "\\") j++;
               j++;
               out = out substr(line, i, j - i + 1); i = j + 1;
            } else if (c1 == "\"") {
               inq = 1; out = out c1; i++;
            } else if (c2 == "/*") {
               inblk = 1; i += 2;
            } else if (c2 == "//") {
               break;
            } else {
               out = out c1; i++;
            }
         }
         print out > outfile;
      }' "$@"
}

violations=0

# Every source file, comments stripped, once: the rules below grep these copies.
STRIPPED="$(mktemp -d)"
trap 'rm -rf "$STRIPPED"' EXIT
mapfile -t FILES < <(find src include -type f \( -name '*.c' -o -name '*.cpp' -o -name '*.h' \) | sort)
printf '%s\n' "${FILES[@]}" | xargs -n1 dirname | sort -u | (cd "$STRIPPED" && xargs mkdir -p)
strip_all "$STRIPPED" "${FILES[@]}"

# check <rule> <regex> <allowlist regex over repo paths>
check() {
   local rule="$1" pattern="$2" allow="$3"
   local hits
   # One grep over the stripped tree; "path:line:text", paths relative to it.
   local rc=0
   hits="$(cd "$STRIPPED" && grep -rnE "$pattern" src include)" || rc=$?
   if [ "$rc" -gt 1 ]; then
      echo "check_llm_blocks_confined: grep failed on rule \"$rule\" (bad pattern?)" >&2
      exit 2
   fi
   [ -z "$hits" ] && return 0
   hits="$(printf '%s\n' "$hits" | grep -vE "^(${allow}):" || true)"
   [ -z "$hits" ] && return 0
   local f
   for f in $(printf '%s\n' "$hits" | cut -d: -f1 | sort -u); do
      echo "VIOLATION ($rule) in $f:"
      printf '%s\n' "$hits" | awk -v f="$f:" 'index($0, f) == 1 { print "  " substr($0, length(f) + 1) }'
      violations=$((violations + 1))
   done
}

check "SQL naming llm_blocks outside its owners" '"[^"]*llm_blocks' \
   'src/auth/auth_db_schema\.c|src/auth/auth_db_migrations_v9[2-9]\.c|src/auth/auth_db_messages\.c|include/auth/auth_db_internal\.h'

check "replay read outside the history loader" 'conv_db_get_messages_for_llm' \
   'src/auth/auth_db_messages\.c|include/auth/auth_db_messages\.h|src/memory/memory_history_loader\.c'

check "replay load outside a replaying surface" 'memory_history_load_(for_llm|rows)[[:space:]]*\(' \
   'src/memory/memory_history_loader\.c|include/memory/memory_history_loader\.h|src/webui/webui_restore\.c|src/core/job_worker\.c|src/core/job_reinvoke\.c|src/messaging/messaging_engine_session\.c'

check "loader key outside the loader" '_blocks_raw' 'src/memory/memory_history_loader\.c'

check "in-memory block keys outside the turn-block path" \
   'LLM_TURN_BLOCKS_KEY|LLM_HISTORY_ROW_STORED_KEY|"_blocks"|"_stored"' \
   'include/llm/llm_turn_blocks\.h|include/llm/llm_history_rows\.h|src/llm/llm_turn_blocks(_stored)?\.c|src/llm/llm_history_rows\.c|src/llm/llm_claude_format\.c|src/llm/llm_openai_history\.c|src/llm/llm_tool_loop\.c|src/memory/memory_history_loader\.c|src/webui/webui_restore\.c|src/core/session_history\.c|src/core/session_voice_save\.c'

check "SELECT * over messages" 'SELECT[^;]*([[:space:],]|[a-z]\.)\*[^;]*FROM[[:space:]]+messages([^_a-z]|$)' 'src/auth/auth_db_migrations\.c'

# Rule 5: legacy assistant writes need a no-blocks: reason.
while IFS= read -r f; do
   out="$(awk '
      { lines[NR] = $0 }
      END {
         for (n = 1; n <= NR; n++) {
            if (lines[n] !~ /conv_db_add_message[a-z_]*[[:space:]]*\(/) continue;
            if (lines[n] ~ /^[[:space:]]*(int|void|\*|\/\/)/) continue;
            call = lines[n] " " lines[n + 1] " " lines[n + 2];
            if (call !~ /"assistant"/) continue;
            ok = 0;
            for (k = n - 4; k <= n; k++) if (k > 0 && lines[k] ~ /no-blocks:/) ok = 1;
            if (!ok) print "  " n ": " lines[n];
         }
         for (n = 1; n <= NR; n++) {
            if (lines[n] !~ /\.role[[:space:]]*=[[:space:]]*"assistant"/) continue;
            ok = 0;
            for (k = n - 4; k <= n + 6; k++)
               if (k > 0 && (lines[k] ~ /\.llm_blocks[[:space:]]*=/ || lines[k] ~ /no-blocks:/)) ok = 1;
            if (!ok) print "  " n ": " lines[n];
         }
      }' "$f")"
   if [ -n "$out" ]; then
      echo "VIOLATION (assistant row without blocks or a no-blocks: reason) in $f:"
      printf '%s\n' "$out"
      violations=$((violations + 1))
   fi
done < <(find src -type f -name '*.c' ! -path 'src/auth/*' -print0 |
         xargs -0 grep -lE '"assistant"' | sort)

if [ "$violations" -gt 0 ]; then
   echo ""
   echo "check_llm_blocks_confined: FAILED -- $violations violation(s).  Stored turn blocks"
   echo "hold vendor-issued reasoning: read them only to rebuild an LLM context"
   echo "(memory_history_load_for_llm), and save a model's reply with conv_db_add_row()."
   exit 1
fi

echo "check_llm_blocks_confined: OK -- stored turn blocks confined to the replay path."
exit 0
