/**
 * DAWN Metrics Module
 * Handles LLM metrics updates and telemetry panel display
 *
 * Usage:
 *   DawnMetrics.handleUpdate(payload)    // Handle metrics_update message
 *   DawnMetrics.updatePanel()            // Refresh telemetry panel display
 *   DawnMetrics.resetAverages()          // Reset session averages
 */
(function (global) {
   'use strict';

   // =============================================================================
   // Session Averages State
   // =============================================================================

   let avgState = {
      tokenRate: { sum: 0, count: 0 },
      ttft: { sum: 0, count: 0 },
      latency: { sum: 0, count: 0 },
   };

   // =============================================================================
   // Metrics Update Handler
   // =============================================================================

   /**
    * Handle metrics_update message from server
    * Updates global metricsState and drives multi-ring visualization + telemetry panel
    * @param {Object} payload - Metrics payload from server
    */
   function handleMetricsUpdate(payload) {
      DawnState.metricsState.state = payload.state || 'idle';
      DawnState.metricsState.ttft_ms = payload.ttft_ms || 0;
      DawnState.metricsState.token_rate = payload.token_rate || 0;
      // Only update context_percent if value is valid (>= 0), otherwise keep previous
      if (payload.context_percent >= 0) {
         DawnState.metricsState.context_percent = payload.context_percent;
      }
      DawnState.metricsState.lastUpdate = performance.now();

      // Preserve last non-zero values for display after streaming ends
      // Also track session averages
      if (DawnState.metricsState.ttft_ms > 0) {
         DawnState.metricsState.last_ttft_ms = DawnState.metricsState.ttft_ms;
         avgState.ttft.sum += DawnState.metricsState.ttft_ms;
         avgState.ttft.count++;
      }
      if (DawnState.metricsState.token_rate > 0) {
         DawnState.metricsState.last_token_rate = DawnState.metricsState.token_rate;
         avgState.tokenRate.sum += DawnState.metricsState.token_rate;
         avgState.tokenRate.count++;
      }

      // Update throughput ring based on token rate
      // Normalize: 0 tokens/sec = 0%, 80 tokens/sec = 100%
      const throughputFill = Math.min(DawnState.metricsState.token_rate / 80, 1.0);
      if (typeof DawnVisualization !== 'undefined') {
         DawnVisualization.renderThroughputRing(throughputFill);
      }

      // Update telemetry panel (right side discrete readouts)
      updateTelemetryPanel();

      // Debug log (only when debug mode is enabled)
      if (DawnState.getDebugMode()) {
         console.log('Metrics update:', DawnState.metricsState);
      }
   }

   // =============================================================================
   // Telemetry Panel
   // =============================================================================

   /**
    * Apply color class to metric value based on thresholds
    * @param {Element} el - DOM element
    * @param {number} value - Current value
    * @param {number} normalMax - Max value for normal (cyan)
    * @param {number} elevatedMax - Max value for elevated (amber), above = extreme (red)
    */
   function applyMetricColor(el, value, normalMax, elevatedMax) {
      el.classList.remove('metric-normal', 'metric-elevated', 'metric-extreme');
      if (value <= 0) return; // No color for placeholder values

      if (value <= normalMax) {
         el.classList.add('metric-normal');
      } else if (value <= elevatedMax) {
         el.classList.add('metric-elevated');
      } else {
         el.classList.add('metric-extreme');
      }
   }

   /**
    * Update the telemetry panel with current metrics
    * Per design: discrete numeric readouts, visually quieter than rings
    * Color coding: cyan (normal) -> amber (elevated) -> red (extreme)
    */
   function updateTelemetryPanel() {
      const panel = document.getElementById('telemetry-panel');
      const tokenRate = document.getElementById('telem-token-rate');
      const tokenRateAvg = document.getElementById('telem-token-rate-avg');
      const ttft = document.getElementById('telem-ttft');
      const ttftAvg = document.getElementById('telem-ttft-avg');
      const latency = document.getElementById('telem-latency');
      const latencyAvg = document.getElementById('telem-latency-avg');

      if (!panel) return;

      // Update token rate with color coding
      // Normal: >30 tok/s, Elevated: 15-30, Extreme: <15
      if (tokenRate) {
         const rate = DawnState.metricsState.token_rate || DawnState.metricsState.last_token_rate;
         tokenRate.innerHTML =
            rate > 0 ? `${rate.toFixed(1)} <small>tok/s</small>` : '-- <small>tok/s</small>';
         // Invert thresholds - higher is better for token rate
         tokenRate.classList.remove('metric-normal', 'metric-elevated', 'metric-extreme');
         if (rate > 0) {
            if (rate >= 30) {
               tokenRate.classList.add('metric-normal');
            } else if (rate >= 15) {
               tokenRate.classList.add('metric-elevated');
            } else {
               tokenRate.classList.add('metric-extreme');
            }
         }
      }
      // Update token rate average
      if (tokenRateAvg && avgState.tokenRate.count > 0) {
         const avg = avgState.tokenRate.sum / avgState.tokenRate.count;
         tokenRateAvg.textContent = `avg ${avg.toFixed(1)}`;
      }

      // Update TTFT with color coding
      // Normal: <500ms, Elevated: 500-1500ms, Extreme: >1500ms
      if (ttft) {
         const ms = DawnState.metricsState.ttft_ms || DawnState.metricsState.last_ttft_ms;
         ttft.innerHTML = ms > 0 ? `${ms} <small>ms</small>` : '-- <small>ms</small>';
         applyMetricColor(ttft, ms, 500, 1500);
      }
      // Update TTFT average
      if (ttftAvg && avgState.ttft.count > 0) {
         const avg = avgState.ttft.sum / avgState.ttft.count;
         ttftAvg.textContent = `avg ${Math.round(avg)}`;
      }

      // Update latency variance with color coding
      // Show as milliseconds variance (stddev of inter-token intervals)
      // Normal: <10ms, Elevated: 10-25ms, Extreme: >25ms
      if (latency) {
         const load = DawnState.hesitationState.loadSmooth;
         if (load > 0.005) {
            // load 0-1 maps to ~3-40ms stddev range
            const msVar = Math.round(3 + load * 37);
            latency.innerHTML = `${msVar} <small>ms var</small>`;
            applyMetricColor(latency, msVar, 10, 25);
            // Track latency average
            avgState.latency.sum += msVar;
            avgState.latency.count++;
         } else {
            latency.innerHTML = '-- <small>ms var</small>';
            latency.classList.remove('metric-normal', 'metric-elevated', 'metric-extreme');
         }
      }
      // Update latency average
      if (latencyAvg && avgState.latency.count > 0) {
         const avg = avgState.latency.sum / avgState.latency.count;
         latencyAvg.textContent = `avg ${Math.round(avg)}`;
      }

      // Update thinking tokens (from reasoning models)
      // Show row when reasoning mode is enabled OR we have tokens to display
      const thinkingTokens = document.getElementById('telem-thinking');
      const thinkingRow = document.getElementById('telem-thinking-row');
      if (thinkingTokens && thinkingRow) {
         const liveTokens = DawnState.streamingState.reasoningTokens || 0;
         if (liveTokens > 0) {
            DawnState.metricsState.last_thinking_tokens = liveTokens;
         }
         const displayTokens = liveTokens || DawnState.metricsState.last_thinking_tokens;

         // Show row if: reasoning mode enabled OR we have tokens to show
         const reasoningSelect = document.getElementById('reasoning-mode-select');
         const reasoningEnabled = reasoningSelect && reasoningSelect.value !== 'disabled';
         const shouldShow = reasoningEnabled || displayTokens > 0;

         if (shouldShow) {
            if (displayTokens > 0) {
               thinkingTokens.innerHTML = `${displayTokens.toLocaleString()} <small>tokens</small>`;
            } else {
               thinkingTokens.innerHTML = `-- <small>tokens</small>`;
            }
            thinkingRow.classList.remove('hidden');
         } else {
            thinkingRow.classList.add('hidden');
         }
      }

      // Dim ring container when idle - not the telemetry panel
      // Keep bright while speaking (audio still playing) even if server sent idle
      if (DawnElements.ringContainer) {
         const isIdle = DawnState.metricsState.state === 'idle';
         const isPlaying =
            typeof DawnAudioPlayback !== 'undefined' && DawnAudioPlayback.isPlaying();
         if (isIdle && !isPlaying) {
            DawnElements.ringContainer.classList.add('idle');
         } else {
            DawnElements.ringContainer.classList.remove('idle');
         }
      }
   }

   /**
    * Reset session averages and displayed metrics for new conversation
    */
   function resetAverages() {
      avgState = {
         tokenRate: { sum: 0, count: 0 },
         ttft: { sum: 0, count: 0 },
         latency: { sum: 0, count: 0 },
      };

      // Also reset the "last" values so they don't persist across conversations
      if (typeof DawnState !== 'undefined' && DawnState.metricsState) {
         DawnState.metricsState.last_ttft_ms = 0;
         DawnState.metricsState.last_token_rate = 0;
         DawnState.metricsState.last_thinking_tokens = 0;
      }

      // Refresh the panel to show cleared values
      updateTelemetryPanel();
   }

   // =============================================================================
   // Export Module
   // =============================================================================

   global.DawnMetrics = {
      handleUpdate: handleMetricsUpdate,
      updatePanel: updateTelemetryPanel,
      resetAverages: resetAverages,
   };
})(window);
