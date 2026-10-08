#!/usr/bin/env bash
# Builds DAWN's libFuzzer harnesses (with ASan + UBSan) and runs each for a time box.
#   tests/fuzz/run_fuzzers.sh [seconds] [harness ...]   (default 60 s each, every harness)
# Needs clang with libFuzzer and each harness's libraries (see LIBS below).
# Each harness starts from its seeds in tests/fuzz/corpus_<harness>/, with the tokens in
# tests/fuzz/<harness>.dict when there is one. A crash, leak,
# timeout or sanitizer report fails the run and leaves the input in $OUT/<harness>/.
set -euo pipefail
cd "$(dirname "$0")/../.."

SECS="${1:-60}"
shift || true
CLANG="${CLANG:-$(command -v clang-20 || command -v clang || echo clang)}"
OUT="${OUT:-${TMPDIR:-/tmp}/dawn_fuzz}"

COMMON="common/src/utils/string_utils.c common/src/logging.c"
declare -A SOURCES LIBS LINK CFLAGS_EXTRA MAXLEN
SOURCES[email_mime]="src/tools/email_mime.c src/tools/email_display.c src/tools/gmail_parts.c
                     src/tools/html_parser.c $COMMON"
LIBS[email_mime]="gmime-3.0 json-c"
MAXLEN[email_mime]=262144
SOURCES[html]="src/tools/html_parser.c $COMMON"
MAXLEN[html]=262144
SOURCES[neutralize]="src/llm/llm_context_text.c"
MAXLEN[neutralize]=65536
SOURCES[caldav]="src/tools/caldav_client.c $COMMON"
LIBS[caldav]="libxml-2.0 libical libcurl"
CFLAGS_EXTRA[caldav]="-DDAWN_ENABLE_CALENDAR_TOOL -DHAVE_LIBICAL"
MAXLEN[caldav]=131072
SOURCES[tts_text]="common/src/tts/tts_preprocessing.cpp common/src/tts/number_to_words.c"
LINK[tts_text]="-lstdc++"
MAXLEN[tts_text]=16384

harnesses=("$@")
if [ ${#harnesses[@]} -eq 0 ]; then
   harnesses=(email_mime html neutralize tts_text caldav)
fi

failed=()
for h in "${harnesses[@]}"; do
   [ -n "${SOURCES[$h]+x}" ] || { echo "unknown harness: $h" >&2; exit 2; }
   dir="$OUT/$h"
   mkdir -p "$dir/corpus"
   cflags="" libs=""
   if [ -n "${LIBS[$h]:-}" ]; then
      cflags=$(pkg-config --cflags ${LIBS[$h]})
      libs=$(pkg-config --libs ${LIBS[$h]})
   fi
   echo "== $h: build"
   # shellcheck disable=SC2086
   "$CLANG" -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
      -Iinclude -Icommon/include $cflags ${CFLAGS_EXTRA[$h]:-} "tests/fuzz/fuzz_$h.c" ${SOURCES[$h]} $libs ${LINK[$h]:-} \
      -o "$dir/fuzz_$h"
   dict=()
   [ -f "tests/fuzz/$h.dict" ] && dict=(-dict="tests/fuzz/$h.dict")
   echo "== $h: fuzz ${SECS}s"
   # New inputs go to $dir/corpus; the tracked seeds are read, never written.
   if ! "$dir/fuzz_$h" -max_total_time="$SECS" -max_len="${MAXLEN[$h]}" -rss_limit_mb=1024 \
      -timeout=10 -print_final_stats=1 -artifact_prefix="$dir/" "${dict[@]}" \
      "$dir/corpus" "tests/fuzz/corpus_$h"; then
      failed+=("$h")
   fi
done

if [ ${#failed[@]} -gt 0 ]; then
   echo "fuzzing found a problem in: ${failed[*]} (inputs in $OUT/<harness>/)" >&2
   exit 1
fi
echo "fuzzing: ${harnesses[*]} clean"
