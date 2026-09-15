#!/usr/bin/env bash

###############################################################################
# check_no_raw_strncpy.sh - ratcheting guard for the safe_strscpy migration
#
# The project is standardizing bounded string copies on safe_strscpy() (arrays)
# and safe_strncpy() (pointer + explicit size); see
# docs/SAFE_STRNCPY_STANDARDIZATION_DESIGN.md. This guard keeps raw strncpy() /
# strncat() from creeping back into directories that have already been swept.
#
# It runs at -O0 on every dawn build (unlike -Wstringop-truncation, which only
# fires under optimization and so is dark in the Debug CI/pre-commit builds), so
# it is the real regression net for the migration.
#
# RATCHET: COVERED_DIRS lists the directories already converted. Add a directory
# to it in the same commit that finishes converting that directory. Anything not
# yet listed is not checked (still mid-migration).
#
# ESCAPE HATCH: a genuine, reviewed raw strncpy/strncat (e.g. a deliberate
# fixed-length substring copy where the 3rd arg is a copy length, not a buffer
# capacity) is allowed with a  /* strncpy-ok: <reason> */  marker on the same
# line OR the line immediately above (like NOLINTNEXTLINE).
#
# Usage:   ./scripts/check_no_raw_strncpy.sh
# Exit:    0 = clean, 1 = a raw strncpy/strncat found in a covered directory
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Directories swept onto safe_strscpy/safe_strncpy. Grows one entry per sweep
# commit. When every source directory is listed, the migration is complete.
COVERED_DIRS=(
   src
   dawn-admin
)

# Raw strncpy/strncat the migration replaces. safe_strncpy/safe_strscpy are the
# replacements, so the word boundary + trailing '(' avoids matching them.
# strncat is included: its bounded `sizeof(dst)-strlen(dst)-1` idiom trips GCC's
# -Wstringop-truncation under -O2 on x86-64 (an arch the Jetson can't reproduce
# locally, so it only surfaces in the docker CI gate). First-party code uses a
# bounded memcpy with an explicit length instead; a genuinely-needed raw call can
# still opt out with the marker below.
FORBIDDEN='(^|[^_[:alnum:]])strn(cpy|cat)[[:space:]]*\('

# Strip // line and /* */ block comments (preserving line numbers) so a prose
# mention can't false-positive; then drop any line carrying the strncpy-ok
# marker before it was stripped.
strip_comments() {
   awk '
      {
         line = $0; out = ""; i = 1; L = length(line); instr = 0; q = "";
         while (i <= L) {
            c1 = substr(line, i, 1); c2 = substr(line, i, 2);
            if (inblk) {
               if (c2 == "*/") { inblk = 0; i += 2 } else { i++ }
            } else if (instr) {
               # Inside a string/char literal: copy verbatim (so a "/*" in a
               # literal cannot open a spurious comment), honoring \-escapes.
               out = out c1;
               if (c1 == "\\") { out = out substr(line, i + 1, 1); i += 2 }
               else { if (c1 == q) instr = 0; i++ }
            } else if (c2 == "/*") {
               inblk = 1; i += 2;
            } else if (c2 == "//") {
               break;
            } else if (c1 == "\"" || c1 == "'\''") {
               q = c1; instr = 1; out = out c1; i++;
            } else {
               out = out c1; i++;
            }
         }
         print out;
      }' "$1"
}

violations=0
scanned=0
while IFS= read -r f; do
   scanned=$((scanned + 1))
   # Lines with an explicit reviewed escape hatch are exempt.
   hits="$(strip_comments "$f" | grep -nE "$FORBIDDEN" || true)"
   if [ -n "$hits" ]; then
      while IFS= read -r h; do
         ln="${h%%:*}"
         # Re-check for the escape-hatch marker. clang-format may wrap a long
         # strncpy() call over several lines, pushing the /* strncpy-ok */ marker onto
         # the closing-paren line below the strncpy( token, so scan the whole statement
         # span (the strncpy line through its ';').  A marker on the line immediately
         # ABOVE also counts (NOLINTNEXTLINE-style) — but ONLY when that line is not
         # itself a strncpy( call, so one marked call cannot exempt an adjacent unmarked
         # one on the next line.
         end=$(awk -v s="$ln" 'NR >= s && /;/ { print NR; exit }' "$f")
         [ -z "$end" ] && end=$((ln + 4))
         if sed -n "${ln},${end}p" "$f" | grep -qE 'strn(cpy|cat)-ok'; then
            continue
         fi
         if [ "$ln" -gt 1 ]; then
            above="$(sed -n "$((ln - 1))p" "$f")"
            if printf '%s' "$above" | grep -qE 'strn(cpy|cat)-ok' &&
               ! printf '%s' "$above" | grep -qE '(^|[^_[:alnum:]])strn(cpy|cat)[[:space:]]*\('; then
               continue
            fi
         fi
         echo "VIOLATION in $f:$h"
         violations=$((violations + 1))
      done <<< "$hits"
   fi
done < <(for d in "${COVERED_DIRS[@]}"; do
   find "$d" -type f \( -name '*.c' -o -name '*.cpp' -o -name '*.h' \) 2>/dev/null
done)

if [ "$violations" -gt 0 ]; then
   echo ""
   echo "check_no_raw_strncpy: FAILED -- $violations raw strncpy/strncat call(s) in a"
   echo "swept directory. Use safe_strscpy(dst, src) for a fixed-size array, or"
   echo "safe_strncpy(dst, src, size) for a pointer with a known capacity. For a bounded"
   echo "append, use a length-checked memcpy (strncat's sizeof-strlen-1 idiom trips"
   echo "-Wstringop-truncation at -O2). A genuine fixed-length copy may be kept with a"
   echo "/* strncpy-ok: <reason> */ (or strncat-ok) marker on the same line."
   exit 1
fi

echo "check_no_raw_strncpy: OK -- $scanned file(s) scanned across ${#COVERED_DIRS[@]} swept dir(s), no raw strncpy."
exit 0
