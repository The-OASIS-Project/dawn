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

   // UI state
   let isOpen = false;
   let activeTab = 'playing';
   let visualizerAnimationId = null;

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
      artistsList: null,
      albumsList: null,
   };

   // Local state
   const localState = {
      volume: parseFloat(localStorage.getItem('musicVolume') || '0.8'),
      muted: localStorage.getItem('musicMuted') === 'true',
      shuffle: localStorage.getItem('musicShuffle') === 'true',
      repeat: localStorage.getItem('musicRepeat') || 'none', // none, one, all
      lastQueueLength: -1, // Track queue changes for auto-refresh
      lastQueueIndex: -1,
      wasPlaying: false, // Track playing state for repeat detection
      queueRestored: false, // Track if we've restored saved queue
   };

   /**
    * Initialize UI bindings
    */
   function init() {
      cacheElements();
      bindEvents();
      setupCallbacks();
      restoreLocalState();
      restorePanelState();

      console.log('Music UI: Initialized');
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
    * Restore saved queue from localStorage
    * Called after subscription is established
    */
   function restoreSavedQueue() {
      const savedQueue = localStorage.getItem('dawn_music_queue');
      if (!savedQueue) return;

      try {
         const queueData = JSON.parse(savedQueue);
         if (!queueData.tracks || queueData.tracks.length === 0) return;

         console.log('Music UI: Restoring saved queue with', queueData.tracks.length, 'tracks');

         // Add each track to the queue
         queueData.tracks.forEach((track) => {
            DawnMusicPlayback.queue('add', { path: track.path });
         });
      } catch (e) {
         console.error('Music UI: Failed to restore queue:', e);
         localStorage.removeItem('dawn_music_queue');
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

      // Library
      elements.searchInput = document.getElementById('music-search');
      elements.libraryStats = document.querySelector('.music-library-stats');
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

      // Tabs
      elements.tabs.forEach((tab) => {
         tab.addEventListener('click', () => {
            const tabName = tab.dataset.tab;
            switchTab(tabName);
         });
      });

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

      // Clear queue button
      const clearQueueBtn = document.getElementById('music-clear-queue');
      if (clearQueueBtn) {
         clearQueueBtn.addEventListener('click', () => {
            DawnMusicPlayback.control('clear_queue');
         });
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
      // Apply saved volume to playback
      DawnMusicPlayback.setVolume(localState.muted ? 0 : localState.volume);
   }

   /**
    * Open music panel
    */
   function open() {
      if (!elements.panel) return;

      isOpen = true;
      elements.panel.classList.remove('hidden');
      localStorage.setItem('dawn_music_panel_open', 'true');

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

      // Focus management for accessibility
      if (elements.closeBtn) {
         elements.closeBtn.focus();
      }
   }

   /**
    * Close music panel
    */
   function close() {
      if (!elements.panel) return;

      isOpen = false;
      elements.panel.classList.add('hidden');
      localStorage.setItem('dawn_music_panel_open', 'false');

      // Update button state
      const musicBtn = document.getElementById('music-btn');
      if (musicBtn) {
         musicBtn.classList.remove('active');
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

   /**
    * Switch active tab
    * @param {string} tabName - Tab name (playing, queue, library)
    */
   function switchTab(tabName) {
      activeTab = tabName;

      // Update tab buttons
      elements.tabs.forEach((tab) => {
         tab.classList.toggle('active', tab.dataset.tab === tabName);
      });

      // Update tab content
      elements.tabContents.forEach((content) => {
         content.classList.toggle('active', content.dataset.tab === tabName);
      });

      // Load tab-specific data
      if (tabName === 'queue') {
         DawnMusicPlayback.queue('list');
      } else if (tabName === 'library') {
         DawnMusicPlayback.browseLibrary('stats');
      }
   }

   /**
    * Handle play/pause button
    */
   function handlePlayPause() {
      const state = DawnMusicPlayback.getState();

      if (state.playing && !state.paused) {
         DawnMusicPlayback.control('pause');
      } else if (state.paused) {
         DawnMusicPlayback.control('play');
      } else if (state.queueLength > 0) {
         // Have queue items - play from current index
         DawnMusicPlayback.control('play_index', { index: state.queueIndex });
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
      const seekTime = percent * state.durationSec;

      DawnMusicPlayback.control('seek', { position_sec: seekTime });
   }

   /**
    * Handle volume change
    * @param {Event} e - Input event
    */
   function handleVolumeChange(e) {
      localState.volume = parseFloat(e.target.value);
      localState.muted = false;
      localStorage.setItem('musicVolume', localState.volume);
      localStorage.setItem('musicMuted', 'false');
      updateVolumeIcon();
      DawnMusicPlayback.setVolume(localState.volume);
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
    * Handle next button - respects shuffle mode
    */
   function handleNext() {
      const playbackState = DawnMusicPlayback.getState();
      if (localState.shuffle && playbackState.queueLength > 1) {
         // Pick random track (excluding current)
         let newIndex;
         do {
            newIndex = Math.floor(Math.random() * playbackState.queueLength);
         } while (newIndex === playbackState.queueIndex && playbackState.queueLength > 1);
         DawnMusicPlayback.control('play_index', { index: newIndex });
      } else {
         DawnMusicPlayback.control('next');
      }
   }

   /**
    * Handle previous button - respects shuffle mode
    */
   function handlePrevious() {
      const playbackState = DawnMusicPlayback.getState();
      if (localState.shuffle && playbackState.queueLength > 1) {
         // Pick random track (excluding current)
         let newIndex;
         do {
            newIndex = Math.floor(Math.random() * playbackState.queueLength);
         } while (newIndex === playbackState.queueIndex && playbackState.queueLength > 1);
         DawnMusicPlayback.control('play_index', { index: newIndex });
      } else {
         DawnMusicPlayback.control('previous');
      }
   }

   /**
    * Toggle shuffle mode
    */
   function toggleShuffle() {
      localState.shuffle = !localState.shuffle;
      localStorage.setItem('musicShuffle', localState.shuffle);
      updateModeButtons();
   }

   /**
    * Cycle repeat mode
    */
   function cycleRepeat() {
      const modes = ['none', 'one', 'all'];
      const currentIndex = modes.indexOf(localState.repeat);
      localState.repeat = modes[(currentIndex + 1) % modes.length];
      localStorage.setItem('musicRepeat', localState.repeat);
      updateModeButtons();
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
         case 'Escape':
            close();
            break;
         case 'ArrowLeft':
            DawnMusicPlayback.control('previous');
            break;
         case 'ArrowRight':
            DawnMusicPlayback.control('next');
            break;
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

      if (queueChanged && activeTab === 'queue') {
         DawnMusicPlayback.queue('list');
      }

      // Restore saved queue on first state update if queue is empty
      if (!localState.queueRestored && state.queueLength === 0) {
         localState.queueRestored = true;
         restoreSavedQueue();
      } else if (state.queueLength > 0) {
         localState.queueRestored = true; // Mark as restored if server already has queue
      }

      // Handle repeat mode when playback stops
      if (localState.wasPlaying && !state.playing && state.queueLength > 0) {
         if (localState.repeat === 'one') {
            // Repeat current track
            setTimeout(() => {
               DawnMusicPlayback.control('play_index', { index: state.queueIndex });
            }, 100);
         } else if (localState.repeat === 'all' && state.queueIndex === 0) {
            // End of queue reached, loop back (server resets to 0 when done)
            setTimeout(() => {
               if (localState.shuffle) {
                  const newIndex = Math.floor(Math.random() * state.queueLength);
                  DawnMusicPlayback.control('play_index', { index: newIndex });
               } else {
                  DawnMusicPlayback.control('play_index', { index: 0 });
               }
            }, 100);
         }
      }
      localState.wasPlaying = state.playing && !state.paused;

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
      // Update progress bar
      if (elements.progressFill && durationSec > 0) {
         const percent = (positionSec / durationSec) * 100;
         elements.progressFill.style.width = `${percent}%`;
      }

      // Update time displays
      if (elements.currentTime) {
         elements.currentTime.textContent = formatTime(positionSec);
      }
      if (elements.totalTime) {
         elements.totalTime.textContent = formatTime(durationSec);
      }
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
         <li class="music-library-item" data-path="${escapeHtml(track.path)}" data-index="${i}">
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

         // Click on title/info area = play immediately
         const infoArea = item.querySelector('.music-library-item-info');
         if (infoArea) {
            infoArea.addEventListener('click', (e) => {
               e.stopPropagation();
               DawnMusicPlayback.control('play', { path: path });
               switchTab('playing');
            });
         }

         // Click on "+" button = add to queue
         const addBtn = item.querySelector('.music-search-add-btn');
         if (addBtn) {
            addBtn.addEventListener('click', (e) => {
               e.stopPropagation();
               DawnMusicPlayback.control('add_to_queue', { path: path });
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
    * Handle library response
    * @param {object} payload - Library data payload
    */
   function handleLibraryResponse(payload) {
      const browseList = document.getElementById('music-browse-list');
      const browseSection = document.getElementById('music-browse-section');
      const sectionHeader = browseSection?.querySelector('.music-library-section-header');

      if (payload.browse_type === 'stats' && elements.libraryStats) {
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
         // Show all tracks
         if (sectionHeader) sectionHeader.textContent = `All Tracks (${payload.count || 0})`;
         renderTrackList(browseList, payload.tracks || []);
      } else if (payload.browse_type === 'artists' && browseList) {
         // Show artists with stats
         if (sectionHeader) sectionHeader.textContent = `Artists (${payload.count || 0})`;
         renderArtistList(browseList, payload.artists || []);
      } else if (payload.browse_type === 'albums' && browseList) {
         // Show albums with stats
         if (sectionHeader) sectionHeader.textContent = `Albums (${payload.count || 0})`;
         renderAlbumList(browseList, payload.albums || []);
      } else if (payload.browse_type === 'tracks_by_artist' && browseList) {
         // Show tracks for a specific artist
         if (sectionHeader)
            sectionHeader.textContent = `${payload.artist} (${payload.count || 0} tracks)`;
         renderTrackList(browseList, payload.tracks || [], true);
      } else if (payload.browse_type === 'tracks_by_album' && browseList) {
         // Show tracks for a specific album
         if (sectionHeader)
            sectionHeader.textContent = `${payload.album} (${payload.count || 0} tracks)`;
         renderTrackList(browseList, payload.tracks || [], true);
      }
   }

   /**
    * Render a list of tracks with play/add buttons
    */
   function renderTrackList(container, tracks) {
      if (tracks.length === 0) {
         container.innerHTML =
            '<li class="music-library-item"><span class="music-library-item-title">No tracks found</span></li>';
         return;
      }

      const html = tracks
         .map(
            (track) => `
         <li class="music-library-item" data-path="${escapeHtml(track.path)}">
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

      container.innerHTML = html;
      bindTrackListEvents(container);
   }

   /**
    * Render a list of artists with album/track counts
    */
   function renderArtistList(container, artists) {
      if (artists.length === 0) {
         container.innerHTML =
            '<li class="music-library-item"><span class="music-library-item-title">No artists found</span></li>';
         return;
      }

      const html = artists
         .map(
            (artist) => `
         <li class="music-library-item" data-artist="${escapeHtml(artist.name)}">
            <div class="music-library-item-info">
               <div class="music-library-item-title">${escapeHtml(artist.name)}</div>
               <div class="music-library-item-artist">${artist.album_count} album${artist.album_count !== 1 ? 's' : ''}, ${artist.track_count} track${artist.track_count !== 1 ? 's' : ''}</div>
            </div>
            <button class="music-search-add-btn" data-action="add-artist" title="Add all tracks">+</button>
         </li>
      `
         )
         .join('');

      container.innerHTML = html;

      // Artist click handlers
      container.querySelectorAll('.music-library-item[data-artist]').forEach((item) => {
         const artistName = item.dataset.artist;

         // Double-click to show artist's tracks
         item.addEventListener('dblclick', (e) => {
            if (e.target.closest('.music-search-add-btn')) return;
            DawnWS.send({
               type: 'music_library',
               payload: { type: 'tracks_by_artist', artist: artistName },
            });
         });

         // + button to add all artist tracks
         const addBtn = item.querySelector('.music-search-add-btn');
         if (addBtn) {
            addBtn.addEventListener('click', (e) => {
               e.stopPropagation();
               DawnMusicPlayback.control('add_artist', { artist: artistName });
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
    */
   function renderAlbumList(container, albums) {
      if (albums.length === 0) {
         container.innerHTML =
            '<li class="music-library-item"><span class="music-library-item-title">No albums found</span></li>';
         return;
      }

      const html = albums
         .map(
            (album) => `
         <li class="music-library-item" data-album="${escapeHtml(album.name)}">
            <div class="music-library-item-info">
               <div class="music-library-item-title">${escapeHtml(album.name)}</div>
               <div class="music-library-item-artist">${escapeHtml(album.artist || 'Unknown')} \u2022 ${album.track_count} track${album.track_count !== 1 ? 's' : ''}</div>
            </div>
            <button class="music-search-add-btn" data-action="add-album" title="Add all tracks">+</button>
         </li>
      `
         )
         .join('');

      container.innerHTML = html;

      // Album click handlers
      container.querySelectorAll('.music-library-item[data-album]').forEach((item) => {
         const albumName = item.dataset.album;

         // Double-click to show album's tracks
         item.addEventListener('dblclick', (e) => {
            if (e.target.closest('.music-search-add-btn')) return;
            DawnWS.send({
               type: 'music_library',
               payload: { type: 'tracks_by_album', album: albumName },
            });
         });

         // + button to add whole album
         const addBtn = item.querySelector('.music-search-add-btn');
         if (addBtn) {
            addBtn.addEventListener('click', (e) => {
               e.stopPropagation();
               DawnMusicPlayback.control('add_album', { album: albumName });
               addBtn.textContent = '\u2713';
               setTimeout(() => {
                  addBtn.textContent = '+';
               }, 1000);
            });
         }
      });
   }

   /**
    * Bind events for track list items
    */
   function bindTrackListEvents(container) {
      container.querySelectorAll('.music-library-item[data-path]').forEach((item) => {
         const path = item.dataset.path;
         if (!path) return;

         // Click on info area = play immediately
         const infoArea = item.querySelector('.music-library-item-info');
         if (infoArea) {
            infoArea.addEventListener('click', (e) => {
               e.stopPropagation();
               DawnMusicPlayback.control('play', { path: path });
               switchTab('playing');
            });
         }

         // + button = add to queue
         const addBtn = item.querySelector('.music-search-add-btn');
         if (addBtn) {
            addBtn.addEventListener('click', (e) => {
               e.stopPropagation();
               DawnMusicPlayback.control('add_to_queue', { path: path });
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
         return;
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
            DawnMusicPlayback.control('play_index', { index: index });
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
