#!/usr/bin/env bash

###############################################################################
# format_code.sh - Automated code formatting for C/C++ and JS/CSS/HTML
#
# This script recursively formats source files using:
# - clang-format for C/C++ files
# - Prettier for JavaScript/CSS/HTML files (optional, graceful degradation)
#
# Usage:
#   ./format_code.sh [directory]
#
# If no directory is specified, formats current directory.
###############################################################################

set -e  # Exit on error

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration — require clang-format-14 for consistent formatting across all platforms
CLANG_FORMAT="clang-format-14"
CONFIG_FILE=".clang-format"

# File extensions to format
C_EXTENSIONS=("c" "h")
CPP_EXTENSIONS=("cpp" "hpp" "cc" "cxx" "hh" "hxx")
WEB_EXTENSIONS=("js" "css" "html")

# Directories and files to exclude (add more as needed)
EXCLUDE_DIRS=(
   ".git"
   "build*"
   "cmake-build-debug"
   "cmake-build-release"
   ".vscode"
   ".idea"
   "node_modules"
   "vendor"
   "third_party"
   "external"
   "whisper.cpp"
   "webrtc-audio-processing"
)

EXCLUDE_FILES=(
   "tweetnacl.c"  # Vendored public-domain Ed25519 (ESP32 OTA) — keep verbatim
   "tweetnacl.h"  # ditto
)

###############################################################################
# Functions
###############################################################################

print_usage() {
   cat << USAGE
Usage: $0 [OPTIONS] [DIRECTORY]

Format source code using clang-format (C/C++) and Prettier (JS/CSS/HTML).

OPTIONS:
   -h, --help          Show this help message
   -n, --dry-run       Show what would be formatted without making changes
   -v, --verbose       Show detailed output
   -c, --config FILE   Use alternate config file (default: .clang-format)
   --check             Check if files are formatted (exit 1 if not)
   --changed           Only format files changed since last commit or staged
   --staged            Operate on the STAGED (git index) content of staged
                       files — what a commit will actually record.  Unlike
                       --changed (which reads working-tree files), --staged
                       --check verifies the index blobs, so a clean working
                       tree can't mask an unformatted staged blob.  Used by
                       the pre-commit hook.

EXAMPLES:
   $0                  Format all files in current directory
   $0 src/             Format all C/C++ files in src/ directory
   $0 www/             Format all JS/CSS/HTML files in www/ directory
   $0 --dry-run        Show what would be changed without formatting
   $0 --check          Check if code is properly formatted (CI mode)
   $0 --changed        Format only uncommitted/staged files (fast)
   $0 --staged --check Verify staged index content (pre-commit hook mode)

NOTE: Prettier formatting requires 'npm install' to be run first.
      If Prettier is not available, only C/C++ files are formatted.

USAGE
}

log_info() {
   echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
   echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
   echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
   echo -e "${RED}[ERROR]${NC} $1"
}

check_clang_format() {
   if ! command -v "$CLANG_FORMAT" &> /dev/null; then
      log_error "clang-format-14 not found. This project requires clang-format version 14."
      echo "  Ubuntu/Debian: sudo apt-get install clang-format-14"
      echo "  macOS:         brew install clang-format@14"
      echo ""
      echo "  Note: Other versions produce different output. Version 14 is required"
      echo "  for consistent formatting across all development platforms."
      exit 1
   fi

   local version
   version=$($CLANG_FORMAT --version | grep -oP '\d+\.\d+' | head -1)
   local major
   major=$(echo "$version" | cut -d. -f1)
   if [ "$major" != "14" ]; then
      log_error "clang-format version 14 required, found version $version"
      echo "  Install with: sudo apt-get install clang-format-14"
      exit 1
   fi
   log_info "Using clang-format version $version"
}

check_prettier() {
   # Check if npx is available
   if ! command -v npx &> /dev/null; then
      log_warning "npx not found. JS/CSS/HTML formatting skipped."
      log_info "Install Node.js and run 'npm install' for web file formatting."
      return 1
   fi

   # Check if package.json and node_modules exist
   if [[ ! -f "package.json" ]]; then
      log_warning "package.json not found. JS/CSS/HTML formatting skipped."
      return 1
   fi

   if [[ ! -d "node_modules" ]]; then
      log_warning "node_modules not found. Run 'npm install' for JS/CSS/HTML formatting."
      return 1
   fi

   # Check if prettier is installed
   if [[ ! -d "node_modules/prettier" ]]; then
      log_warning "Prettier not installed. Run 'npm install' for JS/CSS/HTML formatting."
      return 1
   fi

   local version
   version=$(npx prettier --version 2>/dev/null)
   log_info "Using Prettier version $version"
   return 0
}

should_exclude_dir() {
   local dir=$1
   local basename
   basename=$(basename "$dir")
   
   for exclude in "${EXCLUDE_DIRS[@]}"; do
      if [[ "$basename" == "$exclude" ]]; then
         return 0
      fi
   done
   return 1
}

should_exclude_file() {
   local file=$1
   local basename
   basename=$(basename "$file")
   
   for exclude in "${EXCLUDE_FILES[@]}"; do
      if [[ "$basename" == "$exclude" ]]; then
         return 0
      fi
   done
   return 1
}

is_c_file() {
   local file=$1
   local ext="${file##*.}"
   
   for e in "${C_EXTENSIONS[@]}"; do
      if [[ "$ext" == "$e" ]]; then
         return 0
      fi
   done
   return 1
}

is_cpp_file() {
   local file=$1
   local ext="${file##*.}"
   
   for e in "${CPP_EXTENSIONS[@]}"; do
      if [[ "$ext" == "$e" ]]; then
         return 0
      fi
   done
   return 1
}

format_file() {
   local file=$1
   local dry_run=$2
   local check_mode=$3
   local verbose=$4
   
   if should_exclude_file "$file"; then
      [[ $verbose -eq 1 ]] && log_warning "Skipping excluded file: $file"
      return 0
   fi
   
   if [[ $check_mode -eq 1 ]]; then
      # Check mode: see if file needs formatting
      if ! $CLANG_FORMAT --style=file:"$CONFIG_FILE" --dry-run --Werror "$file" &> /dev/null; then
         log_error "Not formatted: $file"
         return 1
      fi
      [[ $verbose -eq 1 ]] && log_success "OK: $file"
      return 0
   fi
   
   if [[ $dry_run -eq 1 ]]; then
      # Dry run: show what would change
      if ! $CLANG_FORMAT --style=file:"$CONFIG_FILE" --dry-run --Werror "$file" &> /dev/null; then
         log_info "Would format: $file"
      fi
   else
      # Actually format the file
      if $CLANG_FORMAT --style=file:"$CONFIG_FILE" -i "$file"; then
         [[ $verbose -eq 1 ]] && log_success "Formatted: $file"
      else
         log_error "Failed to format: $file"
         return 1
      fi
   fi
   
   return 0
}

find_and_format() {
   local target_dir=$1
   local dry_run=$2
   local check_mode=$3
   local verbose=$4
   
   local count=0
   local failed=0
   
   # Build find command to exclude directories
   local exclude_opts=()
   for dir in "${EXCLUDE_DIRS[@]}"; do
      exclude_opts+=(-path "*/$dir" -prune -o)
   done
   
   # Find all C/C++ files
   while IFS= read -r -d '' file; do
      if is_c_file "$file" || is_cpp_file "$file"; then
         if format_file "$file" "$dry_run" "$check_mode" "$verbose"; then
            ((count++))
         else
            ((failed++))
         fi
      fi
   done < <(find "$target_dir" "${exclude_opts[@]}" -type f -print0)
   
   echo ""
   if [[ $check_mode -eq 1 ]]; then
      if [[ $failed -eq 0 ]]; then
         log_success "All $count files are properly formatted"
         return 0
      else
         log_error "$failed of $count files need formatting"
         return 1
      fi
   elif [[ $dry_run -eq 1 ]]; then
      log_info "Dry run complete. Checked $count files."
   else
      if [[ $failed -eq 0 ]]; then
         log_success "Successfully formatted $count files"
      else
         log_error "Failed to format $failed of $count files"
         return 1
      fi
   fi
}

format_web_files() {
   local target_dir=$1
   local dry_run=$2
   local check_mode=$3
   local verbose=$4

   # Determine the web directory to format
   local web_dir=""

   # If target is www/ or contains www/, format it
   if [[ "$target_dir" == *"www"* ]]; then
      web_dir="$target_dir"
   elif [[ -d "${target_dir}/www" ]]; then
      web_dir="${target_dir}/www"
   else
      # Not formatting web files (e.g., ./format_code.sh src/)
      return 0
   fi

   echo ""
   echo "=== Formatting JS/CSS/HTML ==="

   local result=0
   if [[ $check_mode -eq 1 ]]; then
      if npx prettier --check "$web_dir" 2>/dev/null; then
         log_success "All web files are properly formatted"
      else
         log_error "Some web files need formatting"
         result=1
      fi
   elif [[ $dry_run -eq 1 ]]; then
      log_info "Files that would be formatted:"
      npx prettier --list-different "$web_dir" 2>/dev/null || true
   else
      if npx prettier --write "$web_dir" 2>/dev/null; then
         [[ $verbose -eq 1 ]] && log_success "Web files formatted"
      else
         log_error "Failed to format web files"
         result=1
      fi
   fi

   return $result
}

format_changed_files() {
   local dry_run=$1
   local check_mode=$2
   local verbose=$3
   local prettier_available=$4

   # Get changed C/C++ and web files (staged + unstaged + untracked)
   local c_files=()
   local web_files=()

   while IFS= read -r file; do
      [[ -z "$file" || ! -f "$file" ]] && continue
      if is_c_file "$file" || is_cpp_file "$file"; then
         should_exclude_file "$file" && continue
         # Check if file is inside an excluded directory
         local skip=0
         for dir in "${EXCLUDE_DIRS[@]}"; do
            if [[ "$file" == *"/$dir/"* || "$file" == "$dir/"* ]]; then
               skip=1
               break
            fi
         done
         [[ $skip -eq 1 ]] && continue
         c_files+=("$file")
      fi
      local ext="${file##*.}"
      for e in "${WEB_EXTENSIONS[@]}"; do
         if [[ "$ext" == "$e" ]]; then
            web_files+=("$file")
            break
         fi
      done
   done < <(git diff --name-only HEAD 2>/dev/null; git diff --name-only --cached 2>/dev/null; git ls-files --others --exclude-standard 2>/dev/null)

   # Deduplicate
   local -A seen
   local unique_c=()
   for f in "${c_files[@]}"; do
      if [[ -z "${seen[$f]:-}" ]]; then
         seen[$f]=1
         unique_c+=("$f")
      fi
   done
   local -A seen_web
   local unique_web=()
   for f in "${web_files[@]}"; do
      if [[ -z "${seen_web[$f]:-}" ]]; then
         seen_web[$f]=1
         unique_web+=("$f")
      fi
   done

   local overall_result=0

   # Format C/C++ files
   echo "=== Formatting C/C++ (changed files) ==="
   if [[ ${#unique_c[@]} -eq 0 ]]; then
      log_info "No changed C/C++ files"
   else
      local count=0
      local failed=0
      for file in "${unique_c[@]}"; do
         if format_file "$file" "$dry_run" "$check_mode" "$verbose"; then
            ((count++))
         else
            ((failed++))
         fi
      done
      echo ""
      if [[ $check_mode -eq 1 ]]; then
         if [[ $failed -eq 0 ]]; then
            log_success "All $count changed files are properly formatted"
         else
            log_error "$failed of $count changed files need formatting"
            overall_result=1
         fi
      elif [[ $dry_run -eq 1 ]]; then
         log_info "Dry run complete. Checked $count changed files."
      else
         if [[ $failed -eq 0 ]]; then
            log_success "Successfully formatted $count changed files"
         else
            log_error "Failed to format $failed of $count changed files"
            overall_result=1
         fi
      fi
   fi

   # Format changed web files
   if [[ $prettier_available -eq 1 && ${#unique_web[@]} -gt 0 ]]; then
      echo ""
      echo "=== Formatting JS/CSS/HTML (changed files) ==="
      if [[ $check_mode -eq 1 ]]; then
         if npx prettier --check "${unique_web[@]}" 2>/dev/null; then
            log_success "All changed web files are properly formatted"
         else
            log_error "Some changed web files need formatting"
            overall_result=1
         fi
      elif [[ $dry_run -eq 1 ]]; then
         npx prettier --list-different "${unique_web[@]}" 2>/dev/null || true
      else
         if npx prettier --write "${unique_web[@]}" 2>/dev/null; then
            log_success "Changed web files formatted"
         else
            log_error "Failed to format changed web files"
            overall_result=1
         fi
      fi
   fi

   return $overall_result
}

# Check the STAGED blob of a C/C++ file (git index content) through
# clang-format.  This is what `git commit` records — checking it (rather than
# the working-tree file, as --changed does) closes the "fix-after-stage" gap
# where a clean working tree masks an unformatted blob still sitting in the
# index.
#
# --assume-filename is REQUIRED: clang-format reading from stdin has no path,
# so its main-header rule (the include matching the source basename sorts
# first) can't fire, and it emits spurious include-sort violations on every
# commit.  Passing the real path restores correct behavior.
check_staged_c_blob() {
   local file=$1
   git show ":$file" 2>/dev/null \
      | $CLANG_FORMAT --style=file:"$CONFIG_FILE" --dry-run --Werror \
                      --assume-filename="$file" &> /dev/null
}

# Check the STAGED blob of a web file through Prettier.  Prettier has no
# --check-from-stdin, so format the staged blob via --stdin-filepath and
# compare against the staged input.  The `; echo X` sentinel on both sides
# preserves trailing newlines (command substitution would otherwise strip
# them, hiding a missing-final-newline diff).
check_staged_web_blob() {
   local file=$1
   local staged formatted
   staged=$(git show ":$file" 2>/dev/null; echo X)
   formatted=$(git show ":$file" 2>/dev/null | npx prettier --stdin-filepath "$file" 2>/dev/null; echo X)
   [[ "$staged" == "$formatted" ]]
}

format_staged_files() {
   local dry_run=$1
   local check_mode=$2
   local verbose=$3
   local prettier_available=$4

   # Staged files only (index), additions/copies/modifications/renames —
   # never deletions.  `--name-only` lists each path once, so no dedup needed.
   local c_files=()
   local web_files=()
   while IFS= read -r file; do
      [[ -z "$file" ]] && continue
      if is_c_file "$file" || is_cpp_file "$file"; then
         should_exclude_file "$file" && continue
         local skip=0
         for dir in "${EXCLUDE_DIRS[@]}"; do
            if [[ "$file" == *"/$dir/"* || "$file" == "$dir/"* ]]; then
               skip=1
               break
            fi
         done
         [[ $skip -eq 1 ]] && continue
         c_files+=("$file")
      fi
      local ext="${file##*.}"
      for e in "${WEB_EXTENSIONS[@]}"; do
         if [[ "$ext" == "$e" ]]; then
            web_files+=("$file")
            break
         fi
      done
   done < <(git diff --name-only --cached --diff-filter=ACMR 2>/dev/null)

   local overall_result=0

   # --- C/C++ (staged blobs) ---
   echo "=== Checking C/C++ (staged index content) ==="
   if [[ ${#c_files[@]} -eq 0 ]]; then
      log_info "No staged C/C++ files"
   else
      local count=0
      local failed=0
      for file in "${c_files[@]}"; do
         if check_staged_c_blob "$file"; then
            [[ $verbose -eq 1 ]] && log_success "OK (staged): $file"
            ((count++))
         else
            if [[ $check_mode -eq 1 ]]; then
               log_error "Not formatted (staged): $file"
            elif [[ $dry_run -eq 1 ]]; then
               log_info "Would format (staged): $file"
            else
               # Write path: format the working-tree copy in place; the index
               # must then be re-staged to pick up the fix.
               if [[ -f "$file" ]]; then
                  format_file "$file" 0 0 "$verbose"
                  log_warning "Formatted working tree — re-stage to fix the index: git add $file"
               else
                  log_warning "Staged blob unformatted but no working-tree file: $file (re-stage after formatting)"
               fi
            fi
            ((failed++))
         fi
      done
      echo ""
      if [[ $failed -eq 0 ]]; then
         log_success "All $count staged C/C++ files are properly formatted"
      else
         log_error "$failed of $((count + failed)) staged C/C++ files need formatting"
         overall_result=1
      fi
   fi

   # --- Web (staged blobs) ---
   if [[ $prettier_available -eq 1 && ${#web_files[@]} -gt 0 ]]; then
      echo ""
      echo "=== Checking JS/CSS/HTML (staged index content) ==="
      local wcount=0
      local wfailed=0
      for file in "${web_files[@]}"; do
         if check_staged_web_blob "$file"; then
            [[ $verbose -eq 1 ]] && log_success "OK (staged): $file"
            ((wcount++))
         else
            if [[ $check_mode -eq 1 ]]; then
               log_error "Not formatted (staged): $file"
            elif [[ $dry_run -eq 1 ]]; then
               log_info "Would format (staged): $file"
            else
               if [[ -f "$file" ]]; then
                  npx prettier --write "$file" &> /dev/null
                  log_warning "Formatted working tree — re-stage to fix the index: git add $file"
               else
                  log_warning "Staged blob unformatted but no working-tree file: $file (re-stage after formatting)"
               fi
            fi
            ((wfailed++))
         fi
      done
      echo ""
      if [[ $wfailed -eq 0 ]]; then
         log_success "All $wcount staged web files are properly formatted"
      else
         log_error "$wfailed of $((wcount + wfailed)) staged web files need formatting"
         overall_result=1
      fi
   fi

   return $overall_result
}

###############################################################################
# Main
###############################################################################

main() {
   local target_dir="."
   local dry_run=0
   local check_mode=0
   local verbose=0
   local changed_only=0
   local staged_only=0

   # Parse arguments
   while [[ $# -gt 0 ]]; do
      case $1 in
         -h|--help)
            print_usage
            exit 0
            ;;
         -n|--dry-run)
            dry_run=1
            shift
            ;;
         --check)
            check_mode=1
            shift
            ;;
         --changed)
            changed_only=1
            shift
            ;;
         --staged)
            staged_only=1
            shift
            ;;
         -v|--verbose)
            verbose=1
            shift
            ;;
         -c|--config)
            CONFIG_FILE="$2"
            shift 2
            ;;
         -*)
            log_error "Unknown option: $1"
            print_usage
            exit 1
            ;;
         *)
            target_dir="$1"
            shift
            ;;
      esac
   done
   
   # Check if target directory exists
   if [[ ! -d "$target_dir" ]]; then
      log_error "Directory not found: $target_dir"
      exit 1
   fi
   
   # Check if config file exists
   if [[ ! -f "$CONFIG_FILE" ]]; then
      log_error "Config file not found: $CONFIG_FILE"
      log_info "Create a .clang-format file or specify one with --config"
      exit 1
   fi
   
   # Check for clang-format
   check_clang_format

   # Check for prettier (optional, graceful degradation)
   local prettier_available=0
   if check_prettier; then
      prettier_available=1
   fi

   # --staged and --changed are mutually exclusive; --staged wins.
   if [[ $staged_only -eq 1 && $changed_only -eq 1 ]]; then
      log_warning "--staged and --changed both given; using --staged (index content)"
      changed_only=0
   fi

   # Show what we're doing
   if [[ $staged_only -eq 1 ]]; then
      log_info "STAGED MODE: Operating on staged index content"
   elif [[ $changed_only -eq 1 ]]; then
      log_info "CHANGED MODE: Formatting only uncommitted/staged files"
   elif [[ $dry_run -eq 1 ]]; then
      log_info "DRY RUN: Checking what would be formatted in: $target_dir"
   elif [[ $check_mode -eq 1 ]]; then
      log_info "CHECK MODE: Verifying formatting in: $target_dir"
   else
      log_info "Formatting files in: $target_dir"
   fi

   log_info "Using config: $CONFIG_FILE"
   echo ""

   # Track overall result
   local overall_result=0

   if [[ $staged_only -eq 1 ]]; then
      if ! format_staged_files "$dry_run" "$check_mode" "$verbose" "$prettier_available"; then
         overall_result=1
      fi
      exit $overall_result
   fi

   if [[ $changed_only -eq 1 ]]; then
      if ! format_changed_files "$dry_run" "$check_mode" "$verbose" "$prettier_available"; then
         overall_result=1
      fi
      exit $overall_result
   fi

   # Format C/C++ files
   echo "=== Formatting C/C++ ==="
   if ! find_and_format "$target_dir" "$dry_run" "$check_mode" "$verbose"; then
      overall_result=1
   fi

   # Format web files (if prettier available and target includes www/)
   if [[ $prettier_available -eq 1 ]]; then
      if ! format_web_files "$target_dir" "$dry_run" "$check_mode" "$verbose"; then
         overall_result=1
      fi
   fi

   exit $overall_result
}

# Run main
main "$@"
