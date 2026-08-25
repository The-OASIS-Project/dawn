/**
 * DAWN Visual Renderer Module
 * Detects <dawn-visual> tags in transcript messages and renders them
 * as sandboxed iframes with theme CSS injection and height auto-sizing.
 *
 * Usage:
 *   DawnVisualRender.processElement(textEl)  // Scan for <dawn-visual> tags
 */
(function (global) {
   'use strict';

   /* Track known visual iframe contentWindows for postMessage source validation */
   var knownVisualWindows = new WeakSet();

   /* Map from iframe contentWindow to iframe element for delegated resize handling.
    * Single global listener dispatches by source — no per-iframe listeners needed. */
   var iframeResizeMap = new Map();

   /* Cached vendor scripts for inlining into srcdoc iframes */
   var vendorScriptCache = {};

   /* Cached theme CSS string — rebuilt only when theme changes */
   var cachedThemeCSS = null;
   var cachedVisualClasses = null;

   /* Regex to match <dawn-visual title="..." type="svg|html">...</dawn-visual> */
   var VISUAL_TAG_RE =
      /<dawn-visual\s+title="([^"]*)"\s+type="(svg|html)">([\s\S]*?)<\/dawn-visual>/g;

   /**
    * Build theme CSS that reads actual computed variables from the parent document.
    * This is injected into every iframe so LLM-generated code can use CSS variables.
    */
   function buildThemeCSS() {
      var style = getComputedStyle(document.documentElement);
      var get = function (name) {
         return style.getPropertyValue(name).trim();
      };

      return (
         ':root {\n' +
         '  --color-bg-primary: ' +
         get('--bg-primary') +
         ';\n' +
         '  --color-bg-secondary: ' +
         get('--bg-secondary') +
         ';\n' +
         '  --color-bg-tertiary: ' +
         get('--bg-tertiary') +
         ';\n' +
         '  --color-text-primary: ' +
         get('--text-primary') +
         ';\n' +
         '  --color-text-secondary: ' +
         get('--text-secondary') +
         ';\n' +
         '  --color-text-tertiary: ' +
         get('--text-tertiary') +
         ';\n' +
         '  --color-border: ' +
         get('--border-color') +
         ';\n' +
         '  --color-border-light: ' +
         (get('--border-color-light') || get('--border-color')) +
         ';\n' +
         '  --color-accent: ' +
         get('--accent') +
         ';\n' +
         '  --border-radius-md: 8px;\n' +
         '  --border-radius-lg: 12px;\n' +
         '  --font-sans: Inter, system-ui, sans-serif;\n' +
         '  --font-mono: "JetBrains Mono", monospace;\n' +
         '}\n' +
         'body {\n' +
         '  font-family: var(--font-sans);\n' +
         '  color: var(--color-text-primary);\n' +
         '  background: transparent;\n' +
         '  margin: 0; padding: 0;\n' +
         '}\n'
      );
   }

   /**
    * Build the pre-built CSS classes referenced in _core.md guidelines.
    * These map short class names to styled SVG/HTML elements.
    */
   function buildVisualClasses() {
      return (
         /* Text classes */
         '.t { font-family: var(--font-sans); font-size: 14px; fill: var(--color-text-primary); }\n' +
         '.ts { font-family: var(--font-sans); font-size: 12px; fill: var(--color-text-secondary); }\n' +
         '.th { font-family: var(--font-sans); font-size: 14px; font-weight: 500; fill: var(--color-text-primary); }\n' +
         /* Box class */
         '.box { fill: var(--color-bg-secondary); stroke: var(--color-border); stroke-width: 0.5; }\n' +
         /* Arrow class */
         '.arr { stroke: var(--color-text-secondary); stroke-width: 1.5; fill: none; }\n' +
         /* Clickable node */
         '.node { cursor: pointer; }\n' +
         '.node:hover { opacity: 0.85; }\n' +
         /* Color ramp classes — light mode */
         buildColorRampCSS()
      );
   }

   /**
    * Build color ramp CSS classes from the 9-color palette.
    * Each ramp has fill (50), stroke (600), and text (800) stops.
    */
   function buildColorRampCSS() {
      var ramps = {
         purple: ['#EEEDFE', '#AFA9EC', '#534AB7', '#3C3489'],
         teal: ['#E1F5EE', '#5DCAA5', '#0F6E56', '#085041'],
         coral: ['#FAECE7', '#F0997B', '#993C1D', '#712B13'],
         pink: ['#FBEAF0', '#ED93B1', '#993556', '#72243E'],
         gray: ['#F1EFE8', '#B4B2A9', '#5F5E5A', '#444441'],
         blue: ['#E6F1FB', '#85B7EB', '#185FA5', '#0C447C'],
         green: ['#EAF3DE', '#97C459', '#3B6D11', '#27500A'],
         amber: ['#FAEEDA', '#EF9F27', '#854F0B', '#633806'],
         red: ['#FCEBEB', '#F09595', '#A32D2D', '#791F1F'],
      };

      var css = '';
      var isDark =
         getComputedStyle(document.documentElement)
            .getPropertyValue('--bg-primary')
            .trim()
            .charAt(1) <= '3'; /* Rough dark mode detection: #1x or #2x */

      Object.keys(ramps).forEach(function (name) {
         var r = ramps[name]; /* [50, 200, 600, 800] */
         if (isDark) {
            /* Dark: 800 fill, 200 stroke, light text */
            css +=
               '.c-' +
               name +
               ' rect, .c-' +
               name +
               ' ellipse, .c-' +
               name +
               ' circle, .c-' +
               name +
               ' polygon, .c-' +
               name +
               ' path:not([fill="none"]) { fill: ' +
               r[3] +
               '; stroke: ' +
               r[1] +
               '; }\n';
            css +=
               '.c-' + name + ' text, .c-' + name + ' .th, .c-' + name + ' .t { fill: #f0f0f0; }\n';
            css += '.c-' + name + ' .ts { fill: #ccc; }\n';
         } else {
            /* Light: 50 fill, 600 stroke, 800 text */
            css +=
               '.c-' +
               name +
               ' rect, .c-' +
               name +
               ' ellipse, .c-' +
               name +
               ' circle, .c-' +
               name +
               ' polygon, .c-' +
               name +
               ' path:not([fill="none"]) { fill: ' +
               r[0] +
               '; stroke: ' +
               r[2] +
               '; }\n';
            css +=
               '.c-' +
               name +
               ' text, .c-' +
               name +
               ' .th, .c-' +
               name +
               ' .t { fill: ' +
               r[3] +
               '; }\n';
            css += '.c-' + name + ' .ts { fill: ' + r[2] + '; }\n';
         }
      });

      return css;
   }

   /**
    * Extract viewBox dimensions from SVG code to compute aspect ratio.
    * Returns { width, height } or null if no viewBox found.
    */
   function parseViewBox(svgCode) {
      var match = svgCode.match(/viewBox\s*=\s*"([^"]*)"/);
      if (!match) return null;

      var parts = match[1].trim().split(/\s+/);
      if (parts.length >= 4) {
         return { width: parseFloat(parts[2]), height: parseFloat(parts[3]) };
      }
      return null;
   }

   /**
    * Create a sandboxed iframe for a visual.
    *
    * Phase 1: Static rendering only. No inline scripts (blocked by CSP).
    * The iframe renders SVG/HTML with theme CSS but without interactivity
    * (onclick/sendPrompt). Height is computed from the SVG viewBox aspect ratio.
    *
    * Uses srcdoc with sandbox="allow-scripts". Parent CSP allows
    * 'unsafe-inline' for script-src to enable the bridge script and
    * onclick handlers. The sandbox attribute provides DOM isolation
    * (no allow-same-origin) — iframe cannot access parent page.
    */
   function createVisualFrame(title, type, code) {
      var container = document.createElement('div');
      container.className = 'dawn-visual-container';
      container.setAttribute('data-visual-title', title);

      /* Download button — saves the raw SVG/HTML as a file */
      var downloadBtn = document.createElement('button');
      downloadBtn.className = 'dawn-visual-download';
      downloadBtn.title = 'Download ' + (type === 'svg' ? 'SVG' : 'HTML');
      downloadBtn.setAttribute('aria-label', 'Download visual as file');
      downloadBtn.innerHTML =
         '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
         'stroke-width="2" stroke-linecap="round" stroke-linejoin="round">' +
         '<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/>' +
         '<polyline points="7 10 12 15 17 10"/>' +
         '<line x1="12" y1="15" x2="12" y2="3"/>' +
         '</svg>';
      downloadBtn.addEventListener('click', function () {
         var ext = type === 'svg' ? 'svg' : 'html';
         var mimeType = type === 'svg' ? 'image/svg+xml' : 'text/html';
         var downloadContent;
         if (type === 'svg') {
            /* Embed theme CSS + visual classes into SVG as a <defs><style> block
             * so the downloaded file is self-contained and renders correctly
             * outside the DAWN WebUI iframe context. */
            if (!cachedThemeCSS) cachedThemeCSS = buildThemeCSS();
            if (!cachedVisualClasses) cachedVisualClasses = buildVisualClasses();
            var styleBlock =
               '<defs><style>' + cachedThemeCSS + cachedVisualClasses + '</style></defs>';
            /* Insert style after opening <svg> tag, add XML namespace and
             * background for standalone rendering. Replace width="100%" with
             * the viewBox width so the SVG has intrinsic dimensions. */
            downloadContent = code.replace(/(<svg[^>]*>)/, '$1\n' + styleBlock);
            /* Add xmlns if missing (required for standalone SVG) */
            if (downloadContent.indexOf('xmlns=') === -1) {
               downloadContent = downloadContent.replace(
                  '<svg ',
                  '<svg xmlns="http://www.w3.org/2000/svg" '
               );
            }
            /* Replace width="100%" with viewBox width for standalone sizing */
            var vb = parseViewBox(code);
            if (vb) {
               downloadContent = downloadContent.replace(
                  /width="100%"/,
                  'width="' + vb.width + '"'
               );
            }
            /* Add dark background rect so colors are visible standalone */
            var bgRect =
               '<rect width="100%" height="100%" fill="' +
               getComputedStyle(document.documentElement).getPropertyValue('--bg-primary').trim() +
               '"/>';
            downloadContent = downloadContent.replace(
               /(<\/defs>(?:\s*<\/defs>)?\s*)/,
               '$1' + bgRect + '\n'
            );
         } else {
            if (!cachedThemeCSS) cachedThemeCSS = buildThemeCSS();
            if (!cachedVisualClasses) cachedVisualClasses = buildVisualClasses();
            downloadContent =
               '<!DOCTYPE html><html><head><style>' +
               cachedThemeCSS +
               cachedVisualClasses +
               '</style></head><body>' +
               code +
               '</body></html>';
         }
         var blob = new Blob([downloadContent], { type: mimeType });
         var url = URL.createObjectURL(blob);
         var a = document.createElement('a');
         a.href = url;
         a.download = title + '.' + ext;
         a.click();
         URL.revokeObjectURL(url);
      });
      container.appendChild(downloadBtn);

      var iframe = document.createElement('iframe');
      /* allow-scripts: onclick handlers, sendPrompt bridge, ResizeObserver.
       * No allow-same-origin — iframe cannot access parent DOM/cookies.
       * Vendor scripts (Chart.js) are inlined into srcdoc, not loaded via src. */
      iframe.sandbox = 'allow-scripts';
      iframe.title = 'Visual: ' + title;
      iframe.style.cssText = 'width:100%; border:none; overflow:hidden;';

      /* Cache theme CSS — only rebuild on theme change (MutationObserver in init) */
      if (!cachedThemeCSS) cachedThemeCSS = buildThemeCSS();
      if (!cachedVisualClasses) cachedVisualClasses = buildVisualClasses();
      var themeCSS = cachedThemeCSS;
      var visualClasses = cachedVisualClasses;

      /* Build the bridge script: sendPrompt + debounced ResizeObserver */
      var bridgeScript =
         '<script>\n' +
         'function sendPrompt(t){parent.postMessage({type:"dawn_prompt",text:t},"*")}\n' +
         'var _lastH=0,_tid=0;\n' +
         'new ResizeObserver(function(){\n' +
         '  clearTimeout(_tid);\n' +
         '  _tid=setTimeout(function(){\n' +
         '    var h=document.body.scrollHeight;\n' +
         '    if(h!==_lastH){_lastH=h;\n' +
         '      parent.postMessage({type:"dawn_visual_resize",height:h},"*")}\n' +
         '  },100)\n' +
         '}).observe(document.body);\n' +
         '</' +
         'script>\n';

      var content;
      if (type === 'svg') {
         content =
            '<!DOCTYPE html><html><head><style>' +
            themeCSS +
            visualClasses +
            'body { margin: 0; padding: 0; overflow: hidden; }\n' +
            'svg { display: block; width: 100%; height: auto; }\n' +
            '</style></head><body>\n' +
            bridgeScript +
            code +
            '</body></html>';

         /* Set initial height from viewBox aspect ratio (ResizeObserver refines it).
          * Use pixel estimate based on 680px design width to avoid vw overshoot. */
         var vb = parseViewBox(code);
         if (vb && vb.width > 0) {
            var aspectRatio = vb.height / vb.width;
            iframe.style.height = Math.min(Math.ceil(aspectRatio * 680), vb.height, 800) + 'px';
         } else {
            iframe.style.height = '400px';
         }
      } else {
         /* Gate the Chart.js sizing fix on an actual Chart.js construction, not
          * a bare <canvas>: a non-chart canvas visual (custom 2D drawing) must
          * not get the wrapper + overflow:hidden + forced fill, which would
          * distort it. `new Chart(` is present in every generated chart. */
         var isChartJs = code.indexOf('<canvas') !== -1 && /\bnew\s+Chart\s*\(/.test(code);

         /* --- Chart.js responsive sizing in an opaque-origin srcdoc iframe ---
          *
          * Chart.js sizes a `responsive:true` chart to its canvas's PARENT box.
          * Generated charts put a bare <canvas> directly in <body> (often with
          * only `max-height`), and `maintainAspectRatio:true` locks the chart's
          * aspect ratio to the canvas's DEFAULT 300x150 (ratio 2.0) at
          * construction — the cached `_aspectRatio`. From then on every
          * getMaximumSize() derives height = width / 2 instead of measuring the
          * real container, so the canvas stays 300x150. A manual resize() can't
          * rescue it: a freshly-built chart is mid entrance-animation, and
          * `resize()` while `Chart.animator.running(chart)` is true only stashes
          * `_resizeBeforeDraw` and returns (a true no-op — matches the measured
          * "resize(663,400) does nothing").
          *
          * Canonical Chart.js responsive pattern: wrap the canvas in a
          * position:relative element with a DEFINITE size and let the chart fill
          * it. We do exactly that — inject a `.dawn-chart-box` wrapper (regex,
          * since we can't change the stored markup), and at runtime flip every
          * chart to `maintainAspectRatio:false` then resize() so it adopts the
          * wrapper's real height on its own ResizeObserver. The wrapper's fixed
          * pixel height (clamped by the canvas's own max-height when present) is
          * the source of truth — no reliance on body-box measurement or on
          * resize() timing relative to the entrance animation. */
         var canvasCSS = '';
         var chartFix = '';
         if (isChartJs) {
            /* Resolve a definite wrapper height: honor a stored canvas
             * max-height (cap it to the frame) else use the default frame box. */
            var FRAME_H = 380; /* px; matches the 400px iframe minus container chrome */
            var maxH = code.match(/max-height\s*:\s*(\d+)px/i);
            var boxH = maxH ? Math.min(parseInt(maxH[1], 10), FRAME_H) : FRAME_H;

            canvasCSS =
               'html, body { height: 100%; }\n' +
               'body { margin: 0; padding: 0; overflow: hidden; }\n' +
               /* The wrapper IS the container Chart.js measures: definite size,
                * position:relative so the canvas can absolutely fill it. */
               '.dawn-chart-box { position: relative; width: 100%; height: ' +
               boxH +
               'px; }\n' +
               /* Override the stored inline max-height/margin so the canvas
                * fills the wrapper instead of fighting it. */
               '.dawn-chart-box > canvas { display: block !important;' +
               ' width: 100% !important; height: 100% !important;' +
               ' max-width: none !important; max-height: none !important;' +
               ' margin: 0 !important; }\n';

            /* Wrap each <canvas ...></canvas> in the sizing box. Self-closing
             * <canvas .../> is invalid HTML (canvas needs a closing tag), so
             * matching the open+close pair covers the generated markup. */
            code = code.replace(
               /(<canvas\b[^>]*>\s*<\/canvas>)/gi,
               '<div class="dawn-chart-box">$1</div>'
            );

            /* Runtime: disable maintainAspectRatio (kills the cached 2.0 ratio)
             * and resize so each chart fills its wrapper. maintainAspectRatio
             * false makes getMaximumSize() use the container height directly.
             * Run after the entrance animation settles so resize() isn't
             * swallowed by the `running()` guard; also re-run on the chart's own
             * 'resize' is unnecessary — Chart.js keeps it filled once the ratio
             * lock is removed and the wrapper has a definite size. */
            chartFix =
               '<script>\n' +
               'function _dawnFillCharts(){\n' +
               '  if(!window.Chart)return;var m=Chart.instances||{};\n' +
               '  Object.keys(m).forEach(function(k){var c=m[k];try{\n' +
               '    c.options.maintainAspectRatio=false;\n' +
               '    c.options.responsive=true;\n' +
               '    c.resize();\n' +
               '  }catch(e){}});\n' +
               '}\n' +
               /* First pass after layout; second after the default ~1s entrance
                * animation so any resize stashed by the running()-guard applies. */
               'requestAnimationFrame(function(){requestAnimationFrame(_dawnFillCharts)});\n' +
               'setTimeout(_dawnFillCharts,1100);\n' +
               '</' +
               'script>\n';
         }
         content =
            '<!DOCTYPE html><html><head><style>' +
            themeCSS +
            visualClasses +
            canvasCSS +
            '</style></head><body>\n' +
            bridgeScript +
            code +
            chartFix +
            '</body></html>';
         iframe.style.height = '400px';
      }

      /* Resolve vendor script tags: replace <script src="/js/vendor/X"> with
       * inline <script>...code...</script>. srcdoc iframes can't load external
       * scripts (no base URL), so we fetch once, cache, and inline.
       * Quote-agnostic (single OR double): the model reaches for single-quoted
       * HTML attributes to avoid escaping double-quotes inside render_visual's
       * nested-JSON `details`, so a "-only match would silently skip inlining and
       * leave a dead external <script>, blanking the chart.
       * Escape </script in vendor content to prevent premature tag close. */
      var vendorMatch = content.match(
         /<script\s+src=(["'])(\/js\/vendor\/[^"']+)\1\s*>\s*<\/script>/gi
      );
      if (vendorMatch && vendorMatch.length > 0) {
         var pendingFetches = [];
         vendorMatch.forEach(function (tag) {
            var srcMatch = tag.match(/src=(["'])([^"']+)\1/);
            if (!srcMatch) return;
            var src = srcMatch[2];
            if (vendorScriptCache[src]) {
               content = content.replace(
                  tag,
                  '<script>' + vendorScriptCache[src] + '</' + 'script>'
               );
            } else {
               pendingFetches.push(
                  fetch(src)
                     .then(function (r) {
                        return r.text();
                     })
                     .then(function (text) {
                        /* Escape </script in vendor code to prevent premature tag close */
                        var safe = text.replace(/<\/script/gi, '<\\/script');
                        vendorScriptCache[src] = safe;
                        content = content.replace(tag, '<script>' + safe + '</' + 'script>');
                     })
               );
            }
         });

         if (pendingFetches.length > 0) {
            /* Async path: wait for fetches, then set srcdoc */
            Promise.all(pendingFetches).then(function () {
               iframe.srcdoc = content;
            });
            container.appendChild(iframe);
         } else {
            /* Sync path: all scripts were cached */
            iframe.srcdoc = content;
            container.appendChild(iframe);
         }
      } else {
         iframe.srcdoc = content;
         container.appendChild(iframe);
      }

      /* Register iframe for sendPrompt validation and delegated resize handling */
      var frameRef = iframe;
      iframe.addEventListener('load', function () {
         if (frameRef.contentWindow) {
            knownVisualWindows.add(frameRef.contentWindow);
            iframeResizeMap.set(frameRef.contentWindow, { iframe: frameRef, lastHeight: 0 });
         }
      });

      return container;
   }

   /**
    * Extract <dawn-visual> blocks from raw text BEFORE markdown/sanitize runs.
    * Returns the cleaned text (without visual tags) and an array of visual blocks.
    *
    * @param {string} text - Raw message text that may contain <dawn-visual> tags
    * @returns {{ cleanText: string, visuals: Array<{title: string, type: string, code: string}> }}
    */
   function extractVisuals(text) {
      if (!text || text.indexOf('<dawn-visual') === -1) {
         return { cleanText: text, visuals: [] };
      }

      var visuals = [];
      var match;
      VISUAL_TAG_RE.lastIndex = 0;

      while ((match = VISUAL_TAG_RE.exec(text)) !== null) {
         visuals.push({
            title: match[1],
            type: match[2],
            code: match[3].trim(),
         });
      }

      var cleanText = text.replace(VISUAL_TAG_RE, '').trim();
      return { cleanText: cleanText, visuals: visuals };
   }

   /**
    * Render an array of extracted visual blocks into a text element as iframes.
    *
    * @param {HTMLElement} textEl - The .text element to append visuals to
    * @param {Array<{title: string, type: string, code: string}>} visuals - Extracted visual blocks
    */
   function renderVisuals(textEl, visuals) {
      if (!textEl || !visuals || visuals.length === 0) return;

      visuals.forEach(function (v) {
         var frame = createVisualFrame(v.title, v.type, v.code);
         textEl.appendChild(frame);
      });
   }

   /**
    * Initialize visual renderer.
    * Sets up the sendPrompt bridge listener for interactive diagram nodes.
    */
   function init() {
      /* Single delegated message handler for ALL visual iframes.
       * Handles both sendPrompt (dawn_prompt) and resize (dawn_visual_resize).
       * No per-iframe listeners — prevents unbounded listener accumulation. */
      window.addEventListener('message', function (event) {
         if (!event.data || !event.source) return;

         if (
            event.data.type === 'dawn_prompt' &&
            typeof event.data.text === 'string' &&
            knownVisualWindows.has(event.source)
         ) {
            if (DawnElements.textInput) {
               DawnElements.textInput.value = event.data.text;
               DawnElements.textInput.dispatchEvent(new Event('input'));
               var enterEvent = new KeyboardEvent('keydown', {
                  key: 'Enter',
                  code: 'Enter',
                  keyCode: 13,
                  which: 13,
                  bubbles: true,
               });
               DawnElements.textInput.dispatchEvent(enterEvent);
            }
         } else if (event.data.type === 'dawn_visual_resize') {
            var entry = iframeResizeMap.get(event.source);
            if (entry) {
               var h = event.data.height;
               if (h !== entry.lastHeight && h > 0) {
                  entry.lastHeight = h;
                  entry.iframe.style.height = h + 'px';
                  entry.iframe.style.maxHeight = '';
               }
            }
         }
      });

      /* Invalidate theme CSS cache on theme change */
      var observer = new MutationObserver(function () {
         cachedThemeCSS = null;
         cachedVisualClasses = null;
      });
      observer.observe(document.documentElement, {
         attributes: true,
         attributeFilter: ['class', 'data-theme'],
      });
   }

   /**
    * Show a progress placeholder while a visual is being generated.
    * Called when visual_progress_start arrives from the server.
    */
   function showVisualProgress(payload) {
      var sEntry = DawnState.streamingState.entryElement;
      if (!sEntry) return;

      // Finalize current .text div with markdown (same split as visual arrival)
      if (DawnState.streamingState.textElement && DawnState.streamingState.content) {
         DawnState.streamingState.textElement.innerHTML = DawnFormat.markdown(
            DawnState.streamingState.content
         );
      }

      // Create placeholder card
      var card = document.createElement('div');
      card.className = 'dawn-visual-progress';
      card.setAttribute('data-tool-id', payload.tool_id || '');
      card.dataset.startTime = Date.now();
      card.innerHTML =
         '<div class="dawn-visual-progress-title">Generating visual\u2026</div>' +
         '<div class="dawn-visual-progress-time">0s</div>';
      sEntry.appendChild(card);

      // Tick elapsed time every second
      card._progressTimer = setInterval(function () {
         var elapsed = Math.round((Date.now() - parseInt(card.dataset.startTime)) / 1000);
         var timeEl = card.querySelector('.dawn-visual-progress-time');
         if (timeEl) timeEl.textContent = elapsed + 's';
      }, 1000);

      // Create new .text div for post-progress streaming
      var newTextEl = document.createElement('div');
      newTextEl.className = 'text';
      sEntry.appendChild(newTextEl);

      DawnState.streamingState.preVisualContent =
         (DawnState.streamingState.preVisualContent || '') + DawnState.streamingState.content;
      DawnState.streamingState.content = '';
      DawnState.streamingState.textElement = newTextEl;

      DawnElements.transcript.scrollTop = DawnElements.transcript.scrollHeight;
   }

   global.DawnVisualRender = {
      init: init,
      extractVisuals: extractVisuals,
      renderVisuals: renderVisuals,
      createFrame: createVisualFrame,
      showProgress: showVisualProgress,
   };
})(window);
