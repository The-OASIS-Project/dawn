#!/usr/bin/env bash

###############################################################################
# check_tool_action_kinds.sh - CI invariant for tools' kinds of action
#
# A tool's action_kinds table (tool_action_kind_entry_t, tool_registry.h) is
# checked when the tool registers, and a tool that fails is refused: the daemon
# starts without it.  This catches the mistake at build time instead.
#
# RULES, for every tool_action_kind_entry_t table in src/tools/:
#   1. Every listed action is one of the file's action enum values (a param
#      with .maps_to = TOOL_MAPS_TO_ACTION).
#   2. No action is listed twice.
#   3. A TOOL_KIND_PREPARE entry names a confirm that the same table lists as
#      TOOL_KIND_ACT.
#   4. Any other entry names no confirm (NULL).
#   5. Every row is written { "action", TOOL_KIND_X, NULL | "confirm" }, with
#      an optional fourth field NULL | TOOL_FRAME_X (a row in any other form is
#      reported, not skipped).
#   6. No tool sets .default_kind = TOOL_KIND_PREPARE, and every metadata that
#      sets .action_kinds also sets .action_kind_count.
#   7. Every tool says whose text its results are (.results =
#      TOOL_RESULTS_OWN or TOOL_RESULTS_OUTSIDE); a row may add a fifth field,
#      TOOL_RESULTS_X, after its frame.
#
# Usage:   ./scripts/check_tool_action_kinds.sh
# Exit:    0 = clean, 1 = violation found
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

FILES=${CHECK_TOOL_KINDS_FILES:-$(grep -rlE --include='*.c' \
   'tool_action_kind_entry_t|tool_metadata_t|\.default_kind|\.action_kinds' src || true)}

violations=0
for f in $FILES; do
   if ! perl -0777 -ne '
      my $src = $_;
      $src =~ s{/\*.*?\*/}{}gs;
      $src =~ s{//[^\n]*}{}g;
      my %enum;
      # A parameter ends at a line holding only its closing brace; its enum
      # list closes on a line of its own values.
      for my $param (split /\n[ \t]*\},?[ \t]*(?=\n)/, $src) {
         next unless $param =~ /\.maps_to\s*=\s*TOOL_MAPS_TO_ACTION\b/;
         next unless $param =~ /\.enum_values\s*=\s*\{([^}]*)\}/s;
         my $list = $1;
         $enum{$_} = 1 for ($list =~ /"([^"]+)"/g);
      }
      my $bad = 0;
      if ($src =~ /\.default_kind\s*=\s*TOOL_KIND_PREPARE\b/) {
         print "$ARGV: default_kind can\x27t be TOOL_KIND_PREPARE\n"; $bad = 1;
      }
      while ($src =~ /tool_metadata_t\s+(\w+)\s*=\s*\{(.*?)\n\};/gs) {
         my ($meta, $body) = ($1, $2);
         if ($body =~ /\.action_kinds\s*=/ && $body !~ /\.action_kind_count\s*=/) {
            print "$ARGV: $meta sets .action_kinds without .action_kind_count\n"; $bad = 1;
         }
         if ($body !~ /\.results\s*=\s*TOOL_RESULTS_(OWN|OUTSIDE)\b/) {
            print "$ARGV: $meta doesn\x27t say whose text its results are (.results)\n"; $bad = 1;
         }
      }
      while ($src =~ /tool_action_kind_entry_t\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\n\};/gs) {
         my ($name, $body) = ($1, $2);
         my $written = () = $body =~ /\{/g;
         my (%kind, @rows);
         while ($body =~ /\{\s*"([^"]+)"\s*,\s*TOOL_KIND_(\w+)\s*,\s*(NULL|"([^"]*)")\s*(?:,\s*(?:NULL|TOOL_FRAME_\w+)\s*(?:,\s*TOOL_RESULTS_\w+\s*)?)?\}/g) {
            my ($a, $k, $c) = ($1, $2, defined $4 ? $4 : undef);
            if (exists $kind{$a}) { print "$ARGV: $name lists \x27$a\x27 twice\n"; $bad = 1; }
            $kind{$a} = $k;
            push @rows, [$a, $k, $c];
         }
         if (@rows != $written) {
            print "$ARGV: $name: " . ($written - @rows) . " row(s) not written as { \"action\", TOOL_KIND_X, NULL | \"confirm\"[, NULL | TOOL_FRAME_X[, TOOL_RESULTS_X]] }\n";
            $bad = 1;
         }
         for my $r (@rows) {
            my ($a, $k, $c) = @$r;
            if (!$enum{$a}) { print "$ARGV: $name: \x27$a\x27 is not one of the actions\n"; $bad = 1; }
            if ($k eq "PREPARE") {
               if (!defined $c || !exists $kind{$c} || $kind{$c} ne "ACT") {
                  print "$ARGV: $name: prepare \x27$a\x27 needs a listed confirm that acts\n"; $bad = 1;
               }
            } elsif (defined $c) {
               print "$ARGV: $name: \x27$a\x27 names a confirm but is not PREPARE\n"; $bad = 1;
            }
         }
      }
      exit $bad;
   ' "$f"; then
      violations=$((violations + 1))
   fi
done

# Rule 8: the tools that declare TOOL_RESULTS_OWN are exactly the names in
# include/tools/tool_own_results.h (the list code without the registry reads),
# and its exception pair is exactly the one action whose results differ from
# its tool's.
if [ -z "${CHECK_TOOL_KINDS_FILES:-}" ]; then
   declared="$(perl -0777 -ne '
      my $src = $_;
      $src =~ s{/\*.*?\*/}{}gs;
      while ($src =~ /tool_metadata_t\s+\w+\s*=\s*\{(.*?)\n\};/gs) {
         my $body = $1;
         next unless $body =~ /\.results\s*=\s*TOOL_RESULTS_OWN\b/;
         print "$1\n" if $body =~ /\.name\s*=\s*"([^"]+)"/;
      }' src/tools/*.c | sort -u)"
   listed="$(perl -0777 -ne '
      if (/#define TOOL_OWN_RESULTS_NAMES((?:[^\n]*\\\n)*[^\n]*)\n/) {
         my $l = $1;
         print "$1\n" while $l =~ /"([a-z_]+)"/g;
      }' include/tools/tool_own_results.h | sort -u)"
   if [ "$declared" != "$listed" ]; then
      echo "include/tools/tool_own_results.h doesn't list exactly the tools declaring TOOL_RESULTS_OWN:"
      diff <(printf '%s\n' "$listed") <(printf '%s\n' "$declared") | sed 's/^/  /'
      violations=$((violations + 1))
   fi
   # An action whose results differ from its tool's is the header's one
   # exception pair (TOOL_OWN_RESULTS_EXCEPT_TOOL / _ACTION); an action
   # declaring OWN under a tool that is OUTSIDE has no place in the list.
   overrides="$(perl -0777 -ne '
      my $src = $_;
      $src =~ s{/\*.*?\*/}{}gs;
      $src =~ s{//[^\n]*}{}g;
      my %tables;
      while ($src =~ /tool_action_kind_entry_t\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\n\};/gs) {
         $tables{$1} = $2;
      }
      while ($src =~ /tool_metadata_t\s+\w+\s*=\s*\{(.*?)\n\};/gs) {
         my $body = $1;
         my ($name) = $body =~ /\.name\s*=\s*"([^"]+)"/;
         my ($own) = $body =~ /\.results\s*=\s*TOOL_RESULTS_(OWN|OUTSIDE)\b/;
         my ($table) = $body =~ /\.action_kinds\s*=\s*(\w+)/;
         next unless $name && $own && $table && exists $tables{$table};
         my $rows = $tables{$table};
         while ($rows =~ /\{\s*"([^"]+)"[^{}]*?TOOL_RESULTS_(OWN|OUTSIDE)\s*\}/g) {
            print "$name $1 $2\n" if $2 ne $own;
         }
      }' src/tools/*.c | sort -u)"
   expected="$(perl -0777 -ne '
      my ($t) = /#define TOOL_OWN_RESULTS_EXCEPT_TOOL\s+"([^"]+)"/;
      my ($a) = /#define TOOL_OWN_RESULTS_EXCEPT_ACTION\s+"([^"]+)"/;
      print "$t $a OUTSIDE\n" if $t && $a;' include/tools/tool_own_results.h)"
   if [ "$overrides" != "$expected" ]; then
      echo "include/tools/tool_own_results.h's exception isn't the one action override the tools declare:"
      diff <(printf '%s\n' "$expected") <(printf '%s\n' "$overrides") | sed 's/^/  /'
      violations=$((violations + 1))
   fi
fi

if [ "$violations" -gt 0 ]; then
   echo "check_tool_action_kinds: $violations file(s) with a bad action_kinds table" >&2
   echo "  (a tool whose table fails is refused registration: the daemon would run without it)" >&2
   exit 1
fi
exit 0
