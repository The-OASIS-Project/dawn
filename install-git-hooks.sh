#!/bin/bash
#
# Script to install git hooks (pre-commit + pre-push).
#

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Check if we're in a git repository
if [ ! -d ".git" ]; then
   echo -e "${RED}ERROR: Not a git repository!${NC}"
   echo -e "${YELLOW}Initialize git first with:${NC} git init"
   exit 1
fi

# Check if format_code.sh exists (required by the pre-commit hook)
if [ ! -f "./format_code.sh" ]; then
   echo -e "${RED}ERROR: format_code.sh not found!${NC}"
   exit 1
fi

# Check hook source files
if [ ! -f "./pre-commit.hook" ]; then
   echo -e "${RED}ERROR: pre-commit.hook not found!${NC}"
   exit 1
fi
if [ ! -f "./pre-push.hook" ]; then
   echo -e "${RED}ERROR: pre-push.hook not found!${NC}"
   exit 1
fi

echo -e "${BLUE}Git Hooks Installer${NC}"
echo -e "${BLUE}===================${NC}"
echo ""
echo "Available hooks:"
echo ""
echo -e "  ${BLUE}pre-commit${NC} — checks formatting of the staged content; when source or build"
echo -e "              files are staged, also builds tests-ci + benches and runs the CI suite."
echo ""
echo -e "  ${BLUE}pre-push${NC}   — builds tests-ci + runs the CI test suite; rejects pushes"
echo -e "              if it fails. (Satellite + daemon build-config smoke run in CI.)"
echo ""
echo "Choose an option:"
echo ""
echo -e "  ${GREEN}1)${NC} Install both hooks (recommended)"
echo -e "  ${GREEN}2)${NC} Install pre-commit only"
echo -e "  ${GREEN}3)${NC} Install pre-push only"
echo -e "  ${RED}4)${NC} Remove all hooks"
echo -e "  ${YELLOW}5)${NC} Cancel"
echo ""
read -p "Enter choice [1-5]: " choice

install_pre_commit() {
   cp pre-commit.hook .git/hooks/pre-commit
   chmod +x .git/hooks/pre-commit
   echo -e "${GREEN}✓ pre-commit installed${NC}"
}

install_pre_push() {
   cp pre-push.hook .git/hooks/pre-push
   chmod +x .git/hooks/pre-push
   echo -e "${GREEN}✓ pre-push installed${NC}"
}

remove_hook() {
   local name="$1"
   if [ -f ".git/hooks/$name" ]; then
      rm ".git/hooks/$name"
      echo -e "${GREEN}✓ $name removed${NC}"
   else
      echo -e "${YELLOW}no $name hook found${NC}"
   fi
}

case $choice in
   1)
      install_pre_commit
      install_pre_push
      echo ""
      echo -e "${YELLOW}Reminders:${NC}"
      echo -e "  - Run ${BLUE}./format_code.sh --changed${NC} before committing if formatting fails."
      echo -e "  - The pre-push hook needs a working build directory (build-ci/ preferred, build-debug/ falls back)."
      ;;
   2)
      install_pre_commit
      ;;
   3)
      install_pre_push
      echo ""
      echo -e "${YELLOW}Note:${NC} the pre-push hook requires a working build directory."
      echo -e "      Bootstrap with: ${BLUE}cmake --preset ci${NC} (matches CI; build-debug works as fallback)"
      ;;
   4)
      remove_hook pre-commit
      remove_hook pre-push
      ;;
   5)
      echo "Cancelled."
      exit 0
      ;;
   *)
      echo -e "${RED}Invalid choice!${NC}"
      exit 1
      ;;
esac

echo ""
echo -e "${BLUE}Bypass:${NC} ${YELLOW}git commit --no-verify${NC} / ${YELLOW}git push --no-verify${NC}"
echo ""
