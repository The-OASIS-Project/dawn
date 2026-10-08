# LLM quality suite v2: ranks chat models on DAWN's own tasks by replaying
# requests captured from the daemon (dawn-admin llm capture), running the tool
# loop against stateful mocks, and scoring outcomes.  Design:
# docs/LLM_QUALITY_SUITE_DESIGN.md.  Usage: python3 -m llm_testing.quality --help
#
# License: GPLv3, same as DAWN.
