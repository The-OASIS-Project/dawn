#!/bin/bash

# Test single model with model-specific optimized configuration
# Uses settings from model_configs.conf for each model

# Default draft model for speculative decoding
DEFAULT_DRAFT_MODEL="Qwen3-0.6B-Q8_0.gguf"

show_usage() {
    echo "Usage: $0 [options] <model_name.gguf>"
    echo ""
    echo "Options:"
    echo "  --spec, -s              Enable speculative decoding with default draft model"
    echo "  --draft <model.gguf>    Enable speculative decoding with specific draft model"
    echo "  -h, --help              Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0 Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
    echo "  $0 --spec Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
    echo "  $0 --draft Qwen3-0.6B-Q8_0.gguf Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
    echo ""
    echo "Available models:"
    ls -1 /var/lib/llama-cpp/models/*.gguf 2>/dev/null | xargs -n 1 basename
    echo ""
}

# Parse arguments
USE_SPEC_DECODING=false
DRAFT_MODEL=""
MODEL_ARG=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --spec|-s)
            USE_SPEC_DECODING=true
            DRAFT_MODEL="$DEFAULT_DRAFT_MODEL"
            shift
            ;;
        --draft)
            USE_SPEC_DECODING=true
            DRAFT_MODEL="$2"
            shift 2
            ;;
        -h|--help)
            show_usage
            exit 0
            ;;
        *)
            MODEL_ARG="$1"
            shift
            ;;
    esac
done

if [ -z "$MODEL_ARG" ]; then
    show_usage
    exit 1
fi

MODEL_PATH="/var/lib/llama-cpp/models/$MODEL_ARG"

if [ ! -f "$MODEL_PATH" ]; then
    echo "❌ Model not found: $MODEL_PATH"
    echo ""
    echo "Available models:"
    ls -1 /var/lib/llama-cpp/models/*.gguf 2>/dev/null | xargs -n 1 basename
    exit 1
fi

MODEL_NAME=$(basename "$MODEL_PATH" .gguf)
MODEL_FILE=$(basename "$MODEL_PATH")

# Validate draft model if speculative decoding enabled
if [ "$USE_SPEC_DECODING" = true ]; then
    DRAFT_MODEL_PATH="/var/lib/llama-cpp/models/$DRAFT_MODEL"
    if [ ! -f "$DRAFT_MODEL_PATH" ]; then
        echo "❌ Draft model not found: $DRAFT_MODEL_PATH"
        echo ""
        echo "Available models for draft:"
        ls -1 /var/lib/llama-cpp/models/*.gguf 2>/dev/null | xargs -n 1 basename
        exit 1
    fi
    DRAFT_MODEL_NAME=$(basename "$DRAFT_MODEL_PATH" .gguf)
fi

echo "============================================================================="
echo "Testing Model: $MODEL_NAME"
if [ "$USE_SPEC_DECODING" = true ]; then
    echo "Speculative Decoding: ENABLED (draft: $DRAFT_MODEL_NAME)"
fi
echo "============================================================================="
echo ""

# Load model-specific configuration
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/get_model_config.sh" "$MODEL_FILE"

if [ $? -ne 0 ]; then
    echo "❌ Failed to load model configuration"
    exit 1
fi

# Display configuration being used
echo "Model-Specific Configuration:"
echo "  GPU Layers:      $GPU_LAYERS"
echo "  Context Size:    $CONTEXT"
echo "  Batch Size:      $BATCH / $UBATCH"
echo "  Threads:         $THREADS"
echo "  Temperature:     $TEMP"
echo "  Top-P:           $TOP_P"
echo "  Top-K:           $TOP_K"
echo "  Repeat Penalty:  $REPEAT_PENALTY"
echo "  Extra Flags:     $EXTRA_FLAGS"
if [ "$USE_SPEC_DECODING" = true ]; then
    echo ""
    echo "Speculative Decoding:"
    echo "  Draft Model:     $DRAFT_MODEL"
    echo "  Draft Context:   $CONTEXT"
    echo "  Draft Max:       8"
    echo "  Draft Min:       0"
fi
echo ""

# Stop any running server.
# NOTE: killall only reaches processes owned by the invoking user. The systemd
# llama-server runs as user "llama", so this silently fails against it (see the
# port guard below, which is what actually catches that case).
echo "Stopping any running llama-server..."
killall llama-server 2>/dev/null
sleep 2

# Port guard. If something still holds 8080 we cannot bind it, our llama-server
# exits immediately, and every check below would be answered by the OTHER
# server -- silently benchmarking the wrong model with the wrong settings.
# This burned a full run on 2026-08-27: the systemd service (Qwen 3.6 27B,
# thinking on) answered for a Qwen 3.6 35B-A3B run and produced 7.48 tok/s /
# 19.8%, which looked like a real result and a real regression.
#
# Waits rather than checking once: a llama-server holding a 20 GB model can
# take several seconds to exit and release the socket after the killall above,
# so a single check right after `sleep 2` false-positives on a server that is
# already on its way out.
port_owner() {
    if command -v ss >/dev/null; then
        ss -ltnp 2>/dev/null | grep -E ':8080\b'
    elif command -v lsof >/dev/null; then
        lsof -iTCP:8080 -sTCP:LISTEN 2>/dev/null | tail -n +2
    fi
}

PORT_WAIT=0
while [ -n "$(port_owner)" ] && [ $PORT_WAIT -lt 10 ]; do
    [ $PORT_WAIT -eq 0 ] && echo -n "Waiting for port 8080 to be released"
    echo -n "."
    sleep 1
    PORT_WAIT=$((PORT_WAIT + 1))
done
[ $PORT_WAIT -gt 0 ] && echo ""

OWNER="$(port_owner)"
if [ -n "$OWNER" ]; then
    echo ""
    echo "❌ Port 8080 is still in use after ${PORT_WAIT}s -- refusing to run."
    echo "   Benchmarking anyway would measure whatever owns the port."
    echo ""
    echo "   Holder:"
    echo "$OWNER" | sed 's/^/     /'
    echo ""
    if echo "$OWNER" | grep -q "llama-server"; then
        echo "   That is a llama-server. If it is the systemd service (runs as"
        echo "   user 'llama', so the killall above cannot reach it):"
        echo ""
        echo "     sudo systemctl stop llama-server"
    else
        echo "   That is NOT a llama-server -- some other process is on 8080."
        echo "   Stop it, or free the port, then re-run."
    fi
    echo ""
    exit 1
fi

# Build speculative decoding flags if enabled
SPEC_FLAGS=""
if [ "$USE_SPEC_DECODING" = true ]; then
    SPEC_FLAGS="-md $DRAFT_MODEL_PATH -ngld 99 -cd $CONTEXT --draft-max 8 --draft-min 0"
fi

# Start server with model-specific settings
echo "Starting llama-server with optimized settings..."
if [ "$USE_SPEC_DECODING" = true ]; then
    echo "(Speculative decoding enabled - loading both models...)"
fi

/usr/local/bin/llama-server \
    -m "$MODEL_PATH" \
    $SPEC_FLAGS \
    --gpu-layers $GPU_LAYERS \
    -c $CONTEXT \
    -b $BATCH \
    -ub $UBATCH \
    -t $THREADS \
    --temp $TEMP \
    --top-p $TOP_P \
    --top-k $TOP_K \
    --repeat-penalty $REPEAT_PENALTY \
    $EXTRA_FLAGS \
    --host 127.0.0.1 \
    --port 8080 \
    --parallel 1 \
    --cont-batching \
    --metrics &

SERVER_PID=$!
echo "Server PID: $SERVER_PID"
echo ""

# Wait for server to start AND model to load
echo "Waiting for server to start and model to load..."
echo "(This can take 30-90 seconds for model loading...)"
MAX_WAIT=120
WAITED=0
while [ $WAITED -lt $MAX_WAIT ]; do
    # Our own server must still be alive. If it died (e.g. failed to bind the
    # port) then anything answering below belongs to someone else.
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo ""
        echo "❌ llama-server exited during startup (PID $SERVER_PID is gone)."
        echo "   Scroll up for its error output -- a failed port bind or a"
        echo "   cudaMalloc OOM are the usual causes."
        exit 1
    fi

    # Check if health returns 200 (not 503)
    HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8080/health 2>/dev/null)
    if [ "$HTTP_CODE" = "200" ]; then
        # Try a test query to make sure model is actually loaded
        TEST_RESPONSE=$(curl -s -X POST http://127.0.0.1:8080/v1/chat/completions \
            -H "Content-Type: application/json" \
            -d '{"messages":[{"role":"user","content":"test"}],"max_tokens":5}' 2>/dev/null)

        if echo "$TEST_RESPONSE" | grep -q "choices"; then
            # Identity check: confirm the server answering is serving the model
            # we asked for. Belt-and-braces against ever benchmarking a
            # pre-existing server that happens to hold the port.
            LOADED=$(curl -s http://127.0.0.1:8080/v1/models 2>/dev/null |
                     python3 -c "import sys,json
try:
    d=json.load(sys.stdin)
    print(d.get('data',[{}])[0].get('id',''))
except Exception:
    print('')" 2>/dev/null)
            if [ -n "$LOADED" ] && [ "$(basename "$LOADED")" != "$MODEL_FILE" ]; then
                echo ""
                echo "❌ Wrong model on port 8080 -- refusing to benchmark."
                echo "   requested: $MODEL_FILE"
                echo "   serving:   $(basename "$LOADED")"
                echo ""
                echo "   Another llama-server owns the port. Stop it and re-run:"
                echo "     sudo systemctl stop llama-server"
                echo ""
                kill $SERVER_PID 2>/dev/null
                exit 1
            fi
            echo ""
            echo "✅ Server ready and model loaded: ${LOADED:-$MODEL_FILE}"
            echo ""
            break
        fi
    fi
    sleep 2
    WAITED=$((WAITED + 2))
    echo -n "."
done

if [ $WAITED -ge $MAX_WAIT ]; then
    echo ""
    echo "❌ Server failed to fully load within ${MAX_WAIT}s"
    kill $SERVER_PID 2>/dev/null
    exit 1
fi

# Create results directory
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
if [ "$USE_SPEC_DECODING" = true ]; then
    RESULTS_DIR="./single_model_test_${MODEL_NAME}_spec_${TIMESTAMP}"
else
    RESULTS_DIR="./single_model_test_${MODEL_NAME}_${TIMESTAMP}"
fi
mkdir -p "$RESULTS_DIR"

# Save configuration used
if [ "$USE_SPEC_DECODING" = true ]; then
    SPEC_CONFIG="
Speculative Decoding:
  Draft Model:     $DRAFT_MODEL
  Draft GPU Layers: 99
  Draft Context:   $CONTEXT
  Draft Max:       8
  Draft Min:       0"
    SPEC_CMD="  -md \"$DRAFT_MODEL_PATH\" \\\\
  -ngld 99 \\\\
  -cd $CONTEXT \\\\
  --draft-max 8 --draft-min 0 \\\\"
else
    SPEC_CONFIG=""
    SPEC_CMD=""
fi

cat > "$RESULTS_DIR/config_used.txt" << EOF
Model: $MODEL_NAME
File: $MODEL_FILE
Timestamp: $TIMESTAMP
Speculative Decoding: $USE_SPEC_DECODING

Configuration Applied:
  GPU Layers:      $GPU_LAYERS
  Context Size:    $CONTEXT
  Batch Size:      $BATCH
  Micro-Batch:     $UBATCH
  Threads:         $THREADS
  Temperature:     $TEMP
  Top-P:           $TOP_P
  Top-K:           $TOP_K
  Repeat Penalty:  $REPEAT_PENALTY
  Extra Flags:     $EXTRA_FLAGS
$SPEC_CONFIG

Command Line:
/usr/local/bin/llama-server \\
  -m "$MODEL_PATH" \\
$SPEC_CMD
  --gpu-layers $GPU_LAYERS \\
  -c $CONTEXT \\
  -b $BATCH \\
  -ub $UBATCH \\
  -t $THREADS \\
  --temp $TEMP \\
  --top-p $TOP_P \\
  --top-k $TOP_K \\
  --repeat-penalty $REPEAT_PENALTY \\
  $EXTRA_FLAGS \\
  --host 127.0.0.1 \\
  --port 8080 \\
  --parallel 1 \\
  --cont-batching
EOF

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "SPEED TEST"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

./test_llama_performance.sh | tee "$RESULTS_DIR/speed_results.txt"

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "QUALITY TEST"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

./test_llm_quality.py | tee "$RESULTS_DIR/quality_results.txt"

# Stop server
echo ""
echo "Stopping llama-server..."
kill $SERVER_PID 2>/dev/null
sleep 2

# Parse results
TOKENS_SEC=$(grep "Tokens/sec:" "$RESULTS_DIR/speed_results.txt" | awk '{print $2}' | head -2 | tail -1)
QUALITY_SCORE=$(grep "Total Score:" "$RESULTS_DIR/quality_results.txt" | tail -1 | grep -oP '\d+/\d+')
QUALITY_PCT=$(grep "Total Score:" "$RESULTS_DIR/quality_results.txt" | tail -1 | grep -oP '\(\K[0-9.]+')

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
if [ "$USE_SPEC_DECODING" = true ]; then
    echo "SUMMARY: $MODEL_NAME (+ spec decoding)"
else
    echo "SUMMARY: $MODEL_NAME"
fi
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "Speed:   $TOKENS_SEC tokens/sec"
echo "Quality: $QUALITY_SCORE ($QUALITY_PCT%)"
if [ "$USE_SPEC_DECODING" = true ]; then
    echo "Mode:    Speculative decoding (draft: $DRAFT_MODEL_NAME)"
fi
echo ""

if [ -n "$TOKENS_SEC" ] && [ -n "$QUALITY_PCT" ]; then
    if (( $(echo "$TOKENS_SEC >= 25 && $QUALITY_PCT >= 80" | bc -l 2>/dev/null || echo 0) )); then
        echo "✅ RECOMMENDED FOR DAWN"
    elif (( $(echo "$TOKENS_SEC >= 20 && $QUALITY_PCT >= 70" | bc -l 2>/dev/null || echo 0) )); then
        echo "⚠️  ACCEPTABLE"
    else
        echo "❌ NOT RECOMMENDED"
    fi
fi

echo ""
echo "Results saved to: $RESULTS_DIR/"
echo "Configuration saved to: $RESULTS_DIR/config_used.txt"
echo ""
