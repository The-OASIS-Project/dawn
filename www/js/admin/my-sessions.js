/**
 * DAWN My Sessions Module
 * User session management (view/revoke active sessions)
 */
(function () {
   'use strict';

   /* =============================================================================
    * State
    * ============================================================================= */

   let callbacks = {
      getAuthState: null,
   };

   /* =============================================================================
    * API Requests
    * ============================================================================= */

   function requestListMySessions() {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({ type: 'list_my_sessions' });
      }
   }

   function requestRevokeSession(tokenPrefix) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'revoke_session',
            payload: { token_prefix: tokenPrefix },
         });
      }
   }

   /* =============================================================================
    * Response Handlers
    * ============================================================================= */

   function handleListMySessionsResponse(payload) {
      if (!payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(
               'Failed to load sessions: ' + (payload.error || 'Unknown error'),
               'error'
            );
         }
         return;
      }

      const list = document.getElementById('my-sessions-list');
      if (!list) return;

      const sessions = payload.sessions || [];
      const currentPrefix = payload.current_session || '';

      if (sessions.length === 0) {
         list.innerHTML = '<div class="no-sessions">No active sessions</div>';
         return;
      }

      list.innerHTML = sessions
         .map((session) => {
            const isCurrent = session.token_prefix === currentPrefix;
            const lastActivity = new Date(session.last_activity * 1000);
            const userAgent = parseUserAgent(session.user_agent);

            return `
        <div class="session-item${isCurrent ? ' current-session' : ''}">
          <div class="session-info">
            <div class="session-device">
              <span class="device-icon">${userAgent.icon}</span>
              <span class="device-name">${escapeHtml(userAgent.browser)} on ${escapeHtml(userAgent.os)}</span>
              ${isCurrent ? '<span class="current-badge">Current</span>' : ''}
            </div>
            <div class="session-details">
              <span class="session-ip">${escapeHtml(session.ip_address || 'Unknown IP')}</span>
              <span class="session-time">Last active: ${DawnFormat.relativeTime(lastActivity)}</span>
            </div>
          </div>
          ${
             !isCurrent
                ? `
            <button class="btn-icon btn-revoke" data-prefix="${escapeHtml(session.token_prefix)}"
                    title="Revoke this session">
              <svg viewBox="0 0 24 24" width="18" height="18" stroke="currentColor"
                   stroke-width="2" fill="none" stroke-linecap="round">
                <circle cx="12" cy="12" r="9" opacity="0.15" stroke-width="1.5"/>
                <path d="M15 9L9 15M9 9l6 6"/>
              </svg>
            </button>
          `
                : ''
          }
        </div>
      `;
         })
         .join('');

      // Attach revoke handlers
      list.querySelectorAll('.btn-revoke').forEach((btn) => {
         btn.addEventListener('click', async () => {
            const prefix = btn.dataset.prefix;
            if (
               await DawnDialog.confirm(
                  'Are you sure you want to revoke this session? The device will be logged out.',
                  { title: 'Revoke Session', okText: 'Revoke', danger: true }
               )
            ) {
               requestRevokeSession(prefix);
            }
         });
      });
   }

   function handleRevokeSessionResponse(payload) {
      if (payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Session revoked successfully', 'success');
         }
         setTimeout(requestListMySessions, 100);
      } else {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(
               'Failed to revoke session: ' + (payload.error || 'Unknown error'),
               'error'
            );
         }
      }
   }

   /* =============================================================================
    * Utilities
    * ============================================================================= */

   function escapeHtml(str) {
      if (typeof DawnFormat !== 'undefined' && DawnFormat.escapeHtml) {
         return DawnFormat.escapeHtml(str);
      }
      // Fallback
      const div = document.createElement('div');
      div.textContent = str;
      return div.innerHTML;
   }

   function parseUserAgent(ua) {
      if (!ua) return { browser: 'Unknown', os: 'Unknown', icon: '💻' };

      let browser = 'Unknown';
      let os = 'Unknown';
      let icon = '💻';

      // Detect browser
      if (ua.includes('Firefox')) {
         browser = 'Firefox';
      } else if (ua.includes('Edg/')) {
         browser = 'Edge';
      } else if (ua.includes('Chrome')) {
         browser = 'Chrome';
      } else if (ua.includes('Safari')) {
         browser = 'Safari';
      } else if (ua.includes('Opera') || ua.includes('OPR')) {
         browser = 'Opera';
      }

      // Detect OS
      if (ua.includes('Windows')) {
         os = 'Windows';
         icon = '🖥️';
      } else if (ua.includes('Mac OS')) {
         os = 'macOS';
         icon = '🍎';
      } else if (ua.includes('Linux')) {
         os = 'Linux';
         icon = '🐧';
      } else if (ua.includes('Android')) {
         os = 'Android';
         icon = '📱';
      } else if (ua.includes('iPhone') || ua.includes('iPad')) {
         os = 'iOS';
         icon = '📱';
      }

      return { browser, os, icon };
   }

   /* =============================================================================
    * Initialization
    * ============================================================================= */

   function init() {
      const section = document.getElementById('my-sessions-section');
      if (!section) return;

      const header = section.querySelector('.section-header');
      if (header) {
         header.addEventListener('click', () => {
            setTimeout(() => {
               const authState = callbacks.getAuthState ? callbacks.getAuthState() : {};
               if (!section.classList.contains('collapsed') && authState.authenticated) {
                  requestListMySessions();
               }
            }, 50);
         });
      }

      // Refresh button
      const refreshBtn = document.getElementById('refresh-sessions-btn');
      if (refreshBtn) {
         refreshBtn.addEventListener('click', requestListMySessions);
      }
   }

   /**
    * Set callbacks for shared utilities
    */
   function setCallbacks(cbs) {
      if (cbs.getAuthState) callbacks.getAuthState = cbs.getAuthState;
   }

   /* =============================================================================
    * Export
    * ============================================================================= */

   window.DawnMySessions = {
      init: init,
      setCallbacks: setCallbacks,
      requestList: requestListMySessions,
      handleListResponse: handleListMySessionsResponse,
      handleRevokeResponse: handleRevokeSessionResponse,
   };
})();
