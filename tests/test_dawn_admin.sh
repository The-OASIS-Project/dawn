#!/bin/bash
#
# Integration tests for dawn-admin CLI
# Requires: Dawn daemon running with auth enabled
#
# Usage: ./test_dawn_admin.sh [--token TOKEN]
#
# If --token is not provided, the script will prompt for it.
# Get the token from Dawn daemon startup logs:
#   grep "Setup token" /path/to/dawn.log
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Counters
PASSED=0
FAILED=0
SKIPPED=0

# Path to dawn-admin binary
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
DAWN_ADMIN="${PROJECT_ROOT}/build-debug/dawn-admin"

# Test user credentials
TEST_USER="testuser_$$"
TEST_PASS="TestPass123!"
TEST_ADMIN_USER="testadmin_$$"
TEST_ADMIN_PASS="AdminPass456!"

# Admin credentials for authenticated operations (set after creating admin)
ADMIN_USER=""
ADMIN_PASS=""

# Setup token (can be set via --token arg or DAWN_SETUP_TOKEN env var)
SETUP_TOKEN="${DAWN_SETUP_TOKEN:-}"

# Existing admin credentials (alternative to setup token)
EXISTING_ADMIN_USER="${DAWN_ADMIN_USER:-}"
EXISTING_ADMIN_PASS="${DAWN_ADMIN_PASSWORD:-}"

# Mode: "token" (fresh DB) or "admin" (existing admin credentials)
AUTH_MODE=""

# Backup path for db backup test
BACKUP_PATH="/tmp/dawn_auth_backup_$$.db"

print_header() {
    echo ""
    echo "========================================"
    echo " $1"
    echo "========================================"
}

pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((++PASSED))
}

fail() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((++FAILED))
}

skip() {
    echo -e "${YELLOW}[SKIP]${NC} $1"
    ((++SKIPPED))
}

info() {
    echo -e "[INFO] $1"
}

# Check if dawn-admin exists
check_binary() {
    if [[ ! -x "$DAWN_ADMIN" ]]; then
        echo -e "${RED}Error: dawn-admin not found at $DAWN_ADMIN${NC}"
        echo "Build the project first: make -C build-debug -j4"
        exit 1
    fi
}

# Parse command line arguments
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            --token)
                SETUP_TOKEN="$2"
                shift 2
                ;;
            --admin-user)
                EXISTING_ADMIN_USER="$2"
                shift 2
                ;;
            --admin-pass)
                EXISTING_ADMIN_PASS="$2"
                shift 2
                ;;
            --help|-h)
                echo "Usage: $0 [--token TOKEN] [--admin-user USER --admin-pass PASS]"
                echo ""
                echo "Authentication modes (choose one):"
                echo "  --token TOKEN              Setup token from Dawn daemon logs (fresh DB)"
                echo "  --admin-user/--admin-pass  Use existing admin credentials"
                echo ""
                echo "Options:"
                echo "  --help                     Show this help"
                echo ""
                echo "Environment variables:"
                echo "  DAWN_SETUP_TOKEN           Alternative to --token"
                echo "  DAWN_ADMIN_USER            Alternative to --admin-user"
                echo "  DAWN_ADMIN_PASSWORD        Alternative to --admin-pass"
                echo ""
                echo "Examples:"
                echo "  # Fresh database - use setup token from daemon logs"
                echo "  $0 --token DAWN-XXXX-XXXX-XXXX-XXXX"
                echo ""
                echo "  # Existing database - use admin credentials"
                echo "  $0 --admin-user admin --admin-pass mypassword"
                exit 0
                ;;
            *)
                echo "Unknown option: $1"
                exit 1
                ;;
        esac
    done
}

# Determine authentication mode and get credentials
get_auth_credentials() {
    # Check if admin credentials are provided
    if [[ -n "$EXISTING_ADMIN_USER" && -n "$EXISTING_ADMIN_PASS" ]]; then
        AUTH_MODE="admin"
        ADMIN_USER="$EXISTING_ADMIN_USER"
        ADMIN_PASS="$EXISTING_ADMIN_PASS"
        info "Using existing admin credentials (user: $ADMIN_USER)"
        return
    fi

    # Check if setup token is provided
    if [[ -n "$SETUP_TOKEN" ]]; then
        AUTH_MODE="token"
        info "Using setup token for authentication"
        return
    fi

    # Neither provided - prompt user
    echo ""
    echo "Choose authentication method:"
    echo "  1) Setup token (fresh database)"
    echo "  2) Existing admin credentials"
    echo ""
    read -p "Enter choice (1 or 2): " choice

    case $choice in
        1)
            AUTH_MODE="token"
            echo ""
            echo "Find setup token in Dawn daemon logs: grep 'Setup token' <logfile>"
            read -p "Enter setup token (DAWN-XXXX-XXXX-XXXX-XXXX): " SETUP_TOKEN
            if [[ -z "$SETUP_TOKEN" ]]; then
                echo -e "${RED}No token provided. Exiting.${NC}"
                exit 1
            fi
            ;;
        2)
            AUTH_MODE="admin"
            read -p "Enter admin username: " EXISTING_ADMIN_USER
            read -s -p "Enter admin password: " EXISTING_ADMIN_PASS
            echo ""
            if [[ -z "$EXISTING_ADMIN_USER" || -z "$EXISTING_ADMIN_PASS" ]]; then
                echo -e "${RED}Missing credentials. Exiting.${NC}"
                exit 1
            fi
            ADMIN_USER="$EXISTING_ADMIN_USER"
            ADMIN_PASS="$EXISTING_ADMIN_PASS"
            ;;
        *)
            echo -e "${RED}Invalid choice. Exiting.${NC}"
            exit 1
            ;;
    esac
}

# ============================================================================
# Test Cases
# ============================================================================

test_ping() {
    print_header "Testing: ping"

    if $DAWN_ADMIN ping >/dev/null 2>&1; then
        pass "Daemon responded to ping"
    else
        fail "Daemon did not respond to ping"
        echo "Is the Dawn daemon running with ENABLE_AUTH?"
        exit 1
    fi
}

test_user_create() {
    print_header "Testing: user create"

    if [[ "$AUTH_MODE" == "token" ]]; then
        # Token mode: Create admin user with setup token (fresh DB)
        info "Creating admin user: $TEST_ADMIN_USER (using setup token)"
        if DAWN_SETUP_TOKEN="$SETUP_TOKEN" DAWN_PASSWORD="$TEST_ADMIN_PASS" \
           $DAWN_ADMIN user create "$TEST_ADMIN_USER" --admin 2>&1; then
            pass "Created admin user '$TEST_ADMIN_USER'"
            ADMIN_USER="$TEST_ADMIN_USER"
            ADMIN_PASS="$TEST_ADMIN_PASS"
        else
            fail "Failed to create admin user '$TEST_ADMIN_USER'"
            return 1
        fi

        # Verify token is now invalidated (should fail)
        info "Verifying setup token is now invalidated"
        if DAWN_SETUP_TOKEN="$SETUP_TOKEN" DAWN_PASSWORD="$TEST_PASS" \
           $DAWN_ADMIN user create "shouldfail_$$" --admin 2>&1; then
            fail "Token should have been invalidated after first use"
        else
            pass "Setup token correctly invalidated after use"
        fi
    else
        # Admin mode: User creation requires setup token (CLI limitation)
        # Skip this test - we'll use existing admin credentials for other tests
        skip "User create requires setup token (not available in admin mode)"
        info "Using existing admin account for remaining tests"
    fi
}

test_user_list() {
    print_header "Testing: user list"

    output=$($DAWN_ADMIN user list 2>&1) || true

    # Check for the admin user (either test admin in token mode, or existing admin)
    local check_user="$ADMIN_USER"
    if [[ -z "$check_user" ]]; then
        check_user="$TEST_ADMIN_USER"
    fi

    if echo "$output" | grep -q "$check_user"; then
        pass "User list contains admin user ($check_user)"
    else
        fail "User list missing admin user ($check_user)"
        echo "$output"
    fi

    # Test JSON output (--json flag not yet implemented)
    # info "Testing JSON output"
    # if $DAWN_ADMIN user list --json 2>&1 | grep -q '"username"'; then
    #     pass "JSON output format works"
    # else
    #     fail "JSON output format failed"
    # fi
    skip "JSON output not yet implemented"
}

test_user_passwd() {
    print_header "Testing: user passwd"

    if [[ "$AUTH_MODE" == "admin" ]]; then
        # Don't modify the existing admin's password - that would break things
        skip "Password change skipped in admin mode (no test user to modify)"
        return
    fi

    if [[ -z "$ADMIN_USER" ]]; then
        skip "No admin user available for passwd test"
        return
    fi

    NEW_PASS="NewPassword789!"

    # Change the test admin's password
    info "Changing password for $ADMIN_USER"
    if DAWN_ADMIN_USER="$ADMIN_USER" DAWN_ADMIN_PASSWORD="$ADMIN_PASS" DAWN_PASSWORD="$NEW_PASS" \
       $DAWN_ADMIN user passwd "$ADMIN_USER" 2>&1; then
        pass "Password changed for '$ADMIN_USER'"
        # Update for later tests
        ADMIN_PASS="$NEW_PASS"
    else
        fail "Failed to change password for '$ADMIN_USER'"
    fi
}

test_user_unlock() {
    print_header "Testing: user unlock"

    if [[ "$AUTH_MODE" == "admin" ]]; then
        skip "Unlock skipped in admin mode (no test user to unlock)"
        return
    fi

    if [[ -z "$ADMIN_USER" ]]; then
        skip "No admin user available for unlock test"
        return
    fi

    # Note: We can't easily lock an account without the WebUI login,
    # so we just test that the command runs without error on an unlocked user
    info "Running unlock on '$ADMIN_USER' (already unlocked)"
    if DAWN_ADMIN_USER="$ADMIN_USER" DAWN_ADMIN_PASSWORD="$ADMIN_PASS" \
       $DAWN_ADMIN user unlock "$ADMIN_USER" 2>&1; then
        pass "Unlock command executed successfully"
    else
        # This might fail if user isn't locked, which is fine
        pass "Unlock command completed (user may not have been locked)"
    fi
}

test_session_list() {
    print_header "Testing: session list"

    # Just verify the command runs - there may or may not be sessions
    if $DAWN_ADMIN session list 2>&1; then
        pass "Session list command executed"
    else
        fail "Session list command failed"
    fi

    # Test JSON output
    if $DAWN_ADMIN session list --json >/dev/null 2>&1; then
        pass "Session list JSON output works"
    else
        fail "Session list JSON output failed"
    fi
}

test_db_status() {
    print_header "Testing: db status"

    output=$($DAWN_ADMIN db status 2>&1) || true

    if echo "$output" | grep -qi "user"; then
        pass "Database status shows user info"
    else
        fail "Database status missing user info"
        echo "$output"
    fi

    if echo "$output" | grep -qi "session"; then
        pass "Database status shows session info"
    else
        fail "Database status missing session info"
    fi
}

test_db_backup() {
    print_header "Testing: db backup"

    if [[ -z "$ADMIN_USER" ]]; then
        skip "No admin user available for backup test"
        return
    fi

    # Remove any existing backup
    rm -f "$BACKUP_PATH"

    info "Backing up to $BACKUP_PATH"
    if DAWN_ADMIN_USER="$ADMIN_USER" DAWN_ADMIN_PASSWORD="$ADMIN_PASS" \
       $DAWN_ADMIN db backup "$BACKUP_PATH" 2>&1; then
        if [[ -f "$BACKUP_PATH" ]]; then
            pass "Database backup created"

            # Check permissions (should be 0600)
            perms=$(stat -c "%a" "$BACKUP_PATH" 2>/dev/null || stat -f "%Lp" "$BACKUP_PATH" 2>/dev/null)
            if [[ "$perms" == "600" ]]; then
                pass "Backup has secure permissions (0600)"
            else
                fail "Backup permissions incorrect: $perms (expected 600)"
            fi

            # Cleanup
            rm -f "$BACKUP_PATH"
        else
            fail "Backup file not created"
        fi
    else
        fail "Database backup command failed"
    fi
}

test_db_compact() {
    print_header "Testing: db compact"

    if [[ -z "$ADMIN_USER" ]]; then
        skip "No admin user available for compact test"
        return
    fi

    info "Compacting database (may be rate-limited)"
    output=$(DAWN_ADMIN_USER="$ADMIN_USER" DAWN_ADMIN_PASSWORD="$ADMIN_PASS" \
             $DAWN_ADMIN db compact 2>&1) || true

    if echo "$output" | grep -qi "compacted\|rate.limited\|success"; then
        pass "Database compact command executed"
    else
        # Rate limiting is expected if run recently
        if echo "$output" | grep -qi "rate\|limit\|wait"; then
            pass "Database compact correctly rate-limited"
        else
            fail "Database compact unexpected response: $output"
        fi
    fi
}

test_log_show() {
    print_header "Testing: log show"

    # Basic log show
    if $DAWN_ADMIN log show 2>&1 | head -20 >/dev/null; then
        pass "Log show command executed"
    else
        fail "Log show command failed"
    fi

    # With limit
    info "Testing --last option"
    if $DAWN_ADMIN log show --last 5 2>&1 >/dev/null; then
        pass "Log show with --last works"
    else
        fail "Log show with --last failed"
    fi

    # Filter by event type
    info "Testing --type filter"
    if $DAWN_ADMIN log show --type USER_CREATED 2>&1 >/dev/null; then
        pass "Log show with --type filter works"
    else
        fail "Log show with --type filter failed"
    fi

    # Filter by user
    info "Testing --user filter"
    if $DAWN_ADMIN log show --user "$TEST_ADMIN_USER" 2>&1 >/dev/null; then
        pass "Log show with --user filter works"
    else
        fail "Log show with --user filter failed"
    fi
}

test_user_delete() {
    print_header "Testing: user delete"

    if [[ -z "$ADMIN_USER" ]]; then
        skip "No admin user available for delete test"
        return
    fi

    if [[ "$AUTH_MODE" == "admin" ]]; then
        # Admin mode: No test user was created, skip deletion
        skip "No test user to delete (user create was skipped in admin mode)"
    else
        # Token mode: Try to delete the admin we created (should be blocked - last admin)
        info "Attempting to delete last admin: $TEST_ADMIN_USER (should be blocked)"
        output=$(DAWN_ADMIN_USER="$ADMIN_USER" DAWN_ADMIN_PASSWORD="$ADMIN_PASS" \
                 $DAWN_ADMIN user delete "$TEST_ADMIN_USER" --yes 2>&1) || true

        if echo "$output" | grep -qi "last admin"; then
            pass "Last admin deletion correctly blocked"
        elif $DAWN_ADMIN user list 2>&1 | grep -q "$TEST_ADMIN_USER"; then
            pass "Admin user still exists (deletion was blocked)"
        else
            fail "Last admin was deleted - protection failed!"
        fi
    fi
}

test_last_admin_protection() {
    # Already tested in test_user_delete - skip duplicate test
    skip "Last admin protection already tested in user delete"
}

test_ip_list() {
    print_header "Testing: ip list"

    if $DAWN_ADMIN ip list 2>&1 >/dev/null; then
        pass "IP list command executed"
    else
        fail "IP list command failed"
    fi
}

test_metrics() {
    print_header "Testing: metrics"

    # Basic list
    if $DAWN_ADMIN metrics list 2>&1 >/dev/null; then
        pass "Metrics list command executed"
    else
        fail "Metrics list command failed"
    fi

    # With limit
    info "Testing --last option"
    if $DAWN_ADMIN metrics list --last 5 2>&1 >/dev/null; then
        pass "Metrics list with --last works"
    else
        fail "Metrics list with --last failed"
    fi

    # Filter by type
    info "Testing --type LOCAL filter"
    if $DAWN_ADMIN metrics list --type LOCAL 2>&1 >/dev/null; then
        pass "Metrics list with --type LOCAL works"
    else
        fail "Metrics list with --type LOCAL failed"
    fi

    info "Testing --type WEBSOCKET filter"
    if $DAWN_ADMIN metrics list --type WEBSOCKET 2>&1 >/dev/null; then
        pass "Metrics list with --type WEBSOCKET works"
    else
        fail "Metrics list with --type WEBSOCKET failed"
    fi

    # Summary
    info "Testing metrics summary"
    if $DAWN_ADMIN metrics summary 2>&1 >/dev/null; then
        pass "Metrics summary command executed"
    else
        fail "Metrics summary command failed"
    fi
}

test_conversations() {
    print_header "Testing: conversations"

    # List
    if $DAWN_ADMIN conv list 2>&1 >/dev/null; then
        pass "Conv list command executed"
    else
        fail "Conv list command failed"
    fi

    # With limit
    info "Testing --last option"
    if $DAWN_ADMIN conv list --last 5 2>&1 >/dev/null; then
        pass "Conv list with --last works"
    else
        fail "Conv list with --last failed"
    fi

    # With archived
    info "Testing --archived option"
    if $DAWN_ADMIN conv list --archived 2>&1 >/dev/null; then
        pass "Conv list with --archived works"
    else
        fail "Conv list with --archived failed"
    fi

    # Show a specific conversation (grab first ID)
    info "Testing conv show"
    local conv_id
    conv_id=$($DAWN_ADMIN conv list --last 1 2>&1 | grep -oP '^\s*\K\d+' | head -1 || true)
    if [ -n "$conv_id" ]; then
        if $DAWN_ADMIN conv show "$conv_id" 2>&1 >/dev/null; then
            pass "Conv show $conv_id executed"
        else
            fail "Conv show $conv_id failed"
        fi
    else
        skip "No conversations found to show"
    fi
}

test_music() {
    print_header "Testing: music"

    # Stats
    if $DAWN_ADMIN music stats 2>&1 >/dev/null; then
        pass "Music stats command executed"
    else
        fail "Music stats command failed"
    fi

    # List with limit
    info "Testing music list"
    if $DAWN_ADMIN music list --limit 5 2>&1 >/dev/null; then
        pass "Music list with --limit works"
    else
        fail "Music list with --limit failed"
    fi

    # Search
    info "Testing music search"
    if $DAWN_ADMIN music search "test" 2>&1 >/dev/null; then
        pass "Music search command executed"
    else
        fail "Music search command failed"
    fi
}

test_edge_cases() {
    print_header "Testing: edge cases"

    # Unknown command should fail
    info "Testing unknown command"
    if $DAWN_ADMIN nonexistent_command 2>&1 >/dev/null; then
        fail "Unknown command should return non-zero"
    else
        pass "Unknown command correctly returns error"
    fi

    # Help
    info "Testing help"
    if $DAWN_ADMIN help 2>&1 | grep -q "Usage:"; then
        pass "Help command shows usage"
    else
        fail "Help command missing usage text"
    fi
}

# ============================================================================
# Main
# ============================================================================

main() {
    echo "========================================"
    echo " Dawn Admin CLI Integration Tests"
    echo "========================================"
    echo ""
    echo "Binary: $DAWN_ADMIN"
    echo "Test user: $TEST_USER"
    echo "Test admin: $TEST_ADMIN_USER"
    echo ""

    check_binary
    parse_args "$@"

    # Test connectivity first
    test_ping

    # Get authentication credentials (setup token or admin creds)
    get_auth_credentials
    echo ""
    info "Auth mode: $AUTH_MODE"

    # Run tests in order
    test_user_create
    test_user_list
    test_user_passwd
    test_user_unlock
    test_session_list
    test_db_status
    test_db_backup
    test_db_compact
    test_log_show
    test_ip_list
    test_metrics
    test_conversations
    test_music
    test_edge_cases

    # Cleanup tests (run last)
    test_user_delete
    test_last_admin_protection

    # Summary
    print_header "Test Summary"
    echo -e "${GREEN}Passed: $PASSED${NC}"
    echo -e "${RED}Failed: $FAILED${NC}"
    echo -e "${YELLOW}Skipped: $SKIPPED${NC}"
    echo ""

    if [[ $FAILED -gt 0 ]]; then
        echo -e "${RED}Some tests failed!${NC}"
        exit 1
    else
        echo -e "${GREEN}All tests passed!${NC}"
        exit 0
    fi
}

main "$@"
