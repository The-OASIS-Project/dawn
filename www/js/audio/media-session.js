/*
 * DAWN WebUI — OS media-session integration
 *
 * Bridges DAWN's server-side music playback to the browser's
 * navigator.mediaSession API so the OS recognizes DAWN as the active
 * media player.  Effect: hardware media keys (Play/Pause/Next/Prev on
 * keyboards, headphones, headsets), lock-screen widgets, and OS
 * notification controls all route to DAWN's music transport.
 *
 * The mechanism: MediaSession action handlers only fire when the
 * browser thinks an `<audio>` (or `<video>`) element is actually
 * playing.  DAWN streams audio server-side (the daemon plays through
 * ALSA on Jetson), so the browser never sees a real HTMLMediaElement
 * in the playing state.  This module creates a hidden silent looping
 * `<audio>` and plays/pauses it in lock-step with DawnMusicPlayback's
 * state — keeps the browser's media-session slot held by DAWN.
 *
 * Browser indicator caveat: Chrome (and other browsers) light up the
 * per-tab "this tab is making sound" speaker icon whenever any
 * <audio> element is in the playing state, regardless of volume.
 * The DAWN tab will therefore show the speaker glyph while music
 * plays even though the actual audio is server-side ALSA.  This is
 * honest at the browser-API level (we ARE the active media player)
 * but worth knowing so it doesn't read as a bug.
 *
 * Scope: WebUI-only.  Doesn't affect the local Jetson keyboard or
 * the satellite/voice paths; the OS routes keys to whichever browser
 * tab last successfully held the media-session slot.  Multi-tab
 * contention (DAWN open in two tabs) hands the slot to the most
 * recently primed tab; coordinating tabs over a BroadcastChannel would
 * fix that and is not built.
 *
 * Browser autoplay gating: HTMLMediaElement.play() is rejected when
 * called without a prior user-gesture chain.  This module exposes
 * primeOnGesture() that the music UI calls from click handlers
 * (open panel, play button) to satisfy the gesture requirement —
 * subsequent setPlaying(true) calls then work without rejection.
 *
 * Lifecycle: init() creates the silent audio element and registers
 * action handlers; shutdown() reverses both and revokes the Blob URL.
 * The module also wires a `pagehide` listener that calls shutdown()
 * on SPA navigation / bfcache eviction.
 *
 * Diagnostics: set window.DAWN_MEDIA_SESSION_DEBUG = true OR
 * localStorage.setItem('dawn_media_session_debug', 'true') (the
 * localStorage form survives hard-refresh).  Look for [media-session]
 * console lines tracking audio lifecycle + setMetadata calls + any
 * rejected play() promise.
 */
(function (global) {
   'use strict';

   let silentAudio = null;
   let silentAudioUrl = null;
   let actionCallbacks = null;
   let initialized = false;
   let primedOnGesture = false;
   /* Cached last-applied metadata.  setMetadata is called on every
    * music_state push (multiple times per second for position
    * updates); rebuilding the MediaMetadata object and assigning it
    * triggers an OS-surface repaint even when title/artist/album
    * haven't actually changed.  Cache the tuple and skip the
    * MediaMetadata assignment when unchanged — still call
    * setPositionState (cheap, browser expects frequent updates). */
   let lastMeta = { title: '', artist: '', album: '' };
   /* List of MediaSession action names we register, so shutdown can
    * unregister them by name.  Single source of truth — adding a new
    * action means adding the name here. */
   const REGISTERED_ACTIONS = ['play', 'pause', 'previoustrack', 'nexttrack', 'stop', 'seekto'];

   /* Check both an in-memory flag (set via devtools console for ad-hoc
    * diagnostics: `window.DAWN_MEDIA_SESSION_DEBUG = true`) AND a
    * localStorage flag that survives hard-refresh:
    *   localStorage.setItem('dawn_media_session_debug', 'true')
    * The localStorage form is the recommended diagnostic switch since
    * console-set globals get wiped by page reload — early init/prime
    * logs would otherwise be missed. */
   function isDebugEnabled() {
      if (global.DAWN_MEDIA_SESSION_DEBUG) return true;
      try {
         return (
            typeof localStorage !== 'undefined' &&
            localStorage.getItem('dawn_media_session_debug') === 'true'
         );
      } catch (e) {
         /* localStorage access can throw in privacy modes; ignore. */
         return false;
      }
   }

   function debug() {
      if (isDebugEnabled()) {
         /* eslint-disable-next-line no-console */
         console.log.apply(console, ['[media-session]'].concat([].slice.call(arguments)));
      }
   }

   /* Build a 1-second silent PCM8 WAV in memory and return a blob URL.
    * Sized to be unambiguously "long enough" for browsers to count
    * the audio element as actively playing — a sub-100ms loop has
    * been observed to glitch the "is media playing" state in some
    * browsers.  1 second of 8 kHz mono PCM8 = ~8 KB Blob, fits in
    * a single TCP segment if it were ever served from network. */
   function buildSilentWavBlobUrl() {
      const sampleRate = 8000;
      const samples = sampleRate; /* 1 second */
      const dataSize = samples; /* PCM8: 1 byte per sample */
      const buf = new ArrayBuffer(44 + dataSize);
      const view = new DataView(buf);

      function writeAscii(offset, str) {
         for (let i = 0; i < str.length; i++) {
            view.setUint8(offset + i, str.charCodeAt(i));
         }
      }

      writeAscii(0, 'RIFF');
      view.setUint32(4, 36 + dataSize, true);
      writeAscii(8, 'WAVE');
      writeAscii(12, 'fmt ');
      view.setUint32(16, 16, true); /* subchunk1 size */
      view.setUint16(20, 1, true); /* PCM */
      view.setUint16(22, 1, true); /* mono */
      view.setUint32(24, sampleRate, true);
      view.setUint32(28, sampleRate, true); /* byte rate */
      view.setUint16(32, 1, true); /* block align */
      view.setUint16(34, 8, true); /* bits per sample */
      writeAscii(36, 'data');
      view.setUint32(40, dataSize, true);
      /* PCM8 unsigned: 0x80 = zero amplitude (silence). */
      for (let i = 0; i < samples; i++) {
         view.setUint8(44 + i, 0x80);
      }

      return URL.createObjectURL(new Blob([buf], { type: 'audio/wav' }));
   }

   function buildSilentAudio() {
      const el = document.createElement('audio');
      silentAudioUrl = buildSilentWavBlobUrl();
      el.src = silentAudioUrl;
      el.loop = true;
      el.preload = 'auto';
      /* Non-zero volume.  Some browsers (Safari, certain Chromium
       * builds) treat volume:0 elements as "not really playing
       * audio" and skip claiming the media-session slot.  0.0001
       * is imperceptible (digital silence anyway) but reads as
       * audible to the slot-tracking heuristic. */
      el.volume = 0.0001;
      el.muted = false;
      el.setAttribute('aria-hidden', 'true');
      el.style.display = 'none';

      /* Diagnostic events — only fire under window.DAWN_MEDIA_SESSION_DEBUG. */
      ['canplay', 'play', 'playing', 'pause', 'ended', 'stalled', 'waiting'].forEach(function (ev) {
         el.addEventListener(ev, function () {
            debug('audio:', ev, 'paused=', el.paused, 'readyState=', el.readyState);
         });
      });
      el.addEventListener('error', function () {
         debug('audio: error', el.error && el.error.code, el.error && el.error.message);
      });

      document.body.appendChild(el);
      return el;
   }

   function isSupported() {
      return (
         typeof navigator !== 'undefined' &&
         'mediaSession' in navigator &&
         typeof MediaMetadata !== 'undefined'
      );
   }

   /**
    * Register action handlers and create the silent <audio> sink.
    *
    * @param {Object} callbacks - Handlers for OS media-key events.
    *   All optional; unset handlers leave the corresponding OS
    *   action greyed out / unhandled by DAWN.
    *   - onPlay()         — Play/resume
    *   - onPause()        — Pause
    *   - onPrevious()     — Previous track
    *   - onNext()         — Next track
    *   - onStop()         — Stop
    *   - onSeekTo({time}) — Seek to absolute position (seconds);
    *                        only invoked when duration is known.
    */
   function init(callbacks) {
      if (initialized) return;
      if (!isSupported()) {
         debug('init: navigator.mediaSession unsupported, no-op');
         return;
      }
      actionCallbacks = callbacks || {};
      silentAudio = buildSilentAudio();

      const ms = navigator.mediaSession;
      function setHandler(action, fn) {
         /* setActionHandler throws on unsupported actions in some
          * browsers; wrap defensively so a single unknown action
          * doesn't kill the rest. */
         try {
            ms.setActionHandler(action, fn || null);
         } catch (e) {
            debug('setHandler:', action, 'unsupported');
         }
      }

      setHandler('play', function () {
         debug('action: play');
         if (actionCallbacks.onPlay) actionCallbacks.onPlay();
      });
      setHandler('pause', function () {
         debug('action: pause');
         if (actionCallbacks.onPause) actionCallbacks.onPause();
      });
      setHandler('previoustrack', function () {
         debug('action: previoustrack');
         if (actionCallbacks.onPrevious) actionCallbacks.onPrevious();
      });
      setHandler('nexttrack', function () {
         debug('action: nexttrack');
         if (actionCallbacks.onNext) actionCallbacks.onNext();
      });
      setHandler('stop', function () {
         debug('action: stop');
         if (actionCallbacks.onStop) actionCallbacks.onStop();
      });
      setHandler('seekto', function (e) {
         debug('action: seekto', e.seekTime);
         if (actionCallbacks.onSeekTo && typeof e.seekTime === 'number') {
            actionCallbacks.onSeekTo({ time: e.seekTime });
         }
      });
      initialized = true;
      debug('init: ready');
   }

   /**
    * Prime the silent audio on a user gesture so the browser's
    * autoplay policy allows subsequent setPlaying() calls.  Call
    * from inside event handlers attached to user-clickable
    * elements (panel open, play button, etc.).  Idempotent — only
    * the first successful play() matters; later calls are no-ops.
    *
    * Without this, the chain
    *   user clicks Play → server round-trip → state callback →
    *   setPlaying(true) → audio.play()
    * loses the user-gesture provenance across the async boundary
    * and audio.play() gets rejected.
    */
   function primeOnGesture() {
      if (!isSupported() || !silentAudio || primedOnGesture) return;
      debug('primeOnGesture: attempting initial play()');
      /* Set the guard SYNCHRONOUSLY (before the play() promise
       * resolves) so a second user gesture fired before the first
       * resolves doesn't queue a redundant play() call.  Reset only
       * on rejection so a real failure retries on the next gesture. */
      primedOnGesture = true;
      const p = silentAudio.play();
      if (p && typeof p.then === 'function') {
         p.then(function () {
            debug('primeOnGesture: play() resolved, slot claimed');
            /* Pause immediately so we don't hold the slot before
             * DAWN actually has something to play.  The slot is
             * claimed by the successful play() call; subsequent
             * setPlaying(true) will play() again without
             * autoplay-gating. */
            silentAudio.pause();
         }).catch(function (err) {
            debug('primeOnGesture: play() rejected:', err && err.name, err && err.message);
            primedOnGesture = false;
         });
      } else {
         /* Older browser without play() promise — assume success. */
         silentAudio.pause();
      }
   }

   /**
    * Mirror DAWN's playback state so the browser holds (or releases)
    * the OS media-session slot accordingly.
    *
    * @param {boolean} playing - true while DAWN is playing, false on
    *   pause / stop / no-queue.
    */
   function setPlaying(playing) {
      if (!isSupported() || !silentAudio) return;
      if (playing) {
         const p = silentAudio.play();
         if (p && typeof p.catch === 'function') {
            p.catch(function (err) {
               debug('setPlaying(true): play() rejected:', err && err.name);
            });
         }
         navigator.mediaSession.playbackState = 'playing';
         debug('setPlaying: playing');
      } else {
         silentAudio.pause();
         navigator.mediaSession.playbackState = 'paused';
         debug('setPlaying: paused');
      }
   }

   /**
    * Populate the metadata visible in the OS notification / lock-
    * screen widget.  Pass undefined fields as empty strings — the OS
    * shows "Unknown" rather than the literal "undefined".
    *
    * @param {Object} info
    *   - title, artist, album (strings)
    *   - artwork (optional: array of {src, sizes, type})
    *   - duration (optional: total length in seconds; enables
    *     OS-side seek scrubber)
    *   - position (optional: current playback position, seconds)
    */
   function setMetadata(info) {
      if (!isSupported()) return;
      const nextTitle = info.title || '';
      const nextArtist = info.artist || '';
      const nextAlbum = info.album || '';
      /* Skip the MediaMetadata assignment when title/artist/album
       * haven't changed.  Saves an OS-surface repaint on the
       * many-times-per-second position updates that arrive while
       * a track is playing.  setPositionState below still runs —
       * that's what powers the seek scrubber and the OS expects it
       * frequently. */
      if (
         nextTitle !== lastMeta.title ||
         nextArtist !== lastMeta.artist ||
         nextAlbum !== lastMeta.album
      ) {
         navigator.mediaSession.metadata = new MediaMetadata({
            title: nextTitle,
            artist: nextArtist,
            album: nextAlbum,
            artwork: info.artwork || [],
         });
         lastMeta = { title: nextTitle, artist: nextArtist, album: nextAlbum };
         debug('setMetadata:', nextTitle, '—', nextArtist);
      }
      /* Position state powers the OS seek scrubber when the host
       * surface supports it (macOS Now Playing, Chrome's media
       * notification on desktop).  setPositionState is wrapped in
       * try/catch because some browsers throw on missing duration
       * or position values outside [0, duration].  isFinite check
       * rejects Infinity/NaN which a few browsers throw on. */
      if (typeof info.duration === 'number' && isFinite(info.duration) && info.duration > 0) {
         try {
            navigator.mediaSession.setPositionState({
               duration: info.duration,
               position: typeof info.position === 'number' ? info.position : 0,
               playbackRate: 1.0,
            });
         } catch (e) {
            debug('setPositionState: rejected', e && e.message);
         }
      }
   }

   /**
    * Wipe metadata + release the media-session slot.  Call when
    * playback fully stops (queue empty, unsubscribed, stopped on a
    * track).  Lighter than shutdown() — keeps the <audio> element +
    * Blob URL alive for the next play start, just clears the
    * surface-visible state.
    */
   function clearMetadata() {
      if (!isSupported()) return;
      navigator.mediaSession.metadata = null;
      navigator.mediaSession.playbackState = 'none';
      lastMeta = { title: '', artist: '', album: '' };
      if (silentAudio) {
         silentAudio.pause();
         silentAudio.currentTime = 0;
      }
      debug('clearMetadata');
   }

   /**
    * Full teardown — clears metadata, removes the silent <audio>
    * element from the DOM, revokes the Blob URL, and unregisters
    * all MediaSession action handlers.  Idempotent.
    *
    * Wired to `pagehide` automatically (covers SPA navigation +
    * bfcache eviction); callers don't normally invoke this.  Without
    * the URL.revokeObjectURL the ~8 KB Blob would leak per page
    * load if a future hot-reload mechanism swapped this module.
    */
   function shutdown() {
      if (isSupported()) {
         try {
            navigator.mediaSession.metadata = null;
            navigator.mediaSession.playbackState = 'none';
            REGISTERED_ACTIONS.forEach(function (action) {
               try {
                  navigator.mediaSession.setActionHandler(action, null);
               } catch (e) {
                  /* Unsupported action — ignore. */
               }
            });
         } catch (e) {
            /* If mediaSession itself misbehaves on shutdown, ignore. */
         }
      }
      if (silentAudio) {
         try {
            silentAudio.pause();
            silentAudio.removeAttribute('src');
            silentAudio.load(); /* Detaches the source per HTML spec. */
            if (silentAudio.parentNode) {
               silentAudio.parentNode.removeChild(silentAudio);
            }
         } catch (e) {
            /* Element teardown best-effort. */
         }
         silentAudio = null;
      }
      if (silentAudioUrl) {
         try {
            URL.revokeObjectURL(silentAudioUrl);
         } catch (e) {
            /* Already revoked — ignore. */
         }
         silentAudioUrl = null;
      }
      lastMeta = { title: '', artist: '', album: '' };
      primedOnGesture = false;
      initialized = false;
      actionCallbacks = null;
      debug('shutdown: complete');
   }

   /* pagehide fires on SPA navigation and bfcache eviction (modern
    * unload replacement).  Best-effort cleanup of the media-session
    * slot + Blob URL so we don't leak a stale claim if the user
    * navigates away while music is playing. */
   if (typeof window !== 'undefined' && typeof window.addEventListener === 'function') {
      window.addEventListener('pagehide', function () {
         shutdown();
      });
   }

   global.DawnMediaSession = {
      init: init,
      primeOnGesture: primeOnGesture,
      setPlaying: setPlaying,
      setMetadata: setMetadata,
      clearMetadata: clearMetadata,
      shutdown: shutdown,
   };
})(window);
