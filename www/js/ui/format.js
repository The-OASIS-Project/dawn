/**
 * DAWN Format Utilities Module
 * HTML escaping and markdown formatting with XSS protection
 *
 * Usage:
 *   DawnFormat.escapeHtml('<script>')     // Returns '&lt;script&gt;'
 *   DawnFormat.markdown('**bold**')       // Returns sanitized HTML
 */
(function (global) {
   'use strict';

   // Shared SVG icons for copy buttons (sized via CSS)
   const ICON_CLIPBOARD =
      '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
      'stroke-width="2" stroke-linecap="round" stroke-linejoin="round">' +
      '<rect x="9" y="9" width="13" height="13" rx="2" ry="2"/>' +
      '<path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg>';
   const ICON_CHECK =
      '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
      'stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">' +
      '<polyline points="20 6 9 17 4 12"/></svg>';

   /**
    * Copy text to clipboard with fallback for non-secure (HTTP) contexts
    * @param {string} text - Text to copy
    * @returns {Promise<void>}
    */
   async function copyToClipboard(text) {
      if (navigator.clipboard) {
         return navigator.clipboard.writeText(text);
      }
      // Fallback for plain HTTP (e.g., http://192.168.x.x:3000)
      const textarea = document.createElement('textarea');
      textarea.value = text;
      textarea.style.cssText = 'position:fixed;opacity:0';
      document.body.appendChild(textarea);
      textarea.select();
      document.execCommand('copy');
      document.body.removeChild(textarea);
   }

   /**
    * Escape HTML to prevent XSS
    * @param {string} str - Raw string to escape
    * @returns {string} HTML-escaped string
    */
   function escapeHtml(str) {
      if (!str) return '';
      const div = document.createElement('div');
      div.textContent = str;
      return div.innerHTML;
   }

   /**
    * Format text with full markdown support using marked.js
    * Sanitized with DOMPurify for XSS protection
    * @param {string} text - Markdown text to format
    * @returns {string} Sanitized HTML
    */
   /**
    * Escape a string for safe use inside an HTML attribute value.
    *
    * escapeHtml (above) uses div.textContent → innerHTML, which escapes
    * &lt;/&gt;/&amp; but NOT quotes — safe for text-node insertion but UNSAFE
    * for interpolation between attribute-value quotes.  Use escapeAttr for
    * any `attr="..."` interpolation.
    */
   function escapeAttr(s) {
      if (s == null) return '';
      return String(s)
         .replace(/&/g, '&amp;')
         .replace(/"/g, '&quot;')
         .replace(/'/g, '&#39;')
         .replace(/</g, '&lt;')
         .replace(/>/g, '&gt;');
   }
   const renderer = new marked.Renderer();
   renderer.link = function ({ href, title, text }) {
      const titleAttr = title ? ` title="${escapeAttr(title)}"` : '';
      return `<a href="${escapeAttr(href)}"${titleAttr} target="_blank" rel="noopener noreferrer">${text}</a>`;
   };

   function formatMarkdown(text) {
      const cleanText = text
         // Strip command tags before rendering (they should go to debug panel only)
         .replace(/<command>[\s\S]*?<\/command>/g, '')
         // Strip memory-citation tags (bookkeeping — the server audits + persists
         // clean text; this keeps them out of the live stream too, incl. an
         // orphan opener from a mid-stream break).
         .replace(/<cited>[\s\S]*?<\/cited>/g, '')
         .replace(/<cited>[\s\S]*$/g, '')
         // Compact spelled-out factorial back to the symbol for the printed view
         // ("52 factorial" → "52!"). The LLM is prompted to write "N factorial"
         // so TTS reads it correctly; this restores compact notation on screen.
         // Display-only — the raw text (and thus what TTS speaks) is untouched.
         // EXCEPT when the phrase is quoted: that's the model discussing the
         // notation itself (e.g. 'write it as "52 factorial"'), where collapsing
         // it to "52!" would destroy the contrast it's drawing. Leave those.
         .replace(/(["'`]?)\b(\d+)\s+factorial\b(["'`]?)/gi, (m, pre, n, post) =>
            pre || post ? m : `${n}!`
         );
      // Parse markdown to HTML, then sanitize
      const html = marked.parse(cleanText, { breaks: true, gfm: true, renderer });
      return DOMPurify.sanitize(html, { ADD_ATTR: ['target'] });
   }

   /**
    * Add a copy-message button to a transcript entry's .text element
    * Copies the raw text (markdown or plain) stored in data-raw-text
    * @param {HTMLElement} textEl - The .text element inside a transcript entry
    */
   function addMessageCopyButton(textEl) {
      if (!textEl || textEl.querySelector('.copy-msg-btn')) return;

      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'copy-msg-btn';
      btn.title = 'Copy message';
      btn.setAttribute('aria-label', 'Copy message to clipboard');
      btn.innerHTML = ICON_CLIPBOARD;
      btn.onclick = async (e) => {
         e.stopPropagation();
         const raw = textEl.getAttribute('data-raw-text') || textEl.textContent;
         try {
            await copyToClipboard(raw);
            btn.innerHTML = ICON_CHECK;
            setTimeout(() => (btn.innerHTML = ICON_CLIPBOARD), 2000);
         } catch (err) {
            console.error('Failed to copy message:', err);
         }
      };
      textEl.appendChild(btn);
   }

   /**
    * Add copy buttons to all code blocks in a container
    * @param {HTMLElement} container - Container to search for code blocks
    */
   function addCopyButtons(container) {
      if (!container) return;

      container.querySelectorAll('pre').forEach((pre) => {
         // Skip if already has copy button
         if (pre.querySelector('.copy-btn')) return;

         const btn = document.createElement('button');
         btn.type = 'button';
         btn.className = 'copy-btn';
         btn.title = 'Copy to clipboard';
         btn.setAttribute('aria-label', 'Copy code to clipboard');
         btn.innerHTML = ICON_CLIPBOARD;
         btn.onclick = async (e) => {
            e.stopPropagation();
            const code = pre.querySelector('code');
            const text = code ? code.textContent : pre.textContent;
            try {
               await copyToClipboard(text);
               btn.innerHTML = ICON_CHECK;
               setTimeout(() => (btn.innerHTML = ICON_CLIPBOARD), 2000);
            } catch (err) {
               console.error('Failed to copy:', err);
            }
         };

         pre.appendChild(btn);
      });
   }

   /**
    * Format a date as a relative time string
    * @param {Date} date - Date to format
    * @returns {string} Relative time string (e.g., "5 mins ago", "2 days ago")
    */
   function formatRelativeTime(date) {
      const now = new Date();
      const diffMs = now - date;
      const diffMins = Math.floor(diffMs / 60000);
      const diffHours = Math.floor(diffMins / 60);
      const diffDays = Math.floor(diffHours / 24);

      if (diffMins < 1) return 'Just now';
      if (diffMins < 60) return `${diffMins} min${diffMins !== 1 ? 's' : ''} ago`;
      if (diffHours < 24) return `${diffHours} hour${diffHours !== 1 ? 's' : ''} ago`;
      if (diffDays < 7) return `${diffDays} day${diffDays !== 1 ? 's' : ''} ago`;
      return date.toLocaleDateString();
   }

   /**
    * The configured assistant display name (capitalized), e.g. "Friday".
    * Used for the "AI thought / reasoned" panel header so we never surface the
    * underlying model/provider name (which is mutable infrastructure detail).
    * Falls back to "AI" when the config isn't available yet.
    * @returns {string}
    */
   function assistantName() {
      const cfg = typeof DawnSettings !== 'undefined' ? DawnSettings.getConfig() : null;
      const name = cfg?.general?.ai_name;
      if (!name) return 'AI';
      return name.charAt(0).toUpperCase() + name.slice(1);
   }

   // Expose globally
   global.DawnFormat = {
      escapeHtml: escapeHtml,
      escapeAttr: escapeAttr,
      markdown: formatMarkdown,
      relativeTime: formatRelativeTime,
      addCopyButtons: addCopyButtons,
      addMessageCopyButton: addMessageCopyButton,
      assistantName: assistantName,
   };
})(window);
