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
 * Shared document text extraction — PDF, DOCX, HTML, plain text
 *
 * Extracted from webui_documents.c so both WebUI upload and the
 * document_index LLM tool can share the same extraction pipeline.
 */

#include "tools/document_extract.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "logging.h"
#include "tools/html_parser.h"

#ifdef HAVE_MUPDF
#include <mupdf/fitz.h>
#endif

#ifdef HAVE_LIBZIP
#include <zip.h>
#endif

#if defined(HAVE_LIBZIP) && defined(HAVE_LIBXML2)
#include <libxml/parser.h>
#include <libxml/tree.h>
#endif

/* @p len cut back to a UTF-8 character boundary of @p s (s[len] is the first
 * byte left out): a character split at a size cap would come back from a
 * browser as U+FFFD, longer than the cap. */
static size_t utf8_boundary(const char *s, size_t len) {
   while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80)
      len--;
   return len;
}

/* =============================================================================
 * Allowed Extensions
 * ============================================================================= */

static const char *allowed_extensions[] = {
   ".txt",  ".md",   ".csv",  ".json", ".c",   ".h",   ".cpp", ".py",  ".js",    ".ts",
   ".sh",   ".toml", ".yaml", ".yml",  ".xml", ".log", ".cfg", ".ini", ".conf",  ".rs",
   ".go",   ".java", ".rb",   ".html", ".htm", ".css", ".sql", ".mk",  ".cmake",
#ifdef HAVE_MUPDF
   ".pdf",
#endif
#if defined(HAVE_LIBZIP) && defined(HAVE_LIBXML2)
   ".docx",
#endif
};

static const size_t allowed_extensions_count = sizeof(allowed_extensions) /
                                               sizeof(allowed_extensions[0]);

/* =============================================================================
 * Content-Type to Extension Mapping
 * ============================================================================= */

typedef struct {
   const char *content_type;
   const char *extension;
} content_type_map_t;

static const content_type_map_t content_type_map[] = {
#ifdef HAVE_MUPDF
   { "application/pdf", ".pdf" },
#endif
#if defined(HAVE_LIBZIP) && defined(HAVE_LIBXML2)
   { "application/vnd.openxmlformats-officedocument.wordprocessingml.document", ".docx" },
#endif
   { "text/html", ".html" },
   { "application/xhtml+xml", ".html" },
   { "text/plain", ".txt" },
   { "text/markdown", ".md" },
   { "text/csv", ".csv" },
   { "application/json", ".json" },
   { "text/xml", ".xml" },
   { "application/xml", ".xml" },
   { "text/yaml", ".yaml" },
   { "application/yaml", ".yaml" },
   { "application/x-yaml", ".yaml" },
};

static const size_t content_type_map_count = sizeof(content_type_map) / sizeof(content_type_map[0]);

/* =============================================================================
 * PDF Extraction (MuPDF)
 * ============================================================================= */

#ifdef HAVE_MUPDF
static int extract_pdf_text(const char *data,
                            size_t data_len,
                            size_t max_extracted,
                            int max_pages,
                            doc_extract_result_t *out) {
   /* Validate magic bytes: PDF starts with %PDF- */
   if (data_len < 5 || memcmp(data, "%PDF-", 5) != 0) {
      OLOG_WARNING("document_extract: PDF magic bytes not found");
      return DOC_EXTRACT_ERROR_CORRUPT;
   }

   fz_context *ctx = fz_new_context(NULL, NULL, DOC_MUPDF_MEM_LIMIT);
   if (!ctx) {
      OLOG_ERROR("document_extract: fz_new_context failed");
      return DOC_EXTRACT_ERROR_ALLOC;
   }

   int result_code = DOC_EXTRACT_SUCCESS;
   char *result = NULL;
   fz_stream *stm = NULL;
   fz_document *doc = NULL;
   fz_buffer *buf = NULL;
   fz_output *output = NULL;

   fz_try(ctx) {
      fz_register_document_handlers(ctx);

      stm = fz_open_memory(ctx, (const unsigned char *)data, data_len);
      doc = fz_open_document_with_stream(ctx, "application/pdf", stm);

      int pages = fz_count_pages(ctx, doc);
      out->page_count = pages;

      if (pages > max_pages)
         pages = max_pages;

      buf = fz_new_buffer(ctx, 4096);
      output = fz_new_output_with_buffer(ctx, buf);

      for (int i = 0; i < pages; i++) {
         fz_stext_page *stext = fz_new_stext_page_from_page_number(ctx, doc, i, NULL);
         fz_print_stext_page_as_text(ctx, output, stext);
         fz_drop_stext_page(ctx, stext);

         size_t current = fz_buffer_storage(ctx, buf, NULL);
         if (current > max_extracted)
            break;
      }

      fz_close_output(ctx, output);
      fz_drop_output(ctx, output);
      output = NULL;

      unsigned char *text_data;
      size_t len = fz_buffer_storage(ctx, buf, &text_data);
      if (len > max_extracted)
         len = utf8_boundary((const char *)text_data, max_extracted);

      result = malloc(len + 1);
      if (result) {
         memcpy(result, text_data, len);
         result[len] = '\0';
         out->text = result;
         out->text_len = len;
      } else {
         result_code = DOC_EXTRACT_ERROR_ALLOC;
      }
   }
   fz_always(ctx) {
      if (output)
         fz_drop_output(ctx, output);
      if (buf)
         fz_drop_buffer(ctx, buf);
      if (doc)
         fz_drop_document(ctx, doc);
      if (stm)
         fz_drop_stream(ctx, stm);
   }
   fz_catch(ctx) {
      OLOG_ERROR("document_extract: PDF extraction failed: %s", fz_caught_message(ctx));
      free(result);
      out->text = NULL;
      out->text_len = 0;
      result_code = DOC_EXTRACT_ERROR_CORRUPT;
   }

   fz_drop_context(ctx);

   if (result_code == DOC_EXTRACT_SUCCESS && out->text_len == 0) {
      free(out->text);
      out->text = NULL;
      result_code = DOC_EXTRACT_ERROR_EMPTY;
   }

   return result_code;
}
#endif /* HAVE_MUPDF */

/* =============================================================================
 * DOCX Extraction (libzip + libxml2)
 * ============================================================================= */

#if defined(HAVE_LIBZIP) && defined(HAVE_LIBXML2)

typedef struct {
   char *buf;
   size_t len;
   size_t cap;
   size_t max_cap;
} text_buf_t;

static void text_buf_append(text_buf_t *tb, const char *str, size_t slen) {
   if (tb->len + slen >= tb->max_cap)
      return;

   if (tb->len + slen >= tb->cap) {
      size_t new_cap = tb->cap * 2;
      if (new_cap < tb->len + slen + 1)
         new_cap = tb->len + slen + 1;
      if (new_cap > tb->max_cap + 1)
         new_cap = tb->max_cap + 1;
      char *new_buf = realloc(tb->buf, new_cap);
      if (!new_buf)
         return;
      tb->buf = new_buf;
      tb->cap = new_cap;
   }
   memcpy(tb->buf + tb->len, str, slen);
   tb->len += slen;
   tb->buf[tb->len] = '\0';
}

static void docx_walk_xml(xmlNode *node, text_buf_t *tb) {
   for (xmlNode *cur = node; cur; cur = cur->next) {
      if (tb->len >= tb->max_cap)
         return;

      if (cur->type == XML_ELEMENT_NODE) {
         const char *name = (const char *)cur->name;

         docx_walk_xml(cur->children, tb);

         if (strcmp(name, "p") == 0) {
            text_buf_append(tb, "\n\n", 2);
         } else if (strcmp(name, "br") == 0) {
            text_buf_append(tb, "\n", 1);
         } else if (strcmp(name, "tab") == 0) {
            text_buf_append(tb, "\t", 1);
         } else if (strcmp(name, "tc") == 0) {
            text_buf_append(tb, "\t", 1);
         } else if (strcmp(name, "tr") == 0) {
            text_buf_append(tb, "\n", 1);
         }
      } else if (cur->type == XML_TEXT_NODE && cur->content) {
         if (cur->parent && cur->parent->name &&
             strcmp((const char *)cur->parent->name, "t") == 0) {
            size_t tlen = strlen((const char *)cur->content);
            text_buf_append(tb, (const char *)cur->content, tlen);
         }
      }
   }
}

static int extract_docx_text(const char *data,
                             size_t data_len,
                             size_t max_extracted,
                             doc_extract_result_t *out) {
   /* Validate magic bytes: ZIP starts with PK\x03\x04 */
   if (data_len < 4 || memcmp(data, "PK\x03\x04", 4) != 0) {
      OLOG_WARNING("document_extract: DOCX magic bytes not found (not a ZIP)");
      return DOC_EXTRACT_ERROR_CORRUPT;
   }

   zip_error_t zerr;
   zip_error_init(&zerr);

   zip_source_t *src = zip_source_buffer_create(data, data_len, 0, &zerr);
   if (!src) {
      OLOG_ERROR("document_extract: zip_source_buffer_create failed: %s",
                 zip_error_strerror(&zerr));
      zip_error_fini(&zerr);
      return DOC_EXTRACT_ERROR_CORRUPT;
   }

   zip_t *za = zip_open_from_source(src, ZIP_RDONLY, &zerr);
   if (!za) {
      OLOG_ERROR("document_extract: zip_open_from_source failed: %s", zip_error_strerror(&zerr));
      zip_source_free(src);
      zip_error_fini(&zerr);
      return DOC_EXTRACT_ERROR_CORRUPT;
   }
   zip_error_fini(&zerr);

   zip_file_t *zf = zip_fopen(za, "word/document.xml", 0);
   if (!zf) {
      OLOG_WARNING("document_extract: word/document.xml not found in DOCX");
      zip_close(za);
      return DOC_EXTRACT_ERROR_CORRUPT;
   }

   /* Read with decompression size limit (ZIP bomb protection) */
   char *xml_buf = malloc(max_extracted + 1);
   if (!xml_buf) {
      zip_fclose(zf);
      zip_close(za);
      return DOC_EXTRACT_ERROR_ALLOC;
   }

   size_t total = 0;
   zip_int64_t n;
   while ((n = zip_fread(zf, xml_buf + total, max_extracted - total)) > 0) {
      total += (size_t)n;
      if (total >= max_extracted)
         break;
   }
   xml_buf[total] = '\0';
   zip_fclose(zf);
   zip_close(za);

   if (total == 0) {
      free(xml_buf);
      return DOC_EXTRACT_ERROR_EMPTY;
   }

   /* Parse XML with full XXE prevention */
   xmlParserCtxtPtr parser = xmlNewParserCtxt();
   if (!parser) {
      free(xml_buf);
      return DOC_EXTRACT_ERROR_ALLOC;
   }

   int flags = XML_PARSE_NONET | XML_PARSE_NOWARNING | XML_PARSE_NOERROR;
   xmlDocPtr xmldoc = xmlCtxtReadMemory(parser, xml_buf, (int)total, NULL, NULL, flags);
   free(xml_buf);

   if (!xmldoc) {
      OLOG_ERROR("document_extract: DOCX XML parse failed");
      xmlFreeParserCtxt(parser);
      return DOC_EXTRACT_ERROR_CORRUPT;
   }

   /* Reject DTDs entirely (legitimate DOCX never has one — XXE prevention) */
   if (xmldoc->intSubset) {
      OLOG_WARNING("document_extract: DOCX XML contains DTD, rejecting (XXE prevention)");
      xmlFreeDoc(xmldoc);
      xmlFreeParserCtxt(parser);
      return DOC_EXTRACT_ERROR_CORRUPT;
   }

   xmlNode *root = xmlDocGetRootElement(xmldoc);
   text_buf_t tb = { .buf = malloc(4096), .len = 0, .cap = 4096, .max_cap = max_extracted };
   if (!tb.buf) {
      xmlFreeDoc(xmldoc);
      xmlFreeParserCtxt(parser);
      return DOC_EXTRACT_ERROR_ALLOC;
   }
   tb.buf[0] = '\0';

   docx_walk_xml(root, &tb);

   xmlFreeDoc(xmldoc);
   xmlFreeParserCtxt(parser);

   if (tb.len == 0) {
      free(tb.buf);
      return DOC_EXTRACT_ERROR_EMPTY;
   }

   out->text = tb.buf;
   out->text_len = tb.len;
   return DOC_EXTRACT_SUCCESS;
}
#endif /* HAVE_LIBZIP && HAVE_LIBXML2 */

/* =============================================================================
 * HTML Extraction
 * ============================================================================= */

static int extract_html_text(const char *data,
                             size_t data_len,
                             size_t max_extracted,
                             doc_extract_result_t *out) {
   /* html_extract_text needs null-terminated input.
    * Avoid copying if already terminated (e.g., curl_buffer always null-terminates). */
   char *html_buf = NULL;
   const char *html_ptr = data;
   if (data[data_len] != '\0') {
      html_buf = malloc(data_len + 1);
      if (!html_buf)
         return DOC_EXTRACT_ERROR_ALLOC;
      memcpy(html_buf, data, data_len);
      html_buf[data_len] = '\0';
      html_ptr = html_buf;
   }

   char *extracted = NULL;
   int rc = html_extract_text(html_ptr, data_len, &extracted);
   free(html_buf);

   if (rc == HTML_PARSE_SUCCESS && extracted) {
      size_t len = strlen(extracted);
      if (len > max_extracted) {
         len = utf8_boundary(extracted, max_extracted);
         extracted[len] = '\0';
      }
      if (len == 0) {
         free(extracted);
         return DOC_EXTRACT_ERROR_EMPTY;
      }
      out->text = extracted;
      out->text_len = len;
      return DOC_EXTRACT_SUCCESS;
   }

   free(extracted);
   /* HTML extraction failed — fall through to plain text in caller */
   return DOC_EXTRACT_ERROR_CORRUPT;
}

/* =============================================================================
 * Plain Text Extraction
 * ============================================================================= */

static int extract_plain_text(const char *data,
                              size_t data_len,
                              size_t max_extracted,
                              doc_extract_result_t *out) {
   size_t len = data_len;
   if (len > max_extracted)
      len = max_extracted;

   if (len == 0)
      return DOC_EXTRACT_ERROR_EMPTY;

   char *text = malloc(len + 1);
   if (!text)
      return DOC_EXTRACT_ERROR_ALLOC;

   memcpy(text, data, len);
   text[len] = '\0';
   out->text = text;
   out->text_len = len;
   return DOC_EXTRACT_SUCCESS;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

bool document_extension_allowed(const char *ext) {
   if (!ext || *ext == '\0')
      return false;

   for (size_t i = 0; i < allowed_extensions_count; i++) {
      if (strcasecmp(ext, allowed_extensions[i]) == 0)
         return true;
   }
   return false;
}

const char *document_get_extension(const char *filename) {
   if (!filename)
      return NULL;

   const char *dot = strrchr(filename, '.');
   if (!dot || dot == filename)
      return NULL;

   return dot;
}

const char *document_extension_from_content_type(const char *content_type) {
   if (!content_type)
      return NULL;

   for (size_t i = 0; i < content_type_map_count; i++) {
      /* Match prefix to handle parameters like "text/html; charset=utf-8" */
      size_t ct_len = strlen(content_type_map[i].content_type);
      if (strncasecmp(content_type, content_type_map[i].content_type, ct_len) == 0) {
         /* Verify next char is end, semicolon, or space (not a partial match) */
         char next = content_type[ct_len];
         if (next == '\0' || next == ';' || next == ' ')
            return content_type_map[i].extension;
      }
   }

   return NULL;
}

const char *document_extract_error_string(int error_code) {
   switch (error_code) {
      case DOC_EXTRACT_SUCCESS:
         return "Success";
      case DOC_EXTRACT_ERROR_UNSUPPORTED:
         return "Unsupported file type";
      case DOC_EXTRACT_ERROR_CORRUPT:
         return "File appears corrupted or invalid";
      case DOC_EXTRACT_ERROR_EMPTY:
         return "No text could be extracted";
      case DOC_EXTRACT_ERROR_TOO_LARGE:
         return "Extracted text exceeds maximum size";
      case DOC_EXTRACT_ERROR_ALLOC:
         return "Memory allocation failed";
      default:
         return "Unknown extraction error";
   }
}

int document_extract_from_buffer(const char *data,
                                 size_t data_len,
                                 const char *extension,
                                 size_t max_extracted_size,
                                 int max_pages,
                                 doc_extract_result_t *out) {
   if (!data || data_len == 0 || !extension || !out)
      return DOC_EXTRACT_ERROR_EMPTY;

   /* Initialize output */
   out->text = NULL;
   out->text_len = 0;
   out->page_count = -1;

   /* Dispatch based on extension */
#ifdef HAVE_MUPDF
   if (strcasecmp(extension, ".pdf") == 0)
      return extract_pdf_text(data, data_len, max_extracted_size, max_pages, out);
#endif

#if defined(HAVE_LIBZIP) && defined(HAVE_LIBXML2)
   if (strcasecmp(extension, ".docx") == 0)
      return extract_docx_text(data, data_len, max_extracted_size, out);
#endif

   if (strcasecmp(extension, ".html") == 0 || strcasecmp(extension, ".htm") == 0) {
      int rc = extract_html_text(data, data_len, max_extracted_size, out);
      if (rc == DOC_EXTRACT_SUCCESS)
         return rc;
      /* HTML extraction failed — fall through to plain text */
   }

   /* All other allowed types: plain text passthrough */
   if (!document_extension_allowed(extension))
      return DOC_EXTRACT_ERROR_UNSUPPORTED;

   return extract_plain_text(data, data_len, max_extracted_size, out);
}
