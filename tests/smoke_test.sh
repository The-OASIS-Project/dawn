#!/bin/bash
#
# Smoke test script for Dawn build modes
# Builds all presets and verifies each binary starts successfully
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_ROOT"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Build presets to test
PRESETS=(
    "local"
    "full"
    "debug"
)

# Track results
declare -A BUILD_RESULTS
declare -A RUN_RESULTS
FAILED=0

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_header() {
    echo ""
    echo "=============================================="
    echo " $1"
    echo "=============================================="
}

# Map preset name to build directory
get_build_dir() {
    local preset=$1
    case "$preset" in
        local) echo "build-local" ;;
        full)  echo "build-full" ;;
        debug) echo "build-debug" ;;
        *)           echo "build-${preset}" ;;
    esac
}

# Build a preset
build_preset() {
    local preset=$1
    local build_dir=$(get_build_dir "$preset")

    log_info "Configuring $preset..."
    if ! cmake --preset "$preset" > /tmp/cmake_${preset}.log 2>&1; then
        log_error "CMake configure failed for $preset"
        cat /tmp/cmake_${preset}.log
        return 1
    fi

    log_info "Building $preset..."
    if ! make -C "$build_dir" -j$(nproc) > /tmp/make_${preset}.log 2>&1; then
        log_error "Build failed for $preset"
        tail -50 /tmp/make_${preset}.log
        return 1
    fi

    log_info "Build successful: $build_dir/dawn"
    return 0
}

# Test if binary starts and reaches "Listening..."
test_run() {
    local preset=$1
    local build_dir=$(get_build_dir "$preset")
    local binary="$build_dir/dawn"
    local log_file="/tmp/dawn_${preset}_run.log"

    if [[ ! -x "$binary" ]]; then
        log_error "Binary not found: $binary"
        return 1
    fi

    log_info "Testing $preset startup..."

    # Run with timeout, capture output
    export LD_LIBRARY_PATH=/usr/local/lib
    timeout 8 "$binary" > "$log_file" 2>&1 || true

    # Check for successful startup indicators
    if grep -q "Listening\.\.\." "$log_file"; then
        log_info "$preset: Reached 'Listening...' state"
        return 0
    elif grep -q "ERR.*Failed" "$log_file"; then
        log_error "$preset: Startup failed"
        grep "ERR" "$log_file" | head -5
        return 1
    else
        log_warn "$preset: Did not reach 'Listening...' state"
        tail -10 "$log_file"
        return 1
    fi
}

# Main
log_header "Dawn Smoke Test"
echo "Testing ${#PRESETS[@]} build presets..."
echo "Project root: $PROJECT_ROOT"

# Option to skip builds
SKIP_BUILD=0
if [[ "$1" == "--skip-build" ]]; then
    SKIP_BUILD=1
    log_info "Skipping builds (--skip-build)"
fi

# Build phase
if [[ $SKIP_BUILD -eq 0 ]]; then
    log_header "Build Phase"
    for preset in "${PRESETS[@]}"; do
        echo ""
        if build_preset "$preset"; then
            BUILD_RESULTS[$preset]="PASS"
        else
            BUILD_RESULTS[$preset]="FAIL"
            FAILED=1
        fi
    done
fi

# Run phase
log_header "Run Phase"
for preset in "${PRESETS[@]}"; do
    echo ""
    if test_run "$preset"; then
        RUN_RESULTS[$preset]="PASS"
    else
        RUN_RESULTS[$preset]="FAIL"
        FAILED=1
    fi
done

# Summary
log_header "Results Summary"
printf "%-15s %-10s %-10s\n" "Preset" "Build" "Run"
printf "%-15s %-10s %-10s\n" "-------" "-----" "---"
for preset in "${PRESETS[@]}"; do
    build_status="${BUILD_RESULTS[$preset]:-SKIP}"
    run_status="${RUN_RESULTS[$preset]:-SKIP}"

    # Color the results
    if [[ "$build_status" == "PASS" ]]; then
        build_colored="${GREEN}PASS${NC}"
    elif [[ "$build_status" == "FAIL" ]]; then
        build_colored="${RED}FAIL${NC}"
    else
        build_colored="${YELLOW}SKIP${NC}"
    fi

    if [[ "$run_status" == "PASS" ]]; then
        run_colored="${GREEN}PASS${NC}"
    elif [[ "$run_status" == "FAIL" ]]; then
        run_colored="${RED}FAIL${NC}"
    else
        run_colored="${YELLOW}SKIP${NC}"
    fi

    printf "%-15s " "$preset"
    echo -e "$build_colored       $run_colored"
done

echo ""
if [[ $FAILED -eq 0 ]]; then
    log_info "All tests passed!"
    exit 0
else
    log_error "Some tests failed!"
    exit 1
fi
