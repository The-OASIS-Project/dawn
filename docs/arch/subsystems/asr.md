# ASR Subsystem

Source: `src/asr/`, `include/asr/`, plus shared engines in `common/src/asr/` (Whisper, Silero VAD — reused by the satellite)

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

**Purpose**: Speech recognition with multiple engine support.

## Architecture Pattern: Strategy Pattern

The ASR subsystem uses an abstraction layer (`asr_interface`) to support multiple ASR engines (Whisper, Vosk) interchangeably.

## Key Components

- **`src/asr/asr_interface.c/h`**: ASR abstraction layer
   - `ASRContext` struct: engine-agnostic context
   - `asr_init()`: initialize selected ASR engine
   - `asr_process_audio()`: process audio through selected engine
   - `asr_cleanup()`: clean up resources
   - Engine selection based on compile-time flags (`ENABLE_VOSK`)

- **`common/src/asr/asr_whisper.c`, `common/include/asr/asr_whisper.h`**: Whisper ASR implementation (shared with satellite via the common library)
   - Uses whisper.cpp library
   - GPU acceleration on Jetson (CUDA)
   - Support for multiple model sizes (tiny, base, small)
   - Recommended: **base.en** (best balance of speed/accuracy)
   - VAD-driven pause detection for natural speech boundaries

- **`src/asr/asr_vosk.c`, `include/asr/asr_vosk.h`**: Vosk ASR implementation (optional, legacy)
   - Uses Vosk API with Kaldi backend
   - GPU-accelerated when available (via `vosk_gpu_init()`)
   - Smaller memory footprint than Whisper
   - Compiled only when `ENABLE_VOSK=ON`
   - A second copy lives in `common/src/asr/asr_vosk.c` for the satellite build

- **`common/src/asr/vad_silero.c`, `common/include/asr/vad_silero.h`**: Voice Activity Detection (shared)
   - Uses Silero VAD ONNX model
   - Real-time speech/silence classification
   - Drives pause detection for chunking
   - Configurable sensitivity threshold

- **`src/asr/chunking_manager.c`, `include/asr/chunking_manager.h`**: Long utterance handling
   - Manages multi-chunk speech sequences
   - VAD-driven pause detection
   - Assembles partial results into complete transcriptions
   - Prevents premature cutoff of long commands

- **`src/core/endpointer.c`, `include/core/endpointer.h`**: End-of-speech decision for the local microphone (pure, unit-tested by trace replay)
   - Fed one speech/silence flag per frame; reports a tentative end at a short pause (`[vad.chunking] pause_duration`), a cancel when speech resumes, and the commit
   - Commits after `[vad] end_of_speech_duration` of silence, or at `max_recording_duration` (which wins over a cancel)
   - The tentative and cancel events change no timing today; they are logged so the pauses inside commands can be measured

## Data Flow

```
Audio Input → VAD (Silero) → Chunking Manager → ASR Engine (Whisper/Vosk) → Transcript
                   ↓                                      ↑
              Speech/Silence                         GPU Acceleration
              Classification                         (Jetson + Vosk support)
```

## Performance

| Model         | Platform   | RTF   | Speedup | Accuracy  |
| ------------- | ---------- | ----- | ------- | --------- |
| Whisper tiny  | Jetson GPU | 0.079 | 12.7x   | Good      |
| Whisper base  | Jetson GPU | 0.109 | 9.2x    | Excellent |
| Whisper small | Jetson GPU | 0.225 | 4.4x    | Best      |
| Vosk 0.22     | CPU/GPU    | ~0.15 | 6.7x    | Good      |

**RTF = Real-Time Factor** (lower is faster; 1.0 = realtime, <1.0 = faster than realtime).
