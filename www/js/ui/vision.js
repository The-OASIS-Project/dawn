/**
 * DAWN WebUI - Vision Image Handling Module
 * Includes: file upload, paste, drag-and-drop, client-side compression
 * Supports multiple images (up to 5) per message.
 *
 * Security: SVG explicitly excluded to prevent XSS attacks.
 * All data URIs are validated before rendering.
 */
(function (global) {
   'use strict';

   // Safe data URI prefixes (XSS prevention - SVG excluded)
   const SAFE_PREFIXES = [
      'data:image/jpeg;base64,',
      'data:image/png;base64,',
      'data:image/gif;base64,',
      'data:image/webp;base64,',
   ];

   // Allowed MIME types (SVG explicitly excluded for XSS prevention)
   const VALID_TYPES = ['image/jpeg', 'image/png', 'image/gif', 'image/webp'];

   // Image ID format: "img_" + 12 alphanumeric characters
   const IMAGE_ID_REGEX = /^img_[a-zA-Z0-9]{12}$/;

   // LocalStorage prefix for cached images
   const IMAGE_CACHE_PREFIX = 'dawn_img_';

   // DOM element references
   let previewContainer = null;
   let loadingOverlay = null;
   let dropTarget = null;
   let textInput = null;
   let announcer = null;

   // Camera elements
   let cameraModal = null;
   let cameraEscToken = null; // DawnEscStack registration while the camera modal is open
   let cameraTrapCleanup = null; // DawnSettingsModals.trapFocus cleanup while open
   let cameraTriggerEl = null; // element to return focus to on close (M13)
   let cameraVideo = null;
   let cameraCanvas = null;
   let cameraPreviewImg = null;
   let cameraError = null;
   let cameraLiveControls = null;
   let cameraReviewControls = null;

   // Camera state
   let cameraStream = null;
   let currentFacingMode = 'environment'; // Default to rear camera on mobile
   let hasMultipleCameras = true; // Assume multiple until enumerated
   let cameraCanvasCtx = null; // Cached 2D context for camera canvas

   /**
    * Initialize the vision module
    */
   function init() {
      previewContainer = document.getElementById('image-preview');
      loadingOverlay = document.querySelector('.preview-loading');
      dropTarget = document.getElementById('input-area');
      textInput = document.getElementById('text-input');
      announcer = document.getElementById('vision-announcer');

      // Camera elements
      cameraModal = document.getElementById('camera-modal');
      cameraVideo = document.getElementById('camera-video');
      cameraCanvas = document.getElementById('camera-canvas');
      cameraPreviewImg = document.getElementById('camera-preview-img');
      cameraError = document.getElementById('camera-error');
      cameraLiveControls = document.getElementById('camera-live-controls');
      cameraReviewControls = document.getElementById('camera-review-controls');

      bindEvents();
      bindCameraEvents();
      // Vision support check will happen when config is received
   }

   /**
    * Bind event handlers
    */
   function bindEvents() {
      // Paste handler on text input (images from clipboard)
      if (textInput) {
         textInput.addEventListener('paste', handlePaste);
      }

      // Note: drag-and-drop and file input are handled by DawnAttach (attach.js)
   }

   /**
    * Bind camera modal events
    */
   function bindCameraEvents() {
      if (!cameraModal) return;

      // Close button
      const closeBtn = document.getElementById('camera-close');
      if (closeBtn) {
         closeBtn.addEventListener('click', closeCamera);
      }

      // Capture button
      const captureBtn = document.getElementById('camera-capture-btn');
      if (captureBtn) {
         captureBtn.addEventListener('click', captureFrame);
      }

      // Switch camera button
      const switchBtn = document.getElementById('camera-switch');
      if (switchBtn) {
         switchBtn.addEventListener('click', switchCamera);
      }

      // Retake button
      const retakeBtn = document.getElementById('camera-retake');
      if (retakeBtn) {
         retakeBtn.addEventListener('click', retakePhoto);
      }

      // Use photo button
      const useBtn = document.getElementById('camera-use');
      if (useBtn) {
         useBtn.addEventListener('click', usePhoto);
      }

      // Upload fallback button (when camera fails)
      const fallbackBtn = document.getElementById('camera-upload-fallback');
      if (fallbackBtn) {
         fallbackBtn.addEventListener('click', () => {
            closeCamera();
            // Trigger the unified attach file input
            const attachInput = document.getElementById('attach-file-input');
            if (attachInput) attachInput.click();
         });
      }

      // Tab focus-trap is provided by DawnSettingsModals.trapFocus (registered in
      // openCamera / cleaned up in closeCamera); Escape close via DawnEscStack.

      // Close on backdrop click
      cameraModal.addEventListener('click', (e) => {
         if (e.target === cameraModal) {
            closeCamera();
         }
      });
   }

   /**
    * Check how many cameras are available and update switch button visibility
    */
   async function enumerateCameras() {
      try {
         const devices = await navigator.mediaDevices.enumerateDevices();
         const cameras = devices.filter((d) => d.kind === 'videoinput');
         hasMultipleCameras = cameras.length > 1;
      } catch (err) {
         console.warn('Vision: Could not enumerate cameras:', err);
         hasMultipleCameras = true; // Assume multiple on error (more conservative)
      }
   }

   /**
    * Update camera switch button visibility based on camera count
    */
   function updateCameraSwitchVisibility() {
      const switchBtn = document.getElementById('camera-switch');
      const placeholder = document.querySelector('.btn-placeholder');
      if (switchBtn) {
         if (hasMultipleCameras) {
            switchBtn.classList.remove('hidden');
            if (placeholder) placeholder.classList.add('hidden');
         } else {
            switchBtn.classList.add('hidden');
            // Show placeholder to maintain layout balance
            if (placeholder) placeholder.classList.remove('hidden');
         }
      }
   }

   /**
    * Open camera modal and start video stream
    */
   async function openCamera() {
      if (!cameraModal || !cameraVideo) {
         console.warn('Vision: Camera elements not found');
         return;
      }

      // Check if at image limit
      const max = DawnState.visionState.maxImages || 5;
      if (DawnState.visionState.pendingImages.length >= max) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(`Maximum ${max} images reached`, 'warning');
         }
         return;
      }

      // Show modal
      cameraTriggerEl = document.activeElement;
      cameraModal.classList.remove('hidden');
      if (cameraEscToken === null) {
         cameraEscToken = DawnEscStack.register(() => {
            closeCamera();
            return true;
         });
      }
      // Reuse the shared focus trap (live per-keypress query) instead of an
      // inline one.  skipInitialFocus: openCamera manages its own focus.
      if (
         cameraTrapCleanup === null &&
         window.DawnSettingsModals &&
         typeof window.DawnSettingsModals.trapFocus === 'function'
      ) {
         cameraTrapCleanup = window.DawnSettingsModals.trapFocus(cameraModal, {
            skipInitialFocus: true,
         });
      }
      showCameraLiveControls();
      hideCameraError();

      try {
         // Request camera access
         cameraStream = await navigator.mediaDevices.getUserMedia({
            video: {
               facingMode: currentFacingMode,
               width: { ideal: 1280 },
               height: { ideal: 720 },
            },
         });

         cameraVideo.srcObject = cameraStream;

         // Update video mirror based on facing mode
         updateVideoMirror();

         // Check camera count (requires permission first to get labels)
         await enumerateCameras();
         updateCameraSwitchVisibility();

         // Focus the modal for keyboard navigation
         cameraModal.focus();
      } catch (err) {
         console.error('Vision: Camera access error:', err);
         showCameraError();

         if (typeof DawnToast !== 'undefined') {
            if (err.name === 'NotAllowedError') {
               DawnToast.show('Camera access denied. Check browser permissions.', 'error');
            } else if (err.name === 'NotFoundError') {
               DawnToast.show('No camera found on this device.', 'error');
            } else {
               DawnToast.show('Could not access camera.', 'error');
            }
         }
      }
   }

   /**
    * Close camera modal and stop video stream
    */
   function closeCamera() {
      if (cameraStream) {
         cameraStream.getTracks().forEach((track) => track.stop());
         cameraStream = null;
      }

      if (cameraVideo) {
         cameraVideo.srcObject = null;
      }

      if (cameraPreviewImg) {
         cameraPreviewImg.src = '';
         cameraPreviewImg.classList.add('hidden');
      }

      if (cameraModal) {
         cameraModal.classList.add('hidden');
      }
      if (cameraEscToken !== null) {
         DawnEscStack.unregister(cameraEscToken);
         cameraEscToken = null;
      }
      if (cameraTrapCleanup) {
         cameraTrapCleanup();
         cameraTrapCleanup = null;
      }
      // Return focus to whatever opened the camera (M13).
      if (cameraTriggerEl && typeof cameraTriggerEl.focus === 'function') {
         cameraTriggerEl.focus();
      }
      cameraTriggerEl = null;

      showCameraLiveControls();
   }

   /**
    * Capture a frame from the video stream
    */
   function captureFrame() {
      if (!cameraVideo || !cameraCanvas || !cameraPreviewImg) return;

      // Set canvas size to video dimensions
      cameraCanvas.width = cameraVideo.videoWidth;
      cameraCanvas.height = cameraVideo.videoHeight;

      // Cache 2D context (reuse across captures)
      if (!cameraCanvasCtx) {
         cameraCanvasCtx = cameraCanvas.getContext('2d');
      }

      // Reset transform (in case previous capture applied flip)
      cameraCanvasCtx.setTransform(1, 0, 0, 1, 0, 0);

      // If front camera (mirrored), flip the canvas
      if (currentFacingMode === 'user') {
         cameraCanvasCtx.translate(cameraCanvas.width, 0);
         cameraCanvasCtx.scale(-1, 1);
      }

      cameraCanvasCtx.drawImage(cameraVideo, 0, 0);

      // Convert to data URL
      const dataUrl = cameraCanvas.toDataURL('image/jpeg', 0.9);
      cameraPreviewImg.src = dataUrl;

      // Show preview, hide video
      cameraVideo.classList.add('hidden');
      cameraPreviewImg.classList.remove('hidden');

      // Switch to review controls
      showCameraReviewControls();
   }

   /**
    * Go back to live camera view
    */
   function retakePhoto() {
      if (cameraPreviewImg) {
         cameraPreviewImg.src = '';
         cameraPreviewImg.classList.add('hidden');
      }

      if (cameraVideo) {
         cameraVideo.classList.remove('hidden');
      }

      showCameraLiveControls();
   }

   /**
    * Use the captured photo - add to pending images
    */
   async function usePhoto() {
      if (!cameraPreviewImg || !cameraPreviewImg.src) return;

      try {
         // Convert data URL to blob (without fetch, to avoid CSP issues)
         const dataUrl = cameraPreviewImg.src;
         const blob = dataUrlToBlob(dataUrl);

         // Create a File object
         const file = new File([blob], 'camera-capture.jpg', { type: 'image/jpeg' });

         // Close modal first
         closeCamera();

         // Process through existing image pipeline
         await processImage(file);
      } catch (err) {
         console.error('Vision: Error processing captured image:', err);
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Error processing captured image', 'error');
         }
      }
   }

   /**
    * Convert a data URL to a Blob (CSP-safe, no fetch required)
    */
   function dataUrlToBlob(dataUrl) {
      const parts = dataUrl.split(',');
      const mimeMatch = parts[0].match(/:(.*?);/);
      const mime = mimeMatch ? mimeMatch[1] : 'image/jpeg';
      const base64 = parts[1];
      const binary = atob(base64);
      const array = new Uint8Array(binary.length);
      for (let i = 0; i < binary.length; i++) {
         array[i] = binary.charCodeAt(i);
      }
      return new Blob([array], { type: mime });
   }

   /**
    * Switch between front and rear cameras
    */
   async function switchCamera() {
      // Toggle facing mode
      currentFacingMode = currentFacingMode === 'user' ? 'environment' : 'user';

      // Stop current stream
      if (cameraStream) {
         cameraStream.getTracks().forEach((track) => track.stop());
      }

      try {
         // Get new stream with different camera
         cameraStream = await navigator.mediaDevices.getUserMedia({
            video: {
               facingMode: currentFacingMode,
               width: { ideal: 1280 },
               height: { ideal: 720 },
            },
         });

         cameraVideo.srcObject = cameraStream;
         updateVideoMirror();
      } catch (err) {
         console.error('Vision: Error switching camera:', err);
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Could not switch camera', 'error');
         }
      }
   }

   /**
    * Update video element mirror based on facing mode
    */
   function updateVideoMirror() {
      if (!cameraVideo) return;

      // Mirror front camera (user), don't mirror rear camera (environment)
      if (currentFacingMode === 'user') {
         cameraVideo.classList.remove('rear-camera');
      } else {
         cameraVideo.classList.add('rear-camera');
      }
   }

   /**
    * Show live camera controls
    */
   function showCameraLiveControls() {
      if (cameraLiveControls) cameraLiveControls.classList.remove('hidden');
      if (cameraReviewControls) cameraReviewControls.classList.add('hidden');
   }

   /**
    * Show review controls (after capture)
    */
   function showCameraReviewControls() {
      if (cameraLiveControls) cameraLiveControls.classList.add('hidden');
      if (cameraReviewControls) cameraReviewControls.classList.remove('hidden');
   }

   /**
    * Show camera error state
    */
   function showCameraError() {
      if (cameraError) cameraError.classList.remove('hidden');
      if (cameraVideo) cameraVideo.classList.add('hidden');
      if (cameraLiveControls) cameraLiveControls.classList.add('hidden');
   }

   /**
    * Hide camera error state
    */
   function hideCameraError() {
      if (cameraError) cameraError.classList.add('hidden');
      if (cameraVideo) cameraVideo.classList.remove('hidden');
   }

   /**
    * Handle paste event - capture images from clipboard
    */
   async function handlePaste(event) {
      const items = event.clipboardData?.items;
      if (!items) return;

      const imageItems = [];
      for (const item of items) {
         // SECURITY: Explicit whitelist (excludes SVG)
         if (VALID_TYPES.includes(item.type)) {
            imageItems.push(item);
         }
      }

      if (imageItems.length === 0) return;

      if (!DawnState.visionState.visionEnabled) {
         DawnToast.show('Vision not available for current model', 'warning');
         return;
      }

      event.preventDefault();

      // Convert items to files and process
      const files = imageItems.map((item) => item.getAsFile()).filter(Boolean);
      await processMultipleImages(files);
   }

   /**
    * Process multiple image files
    * @param {File[]} files - Array of image files to process
    */
   async function processMultipleImages(files) {
      const { pendingImages, maxImages } = DawnState.visionState;
      const availableSlots = maxImages - pendingImages.length;

      if (availableSlots <= 0) {
         DawnToast.show(`Maximum ${maxImages} images allowed`, 'warning');
         return;
      }

      // Limit to available slots
      const filesToProcess = files.slice(0, availableSlots);
      if (files.length > availableSlots) {
         DawnToast.show(`Only adding ${availableSlots} image(s), limit is ${maxImages}`, 'warning');
      }

      setLoading(true);

      for (const file of filesToProcess) {
         await processImage(file);
      }

      setLoading(false);
      updateCounter();
   }

   /**
    * Process a single image file - validate, compress, upload, and add to pending
    * Uploads image to server and stores the returned image ID for history.
    */
   async function processImage(file) {
      // Validate type (SECURITY: explicit whitelist, no SVG)
      if (!VALID_TYPES.includes(file.type)) {
         DawnToast.show('Unsupported image type. Use JPEG, PNG, GIF, or WebP.', 'error');
         return;
      }

      try {
         // Compress/resize image client-side (memory efficiency)
         const compressed = await compressImage(file);

         // Validate compressed size
         if (compressed.size > DawnState.visionState.maxSize) {
            DawnToast.show(`Image "${file.name}" too large after compression (max 4MB)`, 'error');
            return;
         }

         // Read full image as base64 (for LLM and preview)
         const dataUrl = await readAsDataURL(compressed);

         // SECURITY: Validate data URI format before storing
         if (!isSafeDataUri(dataUrl)) {
            DawnToast.show('Invalid image data', 'error');
            return;
         }

         // Upload to server and get image ID
         const uploadResult = await uploadImage(compressed);
         if (!uploadResult || !uploadResult.id) {
            DawnToast.show(`Failed to upload image: ${file.name}`, 'error');
            return;
         }

         // Cache the image data locally for preview
         const cacheKey = IMAGE_CACHE_PREFIX + uploadResult.id;
         try {
            localStorage.setItem(cacheKey, dataUrl);
         } catch (e) {
            console.warn('localStorage cache full');
         }

         // The turn names the image by this id; the server reads the stored
         // file (the client never sends image bytes over the socket).
         const imageData = {
            id: uploadResult.id, // Server-assigned ID: the turn's image_ids + history
            mimeType: compressed.type,
            previewUrl: dataUrl, // Keep for preview display
         };

         DawnState.visionState.pendingImages.push(imageData);
         addPreviewItem(dataUrl, DawnState.visionState.pendingImages.length - 1);

         // Announce to screen readers
         const count = DawnState.visionState.pendingImages.length;
         const max = DawnState.visionState.maxImages;
         announce(`Image added. ${count} of ${max} images attached.`);
      } catch (err) {
         console.error('Image processing error:', err);
         DawnToast.show(`Failed to process image: ${file.name}`, 'error');
      }
   }

   /**
    * Compress image to max 1024px on longest edge, JPEG 85% quality
    * Optimized for LLM vision APIs (~1MP = ~1400 tokens on Claude)
    * All formats converted to JPEG for consistent small file sizes
    */
   function compressImage(file) {
      const maxDim = DawnState.visionState.maxDimension;

      return new Promise((resolve, reject) => {
         const img = new Image();

         img.onload = () => {
            let { width, height } = img;

            // Only resize if exceeds max dimension
            if (width > maxDim || height > maxDim) {
               const scale = Math.min(maxDim / width, maxDim / height);
               width = Math.round(width * scale);
               height = Math.round(height * scale);
            }

            const canvas = document.createElement('canvas');
            canvas.width = width;
            canvas.height = height;
            const ctx = canvas.getContext('2d');

            // Fill with white background (handles PNG transparency)
            ctx.fillStyle = '#FFFFFF';
            ctx.fillRect(0, 0, width, height);

            ctx.drawImage(img, 0, 0, width, height);

            // Always output JPEG 85% for consistent small file sizes
            canvas.toBlob(
               (blob) => {
                  if (blob) {
                     resolve(blob);
                  } else {
                     reject(new Error('Canvas toBlob failed'));
                  }
               },
               'image/jpeg',
               0.85
            );

            // Cleanup object URL
            URL.revokeObjectURL(img.src);
         };

         img.onerror = () => {
            URL.revokeObjectURL(img.src);
            reject(new Error('Image load failed'));
         };

         img.src = URL.createObjectURL(file);
      });
   }

   /**
    * Read a blob as a data URL
    */
   function readAsDataURL(blob) {
      return new Promise((resolve, reject) => {
         const reader = new FileReader();
         reader.onload = () => resolve(reader.result);
         reader.onerror = () => reject(reader.error);
         reader.readAsDataURL(blob);
      });
   }

   /**
    * Upload an image to the server and return the image ID
    * @param {Blob} blob - Image data blob
    * @returns {Promise<{id: string, mimeType: string, size: number}|null>}
    */
   async function uploadImage(blob) {
      const formData = new FormData();
      formData.append('image', blob, 'image.jpg');

      try {
         const response = await fetch('/api/images', {
            method: 'POST',
            credentials: 'include',
            body: formData,
         });

         if (!response.ok) {
            const errorData = await response.json().catch(() => ({}));
            console.error('Image upload failed:', response.status, errorData);
            throw new Error(errorData.error || `Upload failed: ${response.status}`);
         }

         const result = await response.json();
         console.log('Image uploaded:', result.id, result.size, 'bytes');
         return result;
      } catch (err) {
         console.error('Image upload error:', err);
         return null;
      }
   }

   /**
    * Evict the oldest cached images from localStorage.
    * localStorage.key() iterates in insertion order, so keys[0..] are oldest-first,
    * giving an LRU-ish eviction. Only touches our own dawn_img_ entries.
    * @param {number} maxRemove - Max entries to remove this pass
    * @returns {number} - Number actually removed
    */
   function evictOldImageCacheEntries(maxRemove) {
      const keys = [];
      for (let i = 0; i < localStorage.length; i++) {
         const k = localStorage.key(i);
         if (k && k.startsWith(IMAGE_CACHE_PREFIX)) {
            keys.push(k);
         }
      }
      const removeCount = Math.min(maxRemove, keys.length);
      for (let i = 0; i < removeCount; i++) {
         localStorage.removeItem(keys[i]);
      }
      return removeCount;
   }

   /**
    * Cache an image data URL, evicting old cached images on quota errors and
    * retrying — so a full cache no longer permanently disables caching.
    * @param {string} cacheKey - localStorage key
    * @param {string} dataUrl - Data URL to cache
    * @returns {boolean} - true if cached, false if skipped
    */
   function cacheImageDataUrl(cacheKey, dataUrl) {
      for (let attempt = 0; attempt < 6; attempt++) {
         try {
            localStorage.setItem(cacheKey, dataUrl);
            return true;
         } catch (e) {
            // Quota exceeded — free space by evicting older cached images, then retry.
            // Quota is a function of BYTES, not entry count: a few large recent entries
            // can dwarf many tiny old ones, so escalate the batch each pass (8,16,32,...)
            // until the write fits or every cached image is gone. evict==0 means nothing
            // is left to free, so this single image alone exceeds the quota.
            if (evictOldImageCacheEntries(8 << attempt) === 0) {
               console.warn('Image cache: item too large for localStorage, skipping cache');
               return false;
            }
         }
      }
      console.warn('Image cache: could not free enough localStorage, skipping cache');
      return false;
   }

   /**
    * Load an image from server by ID (with localStorage caching)
    * @param {string} imageId - Image ID (e.g., "img_a1b2c3d4e5f6")
    * @returns {Promise<string|null>} - Data URL or null if not found
    */
   async function loadImageById(imageId) {
      if (!IMAGE_ID_REGEX.test(imageId)) {
         console.warn('Invalid image ID format:', imageId);
         return null;
      }

      // Check localStorage cache first
      const cacheKey = IMAGE_CACHE_PREFIX + imageId;
      const cached = localStorage.getItem(cacheKey);
      if (cached) {
         return cached;
      }

      try {
         const response = await fetch(`/api/images/${imageId}`, {
            credentials: 'include',
         });

         if (!response.ok) {
            console.warn('Image load failed:', response.status);
            return null;
         }

         const blob = await response.blob();
         const dataUrl = await readAsDataURL(blob);

         // Cache in localStorage, evicting old entries on quota errors so a full
         // cache self-heals instead of permanently refetching every image.
         cacheImageDataUrl(cacheKey, dataUrl);

         return dataUrl;
      } catch (err) {
         console.error('Image load error:', err);
         return null;
      }
   }

   /**
    * Check if a string is a valid image ID
    * @param {string} str - String to check
    * @returns {boolean}
    */
   function isImageId(str) {
      return IMAGE_ID_REGEX.test(str);
   }

   /**
    * SECURITY: Validate data URI is safe for rendering
    * Prevents XSS via SVG or malformed data URIs
    */
   function isSafeDataUri(uri) {
      if (typeof uri !== 'string') return false;

      // Check against safe prefixes (case-insensitive)
      const hasValidPrefix = SAFE_PREFIXES.some((prefix) =>
         uri.toLowerCase().startsWith(prefix.toLowerCase())
      );
      if (!hasValidPrefix) return false;

      // Validate base64 portion contains only valid characters
      const base64Part = uri.split(',')[1];
      if (!base64Part) return false;

      return /^[A-Za-z0-9+/=]+$/.test(base64Part);
   }

   /**
    * Announce message to screen readers via aria-live region
    * @param {string} message - Message to announce
    */
   function announce(message) {
      if (!announcer) return;
      // Clear and re-set to ensure announcement even if same text
      announcer.textContent = '';
      // Small delay ensures screen readers pick up the change
      setTimeout(() => {
         announcer.textContent = message;
      }, 50);
   }

   /**
    * Set loading state
    */
   function setLoading(loading) {
      DawnState.visionState.isProcessing = loading;
      const attachBtn = document.getElementById('attach-btn');
      if (attachBtn) {
         attachBtn.classList.toggle('loading', loading);
      }
      if (loadingOverlay) {
         loadingOverlay.classList.toggle('hidden', !loading);
      }
   }

   /**
    * Add a preview item to the preview container
    * @param {string} dataUrl - Image data URL for preview
    * @param {number} index - Index in pendingImages array
    */
   function addPreviewItem(dataUrl, index) {
      if (!previewContainer) return;

      previewContainer.classList.remove('hidden');

      const item = document.createElement('div');
      item.className = 'preview-item';
      item.dataset.index = index;
      item.setAttribute('role', 'group');
      item.setAttribute('aria-label', `Attached image ${index + 1}`);

      const img = document.createElement('img');
      img.src = dataUrl;
      img.alt = `Attached image ${index + 1}`;

      const removeBtn = document.createElement('button');
      removeBtn.className = 'preview-remove';
      removeBtn.innerHTML = '&times;';
      removeBtn.title = `Remove image ${index + 1}`;
      removeBtn.setAttribute('aria-label', `Remove image ${index + 1}`);
      removeBtn.addEventListener('click', (e) => {
         e.stopPropagation();
         removeImage(index);
      });

      item.appendChild(img);
      item.appendChild(removeBtn);
      previewContainer.appendChild(item);
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
    * Remove an image by index
    * @param {number} index - Index of image to remove
    */
   function removeImage(index) {
      const { pendingImages } = DawnState.visionState;
      if (index < 0 || index >= pendingImages.length) return;

      // Capture for undo — pendingImages is purely client-side, pre-send state,
      // so removal is fully reversible (re-insert at the same index).
      const removedImage = pendingImages[index];
      const removedIndex = index;

      // Remove from array
      pendingImages.splice(index, 1);

      // Rebuild preview container
      rebuildPreviews();
      updateCounter();

      // Announce to screen readers
      const count = pendingImages.length;
      if (count === 0) {
         announce('Image removed. No images attached.');
      } else {
         const max = DawnState.visionState.maxImages;
         announce(`Image removed. ${count} of ${max} images attached.`);
      }

      // Offer a quick Undo (client-side, fully reversible — see showUndo).
      if (typeof DawnToast !== 'undefined' && DawnToast.showUndo) {
         DawnToast.showUndo('Image removed. Undo.', {
            onUndo: () => {
               const imgs = DawnState.visionState.pendingImages;
               imgs.splice(Math.min(removedIndex, imgs.length), 0, removedImage);
               rebuildPreviews();
               updateCounter();
            },
         });
      }
   }

   /**
    * Rebuild all preview items (after removal)
    */
   function rebuildPreviews() {
      if (!previewContainer) return;

      // Clear existing previews
      previewContainer.innerHTML = '';

      const { pendingImages } = DawnState.visionState;

      if (pendingImages.length === 0) {
         previewContainer.classList.add('hidden');
         return;
      }

      // Re-add all previews with updated indices
      pendingImages.forEach((img, idx) => {
         addPreviewItem(img.previewUrl, idx);
      });
   }

   /**
    * Clear all pending images
    */
   /**
    * Clear all pending images
    * @param {boolean} silent - If true, don't announce (e.g., after sending message)
    */
   function clearImages(silent = false) {
      const hadImages = DawnState.visionState.pendingImages.length > 0;
      DawnState.visionState.pendingImages = [];
      if (previewContainer) {
         previewContainer.innerHTML = '';
         previewContainer.classList.add('hidden');
      }
      updateCounter();

      // Announce only if there were images and not silent
      if (hadImages && !silent) {
         announce('All images removed.');
      }
   }

   /**
    * Update vision limits from server config
    * Called when config response is received with vision_limits
    * @param {Object} limits - Server-provided limits
    * @param {number} limits.max_images - Maximum images per message
    * @param {number} limits.max_image_size - Maximum image size in bytes
    * @param {number} limits.max_dimension - Maximum image dimension in pixels
    * @param {number} limits.max_thumbnail_size - Maximum thumbnail size in bytes
    */
   function updateLimits(limits) {
      if (!limits) return;

      const state = DawnState.visionState;

      if (typeof limits.max_images === 'number' && limits.max_images > 0) {
         state.maxImages = limits.max_images;
      }
      // Support both config.vision (KB) and legacy vision_limits (bytes)
      if (typeof limits.max_image_size_kb === 'number' && limits.max_image_size_kb > 0) {
         state.maxSize = limits.max_image_size_kb * 1024;
      } else if (typeof limits.max_image_size === 'number' && limits.max_image_size > 0) {
         state.maxSize = limits.max_image_size;
      }
      if (typeof limits.max_dimension === 'number' && limits.max_dimension > 0) {
         state.maxDimension = limits.max_dimension;
      }

      // Update counter display with new limits
      updateCounter();

      console.log('Vision limits updated from server:', {
         maxImages: state.maxImages,
         maxSize: state.maxSize,
         maxDimension: state.maxDimension,
      });
   }

   /**
    * Check if vision is supported and update UI accordingly
    * Called when config is received or LLM model changes
    */
   function checkVisionSupport(config) {
      // Check if vision is enabled in config
      // This will be determined by the current model's capabilities
      let visionEnabled = false;

      if (config?.llm?.cloud?.vision_enabled) {
         visionEnabled = true;
      } else if (config?.llm?.local?.vision_enabled) {
         visionEnabled = true;
      }

      // Also check model name for known vision models
      const model = config?.llm?.cloud?.model || config?.llm?.local?.model || '';
      const visionModels = [
         'gpt-4o',
         'gpt-4-vision',
         'gpt-4-turbo',
         'claude-3',
         'gemini',
         'llava',
         'qwen-vl',
         'cogvlm',
      ];
      if (visionModels.some((vm) => model.toLowerCase().includes(vm))) {
         visionEnabled = true;
      }

      DawnState.visionState.visionEnabled = visionEnabled;

      // Update dropdown chevron state (camera access via split-button)
      const dropdownBtn = document.getElementById('attach-dropdown-btn');
      if (dropdownBtn) {
         dropdownBtn.disabled = !visionEnabled;
         dropdownBtn.title = visionEnabled
            ? 'More attach options'
            : 'Camera requires a vision-capable model';
      }

      updateCounter();
   }

   /**
    * Get all image IDs for history storage
    * @deprecated Use getPendingImageIds instead
    * @returns {string[]} Array of image IDs (nulls filtered out)
    */
   function getPendingThumbnails() {
      // Return IDs for backward compatibility
      // Images are now stored server-side and referenced by ID
      return DawnState.visionState.pendingImages.map((img) => img.id).filter((id) => id != null);
   }

   /**
    * Check if there are pending images
    */
   function hasPendingImages() {
      return DawnState.visionState.pendingImages.length > 0;
   }

   /**
    * Get the pending images' server ids: what a turn sends (image_ids)
    * @returns {string[]} Array of image IDs
    */
   function getPendingImageIds() {
      return DawnState.visionState.pendingImages.map((img) => img.id).filter((id) => id);
   }

   /**
    * Format text with image markers for storage (multiple images)
    * Uses image IDs instead of inline data for efficient storage.
    *
    * @param {string} text - User's message text
    * @param {string[]} imageIds - Array of server-assigned image IDs
    * @returns {string} - Formatted message with image markers
    */
   function formatMessageWithImages(text, imageIds) {
      if (!imageIds || imageIds.length === 0) {
         return text;
      }

      // Filter to only valid image IDs
      const validIds = imageIds.filter((id) => isImageId(id));
      if (validIds.length === 0) {
         return text;
      }

      // Append each image as separate marker on new line
      let result = text;
      for (const id of validIds) {
         result += '\n[IMAGE:' + id + ']';
      }
      return result;
   }

   /**
    * Format text with inline data URLs (legacy compatibility)
    * @deprecated Use formatMessageWithImages with image IDs instead
    */
   function formatMessageWithDataUrls(text, dataUrls) {
      if (!dataUrls || dataUrls.length === 0) {
         return text;
      }

      // Filter to only safe data URIs
      const safeUrls = dataUrls.filter((url) => isSafeDataUri(url));
      if (safeUrls.length === 0) {
         return text;
      }

      // Append each image as separate marker on new line
      let result = text;
      for (const url of safeUrls) {
         result += '\n[IMAGE:' + url + ']';
      }
      return result;
   }

   /**
    * Parse image markers from stored message (supports both image IDs and inline data)
    * Supports:
    *   - New format: [IMAGE:img_a1b2c3d4e5f6] (image ID)
    *   - Legacy format: [IMAGE:data:image/...] (inline data URL)
    *
    * @param {string} content - Message content potentially containing image markers
    * @returns {{ text: string, imageIds: string[], imageDataUrls: string[] }}
    */
   function parseImageMarkers(content) {
      if (!content || typeof content !== 'string') {
         return { text: content || '', imageIds: [], imageDataUrls: [] };
      }

      const imageIds = [];
      const imageDataUrls = [];
      let text = content;

      // Find all image markers (both ID and data URL formats)
      // New format: [IMAGE:img_xxx]
      const idRegex = /\n?\[IMAGE:(img_[a-zA-Z0-9]{12})\]/g;
      // Legacy format: [IMAGE:data:image/...]
      const dataRegex = /\n?\[IMAGE:(data:image\/[^\[\]]+)\]/g;

      let match;

      // Extract image IDs
      while ((match = idRegex.exec(content)) !== null) {
         imageIds.push(match[1]);
      }

      // Extract inline data URLs (legacy)
      while ((match = dataRegex.exec(content)) !== null) {
         const dataUrl = match[1];
         // SECURITY: Validate each extracted data URI
         if (isSafeDataUri(dataUrl)) {
            imageDataUrls.push(dataUrl);
         } else {
            console.warn('Invalid image data URI in stored message, ignoring');
         }
      }

      // Remove all markers from text
      text = content.replace(idRegex, '').replace(dataRegex, '').trimEnd();

      return { text, imageIds, imageDataUrls };
   }

   /**
    * Load images from parsed markers (handles both IDs and inline data)
    * Returns array of data URLs ready for rendering.
    *
    * @param {{ imageIds: string[], imageDataUrls: string[] }} parsed - Output from parseImageMarkers
    * @returns {Promise<string[]>} - Array of data URLs
    */
   async function loadParsedImages(parsed) {
      const results = [];

      // Load images by ID from server (with local caching)
      for (const id of parsed.imageIds || []) {
         const dataUrl = await loadImageById(id);
         if (dataUrl) {
            results.push(dataUrl);
         }
      }

      // Include inline data URLs directly (legacy)
      for (const dataUrl of parsed.imageDataUrls || []) {
         results.push(dataUrl);
      }

      return results;
   }

   // Legacy single-image functions for backward compatibility
   function getPendingImage() {
      const images = DawnState.visionState.pendingImages;
      return images.length > 0 ? images[0] : null;
   }

   function getPendingThumbnail() {
      // Return image ID instead of thumbnail (server-side storage)
      const images = DawnState.visionState.pendingImages;
      return images.length > 0 ? images[0]?.id || null : null;
   }

   function hasPendingImage() {
      return DawnState.visionState.pendingImages.length > 0;
   }

   // Legacy wrapper for single image format (accepts either ID or data URL)
   function formatMessageWithImage(text, imageIdOrDataUrl) {
      if (!imageIdOrDataUrl) return text;
      // Check if it's an image ID or a data URL
      if (isImageId(imageIdOrDataUrl)) {
         return formatMessageWithImages(text, [imageIdOrDataUrl]);
      }
      // Legacy data URL path
      return formatMessageWithDataUrls(text, [imageIdOrDataUrl]);
   }

   // Legacy wrapper for single image parse
   function parseImageMarker(content) {
      const { text, imageIds, imageDataUrls } = parseImageMarkers(content);
      return {
         text,
         imageId: imageIds.length > 0 ? imageIds[0] : null,
         imageDataUrl: imageDataUrls.length > 0 ? imageDataUrls[0] : null,
      };
   }

   // Alias for backward compatibility
   const clearImage = clearImages;

   // Export module
   global.DawnVision = {
      init,
      clearImage, // Legacy alias
      clearImages,
      checkVisionSupport,
      updateLimits,
      // Multi-image API
      processMultipleImages,
      getPendingImageIds,
      hasPendingImages,
      formatMessageWithImages,
      parseImageMarkers,
      loadParsedImages,
      removeImage,
      // Image loading
      loadImageById,
      isImageId,
      // Legacy single-image API (backward compatibility)
      getPendingImage,
      getPendingThumbnail,
      hasPendingImage,
      formatMessageWithImage,
      parseImageMarker,
      // Camera capture
      openCamera,
      closeCamera,
      // Utilities
      isSafeDataUri,
      SAFE_PREFIXES,
      IMAGE_ID_REGEX,
   };
})(window);
