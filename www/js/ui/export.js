/**
 * DAWN Conversation Export Module
 * Builds self-contained HTML exports from conversation JSON data
 */
(function () {
   'use strict';

   /* =============================================================================
    * ASCII Art Header
    * ============================================================================= */

   const ASCII_HEADER =
      `____        ___        _       __   _   _ \n` +
      ` |  _ \\      / _ \\      | |     / /  | \\ | |  \n` +
      ` | | | |    | |_| |     | | /| / /   |  \\| |  \n` +
      ` | |_| |    |  _  |     | |/ |/ /    | |\\  |  \n` +
      `    |____/ (_) |_| |_| (_) |__/|__/ (_) |_| \\_| (_) `;

   /* =============================================================================
    * Theme Snapshot
    * ============================================================================= */

   const THEME_VARS = [
      '--bg-primary',
      '--bg-secondary',
      '--bg-tertiary',
      '--text-primary',
      '--text-secondary',
      '--text-tertiary',
      '--accent',
      '--accent-dim',
      '--accent-subtle',
      '--accent-border',
      '--accent-hover',
      '--accent-focus',
      '--accent-medium',
      '--error',
      '--success',
      '--warning',
      '--border-radius',
   ];

   /**
    * Snapshot current theme CSS variable values from the DOM
    * Sanitizes values to prevent style-block breakout in exported HTML
    * @returns {string} CSS :root block with resolved values
    */
   function snapshotTheme() {
      const style = getComputedStyle(document.documentElement);
      const themeName = typeof DawnTheme !== 'undefined' ? DawnTheme.current() : 'cyan';
      let css = `   /* DAWN Theme: ${themeName} */\n`;
      THEME_VARS.forEach((v) => {
         let val = style.getPropertyValue(v).trim();
         if (val) {
            // Sanitize: strip anything that could break out of the style block
            val = val.replace(/<\//g, '');
            css += `   ${v}: ${val};\n`;
         }
      });
      return css;
   }

   /* =============================================================================
    * HTML Escape
    * ============================================================================= */

   /**
    * Escape HTML special characters to prevent XSS in exported files
    * @param {string} str - Raw text
    * @returns {string} HTML-safe text
    */
   function escapeHtml(str) {
      if (!str) return '';
      return str
         .replace(/&/g, '&amp;')
         .replace(/</g, '&lt;')
         .replace(/>/g, '&gt;')
         .replace(/"/g, '&quot;');
   }

   /* =============================================================================
    * Date/Time Formatting
    * ============================================================================= */

   /**
    * Format an ISO 8601 timestamp for display
    * @param {string} iso - ISO 8601 timestamp
    * @returns {string} Formatted date/time
    */
   function formatDateTime(iso) {
      if (!iso) return '';
      return DawnTime.format(
         Date.parse(iso) / 1000,
         {
            year: 'numeric',
            month: 'short',
            day: 'numeric',
            hour: 'numeric',
            minute: '2-digit',
         },
         'en-US'
      );
   }

   /**
    * Format just the time portion
    * @param {string} iso - ISO 8601 timestamp
    * @returns {string} Time string (e.g., "2:34 PM")
    */
   function formatTime(iso) {
      if (!iso) return '';
      return DawnTime.format(
         Date.parse(iso) / 1000,
         { hour: 'numeric', minute: '2-digit' },
         'en-US'
      );
   }

   /* =============================================================================
    * Lightweight Markdown to HTML
    * ============================================================================= */

   /**
    * Convert markdown text to HTML. Handles common patterns:
    * headers, bold, italic, code blocks, inline code, links, lists, blockquotes
    * @param {string} text - Raw markdown text (already HTML-escaped)
    * @returns {string} HTML string
    */
   function markdownToHtml(text) {
      if (!text) return '';

      // Work with raw text, escape HTML first
      let src = escapeHtml(text);

      // Fenced code blocks: ```lang\n...\n```
      src = src.replace(
         /```(\w*)\n([\s\S]*?)```/g,
         (_, lang, code) => `<pre class="code-block"><code>${code.trimEnd()}</code></pre>`
      );

      // Process line-by-line for block elements
      const lines = src.split('\n');
      const out = [];
      let inList = false;
      let listType = null; // 'ul' or 'ol'
      let inBlockquote = false;

      for (let i = 0; i < lines.length; i++) {
         let line = lines[i];

         // Skip lines inside code blocks (already handled)
         if (line.includes('<pre class="code-block">')) {
            // Collect until closing </pre>
            let block = line;
            while (!block.includes('</pre>') && i + 1 < lines.length) {
               i++;
               block += '\n' + lines[i];
            }
            closeList();
            closeBlockquote();
            out.push(block);
            continue;
         }

         // Blockquote
         const bqMatch = line.match(/^&gt;\s?(.*)/);
         if (bqMatch) {
            closeList();
            if (!inBlockquote) {
               out.push('<blockquote>');
               inBlockquote = true;
            }
            out.push(inlineFormat(bqMatch[1]));
            continue;
         } else if (inBlockquote) {
            closeBlockquote();
         }

         // Headers: # through ####
         const hMatch = line.match(/^(#{1,4})\s+(.+)/);
         if (hMatch) {
            closeList();
            const level = hMatch[1].length;
            out.push(`<h${level}>${inlineFormat(hMatch[2])}</h${level}>`);
            continue;
         }

         // Horizontal rule
         if (/^(-{3,}|\*{3,}|_{3,})$/.test(line.trim())) {
            closeList();
            out.push('<hr>');
            continue;
         }

         // Unordered list item: - or * or +
         const ulMatch = line.match(/^(\s*)[-*+]\s+(.*)/);
         if (ulMatch) {
            if (!inList || listType !== 'ul') {
               closeList();
               out.push('<ul>');
               inList = true;
               listType = 'ul';
            }
            out.push(`<li>${inlineFormat(ulMatch[2])}</li>`);
            continue;
         }

         // Ordered list item: 1. 2. etc
         const olMatch = line.match(/^(\s*)\d+\.\s+(.*)/);
         if (olMatch) {
            if (!inList || listType !== 'ol') {
               closeList();
               out.push('<ol>');
               inList = true;
               listType = 'ol';
            }
            out.push(`<li>${inlineFormat(olMatch[2])}</li>`);
            continue;
         }

         // Close any open list if this isn't a list item
         closeList();

         // Empty line = paragraph break
         if (line.trim() === '') {
            out.push('<br>');
            continue;
         }

         // Normal paragraph text
         out.push(inlineFormat(line));
         // Add <br> for consecutive non-empty lines (soft line breaks)
         if (i + 1 < lines.length && lines[i + 1].trim() !== '' && !isBlockStart(lines[i + 1])) {
            out.push('<br>');
         }
      }

      closeList();
      closeBlockquote();
      return out.join('\n');

      function closeList() {
         if (inList) {
            out.push(listType === 'ol' ? '</ol>' : '</ul>');
            inList = false;
            listType = null;
         }
      }

      function closeBlockquote() {
         if (inBlockquote) {
            out.push('</blockquote>');
            inBlockquote = false;
         }
      }

      function isBlockStart(l) {
         return (
            /^#{1,4}\s/.test(l) ||
            /^[-*+]\s/.test(l) ||
            /^\d+\.\s/.test(l) ||
            /^&gt;\s/.test(l) ||
            /^(-{3,}|\*{3,}|_{3,})$/.test(l.trim())
         );
      }
   }

   /**
    * Check if a URL (after HTML entity decoding) contains unsafe characters
    * that could break out of an href attribute
    * @param {string} url - HTML-escaped URL string
    * @returns {boolean} true if the URL is safe for href attribute use
    */
   function isSafeUrl(url) {
      // Decode HTML entities back to check the raw URL
      const raw = url
         .replace(/&lt;/g, '<')
         .replace(/&gt;/g, '>')
         .replace(/&quot;/g, '"')
         .replace(/&amp;/g, '&'); // last, so "&amp;lt;" stays "&lt;"
      return !/["'<>]/.test(raw);
   }

   /**
    * Apply inline markdown formatting: bold, italic, inline code, links
    * @param {string} text - Single line of HTML-escaped text
    * @returns {string} Line with inline formatting applied
    */
   function inlineFormat(text) {
      return (
         text
            // Inline code (must come before bold/italic to avoid conflicts)
            .replace(/`([^`]+)`/g, '<code>$1</code>')
            // Bold + italic
            .replace(/\*\*\*(.+?)\*\*\*/g, '<strong><em>$1</em></strong>')
            // Bold
            .replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>')
            // Italic
            .replace(/\*(.+?)\*/g, '<em>$1</em>')
            // Links: [text](url) — reject URLs with unsafe chars after entity decode
            .replace(/\[([^\]]+)\]\((https?:\/\/[^)]+)\)/g, (_, linkText, url) =>
               isSafeUrl(url)
                  ? `<a href="${url}" target="_blank" rel="noopener">${linkText}</a>`
                  : `${linkText} (${url})`
            )
            // Bare URLs — reject URLs with unsafe chars after entity decode
            .replace(/(?<!["=])(https?:\/\/[^\s<]+)/g, (_, url) =>
               isSafeUrl(url) ? `<a href="${url}" target="_blank" rel="noopener">${url}</a>` : url
            )
      );
   }

   /* =============================================================================
    * HTML Builder
    * ============================================================================= */

   /**
    * Build a self-contained HTML document from export data
    * @param {Object} data - The export JSON data object
    * @returns {string} Complete HTML document string
    */
   function buildHtmlExport(data) {
      const conv = data.conversation || {};
      const messages = data.messages || [];
      const themeVars = snapshotTheme();

      // Get AI name from config
      const config = typeof DawnSettings !== 'undefined' ? DawnSettings.getConfig() : null;
      const aiName = config?.general?.ai_name
         ? config.general.ai_name.charAt(0).toUpperCase() + config.general.ai_name.slice(1)
         : 'Assistant';

      // Build messages HTML
      const messagesHtml = messages
         .map((msg) => {
            const isUser = msg.role === 'user';
            const roleLabel = escapeHtml(isUser ? 'You' : aiName);
            const roleClass = isUser ? 'user' : 'assistant';
            const time = formatTime(msg.timestamp);
            const content = markdownToHtml(msg.content);

            return `      <div class="msg ${roleClass}">
         <div class="msg-header">
            <span class="msg-role">${roleLabel}</span>
            <span class="msg-time">${time}</span>
         </div>
         <div class="msg-body">${content}</div>
      </div>`;
         })
         .join('\n');

      // Build metadata items
      const metaItems = [];

      const created = formatDateTime(conv.created_at);
      if (created) metaItems.push({ label: 'Date', value: created });

      if (conv.message_count)
         metaItems.push({
            label: 'Messages',
            value: String(conv.message_count),
         });

      const model = conv.llm_settings?.model;
      if (model) metaItems.push({ label: 'Model', value: model });

      if (conv.origin)
         metaItems.push({
            label: 'Source',
            value: conv.origin === 'webui' ? 'WebUI' : conv.origin,
         });

      if (conv.llm_settings?.thinking_mode && conv.llm_settings.thinking_mode !== 'disabled')
         metaItems.push({
            label: 'Thinking',
            value: conv.llm_settings.thinking_mode,
         });

      if (conv.is_private) metaItems.push({ label: 'Privacy', value: 'Private' });

      const metaHtml = metaItems
         .map(
            (m) => `            <div class="meta-item">
               <div class="meta-label">${escapeHtml(m.label)}</div>
               <div class="meta-value">${escapeHtml(m.value)}</div>
            </div>`
         )
         .join('\n');

      return `<!DOCTYPE html>
<html lang="en">
<head>
   <meta charset="UTF-8">
   <meta name="viewport" content="width=device-width, initial-scale=1.0">
   <title>D.A.W.N. - ${escapeHtml(conv.title || 'Conversation Export')}</title>
   <style>
:root {
${themeVars}}
*, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
body {
   background: #121417;
   background: var(--bg-primary);
   color: #e6e6e6;
   color: var(--text-primary);
   font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
   padding: 2rem 1rem;
   -webkit-font-smoothing: antialiased;
}
.container { max-width: 780px; margin: 0 auto; }
.ascii-header {
   font-family: 'Consolas', 'SF Mono', 'Monaco', monospace;
   color: var(--accent);
   font-size: 0.65rem;
   line-height: 1.2;
   text-align: center;
   margin-bottom: 0.5rem;
   white-space: pre;
   user-select: none;
}
.export-label {
   font-family: 'Consolas', 'SF Mono', monospace;
   font-size: 0.7rem;
   text-transform: uppercase;
   letter-spacing: 0.15em;
   color: var(--text-tertiary);
   text-align: center;
   margin-bottom: 1.5rem;
}
.header-rule {
   border: none;
   border-top: 1px solid var(--accent-border);
   margin-bottom: 2rem;
}
.meta-card {
   background: var(--bg-secondary);
   border: 1px solid var(--accent-border);
   border-radius: var(--border-radius);
   padding: 1.25rem 1.5rem;
   margin-bottom: 2rem;
}
.meta-title {
   font-size: 1.1rem;
   font-weight: 600;
   color: var(--text-primary);
   margin-bottom: 1rem;
   padding-bottom: 0.75rem;
   border-bottom: 1px solid var(--accent-border);
}
.meta-grid {
   display: grid;
   grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
   gap: 0.75rem 2rem;
}
.meta-label {
   font-family: 'Consolas', 'SF Mono', monospace;
   font-size: 0.7rem;
   text-transform: uppercase;
   letter-spacing: 0.08em;
   color: var(--text-tertiary);
   margin-bottom: 0.15rem;
}
.meta-value {
   font-size: 0.9rem;
   color: var(--text-primary);
}
.messages { margin-top: 1rem; }
.msg {
   padding: 1rem 1.25rem;
   margin-bottom: 1rem;
   border-radius: var(--border-radius);
}
.msg-header {
   display: flex;
   justify-content: space-between;
   align-items: baseline;
   margin-bottom: 0.5rem;
}
.msg-role {
   font-family: 'Consolas', 'SF Mono', 'Monaco', monospace;
   font-size: 0.7rem;
   text-transform: uppercase;
   letter-spacing: 0.08em;
   color: var(--text-secondary);
}
.msg-time {
   font-family: 'Consolas', 'SF Mono', 'Monaco', monospace;
   font-size: 0.7rem;
   color: var(--text-tertiary);
}
.msg-body {
   font-size: 0.9375rem;
   line-height: 1.6;
   color: var(--text-primary);
   word-wrap: break-word;
   overflow-wrap: break-word;
}
.msg-body code {
   font-family: 'Consolas', 'SF Mono', monospace;
   font-size: 0.85em;
   background: var(--bg-tertiary);
   padding: 0.15em 0.4em;
   border-radius: 3px;
}
.msg-body pre.code-block {
   background: var(--bg-tertiary);
   border: 1px solid var(--accent-border);
   border-radius: 6px;
   padding: 0.75rem 1rem;
   margin: 0.5rem 0;
   overflow-x: auto;
   font-size: 0.85em;
   line-height: 1.5;
}
.msg-body pre.code-block code {
   background: none;
   padding: 0;
   border-radius: 0;
   font-size: inherit;
}
.msg-body h1, .msg-body h2, .msg-body h3, .msg-body h4 {
   margin: 0.75rem 0 0.35rem;
   color: var(--text-primary);
}
.msg-body h1 { font-size: 1.2em; }
.msg-body h2 { font-size: 1.1em; }
.msg-body h3 { font-size: 1.0em; }
.msg-body h4 { font-size: 0.95em; color: var(--text-secondary); }
.msg-body ul, .msg-body ol {
   margin: 0.35rem 0;
   padding-left: 1.5rem;
}
.msg-body li { margin-bottom: 0.2rem; }
.msg-body blockquote {
   border-left: 3px solid var(--accent-dim);
   padding: 0.25rem 0.75rem;
   margin: 0.5rem 0;
   color: var(--text-secondary);
   font-style: italic;
}
.msg-body a {
   color: var(--accent);
   text-decoration: none;
}
.msg-body a:hover { text-decoration: underline; }
.msg-body strong { font-weight: 600; }
.msg-body hr {
   border: none;
   border-top: 1px solid var(--accent-border);
   margin: 0.75rem 0;
}
.msg.user {
   background: var(--bg-tertiary);
}
.msg.assistant {
   background: var(--accent-subtle);
   border-left: 3px solid var(--accent);
}
.msg.assistant .msg-role {
   color: var(--accent);
}
.msg.system {
   background: var(--bg-secondary);
   border-left: 3px solid var(--text-tertiary);
   opacity: 0.7;
}
.export-footer {
   text-align: center;
   padding: 2rem 0 1rem;
   border-top: 1px solid var(--accent-border);
   margin-top: 2rem;
}
.export-footer span {
   font-family: 'Consolas', 'SF Mono', monospace;
   font-size: 0.7rem;
   text-transform: uppercase;
   letter-spacing: 0.1em;
   color: var(--text-tertiary);
}
@media (max-width: 600px) {
   body { padding: 0.75rem; }
   .ascii-header { font-size: 0.45rem; }
   .meta-grid { grid-template-columns: 1fr 1fr; }
   .msg { padding: 0.75rem 1rem; }
   .msg-header { flex-direction: column; gap: 0.15rem; }
}
@media (max-width: 400px) {
   .ascii-header { display: none; }
   .meta-grid { grid-template-columns: 1fr; }
}
   </style>
</head>
<body>
   <div class="container">
      <pre class="ascii-header">${ASCII_HEADER}</pre>
      <div class="export-label">Conversation Export</div>
      <hr class="header-rule">

      <div class="meta-card">
         <div class="meta-title">${escapeHtml(conv.title || 'Untitled Conversation')}</div>
         <div class="meta-grid">
${metaHtml}
         </div>
      </div>

      <div class="messages">
${messagesHtml}
      </div>

      <div class="export-footer">
         <span>Exported from D.A.W.N. // The OASIS Project</span>
      </div>
   </div>
</body>
</html>`;
   }

   /* =============================================================================
    * Export
    * ============================================================================= */

   window.DawnExport = {
      buildHtml: buildHtmlExport,
   };
})();
