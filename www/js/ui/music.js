/**
 * DAWN Music Panel UI Module
 * WinAmp-inspired music player interface
 *
 * Usage:
 *   DawnMusicUI.init()          // Initialize UI bindings
 *   DawnMusicUI.open()          // Open music panel
 *   DawnMusicUI.close()         // Close music panel
 *   DawnMusicUI.toggle()        // Toggle panel visibility
 */
(function (global) {
   'use strict';

   // Constants
   const BROWSE_PAGE_SIZE = 50;

   // UI state
   let isOpen = false;
   let activeTab = 'playing';
   let visualizerAnimationId = null;
   let musicEscToken = null; /* DawnEscStack registration while the panel is open */
   /* Cleanup fn returned by Modals.trapFocus when the panel opens;
    * null when closed.  Keeps Tab/Shift+Tab cycling within the panel
    * instead of escaping to the rest of the page. */
   let focusTrapCleanup = null;

   // Cached DOM elements
   const elements = {
      panel: null,
      closeBtn: null,
      tabs: null,
      tabContents: null,

      // Now Playing
      artworkContainer: null,
      visualizer: null,
      trackTitle: null,
      trackArtist: null,
      trackAlbum: null,
      progressBar: null,
      progressFill: null,
      currentTime: null,
      totalTime: null,

      // Transport
      playPauseBtn: null,
      prevBtn: null,
      nextBtn: null,
      stopBtn: null,

      // Secondary controls
      volumeSlider: null,
      volumeIcon: null,
      shuffleBtn: null,
      repeatBtn: null,

      // Status
      statusFormat: null,
      statusBitrate: null,
      statusBuffer: null,
      statusLed: null,

      // Queue
      queueList: null,

      // Library
      searchInput: null,
      libraryStats: null,
      libraryTab: null,
      artistsList: null,
      albumsList: null,
   };

   // Local state
   const localState = {
      volume: parseFloat(localStorage.getItem('musicVolume') || '0.8'),
      muted: localStorage.getItem('musicMuted') === 'true',
      shuffle: localStorage.getItem('musicShuffle') === 'true', // Display hint until server syncs
      repeat: localStorage.getItem('musicRepeat') || 'none', // Display hint until server syncs
      lastQueueLength: -1, // Track queue changes for auto-refresh
      lastQueueIndex: -1,
      queueRestored: false, // Track if we've restored saved queue
      browseOffset: 0,
      browseTotalCount: 0,
      browseCurrentType: null, // 'tracks', 'artists', 'albums'
      browseLoading: false,
      libraryInitialized: false, // True after first stats fetch
      lastLocalVolumeChange: 0, // Timestamp of last local volume change
      volumeDebounceTimer: null, // Debounce timer for server volume sync
      volumeRestoredToServer: false, // True after saved volume sent to server on first state
      seekTarget: 0, // Optimistic seek/track-change position (s), shown until the server confirms
      seekPendingUntil: 0, // ms epoch: suppress stale pre-seek position echoes until then
      trackChangePending: false, // optimistic next/prev/play_index in flight (see changeTrack)
   };

   // Optimistic-UI timing. Coupled: an echo must converge to the target within the
   // window, and the buffered-position correction (music-playback.js) keeps the
   // reported position below the target until the ring drains — so the window must
   // outlast that drain. Move the two together.
   const SEEK_OPTIMISTIC_MS = 2000; // show the optimistic target this long before releasing
   const SEEK_CONVERGE_SEC = 1.5; // an echo within this of the target counts as "caught up"

   /**
    * Initialize UI bindings
    */
   function init() {
      cacheElements();
      bindEvents();
      setupCallbacks();
      setupMediaSession();
      restoreLocalState();
      restorePanelState();

      console.log('Music UI: Initialized');
   }

   /* OS media-session integration — registers DAWN as the active
    * media player so hardware media keys (Play/Pause/Next/Prev),
    * lock-screen widgets, and OS notification controls all route to
    * DAWN's transport.  See www/js/audio/media-session.js for the
    * silent-audio mechanism that keeps the media-session slot held
    * while DAWN streams audio server-side. */
   function setupMediaSession() {
      if (!window.DawnMediaSession) return;
      DawnMediaSession.init({
         onPlay: function () {
            const s = DawnMusicPlayback.getState();
            if (s.paused) {
               DawnMusicPlayback.control('play');
            } else if (s.queueLength > 0) {
               changeTrack('play_index', { index: s.queueIndex });
            }
         },
         onPause: function () {
            DawnMusicPlayback.control('pause');
         },
         onPrevious: function () {
            changeTrack('previous');
         },
         onNext: function () {
            changeTrack('next');
         },
         onStop: function () {
            DawnMusicPlayback.control('stop');
         },
         onSeekTo: function (e) {
            beginSeek(e.time);
         },
      });
   }

   /**
    * Restore panel open/closed state from localStorage
    */
   function restorePanelState() {
      const savedState = localStorage.getItem('dawn_music_panel_open');
      if (savedState === 'true') {
         open();
      }
   }

   /**
    * Cache DOM elements
    */
   function cacheElements() {
      elements.panel = document.getElementById('music-panel');
      elements.closeBtn = document.getElementById('music-close');
      elements.tabs = document.querySelectorAll('.music-tab');
      elements.tabContents = document.querySelectorAll('.music-tab-content');

      // Now Playing
      elements.artworkContainer = document.querySelector('.music-artwork-container');
      elements.visualizer = document.querySelector('.music-visualizer');
      elements.trackTitle = document.querySelector('.music-track-title');
      elements.trackArtist = document.querySelector('.music-track-artist');
      elements.trackAlbum = document.querySelector('.music-track-album');
      elements.progressBar = document.querySelector('.music-progress-bar');
      elements.progressFill = document.querySelector('.music-progress-fill');
      elements.currentTime = document.querySelector('.music-current-time');
      elements.totalTime = document.querySelector('.music-total-time');

      // Transport
      elements.playPauseBtn = document.getElementById('music-play-pause');
      elements.prevBtn = document.getElementById('music-prev');
      elements.nextBtn = document.getElementById('music-next');
      elements.stopBtn = document.getElementById('music-stop');

      // Secondary controls
      elements.volumeSlider = document.getElementById('music-volume');
      elements.volumeIcon = document.querySelector('.music-volume-icon');
      elements.shuffleBtn = document.getElementById('music-shuffle');
      elements.repeatBtn = document.getElementById('music-repeat');

      // Status
      elements.statusFormat = document.querySelector('.music-status-format');
      elements.statusBitrate = document.querySelector('.music-status-bitrate');
      elements.statusBuffer = document.querySelector('.music-status-buffer');
      elements.statusLed = document.querySelector('.music-status-led');

      // Queue
      elements.queueList = document.querySelector('.music-queue-list');
      elements.queueFilter = document.getElementById('music-queue-filter');

      // Library
      elements.searchInput = document.getElementById('music-search');
      elements.libraryStats = document.querySelector('.music-library-stats');
      elements.libraryTab = document.querySelector('.music-tab-content[data-tab="library"]');
      elements.searchResults = document.getElementById('music-search-results');
      elements.searchResultsList = document.getElementById('music-results-list');
      elements.browseSection = document.getElementById('music-browse-section');
   }

   /**
    * Bind event listeners
    */
   function bindEvents() {
      // Panel close
      if (elements.closeBtn) {
         elements.closeBtn.addEventListener('click', close);
      }

      // Tabs — shared DawnTablist helper owns click + arrows.
      if (window.DawnTablist && elements.tabs && elements.tabs.length > 0) {
         tablist = window.DawnTablist.bind({
            tabs: elements.tabs,
            getActive: () => activeTab,
            onActivate: (name) => switchTab(name),
         });
         tablist.sync(); /* initial markup → matches activeTab */
      }

      // Transport controls
      if (elements.playPauseBtn) {
         elements.playPauseBtn.addEventListener('click', handlePlayPause);
      }
      if (elements.prevBtn) {
         elements.prevBtn.addEventListener('click', handlePrevious);
      }
      if (elements.nextBtn) {
         elements.nextBtn.addEventListener('click', handleNext);
      }
      if (elements.stopBtn) {
         elements.stopBtn.addEventListener('click', () => DawnMusicPlayback.control('stop'));
      }

      // Progress bar seeking
      if (elements.progressBar) {
         elements.progressBar.addEventListener('click', handleSeek);
         // role="slider" must be keyboard-operable (WCAG 2.1.1). Arrows scoped to the
         // bar's own focus — no collision with the tablist (that only fires on the tab
         // strip). Left/Right ±5s, PageUp/Dn ±15s, Home/End to start/end.
         elements.progressBar.addEventListener('keydown', handleProgressKey);
      }

      // Volume
      if (elements.volumeSlider) {
         elements.volumeSlider.addEventListener('input', handleVolumeChange);
      }
      if (elements.volumeIcon) {
         elements.volumeIcon.addEventListener('click', toggleMute);
      }

      // Mode buttons
      if (elements.shuffleBtn) {
         elements.shuffleBtn.addEventListener('click', toggleShuffle);
      }
      if (elements.repeatBtn) {
         elements.repeatBtn.addEventListener('click', cycleRepeat);
      }

      // Library search
      if (elements.searchInput) {
         elements.searchInput.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') {
               const query = e.target.value.trim();
               if (query) {
                  DawnMusicPlayback.search(query);
               }
            }
         });

         // Clear search results when input is emptied (native X button, backspace, etc.)
         elements.searchInput.addEventListener('input', () => {
            if (!elements.searchInput.value.trim()) {
               clearSearchResults();
            }
         });

         // Also handle the 'search' event (fired by native clear button on some browsers)
         elements.searchInput.addEventListener('search', () => {
            if (!elements.searchInput.value.trim()) {
               clearSearchResults();
            }
         });
      }

      // Queue filter
      if (elements.queueFilter) {
         elements.queueFilter.addEventListener('input', filterQueue);
         elements.queueFilter.addEventListener('search', filterQueue);
      }

      // Clear queue button
      const clearQueueBtn = document.getElementById('music-clear-queue');
      if (clearQueueBtn) {
         clearQueueBtn.addEventListener('click', () => {
            DawnMusicPlayback.control('clear_queue');
         });
      }

      // Library infinite scroll
      if (elements.libraryTab) {
         elements.libraryTab.addEventListener('scroll', handleLibraryScroll);
      }

      // Keyboard shortcuts
      document.addEventListener('keydown', handleKeyboard);

      // Music button in toolbar
      const musicBtn = document.getElementById('music-btn');
      if (musicBtn) {
         musicBtn.addEventListener('click', toggle);
      }
   }

   /**
    * Setup playback callbacks
    */
   function setupCallbacks() {
      DawnMusicPlayback.setCallbacks({
         onStateChange: handleStateChange,
         onPositionUpdate: handlePositionUpdate,
         onBufferUpdate: handleBufferUpdate,
         onError: handleError,
         onSearchResults: handleSearchResults,
         onLibraryResponse: handleLibraryResponse,
         onQueueResponse: handleQueueResponse,
      });
   }

   /**
    * Restore local state from localStorage
    */
   function restoreLocalState() {
      if (elements.volumeSlider) {
         elements.volumeSlider.value = localState.volume;
      }
      updateVolumeIcon();
      updateModeButtons();
      // Apply saved volume to playback (client-side gainNode)
      DawnMusicPlayback.setVolume(localState.muted ? 0 : localState.volume);
   }

   /**
    * Open music panel
    */
   function open() {
      if (!elements.panel) return;

      /* Panel-open is a user gesture — prime the OS media-session
       * sink here so it's ready before the user clicks Play.  No-op
       * after the first successful prime. */
      if (window.DawnMediaSession) DawnMediaSession.primeOnGesture();

      isOpen = true;
      elements.panel.classList.remove('hidden');
      DawnStore.setBool(DawnStore.KEYS.MUSIC_PANEL_OPEN, true);
      if (musicEscToken === null) {
         musicEscToken = DawnEscStack.register(() => {
            close();
            return true;
         });
      }

      // Update button state
      const musicBtn = document.getElementById('music-btn');
      if (musicBtn) {
         musicBtn.classList.add('active');
      }

      // Subscribe to music stream if not already (uses server default quality)
      if (!DawnMusicPlayback.isSubscribed()) {
         DawnMusicPlayback.subscribe();
      }

      // Load library stats
      DawnMusicPlayback.browseLibrary('stats');

      // Start visualizer if playing
      if (DawnMusicPlayback.isPlaying()) {
         startVisualizer();
      }

      /* Focus management for accessibility.  Defer focus() via
       * setTimeout(0) so the .hidden-removed visibility transition
       * settles before we move focus — calling focus() on an element
       * the browser still considers off-screen can silently fail and
       * leave focus on the trigger button.  Same shape scheduler-
       * queue.js uses for the same reason. */
      setTimeout(() => {
         if (elements.closeBtn) {
            elements.closeBtn.focus();
         }
      }, 0);

      /* Trap Tab/Shift+Tab within the panel.  skipInitialFocus because
       * we focused the close button above (deferred via setTimeout).
       * Cleanup stored at module scope and fired in close() so the
       * listener is removed when the panel hides.  The shared helper
       * is exposed on window.DawnSettingsModals (the `Modals` short
       * alias is local to settings.js). */
      const M = window.DawnSettingsModals;
      if (M && typeof M.trapFocus === 'function') {
         focusTrapCleanup = M.trapFocus(elements.panel, { skipInitialFocus: true });
      }
   }

   /**
    * Close music panel
    */
   function close() {
      if (!elements.panel) return;

      isOpen = false;
      elements.panel.classList.add('hidden');
      DawnStore.setBool(DawnStore.KEYS.MUSIC_PANEL_OPEN, false);
      if (musicEscToken !== null) {
         DawnEscStack.unregister(musicEscToken);
         musicEscToken = null;
      }

      // Update button state
      const musicBtn = document.getElementById('music-btn');
      if (musicBtn) {
         musicBtn.classList.remove('active');
      }

      if (focusTrapCleanup) {
         focusTrapCleanup();
         focusTrapCleanup = null;
      }

      stopVisualizer();
   }

   /**
    * Toggle music panel
    */
   function toggle() {
      if (isOpen) {
         close();
      } else {
         open();
      }
   }

   /* Tab strip — bound to the shared DawnTablist helper in bindEvents().
    * Click + ←/→/Home/End handled centrally; consumer just owns
    * activeTab and re-applies DOM via tablist.sync(). */
   let tablist = null;

   /**
    * Switch active tab
    * @param {string} tabName - Tab name (playing, queue, library)
    */
   function switchTab(tabName) {
      activeTab = tabName;

      if (tablist) tablist.sync();

      // Update tab content
      elements.tabContents.forEach((content) => {
         content.classList.toggle('active', content.dataset.tab === tabName);
      });

      // Load tab-specific data
      if (tabName === 'queue') {
         DawnMusicPlayback.queue('list');
      } else if (tabName === 'library') {
         // Only fetch stats on the first visit — preserve whatever the user
         // was viewing (drill-down, search results, etc.) on subsequent switches
         if (!localState.libraryInitialized) {
            DawnMusicPlayback.browseLibrary('stats');
         }
      }
   }

   /**
    * Handle play/pause button
    */
   function handlePlayPause() {
      /* Prime the OS media-session sink on this user gesture so the
       * silent audio's .play() doesn't get autoplay-rejected.  No-op
       * after the first successful prime. */
      if (window.DawnMediaSession) DawnMediaSession.primeOnGesture();

      const state = DawnMusicPlayback.getState();

      if (state.playing && !state.paused) {
         DawnMusicPlayback.control('pause');
      } else if (state.paused) {
         DawnMusicPlayback.control('play');
      } else if (state.queueLength > 0) {
         // Have queue items - play from current index
         changeTrack('play_index', { index: state.queueIndex });
      } else {
         // Nothing in queue, open library search
         switchTab('library');
         if (elements.searchInput) {
            elements.searchInput.focus();
         }
      }
   }

   /**
    * Handle progress bar seek
    * @param {MouseEvent} e - Click event
    */
   function handleSeek(e) {
      const rect = elements.progressBar.getBoundingClientRect();
      const percent = (e.clientX - rect.left) / rect.width;
      const state = DawnMusicPlayback.getState();
      beginSeek(percent * state.durationSec);
   }

   /**
    * Keyboard seek for the role="slider" progress bar.
    * @param {KeyboardEvent} e
    */
   function handleProgressKey(e) {
      const st = DawnMusicPlayback.getState();
      const dur = st.durationSec || 0;
      if (dur <= 0) return;
      // Accumulate off the pending optimistic target so repeated presses add up.
      const pending = localState.seekPendingUntil && Date.now() < localState.seekPendingUntil;
      const base = pending ? localState.seekTarget : st.positionSec || 0;
      let target;
      switch (e.key) {
         case 'ArrowRight':
         case 'ArrowUp':
            target = base + 5;
            break;
         case 'ArrowLeft':
         case 'ArrowDown':
            target = base - 5;
            break;
         case 'PageUp':
            target = base + 15;
            break;
         case 'PageDown':
            target = base - 15;
            break;
         case 'Home':
            target = 0;
            break;
         case 'End':
            target = dur;
            break;
         default:
            return;
      }
      e.preventDefault();
      beginSeek(Math.max(0, Math.min(dur, target)));
   }

   /**
    * Handle volume change
    * @param {Event} e - Input event
    */
   function handleVolumeChange(e) {
      localState.volume = parseFloat(e.target.value);
      localState.muted = false;
      localState.lastLocalVolumeChange = Date.now();
      localStorage.setItem('musicVolume', localState.volume);
      localStorage.setItem('musicMuted', 'false');
      updateVolumeIcon();
      DawnMusicPlayback.setVolume(localState.volume);

      // Debounce server sync (50ms) to avoid flooding during slider drag
      clearTimeout(localState.volumeDebounceTimer);
      localState.volumeDebounceTimer = setTimeout(() => {
         DawnMusicPlayback.control('volume', { level: localState.volume });
      }, 50);
   }

   /**
    * Toggle mute
    */
   function toggleMute() {
      localState.muted = !localState.muted;
      localStorage.setItem('musicMuted', localState.muted);
      updateVolumeIcon();
      DawnMusicPlayback.setVolume(localState.muted ? 0 : localState.volume);
   }

   /**
    * Handle next button — server handles shuffle/repeat logic
    */
   function handleNext() {
      changeTrack('next');
   }

   /**
    * Handle previous button — server handles shuffle/repeat logic
    */
   function handlePrevious() {
      changeTrack('previous');
   }

   /**
    * Toggle shuffle mode (server-side, optimistic update)
    */
   function toggleShuffle() {
      localState.shuffle = !localState.shuffle;
      updateModeButtons();
      DawnMusicPlayback.control('toggle_shuffle');
   }

   /**
    * Cycle repeat mode (server-side, optimistic update)
    */
   function cycleRepeat() {
      const modes = ['none', 'all', 'one'];
      const currentIndex = modes.indexOf(localState.repeat);
      localState.repeat = modes[(currentIndex + 1) % modes.length];
      updateModeButtons();
      DawnMusicPlayback.control('cycle_repeat');
   }

   /**
    * Update volume icon based on state
    */
   function updateVolumeIcon() {
      if (!elements.volumeIcon) return;

      // Use CSS classes to control SVG visibility
      elements.volumeIcon.classList.remove('muted', 'low');

      if (localState.muted || localState.volume === 0) {
         elements.volumeIcon.classList.add('muted');
      } else if (localState.volume < 0.5) {
         elements.volumeIcon.classList.add('low');
      }
   }

   /**
    * Update mode button states
    */
   function updateModeButtons() {
      if (elements.shuffleBtn) {
         elements.shuffleBtn.classList.toggle('active', localState.shuffle);
      }
      if (elements.repeatBtn) {
         elements.repeatBtn.classList.toggle('active', localState.repeat !== 'none');
         elements.repeatBtn.classList.toggle('repeat-one', localState.repeat === 'one');
         // Update title based on mode
         elements.repeatBtn.title =
            localState.repeat === 'one'
               ? 'Repeat One'
               : localState.repeat === 'all'
                 ? 'Repeat All'
                 : 'Repeat Off';
      }
   }

   /**
    * Handle keyboard shortcuts
    * @param {KeyboardEvent} e - Keyboard event
    */
   function handleKeyboard(e) {
      // Only handle when panel is open and focus is within the music panel
      // Don't intercept when user is typing in inputs/textareas elsewhere
      if (!isOpen) return;
      if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA') return;
      if (!elements.panel || !elements.panel.contains(document.activeElement)) return;

      switch (e.key) {
         case ' ':
            e.preventDefault();
            handlePlayPause();
            break;
         /* Escape-to-close is handled via DawnEscStack (register-on-open in
          * open() / unregister in close()) so it participates in the global
          * layer stack regardless of where focus sits. */
         /* ArrowLeft / ArrowRight intentionally NOT bound — they're
          * reserved for tablist navigation (DawnTablist) when focus
          * is on the tab strip.  Binding them here would double-fire
          * (cycle tabs AND skip tracks) on every keypress.  Use the
          * transport buttons for previous/next instead. */
         case 'ArrowUp':
            e.preventDefault();
            localState.volume = Math.min(1, localState.volume + 0.1);
            if (elements.volumeSlider) elements.volumeSlider.value = localState.volume;
            handleVolumeChange({ target: { value: localState.volume } });
            break;
         case 'ArrowDown':
            e.preventDefault();
            localState.volume = Math.max(0, localState.volume - 0.1);
            if (elements.volumeSlider) elements.volumeSlider.value = localState.volume;
            handleVolumeChange({ target: { value: localState.volume } });
            break;
         case 'm':
            toggleMute();
            break;
         case 's':
            toggleShuffle();
            break;
         case 'r':
            cycleRepeat();
            break;
      }
   }

   /**
    * Handle state change from playback
    * @param {object} state - Playback state
    */
   function handleStateChange(state) {
      // Update track info
      if (state.track) {
         if (elements.trackTitle) {
            elements.trackTitle.textContent = state.track.title || 'Unknown Track';
         }
         if (elements.trackArtist) {
            elements.trackArtist.textContent = state.track.artist || 'Unknown Artist';
         }
         if (elements.trackAlbum) {
            elements.trackAlbum.textContent = state.track.album || '';
         }
      } else {
         if (elements.trackTitle) elements.trackTitle.textContent = 'No track playing';
         if (elements.trackArtist) elements.trackArtist.textContent = '';
         if (elements.trackAlbum) elements.trackAlbum.textContent = '';
      }

      /* OS media-session sync.  Three-state dispatch:
       *
       *  1. Track present + playing/paused → setMetadata + setPlaying
       *     (OS widget shows current track in correct play/pause state).
       *  2. Track present + stopped (playing=false AND paused=false)
       *     → clearMetadata (release the slot rather than leave a
       *     dangling "paused on track X" with no resume path).
       *  3. No track (queue cleared, unsubscribed, never-played)
       *     → clearMetadata.
       *
       * Artwork omitted intentionally — DAWN's music DB doesn't yet
       * store per-track cover art.  Live-tested on Chrome+Windows:
       * the Chrome toolbar media icon, Win+A Action Center widget,
       * and lock-screen panel ALL surface DAWN cleanly with no
       * artwork — they fall back to a neutral generic-media glyph.
       * Adding artwork is a future feature once the music DB grows
       * a cover_art column (local file scan / Plex passthrough);
       * the setMetadata API accepts an artwork:[] array for that. */
      if (window.DawnMediaSession) {
         const isStopped = state.track && !state.playing && !state.paused;
         if (state.track && !isStopped) {
            DawnMediaSession.setMetadata({
               title: state.track.title || 'Unknown Track',
               artist: state.track.artist || 'Unknown Artist',
               album: state.track.album || '',
               duration:
                  typeof state.durationSec === 'number' && state.durationSec > 0
                     ? state.durationSec
                     : undefined,
               position: typeof state.positionSec === 'number' ? state.positionSec : 0,
            });
            DawnMediaSession.setPlaying(state.playing && !state.paused);
         } else {
            DawnMediaSession.clearMetadata();
         }
      }

      // Update play/pause button
      if (elements.playPauseBtn) {
         if (state.playing && !state.paused) {
            elements.playPauseBtn.innerHTML = '\u{23F8}'; // Pause icon
            elements.playPauseBtn.title = 'Pause';
         } else {
            elements.playPauseBtn.innerHTML = '\u{25B6}'; // Play icon
            elements.playPauseBtn.title = 'Play';
         }
      }

      // Update status bar
      if (elements.statusFormat) {
         if (state.sourceRate > 0) {
            const rateKhz =
               state.sourceRate >= 1000
                  ? `${(state.sourceRate / 1000).toFixed(1)}k`
                  : state.sourceRate;
            elements.statusFormat.textContent = `${state.sourceFormat} ${rateKhz}`;
         } else {
            elements.statusFormat.textContent = '--';
         }
      }
      if (elements.statusBitrate) {
         elements.statusBitrate.textContent = `${state.bitrate / 1000} kbps ${state.bitrateMode.toUpperCase()}`;
      }
      if (elements.statusLed) {
         elements.statusLed.classList.toggle('active', state.playing && !state.paused);
      }

      // Update music button indicator
      const musicBtn = document.getElementById('music-btn');
      if (musicBtn) {
         musicBtn.classList.toggle('playing', state.playing && !state.paused);
      }

      // Refresh queue if it changed (length or current index)
      const queueChanged =
         state.queueLength !== localState.lastQueueLength ||
         state.queueIndex !== localState.lastQueueIndex;
      localState.lastQueueLength = state.queueLength;
      localState.lastQueueIndex = state.queueIndex;

      if (queueChanged) {
         DawnMusicPlayback.queue('list');
      }

      // Reconcile an optimistic track-change: changeTrack() resets the bar to 0 assuming
      // the track advances. If the server shows the index did NOT move (end of queue, or
      // a rejected change), release the pending window now so the bar doesn't sit at 0
      // for the full timeout and then snap back to the still-playing track's position.
      if (localState.trackChangePending) {
         localState.trackChangePending = false;
         if (!queueChanged) {
            localState.seekPendingUntil = 0;
         }
      }

      // Queue is now persisted server-side in SQLite — no localStorage restore needed
      localState.queueRestored = true;

      // Sync shuffle/repeat from server state (persist as display hint for next page load)
      localState.shuffle = state.shuffle || false;
      const repeatMap = { 0: 'none', 1: 'all', 2: 'one' };
      localState.repeat = repeatMap[state.repeatMode] || 'none';
      localStorage.setItem('musicShuffle', localState.shuffle);
      localStorage.setItem('musicRepeat', localState.repeat);
      updateModeButtons();

      // Volume sync: on first state after page load, push saved volume TO server
      // (conn->volume resets to default on reconnect). After that, accept server updates.
      if (!localState.volumeRestoredToServer) {
         localState.volumeRestoredToServer = true;
         localState.lastLocalVolumeChange = Date.now();
         DawnMusicPlayback.control('volume', { level: localState.volume });
      } else if (
         state.volume !== undefined &&
         Date.now() - localState.lastLocalVolumeChange > 500
      ) {
         localState.volume = state.volume;
         localState.muted = false;
         localStorage.setItem('musicVolume', localState.volume);
         localStorage.setItem('musicMuted', 'false');
         if (elements.volumeSlider) {
            elements.volumeSlider.value = localState.volume;
         }
         updateVolumeIcon();
         DawnMusicPlayback.setVolume(localState.volume);
      }

      // Start/stop visualizer
      if (state.playing && !state.paused && isOpen) {
         startVisualizer();
      } else {
         stopVisualizer();
      }
   }

   /**
    * Handle position update
    * @param {number} positionSec - Current position in seconds
    * @param {number} durationSec - Total duration in seconds
    */
   function handlePositionUpdate(positionSec, durationSec) {
      // While a seek/track-change is pending, ignore position echoes until the
      // server's reported position converges to the target — otherwise a stale
      // pre-change update would snap the bar back before jumping forward again.
      if (localState.seekPendingUntil && Date.now() < localState.seekPendingUntil) {
         if (Math.abs(positionSec - localState.seekTarget) > SEEK_CONVERGE_SEC) {
            return;
         }
         localState.seekPendingUntil = 0; // server caught up — resume live updates
      }
      applyProgress(positionSec, durationSec);
   }

   /**
    * Render a position onto the progress bar + time displays.
    * @param {number} positionSec
    * @param {number} durationSec
    */
   function applyProgress(positionSec, durationSec) {
      if (elements.progressFill && durationSec > 0) {
         elements.progressFill.style.width = `${(positionSec / durationSec) * 100}%`;
      }
      // Keep the role="slider" value truthful to assistive tech (WCAG 4.1.2). The bar
      // is the single render authority, so the optimistic jump is reflected here too.
      if (elements.progressBar && durationSec > 0) {
         elements.progressBar.setAttribute('aria-valuenow', String(Math.round(positionSec)));
         elements.progressBar.setAttribute('aria-valuemax', String(Math.round(durationSec)));
         elements.progressBar.setAttribute(
            'aria-valuetext',
            `${formatTime(positionSec)} of ${formatTime(durationSec)}`
         );
      }
      if (elements.currentTime) {
         elements.currentTime.textContent = formatTime(positionSec);
      }
      if (elements.totalTime) {
         elements.totalTime.textContent = formatTime(durationSec);
      }
   }

   /**
    * Optimistic-UI helper: render a target position now and suppress stale position
    * echoes until the server's reported position converges to it.
    * @param {number} targetSec
    */
   function optimisticPosition(targetSec) {
      const durationSec = DawnMusicPlayback.getState().durationSec || 0;
      localState.seekTarget = targetSec;
      localState.seekPendingUntil = Date.now() + SEEK_OPTIMISTIC_MS;
      // Snap the discrete optimistic jump instead of sliding it through the fill's
      // 0.1s width transition (which undercuts the "instant" feel and is exactly the
      // large motion prefers-reduced-motion users opt out of). Live incremental
      // updates keep the CSS ease.
      if (elements.progressFill) {
         elements.progressFill.style.transition = 'none';
      }
      applyProgress(targetSec, durationSec);
      if (elements.progressFill) {
         void elements.progressFill.offsetWidth; // commit the snap, then restore the ease
         elements.progressFill.style.transition = '';
      }
   }

   /**
    * Seek with optimistic UI: jump the bar to the target immediately.
    * @param {number} seekTime - Target position in seconds
    */
   function beginSeek(seekTime) {
      if (typeof seekTime !== 'number' || !isFinite(seekTime)) return;
      optimisticPosition(seekTime);
      DawnMusicPlayback.control('seek', { position_sec: seekTime });
   }

   /**
    * Track-change control (next/previous/play_index) with optimistic UI: reset the
    * bar to 0 immediately so it doesn't lag on the old track's position until the
    * server echoes the new track's music_state.
    * @param {string} action
    * @param {object} [opts]
    */
   function changeTrack(action, opts) {
      localState.trackChangePending = true;
      optimisticPosition(0);
      DawnMusicPlayback.control(action, opts);
   }

   /**
    * Handle buffer status update
    * @param {number} percent - Buffer fill percentage (0-100)
    */
   function handleBufferUpdate(percent) {
      if (elements.statusBuffer) {
         elements.statusBuffer.textContent = `Buf ${percent}%`;
      }
   }

   /**
    * Handle playback error
    * @param {string} code - Error code
    * @param {string} message - Error message
    */
   function handleError(code, message) {
      console.error('Music UI: Error', code, message);
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show(message || `Music error: ${code}`, 'error');
      }
   }

   /**
    * Clear search results and return to browse view
    */
   function clearSearchResults() {
      if (elements.searchResults) {
         elements.searchResults.classList.add('hidden');
      }
      if (elements.browseSection) {
         elements.browseSection.classList.remove('hidden');
      }
   }

   /**
    * Handle search results
    * @param {object} payload - Search results payload
    */
   function handleSearchResults(payload) {
      if (!elements.searchResults || !elements.searchResultsList) return;

      const results = payload.results || [];
      const query = payload.query || '';
      const count = payload.count || results.length;

      // Show search results section, hide browse section
      elements.searchResults.classList.remove('hidden');
      if (elements.browseSection) {
         elements.browseSection.classList.add('hidden');
      }

      // Update header
      const header = elements.searchResults.querySelector('.music-search-results-header');
      if (header) {
         header.textContent = `Results for "${query}" (${count})`;
      }

      if (results.length === 0) {
         elements.searchResultsList.innerHTML =
            '<li class="music-library-item"><span class="music-library-item-title">No results found</span></li>';
         return;
      }

      // Build results HTML with play and add-to-queue buttons
      const html = results
         .map(
            (track, i) => `
         <li class="music-library-item" data-path="${escapeAttr(track.path)}" data-index="${i}"
            data-title="${escapeAttr(track.title || track.display_name || '')}"
            data-artist="${escapeAttr(track.artist || '')}"
            data-album="${escapeAttr(track.album || '')}"
            data-duration="${track.duration_sec || 0}">
            <div class="music-library-item-info">
               <div class="music-library-item-title">${escapeHtml(track.title || track.display_name || 'Unknown')}</div>
               <div class="music-library-item-artist">${escapeHtml(track.artist || '')}</div>
            </div>
            <span class="music-library-item-duration">${formatTime(track.duration_sec || 0)}</span>
            <button class="music-search-add-btn" data-action="add" title="Add to queue">+</button>
         </li>
      `
         )
         .join('');

      elements.searchResultsList.innerHTML = html;

      // Bind click events
      elements.searchResultsList.querySelectorAll('.music-library-item').forEach((item) => {
         const path = item.dataset.path;
         if (!path) return;

         const trackMeta = {
            path: path,
            title: item.dataset.title || '',
            artist: item.dataset.artist || '',
            album: item.dataset.album || '',
            duration_sec: parseInt(item.dataset.duration) || 0,
         };

         // Click on title/info area = play immediately
         const infoArea = item.querySelector('.music-library-item-info');
         if (infoArea) {
            infoArea.addEventListener('click', (e) => {
               e.stopPropagation();
               DawnMusicPlayback.control('play', trackMeta);
               switchTab('playing');
            });
         }

         // Click on "+" button = add to queue
         const addBtn = item.querySelector('.music-search-add-btn');
         if (addBtn) {
            addBtn.addEventListener('click', (e) => {
               e.stopPropagation();
               DawnMusicPlayback.control('add_to_queue', trackMeta);
               // Visual feedback
               addBtn.textContent = '\u2713'; // Checkmark
               setTimeout(() => {
                  addBtn.textContent = '+';
               }, 1000);
            });
         }
      });

      console.log('Music UI: Displayed', results.length, 'search results');
   }

   /**
    * Handle scroll on the library tab for infinite loading
    */
   function handleLibraryScroll() {
      if (localState.browseLoading) return;
      if (!localState.browseCurrentType) return;
      if (localState.browseOffset + BROWSE_PAGE_SIZE >= localState.browseTotalCount) return;
      if (!elements.libraryTab) return;

      const distanceFromBottom =
         elements.libraryTab.scrollHeight -
         elements.libraryTab.scrollTop -
         elements.libraryTab.clientHeight;
      if (distanceFromBottom < 200) {
         loadNextBrowsePage();
      }
   }

   /**
    * Load the next page of browse results
    */
   function loadNextBrowsePage() {
      localState.browseLoading = true;
      localState.browseOffset += BROWSE_PAGE_SIZE;
      DawnWS.send({
         type: 'music_library',
         payload: {
            type: localState.browseCurrentType,
            offset: localState.browseOffset,
            limit: BROWSE_PAGE_SIZE,
         },
      });

      // Safety timeout: reset loading flag if response never arrives
      setTimeout(() => {
         localState.browseLoading = false;
      }, 5000);
   }

   /**
    * Handle library response
    * @param {object} payload - Library data payload
    */
   function handleLibraryResponse(payload) {
      const browseList = document.getElementById('music-browse-list');
      const browseSection = document.getElementById('music-browse-section');
      const sectionHeader = browseSection?.querySelector('.music-library-section-header');

      // Determine if this is an appended page (offset > 0)
      const isAppend = (payload.offset || 0) > 0;

      if (payload.browse_type === 'stats' && elements.libraryStats) {
         // Mark library as initialized after first stats fetch
         localState.libraryInitialized = true;
         // Reset pagination state when returning to stats view
         localState.browseCurrentType = null;
         localState.browseOffset = 0;
         localState.browseTotalCount = 0;
         localState.browseLoading = false;

         // Update library stats with clickable items
         const html = `
            <div class="music-library-stat" data-browse="tracks" style="cursor: pointer;">
               <div class="music-library-stat-value">${payload.track_count || 0}</div>
               <div class="music-library-stat-label">Tracks</div>
            </div>
            <div class="music-library-stat" data-browse="artists" style="cursor: pointer;">
               <div class="music-library-stat-value">${payload.artist_count || 0}</div>
               <div class="music-library-stat-label">Artists</div>
            </div>
            <div class="music-library-stat" data-browse="albums" style="cursor: pointer;">
               <div class="music-library-stat-value">${payload.album_count || 0}</div>
               <div class="music-library-stat-label">Albums</div>
            </div>
         `;
         elements.libraryStats.innerHTML = html;

         // Add click handlers to stats
         elements.libraryStats.querySelectorAll('.music-library-stat').forEach((stat) => {
            stat.addEventListener('click', () => {
               const browseType = stat.dataset.browse;
               DawnWS.send({ type: 'music_library', payload: { type: browseType } });
            });
         });

         // Reset browse section to default
         if (sectionHeader) sectionHeader.textContent = 'Browse Library';
         if (browseList)
            browseList.innerHTML =
               '<li class="music-library-item"><span class="music-library-item-title">Click on Tracks, Artists, or Albums above to browse</span></li>';
      } else if (payload.browse_type === 'tracks' && browseList) {
         const total = payload.total_count || payload.count || 0;
         if (sectionHeader) sectionHeader.textContent = `All Tracks (${total})`;
         if (isAppend) {
            renderTrackList(browseList, payload.tracks || [], false, true);
         } else {
            localState.browseCurrentType = 'tracks';
            localState.browseOffset = 0;
            localState.browseTotalCount = total;
            renderTrackList(browseList, payload.tracks || []);
         }
         localState.browseLoading = false;
      } else if (payload.browse_type === 'artists' && browseList) {
         const total = payload.total_count || payload.count || 0;
         if (sectionHeader) sectionHeader.textContent = `Artists (${total})`;
         if (isAppend) {
            renderArtistList(browseList, payload.artists || [], true);
         } else {
            localState.browseCurrentType = 'artists';
            localState.browseOffset = 0;
            localState.browseTotalCount = total;
            renderArtistList(browseList, payload.artists || []);
         }
         localState.browseLoading = false;
      } else if (payload.browse_type === 'albums' && browseList) {
         const total = payload.total_count || payload.count || 0;
         if (sectionHeader) sectionHeader.textContent = `Albums (${total})`;
         if (isAppend) {
            renderAlbumList(browseList, payload.albums || [], true);
         } else {
            localState.browseCurrentType = 'albums';
            localState.browseOffset = 0;
            localState.browseTotalCount = total;
            renderAlbumList(browseList, payload.albums || []);
         }
         localState.browseLoading = false;
      } else if (payload.browse_type === 'tracks_by_artist' && browseList) {
         // Not paginated — reset state
         localState.browseCurrentType = null;
         localState.browseOffset = 0;
         localState.browseTotalCount = 0;
         localState.browseLoading = false;
         if (sectionHeader)
            sectionHeader.textContent = `${payload.artist} (${payload.count || 0} tracks)`;
         renderTrackList(browseList, payload.tracks || [], true);
      } else if (payload.browse_type === 'tracks_by_album' && browseList) {
         // Not paginated — reset state
         localState.browseCurrentType = null;
         localState.browseOffset = 0;
         localState.browseTotalCount = 0;
         localState.browseLoading = false;
         if (sectionHeader)
            sectionHeader.textContent = `${payload.album} (${payload.count || 0} tracks)`;
         renderTrackList(browseList, payload.tracks || [], true);
      }
   }

   /**
    * Render a list of tracks with play/add buttons
    * @param {HTMLElement} container - List container
    * @param {Array} tracks - Track data array
    * @param {boolean} [isSubList=false] - Whether this is a sub-list (artist/album tracks)
    * @param {boolean} [append=false] - Append to existing list instead of replacing
    */
   function renderTrackList(container, tracks, isSubList, append) {
      if (tracks.length === 0 && !append) {
         container.innerHTML =
            '<li class="music-library-item"><span class="music-library-item-title">No tracks found</span></li>';
         return;
      }

      const html = tracks
         .map(
            (track) => `
         <li class="music-library-item" data-path="${escapeAttr(track.path)}"
            data-title="${escapeAttr(track.title || '')}"
            data-artist="${escapeAttr(track.artist || '')}"
            data-album="${escapeAttr(track.album || '')}"
            data-duration="${track.duration_sec || 0}">
            <div class="music-library-item-info">
               <div class="music-library-item-title">${escapeHtml(track.title || 'Unknown')}</div>
               <div class="music-library-item-artist">${escapeHtml(track.artist || '')}${track.album ? ' \u2022 ' + escapeHtml(track.album) : ''}</div>
            </div>
            <span class="music-library-item-duration">${formatTime(track.duration_sec || 0)}</span>
            <button class="music-search-add-btn" data-action="add" title="Add to queue">+</button>
         </li>
      `
         )
         .join('');

      if (append) {
         container.insertAdjacentHTML('beforeend', html);
         // Bind events only on newly added items
         const allItems = container.querySelectorAll('.music-library-item[data-path]');
         const newItems = Array.from(allItems).slice(-tracks.length);
         bindTrackListItemEvents(newItems);
      } else {
         container.innerHTML = html;
         bindTrackListEvents(container);
      }
   }

   /**
    * Render a list of artists with album/track counts
    * @param {HTMLElement} container - List container
    * @param {Array} artists - Artist data array
    * @param {boolean} [append=false] - Append to existing list instead of replacing
    */
   function renderArtistList(container, artists, append) {
      if (artists.length === 0 && !append) {
         container.innerHTML =
            '<li class="music-library-item"><span class="music-library-item-title">No artists found</span></li>';
         return;
      }

      const html = artists
         .map(
            (artist) => `
         <li class="music-library-item" data-artist="${escapeAttr(artist.name)}" data-artist-key="${escapeAttr(artist.key || '')}">
            <div class="music-library-item-info">
               <div class="music-library-item-title">${escapeHtml(artist.name)}</div>
               <div class="music-library-item-artist">${artist.album_count} album${artist.album_count !== 1 ? 's' : ''}, ${artist.track_count} track${artist.track_count !== 1 ? 's' : ''}</div>
            </div>
            <button class="music-search-add-btn" data-action="add-artist" title="Add all tracks">+</button>
            <button class="music-library-drill-btn" title="Show tracks">\u203A</button>
         </li>
      `
         )
         .join('');

      if (append) {
         container.insertAdjacentHTML('beforeend', html);
         const allItems = container.querySelectorAll('.music-library-item[data-artist]');
         const newItems = Array.from(allItems).slice(-artists.length);
         bindArtistListItemEvents(newItems);
      } else {
         container.innerHTML = html;
         bindArtistListItemEvents(container.querySelectorAll('.music-library-item[data-artist]'));
      }
   }

   /**
    * Bind events for artist list items
    * @param {NodeList|Array} items - Artist list items to bind
    */
   function bindArtistListItemEvents(items) {
      items.forEach((item) => {
         const artistName = item.dataset.artist;
         const artistKey = item.dataset.artistKey;

         // Drill-down to show artist's tracks
         function drillIntoArtist() {
            const payload = { type: 'tracks_by_artist', artist: artistName };
            if (artistKey) payload.artist_key = artistKey;
            DawnWS.send({ type: 'music_library', payload });
         }

         item.addEventListener('dblclick', (e) => {
            if (e.target.closest('.music-search-add-btn')) return;
            drillIntoArtist();
         });

         const drillBtn = item.querySelector('.music-library-drill-btn');
         if (drillBtn) {
            drillBtn.addEventListener('click', (e) => {
               e.stopPropagation();
               drillIntoArtist();
            });
         }

         // + button to add all artist tracks
         const addBtn = item.querySelector('.music-search-add-btn');
         if (addBtn) {
            addBtn.addEventListener('click', (e) => {
               e.stopPropagation();
               const addPayload = { artist: artistName };
               if (artistKey) addPayload.artist_key = artistKey;
               DawnMusicPlayback.control('add_artist', addPayload);
               addBtn.textContent = '\u2713';
               setTimeout(() => {
                  addBtn.textContent = '+';
               }, 1000);
            });
         }
      });
   }

   /**
    * Render a list of albums with track counts
    * @param {HTMLElement} container - List container
    * @param {Array} albums - Album data array
    * @param {boolean} [append=false] - Append to existing list instead of replacing
    */
   function renderAlbumList(container, albums, append) {
      if (albums.length === 0 && !append) {
         container.innerHTML =
            '<li class="music-library-item"><span class="music-library-item-title">No albums found</span></li>';
         return;
      }

      const html = albums
         .map(
            (album) => `
         <li class="music-library-item" data-album="${escapeAttr(album.name)}" data-album-key="${escapeAttr(album.key || '')}">
            <div class="music-library-item-info">
               <div class="music-library-item-title">${escapeHtml(album.name)}</div>
               <div class="music-library-item-artist">${escapeHtml(album.artist || 'Unknown')} \u2022 ${album.track_count} track${album.track_count !== 1 ? 's' : ''}</div>
            </div>
            <button class="music-search-add-btn" data-action="add-album" title="Add all tracks">+</button>
            <button class="music-library-drill-btn" title="Show tracks">\u203A</button>
         </li>
      `
         )
         .join('');

      if (append) {
         container.insertAdjacentHTML('beforeend', html);
         const allItems = container.querySelectorAll('.music-library-item[data-album]');
         const newItems = Array.from(allItems).slice(-albums.length);
         bindAlbumListItemEvents(newItems);
      } else {
         container.innerHTML = html;
         bindAlbumListItemEvents(container.querySelectorAll('.music-library-item[data-album]'));
      }
   }

   /**
    * Bind events for album list items
    * @param {NodeList|Array} items - Album list items to bind
    */
   function bindAlbumListItemEvents(items) {
      items.forEach((item) => {
         const albumName = item.dataset.album;
         const albumKey = item.dataset.albumKey;

         // Drill-down to show album's tracks
         function drillIntoAlbum() {
            const payload = { type: 'tracks_by_album', album: albumName };
            if (albumKey) payload.album_key = albumKey;
            DawnWS.send({ type: 'music_library', payload });
         }

         item.addEventListener('dblclick', (e) => {
            if (e.target.closest('.music-search-add-btn')) return;
            drillIntoAlbum();
         });

         const drillBtn = item.querySelector('.music-library-drill-btn');
         if (drillBtn) {
            drillBtn.addEventListener('click', (e) => {
               e.stopPropagation();
               drillIntoAlbum();
            });
         }

         // + button to add whole album
         const addBtn = item.querySelector('.music-search-add-btn');
         if (addBtn) {
            addBtn.addEventListener('click', (e) => {
               e.stopPropagation();
               const addPayload = { album: albumName };
               if (albumKey) addPayload.album_key = albumKey;
               DawnMusicPlayback.control('add_album', addPayload);
               addBtn.textContent = '\u2713';
               setTimeout(() => {
                  addBtn.textContent = '+';
               }, 1000);
            });
         }
      });
   }

   /**
    * Bind events for track list items in a container
    */
   function bindTrackListEvents(container) {
      bindTrackListItemEvents(container.querySelectorAll('.music-library-item[data-path]'));
   }

   /**
    * Bind events for individual track list items
    * @param {NodeList|Array} items - Track list items to bind
    */
   function bindTrackListItemEvents(items) {
      items.forEach((item) => {
         const path = item.dataset.path;
         if (!path) return;

         const trackMeta = {
            path: path,
            title: item.dataset.title || '',
            artist: item.dataset.artist || '',
            album: item.dataset.album || '',
            duration_sec: parseInt(item.dataset.duration) || 0,
         };

         // Click on info area = play immediately
         const infoArea = item.querySelector('.music-library-item-info');
         if (infoArea) {
            infoArea.addEventListener('click', (e) => {
               e.stopPropagation();
               DawnMusicPlayback.control('play', trackMeta);
               switchTab('playing');
            });
         }

         // + button = add to queue
         const addBtn = item.querySelector('.music-search-add-btn');
         if (addBtn) {
            addBtn.addEventListener('click', (e) => {
               e.stopPropagation();
               DawnMusicPlayback.control('add_to_queue', trackMeta);
               addBtn.textContent = '\u2713';
               setTimeout(() => {
                  addBtn.textContent = '+';
               }, 1000);
            });
         }
      });
   }

   /**
    * Handle queue response
    * @param {object} payload - Queue data payload
    */
   function handleQueueResponse(payload) {
      if (!elements.queueList) return;

      // Save queue to localStorage for persistence across refreshes
      if (payload.queue) {
         const queueData = {
            tracks: payload.queue.map((t) => ({
               path: t.path,
               title: t.title,
               artist: t.artist,
               album: t.album,
               duration_sec: t.duration_sec,
            })),
            currentIndex: payload.current_index || 0,
         };
         localStorage.setItem('dawn_music_queue', JSON.stringify(queueData));
      }

      // Update clear button visibility
      const clearBtn = document.getElementById('music-clear-queue');
      if (clearBtn) {
         clearBtn.style.display = payload.queue && payload.queue.length > 0 ? 'block' : 'none';
      }

      if (!payload.queue || payload.queue.length === 0) {
         elements.queueList.innerHTML = '<div class="music-queue-empty">Queue is empty</div>';
         localStorage.removeItem('dawn_music_queue');
         if (elements.queueFilter) {
            elements.queueFilter.style.display = 'none';
            elements.queueFilter.value = '';
         }
         return;
      }

      // Show filter when queue has items
      if (elements.queueFilter) {
         elements.queueFilter.style.display = 'block';
      }

      const html = payload.queue
         .map(
            (track, i) => `
         <li class="music-queue-item${i === payload.current_index ? ' playing' : ''}" data-index="${i}">
            <span class="music-queue-item-number">${i + 1}</span>
            <div class="music-queue-item-info">
               <div class="music-queue-item-title">${escapeHtml(track.title || 'Unknown')}</div>
               <div class="music-queue-item-artist">${escapeHtml(track.artist || '')}</div>
            </div>
            <span class="music-queue-item-duration">${formatTime(track.duration_sec || 0)}</span>
            <button class="music-queue-remove-btn" data-action="remove" title="Remove">\u2212</button>
         </li>
      `
         )
         .join('');

      elements.queueList.innerHTML = html;

      // Bind queue item events - double-click to play
      elements.queueList.querySelectorAll('.music-queue-item').forEach((item) => {
         item.addEventListener('dblclick', (e) => {
            if (e.target.closest('.music-queue-remove-btn')) return;
            const index = parseInt(item.dataset.index);
            changeTrack('play_index', { index: index });
         });
      });

      elements.queueList.querySelectorAll('.music-queue-remove-btn').forEach((btn) => {
         btn.addEventListener('click', (e) => {
            e.stopPropagation();
            const index = parseInt(btn.closest('.music-queue-item').dataset.index);
            DawnMusicPlayback.control('remove_from_queue', { index: index });
         });
      });
   }

   /**
    * Filter queue items by title/artist based on the queue filter input
    */
   function filterQueue() {
      if (!elements.queueList || !elements.queueFilter) return;
      const query = elements.queueFilter.value.trim().toLowerCase();
      const items = elements.queueList.querySelectorAll('.music-queue-item');
      items.forEach((item) => {
         if (!query) {
            item.classList.remove('hidden');
            return;
         }
         const title =
            item.querySelector('.music-queue-item-title')?.textContent.toLowerCase() || '';
         const artist =
            item.querySelector('.music-queue-item-artist')?.textContent.toLowerCase() || '';
         item.classList.toggle('hidden', !title.includes(query) && !artist.includes(query));
      });
   }

   /**
    * Start visualizer animation
    */
   function startVisualizer() {
      if (visualizerAnimationId || !elements.visualizer) return;

      const numBars = 32;

      // Create visualizer bars if not exists
      if (elements.visualizer.children.length === 0) {
         for (let i = 0; i < numBars; i++) {
            const bar = document.createElement('div');
            bar.className = 'music-viz-bar';
            elements.visualizer.appendChild(bar);
         }
      }

      // Pre-calculate logarithmic frequency bin mapping
      // Maps bars 0-31 to FFT bins using log scale (more bars for bass, fewer for treble)
      // At 48kHz with fftSize=2048: 1024 bins, each ~23.4Hz wide
      // We focus on 60Hz - 16kHz range (bins ~2-680)
      const binMapping = [];
      const minFreq = 60;
      const maxFreq = 16000;
      const sampleRate = 48000;
      const fftSize = 2048;
      const binWidth = sampleRate / fftSize;

      for (let i = 0; i < numBars; i++) {
         // Logarithmic interpolation between min and max frequency
         const freq = minFreq * Math.pow(maxFreq / minFreq, i / (numBars - 1));
         const bin = Math.round(freq / binWidth);
         binMapping.push(Math.min(bin, fftSize / 2 - 1));
      }

      function animate() {
         if (document.hidden) {
            // Pause animation when tab is hidden; visibilitychange restarts it
            visualizerAnimationId = null;
            return;
         }

         const analyser = DawnMusicPlayback.getAnalyser();
         const fftData = DawnMusicPlayback.getFFTData();

         if (analyser && fftData) {
            analyser.getByteFrequencyData(fftData);
            const bars = elements.visualizer.children;

            for (let i = 0; i < bars.length; i++) {
               // Average a few bins around the target for smoother display
               const centerBin = binMapping[i];
               const startBin = Math.max(0, centerBin - 1);
               const endBin = Math.min(fftData.length - 1, centerBin + 1);
               let sum = 0;
               for (let b = startBin; b <= endBin; b++) {
                  sum += fftData[b];
               }
               const value = sum / (endBin - startBin + 1);
               const height = (value / 255) * 100;
               bars[i].style.height = `${Math.max(2, height)}%`;
            }
         } else {
            // Simulate visualization when no real data
            const bars = elements.visualizer.children;
            for (let i = 0; i < bars.length; i++) {
               const height = Math.random() * 60 + 10;
               bars[i].style.height = `${height}%`;
            }
         }

         visualizerAnimationId = requestAnimationFrame(animate);
      }

      // Resume animation when tab becomes visible (register once)
      if (!startVisualizer._visListenerAdded) {
         document.addEventListener('visibilitychange', function () {
            if (!document.hidden && !visualizerAnimationId && elements.visualizer) {
               startVisualizer();
            }
         });
         startVisualizer._visListenerAdded = true;
      }

      animate();
   }

   /**
    * Stop visualizer animation
    */
   function stopVisualizer() {
      if (visualizerAnimationId) {
         cancelAnimationFrame(visualizerAnimationId);
         visualizerAnimationId = null;
      }

      // Reset bars
      if (elements.visualizer) {
         const bars = elements.visualizer.children;
         for (let i = 0; i < bars.length; i++) {
            bars[i].style.height = '2%';
         }
      }
   }

   /**
    * Format seconds as MM:SS
    * @param {number} seconds - Time in seconds
    * @returns {string} Formatted time
    */
   function formatTime(seconds) {
      if (!seconds || isNaN(seconds)) return '0:00';
      const mins = Math.floor(seconds / 60);
      const secs = Math.floor(seconds % 60);
      return `${mins}:${secs.toString().padStart(2, '0')}`;
   }

   /**
    * Escape HTML entities
    * @param {string} str - String to escape
    * @returns {string} Escaped string
    */
   function escapeHtml(str) {
      const div = document.createElement('div');
      div.textContent = str;
      return div.innerHTML;
   }

   /** Escape a string for use inside a double-quoted HTML attribute */
   function escapeAttr(str) {
      return escapeHtml(str).replace(/"/g, '&quot;');
   }

   /**
    * Check if panel is open
    * @returns {boolean}
    */
   function isVisible() {
      return isOpen;
   }

   // Expose globally
   global.DawnMusicUI = {
      init: init,
      open: open,
      close: close,
      toggle: toggle,
      isVisible: isVisible,
   };
})(window);
