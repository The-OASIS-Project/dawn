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
   function renderLink({ href, title, text }) {
      const titleAttr = title ? ` title="${escapeAttr(title)}"` : '';
      return `<a href="${escapeAttr(href)}"${titleAttr} target="_blank" rel="noopener noreferrer">${text}</a>`;
   }

   // Raw HTML shown as the characters written (marked hands html() a token:
   // { text, block }).  A block keeps its line breaks.
   function htmlAsText({ text, block }) {
      const shown = escapeAttr(text);
      return block ? `<p>${shown.replace(/\n+$/, '').replace(/\n/g, '<br>')}</p>\n` : shown;
   }

   // A citation tag the strip left in a reply (a mention, or an example
   // mid-reply) and an <end_of_turn> a cloud model mentions are text, not
   // elements DOMPurify would drop; any other HTML in a reply takes the usual
   // path.
   const CITED_TAG_TOKEN_RE = /^<\s*(?:\/?\s*cited|end_of_turn)\s*>/;
   const renderer = new marked.Renderer();
   renderer.link = renderLink;
   renderer.html = (token) =>
      CITED_TAG_TOKEN_RE.test(token.text) ? htmlAsText(token) : token.text;

   // What a user typed shows as typed: HTML in it is text, Markdown still works.
   const userRenderer = new marked.Renderer();
   userRenderer.link = renderLink;
   userRenderer.html = htmlAsText;
   // marked hands over the text inside an inline <code>/<pre>/<script> as
   // already escaped when it is not; escape it here.
   userRenderer.text = function (token) {
      if (token.tokens) {
         return this.parser.parseInline(token.tokens);
      }
      return token.escaped
         ? escapeAttr(token.text)
         : marked.Renderer.prototype.text.call(this, token);
   };

   // The server's citation tag grammar (text_filter.h): CITED_TAG_CONTENT_CHARS,
   // and a tag counts only at the end of the text, with the whitespace after
   // it (or before another tag).
   const CITED_TAG_RE = /<cited>[0-9MmIiDd:[\], \t\n]*<\/cited>\s{0,32}(?=$|<cited>)/g;
   const CITED_TAG_CUT_RE =
      /<cited>[0-9MmIiDd:[\], \t\n]*(?:<(?:\/(?:c(?:i(?:t(?:e(?:d)?)?)?)?)?)?)?$/;

   // What a rendered message may not carry, past DOMPurify's defaults (which
   // allow all of it): a <style> block restyles the whole page; an inline
   // style, or a class naming one of the app's own full-screen rules (a modal
   // overlay), lays an element over it; an id shadows one the app looks up;
   // and a <form> can post what is typed into it to another site.  A reply
   // shaped by a fetched page or an email could otherwise blank or cover the
   // UI.  Nothing rendered here needs a class or an id.
   const SANITIZE_OPTS = {
      ADD_ATTR: ['target'],
      FORBID_TAGS: ['style', 'form'],
      FORBID_ATTR: ['style', 'class', 'id'],
   };

   /**
    * @param {string} text - Markdown to render
    * @param {{user?: boolean, inner?: boolean}} [opts] - user: text the user
    *        typed (no citation tags to strip; HTML in it shows as typed).
    *        inner: a piece of a message that isn't its end (more of it follows),
    *        so a tag ending the piece is not the message's trailing tag.
    */
   function formatMarkdown(text, opts) {
      const user = !!(opts && opts.user);
      const inner = !!(opts && opts.inner);
      const cleanText = (user || inner ? text : stripCitedTags(text))
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
      const html = marked.parse(cleanText, {
         breaks: true,
         gfm: true,
         renderer: user ? userRenderer : renderer,
      });
      return DOMPurify.sanitize(html, SANITIZE_OPTS);
   }

   // Strip memory-citation tags (bookkeeping — the server audits + persists
   // clean text; this keeps them out of the live stream too, incl. a tag cut
   // off by a mid-stream break).  A tag mid-reply is text and stays.  A run
   // past the server's hold ended by prose isn't mirrored: the server removed it.
   function stripCitedTags(text) {
      return text.replace(CITED_TAG_RE, '').replace(CITED_TAG_CUT_RE, '');
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
      return DawnTime.formatDate(date.getTime() / 1000);
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
