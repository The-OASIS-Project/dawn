#!/bin/bash
#
# DAWN Model Setup Script
#
# This script downloads and configures the required ML models for DAWN:
# - Whisper ASR models (speech recognition) - downloaded from Hugging Face
# - Vosk model (optional legacy ASR) - downloaded from alphacephei.com
#
# Note: TTS (Piper) and VAD (Silero) models are committed to git and don't
# need to be downloaded.
#
# Usage: ./setup_models.sh [options]
#
# Options:
#   --vosk            Include large Vosk model (~1.8GB, higher accuracy)
#   --vosk-small      Include small Vosk model (~40MB, recommended for satellite)
#   --whisper-model   Whisper model size: tiny, base, small, medium (default: base)
#   --help            Show this help message
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default options
WHISPER_MODEL="base"
INCLUDE_VOSK=false
VOSK_VARIANT=""

# Project root (where this script lives)
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Model directories
MODELS_DIR="$PROJECT_ROOT/models"
WHISPER_DIR="$PROJECT_ROOT/whisper.cpp"
VOSK_LARGE_DIR="$PROJECT_ROOT/vosk-model-en-us-0.22"
VOSK_SMALL_DIR="$PROJECT_ROOT/vosk-model-small-en-us-0.15"

print_header() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}  DAWN Model Setup${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""
}

print_step() {
    echo -e "${GREEN}[*]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[!]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

show_help() {
    echo "DAWN Model Setup Script"
    echo ""
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  --vosk-small         Include small Vosk model (~40MB, satellite default)"
    echo "  --vosk               Include large Vosk model (~1.8GB, higher accuracy)"
    echo "  --whisper-model SIZE Whisper model (default: base)"
    echo "                       Accepts: tiny, base, small, medium"
    echo "                       Quantized: tiny-q5_1, base-q5_1, etc."
    echo "  --help               Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0                               # Standard setup (Whisper base)"
    echo "  $0 --vosk-small                  # Add Vosk small (satellite default)"
    echo "  $0 --whisper-model tiny-q5_1     # Quantized tiny (best for Pi 4)"
    echo "  $0 --whisper-model tiny          # Tiny English-only"
    echo "  $0 --vosk                        # Include large Vosk model"
    echo ""
    echo "Model Sizes (approximate, English-only):"
    echo "  Whisper tiny-q5_1: ~30MB   (fastest, recommended for Pi 4)"
    echo "  Whisper tiny:      ~75MB   (fast, good for short commands)"
    echo "  Whisper base:      ~142MB  (recommended for Jetson GPU)"
    echo "  Whisper small:     ~466MB  (better accuracy, slower)"
    echo "  Whisper medium:    ~1.5GB  (best accuracy, slowest)"
    echo "  Vosk small-en-us:  ~40MB   (streaming, recommended for satellite)"
    echo "  Vosk en-us:        ~1.8GB  (streaming, higher accuracy)"
    echo ""
    echo "Note: TTS (Piper) and VAD (Silero) models are committed to git."
    echo ""
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            --vosk-small)
                INCLUDE_VOSK=true
                VOSK_VARIANT="small"
                shift
                ;;
            --vosk)
                INCLUDE_VOSK=true
                VOSK_VARIANT="large"
                shift
                ;;
            --whisper-model)
                WHISPER_MODEL="$2"
                shift 2
                ;;
            --help)
                show_help
                exit 0
                ;;
            *)
                print_error "Unknown option: $1"
                show_help
                exit 1
                ;;
        esac
    done

    # Parse whisper model: split "tiny-q5_1" into base="tiny" quant="-q5_1"
    WHISPER_BASE="${WHISPER_MODEL%%-q*}"
    if [ "$WHISPER_BASE" != "$WHISPER_MODEL" ]; then
        WHISPER_QUANT="-${WHISPER_MODEL#*-}"
    else
        WHISPER_QUANT=""
    fi

    # Validate base model size
    case $WHISPER_BASE in
        tiny|base|small|medium) ;;
        *)
            print_error "Invalid Whisper model: $WHISPER_MODEL"
            echo "Valid base sizes: tiny, base, small, medium"
            echo "Quantized variants: tiny-q5_1, tiny-q5_0, tiny-q8_0, etc."
            exit 1
            ;;
    esac

    # Validate quantization suffix if present
    if [ -n "$WHISPER_QUANT" ]; then
        case $WHISPER_QUANT in
            -q5_0|-q5_1|-q8_0) ;;
            *)
                print_error "Invalid quantization: $WHISPER_QUANT"
                echo "Valid quantizations: q5_0, q5_1, q8_0"
                exit 1
                ;;
        esac
    fi
}

check_dependencies() {
    print_step "Checking dependencies..."

    local missing=()

    command -v wget >/dev/null 2>&1 || missing+=("wget")
    command -v unzip >/dev/null 2>&1 || missing+=("unzip")
    command -v git >/dev/null 2>&1 || missing+=("git")

    if [ ${#missing[@]} -ne 0 ]; then
        print_error "Missing required tools: ${missing[*]}"
        echo "Install with: sudo apt install ${missing[*]}"
        exit 1
    fi

    print_success "All dependencies found"
}

setup_models_directory() {
    print_step "Setting up models directory..."

    mkdir -p "$MODELS_DIR"

    print_success "Models directory ready: $MODELS_DIR"
}

setup_whisper() {
    print_step "Setting up Whisper ASR..."

    # Check if whisper.cpp exists
    if [ ! -d "$WHISPER_DIR" ]; then
        print_warning "whisper.cpp not found at $WHISPER_DIR"
        echo "  Whisper should be set up as a git submodule."
        echo "  Run: git submodule update --init --recursive"
        return 1
    fi

    # Create models directory in whisper.cpp if needed
    mkdir -p "$WHISPER_DIR/models"

    # Build the model filename
    # Format: ggml-{base}.en{-quant}.bin  (e.g., ggml-tiny.en-q5_1.bin)
    local model_filename="ggml-${WHISPER_BASE}.en${WHISPER_QUANT}.bin"
    local model_file="$WHISPER_DIR/models/$model_filename"

    if [ -f "$model_file" ]; then
        print_success "Whisper $WHISPER_MODEL model already exists: $model_filename"
    else
        print_step "Downloading Whisper $WHISPER_MODEL model ($model_filename)..."

        local model_url="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/$model_filename"

        wget -q --show-progress -O "$model_file" "$model_url" || {
            print_error "Failed to download Whisper model from $model_url"
            rm -f "$model_file"
            return 1
        }

        print_success "Whisper $WHISPER_MODEL model downloaded: $model_filename"
    fi

    # Create symlink in models directory
    local symlink="$MODELS_DIR/whisper.cpp"

    if [ -L "$symlink" ]; then
        print_success "Symlink already exists: models/whisper.cpp"
    elif [ -e "$symlink" ]; then
        print_error "$symlink exists but is not a symlink. Please remove manually."
        return 1
    else
        # Create relative symlink
        ln -s "../whisper.cpp/models" "$symlink"
        print_success "Created symlink: models/whisper.cpp -> ../whisper.cpp/models"
    fi
}

setup_vosk() {
    if [ "$INCLUDE_VOSK" != true ]; then
        return 0
    fi

    local vosk_model_name
    local vosk_dir
    local vosk_url
    local vosk_size

    if [ "$VOSK_VARIANT" = "small" ]; then
        vosk_model_name="vosk-model-small-en-us-0.15"
        vosk_dir="$VOSK_SMALL_DIR"
        vosk_url="https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip"
        vosk_size="~40MB"
    else
        vosk_model_name="vosk-model-en-us-0.22"
        vosk_dir="$VOSK_LARGE_DIR"
        vosk_url="https://alphacephei.com/vosk/models/vosk-model-en-us-0.22.zip"
        vosk_size="~1.8GB"
    fi

    print_step "Setting up Vosk ASR ($vosk_model_name, $vosk_size)..."

    if [ -d "$vosk_dir" ]; then
        print_success "Vosk model already exists: $vosk_model_name"
    else
        print_step "Downloading Vosk model ($vosk_size, this may take a while)..."

        local vosk_zip="$PROJECT_ROOT/${vosk_model_name}.zip"

        wget -q --show-progress -O "$vosk_zip" "$vosk_url" || {
            print_error "Failed to download Vosk model"
            rm -f "$vosk_zip"
            return 1
        }

        print_step "Extracting Vosk model..."
        unzip -q "$vosk_zip" -d "$PROJECT_ROOT"
        rm "$vosk_zip"

        print_success "Vosk model downloaded and extracted"
    fi

    # Create symlink in models directory
    local symlink="$MODELS_DIR/$vosk_model_name"

    if [ -L "$symlink" ]; then
        print_success "Symlink already exists: models/$vosk_model_name"
    elif [ -e "$symlink" ]; then
        print_error "$symlink exists but is not a symlink. Please remove manually."
        return 1
    else
        ln -s "../$vosk_model_name" "$symlink"
        print_success "Created symlink: models/$vosk_model_name -> ../$vosk_model_name"
    fi
}

verify_committed_models() {
    print_step "Verifying committed models (TTS/VAD)..."

    # Check for Piper voice model (committed to git)
    local piper_model="$MODELS_DIR/en_GB-alba-medium.onnx"
    if [ -f "$piper_model" ]; then
        print_success "Piper TTS voice exists: en_GB-alba-medium.onnx"
    else
        print_warning "Piper TTS voice not found - should be in git"
        echo "  Expected: $piper_model"
    fi

    # Check for VAD model (committed to git)
    local vad_model="$MODELS_DIR/silero_vad_16k_op15.onnx"
    if [ -f "$vad_model" ]; then
        print_success "Silero VAD model exists: silero_vad_16k_op15.onnx"
    else
        print_warning "Silero VAD model not found - should be in git"
        echo "  Expected: $vad_model"
    fi
}

setup_build_symlink() {
    print_step "Setting up build directory symlink..."

    local build_dir="$PROJECT_ROOT/build"

    if [ ! -d "$build_dir" ]; then
        print_warning "Build directory not found. Create it with: mkdir build && cd build && cmake .."
        return 0
    fi

    local build_symlink="$build_dir/models"

    if [ -L "$build_symlink" ]; then
        print_success "Build models symlink already exists"
    elif [ -e "$build_symlink" ]; then
        print_warning "$build_symlink exists but is not a symlink"
    else
        ln -s "../models" "$build_symlink"
        print_success "Created symlink: build/models -> ../models"
    fi
}

print_summary() {
    echo ""
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}  Setup Complete!${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""
    echo "Models directory structure:"
    echo ""
    ls -la "$MODELS_DIR" 2>/dev/null | grep -v "^total" | while read line; do
        echo "  $line"
    done
    echo ""
    echo "Next steps:"
    echo "  1. Build DAWN:  cd build && cmake .. && make -j8"
    echo "  2. Run DAWN:    cd build && ./dawn"
    echo ""

    if [ "$INCLUDE_VOSK" != true ]; then
        echo "Note: Vosk was not installed. To add Vosk support later:"
        echo "  ./setup_models.sh --vosk-small   # ~40MB, recommended for satellite"
        echo "  ./setup_models.sh --vosk         # ~1.8GB, higher accuracy"
        echo ""
    fi
}

# Main
main() {
    parse_args "$@"

    print_header

    echo "Configuration:"
    echo "  Whisper model: $WHISPER_MODEL"
    echo "  Include Vosk:  $INCLUDE_VOSK${VOSK_VARIANT:+ ($VOSK_VARIANT)}"
    echo ""

    check_dependencies
    setup_models_directory
    setup_whisper
    setup_vosk
    verify_committed_models
    setup_build_symlink

    print_summary
}

main "$@"
