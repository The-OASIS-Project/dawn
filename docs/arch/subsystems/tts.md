# TTS Subsystem

Source: `src/tts/`, `include/tts/`

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

**Purpose**: High-quality text-to-speech synthesis.

## Key Components

- **text_to_speech.cpp/h**: TTS engine wrapper
   - Thread-safe interface with mutex protection (`tts_mutex`)
   - Converts text to WAV audio
   - Text preprocessing for natural phrasing (em-dash conversion, etc.)
   - Supports streaming integration with LLM sentence buffer

- **`third_party/piper/piper.cpp`**: Piper TTS integration (vendored)
   - Uses Piper library with ONNX Runtime
   - Phoneme-based synthesis
   - Multiple voice models supported
   - Recommended: `en_GB-alba-medium` (good quality, reasonable speed)

## Data Flow

```
Text → Preprocessing → Piper Phonemization → ONNX Inference → WAV Audio → ALSA/Pulse
```

## Thread Safety

TTS is protected by a global mutex (`tts_mutex`) to prevent concurrent access from:

- Main loop (local audio)
- Network server thread (remote audio)
- Streaming LLM sentence buffer
