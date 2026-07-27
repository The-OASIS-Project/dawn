/**
 * DAWN Memory Panel Module
 * Manages memory viewing, searching, and deletion
 */
(function () {
   'use strict';

   /* =============================================================================
    * Constants
    * ============================================================================= */

   const PAGE_SIZE = 20;

   /* =============================================================================
    * State
    * ============================================================================= */

   let memoryState = {
      stats: null,
      facts: [],
      preferences: [],
      allPreferences: [], // Keep unfiltered copy for client-side search
      summaries: [],
      entities: [],
      allEntities: [], // Keep unfiltered copy for client-side search
      activeTab: 'facts',
      searchQuery: '',
      searchTimeout: null,
      tabOffset: { facts: 0, preferences: 0, summaries: 0, entities: 0, contacts: 0 },
      tabHasMore: {
         facts: false,
         preferences: false,
         summaries: false,
         entities: false,
         contacts: false,
      },
      loading: false,
   };

   // Callbacks for shared utilities
   let callbacks = {
      getAuthState: null,
      trapFocus: null,
   };

   // Focus management state
   let focusTrapCleanup = null;
   let triggerElement = null;
   let memoryEscToken = null; // DawnEscStack registration while the panel is open

   /* Phase 2 entity-merge: one-shot flag so auto-route-to-Graph fires
    * on the FIRST open per page-load when proposals are pending, then
    * leaves subsequent opens alone.  Reset on full page reload (module
    * IIFE re-initialises). */
   let autoRoutedThisLoad = false;

   /* =============================================================================
    * Elements
    * ============================================================================= */

   const memoryElements = {
      btn: null,
      popover: null,
      closeBtn: null,
      searchInput: null,
      list: null,
      forgetAllBtn: null,
      exportBtn: null,
      importBtn: null,
      loadMoreBtn: null,
      tabs: null,
      statFacts: null,
      statPrefs: null,
      statSummaries: null,
      statEntities: null,
      statContacts: null,
   };

   /* Import/Export surface (modals + their private state) lives in
    * www/js/ui/memory_import.js — see DawnMemoryImport.  The
    * DawnMemory.handleExportResponse / handleImportResponse thin-
    * forwarders below preserve the surface dawn.js dispatches against. */

   /* =============================================================================
    * API Requests
    * ============================================================================= */

   function requestStats() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({ type: 'get_memory_stats' });
   }

   function requestFacts(offset) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      memoryState.loading = true;
      DawnWS.send({
         type: 'list_memory_facts',
         payload: { limit: PAGE_SIZE, offset: offset || 0 },
      });
   }

   function requestPreferences(offset) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      memoryState.loading = true;
      DawnWS.send({
         type: 'list_memory_preferences',
         payload: { limit: PAGE_SIZE, offset: offset || 0 },
      });
   }

   function requestSummaries(offset) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      memoryState.loading = true;
      DawnWS.send({
         type: 'list_memory_summaries',
         payload: { limit: PAGE_SIZE, offset: offset || 0 },
      });
   }

   function requestEntities(offset) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      memoryState.loading = true;
      DawnWS.send({
         type: 'list_memory_entities',
         payload: { limit: PAGE_SIZE, offset: offset || 0 },
      });
   }

   function requestDeleteEntity(entityId) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'delete_memory_entity',
         payload: { entity_id: entityId },
      });
   }

   /* Phase 1 entity-merge alias + proposal request senders live in
    * www/js/ui/memory_aliases.js — see DawnMemoryAliases.{requestProposalList,
    * tryHandleClick} for the public surface. */

   function requestSearch(query) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      if (!query || query.trim().length === 0) return;
      memoryState.loading = true;
      DawnWS.send({
         type: 'search_memory',
         payload: { query: query.trim() },
      });
   }

   function requestDeleteFact(factId) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'delete_memory_fact',
         payload: { fact_id: factId },
      });
   }

   function requestDeletePreference(category) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'delete_memory_preference',
         payload: { category: category },
      });
   }

   function requestDeleteSummary(summaryId) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'delete_memory_summary',
         payload: { summary_id: summaryId },
      });
   }

   function requestDeleteAll() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'delete_all_memories',
         payload: { confirm: 'DELETE' },
      });
   }

   /* =============================================================================
    * Response Handlers
    * ============================================================================= */

   function handleStatsResponse(payload) {
      if (!payload.success) {
         console.error('Failed to get memory stats:', payload.error);
         return;
      }

      memoryState.stats = payload;
      updateStatsDisplay();
   }

   function handleEntitiesResponse(payload) {
      memoryState.loading = false;

      if (!payload.success) {
         console.error('Failed to list memory entities:', payload.error);
         showEmptyState('Failed to load entities');
         return;
      }

      const newEntities = payload.entities || [];
      memoryState.tabHasMore.entities = payload.has_more || false;

      if (memoryState.tabOffset.entities > 0 && newEntities.length > 0) {
         memoryState.entities = memoryState.entities.concat(newEntities);
         memoryState.allEntities = memoryState.allEntities.concat(newEntities);
         if (memoryState.activeTab === 'entities' && !memoryState.searchQuery) {
            appendEntitiesToList(newEntities);
         }
      } else {
         memoryState.entities = newEntities;
         memoryState.allEntities = newEntities;
         if (memoryState.activeTab === 'entities' && !memoryState.searchQuery) {
            renderEntitiesList();
         }
      }
   }

   function handleDeleteEntityResponse(payload) {
      if (!payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.error || 'Failed to delete entity', 'error');
         }
         return;
      }

      if (typeof DawnToast !== 'undefined') {
         DawnToast.show('Entity deleted', 'success');
      }

      requestStats();
      memoryState.tabOffset.entities = 0;
      requestEntities(0);
   }

   /* Phase 1 entity-merge response handlers — thin forwarders that preserve
    * the DawnMemory.handle*Response surface dawn.js dispatches against, while
    * the real work lives in www/js/ui/memory_aliases.js. */
   function handleMergeEntityResponse(payload) {
      if (window.DawnMemoryAliases) DawnMemoryAliases.handleMergeResponse(payload);
   }
   function handleEntityAliasesResponse(payload) {
      if (window.DawnMemoryAliases) DawnMemoryAliases.handleAliasesResponse(payload);
   }
   function handleEntityMergeProposalListResponse(payload) {
      if (window.DawnMemoryAliases) DawnMemoryAliases.handleProposalListResponse(payload);
   }
   function handleEntityLinkResponse(payload) {
      if (window.DawnMemoryAliases) DawnMemoryAliases.handleLinkResponse(payload);
   }
   function handleEntityUnlinkResponse(payload) {
      if (window.DawnMemoryAliases) DawnMemoryAliases.handleUnlinkResponse(payload);
   }
   function handleEntityProposalResolveResponse(payload) {
      if (window.DawnMemoryAliases) DawnMemoryAliases.handleProposalResolveResponse(payload);
   }

   /* Import/Export response handlers — thin forwarders that preserve the
    * DawnMemory.handle*Response surface dawn.js dispatches against, while
    * the real work lives in www/js/ui/memory_import.js. */
   function handleExportResponse(payload) {
      if (window.DawnMemoryImport) DawnMemoryImport.handleExportResponse(payload);
   }
   function handleImportResponse(payload) {
      if (window.DawnMemoryImport) DawnMemoryImport.handleImportResponse(payload);
   }

   function handleFactsResponse(payload) {
      memoryState.loading = false;

      if (!payload.success) {
         console.error('Failed to list memory facts:', payload.error);
         showEmptyState('Failed to load facts');
         return;
      }

      const newFacts = payload.facts || [];
      memoryState.tabHasMore.facts = payload.has_more || false;

      // If offset > 0, append to DOM efficiently; otherwise full render
      if (memoryState.tabOffset.facts > 0 && newFacts.length > 0) {
         memoryState.facts = memoryState.facts.concat(newFacts);
         if (memoryState.activeTab === 'facts' && !memoryState.searchQuery) {
            appendFactsToList(newFacts);
         }
      } else {
         memoryState.facts = newFacts;
         if (memoryState.activeTab === 'facts' && !memoryState.searchQuery) {
            renderFactsList();
         }
      }
   }

   /**
    * Append new facts to the list without rebuilding entire DOM
    */
   function appendFactsToList(newFacts) {
      if (!memoryElements.list || newFacts.length === 0) return;

      const html = newFacts.map((fact) => renderFactItem(fact)).join('');
      memoryElements.list.insertAdjacentHTML('beforeend', html);

      // Update load more button state
      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.disabled =
            !memoryState.tabHasMore.facts || !!memoryState.searchQuery;
      }
   }

   function appendPrefsToList(newPrefs) {
      if (!memoryElements.list || newPrefs.length === 0) return;

      const html = newPrefs.map((pref) => renderPreferenceItem(pref)).join('');
      memoryElements.list.insertAdjacentHTML('beforeend', html);

      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.disabled =
            !memoryState.tabHasMore.preferences || !!memoryState.searchQuery;
      }
   }

   function appendSummariesToList(newSummaries) {
      if (!memoryElements.list || newSummaries.length === 0) return;

      const html = newSummaries.map((summary) => renderSummaryItem(summary)).join('');
      memoryElements.list.insertAdjacentHTML('beforeend', html);

      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.disabled =
            !memoryState.tabHasMore.summaries || !!memoryState.searchQuery;
      }
   }

   function appendEntitiesToList(newEntities) {
      if (!memoryElements.list || newEntities.length === 0) return;

      const html = newEntities.map((entity) => renderEntityItem(entity)).join('');
      memoryElements.list.insertAdjacentHTML('beforeend', html);

      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.disabled =
            !memoryState.tabHasMore.entities || !!memoryState.searchQuery;
      }
   }

   function handlePreferencesResponse(payload) {
      memoryState.loading = false;

      if (!payload.success) {
         console.error('Failed to list memory preferences:', payload.error);
         showEmptyState('Failed to load preferences');
         return;
      }

      const newPrefs = payload.preferences || [];
      memoryState.tabHasMore.preferences = payload.has_more || false;

      if (memoryState.tabOffset.preferences > 0 && newPrefs.length > 0) {
         memoryState.preferences = memoryState.preferences.concat(newPrefs);
         memoryState.allPreferences = memoryState.allPreferences.concat(newPrefs);
         if (memoryState.activeTab === 'preferences' && !memoryState.searchQuery) {
            appendPrefsToList(newPrefs);
         }
      } else {
         memoryState.preferences = newPrefs;
         memoryState.allPreferences = newPrefs;
         if (memoryState.activeTab === 'preferences' && !memoryState.searchQuery) {
            renderPreferencesList();
         }
      }
   }

   function handleSummariesResponse(payload) {
      memoryState.loading = false;

      if (!payload.success) {
         console.error('Failed to list memory summaries:', payload.error);
         showEmptyState('Failed to load summaries');
         return;
      }

      const newSummaries = payload.summaries || [];
      memoryState.tabHasMore.summaries = payload.has_more || false;

      if (memoryState.tabOffset.summaries > 0 && newSummaries.length > 0) {
         memoryState.summaries = memoryState.summaries.concat(newSummaries);
         if (memoryState.activeTab === 'summaries' && !memoryState.searchQuery) {
            appendSummariesToList(newSummaries);
         }
      } else {
         memoryState.summaries = newSummaries;
         if (memoryState.activeTab === 'summaries' && !memoryState.searchQuery) {
            renderSummariesList();
         }
      }
   }

   function handleSearchResponse(payload) {
      memoryState.loading = false;

      if (!payload.success) {
         console.error('Failed to search memory:', payload.error);
         showEmptyState('Search failed');
         return;
      }

      // All four record types now searched server-side, so we get full-DB
      // hits regardless of how much the user has paginated through.  The
      // older client-side filter on `allPreferences` / `allEntities` only
      // saw the loaded-page subset and silently missed past-page matches.
      memoryState.facts = payload.facts || [];
      memoryState.summaries = payload.summaries || [];
      memoryState.preferences = payload.preferences || [];
      memoryState.entities = payload.entities || [];

      // Render search results based on active tab
      if (memoryState.activeTab === 'facts') {
         renderFactsList();
      } else if (memoryState.activeTab === 'preferences') {
         renderPreferencesList();
      } else if (memoryState.activeTab === 'summaries') {
         renderSummariesList();
      } else if (memoryState.activeTab === 'entities') {
         renderEntitiesList();
      }

      // Disable load more button during search
      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.disabled = true;
      }
   }

   function handleDeleteFactResponse(payload) {
      if (!payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.error || 'Failed to delete fact', 'error');
         }
         return;
      }

      if (typeof DawnToast !== 'undefined') {
         DawnToast.show('Fact deleted', 'success');
      }

      // Refresh data
      requestStats();
      memoryState.tabOffset.facts = 0;
      requestFacts(0);
   }

   function handleDeletePreferenceResponse(payload) {
      if (!payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.error || 'Failed to delete preference', 'error');
         }
         return;
      }

      if (typeof DawnToast !== 'undefined') {
         DawnToast.show('Preference deleted', 'success');
      }

      // Refresh data
      requestStats();
      memoryState.tabOffset.preferences = 0;
      requestPreferences(0);
   }

   function handleDeleteSummaryResponse(payload) {
      if (!payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.error || 'Failed to delete summary', 'error');
         }
         return;
      }

      if (typeof DawnToast !== 'undefined') {
         DawnToast.show('Summary deleted', 'success');
      }

      // Refresh data
      requestStats();
      memoryState.tabOffset.summaries = 0;
      requestSummaries(0);
   }

   function handleDeleteAllResponse(payload) {
      if (!payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.error || 'Failed to delete memories', 'error');
         }
         return;
      }

      if (typeof DawnToast !== 'undefined') {
         DawnToast.show('All memories deleted', 'success');
      }

      // Clear local state and refresh
      memoryState.facts = [];
      memoryState.preferences = [];
      memoryState.summaries = [];
      memoryState.entities = [];
      memoryState.allEntities = [];
      memoryState.tabOffset = { facts: 0, preferences: 0, summaries: 0, entities: 0, contacts: 0 };
      memoryState.tabHasMore = {
         facts: false,
         preferences: false,
         summaries: false,
         entities: false,
         contacts: false,
      };
      requestStats();
      showEmptyState('No memories yet');
   }

   /* =============================================================================
    * UI Update Functions
    * ============================================================================= */

   function updateStatsDisplay() {
      if (!memoryState.stats) return;

      if (memoryElements.statFacts) {
         memoryElements.statFacts.textContent = memoryState.stats.fact_count || 0;
      }
      if (memoryElements.statPrefs) {
         memoryElements.statPrefs.textContent = memoryState.stats.pref_count || 0;
      }
      if (memoryElements.statSummaries) {
         memoryElements.statSummaries.textContent = memoryState.stats.summary_count || 0;
      }
      if (memoryElements.statEntities) {
         memoryElements.statEntities.textContent = memoryState.stats.entity_count || 0;
      }
      if (memoryElements.statContacts) {
         memoryElements.statContacts.textContent = memoryState.stats.contact_count || 0;
      }
   }

   function showEmptyState(message) {
      if (!memoryElements.list) return;
      memoryElements.list.innerHTML = `<div class="memory-empty">${escapeHtml(message)}</div>`;
      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.disabled = true;
      }
   }

   function showLoading() {
      if (!memoryElements.list) return;
      memoryElements.list.innerHTML = '<div class="memory-loading">Loading...</div>';
   }

   function renderFactsList() {
      if (!memoryElements.list) return;

      if (memoryState.facts.length === 0) {
         showEmptyState(memoryState.searchQuery ? 'No facts found' : 'No facts stored yet');
         return;
      }

      const html = memoryState.facts.map((fact) => renderFactItem(fact)).join('');
      memoryElements.list.innerHTML = html;

      // Update load more button - enable if more available
      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.classList.remove('hidden');
         memoryElements.loadMoreBtn.disabled =
            !memoryState.tabHasMore.facts || !!memoryState.searchQuery;
      }
   }

   function renderFactItem(fact) {
      const confidence = fact.confidence || 0;
      let confidenceClass = 'confidence-low';
      if (confidence >= 0.8) confidenceClass = 'confidence-high';
      else if (confidence >= 0.5) confidenceClass = 'confidence-medium';

      const dateStr = formatDate(fact.created_at);
      const confidencePercent = Math.round(confidence * 100);

      // Show "show source" button only when provenance is recorded (v40+)
      const hasSource = !!fact.source_conversation_id;
      const sourceBtn = hasSource
         ? `<button class="memory-item-source-btn" data-fact-id="${fact.id}"
               title="Show source conversation" aria-label="Show source">
               <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                  <path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/>
               </svg>
            </button>`
         : '';

      return `
         <div class="memory-item fact" data-fact-id="${fact.id}">
            <div class="memory-item-header">
               <div class="memory-item-text">${escapeHtml(fact.fact_text)}</div>
               <div class="memory-item-actions">
                  ${sourceBtn}
                  <button class="memory-item-delete" data-fact-id="${fact.id}" title="Delete this fact" aria-label="Delete fact">
                     <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                        <path d="M3 6h18M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/>
                     </svg>
                  </button>
               </div>
            </div>
            <div class="memory-item-meta">
               <span class="memory-item-confidence ${confidenceClass}">${confidencePercent}%</span>
               <span class="memory-item-source">${escapeHtml(fact.source || 'unknown')}</span>
               <span class="memory-item-date">${dateStr}</span>
            </div>
         </div>
      `;
   }

   function handleFactSourceResponse(payload) {
      const modal = document.getElementById('memory-source-modal');
      const body = document.getElementById('memory-source-body');
      if (!modal || !body) return;

      if (!payload.success) {
         body.innerHTML = '<p class="memory-source-unavailable">Source no longer available.</p>';
      } else {
         const msgs = payload.messages || [];
         if (msgs.length === 0) {
            body.innerHTML =
               '<p class="memory-source-unavailable">No messages in source range.</p>';
         } else {
            body.innerHTML = msgs
               .map(
                  (m) => `
               <div class="memory-source-message memory-source-${escapeHtml(m.role)}">
                  <span class="memory-source-role">${escapeHtml(m.role)}</span>
                  <span class="memory-source-content">${escapeHtml(m.content || '')}</span>
               </div>
            `
               )
               .join('');
         }
      }
      modal.classList.remove('hidden');
   }

   let sourceModalTrigger = null;
   let sourceEscToken = null; // DawnEscStack registration while the source modal is open

   function openSourceModal(factId) {
      const modal = document.getElementById('memory-source-modal');
      const body = document.getElementById('memory-source-body');
      if (!modal || !body) return;
      sourceModalTrigger = document.activeElement;
      body.innerHTML = '<p class="memory-source-loading">Loading…</p>';
      modal.classList.remove('hidden');
      if (sourceEscToken === null) {
         sourceEscToken = DawnEscStack.register(() => {
            closeSourceModal();
            return true;
         });
      }
      const closeBtn = document.getElementById('memory-source-close');
      if (closeBtn) closeBtn.focus();
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({ type: 'get_memory_fact_source', payload: { fact_id: factId } });
      }
   }

   function closeSourceModal() {
      const modal = document.getElementById('memory-source-modal');
      if (modal) modal.classList.add('hidden');
      if (sourceEscToken !== null) {
         DawnEscStack.unregister(sourceEscToken);
         sourceEscToken = null;
      }
      if (sourceModalTrigger && typeof sourceModalTrigger.focus === 'function') {
         sourceModalTrigger.focus();
         sourceModalTrigger = null;
      }
   }

   function renderPreferencesList() {
      if (!memoryElements.list) return;

      if (memoryState.preferences.length === 0) {
         showEmptyState('No preferences stored yet');
         return;
      }

      const html = memoryState.preferences.map((pref) => renderPreferenceItem(pref)).join('');
      memoryElements.list.innerHTML = html;

      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.classList.remove('hidden');
         memoryElements.loadMoreBtn.disabled =
            !memoryState.tabHasMore.preferences || !!memoryState.searchQuery;
      }
   }

   function renderPreferenceItem(pref) {
      const confidence = pref.confidence || 0;
      let confidenceClass = 'confidence-low';
      if (confidence >= 0.8) confidenceClass = 'confidence-high';
      else if (confidence >= 0.5) confidenceClass = 'confidence-medium';

      const dateStr = formatDate(pref.updated_at || pref.created_at);
      const confidencePercent = Math.round(confidence * 100);

      return `
         <div class="memory-item preference" data-pref-category="${escapeHtml(pref.category)}">
            <div class="memory-item-header">
               <div class="memory-item-category">${escapeHtml(pref.category)}</div>
               <button class="memory-item-delete" data-pref-category="${escapeHtml(pref.category)}" title="Delete this preference" aria-label="Delete preference">
                  <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                     <path d="M3 6h18M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/>
                  </svg>
               </button>
            </div>
            <div class="memory-item-value">${escapeHtml(pref.value)}</div>
            <div class="memory-item-meta">
               <span class="memory-item-confidence ${confidenceClass}">${confidencePercent}%</span>
               <span class="memory-item-source">${escapeHtml(pref.source || 'unknown')}</span>
               <span class="memory-item-date">${dateStr}</span>
               ${pref.reinforcement_count > 1 ? `<span>${pref.reinforcement_count}x</span>` : ''}
            </div>
         </div>
      `;
   }

   function renderSummariesList() {
      if (!memoryElements.list) return;

      if (memoryState.summaries.length === 0) {
         showEmptyState(
            memoryState.searchQuery ? 'No summaries found' : 'No conversation summaries yet'
         );
         return;
      }

      const html = memoryState.summaries.map((summary) => renderSummaryItem(summary)).join('');
      memoryElements.list.innerHTML = html;

      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.classList.remove('hidden');
         memoryElements.loadMoreBtn.disabled =
            !memoryState.tabHasMore.summaries || !!memoryState.searchQuery;
      }
   }

   function renderSummaryItem(summary) {
      const dateStr = formatDate(summary.created_at);

      // Parse topics (comma-separated)
      const topics = (summary.topics || '')
         .split(',')
         .map((t) => t.trim())
         .filter((t) => t.length > 0);
      const topicsHtml = topics
         .slice(0, 5)
         .map((t) => `<span class="memory-item-topic">${escapeHtml(t)}</span>`)
         .join('');

      return `
         <div class="memory-item summary" data-summary-id="${summary.id}">
            <div class="memory-item-header">
               <div class="memory-item-text">${escapeHtml(summary.summary)}</div>
               <button class="memory-item-delete" data-summary-id="${summary.id}" title="Delete this summary" aria-label="Delete summary">
                  <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                     <path d="M3 6h18M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/>
                  </svg>
               </button>
            </div>
            ${topicsHtml ? `<div class="memory-item-topics">${topicsHtml}</div>` : ''}
            <div class="memory-item-meta">
               <span class="memory-item-date">${dateStr}</span>
               ${summary.message_count ? `<span>${summary.message_count} messages</span>` : ''}
            </div>
         </div>
      `;
   }

   /* =============================================================================
    * Entity Rendering
    * ============================================================================= */

   const ENTITY_TYPE_CLASSES = {
      person: 'entity-type-person',
      pet: 'entity-type-pet',
      project: 'entity-type-project',
      device: 'entity-type-device',
      place: 'entity-type-place',
      organization: 'entity-type-organization',
   };

   function renderEntitiesList() {
      if (!memoryElements.list) return;

      if (memoryState.entities.length === 0) {
         showEmptyState(
            memoryState.searchQuery ? 'No entities found' : 'No entities discovered yet'
         );
         if (window.DawnMemoryAliases) DawnMemoryAliases.renderProposalsPanel();
         return;
      }

      const html = memoryState.entities.map((entity) => renderEntityItem(entity)).join('');
      memoryElements.list.innerHTML = html;
      /* Prepend the Phase 1 Suggested-Merges panel (no-op if there are no
       * pending proposals).  Lives in www/js/ui/memory_aliases.js. */
      if (window.DawnMemoryAliases) DawnMemoryAliases.renderProposalsPanel();

      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.classList.remove('hidden');
         memoryElements.loadMoreBtn.disabled =
            !memoryState.tabHasMore.entities || !!memoryState.searchQuery;
      }
   }

   function renderEntityItem(entity) {
      const typeClass = ENTITY_TYPE_CLASSES[entity.entity_type] || 'entity-type-default';
      const dateStr = formatDate(entity.first_seen);
      const relations = aggregateRelationsForDisplay(entity.relations || []);
      const maxVisible = 3;
      const hasMore = relations.length > maxVisible;
      const visible = hasMore ? relations.slice(0, maxVisible) : relations;

      const relationsHtml = visible.map((rel) => renderRelationLine(rel)).join('');

      const moreHtml = hasMore
         ? `<div class="entity-relations-more" data-expand-entity="${entity.id}" tabindex="0" role="button">` +
           `+${relations.length - maxVisible} more</div>`
         : '';

      const hiddenHtml = hasMore
         ? `<div class="entity-relations-hidden" data-entity-hidden="${entity.id}" style="display:none">` +
           relations
              .slice(maxVisible)
              .map((rel) => renderRelationLine(rel))
              .join('') +
           '</div>'
         : '';

      const contactBadgeHtml =
         entity.entity_type === 'person'
            ? `<span class="entity-contact-badge" data-contact-entity-id="${entity.id}" ` +
              `data-contact-entity-name="${escapeHtml(entity.name)}" tabindex="0" role="button" ` +
              `title="View contacts for ${escapeHtml(entity.name)}">` +
              '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">' +
              '<path d="M16 21v-2a4 4 0 0 0-4-4H5a4 4 0 0 0-4 4v2"/>' +
              '<circle cx="8.5" cy="7" r="4"/></svg> contacts</span>'
            : '';

      // Phase 1 entity-merge: badge + expand slot for soft aliases.
      const aliasCount = entity.alias_count || 0;
      const aliasBadgeHtml =
         aliasCount > 0
            ? `<button class="entity-alias-badge" data-alias-toggle-id="${entity.id}" ` +
              `title="Show ${aliasCount} soft alias${aliasCount === 1 ? '' : 'es'}" ` +
              `aria-expanded="false">${aliasCount} alias${aliasCount === 1 ? '' : 'es'}</button>`
            : '';
      const aliasSlotHtml =
         aliasCount > 0
            ? `<div class="entity-aliases" data-aliases-for="${entity.id}" hidden></div>`
            : '';

      return (
         `<div class="memory-item entity" data-entity-id="${entity.id}">` +
         `<div class="entity-header">` +
         `<span class="entity-type-badge ${typeClass}">${escapeHtml(entity.entity_type || 'other')}</span>` +
         `<span class="entity-name">${escapeHtml(entity.name)}</span>` +
         aliasBadgeHtml +
         contactBadgeHtml +
         `<button class="entity-merge-btn" data-merge-entity-id="${entity.id}" ` +
         `title="Soft-link into another entity (reversible)" aria-label="Merge entity">` +
         `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">` +
         `<path d="M17 1l4 4-4 4"/><path d="M3 11V9a4 4 0 0 1 4-4h14"/>` +
         `<path d="M7 23l-4-4 4-4"/><path d="M21 13v2a4 4 0 0 1-4 4H3"/>` +
         `</svg></button>` +
         `<button class="memory-item-delete" data-entity-id="${entity.id}" ` +
         `title="Delete this entity" aria-label="Delete entity">` +
         `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">` +
         `<path d="M3 6h18M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/>` +
         `</svg></button>` +
         `</div>` +
         `<div class="memory-item-meta">` +
         `<span class="entity-mentions" ` +
         `title="Number of conversation references to this entity (aggregated across all soft aliases). ` +
         `Bumped once per extraction that names the entity — independent of how many facts that ` +
         `extraction produced.">` +
         `${entity.mention_count || 0} mentions</span>` +
         (dateStr ? `<span class="memory-item-date">${dateStr}</span>` : '') +
         `</div>` +
         aliasSlotHtml +
         (relations.length > 0
            ? `<div class="entity-relations">` +
              `<div class="entity-relations-label" ` +
              `title="Known facts and connections — each line is an edge in the entity graph ` +
              `connecting this entity to another entity or a literal value. Aggregated across ` +
              `all soft aliases.">` +
              `Relations</div>` +
              `${relationsHtml}${hiddenHtml}${moreHtml}` +
              `</div>`
            : '') +
         `</div>`
      );
   }

   /* v49 display-side aggregation: relations come from the server per-row
    * with per-row mention_count (the partial-UNIQUE invariant in
    * idx_memory_relations_unique_open is scoped to literal entity_id, not
    * canonical class).  When an equivalence-class spans multiple alias rows
    * (post-entity-merge), each alias row contributes its own (relation,
    * object) row to entity.relations; sum the counts so the rendered "× N"
    * badge reflects the class-wide observation count.
    *
    * Group key mirrors the SQL UNIQUE-index semantics: distinguish
    * entity-linked relations (`e:${object_entity_id}`) from literal-value
    * relations (`v:${object_name}`).  Without the prefix, an entity-linked
    * relation pointing at id=5 (name "DAWN") would collapse with a literal
    * relation `object_value="DAWN"` — semantically different edges. */
   function aggregateRelationsForDisplay(relations) {
      if (!Array.isArray(relations) || relations.length === 0) return relations || [];
      const byKey = new Map();
      const order = [];
      for (const rel of relations) {
         const objKey =
            rel.object_entity_id && rel.object_entity_id > 0
               ? `e:${rel.object_entity_id}`
               : `v:${rel.object_name || ''}`;
         const key = `${rel.direction || 'out'}|${rel.relation || ''}|${objKey}`;
         const count = Math.max(1, rel.mention_count | 0);
         if (byKey.has(key)) {
            const existing = byKey.get(key);
            existing.mention_count = (existing.mention_count | 0) + count;
            if ((rel.confidence || 0) > (existing.confidence || 0)) {
               existing.confidence = rel.confidence;
            }
            /* Widen the validity span across merged alias rows rather than
               keeping whichever happened to be first: two rows for the same
               relation are two sightings of one thing, so the union is the
               honest answer. Absent endpoints don't narrow it. */
            if (rel.valid_from && (!existing.valid_from || rel.valid_from < existing.valid_from)) {
               existing.valid_from = rel.valid_from;
            }
            if (rel.valid_to && (!existing.valid_to || rel.valid_to > existing.valid_to)) {
               existing.valid_to = rel.valid_to;
            }
         } else {
            const copy = Object.assign({}, rel, { mention_count: count });
            byKey.set(key, copy);
            order.push(key);
         }
      }
      return order.map((k) => byKey.get(k));
   }

   function renderRelationLine(rel) {
      const isLinked = rel.object_entity_id && rel.object_entity_id > 0;
      const targetClass = isLinked ? 'entity-relation-target' : 'entity-relation-value';
      const targetAttr = isLinked
         ? ` data-target-entity="${rel.object_entity_id}" tabindex="0" role="button"`
         : '';
      const targetName = escapeHtml(rel.object_name || '');
      const dirIndicator =
         rel.direction === 'in' ? '<span class="entity-relation-arrow">&larr;</span>' : '';

      const arrow = rel.direction === 'in' ? '&larr;' : '&rarr;';

      const count = rel.mention_count | 0;
      /* aria-label mirrors title because title on a non-interactive span is
       * unreliable in screen readers (most engines surface it only on hover
       * focus, which never fires for non-focusable elements). */
      const countHtml =
         count > 1
            ? ` <span class="entity-relation-count" aria-label="Observed ${count} times" title="Observed ${count} times">&times;&nbsp;${count}</span>`
            : '';

      return (
         `<div class="entity-relation">` +
         `<span class="entity-relation-arrow">${arrow}</span>` +
         `<span class="entity-relation-verb">${escapeHtml(rel.relation)}</span> ` +
         `<span class="${targetClass}"${targetAttr}>${targetName}</span>` +
         countHtml +
         renderRelationWhen(rel) +
         `</div>`
      );
   }

   /* Unix seconds -> YYYY-MM-DD, formatted in UTC.
      Deliberately not local time: these are stored as UTC midnight for a bare
      date, so rendering them in a negative-offset zone would show the previous
      day — a conference on the 27th would read as the 26th. */
   function formatRelationDate(ts) {
      const d = new Date(ts * 1000);
      const mm = String(d.getUTCMonth() + 1).padStart(2, '0');
      const dd = String(d.getUTCDate()).padStart(2, '0');
      return `${d.getUTCFullYear()}-${mm}-${dd}`;
   }

   /* The "when" of a relation. Rendered only when known — an absent range shows
      nothing rather than a placeholder, since most relations are timeless
      ("John owns a Jetson") and a "no date" marker on every one would be noise. */
   function renderRelationWhen(rel) {
      const from = rel.valid_from || 0;
      const to = rel.valid_to || 0;
      if (!from && !to) return '';

      let text;
      if (from && to) {
         /* Equal endpoints are the normal shape for a single-day event, not a
            degenerate range — show one date, not "X – X". */
         text =
            from === to
               ? formatRelationDate(from)
               : `${formatRelationDate(from)} – ${formatRelationDate(to)}`;
      } else if (from) {
         text = `since ${formatRelationDate(from)}`;
      } else {
         text = `until ${formatRelationDate(to)}`;
      }
      return ` <span class="entity-relation-when">${escapeHtml(text)}</span>`;
   }

   /**
    * Set up event delegation for delete buttons (called once in init)
    */
   function setupDeleteDelegation() {
      if (!memoryElements.list) return;
      memoryElements.list.addEventListener('click', handleListClick);
      memoryElements.list.addEventListener('keydown', handleListKeydown);
   }

   function handleListKeydown(e) {
      if (e.key !== 'Enter' && e.key !== ' ') return;
      const target = e.target.closest(
         '.entity-relation-target, .entity-relations-more, .entity-contact-badge, ' +
            '.entity-merge-btn, .entity-alias-badge, .entity-alias-split, ' +
            '.merge-proposal-approve, .merge-proposal-reject'
      );
      if (!target) return;
      e.preventDefault();
      target.click();
   }

   async function handleListClick(e) {
      /* Phase 1 entity-merge dispatch — runs FIRST so that:
       *   (a) when merge mode is active, a click on any non-source entity
       *       card lands as the target even on its relation-target /
       *       +N-more / contact-badge / merge-button children;
       *   (b) clicks on alias affordances (merge-btn, alias-badge,
       *       split, proposal approve/reject) get caught regardless of
       *       what they're nested inside.
       * Lives in www/js/ui/memory_aliases.js. */
      if (window.DawnMemoryAliases && DawnMemoryAliases.tryHandleClick(e)) {
         return;
      }

      // Handle relation target clicks (scroll to entity)
      const relTarget = e.target.closest('.entity-relation-target');
      if (relTarget) {
         const targetId = relTarget.dataset.targetEntity;
         if (targetId) {
            scrollToEntity(parseInt(targetId, 10));
         }
         return;
      }

      // Handle "+N more" expand toggle
      const moreToggle = e.target.closest('.entity-relations-more');
      if (moreToggle) {
         const entityId = moreToggle.dataset.expandEntity;
         const hidden = memoryElements.list.querySelector(`[data-entity-hidden="${entityId}"]`);
         if (hidden) {
            const isVisible = hidden.style.display !== 'none';
            hidden.style.display = isVisible ? 'none' : '';
            moreToggle.textContent = isVisible ? `+${hidden.children.length} more` : 'show less';
         }
         return;
      }

      // Handle entity contact badge click — switch to contacts tab filtered by entity
      const contactBadge = e.target.closest('.entity-contact-badge');
      if (contactBadge) {
         const entityId = parseInt(contactBadge.dataset.contactEntityId, 10);
         const entityName = contactBadge.dataset.contactEntityName;
         if (entityId && entityName && typeof DawnContacts !== 'undefined') {
            switchTab('contacts');
            DawnContacts.filterByEntity(entityId, entityName);
         }
         return;
      }

      // "show source" button
      const sourceBtn = e.target.closest('.memory-item-source-btn');
      if (sourceBtn) {
         e.stopPropagation();
         openSourceModal(parseInt(sourceBtn.dataset.factId, 10));
         return;
      }

      const btn = e.target.closest('.memory-item-delete');
      if (!btn) return;

      e.stopPropagation();
      const factId = btn.dataset.factId;
      const prefCategory = btn.dataset.prefCategory;
      const summaryId = btn.dataset.summaryId;
      const entityId = btn.dataset.entityId;

      if (factId) {
         const fact = memoryState.facts.find((f) => f.id === parseInt(factId, 10));
         const detail = fact ? fact.fact_text : null;

         if (await DawnDialog.confirm('Delete this fact?', { detail: detail, danger: true })) {
            requestDeleteFact(parseInt(factId, 10));
         }
      } else if (prefCategory) {
         const pref = memoryState.preferences.find((p) => p.category === prefCategory);
         const detail = pref ? `${pref.category}: ${pref.value}` : null;

         if (
            await DawnDialog.confirm('Delete this preference?', { detail: detail, danger: true })
         ) {
            requestDeletePreference(prefCategory);
         }
      } else if (summaryId) {
         const summary = memoryState.summaries.find((s) => s.id === parseInt(summaryId, 10));
         const detail = summary ? summary.summary : null;

         if (await DawnDialog.confirm('Delete this summary?', { detail: detail, danger: true })) {
            requestDeleteSummary(parseInt(summaryId, 10));
         }
      } else if (entityId) {
         const entity = memoryState.entities.find((e) => e.id === parseInt(entityId, 10));
         const detail = entity ? `${entity.name} (${entity.entity_type})` : null;

         if (
            await DawnDialog.confirm('Delete this entity and its relations?', {
               detail: detail,
               danger: true,
            })
         ) {
            requestDeleteEntity(parseInt(entityId, 10));
         }
      }
   }

   function scrollToEntity(entityId) {
      if (!memoryElements.list) return;
      const el = memoryElements.list.querySelector(
         `.memory-item.entity[data-entity-id="${entityId}"]`
      );
      if (el) {
         el.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
         el.classList.remove('highlight-pulse');
         // Force reflow to restart animation
         void el.offsetWidth;
         el.classList.add('highlight-pulse');
      }
   }

   /* =============================================================================
    * Panel Control
    * ============================================================================= */

   function open() {
      if (!memoryElements.popover) return;

      // Close doc library if open
      if (typeof DawnDocLibrary !== 'undefined') DawnDocLibrary.close();
      if (typeof DawnCodeProjects !== 'undefined') DawnCodeProjects.close();
      if (typeof DawnJobs !== 'undefined') DawnJobs.close();

      // Store trigger element for focus restoration
      triggerElement = document.activeElement;

      memoryElements.popover.classList.remove('hidden');
      memoryElements.btn.classList.add('active');

      /* Phase 2 entity-merge auto-route: on the FIRST open per page-load
       * with pending proposals, jump straight to the Graph tab so the
       * user lands on the Suggested-Merges panel.  Subsequent opens
       * respect whichever tab they last selected — sticky auto-route
       * was reportedly hijacking Facts-tab visits after a user dismissed
       * the panel without resolving proposals.  `autoRoutedThisLoad`
       * is module-private to memory.js so a page reload re-enables it. */
      if (
         !autoRoutedThisLoad &&
         window.DawnMemoryAliases &&
         typeof DawnMemoryAliases.shouldAutoRouteToGraph === 'function' &&
         DawnMemoryAliases.shouldAutoRouteToGraph() &&
         memoryState.activeTab !== 'entities'
      ) {
         switchTab('entities');
         autoRoutedThisLoad = true;
         /* Visual cue: flash the Graph tab so the user's eye is drawn
          * to why the panel opened where it did.  CSS class removed
          * after the 600ms animation completes; if the user closed the
          * panel before then, the next open won't re-flash because
          * autoRoutedThisLoad is sticky. */
         setTimeout(() => {
            const tabBtn = document.querySelector('.memory-tab[data-tab="entities"]');
            if (tabBtn) {
               tabBtn.classList.add('just-routed');
               setTimeout(() => tabBtn.classList.remove('just-routed'), 600);
            }
         }, 0);
      }

      // Request fresh data
      requestStats();
      loadActiveTabData();

      // Set up focus trap
      if (callbacks.trapFocus) {
         focusTrapCleanup = callbacks.trapFocus(memoryElements.popover);
      } else if (memoryElements.searchInput) {
         // Fallback: just focus search input
         setTimeout(() => memoryElements.searchInput.focus(), 100);
      }

      // Add click-outside listener
      document.addEventListener('click', handleClickOutside);
      if (memoryEscToken === null) {
         memoryEscToken = DawnEscStack.register(() => {
            close();
            return true;
         });
      }
   }

   function close() {
      if (!memoryElements.popover) return;

      if (window.DawnMemoryAliases) DawnMemoryAliases.cancelMergeMode();
      memoryElements.popover.classList.add('hidden');
      memoryElements.btn.classList.remove('active');

      // Clear search
      if (memoryElements.searchInput) {
         memoryElements.searchInput.value = '';
      }
      memoryState.searchQuery = '';

      // Clean up focus trap
      if (focusTrapCleanup) {
         focusTrapCleanup();
         focusTrapCleanup = null;
      }

      // Restore focus to trigger element
      if (triggerElement && typeof triggerElement.focus === 'function') {
         triggerElement.focus();
         triggerElement = null;
      }

      // Remove listeners
      document.removeEventListener('click', handleClickOutside);
      if (memoryEscToken !== null) {
         DawnEscStack.unregister(memoryEscToken);
         memoryEscToken = null;
      }
   }

   function toggle() {
      if (memoryElements.popover && memoryElements.popover.classList.contains('hidden')) {
         open();
      } else {
         close();
      }
   }

   function handleClickOutside(e) {
      // Don't close if clicking inside the contact modal (it's a sibling, not a child)
      const contactModal = document.getElementById('contact-modal');
      if (
         contactModal &&
         !contactModal.classList.contains('hidden') &&
         contactModal.contains(e.target)
      )
         return;

      if (
         memoryElements.popover &&
         !memoryElements.popover.contains(e.target) &&
         memoryElements.btn &&
         !memoryElements.btn.contains(e.target)
      ) {
         close();
      }
   }

   /* Escape close is handled via DawnEscStack (register-on-open in open() /
    * unregister in close()).  A modal opened above the panel (contact, source)
    * registers higher on the stack, so LIFO closes it first — no manual guard. */

   /* =============================================================================
    * Tab Handling
    * ============================================================================= */

   /* Tab strip — bound to the shared DawnTablist helper in setupEvents().
    * Click + ←/→/Home/End handled centrally; consumer just owns
    * memoryState.activeTab and re-applies DOM via tablist.sync(). */
   let tablist = null;

   function switchTab(tabName) {
      if (memoryState.activeTab === tabName) return;

      if (window.DawnMemoryAliases) DawnMemoryAliases.cancelMergeMode();
      memoryState.activeTab = tabName;

      if (tablist) tablist.sync();

      // Update tabpanel aria-labelledby to point to active tab
      if (memoryElements.list) {
         memoryElements.list.setAttribute('aria-labelledby', 'memory-tab-' + tabName);
      }

      // Clear search and load data
      if (memoryElements.searchInput) {
         memoryElements.searchInput.value = '';
      }
      memoryState.searchQuery = '';

      loadActiveTabData();
   }

   function loadActiveTabData() {
      showLoading();

      // Disable load more while loading
      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.classList.remove('hidden');
         memoryElements.loadMoreBtn.disabled = true;
      }

      const tab = memoryState.activeTab;
      memoryState.tabOffset[tab] = 0;

      switch (tab) {
         case 'facts':
            requestFacts(0);
            break;
         case 'preferences':
            requestPreferences(0);
            break;
         case 'summaries':
            requestSummaries(0);
            break;
         case 'entities':
            /* Refresh both entity list and proposal queue.  Alias cache reset
             * is owned by DawnMemoryAliases (memory_aliases.js). */
            requestEntities(0);
            if (window.DawnMemoryAliases) DawnMemoryAliases.requestProposalList();
            break;
         case 'contacts':
            if (typeof DawnContacts !== 'undefined') DawnContacts.loadContacts();
            break;
      }
   }

   /* =============================================================================
    * Search Handling
    * ============================================================================= */

   function handleSearchInput() {
      const query = memoryElements.searchInput.value.trim();

      // Clear existing timeout
      if (memoryState.searchTimeout) {
         clearTimeout(memoryState.searchTimeout);
      }

      // If empty, reload tab data
      if (query.length === 0) {
         memoryState.searchQuery = '';
         loadActiveTabData();
         return;
      }

      // Debounce search
      memoryState.searchTimeout = setTimeout(() => {
         memoryState.searchQuery = query;
         showLoading();
         if (memoryState.activeTab === 'contacts' && typeof DawnContacts !== 'undefined') {
            DawnContacts.searchContacts(query);
         } else {
            requestSearch(query);
         }
      }, 300);
   }

   /* =============================================================================
    * Forget All Handling
    * ============================================================================= */

   async function handleForgetAll() {
      const value = await DawnDialog.prompt('Type DELETE to confirm forgetting all memories.', '', {
         title: 'Forget Everything?',
         placeholder: 'DELETE',
      });
      if (value == null) return; // cancelled
      if (value.toUpperCase() === 'DELETE') {
         requestDeleteAll();
      } else if (value) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('You must type DELETE to confirm', 'error');
         }
      }
   }

   /* =============================================================================
    * Load More Handling
    * ============================================================================= */

   function handleLoadMore() {
      const tab = memoryState.activeTab;

      if (tab === 'contacts') {
         if (typeof DawnContacts !== 'undefined') DawnContacts.loadMore();
         return;
      }

      if (!memoryState.tabHasMore[tab]) return;
      if (memoryState.loading) return;

      memoryState.tabOffset[tab] += PAGE_SIZE;

      switch (tab) {
         case 'facts':
            requestFacts(memoryState.tabOffset[tab]);
            break;
         case 'preferences':
            requestPreferences(memoryState.tabOffset[tab]);
            break;
         case 'summaries':
            requestSummaries(memoryState.tabOffset[tab]);
            break;
         case 'entities':
            requestEntities(memoryState.tabOffset[tab]);
            break;
      }
   }

   /* =============================================================================
    * Export / Import — moved to www/js/ui/memory_import.js (DawnMemoryImport).
    * The DawnMemory.handleExportResponse / handleImportResponse thin-forwarders
    * (in the public surface below) keep dawn.js's dispatch unchanged.
    * ============================================================================= */

   /* =============================================================================
    * Utility Functions
    * ============================================================================= */

   function formatDate(timestamp) {
      if (!timestamp) return '';

      const date = new Date(timestamp * 1000);
      const now = new Date();
      const diffDays = Math.floor((now - date) / (1000 * 60 * 60 * 24));

      if (diffDays === 0) {
         return 'Today';
      } else if (diffDays === 1) {
         return 'Yesterday';
      } else if (diffDays < 7) {
         return date.toLocaleDateString(undefined, { weekday: 'short' });
      } else {
         return date.toLocaleDateString(undefined, { month: 'short', day: 'numeric' });
      }
   }

   function escapeHtml(text) {
      if (!text) return '';
      const div = document.createElement('div');
      div.textContent = text;
      return div.innerHTML;
   }

   /* =============================================================================
    * Visibility Control
    * ============================================================================= */

   function updateVisibility() {
      if (!memoryElements.btn) return;

      const authState = callbacks.getAuthState ? callbacks.getAuthState() : null;
      const isAuthenticated = authState && authState.authenticated;

      if (isAuthenticated) {
         memoryElements.btn.classList.remove('hidden');
      } else {
         memoryElements.btn.classList.add('hidden');
         close();
      }
   }

   /* =============================================================================
    * Initialization
    * ============================================================================= */

   function init(options) {
      // Store callbacks
      if (options) {
         callbacks.getAuthState = options.getAuthState;
         callbacks.trapFocus = options.trapFocus;
      }

      // Get elements
      memoryElements.btn = document.getElementById('memory-btn');
      memoryElements.popover = document.getElementById('memory-popover');
      memoryElements.closeBtn = document.getElementById('memory-close');
      memoryElements.searchInput = document.getElementById('memory-search-input');
      memoryElements.list = document.getElementById('memory-list');
      memoryElements.forgetAllBtn = document.getElementById('memory-forget-all');
      memoryElements.exportBtn = document.getElementById('memory-export');
      memoryElements.importBtn = document.getElementById('memory-import');
      memoryElements.loadMoreBtn = document.getElementById('memory-load-more');
      memoryElements.tabs = document.querySelectorAll('.memory-tab');
      memoryElements.statFacts = document.getElementById('memory-fact-count');
      memoryElements.statPrefs = document.getElementById('memory-pref-count');
      memoryElements.statSummaries = document.getElementById('memory-summary-count');
      memoryElements.statEntities = document.getElementById('memory-entity-count');
      memoryElements.statContacts = document.getElementById('memory-contact-count');

      if (!memoryElements.btn || !memoryElements.popover) {
         console.warn('DawnMemory: Required elements not found');
         return;
      }

      /* Phase 1 entity-merge UI: hand the alias module shared refs.
       * onEntitiesChanged is the canonical post-mutation refresh path
       * (re-pull stats + first entity page). */
      if (window.DawnMemoryAliases) {
         DawnMemoryAliases.init({
            memoryState: memoryState,
            memoryElements: memoryElements,
            callbacks: callbacks,
            escapeHtml: escapeHtml,
            isEntitiesTabActive: () => memoryState.activeTab === 'entities',
            onEntitiesChanged: () => {
               requestStats();
               memoryState.tabOffset.entities = 0;
               requestEntities(0);
            },
         });
      }

      /* Import/Export surface (modals + buttons) — owned by
       * www/js/ui/memory_import.js.  It wires its own buttons/modals;
       * onMemoriesChanged is the post-commit refresh (matches the old
       * inline handleImportResponse: re-pull stats + reload active tab). */
      if (window.DawnMemoryImport) {
         DawnMemoryImport.init({
            escapeHtml: escapeHtml,
            onMemoriesChanged: () => {
               requestStats();
               switchTab(memoryState.activeTab);
            },
         });
      }

      // Set up event delegation for delete buttons (single listener)
      setupDeleteDelegation();

      // Button click handler
      memoryElements.btn.addEventListener('click', (e) => {
         e.stopPropagation();
         toggle();
      });

      // Close button handler
      if (memoryElements.closeBtn) {
         memoryElements.closeBtn.addEventListener('click', close);
      }

      // Tab handlers — shared DawnTablist helper owns click + arrows.
      if (window.DawnTablist && memoryElements.tabs && memoryElements.tabs.length > 0) {
         tablist = window.DawnTablist.bind({
            tabs: memoryElements.tabs,
            getActive: () => memoryState.activeTab,
            onActivate: (name) => switchTab(name),
         });
         tablist.sync(); /* initial markup → matches memoryState.activeTab */
      }

      // Search handler
      if (memoryElements.searchInput) {
         memoryElements.searchInput.addEventListener('input', handleSearchInput);
      }

      // Forget all handler
      if (memoryElements.forgetAllBtn) {
         memoryElements.forgetAllBtn.addEventListener('click', handleForgetAll);
      }

      // Export/Import buttons + modals are wired by DawnMemoryImport.init() above.

      // Source modal close button, overlay click, and ESC key
      const srcClose = document.getElementById('memory-source-close');
      if (srcClose) srcClose.addEventListener('click', closeSourceModal);
      const srcModal = document.getElementById('memory-source-modal');
      if (srcModal) {
         srcModal.addEventListener('click', (e) => {
            if (e.target === srcModal) closeSourceModal();
         });
         // Escape close is handled via DawnEscStack (register-on-open in
         // openSourceModal / unregister in closeSourceModal).
      }

      // Initialize contacts module
      if (typeof DawnContacts !== 'undefined') DawnContacts.init();

      // Load more handler
      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.addEventListener('click', handleLoadMore);
      }

      console.log('DawnMemory: Initialized');
   }

   /* =============================================================================
    * Export
    * ============================================================================= */

   window.DawnMemory = {
      init,
      open,
      close,
      toggle,
      updateVisibility,
      // Response handlers
      handleStatsResponse,
      handleFactsResponse,
      handlePreferencesResponse,
      handleSummariesResponse,
      handleEntitiesResponse,
      handleSearchResponse,
      handleDeleteFactResponse,
      handleDeletePreferenceResponse,
      handleDeleteSummaryResponse,
      handleDeleteEntityResponse,
      handleMergeEntityResponse,
      // Phase 1 entity-merge response handlers
      handleEntityAliasesResponse,
      handleEntityMergeProposalListResponse,
      handleEntityLinkResponse,
      handleEntityUnlinkResponse,
      handleEntityProposalResolveResponse,
      handleDeleteAllResponse,
      handleExportResponse,
      handleImportResponse,
      handleFactSourceResponse,
   };
})();
