# Audio Subsystem

Source: `src/audio/`, `include/audio/`. The thread-safe ring buffer lives in `common/src/audio/ring_buffer.c` because the satellite uses it too.

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

**Purpose**: Audio capture, buffering, and playback.

## Key Components

- **audio_capture_thread.c/h**: Dedicated audio capture thread
   - Runs in separate thread to avoid blocking main loop
   - Continuous capture from ALSA/PulseAudio device
   - Writes to ring buffer for main loop consumption
   - Handles capture errors gracefully

- **`common/src/audio/ring_buffer.c`, `common/include/audio/ring_buffer.h`**: Thread-safe circular buffer (shared with satellite)
   - Lock-free or mutex-protected (implementation dependent)
   - Fixed-size buffer for audio samples
   - Overwrite policy when buffer full
   - Used for smooth audio streaming between threads

- **flac_playback.c/h**: Music/audio file playback
   - Multi-format decoding (FLAC, MP3, Ogg Vorbis) via unified audio_decoder API
   - ALSA/PulseAudio output with automatic sample rate conversion
   - Used for notification sounds, music playback

- **music_open.c/h**: Opening a library track, shared by every player
   - `music_path_is_valid()`: a local path must resolve inside `[paths] music_dir`; a `plex:`
     path must be a well-formed Plex part key (no authority, query, fragment or traversal)
   - `music_open_track()`: the check, then a Plex download to a temp file (cancellable,
     unlinked once open) or a local open
   - The WebUI stream adds its own sample-rate and channel limits on top

- **music_db.c/h**: Unified music metadata database
   - SQLite-based cache for artist/title/album/genre tags with source tracking
   - Multi-source support (local files + Plex) via `music_source_t` enum
   - Priority-based deduplication (local wins over Plex for same artist+album+title)
   - COLLATE NOCASE matching on dedup index for cross-source consistency
   - Indexed search across metadata fields (including genre) with LIKE escaping
   - Incremental scanning with source-scoped stale deletion
   - Automatic cleanup of deleted files (per-source)

- **music_source.c/h**: Music source abstraction layer
   - `music_source_provider_t` callback interface for pluggable sources
   - Source name/prefix helpers (`music_source_name()`, `music_source_path_prefix()`)
   - Path-to-source detection (`music_source_from_path()`)

- **plex_db.c/h**: Plex-to-unified-DB sync layer
   - Fetches all Plex tracks via `plex_client_list_all_tracks()`
   - Inserts into unified `music_metadata` table with `source = MUSIC_SOURCE_PLEX`
   - Runs during music scanner cycle when Plex is configured

- **music_scanner.c/h**: Background music library scanner
   - Dedicated thread for non-blocking scans
   - Configurable scan interval (default: 60 minutes)
   - Scans local files and syncs Plex library in each cycle
   - Manual rescan trigger via admin socket
   - Mutex/condvar synchronization for thread safety

- **plex_client.c/h**: Plex Media Server REST API client
   - Authentication via X-Plex-Token header
   - `plex_client_list_all_tracks()` for bulk sync into unified DB
   - Stream URL construction for download-to-temp playback
   - Scrobble reporting on track completion
   - Server discovery and connection testing

- **http_download.c/h**: HTTP download-to-temp utility
   - libcurl-based download with 300s hard timeout
   - Used by Plex client to fetch tracks for audio_decoder

- **mic_passthrough.c/h**: Microphone passthrough
   - Direct microphone → speaker routing
   - Used for testing, debugging
   - Useful for verifying audio capture/playback setup

## Threading Model

```
┌──────────────────┐
│ Capture Thread   │ (continuous capture)
│                  │
│ ALSA/Pulse → RB  │ (write to ring buffer)
└────────┬─────────┘
         │
    Ring Buffer (thread-safe)
         │
┌────────▼─────────┐
│   Main Thread    │ (state machine)
│                  │
│   RB → VAD → ASR │ (read from ring buffer)
└──────────────────┘
```

**RB = Ring Buffer**.
