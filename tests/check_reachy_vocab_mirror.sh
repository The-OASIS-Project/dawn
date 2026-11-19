#!/bin/bash
#
# Verifies that DAWN's expression vocabulary and the Reachy Mini bridge's move
# map name the same expressions:
#
#   C    : include/core/text_filter.h       EMOTE_FEELINGS + EMOTE_ACTS X-macros
#   TOML : services/reachy-bridge/reachy-bridge.toml   [expressions] keys
#
# The bridge is Python and never sees the C header, so it can't check this
# itself.  A name DAWN may send with no move does nothing on the robot; a move
# map entry DAWN never sends is dead config.
#
# Wired into ctest -L ci via tests/CMakeLists.txt as `check_reachy_vocab_mirror`.
#

set -e
set -u
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
HEADER="${REPO_ROOT}/include/core/text_filter.h"
TOML="${REPO_ROOT}/services/reachy-bridge/reachy-bridge.toml"

# The X(name) entries of the two vocabulary macros (each a backslash-continued block).
c_names=$(awk '
   /^#define EMOTE_(FEELINGS|ACTS)\(X\)/ { inblock = 1 }
   inblock {
      while (match($0, /X\([a-z_-]+\)/)) {
         print substr($0, RSTART + 2, RLENGTH - 3)
         $0 = substr($0, RSTART + RLENGTH)
      }
      if ($0 !~ /\\[[:space:]]*$/) inblock = 0
   }
' "$HEADER" | sort)

# The keys of the [expressions] table, up to the next table.
toml_names=$(awk '
   /^\[expressions\]/ { intable = 1; next }
   /^\[/ { intable = 0 }
   intable && /^[a-z_-]+[[:space:]]*=/ { sub(/[[:space:]]*=.*/, ""); print }
' "$TOML" | sort)

# The names DAWN sends are EMOTE_NAMES: it must be exactly the two macros read
# above, or a third group would go unchecked.
if ! grep -qE '^#define EMOTE_NAMES\(X\) EMOTE_FEELINGS\(X\) EMOTE_ACTS\(X\)[[:space:]]*$' "$HEADER"; then
   echo "FAIL: EMOTE_NAMES is no longer EMOTE_FEELINGS(X) EMOTE_ACTS(X); update this check"
   exit 1
fi

if [ -z "$c_names" ] || [ -z "$toml_names" ]; then
   echo "FAIL: could not read the vocabulary (C: $(echo "$c_names" | wc -w) names, TOML: $(echo "$toml_names" | wc -w))"
   exit 1
fi

if [ "$c_names" != "$toml_names" ]; then
   echo "FAIL: expression names differ between text_filter.h and reachy-bridge.toml"
   diff <(echo "$c_names") <(echo "$toml_names") | sed 's/^</  only in C:   /; s/^>/  only in TOML:/' | grep -v '^---\|^[0-9]'
   exit 1
fi

echo "OK: $(echo "$c_names" | wc -l) expression names match"
