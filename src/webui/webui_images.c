#define _GNU_SOURCE /* For memmem, strcasestr */

/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * WebUI Image Handlers
 *
 * Handles HTTP endpoints for image upload/download.
 */

#include "webui/webui_images.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "image_store.h"
#include "logging.h"
#include "utils/string_utils.h"
#include "webui/webui_internal.h"

/* =============================================================================
 * Internal Helpers
 * ============================================================================= */

/**
 * @brief Send JSON error response
 */
static int send_json_error(struct lws *wsi, int status, const char *error) {
   char body[256];
   int body_len = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", error);

   unsigned char buffer[LWS_PRE + 1024];
   unsigned char *start = &buffer[LWS_PRE];
   unsigned char *p = start;
   unsigned char *end = &buffer[sizeof(buffer) - 1];

   if (lws_add_http_header_status(wsi, (unsigned int)status, &p, end))
      return LWS_CLOSE_CONNECTION;
   if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                    (unsigned char *)"application/json", 16, &p, end))
      return LWS_CLOSE_CONNECTION;
   if (lws_add_http_header_content_length(wsi, (unsigned long)body_len, &p, end))
      return LWS_CLOSE_CONNECTION;
   if (webui_add_security_headers(wsi, &p, end))
      return LWS_CLOSE_CONNECTION;
   if (lws_finalize_http_header(wsi, &p, end))
      return LWS_CLOSE_CONNECTION;

   int n = lws_write(wsi, start, (size_t)(p - start), LWS_WRITE_HTTP_HEADERS);
   if (n < 0)
      return LWS_CLOSE_CONNECTION;

   n = lws_write(wsi, (unsigned char *)body, (size_t)body_len, LWS_WRITE_HTTP);
   if (n < 0)
      return LWS_CLOSE_CONNECTION;

   return LWS_CLOSE_CONNECTION; /* Close connection */
}

/**
 * @brief Send JSON success response for upload
 */
static int send_upload_success(struct lws *wsi,
                               const char *id,
                               const char *mime_type,
                               size_t size) {
   char body[512];
   int body_len = snprintf(body, sizeof(body), "{\"id\":\"%s\",\"mime_type\":\"%s\",\"size\":%zu}",
                           id, mime_type, size);

   unsigned char buffer[LWS_PRE + 1024];
   unsigned char *start = &buffer[LWS_PRE];
   unsigned char *p = start;
   unsigned char *end = &buffer[sizeof(buffer) - 1];

   if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end))
      return LWS_CLOSE_CONNECTION;
   if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                    (unsigned char *)"application/json", 16, &p, end))
      return LWS_CLOSE_CONNECTION;
   if (lws_add_http_header_content_length(wsi, (unsigned long)body_len, &p, end))
      return LWS_CLOSE_CONNECTION;
   if (webui_add_security_headers(wsi, &p, end))
      return LWS_CLOSE_CONNECTION;
   if (lws_finalize_http_header(wsi, &p, end))
      return LWS_CLOSE_CONNECTION;

   int n = lws_write(wsi, start, (size_t)(p - start), LWS_WRITE_HTTP_HEADERS);
   if (n < 0)
      return LWS_CLOSE_CONNECTION;

   n = lws_write(wsi, (unsigned char *)body, (size_t)body_len, LWS_WRITE_HTTP);
   if (n < 0)
      return LWS_CLOSE_CONNECTION;

   return LWS_CLOSE_CONNECTION; /* Close connection */
}

/**
 * @brief Validate image magic bytes match declared MIME type
 *
 * Defense-in-depth: ensures uploaded binary actually matches the declared type.
 * Prevents uploading arbitrary data disguised as images.
 */
static bool validate_image_magic(const unsigned char *data, size_t len, const char *mime_type) {
   if (!data || len < 4 || !mime_type)
      return false;

   if (strcmp(mime_type, "image/jpeg") == 0) {
      /* JPEG: FFD8FF */
      return (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF);
   }

   if (strcmp(mime_type, "image/png") == 0) {
      /* PNG: 89504E47 (0x89 P N G) */
      return (len >= 4 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47);
   }

   if (strcmp(mime_type, "image/gif") == 0) {
      /* GIF: GIF87a or GIF89a */
      return (len >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8' &&
              (data[4] == '7' || data[4] == '9') && data[5] == 'a');
   }

   if (strcmp(mime_type, "image/webp") == 0) {
      /* WebP: RIFF....WEBP */
      return (len >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
              data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P');
   }

   return false; /* Unknown type */
}

/**
 * @brief Extract boundary from Content-Type header
 *
 * Example: "multipart/form-data; boundary=----WebKitFormBoundary..."
 */
static bool extract_boundary(const char *content_type, char *boundary, size_t boundary_size) {
   const char *boundary_start = strstr(content_type, "boundary=");
   if (!boundary_start)
      return false;

   boundary_start += 9; /* Skip "boundary=" */

   /* Handle quoted boundary */
   if (*boundary_start == '"') {
      boundary_start++;
      const char *end = strchr(boundary_start, '"');
      if (!end)
         return false;
      size_t len = (size_t)(end - boundary_start);
      if (len >= boundary_size)
         return false;
      memcpy(boundary, boundary_start, len);
      boundary[len] = '\0';
   } else {
      /* Unquoted - ends at whitespace or semicolon */
      size_t i = 0;
      while (boundary_start[i] && boundary_start[i] != ';' && boundary_start[i] != ' ' &&
             boundary_start[i] != '\r' && boundary_start[i] != '\n') {
         if (i >= boundary_size - 1)
            return false;
         boundary[i] = boundary_start[i];
         i++;
      }
      boundary[i] = '\0';
   }

   return strlen(boundary) > 0;
}

/**
 * @brief Parse multipart form data to extract image
 *
 * Finds the image part and extracts its data and content-type.
 */
static bool parse_multipart(http_image_session_t *session,
                            const unsigned char **image_data,
                            size_t *image_len,
                            char *mime_type,
                            size_t mime_size) {
   /* Build full boundary string: "--" + boundary */
   char full_boundary[136];
   snprintf(full_boundary, sizeof(full_boundary), "--%s", session->boundary);
   size_t boundary_len = strlen(full_boundary);

   const unsigned char *data = session->data;
   size_t data_len = session->data_len;

   /* Find first boundary */
   const unsigned char *part_start = (const unsigned char *)memmem(data, data_len, full_boundary,
                                                                   boundary_len);
   if (!part_start)
      return false;

   /* Move past boundary and CRLF */
   part_start += boundary_len;
   if (part_start + 2 > data + data_len)
      return false;
   if (part_start[0] == '\r' && part_start[1] == '\n')
      part_start += 2;

   /* Find part headers end (blank line) */
   const unsigned char *headers_end = (const unsigned char *)memmem(
       part_start, (size_t)(data + data_len - part_start), "\r\n\r\n", 4);
   if (!headers_end)
      return false;

   /* Extract Content-Type from headers */
   const unsigned char *ct_start = (const unsigned char *)strcasestr((const char *)part_start,
                                                                     "Content-Type:");
   if (ct_start && ct_start < headers_end) {
      ct_start += 13; /* Skip "Content-Type:" */
      while (*ct_start == ' ')
         ct_start++;

      const unsigned char *ct_end = ct_start;
      while (ct_end < headers_end && *ct_end != '\r' && *ct_end != '\n')
         ct_end++;

      size_t ct_len = (size_t)(ct_end - ct_start);
      if (ct_len < mime_size) {
         memcpy(mime_type, ct_start, ct_len);
         mime_type[ct_len] = '\0';
      }
   } else {
      /* Default to octet-stream if no Content-Type */
      safe_strncpy(mime_type, "application/octet-stream", mime_size);
   }

   /* Data starts after headers */
   const unsigned char *content_start = headers_end + 4;

   /* Find end boundary */
   char end_boundary[140];
   snprintf(end_boundary, sizeof(end_boundary), "\r\n--%s", session->boundary);
   size_t end_boundary_len = strlen(end_boundary);

   const unsigned char *content_end = (const unsigned char *)memmem(
       content_start, (size_t)(data + data_len - content_start), end_boundary, end_boundary_len);
   if (!content_end)
      return false;

   *image_data = content_start;
   *image_len = (size_t)(content_end - content_start);

   return true;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

void webui_images_session_free(http_image_session_t *session) {
   if (!session)
      return;

   if (session->data) {
      free(session->data);
      session->data = NULL;
   }
   free(session);
}

int webui_images_handle_upload_start(struct lws *wsi,
                                     http_image_session_t **session_out,
                                     int user_id) {
   if (!session_out)
      return LWS_CLOSE_CONNECTION;

   *session_out = NULL;

   /* Check image store is ready */
   if (!image_store_is_ready()) {
      OLOG_ERROR("webui_images: image store not initialized");
      return send_json_error(wsi, HTTP_STATUS_SERVICE_UNAVAILABLE, "Image storage unavailable");
   }

   /* Get Content-Type header */
   char content_type[256];
   int ct_len = lws_hdr_copy(wsi, content_type, sizeof(content_type), WSI_TOKEN_HTTP_CONTENT_TYPE);
   if (ct_len <= 0) {
      return send_json_error(wsi, HTTP_STATUS_BAD_REQUEST, "Missing Content-Type");
   }

   /* Get Content-Length with overflow protection */
   char content_length_str[32];
   int cl_len = lws_hdr_copy(wsi, content_length_str, sizeof(content_length_str),
                             WSI_TOKEN_HTTP_CONTENT_LENGTH);
   /* Snapshot config limit for this upload (avoids TOCTOU if config changes mid-upload) */
   const size_t max_image_size = (size_t)g_config.vision.max_image_size_kb * 1024;

   size_t content_length = 0;
   if (cl_len > 0) {
      char *endptr;
      long long parsed = strtoll(content_length_str, &endptr, 10);
      if (*endptr != '\0' || parsed < 0 || (size_t)parsed > max_image_size) {
         return send_json_error(wsi, HTTP_STATUS_BAD_REQUEST, "Invalid Content-Length");
      }
      content_length = (size_t)parsed;
   }

   /* Check if multipart */
   bool is_multipart = (strstr(content_type, "multipart/form-data") != NULL);

   /* Allocate session */
   http_image_session_t *session = calloc(1, sizeof(http_image_session_t));
   if (!session) {
      return send_json_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Memory allocation failed");
   }

   session->user_id = user_id;
   session->is_multipart = is_multipart;
   session->content_length = content_length;
   session->max_image_size = max_image_size;
   safe_strscpy(session->mime_type, content_type);

   /* Extract boundary for multipart */
   if (is_multipart) {
      if (!extract_boundary(content_type, session->boundary, sizeof(session->boundary))) {
         free(session);
         return send_json_error(wsi, HTTP_STATUS_BAD_REQUEST, "Missing multipart boundary");
      }
   }

   /* Allocate initial data buffer */
   session->data_cap = (content_length > 0 && content_length <= max_image_size)
                           ? content_length
                           : 64 * 1024; /* Start with 64KB */
   session->data = malloc(session->data_cap);
   if (!session->data) {
      free(session);
      return send_json_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Memory allocation failed");
   }

   *session_out = session;
   return 0; /* Continue to body callbacks */
}

int webui_images_handle_upload_body(struct lws *wsi,
                                    http_image_session_t *session,
                                    const void *data,
                                    size_t len) {
   (void)wsi;

   if (!session || !data || len == 0)
      return 0;

   /* Check size limit (using snapshot from upload_start) */
   if (session->data_len + len > session->max_image_size) {
      OLOG_WARNING("webui_images: upload exceeds size limit (%zu + %zu > %zu)", session->data_len,
                   len, session->max_image_size);
      return LWS_CLOSE_CONNECTION;
   }

   /* Grow buffer if needed */
   if (session->data_len + len > session->data_cap) {
      size_t new_cap = session->data_cap * 2;
      if (new_cap < session->data_len + len)
         new_cap = session->data_len + len;
      if (new_cap > session->max_image_size)
         new_cap = session->max_image_size;

      unsigned char *new_data = realloc(session->data, new_cap);
      if (!new_data) {
         OLOG_ERROR("webui_images: failed to grow buffer");
         return LWS_CLOSE_CONNECTION;
      }
      session->data = new_data;
      session->data_cap = new_cap;
   }

   /* Append data */
   memcpy(session->data + session->data_len, data, len);
   session->data_len += len;

   return 0;
}

int webui_images_handle_upload_complete(struct lws *wsi, http_image_session_t *session) {
   if (!session) {
      return send_json_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "No session data");
   }

   const unsigned char *image_data = NULL;
   size_t image_len = 0;
   char mime_type[64] = { 0 };

   if (session->is_multipart) {
      /* Parse multipart to extract image */
      if (!parse_multipart(session, &image_data, &image_len, mime_type, sizeof(mime_type))) {
         webui_images_session_free(session);
         return send_json_error(wsi, HTTP_STATUS_BAD_REQUEST, "Invalid multipart data");
      }
   } else {
      /* Direct binary upload */
      image_data = session->data;
      image_len = session->data_len;
      safe_strscpy(mime_type, session->mime_type);
   }

   /* Validate MIME type */
   if (!image_store_validate_mime(mime_type)) {
      OLOG_WARNING("webui_images: rejected upload with mime type: %s", mime_type);
      webui_images_session_free(session);
      return send_json_error(wsi, HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE, "Unsupported image type");
   }

   /* Validate magic bytes match declared MIME type */
   if (!validate_image_magic(image_data, image_len, mime_type)) {
      OLOG_WARNING("webui_images: magic bytes mismatch for %s", mime_type);
      webui_images_session_free(session);
      return send_json_error(wsi, HTTP_STATUS_BAD_REQUEST,
                             "Image data does not match declared type");
   }

   /* Save image */
   char image_id[IMAGE_ID_LEN];
   int result = image_store_save(session->user_id, image_data, image_len, mime_type, image_id);

   webui_images_session_free(session);

   switch (result) {
      case IMAGE_STORE_SUCCESS:
         OLOG_INFO("webui_images: uploaded %s (%zu bytes, %s)", image_id, image_len, mime_type);
         return send_upload_success(wsi, image_id, mime_type, image_len);

      case IMAGE_STORE_TOO_LARGE:
         return send_json_error(wsi, HTTP_STATUS_REQ_ENTITY_TOO_LARGE, "Image too large");

      case IMAGE_STORE_LIMIT_EXCEEDED:
         return send_json_error(wsi, HTTP_STATUS_FORBIDDEN, "Image limit exceeded");

      case IMAGE_STORE_INVALID:
         return send_json_error(wsi, HTTP_STATUS_BAD_REQUEST, "Invalid image data");

      default:
         return send_json_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Failed to save image");
   }
}

int webui_images_handle_download(struct lws *wsi, const char *image_id, int user_id) {
   /* Validate ID format (prevents path traversal) */
   if (!image_store_validate_id(image_id)) {
      return send_json_error(wsi, HTTP_STATUS_BAD_REQUEST, "Invalid image ID");
   }

   /* Get filesystem path (also does access check and updates last_accessed) */
   char filepath[IMAGE_PATH_MAX];
   char mime_type[IMAGE_MIME_MAX];

   int result = image_store_get_path(image_id, user_id, filepath, mime_type);

   switch (result) {
      case IMAGE_STORE_SUCCESS:
         break; /* Continue below */

      case IMAGE_STORE_NOT_FOUND:
         return send_json_error(wsi, HTTP_STATUS_NOT_FOUND, "Image not found");

      case IMAGE_STORE_FORBIDDEN:
         return send_json_error(wsi, HTTP_STATUS_FORBIDDEN, "Access denied");

      default:
         return send_json_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Failed to load image");
   }

   /* Build extra headers for lws_serve_http_file() */
   char extra_headers[512];
   int hdr_len = snprintf(extra_headers, sizeof(extra_headers),
                          "Cache-Control: private, max-age=31536000\r\n"
                          "X-Content-Type-Options: nosniff\r\n"
                          "Content-Disposition: inline\r\n");

   /* Zero-copy file serving via kernel sendfile */
   int n = lws_serve_http_file(wsi, filepath, mime_type, extra_headers, hdr_len);
   if (n < 0) {
      return send_json_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Failed to serve image");
   }

   /* n == 0 means file is being served asynchronously (completion via LWS_CALLBACK) */
   return 0;
}
