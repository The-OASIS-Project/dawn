#!/usr/bin/env bash

###############################################################################
# check_tool_error_mark.sh - CI invariant: a tool's error result carries the mark
#
# A tool result is an error only when it starts with TOOL_RESULT_ERROR_MARK
# (tool_registry.h).  An unmarked "Error: ..." counts as a success: is_error is
# false in the request, the tool-iteration UI and the plan executor show it as
# done, and a framed tool's error is framed as someone else's text.
#
# RULE, for every .c file in src/tools/ (comments ignored): a string literal
# starting with "Error passed as the first argument of strdup() or
# make_response() is preceded by TOOL_RESULT_ERROR_MARK.
#
# Not covered: an "Error ..." formatted into a buffer that is returned later
# (snprintf then strdup); mark those by hand.
#
# Usage:   ./scripts/check_tool_error_mark.sh
#          CHECK_TOOL_ERROR_MARK_FILES="a.c b.c" ./scripts/check_tool_error_mark.sh
# Exit:    0 = clean, 1 = violation found
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

FILES=${CHECK_TOOL_ERROR_MARK_FILES:-$(ls src/tools/*.c)}

# shellcheck disable=SC2086
if ! perl -0777 -ne '
   my $src = $_;
   # Blank comments out, keeping their newlines so line numbers stay right.
   # String and character literals are matched first and kept, so a "/*" or
   # "//" inside one (a glob, a URL) does not start a comment.
   $src =~ s{("(?:\\.|[^"\\\n])*"|\x27(?:\\.|[^\x27\\\n])*\x27)|/\*.*?\*/|//[^\n]*}{
      defined $1 ? $1 : do { my $c = $&; $c =~ s/[^\n]//g; $c }
   }gse;
   while ($src =~ /\b(strdup|make_response)\s*\(\s*"Error/g) {
      my $line = 1 + (substr($src, 0, $-[0]) =~ tr/\n//);
      print "$ARGV:$line: $1(\"Error...\") without TOOL_RESULT_ERROR_MARK\n";
      $bad = 1;
   }
   END { exit($bad ? 1 : 0) }
' $FILES; then
   echo "check_tool_error_mark: an error result without TOOL_RESULT_ERROR_MARK" \
      "counts as a success" >&2
   exit 1
fi
exit 0
