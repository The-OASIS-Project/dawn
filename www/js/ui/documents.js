/**
 * DAWN WebUI - Document Upload Module
 * Handles drag-and-drop, file picker, and document chip UI.
 * Documents are uploaded to the server for text extraction, then held in client
 * state until prepended to the user's next message.
 *
 * Supports plain text, PDF, DOCX, and HTML files.
 * Binary formats (PDF, DOCX) are extracted server-side.
 * Includes token budget checking with auto-summarize for large documents.
 */
(function (global) {
   'use strict';

   // Allowed file extensions (must match server-side list)
   const ALLOWED_EXTENSIONS = new Set([
      '.txt',
      '.md',
      '.csv',
      '.json',
      '.c',
      '.h',
      '.cpp',
      '.py',
      '.js',
      '.ts',
      '.sh',
      '.toml',
      '.yaml',
      '.yml',
      '.xml',
      '.log',
      '.cfg',
      '.ini',
      '.conf',
      '.rs',
      '.go',
      '.java',
      '.rb',
      '.html',
      '.htm',
      '.css',
      '.sql',
      '.mk',
      '.cmake',
      '.pdf',
      '.docx',
   ]);

   // Binary formats that need server-side extraction (can't FileReader.readAsText)
   const BINARY_FORMATS = new Set(['.pdf', '.docx']);

   // Code file extensions for chip color coding
   const CODE_FORMATS = new Set([
      '.c',
      '.h',
      '.cpp',
      '.py',
      '.js',
      '.ts',
      '.rs',
      '.go',
      '.java',
      '.rb',
      '.sh',
      '.sql',
   ]);

   // Prose-heavy formats that look better in proportional font
   const PROSE_FORMATS = new Set(['.pdf', '.docx']);

   // Token budget reserve (room for LLM response)
   const RESPONSE_RESERVE = 1024;

   // DOM element references
   let chipContainer = null;
   let announcer = null;

   /**
    * Initialize the documents module
    */
   function init() {
      chipContainer = document.getElementById('document-chips');
      announcer = document.getElementById('document-announcer');

      // Note: file input and drag-and-drop are handled by DawnAttach (attach.js)
   }

   /**
    * Get file extension (lowercase, with dot)
    */
   function getExtension(filename) {
      const dot = filename.lastIndexOf('.');
      if (dot < 0) return '';
      return filename.slice(dot).toLowerCase();
   }

   /**
    * Check if file extension is allowed
    */
   function isAllowedExtension(filename) {
      return ALLOWED_EXTENSIONS.has(getExtension(filename));
   }

   /**
    * Get the format category for chip color coding
    */
   function getFormatCategory(ext) {
      if (BINARY_FORMATS.has(ext)) return 'document';
      if (CODE_FORMATS.has(ext)) return 'code';
      return 'text';
   }

   /**
    * Format extension for display (strip dot, uppercase)
    */
   function formatExtension(ext) {
      return (ext || '').replace(/^\./, '').toUpperCase();
   }

   /**
    * Process multiple document files (respecting slot limits)
    */
   function processMultipleDocuments(files) {
      const { pendingDocuments, maxDocuments, maxFileSize } = DawnState.documentState;
      const availableSlots = maxDocuments - pendingDocuments.length;

      if (availableSlots <= 0) {
         DawnToast.show(`Maximum ${maxDocuments} documents allowed`, 'warning');
         return;
      }

      const filesToProcess = files.slice(0, availableSlots);
      if (files.length > availableSlots) {
         DawnToast.show(
            `Only adding ${availableSlots} document(s), limit is ${maxDocuments}`,
            'warning'
         );
      }

      for (const file of filesToProcess) {
         uploadDocument(file);
      }
   }

   /**
    * Upload a single document file to the server
    */
   async function uploadDocument(file) {
      const { maxFileSize } = DawnState.documentState;
      const ext = getExtension(file.name);
      const isBinary = BINARY_FORMATS.has(ext);

      // Client-side validation
      if (!isAllowedExtension(file.name)) {
         DawnToast.show('Unsupported file type', 'error');
         return;
      }

      if (file.size > maxFileSize) {
         const maxKB = Math.round(maxFileSize / 1024);
         DawnToast.show(`File too large (max ${maxKB} KB)`, 'error');
         return;
      }

      // Show skeleton chip with extraction status for binary formats
      const chipIndex = addSkeletonChip(file.name, isBinary);

      try {
         const formData = new FormData();
         formData.append('document', file, file.name);

         const response = await fetch('/api/documents', {
            method: 'POST',
            credentials: 'include',
            body: formData,
         });

         if (!response.ok) {
            removeChip(chipIndex, true);
            const status = response.status;
            if (status === 413) {
               const maxKB = Math.round(DawnState.documentState.maxFileSize / 1024);
               DawnToast.show(`File too large (max ${maxKB} KB)`, 'error');
            } else if (status === 415) {
               DawnToast.show('Unsupported file type', 'error');
            } else if (status === 422) {
               const errData = await response.json().catch(() => null);
               const msg = errData?.error || 'Could not extract text from file';
               DawnToast.show(msg, 'error');
            } else {
               DawnToast.show('Could not read file', 'error');
            }
            return;
         }

         const result = await response.json();

         // Add to pending documents
         const docEntry = {
            filename: result.filename,
            content: result.content,
            size: result.size || file.size,
            type: result.type || ext,
            estimated_tokens: result.estimated_tokens || 0,
            page_count: result.page_count || null,
            summarized: false,
            // v68: server-stored original file id (absent if storage disabled).
            original_blob_id: result.original_blob_id || null,
         };

         DawnState.documentState.pendingDocuments.push(docEntry);

         // Resolve skeleton chip to full chip
         resolveChip(chipIndex, docEntry);
         updateCounter();

         // Check token budget and offer summarization if needed
         checkTokenBudget(docEntry);

         // Announce to screen readers
         const count = DawnState.documentState.pendingDocuments.length;
         const max = DawnState.documentState.maxDocuments;
         const formatNote = isBinary ? ` (text extracted from ${formatExtension(ext)})` : '';
         announce(
            `Document added: ${docEntry.filename}${formatNote}. ${count} of ${max} documents attached.`
         );
      } catch (err) {
         console.error('Document upload error:', err);
         removeChip(chipIndex, true);
         DawnToast.show('Document upload failed', 'error');
      }
   }

   /**
    * Check if document exceeds available token budget and offer summarization
    */
   function checkTokenBudget(docEntry) {
      if (!docEntry.estimated_tokens || docEntry.estimated_tokens <= 0) return;

      // Get current context state from the gauge
      let contextMax = 0;
      let contextCurrent = 0;

      if (typeof DawnContextGauge !== 'undefined' && DawnContextGauge.getState) {
         const state = DawnContextGauge.getState();
         contextMax = state.max || 0;
         contextCurrent = state.current || 0;
      }

      if (contextMax <= 0) return; // No context info available

      const available = contextMax - contextCurrent - RESPONSE_RESERVE;
      if (docEntry.estimated_tokens <= available) return; // Fits fine

      // Document exceeds budget — show warning with summarize option
      const docTokens = docEntry.estimated_tokens.toLocaleString();
      const availTokens = Math.max(0, available).toLocaleString();

      DawnToast.show(
         `Document ~${docTokens} tokens. Only ~${availTokens} available.`,
         'warning',
         10000,
         {
            actions: [
               {
                  label: 'Summarize to fit',
                  onClick: (e) => {
                     summarizeDocument(docEntry, Math.max(available, 256));
                     e.target.closest('.toast')?.remove();
                  },
               },
            ],
         }
      );
   }

   /**
    * Summarize a document to fit within a token budget via TF-IDF
    */
   async function summarizeDocument(docEntry, targetTokens) {
      const docIndex = DawnState.documentState.pendingDocuments.indexOf(docEntry);
      if (docIndex < 0) return;

      try {
         const response = await fetch('/api/documents/summarize', {
            method: 'POST',
            credentials: 'include',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
               content: docEntry.content,
               target_tokens: targetTokens,
            }),
         });

         if (!response.ok) {
            DawnToast.show('Summarization failed', 'error');
            return;
         }

         const result = await response.json();

         // Update the document entry in place
         docEntry.content = result.content;
         docEntry.size = result.size;
         docEntry.estimated_tokens = result.estimated_tokens;
         docEntry.summarized = true;

         // Rebuild chips to reflect the update
         rebuildChips();
         DawnToast.show('Document summarized to fit context', 'info');
      } catch (err) {
         console.error('Summarize error:', err);
         DawnToast.show('Summarization failed', 'error');
      }
   }

   // --- Chip UI ---

   /**
    * Add a skeleton (loading) chip
    * Returns a temporary ID for later resolution
    */
   function addSkeletonChip(filename, isBinary) {
      if (!chipContainer) return -1;

      chipContainer.classList.remove('hidden');

      const chip = document.createElement('div');
      chip.className = 'document-chip skeleton';
      chip.dataset.tempId = String(Date.now());

      const nameSpan = document.createElement('span');
      nameSpan.className = 'doc-chip-name';
      nameSpan.textContent = truncateFilename(filename, 20);
      nameSpan.title = filename;

      const spinner = document.createElement('span');
      spinner.className = 'doc-chip-spinner';

      chip.appendChild(nameSpan);

      // Show extraction status for binary formats
      if (isBinary) {
         const statusSpan = document.createElement('span');
         statusSpan.className = 'doc-chip-status';
         statusSpan.textContent = 'Extracting\u2026';
         chip.appendChild(statusSpan);
      }

      chip.appendChild(spinner);
      chipContainer.appendChild(chip);

      return chip.dataset.tempId;
   }

   /**
    * Resolve a skeleton chip into a full document chip
    */
   function resolveChip(tempId, docEntry) {
      if (!chipContainer) return;

      const chip = chipContainer.querySelector(`.document-chip[data-temp-id="${tempId}"]`);
      if (!chip) return;

      chip.classList.remove('skeleton');
      chip.removeAttribute('data-temp-id');

      const docIndex = DawnState.documentState.pendingDocuments.indexOf(docEntry);
      chip.dataset.index = String(docIndex);
      chip.setAttribute('role', 'group');
      chip.setAttribute('aria-label', `Attached document: ${docEntry.filename}`);

      // Replace content
      chip.innerHTML = '';

      buildChipContent(chip, docEntry, docIndex);
   }

   /**
    * Build the inner content of a document chip
    */
   function buildChipContent(chip, docEntry, docIndex) {
      const ext = docEntry.type || '';
      const category = getFormatCategory(ext);

      const typeSpan = document.createElement('span');
      typeSpan.className = 'doc-chip-type';
      typeSpan.dataset.category = category;
      typeSpan.textContent = formatExtension(ext);

      const nameSpan = document.createElement('span');
      nameSpan.className = 'doc-chip-name';
      nameSpan.textContent = truncateFilename(docEntry.filename, 20);
      nameSpan.title = docEntry.filename;

      const sizeSpan = document.createElement('span');
      sizeSpan.className = 'doc-chip-size';
      // Show page count for PDFs
      const sizeText = docEntry.page_count
         ? `${docEntry.page_count} pg  ${formatSize(docEntry.size)}`
         : formatSize(docEntry.size);
      sizeSpan.textContent = sizeText;

      // Summarized indicator
      if (docEntry.summarized) {
         const sumSpan = document.createElement('span');
         sumSpan.className = 'doc-chip-summarized';
         sumSpan.textContent = '(summarized)';
         sumSpan.title = 'Content was summarized to fit context window';
         chip.appendChild(typeSpan);
         chip.appendChild(nameSpan);
         chip.appendChild(sizeSpan);
         chip.appendChild(sumSpan);
      } else {
         chip.appendChild(typeSpan);
         chip.appendChild(nameSpan);
         chip.appendChild(sizeSpan);
      }

      const removeBtn = document.createElement('button');
      removeBtn.className = 'doc-chip-remove';
      removeBtn.innerHTML = '&times;';
      removeBtn.title = `Remove ${docEntry.filename}`;
      removeBtn.setAttribute('aria-label', `Remove document: ${docEntry.filename}`);
      removeBtn.addEventListener('click', (e) => {
         e.stopPropagation();
         removeDocument(docIndex);
      });
      chip.appendChild(removeBtn);
   }

   /**
    * Remove a chip by temp ID (for upload failures)
    */
   function removeChip(tempId, isSkeleton) {
      if (!chipContainer) return;

      const selector = isSkeleton
         ? `.document-chip[data-temp-id="${tempId}"]`
         : `.document-chip[data-index="${tempId}"]`;
      const chip = chipContainer.querySelector(selector);
      if (chip) {
         chip.remove();
      }

      if (chipContainer.children.length === 0) {
         chipContainer.classList.add('hidden');
      }
   }

   /**
    * Remove a document by index
    */
   function removeDocument(index) {
      const { pendingDocuments } = DawnState.documentState;
      if (index < 0 || index >= pendingDocuments.length) return;

      const removed = pendingDocuments[index];
      pendingDocuments.splice(index, 1);
      rebuildChips();
      updateCounter();

      const count = pendingDocuments.length;
      if (count === 0) {
         announce(`Document removed: ${removed.filename}. No documents attached.`);
      } else {
         const max = DawnState.documentState.maxDocuments;
         announce(`Document removed: ${removed.filename}. ${count} of ${max} documents attached.`);
      }
   }

   /**
    * Rebuild all chips after removal (to fix indices)
    */
   function rebuildChips() {
      if (!chipContainer) return;
      chipContainer.innerHTML = '';

      const { pendingDocuments } = DawnState.documentState;
      if (pendingDocuments.length === 0) {
         chipContainer.classList.add('hidden');
         return;
      }

      chipContainer.classList.remove('hidden');
      pendingDocuments.forEach((doc, idx) => {
         const chip = document.createElement('div');
         chip.className = 'document-chip';
         chip.dataset.index = String(idx);
         chip.setAttribute('role', 'group');
         chip.setAttribute('aria-label', `Attached document: ${doc.filename}`);

         buildChipContent(chip, doc, idx);
         chipContainer.appendChild(chip);
      });
   }

   /**
    * Update the unified attachment counter
    */
   function updateCounter() {
      if (typeof DawnAttach !== 'undefined') {
         DawnAttach.updateCounter();
      }
   }

   /**
    * Get and clear all pending documents (called by sendTextMessage)
    */
   function getAndClearDocuments() {
      const docs = DawnState.documentState.pendingDocuments.slice();
      DawnState.documentState.pendingDocuments = [];

      if (chipContainer) {
         chipContainer.innerHTML = '';
         chipContainer.classList.add('hidden');
      }
      updateCounter();

      return docs;
   }

   /**
    * Check if there are pending documents
    */
   function hasPendingDocuments() {
      return DawnState.documentState.pendingDocuments.length > 0;
   }

   // --- Transcript Document Parsing ---

   /**
    * Regex to match [ATTACHED DOCUMENT: filename (N bytes)]...content...[END DOCUMENT].
    * The optional " blob:blb_xxxxxxxxxxxx" suffix (v68) links the stored original
    * file; older messages without it still match (the group is optional).
    * The "blob:<id>]" marker is mirrored by the producer (src/webui/webui_attachments.c) and the
    * orphan-sweep SQL; kept in sync by scripts/check_blob_marker_sync.sh.
    */
   const DOC_MARKER_RE =
      /\[ATTACHED DOCUMENT: (.+?) \((\d+) bytes\)(?: blob:(blb_[A-Za-z0-9]{12}))?\]\n([\s\S]*?)\n+\[END DOCUMENT\]/g;

   /**
    * Parse document markers from message text.
    * Returns { cleanText, documents: [{filename, size, content, blobId}] }
    */
   function parseDocumentMarkers(text) {
      const documents = [];
      const cleanText = text
         .replace(DOC_MARKER_RE, (_, filename, sizeBytes, blobId, content) => {
            documents.push({
               filename,
               size: formatSize(parseInt(sizeBytes, 10)),
               content,
               blobId: blobId || null,
            });
            return '';
         })
         .trim();

      return { cleanText, documents };
   }

   /**
    * Open the document viewer modal with given filename and content
    */
   function openDocumentViewer(filename, content) {
      const modal = document.getElementById('document-viewer-modal');
      const title = document.getElementById('document-viewer-title');
      const pre = document.getElementById('document-viewer-pre');
      const closeBtn = document.getElementById('document-viewer-close');
      const body = document.querySelector('.document-viewer-body');

      if (!modal || !title || !pre) return;

      title.textContent = filename;
      pre.textContent = content;

      // Format-aware rendering: use proportional font for prose formats
      const ext = getExtension(filename);
      const isProse = PROSE_FORMATS.has(ext);
      if (isProse) {
         pre.classList.add('doc-prose');
      } else {
         pre.classList.remove('doc-prose');
      }

      modal.classList.remove('hidden');

      let escToken = null; // DawnEscStack registration while this modal is open

      // Store the element that opened the modal for focus restoration
      const triggerEl = document.activeElement;

      // Scroll body to top
      if (body) body.scrollTop = 0;

      // Focus the close button
      if (closeBtn) closeBtn.focus();

      function closeModal() {
         modal.classList.add('hidden');
         // Restore focus to the trigger element
         if (triggerEl && typeof triggerEl.focus === 'function') {
            triggerEl.focus();
         }
         // Remove event listeners
         modal.removeEventListener('click', handleBackdropClick);
         document.removeEventListener('keydown', handleKeydown);
         if (escToken !== null) {
            DawnEscStack.unregister(escToken);
            escToken = null;
         }
      }

      function handleBackdropClick(e) {
         if (e.target === modal) closeModal();
      }

      // Escape close is handled via DawnEscStack (registered below); this handler
      // keeps only the Tab focus-trap.
      function handleKeydown(e) {
         // Focus trap: Tab cycles between close button and body
         if (e.key === 'Tab' && body && closeBtn) {
            const focusable = [closeBtn, body];
            const first = focusable[0];
            const last = focusable[focusable.length - 1];
            if (e.shiftKey) {
               if (document.activeElement === first) {
                  e.preventDefault();
                  last.focus();
               }
            } else {
               if (document.activeElement === last) {
                  e.preventDefault();
                  first.focus();
               }
            }
         }
      }

      if (closeBtn) closeBtn.addEventListener('click', closeModal, { once: true });
      modal.addEventListener('click', handleBackdropClick);
      document.addEventListener('keydown', handleKeydown);
      escToken = DawnEscStack.register(() => {
         closeModal();
         return true;
      });
   }

   // --- Utilities ---

   function truncateFilename(name, maxLen) {
      if (name.length <= maxLen) return name;
      const ext = getExtension(name);
      const base = name.slice(0, name.length - ext.length);
      const available = maxLen - ext.length - 1; // 1 for ellipsis
      if (available <= 0) return name.slice(0, maxLen);
      return base.slice(0, available) + '\u2026' + ext;
   }

   function formatSize(bytes) {
      if (bytes < 1024) return bytes + ' B';
      if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
      return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
   }

   function announce(message) {
      if (!announcer) return;
      announcer.textContent = '';
      setTimeout(() => {
         announcer.textContent = message;
      }, 50);
   }

   // Export module
   global.DawnDocuments = {
      init,
      processMultipleDocuments,
      getAndClearDocuments,
      hasPendingDocuments,
      isAllowedExtension,
      parseDocumentMarkers,
      openDocumentViewer,
      // Shared with transcript chips so reloaded documents match the upload UI.
      getExtension,
      getFormatCategory,
      formatExtension,
   };
})(window);
