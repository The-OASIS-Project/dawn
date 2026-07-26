#!/bin/bash
#
# Verifies that every section defined in SETTINGS_SCHEMA is listed in
# SECTION_CATEGORIES, in www/js/ui/settings/schema.js.
#
# Why this exists: SECTION_CATEGORIES is the render gate.  Its own comment says
# "Sections not listed here will not be rendered" — so a section can be wired
# through all nine server-side + client-side touchpoints in
# docs/CONFIGURATION_GUIDE.md, parse correctly, round-trip correctly, and STILL
# be completely unreachable in the UI because nobody added its name to a
# category list.  Nothing fails to build and no test goes red; the settings just
# aren't there.
#
# This shipped twice before the guard existed: `[attention]` (SAGE watches) and
# `[jobs]` (background jobs) were both fully wired and both invisible.  Found
# only when a user went looking for a panel that should have been there.
#
# Same failure mode, and the same fix, as the config_write_toml gap that
# tests/test_config_roundtrip.c guards on the server side.
#
# Wired into ctest -L ci via tests/CMakeLists.txt as
# `check_settings_sections_rendered`.
#

set -e
set -u
# pipefail so a grep/sed failure inside an extraction pipe surfaces instead of
# yielding an empty list that silently "passes".
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SCHEMA_JS="${REPO_ROOT}/www/js/ui/settings/schema.js"

if [ ! -f "${SCHEMA_JS}" ]; then
   echo "ERROR: schema not found: ${SCHEMA_JS}" >&2
   exit 2
fi

# Section keys: top-level entries of SETTINGS_SCHEMA, at exactly 6 spaces of
# indent (categories and field definitions sit deeper or shallower).
DEFINED="$(grep -oE '^      [a-z_0-9]+: \{' "${SCHEMA_JS}" | sed 's/[ :{]//g' | sort -u)"

# Names quoted inside the SECTION_CATEGORIES array.  This also picks up each
# category's own `id`, which is harmless: we only test that every DEFINED
# section appears, never the reverse.
RENDERED="$(sed -n '/SECTION_CATEGORIES = \[/,/^   \];/p' "${SCHEMA_JS}" |
   grep -oE "'[a-z_0-9]+'" | tr -d "'" | sort -u)"

if [ -z "${DEFINED}" ]; then
   echo "ERROR: extracted no sections from SETTINGS_SCHEMA — has the file's shape changed?" >&2
   exit 2
fi
if [ -z "${RENDERED}" ]; then
   echo "ERROR: extracted no names from SECTION_CATEGORIES — has the file's shape changed?" >&2
   exit 2
fi

ORPHANS="$(comm -23 <(echo "${DEFINED}") <(echo "${RENDERED}"))"

if [ -n "${ORPHANS}" ]; then
   echo "FAIL: settings section(s) defined in SETTINGS_SCHEMA but in NO category," >&2
   echo "      so their panel never renders and the settings are unreachable:" >&2
   echo "" >&2
   for s in ${ORPHANS}; do
      echo "        - ${s}" >&2
   done
   echo "" >&2
   echo "  Add each to a category's \`sections: [...]\` in SECTION_CATEGORIES" >&2
   echo "  (${SCHEMA_JS})." >&2
   echo "  See docs/CONFIGURATION_GUIDE.md, 'Adding a whole new section'." >&2
   exit 1
fi

echo "PASS: all $(echo "${DEFINED}" | wc -l) settings sections are assigned to a category"
exit 0
