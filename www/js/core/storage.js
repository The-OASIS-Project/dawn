/**
 * DAWN Client Storage Module
 * Thin, safe wrapper over localStorage with a central key registry.
 *
 * SCOPE: client-local UI/session state ONLY (theme cache, session token, small
 * per-client toggles).  DAWN *application* settings live server-side (dawn.toml
 * / user_settings), never here — do not turn DawnStore into a shadow settings
 * store.
 *
 * Every access is try/catch-guarded so private-mode / disabled-storage / quota
 * failures degrade to the fallback instead of throwing (mirrors the defensive
 * pattern already used in state.js / media-session.js).
 *
 * Usage:
 *   DawnStore.get(DawnStore.KEYS.THEME, 'cyan')
 *   DawnStore.set(DawnStore.KEYS.THEME, 'purple')
 *   DawnStore.getBool(DawnStore.KEYS.TTS_ENABLED, false)
 *   DawnStore.getJSON(DawnStore.KEYS.MUSIC_QUEUE, [])
 *
 * NOTE: the pre-paint theme bootstrap in index.html/login.html reads
 * localStorage['dawn_theme'] DIRECTLY (it runs before any script loads, so
 * DawnStore is not yet defined).  That single key string is mirrored in
 * DawnStore.KEYS.THEME below — keep them in sync.
 */
(function (global) {
   'use strict';

   /**
    * Central registry of localStorage key strings.  Values are the EXACT
    * historical strings so existing users lose nothing when call sites migrate.
    * Frozen so a typo'd key throws in strict mode rather than silently writing
    * a new one.
    */
   // Only keys actually managed through DawnStore live here — a key with no
   // DawnStore consumer would just be a second spelling of a raw literal, which
   // is the drift this registry exists to prevent.  Add a key here at the same
   // time you migrate its call sites off raw localStorage.
   const KEYS = Object.freeze({
      THEME: 'dawn_theme',
      SESSION_TOKEN: 'dawn_session_token',
      TTS_ENABLED: 'ttsEnabled',
      DEBUG_MODE: 'dawn_debug_mode',
      MUSIC_PANEL_OPEN: 'dawn_music_panel_open',
      SETTINGS_SHOW_ADVANCED: 'dawn-settings-show-advanced',
      CALENDAR_VIEW: 'dawn_calendar_view',
      CALENDAR_UPCOMING_ONLY: 'dawn_calendar_upcoming_only',
   });

   /**
    * Read a raw string value.
    * @param {string} key
    * @param {*} [fallback=null]
    * @returns {string|*} stored string, or fallback if missing/unavailable
    */
   function get(key, fallback) {
      if (fallback === undefined) fallback = null;
      try {
         const v = localStorage.getItem(key);
         return v === null ? fallback : v;
      } catch (e) {
         return fallback;
      }
   }

   /**
    * Write a raw string value.
    * @returns {boolean} true on success
    */
   function set(key, value) {
      try {
         localStorage.setItem(key, value);
         return true;
      } catch (e) {
         return false;
      }
   }

   /**
    * Remove a key.
    * @returns {boolean} true on success
    */
   function remove(key) {
      try {
         localStorage.removeItem(key);
         return true;
      } catch (e) {
         return false;
      }
   }

   /**
    * Read a boolean (stored as the string 'true'/'false').
    * @param {string} key
    * @param {boolean} [fallback=false]
    * @returns {boolean}
    */
   function getBool(key, fallback) {
      if (fallback === undefined) fallback = false;
      const v = get(key, null);
      if (v === null) return fallback;
      return v === 'true';
   }

   /**
    * Write a boolean.
    * @returns {boolean} true on success
    */
   function setBool(key, value) {
      return set(key, value ? 'true' : 'false');
   }

   /**
    * Read + JSON.parse a value.
    * SECURITY: the returned object must never be recursively merged into another
    * object (e.g. a deep `merge`/`Object.assign`-into-existing).  localStorage is
    * same-origin-writable, so a crafted `{"__proto__":{...}}` could pollute via a
    * recursive merge.  Current consumers only read `.value`/index fields — safe.
    * @param {string} key
    * @param {*} [fallback=null]
    * @returns {*} parsed value, or fallback on missing / parse error
    */
   function getJSON(key, fallback) {
      if (fallback === undefined) fallback = null;
      const raw = get(key, null);
      if (raw === null) return fallback;
      try {
         return JSON.parse(raw);
      } catch (e) {
         return fallback;
      }
   }

   /**
    * JSON.stringify + write a value.
    * @returns {boolean} true on success
    */
   function setJSON(key, value) {
      try {
         return set(key, JSON.stringify(value));
      } catch (e) {
         return false;
      }
   }

   // Expose globally
   global.DawnStore = {
      KEYS: KEYS,
      get: get,
      set: set,
      remove: remove,
      getBool: getBool,
      setBool: setBool,
      getJSON: getJSON,
      setJSON: setJSON,
   };
})(window);
