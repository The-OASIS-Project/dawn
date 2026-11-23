#!/usr/bin/env bash

###############################################################################
# check_tool_error_mark_test.sh - scripts/check_tool_error_mark.sh on fixtures
#
# The checker must refuse an unmarked "Error" literal returned through strdup()
# or make_response(), on one line or across two, and pass a marked one, one in
# a comment and an "Error" that is not a returned literal.
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CHECK="$REPO_ROOT/scripts/check_tool_error_mark.sh"
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT

cat >"$DIR/good.c" <<'EOF'
static char *a(void) { return strdup(TOOL_RESULT_ERROR_MARK "Error: marked"); }
static char *b(void) {
   return make_response(TOOL_RESULT_ERROR_MARK
                        "Error: marked across lines");
}
/* return strdup("Error: in a comment"); */
// return strdup("Error: in a line comment");
static char *c(void) { return strdup("Done."); }
static void d(char *buf) { snprintf(buf, 8, "Error"); }
EOF

cat >"$DIR/bad_one_line.c" <<'EOF'
static char *a(void) { return strdup("Error: unmarked"); }
EOF

cat >"$DIR/bad_two_lines.c" <<'EOF'
static char *b(void) {
   return make_response(
       "Error: unmarked across lines");
}
EOF

# A "/*" inside a string literal opens no comment: the error after it is seen.
cat >"$DIR/bad_after_glob.c" <<'EOF'
static const char *desc = "Index the files under docs/*";
static char *c(void) { return strdup("Error: unmarked after a glob"); }
/* a real comment closes here */
EOF

fail=0
if ! CHECK_TOOL_ERROR_MARK_FILES="$DIR/good.c" "$CHECK" >/dev/null 2>&1; then
   echo "FAIL: marked results were refused"
   fail=1
fi
for f in bad_one_line bad_two_lines bad_after_glob; do
   if CHECK_TOOL_ERROR_MARK_FILES="$DIR/$f.c" "$CHECK" >/dev/null 2>&1; then
      echo "FAIL: $f.c passed"
      fail=1
   fi
done
out="$(CHECK_TOOL_ERROR_MARK_FILES="$DIR/bad_two_lines.c" "$CHECK" 2>/dev/null || true)"
if [[ "$out" != *"bad_two_lines.c:2:"* ]]; then
   echo "FAIL: line number not reported (got: $out)"
   fail=1
fi
if ! "$CHECK" >/dev/null; then
   echo "FAIL: the tree has unmarked error results"
   fail=1
fi

[ "$fail" -eq 0 ] && echo "check_tool_error_mark_test: OK"
exit "$fail"
