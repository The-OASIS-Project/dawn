/**
 * DAWN Metrics Panel Module
 * Server-side metrics dashboard (sessions, queries, tokens, latency)
 */
(function () {
   'use strict';

   /* =============================================================================
    * State
    * ============================================================================= */

   let metricsInterval = null;
   let metricsVisible = false;
   let metricsEscToken = null; // DawnEscStack registration while the panel is open

   /* =============================================================================
    * Panel Control
    * ============================================================================= */

   function toggleMetricsPanel() {
      const panel = document.getElementById('metrics-panel');
      if (panel.classList.contains('hidden')) {
         showMetricsPanel();
      } else {
         hideMetricsPanel();
      }
   }

   function showMetricsPanel() {
      const panel = document.getElementById('metrics-panel');
      panel.classList.remove('hidden');
      // Deferred past the click that opened it: a popover that click closes
      // takes focus back to its own button on the way out.
      const closeBtn = document.getElementById('metrics-close');
      setTimeout(() => {
         if (closeBtn && !panel.classList.contains('hidden')) closeBtn.focus();
      }, 0);
      // The panel stays open as a monitor while the user works elsewhere, so
      // Escape closes it only when focus is inside it; otherwise the key goes
      // on to whatever is under it.
      if (metricsEscToken === null && typeof DawnEscStack !== 'undefined') {
         metricsEscToken = DawnEscStack.register(() => {
            if (!panel.contains(document.activeElement)) return false;
            hideMetricsPanel();
            return true;
         });
      }
      metricsVisible = true;
      requestMetrics();
      // Refresh every 2 seconds while visible
      metricsInterval = setInterval(requestMetrics, 2000);
   }

   function hideMetricsPanel() {
      const panel = document.getElementById('metrics-panel');
      // The closed panel is out of the tab order: focus inside it goes back to
      // the menu that opens it.
      const hadFocus = panel.contains(document.activeElement);
      panel.classList.add('hidden');
      if (hadFocus) {
         const opener = document.getElementById('user-badge');
         if (opener) opener.focus();
      }
      metricsVisible = false;
      if (metricsEscToken !== null) {
         DawnEscStack.unregister(metricsEscToken);
         metricsEscToken = null;
      }
      if (metricsInterval) {
         clearInterval(metricsInterval);
         metricsInterval = null;
      }
   }

   function requestMetrics() {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({ type: 'get_metrics' });
      }
   }

   /* =============================================================================
    * Response Handler
    * ============================================================================= */

   function handleMetricsResponse(payload) {
      if (!metricsVisible) return;

      // Session stats
      const uptime = payload.session?.uptime_seconds || 0;
      const hours = Math.floor(uptime / 3600);
      const mins = Math.floor((uptime % 3600) / 60);
      const uptimeStr = hours > 0 ? `${hours}h ${mins}m` : `${mins}m`;
      setText('m-uptime', uptimeStr);

      const queries = payload.session?.queries_total || 0;
      const cloud = payload.session?.queries_cloud || 0;
      const local = payload.session?.queries_local || 0;
      setText('m-queries', `${queries} (${cloud}c/${local}l)`);
      setText('m-errors', payload.session?.errors || 0);
      setText('m-bargeins', payload.session?.bargeins || 0);

      // Tokens
      const tc = payload.tokens || {};
      setText('m-tokens-cloud', `${formatNum(tc.cloud_input)}/${formatNum(tc.cloud_output)}`);
      setText('m-tokens-local', `${formatNum(tc.local_input)}/${formatNum(tc.local_output)}`);
      setText('m-tokens-cached', formatNum(tc.cached));

      // Last pipeline
      const last = payload.last || {};
      setText('m-last-asr', formatMs(last.asr_ms));
      setText('m-last-llm', formatMs(last.llm_total_ms));
      setText('m-last-tts', formatMs(last.tts_ms));

      // Averages
      const avg = payload.averages || {};
      setText('m-avg-asr', formatMs(avg.asr_ms));
      setText('m-avg-llm', formatMs(avg.llm_total_ms));
      setText('m-avg-tts', formatMs(avg.tts_ms));

      // System
      const vad = payload.state?.vad_probability || 0;
      setText('m-vad', `${(vad * 100).toFixed(0)}%`);

      const aec = payload.aec || {};
      let aecStr = aec.enabled ? (aec.calibrated ? `${aec.delay_ms}ms` : 'uncal') : 'off';
      setText('m-aec', aecStr);
   }

   /* =============================================================================
    * Utilities
    * ============================================================================= */

   function setText(id, value) {
      const el = document.getElementById(id);
      if (el) el.textContent = value;
   }

   function formatMs(ms) {
      if (ms === undefined || ms === null || ms === 0) return '--';
      if (ms < 1000) return `${Math.round(ms)}ms`;
      return `${(ms / 1000).toFixed(1)}s`;
   }

   function formatNum(n) {
      if (n === undefined || n === null) return '--';
      if (n >= 1000000) return `${(n / 1000000).toFixed(1)}M`;
      if (n >= 1000) return `${(n / 1000).toFixed(1)}K`;
      return String(n);
   }

   /* =============================================================================
    * Initialization
    * ============================================================================= */

   function init() {
      const metricsClose = document.getElementById('metrics-close');
      if (metricsClose) {
         metricsClose.addEventListener('click', hideMetricsPanel);
      }
   }

   /**
    * Check if metrics panel is currently visible
    */
   function isVisible() {
      return metricsVisible;
   }

   /* =============================================================================
    * Export
    * ============================================================================= */

   window.DawnMetricsPanel = {
      init: init,
      toggle: toggleMetricsPanel,
      show: showMetricsPanel,
      hide: hideMetricsPanel,
      handleResponse: handleMetricsResponse,
      isVisible: isVisible,
   };
})();
