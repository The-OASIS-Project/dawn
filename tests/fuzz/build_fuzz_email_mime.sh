#!/usr/bin/env bash
# Builds and runs the email-reading fuzzer (libFuzzer + ASan/UBSan).
#   tests/fuzz/build_fuzz_email_mime.sh [seconds]   (default 300)
# Needs clang with libFuzzer (e.g. clang-20) and libgmime-3.0-dev.
set -euo pipefail
cd "$(dirname "$0")/../.."
CLANG="${CLANG:-$(command -v clang-20 || command -v clang || echo /usr/lib/llvm-20/bin/clang)}"
OUT="${OUT:-/tmp/dawn_fuzz_email_mime}"
mkdir -p "$OUT/corpus"
cp -n tests/fuzz/corpus_email_mime/* "$OUT/corpus/" 2>/dev/null || true
"$CLANG" -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
   -Iinclude -Icommon/include $(pkg-config --cflags gmime-3.0 json-c) \
   tests/fuzz/fuzz_email_mime.c src/tools/email_mime.c src/tools/email_display.c src/tools/gmail_parts.c \
   src/tools/html_parser.c common/src/utils/string_utils.c common/src/logging.c \
   $(pkg-config --libs gmime-3.0 json-c) -o "$OUT/fuzz_email_mime"
"$OUT/fuzz_email_mime" -max_total_time="${1:-300}" -max_len=262144 -rss_limit_mb=1024 \
   -timeout=10 "$OUT/corpus"
