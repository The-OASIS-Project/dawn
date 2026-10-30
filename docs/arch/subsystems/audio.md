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

- **music_speaker.c/h**: The local speaker
   - A music player (below) on its owner's queue: the local device's user (`[memory]
     default_voice_user_id`, or whoever the local mic is assigned to). The owner's WebUI tabs
     show and edit the same queue; a change of owner stops the old owner's music and plays the
     new owner's queue on the next request
   - Its output plays through the audio backend (ALSA or PulseAudio), converting to the
     device's rate; the device opens when a track starts and closes on a pause, seek or stop,
     and switching the playback device moves playing music to it
   - Holds the speaker's volume (`setMusicVolume`/`getMusicVolume`) and ducking, applied to the
     samples as they're written (ducking never changes the set volume)
   - Who may play on it is the music tool's rule (`music_tool_speaker_allowed`): the owner,
     from the local mic, a messaging channel or no session (the scheduler, MQTT)

- **music_open.c/h**: Opening a library track, shared by every player
   - `music_path_is_valid()`: a local path must resolve inside `[paths] music_dir`; a `plex:`
     path must be a well-formed Plex part key (no authority, query, fragment or traversal)
   - `music_open_track()`: the check, then a Plex download to a temp file (cancellable,
     unlinked once open) or a local open
   - The WebUI stream adds its own sample-rate and channel limits on top

- **music_nav.c/h**: What a player plays next, shared by every player
   - A play history (back and forward under shuffle), in memory, cleared when the queue is replaced or cleared
   - `music_nav_pick()`: end of queue, repeat-all/one, failure caps (`MUSIC_NAV_MAX_FAILED`),
     shuffle; pure, the caller holds its queue's lock

- **music_player.c, music_player_track.c, music_player.h**: The music player
   - Plays a user's queue (shared with the user's other players; each keeps its own place,
     which follows their edits) to one output given as `music_output_ops_t`: `track_begin`,
     `write` (paces the player), `halt` (pause, flush, idle) and `notify` (state, position,
     error), each on the player's thread with no player lock held
   - Its thread opens the pending track (`music_open_track()`), applies seeks, reads the
     decoder, writes to the output, and at a track's end picks the next with `music_nav_pick()`,
     skipping tracks that won't open; a helper prefetches the next remote track
   - Controls under its control lock (play, step, stop, remove, clear, await a track's
     outcome) and without it (pause, seek, snapshot); internals in `music_player_internal.h`
   - Used by the WebUI stream (`webui_music.c` is its output) and the local speaker
     (`music_speaker.c`)

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
   - Path-to-source detection (`music_source_from_path()`), and `music_source_is_remote()`
     (downloaded before it plays: prefetched, and its server can be unreachable)

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
