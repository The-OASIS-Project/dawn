# Dawn Streaming Tests

This directory contains test programs for the LLM streaming implementation.

## Test Programs

### test_sse_parser
Tests the Server-Sent Events (SSE) parser with various scenarios:
- Simple events
- Events with types
- Multi-line data
- Split events across chunks
- OpenAI and Claude JSON formats

**Expected:** 11 events

### test_sentence_buffer
Tests the sentence boundary detection for TTS:
- Simple sentences
- Split sentences across chunks
- Multiple terminators (., !, ?, :)
- Multiple sentences in one chunk
- Incomplete sentences
- Token-by-token streaming

**Expected:** ~13 sentences

### test_streaming
End-to-end test of LLM streaming with real API calls:
- Non-streaming baseline
- Raw chunk streaming
- Sentence-buffered streaming for TTS

**Note:** Requires valid API keys in `secrets.toml`

## Building Tests

```bash
cd tests
mkdir build
cd build
cmake ..
make
```

## Running Tests

```bash
# SSE Parser test
./test_sse_parser

# Sentence Buffer test
./test_sentence_buffer

# Streaming test (requires API keys)
./test_streaming          # Auto-detect provider
./test_streaming openai   # Force OpenAI
./test_streaming claude   # Force Claude
```

## Cleaning Up

```bash
cd build
make clean
# Or remove build directory entirely:
cd ..
rm -rf build
```
