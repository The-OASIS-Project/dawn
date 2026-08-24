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
 * WebUI Document Upload Handlers
 *
 * Handles POST /api/documents for document upload with text extraction.
 * Supports plain text, PDF (MuPDF), DOCX (libzip + libxml2), and HTML.
 * Uses json-c for safe JSON construction (handles escaping of quotes,
 * backslashes, newlines in document content).
 */

#include "webui/webui_documents.h"

#include <ctype.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config/dawn_config.h"
#include "core/hash_util.h"
#include "document_original_store.h"
#include "logging.h"
#include "tools/document_db.h"
#include "tools/document_extract.h"
#include "tools/tfidf_summarizer.h"
#include "utils/string_utils.h"
#include "webui/webui_internal.h"

/* =============================================================================
 * Session Data (forward-declared in header)
 * ============================================================================= */

struct document_upload_session {
   char boundary[128];
   char filename[256];
   char *content_buf;
   size_t content_len;
   size_t content_cap;
   bool headers_parsed;
   int page_count; /* PDF page count, -1 if not applicable */
   /* Config snapshots captured at upload_start to avoid TOCTOU */
   size_t max_file_size;      /* bytes */
   size_t max_extracted_size; /* bytes */
   int max_pages;
   int user_id;                        /* authenticated uploader (for original storage) */
   char original_blob_id[BLOB_ID_LEN]; /* set when the original was stored (else "") */
};

/* =============================================================================
 * Internal Helpers
 * ============================================================================= */

/**
 * @brief Send JSON error response for document upload (uses json-c for safe escaping)
 */
static int send_doc_error(struct lws *wsi, int status, const char *error) {
   json_object *err_obj = json_object_new_object();
   if (!err_obj) {
      /* Fallback: bare minimum response */
      lws_return_http_status(wsi, (unsigned int)status, error);
      return LWS_CLOSE_CONNECTION;
   }

   json_object_object_add(err_obj, "error", json_object_new_string(error));
   const char *json_str = json_object_to_json_string_ext(err_obj, JSON_C_TO_STRING_PLAIN);
   size_t json_len = strlen(json_str);

   unsigned char buffer[LWS_PRE + 1024];
   unsigned char *start = &buffer[LWS_PRE];
   unsigned char *p = start;
   unsigned char *end = &buffer[sizeof(buffer) - 1];

   if (lws_add_http_header_status(wsi, (unsigned int)status, &p, end) ||
       lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                    (unsigned char *)"application/json", 16, &p, end) ||
       lws_add_http_header_content_length(wsi, (unsigned long)json_len, &p, end) ||
       webui_add_security_headers(wsi, &p, end) || lws_finalize_http_header(wsi, &p, end)) {
      json_object_put(err_obj);
      return LWS_CLOSE_CONNECTION;
   }

   int n = lws_write(wsi, start, (size_t)(p - start), LWS_WRITE_HTTP_HEADERS);
   if (n >= 0)
      lws_write(wsi, (unsigned char *)json_str, json_len, LWS_WRITE_HTTP);

   json_object_put(err_obj);
   return LWS_CLOSE_CONNECTION; /* Close connection */
}

/**
 * @brief Extract boundary from Content-Type header
 */
static bool extract_doc_boundary(const char *content_type, char *boundary, size_t boundary_size) {
   const char *boundary_start = strstr(content_type, "boundary=");
   if (!boundary_start)
      return false;

   boundary_start += 9; /* Skip "boundary=" */

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
 * @brief Extract filename from Content-Disposition header within multipart data
 */
static bool extract_filename(const char *headers,
                             size_t headers_len,
                             char *filename,
                             size_t filename_size) {
   /* Look for filename="..." in the headers */
   const char *fn_start = (const char *)memmem(headers, headers_len, "filename=\"", 10);
   if (!fn_start)
      return false;

   fn_start += 10; /* Skip 'filename="' */
   const char *fn_end = memchr(fn_start, '"', (size_t)(headers + headers_len - fn_start));
   if (!fn_end)
      return false;

   size_t fn_len = (size_t)(fn_end - fn_start);

   /* Check for embedded null bytes (security) */
   if (memchr(fn_start, '\0', fn_len))
      return false;

   /* Strip path components (both Unix and Windows paths) */
   const char *basename = fn_start;
   for (const char *c = fn_start; c < fn_end; c++) {
      if (*c == '/' || *c == '\\')
         basename = c + 1;
   }
   fn_len = (size_t)(fn_end - basename);

   /* Truncate to display size */
   if (fn_len >= filename_size)
      fn_len = filename_size - 1;

   memcpy(filename, basename, fn_len);
   filename[fn_len] = '\0';

   return fn_len > 0;
}

/**
 * @brief Parse multipart form data to extract document content and filename
 */
static bool parse_doc_multipart(document_upload_session_t *session) {
   char full_boundary[136];
   snprintf(full_boundary, sizeof(full_boundary), "--%s", session->boundary);
   size_t boundary_len = strlen(full_boundary);

   const char *data = session->content_buf;
   size_t data_len = session->content_len;

   /* Find first boundary */
   const char *part_start = (const char *)memmem(data, data_len, full_boundary, boundary_len);
   if (!part_start)
      return false;

   /* Move past boundary and CRLF */
   part_start += boundary_len;
   if (part_start + 2 > data + data_len)
      return false;
   if (part_start[0] == '\r' && part_start[1] == '\n')
      part_start += 2;

   /* Find headers end (blank line) */
   const char *headers_end = (const char *)memmem(part_start,
                                                  (size_t)(data + data_len - part_start),
                                                  "\r\n\r\n", 4);
   if (!headers_end)
      return false;

   size_t headers_len = (size_t)(headers_end - part_start);

   /* Extract filename from Content-Disposition */
   extract_filename(part_start, headers_len, session->filename, sizeof(session->filename));

   /* Content starts after headers */
   const char *content_start = headers_end + 4;

   /* Find end boundary */
   char end_boundary[140];
   snprintf(end_boundary, sizeof(end_boundary), "\r\n--%s", session->boundary);
   size_t end_boundary_len = strlen(end_boundary);

   const char *content_end = (const char *)memmem(content_start,
                                                  (size_t)(data + data_len - content_start),
                                                  end_boundary, end_boundary_len);
   if (!content_end)
      return false;

   size_t content_len = (size_t)(content_end - content_start);

   /* Move content to beginning of buffer (overwrite multipart framing) */
   memmove(session->content_buf, content_start, content_len);
   session->content_buf[content_len] = '\0';
   session->content_len = content_len;

   return true;
}

/* =============================================================================
 * Send JSON Success Response
 * ============================================================================= */

/**
 * @brief Send JSON success response with extracted document content
 *
 * Uses json-c for safe JSON construction (handles escaping).
 * Includes estimated_tokens for client-side token budget checks.
 */
/** @brief Map a document extension to a storage MIME (for the blob filename +
 *  download Content-Type).  Leading dot tolerated; unknown -> octet-stream. */
static const char *doc_ext_to_mime(const char *ext) {
   if (!ext) {
      return "application/octet-stream";
   }
   if (ext[0] == '.') {
      ext++;
   }
   if (strcasecmp(ext, "pdf") == 0) {
      return "application/pdf";
   }
   if (strcasecmp(ext, "docx") == 0) {
      return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
   }
   if (strcasecmp(ext, "doc") == 0) {
      return "application/msword";
   }
   if (strcasecmp(ext, "txt") == 0) {
      return "text/plain";
   }
   if (strcasecmp(ext, "md") == 0 || strcasecmp(ext, "markdown") == 0) {
      return "text/markdown";
   }
   if (strcasecmp(ext, "rtf") == 0) {
      return "application/rtf";
   }
   return "application/octet-stream";
}

static int send_doc_success(struct lws *wsi, document_upload_session_t *session, const char *ext) {
   /* Build JSON response using json-c */
   json_object *resp = json_object_new_object();
   if (!resp) {
      OLOG_ERROR("webui_documents: json_object_new_object failed");
      return send_doc_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "JSON construction failed");
   }

   json_object_object_add(resp, "success", json_object_new_boolean(1));
   json_object_object_add(resp, "filename", json_object_new_string(session->filename));

   /* Capture size before freeing buffer */
   size_t content_size = session->content_len;

   /* Token estimate: chars / 4 heuristic (matches llm_context_estimate_tokens) */
   int estimated_tokens = (int)(content_size / 4);

   /* Create content string object, then free raw buffer to reduce peak memory */
   json_object *content_obj = json_object_new_string(session->content_buf);
   free(session->content_buf);
   session->content_buf = NULL;
   session->content_len = 0;
   json_object_object_add(resp, "content", content_obj);

   json_object_object_add(resp, "size", json_object_new_int64((int64_t)content_size));
   json_object_object_add(resp, "type", json_object_new_string(ext ? ext : ""));
   json_object_object_add(resp, "estimated_tokens", json_object_new_int(estimated_tokens));

   /* Original-file blob id (v68) — the browser echoes it into the index message
    * so the stored conversation can offer the real file (Phase 6). */
   if (session->original_blob_id[0]) {
      json_object_object_add(resp, "original_blob_id",
                             json_object_new_string(session->original_blob_id));
   }

   /* Include page count for PDF files */
   if (session->page_count > 0) {
      json_object_object_add(resp, "page_count", json_object_new_int(session->page_count));
   }

   const char *json_str = json_object_to_json_string_ext(resp, JSON_C_TO_STRING_PLAIN);
   size_t json_len = strlen(json_str);

   /* Allocate response buffer for headers only (body written from json_str directly) */
   size_t buf_size = LWS_PRE + 1024;
   unsigned char *buffer = malloc(buf_size);
   if (!buffer) {
      json_object_put(resp);
      return send_doc_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Memory allocation failed");
   }

   unsigned char *start = &buffer[LWS_PRE];
   unsigned char *p = start;
   unsigned char *end = &buffer[buf_size - 1];

   if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end)) {
      free(buffer);
      json_object_put(resp);
      return LWS_CLOSE_CONNECTION;
   }
   if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                    (unsigned char *)"application/json", 16, &p, end)) {
      free(buffer);
      json_object_put(resp);
      return LWS_CLOSE_CONNECTION;
   }
   if (lws_add_http_header_content_length(wsi, (unsigned long)json_len, &p, end)) {
      free(buffer);
      json_object_put(resp);
      return LWS_CLOSE_CONNECTION;
   }
   if (webui_add_security_headers(wsi, &p, end)) {
      free(buffer);
      json_object_put(resp);
      return LWS_CLOSE_CONNECTION;
   }
   if (lws_finalize_http_header(wsi, &p, end)) {
      free(buffer);
      json_object_put(resp);
      return LWS_CLOSE_CONNECTION;
   }

   /* Write headers */
   int n = lws_write(wsi, start, (size_t)(p - start), LWS_WRITE_HTTP_HEADERS);
   if (n < 0) {
      free(buffer);
      json_object_put(resp);
      return LWS_CLOSE_CONNECTION;
   }

   /* Write body */
   n = lws_write(wsi, (unsigned char *)json_str, json_len, LWS_WRITE_HTTP);

   free(buffer);
   json_object_put(resp); /* Frees json-c internal buffers including json_str */

   if (n < 0)
      return LWS_CLOSE_CONNECTION;

   return LWS_CLOSE_CONNECTION; /* Close connection */
}

/**
 * @brief Send a JSON response for the summarize endpoint
 */
static int send_doc_json_response(struct lws *wsi, int status, json_object *resp) {
   const char *json_str = json_object_to_json_string_ext(resp, JSON_C_TO_STRING_PLAIN);
   size_t json_len = strlen(json_str);

   size_t buf_size = LWS_PRE + 1024;
   unsigned char *buffer = malloc(buf_size);
   if (!buffer) {
      json_object_put(resp);
      return LWS_CLOSE_CONNECTION;
   }

   unsigned char *start = &buffer[LWS_PRE];
   unsigned char *p = start;
   unsigned char *end = &buffer[buf_size - 1];

   if (lws_add_http_header_status(wsi, (unsigned int)status, &p, end) ||
       lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                    (unsigned char *)"application/json", 16, &p, end) ||
       lws_add_http_header_content_length(wsi, (unsigned long)json_len, &p, end) ||
       webui_add_security_headers(wsi, &p, end) || lws_finalize_http_header(wsi, &p, end)) {
      free(buffer);
      json_object_put(resp);
      return LWS_CLOSE_CONNECTION;
   }

   int n = lws_write(wsi, start, (size_t)(p - start), LWS_WRITE_HTTP_HEADERS);
   if (n >= 0)
      lws_write(wsi, (unsigned char *)json_str, json_len, LWS_WRITE_HTTP);

   free(buffer);
   json_object_put(resp);
   return LWS_CLOSE_CONNECTION; /* Close connection */
}

/* =============================================================================
 * Public API
 * ============================================================================= */

void webui_documents_session_free(document_upload_session_t *session) {
   if (!session)
      return;

   if (session->content_buf) {
      free(session->content_buf);
      session->content_buf = NULL;
   }
   free(session);
}

int webui_documents_handle_upload_start(struct lws *wsi,
                                        document_upload_session_t **session_out,
                                        int user_id) {
   if (!session_out)
      return LWS_CLOSE_CONNECTION;

   *session_out = NULL;

   /* Get Content-Type header */
   char content_type[256];
   int ct_len = lws_hdr_copy(wsi, content_type, sizeof(content_type), WSI_TOKEN_HTTP_CONTENT_TYPE);
   if (ct_len <= 0) {
      return send_doc_error(wsi, HTTP_STATUS_BAD_REQUEST, "Missing Content-Type");
   }

   /* Must be multipart/form-data */
   if (!strstr(content_type, "multipart/form-data")) {
      return send_doc_error(wsi, HTTP_STATUS_BAD_REQUEST, "Expected multipart/form-data");
   }

   /* Get Content-Length with overflow protection */
   char content_length_str[32];
   int cl_len = lws_hdr_copy(wsi, content_length_str, sizeof(content_length_str),
                             WSI_TOKEN_HTTP_CONTENT_LENGTH);
   /* Snapshot config limits for this upload (avoids TOCTOU if config changes mid-upload) */
   const size_t max_file_size = (size_t)g_config.documents.max_file_size_kb * 1024;

   size_t content_length = 0;
   if (cl_len > 0) {
      char *endptr;
      long long parsed = strtoll(content_length_str, &endptr, 10);
      if (*endptr != '\0' || parsed < 0 || (size_t)parsed > max_file_size + 1024) {
         char err_msg[64];
         snprintf(err_msg, sizeof(err_msg), "File too large (max %d KB)",
                  g_config.documents.max_file_size_kb);
         return send_doc_error(wsi, HTTP_STATUS_REQ_ENTITY_TOO_LARGE, err_msg);
      }
      content_length = (size_t)parsed;
   }

   /* Allocate session */
   document_upload_session_t *session = calloc(1, sizeof(document_upload_session_t));
   if (!session) {
      return send_doc_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Memory allocation failed");
   }

   session->page_count = -1; /* Not applicable by default */
   session->max_file_size = max_file_size;
   session->max_extracted_size = (size_t)g_config.documents.max_extracted_size_kb * 1024;
   session->max_pages = g_config.documents.max_pages;
   session->user_id = user_id;

   /* Extract boundary */
   if (!extract_doc_boundary(content_type, session->boundary, sizeof(session->boundary))) {
      free(session);
      return send_doc_error(wsi, HTTP_STATUS_BAD_REQUEST, "Missing multipart boundary");
   }

   /* Allocate initial data buffer */
   session->content_cap = (content_length > 0 && content_length <= max_file_size + 1024)
                              ? content_length
                              : 32 * 1024; /* Start with 32KB */
   session->content_buf = malloc(session->content_cap);
   if (!session->content_buf) {
      free(session);
      return send_doc_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Memory allocation failed");
   }

   *session_out = session;
   return 0; /* Continue to body callbacks */
}

int webui_documents_handle_upload_body(struct lws *wsi,
                                       document_upload_session_t *session,
                                       const char *data,
                                       size_t len) {
   (void)wsi;

   if (!session || !data || len == 0)
      return 0;

   /* Check size limit (content + multipart overhead) */
   if (session->content_len + len > session->max_file_size + 1024) {
      OLOG_WARNING("webui_documents: upload exceeds size limit (%zu + %zu)", session->content_len,
                   len);
      return LWS_CLOSE_CONNECTION;
   }

   /* Grow buffer if needed */
   if (session->content_len + len > session->content_cap) {
      size_t new_cap = session->content_cap * 2;
      if (new_cap < session->content_len + len)
         new_cap = session->content_len + len;
      if (new_cap > session->max_file_size + 1024)
         new_cap = session->max_file_size + 1024;

      char *new_buf = realloc(session->content_buf, new_cap);
      if (!new_buf) {
         OLOG_ERROR("webui_documents: failed to grow buffer");
         return LWS_CLOSE_CONNECTION;
      }
      session->content_buf = new_buf;
      session->content_cap = new_cap;
   }

   /* Append data */
   memcpy(session->content_buf + session->content_len, data, len);
   session->content_len += len;

   return 0;
}

int webui_documents_handle_upload_complete(struct lws *wsi, document_upload_session_t *session) {
   if (!session) {
      return send_doc_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "No session data");
   }

   /* Parse multipart to extract content and filename */
   if (!parse_doc_multipart(session)) {
      webui_documents_session_free(session);
      return send_doc_error(wsi, HTTP_STATUS_BAD_REQUEST, "Invalid multipart data");
   }

   /* Validate filename has an allowed extension */
   const char *ext = document_get_extension(session->filename);
   if (!document_extension_allowed(ext)) {
      OLOG_WARNING("webui_documents: rejected upload with extension: %s (file: %s)",
                   ext ? ext : "(none)", session->filename);
      webui_documents_session_free(session);
      return send_doc_error(wsi, HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE, "Unsupported file type");
   }

   /* Check raw content size */
   if (session->content_len > session->max_file_size) {
      char err_msg[64];
      snprintf(err_msg, sizeof(err_msg), "File too large (max %zu KB)",
               session->max_file_size / 1024);
      webui_documents_session_free(session);
      return send_doc_error(wsi, HTTP_STATUS_REQ_ENTITY_TOO_LARGE, err_msg);
   }

   /* Extract text using shared document extraction module */
   doc_extract_result_t extract = { 0 };
   int extract_rc = document_extract_from_buffer(session->content_buf, session->content_len, ext,
                                                 session->max_extracted_size, session->max_pages,
                                                 &extract);
   if (extract_rc != DOC_EXTRACT_SUCCESS) {
      const char *err_msg = document_extract_error_string(extract_rc);
      OLOG_WARNING("webui_documents: extraction failed for %s: %s", session->filename, err_msg);
      webui_documents_session_free(session);
      return send_doc_error(wsi, 422, err_msg);
   }

   /* Store the ORIGINAL upload bytes here — they're valid only until the free
    * below.  Only on the post-extraction-success path (so we never store an
    * unvalidated blob).  Best-effort: a storage failure must not fail the upload,
    * since the extracted text is the primary product. */
   session->original_blob_id[0] = '\0';
   if (g_config.documents.originals_enabled && document_originals_ready() && session->user_id > 0 &&
       session->content_len > 0) {
      size_t max_orig = (size_t)g_config.documents.max_original_size_mb * 1024 * 1024;
      if (session->content_len <= max_orig) {
         char content_hash[DAWN_SHA256_HEX_LEN];
         dawn_sha256_hex(session->content_buf, session->content_len, content_hash);
         char blob_id[BLOB_ID_LEN];
         int srv = document_original_save(session->user_id, session->content_buf,
                                          session->content_len, doc_ext_to_mime(ext), content_hash,
                                          session->filename, blob_id);
         if (srv == BLOB_STORE_SUCCESS) {
            snprintf(session->original_blob_id, sizeof(session->original_blob_id), "%s", blob_id);
         } else {
            OLOG_WARNING("webui_documents: original not stored for %s (rc=%d)", session->filename,
                         srv);
         }
      } else {
         OLOG_INFO("webui_documents: original for %s exceeds max_original_size_mb; not stored",
                   session->filename);
      }
   }

   /* Replace raw content with extracted text */
   free(session->content_buf);
   session->content_buf = extract.text;
   session->content_len = extract.text_len;
   session->content_cap = extract.text_len + 1;
   if (extract.page_count >= 0)
      session->page_count = extract.page_count;

   /* Null-terminate for string operations — ensure capacity for the trailing NUL */
   if (session->content_len >= session->content_cap) {
      size_t new_cap = session->content_len + 1;
      char *new_buf = realloc(session->content_buf, new_cap);
      if (!new_buf) {
         webui_documents_session_free(session);
         return send_doc_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Memory allocation failed");
      }
      session->content_buf = new_buf;
      session->content_cap = new_cap;
   }
   session->content_buf[session->content_len] = '\0';

   /* Sanitize UTF-8 for JSON safety */
   sanitize_utf8_for_json(session->content_buf);
   session->content_len = strlen(session->content_buf);

   OLOG_INFO("webui_documents: uploaded %s (%zu bytes, %s)", session->filename,
             session->content_len, ext);

   /* Send JSON response with extracted content */
   int result = send_doc_success(wsi, session, ext);

   webui_documents_session_free(session);
   return result;
}

/* =============================================================================
 * Summarize Endpoint
 * ============================================================================= */

int webui_documents_handle_summarize(struct lws *wsi, const char *body, size_t body_len) {
   if (!body || body_len == 0) {
      return send_doc_error(wsi, HTTP_STATUS_BAD_REQUEST, "Empty request body");
   }

   /* Parse JSON body */
   json_object *req = json_tokener_parse(body);
   if (!req) {
      return send_doc_error(wsi, HTTP_STATUS_BAD_REQUEST, "Invalid JSON");
   }

   json_object *content_obj = NULL;
   json_object *target_obj = NULL;

   if (!json_object_object_get_ex(req, "content", &content_obj) ||
       !json_object_object_get_ex(req, "target_tokens", &target_obj)) {
      json_object_put(req);
      return send_doc_error(wsi, HTTP_STATUS_BAD_REQUEST, "Missing content or target_tokens field");
   }

   const char *content = json_object_get_string(content_obj);
   int target_tokens = json_object_get_int(target_obj);

   if (!content || target_tokens <= 0) {
      json_object_put(req);
      return send_doc_error(wsi, HTTP_STATUS_BAD_REQUEST, "Invalid content or target_tokens");
   }

   /* Calculate keep ratio */
   int current_tokens = (int)(strlen(content) / 4);
   float keep_ratio = (current_tokens > 0) ? (float)target_tokens / (float)current_tokens : 1.0f;
   if (keep_ratio >= 1.0f)
      keep_ratio = 1.0f;
   if (keep_ratio < 0.05f)
      keep_ratio = 0.05f; /* Floor at 5% */

   /* Run TF-IDF summarization */
   char *summary = NULL;
   int rc = tfidf_summarize(content, &summary, keep_ratio, TFIDF_MIN_SENTENCES);

   json_object_put(req); /* Done with input */

   if (rc != TFIDF_SUCCESS || !summary) {
      free(summary);
      return send_doc_error(wsi, 422, "Summarization failed");
   }

   /* Build response */
   json_object *resp = json_object_new_object();
   if (!resp) {
      free(summary);
      return send_doc_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "JSON construction failed");
   }

   size_t summary_len = strlen(summary);
   int new_estimated_tokens = (int)(summary_len / 4);

   json_object_object_add(resp, "success", json_object_new_boolean(1));
   json_object_object_add(resp, "content", json_object_new_string(summary));
   json_object_object_add(resp, "size", json_object_new_int64((int64_t)summary_len));
   json_object_object_add(resp, "estimated_tokens", json_object_new_int(new_estimated_tokens));

   free(summary);
   return send_doc_json_response(wsi, HTTP_STATUS_OK, resp);
}

/* =============================================================================
 * Original-file download (v68)
 * ============================================================================= */

/** @brief Sanitize an original filename for a Content-Disposition header value:
 *  keep only [A-Za-z0-9._-] (defeats CR/LF/quote header injection). */
static void sanitize_disposition_filename(const char *in, char *out, size_t out_size) {
   size_t j = 0;
   for (size_t i = 0; in && in[i] && j + 1 < out_size; i++) {
      char c = in[i];
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
          c == '_' || c == '-') {
         out[j++] = c;
      }
   }
   if (j == 0) {
      snprintf(out, out_size, "download");
      return;
   }
   out[j] = '\0';
}

int webui_documents_handle_original_download(struct lws *wsi, const char *blob_id, int user_id) {
   if (!document_original_validate_id(blob_id)) {
      return send_doc_error(wsi, HTTP_STATUS_BAD_REQUEST, "Invalid document ID");
   }

   /* Resolve the effective reader: the owner reads their own original, and any user
    * reads a global doc's original (honoring is_global the same way the full-text
    * reader and library list do — the documents layer owns that decision since the
    * blob store is owner-only and global-blind).  On no referencing row, fall back to
    * the requester so the blob store still enforces owner-only. */
   int reader_id = user_id;
   int owner_id = 0;
   if (document_db_original_blob_reader(blob_id, user_id, &owner_id) == SUCCESS)
      reader_id = owner_id;

   char filepath[BLOB_PATH_MAX];
   char mime_type[BLOB_MIME_MAX];
   int result = document_original_get_path(blob_id, reader_id, filepath, mime_type);
   switch (result) {
      case BLOB_STORE_SUCCESS:
         break;
      case BLOB_STORE_NOT_FOUND:
         return send_doc_error(wsi, HTTP_STATUS_NOT_FOUND, "Document not found");
      case BLOB_STORE_FORBIDDEN:
         return send_doc_error(wsi, HTTP_STATUS_FORBIDDEN, "Access denied");
      default:
         return send_doc_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Failed to load document");
   }

   char orig_name[BLOB_FILENAME_ORIGINAL_MAX];
   if (document_original_get_filename(blob_id, reader_id, orig_name, sizeof(orig_name)) !=
           BLOB_STORE_SUCCESS ||
       orig_name[0] == '\0') {
      snprintf(orig_name, sizeof(orig_name), "document");
   }
   char safe_name[BLOB_FILENAME_ORIGINAL_MAX];
   sanitize_disposition_filename(orig_name, safe_name, sizeof(safe_name));

   /* Always an attachment + nosniff — original docs are never rendered inline. */
   char extra_headers[512];
   int hdr_len = snprintf(extra_headers, sizeof(extra_headers),
                          "Cache-Control: private, max-age=31536000\r\n"
                          "X-Content-Type-Options: nosniff\r\n"
                          "Content-Disposition: attachment; filename=\"%s\"\r\n",
                          safe_name);

   int n = lws_serve_http_file(wsi, filepath, mime_type, extra_headers, hdr_len);
   if (n < 0) {
      return send_doc_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Failed to serve document");
   }
   return 0; /* async serve completes via LWS callback */
}
