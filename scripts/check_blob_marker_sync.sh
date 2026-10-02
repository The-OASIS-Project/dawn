#!/usr/bin/env bash

###############################################################################
# check_blob_marker_sync.sh - CI invariant for the chat-attachment blob marker
#
# A document uploaded into a chat embeds its stored-original blob id in the
# message text as a marker:  "[ATTACHED DOCUMENT: name (NNN bytes) blob:<id>]".
# That exact "blob:<id>]" token is independently encoded in THREE places that
# must agree, across the C/JS boundary (so a single shared constant is not
# possible):
#
#   1. PRODUCER  src/webui/webui_attachments.c - builds " blob:<id>]" from a text
#                frame's attachments (clients without the document_attachments
#                flag still inline the same marker themselves)
#   2. PARSER    www/js/ui/documents.js  - DOC_MARKER_RE regex captures it back
#   3. SWEEP SQL src/auth/auth_db_statements.c - the orphan sweep's
#                "NOT EXISTS (... messages.content LIKE '%blob:'||id||']%')"
#                guard that keeps a chat-attached original from being reclaimed
#
# WHY THIS MATTERS (data loss): if the marker format drifts in ONE place — drop
# the trailing ']', rename "blob:" — the sweep's LIKE stops matching live chat
# attachments, and the maintenance thread reclaims still-referenced original
# files after the grace window.  There is no compile-time signal; the loss is
# silent.  This check fails CI if the marker token disappears from any of the
# three sites, forcing the change to be made in all three together.
#
# It is intentionally a presence check, not a format parser: it asserts the
# "blob:" token still appears in each site's marker context.  Touching the
# format means updating all three AND this script's expectations.
#
# Usage:   ./scripts/check_blob_marker_sync.sh
# Exit:    0 = all three in sync, 1 = marker missing from one or more sites
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# site|file|expected-token (a fixed grep -F substring that anchors the marker in
# that site's surrounding syntax — drift in any of them is what we want to catch).
CHECKS=(
   "producer|src/webui/webui_attachments.c| blob:%s]"
   "parser|www/js/ui/documents.js|blob:(blb_"
   "sweep-sql|src/auth/auth_db_statements.c|blob:' || b.id || ']"
)

missing=0
for entry in "${CHECKS[@]}"; do
   IFS='|' read -r site file token <<<"$entry"
   if [ ! -f "$file" ]; then
      echo "VIOLATION: $site source missing: $file"
      missing=$((missing + 1))
      continue
   fi
   if ! grep -qF -- "$token" "$file"; then
      echo "VIOLATION: $site ($file) no longer contains the blob marker token: '$token'"
      missing=$((missing + 1))
   fi
done

if [ "$missing" -gt 0 ]; then
   echo ""
   echo "check_blob_marker_sync: FAILED -- the chat-attachment 'blob:<id>]' marker is"
   echo "out of sync across producer / parser / orphan-sweep SQL.  All three must encode"
   echo "the SAME marker format, or the maintenance sweep silently reclaims still-attached"
   echo "original files (data loss).  Update all three sites (and this script) together."
   exit 1
fi

echo "check_blob_marker_sync: OK -- blob marker in sync across producer, parser, sweep SQL."
exit 0
