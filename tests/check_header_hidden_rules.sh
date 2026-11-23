#!/bin/bash
#
# Verifies that every element in the WebUI header (<header id="header"> in
# www/index.html) that starts out with the `hidden` class has a CSS rule that
# styles it: `#<id>.hidden` or `.<one of its classes>.hidden` in www/css.
#
# Why this exists: the WebUI has no global `.hidden { display: none }` utility.
# Header buttons start as class="hidden" and are revealed by JS once the user
# is logged in; without a matching `.hidden` rule the class styles nothing, so
# the button shows to a logged-out visitor and the auth gate silently does
# nothing.  #memory-btn, #scheduler-btn and #history-btn all shipped that way.
#
# Wired into ctest -L ci via tests/CMakeLists.txt as `check_header_hidden_rules`.
#

set -e
set -u
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
INDEX_HTML="${REPO_ROOT}/www/index.html"
CSS_DIR="${REPO_ROOT}/www/css"

if [ ! -f "${INDEX_HTML}" ] || [ ! -d "${CSS_DIR}" ]; then
   echo "ERROR: ${INDEX_HTML} or ${CSS_DIR} not found" >&2
   exit 2
fi

# One line per hidden-at-load header element: "<id> <class> <class> ...".
# Tags span several lines (Prettier), so read the header as one string.
ELEMENTS="$(sed -n '/<header id="header">/,/<\/header>/p' "${INDEX_HTML}" | perl -0777 -ne '
   while (/<[a-z][a-z0-9-]*\b([^>]*)>/g) {
      my $a = $1;
      next unless $a =~ /\bid="([^"]+)"/;
      my $id = $1;
      next unless $a =~ /\bclass="([^"]*)"/;
      my @c = grep { $_ ne "hidden" } split " ", $1;
      next if scalar(@c) == scalar(split " ", $1);
      print join(" ", $id, @c), "\n";
   }')"

if [ -z "${ELEMENTS}" ]; then
   echo "ERROR: found no hidden header elements in ${INDEX_HTML} — has the file's shape changed?" >&2
   exit 2
fi

# Comments out first: a rule named in a comment is not a rule.
CSS="$(cat "${CSS_DIR}"/*.css "${CSS_DIR}"/*/*.css 2>/dev/null | perl -0777 -pe 's{/\*.*?\*/}{}gs')"
MISSING=""
COUNT=0
while read -r id classes; do
   COUNT=$((COUNT + 1))
   found=0
   for sel in "#${id}" $(for c in ${classes}; do echo ".${c}"; done); do
      if grep -qF -- "${sel}.hidden" <<<"${CSS}"; then
         found=1
         break
      fi
   done
   if [ "${found}" -eq 0 ]; then
      MISSING="${MISSING} ${id}"
   fi
done <<<"${ELEMENTS}"

if [ -n "${MISSING}" ]; then
   echo "FAIL: header element(s) start as class=\"hidden\" but no CSS rule hides them," >&2
   echo "      so they show regardless of the JS that toggles the class:" >&2
   echo "" >&2
   for id in ${MISSING}; do
      echo "        - #${id}  (add \`#${id}.hidden { display: none; }\` beside its rule)" >&2
   done
   exit 1
fi

echo "PASS: all ${COUNT} hidden-at-load header elements have a .hidden rule"
exit 0
