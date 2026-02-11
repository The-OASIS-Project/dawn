#!/bin/bash
#
# Script to install git pre-commit hooks for code formatting
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

# Check if format_code.sh exists
if [ ! -f "./format_code.sh" ]; then
   echo -e "${RED}ERROR: format_code.sh not found!${NC}"
   exit 1
fi

echo -e "${BLUE}Git Pre-commit Hook Installer${NC}"
echo -e "${BLUE}=============================${NC}"
echo ""
echo "This will install a pre-commit hook that:"
echo "  - Checks if changed/staged C/C++ code is properly formatted"
echo "  - Rejects commits if code needs formatting"
echo "  - Requires you to run ./format_code.sh before committing"
echo ""
echo "Choose an option:"
echo ""
echo -e "  ${GREEN}1)${NC} Install pre-commit hook"
echo -e "  ${RED}2)${NC} Remove existing hook"
echo -e "  ${RED}3)${NC} Cancel"
echo ""
read -p "Enter choice [1-3]: " choice

case $choice in
   1)
      cp pre-commit.hook .git/hooks/pre-commit
      chmod +x .git/hooks/pre-commit
      echo -e "${GREEN}✓ Pre-commit hook installed!${NC}"
      echo ""
      echo -e "${YELLOW}To commit, your code must be formatted first.${NC}"
      echo -e "Run: ${BLUE}./format_code.sh${NC}"
      ;;
   2)
      if [ -f ".git/hooks/pre-commit" ]; then
         rm .git/hooks/pre-commit
         echo -e "${GREEN}✓ Pre-commit hook removed.${NC}"
      else
         echo -e "${YELLOW}No pre-commit hook found.${NC}"
      fi
      ;;
   3)
      echo "Cancelled."
      exit 0
      ;;
   *)
      echo -e "${RED}Invalid choice!${NC}"
      exit 1
      ;;
esac

echo ""
echo -e "${BLUE}Note:${NC} You can bypass the hook with: ${YELLOW}git commit --no-verify${NC}"
echo ""
