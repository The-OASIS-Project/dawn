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
 * CalDAV protocol client — RFC 4791 calendar operations via libcurl + libxml2.
 * 3-step RFC-compliant discovery, REPORT queries, PUT/DELETE mutations.
 */

#ifdef DAWN_ENABLE_CALENDAR_TOOL

#include "tools/caldav_client.h"

#include <ctype.h>
#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/curl_buffer.h"
#include "logging.h"

#ifdef HAVE_LIBICAL
#include <libical/ical.h>
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

#define CALDAV_TIMEOUT_SEC 30
#define CALDAV_MAX_RESPONSE_SIZE (4 * 1024 * 1024) /* 4 MB */

/* No network fetches from inside the XML; a malformed response is an error
 * return, not lines on stderr (the server is whoever the account points at). */
#define CALDAV_XML_FLAGS \
   (XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_NOERROR | XML_PARSE_NOWARNING)

/* XML namespace URIs */
#define NS_DAV "DAV:"
#define NS_CALDAV "urn:ietf:params:xml:ns:caldav"
#define NS_CS "http://calendarserver.org/ns/"
#define NS_APPLE "http://apple.com/ns/ical/"

/* ============================================================================
 * Internal: curl response buffer
 *
 * Uses the shared curl_buffer_t (include/core/curl_buffer.h) with a 4 MB
 * cap on CalDAV responses (large calendars with many events).  The
 * shared helper truncates with a flag instead of aborting mid-transfer
 * — callers MUST check `resp.truncated` after curl_easy_perform and
 * reject the response.
 * ============================================================================ */

/** Capture ETag from response headers */
typedef struct {
   char etag[128];
} etag_capture_t;

static size_t header_etag_cb(char *buf, size_t size, size_t nmemb, void *userdata) {
   size_t total = size * nmemb;
   etag_capture_t *cap = userdata;

   /* Look for "ETag:" header (case-insensitive) */
   if (total > 5 && strncasecmp(buf, "ETag:", 5) == 0) {
      const char *val = buf + 5;
      while (*val == ' ' || *val == '\t')
         val++;
      /* Strip surrounding quotes and trailing whitespace */
      size_t vlen = total - (size_t)(val - buf);
      while (vlen > 0 && (val[vlen - 1] == '\r' || val[vlen - 1] == '\n' || val[vlen - 1] == ' '))
         vlen--;
      if (vlen >= 2 && val[0] == '"' && val[vlen - 1] == '"') {
         val++;
         vlen -= 2;
      }
      if (vlen > 0 && vlen < sizeof(cap->etag))
         snprintf(cap->etag, sizeof(cap->etag), "%.*s", (int)vlen, val);
   }
   return total;
}

/* ============================================================================
 * Internal: curl setup helpers
 * ============================================================================ */

static CURL *caldav_curl_init(const caldav_auth_t *auth, curl_buffer_t *resp) {
   CURL *curl = curl_easy_init();
   if (!curl)
      return NULL;

   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)CALDAV_TIMEOUT_SEC);
   /* Disable redirects to prevent credential leakage to third-party hosts */
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
   /* SSRF protection: restrict to HTTPS only (CalDAV servers must use TLS) */
   DAWN_CURL_SET_PROTOCOLS(curl, "https");

   if (auth && auth->bearer_token) {
      curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BEARER);
      curl_easy_setopt(curl, CURLOPT_XOAUTH2_BEARER, auth->bearer_token);
   } else if (auth && auth->username && auth->password) {
      curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
      curl_easy_setopt(curl, CURLOPT_USERNAME, auth->username);
      curl_easy_setopt(curl, CURLOPT_PASSWORD, auth->password);
   }

   return curl;
}

static caldav_error_t http_status_to_error(long status) {
   if (status >= 200 && status <= 207)
      return CALDAV_OK;
   if (status == 401)
      return CALDAV_ERR_AUTH;
   if (status == 403)
      return CALDAV_ERR_FORBIDDEN;
   if (status == 404)
      return CALDAV_ERR_NOT_FOUND;
   if (status == 412)
      return CALDAV_ERR_CONFLICT;
   if (status >= 500)
      return CALDAV_ERR_SERVER;
   return CALDAV_ERR_NETWORK;
}

/* ============================================================================
 * Internal: PROPFIND helper
 * ============================================================================ */

static caldav_error_t do_propfind(CURL *curl,
                                  const char *url,
                                  const char *depth,
                                  const char *xml_body,
                                  curl_buffer_t *resp) {
   curl_buffer_reset(resp);

   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PROPFIND");
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, xml_body);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(xml_body));

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Content-Type: application/xml; charset=utf-8");
   char depth_hdr[32];
   snprintf(depth_hdr, sizeof(depth_hdr), "Depth: %s", depth);
   headers = curl_slist_append(headers, depth_hdr);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

   CURLcode res = curl_easy_perform(curl);
   curl_slist_free_all(headers);

   if (res != CURLE_OK) {
      OLOG_ERROR("caldav: PROPFIND %s failed: %s", url, curl_easy_strerror(res));
      return CALDAV_ERR_NETWORK;
   }

   if (resp->truncated) {
      OLOG_ERROR("caldav: PROPFIND %s response exceeded %d byte cap; rejecting", url,
                 CALDAV_MAX_RESPONSE_SIZE);
      return CALDAV_ERR_NETWORK;
   }

   long status = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
   if (status >= 300) {
      OLOG_WARNING("caldav: PROPFIND %s Depth:%s -> HTTP %ld", url, depth, status);
      if (resp->data && resp->size > 0) {
         char snippet[201];
         size_t n = resp->size < 200 ? resp->size : 200;
         memcpy(snippet, resp->data, n);
         snippet[n] = '\0';
         OLOG_WARNING("caldav: response: %.200s", snippet);
      }
   }
   return http_status_to_error(status);
}

/* ============================================================================
 * Internal: URL resolution
 *
 * CalDAV servers return relative, absolute-path, or full URLs in href
 * responses. This handles all three cases.
 * ============================================================================ */

static int resolve_href(const char *base_url, const char *href, char *out, size_t out_len) {
   if (!href || !href[0]) {
      out[0] = '\0';
      return 1;
   }

   /* Case 1: Full URL (starts with http:// or https://) */
   if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) {
      snprintf(out, out_len, "%s", href);
      return 0;
   }

   /* Extract scheme + host from base_url */
   const char *scheme_end = strstr(base_url, "://");
   if (!scheme_end) {
      snprintf(out, out_len, "%s", href);
      return 1;
   }
   const char *host_start = scheme_end + 3;
   const char *host_end = strchr(host_start, '/');
   size_t base_prefix_len;
   if (host_end) {
      base_prefix_len = (size_t)(host_end - base_url);
   } else {
      base_prefix_len = strlen(base_url);
   }

   /* Case 2: Absolute path (starts with /) */
   if (href[0] == '/') {
      snprintf(out, out_len, "%.*s%s", (int)base_prefix_len, base_url, href);
      return 0;
   }

   /* Case 3: Relative path — append to base URL's directory */
   const char *last_slash = strrchr(base_url, '/');
   if (last_slash && last_slash > scheme_end + 2) {
      size_t dir_len = (size_t)(last_slash - base_url) + 1;
      snprintf(out, out_len, "%.*s%s", (int)dir_len, base_url, href);
   } else {
      snprintf(out, out_len, "%s/%s", base_url, href);
   }
   return 0;
}

/* ============================================================================
 * Internal: XML parsing helpers
 * ============================================================================ */

/** Register standard CalDAV namespaces for XPath */
static void register_namespaces(xmlXPathContextPtr ctx) {
   xmlXPathRegisterNs(ctx, (const xmlChar *)"d", (const xmlChar *)NS_DAV);
   xmlXPathRegisterNs(ctx, (const xmlChar *)"c", (const xmlChar *)NS_CALDAV);
   xmlXPathRegisterNs(ctx, (const xmlChar *)"cs", (const xmlChar *)NS_CS);
   xmlXPathRegisterNs(ctx, (const xmlChar *)"a", (const xmlChar *)NS_APPLE);
}

/** Extract text content from first XPath match, relative to a context node */
static int xpath_text(xmlXPathContextPtr ctx,
                      xmlNodePtr node,
                      const char *expr,
                      char *out,
                      size_t out_len) {
   xmlNodePtr saved = ctx->node;
   ctx->node = node;

   xmlXPathObjectPtr result = xmlXPathEvalExpression((const xmlChar *)expr, ctx);
   ctx->node = saved;

   if (!result)
      return 1;

   if (result->nodesetval && result->nodesetval->nodeNr > 0) {
      xmlNodePtr n = result->nodesetval->nodeTab[0];
      xmlChar *content = xmlNodeGetContent(n);
      if (content) {
         snprintf(out, out_len, "%s", (const char *)content);
         xmlFree(content);
         xmlXPathFreeObject(result);
         return 0;
      }
   }
   xmlXPathFreeObject(result);
   return 1;
}

/* ============================================================================
 * Discovery Step 1: Find current-user-principal (RFC 5397)
 * ============================================================================ */

static const char *PROPFIND_PRINCIPAL_XML = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                                            "<d:propfind xmlns:d=\"DAV:\">"
                                            "<d:prop>"
                                            "<d:current-user-principal/>"
                                            "<d:resourcetype/>"
                                            "</d:prop>"
                                            "</d:propfind>";

static caldav_error_t discover_principal(CURL *curl,
                                         const char *base_url,
                                         curl_buffer_t *resp,
                                         char *principal_out,
                                         size_t principal_len) {
   principal_out[0] = '\0';

   caldav_error_t err = do_propfind(curl, base_url, "0", PROPFIND_PRINCIPAL_XML, resp);
   if (err != CALDAV_OK)
      return err;

   xmlDocPtr doc = xmlReadMemory(resp->data, (int)resp->size, NULL, NULL, CALDAV_XML_FLAGS);
   if (!doc)
      return CALDAV_ERR_PARSE;

   xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
   if (!ctx) {
      xmlFreeDoc(doc);
      return CALDAV_ERR_ALLOC;
   }
   register_namespaces(ctx);

   /* Look for current-user-principal/href */
   char href[512] = { 0 };
   xpath_text(ctx, xmlDocGetRootElement(doc), "//d:current-user-principal/d:href", href,
              sizeof(href));

   xmlXPathFreeContext(ctx);
   xmlFreeDoc(doc);

   if (href[0]) {
      resolve_href(base_url, href, principal_out, principal_len);
      OLOG_INFO("caldav: principal URL: %s", principal_out);
   }
   return CALDAV_OK;
}

/* ============================================================================
 * Discovery Step 2: Find calendar-home-set (RFC 4791 §6.2.1)
 * ============================================================================ */

static const char *PROPFIND_CALHOME_XML =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
    "<d:prop>"
    "<c:calendar-home-set/>"
    "</d:prop>"
    "</d:propfind>";

static caldav_error_t discover_calendar_home(CURL *curl,
                                             const char *principal_url,
                                             curl_buffer_t *resp,
                                             char *home_out,
                                             size_t home_len) {
   home_out[0] = '\0';

   caldav_error_t err = do_propfind(curl, principal_url, "0", PROPFIND_CALHOME_XML, resp);
   if (err != CALDAV_OK)
      return err;

   xmlDocPtr doc = xmlReadMemory(resp->data, (int)resp->size, NULL, NULL, CALDAV_XML_FLAGS);
   if (!doc)
      return CALDAV_ERR_PARSE;

   xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
   if (!ctx) {
      xmlFreeDoc(doc);
      return CALDAV_ERR_ALLOC;
   }
   register_namespaces(ctx);

   char href[512] = { 0 };
   xpath_text(ctx, xmlDocGetRootElement(doc), "//c:calendar-home-set/d:href", href, sizeof(href));

   xmlXPathFreeContext(ctx);
   xmlFreeDoc(doc);

   if (href[0]) {
      resolve_href(principal_url, href, home_out, home_len);
      OLOG_INFO("caldav: calendar-home-set: %s", home_out);
      return CALDAV_OK;
   }

   return CALDAV_ERR_NO_CALENDARS;
}

/* ============================================================================
 * Discovery Step 3: Enumerate calendar collections (PROPFIND Depth:1)
 * ============================================================================ */

static const char *PROPFIND_COLLECTIONS_XML =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\""
    " xmlns:cs=\"http://calendarserver.org/ns/\""
    " xmlns:a=\"http://apple.com/ns/ical/\">"
    "<d:prop>"
    "<d:resourcetype/>"
    "<d:displayname/>"
    "<cs:getctag/>"
    "<d:current-user-privilege-set/>"
    "<a:calendar-color/>"
    "</d:prop>"
    "</d:propfind>";

static caldav_error_t discover_collections(CURL *curl,
                                           const char *home_url,
                                           curl_buffer_t *resp,
                                           caldav_calendar_info_t **out,
                                           int *out_count) {
   *out = NULL;
   *out_count = 0;

   caldav_error_t err = do_propfind(curl, home_url, "1", PROPFIND_COLLECTIONS_XML, resp);
   if (err != CALDAV_OK)
      return err;

   xmlDocPtr doc = xmlReadMemory(resp->data, (int)resp->size, NULL, NULL, CALDAV_XML_FLAGS);
   if (!doc)
      return CALDAV_ERR_PARSE;

   xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
   if (!ctx) {
      xmlFreeDoc(doc);
      return CALDAV_ERR_ALLOC;
   }
   register_namespaces(ctx);

   /* Find all response elements */
   xmlXPathObjectPtr responses = xmlXPathEvalExpression((const xmlChar *)"//d:response", ctx);
   if (!responses || !responses->nodesetval) {
      xmlXPathFreeContext(ctx);
      xmlFreeDoc(doc);
      return CALDAV_ERR_NO_CALENDARS;
   }

   int cap = 16;
   caldav_calendar_info_t *cals = calloc((size_t)cap, sizeof(caldav_calendar_info_t));
   if (!cals) {
      xmlXPathFreeObject(responses);
      xmlXPathFreeContext(ctx);
      xmlFreeDoc(doc);
      return CALDAV_ERR_ALLOC;
   }

   int count = 0;
   for (int i = 0; i < responses->nodesetval->nodeNr; i++) {
      xmlNodePtr resp_node = responses->nodesetval->nodeTab[i];

      /* Check if this resource has <cal:calendar/> in resourcetype */
      ctx->node = resp_node;
      xmlXPathObjectPtr rt = xmlXPathEvalExpression((const xmlChar *)".//d:resourcetype/c:calendar",
                                                    ctx);
      bool is_calendar = (rt && rt->nodesetval && rt->nodesetval->nodeNr > 0);
      xmlXPathFreeObject(rt);

      if (!is_calendar)
         continue;

      if (count >= cap) {
         cap *= 2;
         caldav_calendar_info_t *tmp = realloc(cals, (size_t)cap * sizeof(caldav_calendar_info_t));
         if (!tmp)
            break;
         cals = tmp;
      }

      caldav_calendar_info_t *cal = &cals[count];
      memset(cal, 0, sizeof(*cal));

      /* Extract href */
      char href[512] = { 0 };
      xpath_text(ctx, resp_node, "d:href", href, sizeof(href));
      resolve_href(home_url, href, cal->path, sizeof(cal->path));

      /* Display name */
      xpath_text(ctx, resp_node, ".//d:displayname", cal->display_name, sizeof(cal->display_name));

      /* CTag */
      xpath_text(ctx, resp_node, ".//cs:getctag", cal->ctag, sizeof(cal->ctag));

      /* Calendar color: try standard first, then Apple */
      if (xpath_text(ctx, resp_node, ".//a:calendar-color", cal->color, sizeof(cal->color)) != 0) {
         /* No Apple color, try x:calendar-color if needed */
      }

      /* Check read-only */
      xmlXPathObjectPtr write_priv = xmlXPathEvalExpression(
          (const xmlChar *)".//d:current-user-privilege-set/d:privilege/d:write", ctx);
      cal->read_only = !(write_priv && write_priv->nodesetval &&
                         write_priv->nodesetval->nodeNr > 0);
      xmlXPathFreeObject(write_priv);

      count++;
   }

   xmlXPathFreeObject(responses);
   xmlXPathFreeContext(ctx);
   xmlFreeDoc(doc);

   *out = cals;
   *out_count = count;
   return count > 0 ? CALDAV_OK : CALDAV_ERR_NO_CALENDARS;
}

/* ============================================================================
 * Public: caldav_discover — 3-step RFC-compliant discovery
 * ============================================================================ */

caldav_error_t caldav_discover(const char *base_url,
                               const caldav_auth_t *auth,
                               caldav_discovery_result_t *result) {
   memset(result, 0, sizeof(*result));

   curl_buffer_t resp;
   curl_buffer_init_with_max(&resp, CALDAV_MAX_RESPONSE_SIZE);

   CURL *curl = caldav_curl_init(auth, &resp);
   if (!curl) {
      curl_buffer_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   caldav_error_t err;

   /* Step 1: Find current-user-principal */
   char principal_url[512] = { 0 };
   err = discover_principal(curl, base_url, &resp, principal_url, sizeof(principal_url));

   if (err != CALDAV_OK || !principal_url[0]) {
      /* Fallback: try base URL directly as calendar-home (Radicale-style) */
      OLOG_INFO("caldav: no principal found, trying base URL as calendar-home");
      err = discover_collections(curl, base_url, &resp, &result->calendars,
                                 &result->calendar_count);
      if (err == CALDAV_OK) {
         snprintf(result->calendar_home_url, sizeof(result->calendar_home_url), "%s", base_url);
      }
      curl_easy_cleanup(curl);
      curl_buffer_free(&resp);
      return err;
   }

   snprintf(result->principal_url, sizeof(result->principal_url), "%s", principal_url);

   /* Step 2: Find calendar-home-set */
   char home_url[512] = { 0 };
   err = discover_calendar_home(curl, principal_url, &resp, home_url, sizeof(home_url));
   if (err != CALDAV_OK) {
      curl_easy_cleanup(curl);
      curl_buffer_free(&resp);
      return err;
   }

   snprintf(result->calendar_home_url, sizeof(result->calendar_home_url), "%s", home_url);

   /* Step 3: Enumerate calendar collections */
   err = discover_collections(curl, home_url, &resp, &result->calendars, &result->calendar_count);

   curl_easy_cleanup(curl);
   curl_buffer_free(&resp);
   return err;
}

void caldav_discovery_free(caldav_discovery_result_t *result) {
   free(result->calendars);
   result->calendars = NULL;
   result->calendar_count = 0;
}

/* ============================================================================
 * Public: caldav_get_ctag
 * ============================================================================ */

static const char *PROPFIND_CTAG_XML =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\" xmlns:cs=\"http://calendarserver.org/ns/\">"
    "<d:prop><cs:getctag/></d:prop>"
    "</d:propfind>";

caldav_error_t caldav_get_ctag(const char *calendar_url,
                               const caldav_auth_t *auth,
                               char *ctag_out,
                               size_t ctag_len) {
   ctag_out[0] = '\0';

   curl_buffer_t resp;
   curl_buffer_init_with_max(&resp, CALDAV_MAX_RESPONSE_SIZE);

   CURL *curl = caldav_curl_init(auth, &resp);
   if (!curl) {
      curl_buffer_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   caldav_error_t err = do_propfind(curl, calendar_url, "0", PROPFIND_CTAG_XML, &resp);
   curl_easy_cleanup(curl);

   if (err != CALDAV_OK) {
      curl_buffer_free(&resp);
      return err;
   }

   xmlDocPtr doc = xmlReadMemory(resp.data, (int)resp.size, NULL, NULL, CALDAV_XML_FLAGS);
   curl_buffer_free(&resp);
   if (!doc)
      return CALDAV_ERR_PARSE;

   xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
   if (!ctx) {
      xmlFreeDoc(doc);
      return CALDAV_ERR_ALLOC;
   }
   register_namespaces(ctx);
   xpath_text(ctx, xmlDocGetRootElement(doc), "//cs:getctag", ctag_out, ctag_len);
   xmlXPathFreeContext(ctx);
   xmlFreeDoc(doc);

   return ctag_out[0] ? CALDAV_OK : CALDAV_ERR_PARSE;
}

/* ============================================================================
 * Public: caldav_fetch_events (REPORT with calendar-query)
 * ============================================================================ */

caldav_error_t caldav_fetch_events(const char *calendar_url,
                                   const caldav_auth_t *auth,
                                   time_t range_start,
                                   time_t range_end,
                                   caldav_event_list_t *result) {
   result->events = NULL;
   result->count = 0;

   /* Build time-range filter */
   struct tm ts, te;
   gmtime_r(&range_start, &ts);
   gmtime_r(&range_end, &te);

   char xml[1024];
   snprintf(xml, sizeof(xml),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<c:calendar-query xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
            "<d:prop><d:getetag/><c:calendar-data/></d:prop>"
            "<c:filter><c:comp-filter name=\"VCALENDAR\">"
            "<c:comp-filter name=\"VEVENT\">"
            "<c:time-range start=\"%04d%02d%02dT000000Z\" end=\"%04d%02d%02dT000000Z\"/>"
            "</c:comp-filter></c:comp-filter></c:filter>"
            "</c:calendar-query>",
            ts.tm_year + 1900, ts.tm_mon + 1, ts.tm_mday, te.tm_year + 1900, te.tm_mon + 1,
            te.tm_mday);

   curl_buffer_t resp;
   curl_buffer_init_with_max(&resp, CALDAV_MAX_RESPONSE_SIZE);

   CURL *curl = caldav_curl_init(auth, &resp);
   if (!curl) {
      curl_buffer_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   /* REPORT method */
   curl_buffer_reset(&resp);
   curl_easy_setopt(curl, CURLOPT_URL, calendar_url);
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "REPORT");
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, xml);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(xml));

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Content-Type: application/xml; charset=utf-8");
   headers = curl_slist_append(headers, "Depth: 1");
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

   CURLcode cres = curl_easy_perform(curl);
   curl_slist_free_all(headers);

   if (cres != CURLE_OK) {
      OLOG_ERROR("caldav: REPORT %s failed: %s", calendar_url, curl_easy_strerror(cres));
      curl_easy_cleanup(curl);
      curl_buffer_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   if (resp.truncated) {
      OLOG_ERROR("caldav: REPORT %s response exceeded %d byte cap; rejecting", calendar_url,
                 CALDAV_MAX_RESPONSE_SIZE);
      curl_easy_cleanup(curl);
      curl_buffer_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   long status = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
   curl_easy_cleanup(curl);

   caldav_error_t err = http_status_to_error(status);
   if (err != CALDAV_OK) {
      curl_buffer_free(&resp);
      return err;
   }

   err = caldav_parse_events(resp.data, resp.size, calendar_url, result);
   curl_buffer_free(&resp);
   return err;
}

#ifdef HAVE_LIBICAL
/* Most RRULEs a zone of a name libical doesn't know may have (all its blocks
 * together).  Outlook and Exchange send two. */
#define CALDAV_TZ_MAX_RULES 16

/* Earliest year a repeating block may start: Windows zones start in 1601. */
#define CALDAV_TZ_MIN_START_YEAR 1600

/* Most VTIMEZONEs one event may carry: libical's own bookkeeping of them is
 * quadratic, and a real event names two or three zones. */
#define CALDAV_MAX_VTIMEZONES 16

/* Zone names already warned about in one fetch, so a hostile calendar logs
 * each once, not once per event per sync. */
typedef struct {
   char names[8][64];
   int count;
} tz_warned_t;

static void warn_zone_once(tz_warned_t *warned, const char *tzid, const char *why) {
   const char *name = tzid ? tzid : "?";
   for (int i = 0; i < warned->count; i++) {
      if (strncmp(warned->names[i], name, sizeof(warned->names[i]) - 1) == 0)
         return;
   }
   if (warned->count < (int)(sizeof(warned->names) / sizeof(warned->names[0])))
      snprintf(warned->names[warned->count++], sizeof(warned->names[0]), "%s", name);
   OLOG_WARNING("caldav: ignoring time zone '%.64s' (%s); its events are read as UTC", name, why);
}

/* Whether libical may look @p tzid up: it opens the zone file by name, so
 * "../" would reach any file, such as one of the daemon's own pipes. */
static bool tzid_is_safe(const char *tzid) {
   return tzid && !strstr(tzid, "..");
}

/* Entries in a libical BY* list (ICAL_RECURRENCE_ARRAY_MAX ends it). */
static int by_count(const short *list, int size) {
   int n = 0;
   while (n < size && list[n] != ICAL_RECURRENCE_ARRAY_MAX)
      n++;
   return n;
}

/* Whether the days of the month in @p r are seven in a row, all counted from
 * the start or all from the end (the older "BYMONTHDAY=8,...,14;BYDAY=SU"). */
static bool week_of_month_days(const struct icalrecurrencetype *r, int n_mday, int days) {
   if (n_mday != 7)
      return false;
   int lo = r->by_month_day[0];
   int hi = lo;
   for (int k = 0; k < n_mday; k++) {
      const int d = r->by_month_day[k];
      if (d == 0 || abs(d) > days || (d > 0) != (lo > 0))
         return false;
      lo = d < lo ? d : lo;
      hi = d > hi ? d : hi;
      for (int j = 0; j < k; j++) {
         if (r->by_month_day[j] == d)
            return false;
      }
   }
   return hi - lo == 6;
}

/* Whether a time zone rule names one real date a year, the way real zones
 * do, so libical never searches for a date that doesn't exist: in one month,
 * a weekday by position ("second Sunday", "last Sunday", also written with
 * BYSETPOS), one day of the month, or seven days in a row with a weekday.
 * Interval 1 only (a two-year interval from the wrong year never matches),
 * at most one hour, minute and second, and no non-Gregorian RSCALE (libical's
 * search for one can never end). */
static bool zone_rule_is_bounded(const struct icalrecurrencetype *r) {
   static const int month_days[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
   if (r->rscale || r->freq != ICAL_YEARLY_RECURRENCE || r->interval > 1 ||
       by_count(r->by_month, ICAL_BY_MONTH_SIZE) != 1 || r->by_month[0] < 1 ||
       r->by_month[0] > 12 || by_count(r->by_year_day, ICAL_BY_YEARDAY_SIZE) != 0 ||
       by_count(r->by_week_no, ICAL_BY_WEEKNO_SIZE) != 0 ||
       by_count(r->by_hour, ICAL_BY_HOUR_SIZE) > 1 ||
       by_count(r->by_minute, ICAL_BY_MINUTE_SIZE) > 1 ||
       by_count(r->by_second, ICAL_BY_SECOND_SIZE) > 1)
      return false;
   const int days = month_days[r->by_month[0] - 1];
   const int n_day = by_count(r->by_day, ICAL_BY_DAY_SIZE);
   const int n_mday = by_count(r->by_month_day, ICAL_BY_MONTHDAY_SIZE);
   const int n_setpos = by_count(r->by_set_pos, ICAL_BY_SETPOS_SIZE);
   const int pos = n_day == 1 ? icalrecurrencetype_day_position(r->by_day[0]) : 0;
   if (n_day > 1 || abs(pos) > 5)
      return false;

   if (n_mday == 0) {
      if (n_setpos == 0)
         return n_day == 1 && pos != 0;
      return n_day == 1 && pos == 0 && n_setpos == 1 && r->by_set_pos[0] != 0 &&
             abs(r->by_set_pos[0]) <= 5;
   }
   if (n_setpos != 0)
      return false;
   if (n_day == 0)
      return n_mday == 1 && r->by_month_day[0] != 0 && abs(r->by_month_day[0]) <= days;
   return pos == 0 && week_of_month_days(r, n_mday, days);
}

/* Whether libical can resolve times against @p vtz with bounded work: it
 * expands every RRULE of every block from the block's start to the year asked
 * for, so a rule changing more than once a year (a fuzz input's FREQ=HOURLY
 * took ~20 s per event), one that never matches, an early start or many rules
 * stall the sync.  Real zones change at most twice a year. */
static bool timezone_is_bounded(icalcomponent *vtz) {
   int rules = 0;
   for (icalcomponent *obs = icalcomponent_get_first_component(vtz, ICAL_ANY_COMPONENT); obs;
        obs = icalcomponent_get_next_component(vtz, ICAL_ANY_COMPONENT)) {
      icalproperty *p = icalcomponent_get_first_property(obs, ICAL_RRULE_PROPERTY);
      if (!p)
         continue; /* single changes (DTSTART/RDATE): cheap */
      /* The property's own value: the component form would resolve a TZID. */
      icalproperty *start = icalcomponent_get_first_property(obs, ICAL_DTSTART_PROPERTY);
      if (!start || icalproperty_get_dtstart(start).year < CALDAV_TZ_MIN_START_YEAR)
         return false;
      for (; p; p = icalcomponent_get_next_property(obs, ICAL_RRULE_PROPERTY)) {
         struct icalrecurrencetype r = icalproperty_get_rrule(p);
         if (++rules > CALDAV_TZ_MAX_RULES || !zone_rule_is_bounded(&r))
            return false;
      }
   }
   return true;
}

/* Whether to set aside the calendar's own definition of @p vtz: always when
 * libical has a zone of that name (its copy is the one used then: the same
 * rules, without trusting the server's), and otherwise when it isn't bounded
 * or its name isn't safe to look up (an event in it is then read as UTC). */
static bool timezone_set_aside(icalcomponent *vtz, tz_warned_t *warned) {
   icalproperty *id = icalcomponent_get_first_property(vtz, ICAL_TZID_PROPERTY);
   const char *tzid = id ? icalproperty_get_tzid(id) : NULL;
   if (!tzid_is_safe(tzid)) {
      warn_zone_once(warned, tzid, "unsafe name");
      return true;
   }
   if (icaltimezone_get_builtin_timezone(tzid))
      return true;
   if (timezone_is_bounded(vtz))
      return false;
   warn_zone_once(warned, tzid, "not a real zone's rules");
   return true;
}

/* Set aside the time zones in @p parent (its direct VTIMEZONE children, the
 * only ones libical looks up for a component under it). */
static void set_aside_timezones(icalcomponent *parent, tz_warned_t *warned) {
   icalcomponent *vtz = icalcomponent_get_first_component(parent, ICAL_VTIMEZONE_COMPONENT);
   while (vtz) {
      icalcomponent *next = icalcomponent_get_next_component(parent, ICAL_VTIMEZONE_COMPONENT);
      if (timezone_set_aside(vtz, warned)) {
         icalcomponent_remove_component(parent, vtz);
         icalcomponent_free(vtz);
      }
      vtz = next;
   }
}

/* Drop a TZID that isn't safe to look up from the event's start and end, so
 * libical's own fallback (a zone file of that name) never opens it either. */
static void strip_unsafe_tzids(icalcomponent *vevent, tz_warned_t *warned) {
   const icalproperty_kind kinds[] = { ICAL_DTSTART_PROPERTY, ICAL_DTEND_PROPERTY };
   for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
      icalproperty *p = icalcomponent_get_first_property(vevent, kinds[k]);
      icalparameter *param = p ? icalproperty_get_first_parameter(p, ICAL_TZID_PARAMETER) : NULL;
      if (param && !tzid_is_safe(icalparameter_get_tzid(param))) {
         warn_zone_once(warned, icalparameter_get_tzid(param), "unsafe name");
         icalproperty_remove_parameter_by_kind(p, ICAL_TZID_PARAMETER);
      }
   }
}

/* Case-insensitive match of @p word at @p p, reading through folded lines
 * (a line break followed by a space or tab, which iCalendar unfolds). */
static bool matches_unfolded(const char *p, const char *word) {
   for (; *word; word++) {
      while ((p[0] == '\r' && p[1] == '\n' && (p[2] == ' ' || p[2] == '\t')) ||
             (p[0] == '\n' && (p[1] == ' ' || p[1] == '\t')))
         p += p[0] == '\r' ? 3 : 2;
      if (toupper((unsigned char)*p) != *word)
         return false;
      p++;
   }
   return true;
}

/* Whether @p ical begins more than CALDAV_MAX_VTIMEZONES time zones, counted
 * before parsing (libical's handling of each is linear in their number). */
static bool too_many_vtimezones(const char *ical) {
   int count = 0;
   for (const char *p = ical; *p; p++) {
      const bool line_start = p == ical || (p[-1] == '\n' && *p != ' ' && *p != '\t');
      if (line_start && matches_unfolded(p, "BEGIN:VTIMEZONE") && ++count > CALDAV_MAX_VTIMEZONES)
         return true;
   }
   return false;
}

/* The event a resource is about: its series (the VEVENT without a
 * RECURRENCE-ID), which a server may list after a changed occurrence. */
static icalcomponent *master_event(icalcomponent *comp) {
   icalcomponent *first = icalcomponent_get_first_component(comp, ICAL_VEVENT_COMPONENT);
   for (icalcomponent *v = first; v;
        v = icalcomponent_get_next_component(comp, ICAL_VEVENT_COMPONENT)) {
      if (!icalcomponent_get_first_property(v, ICAL_RECURRENCEID_PROPERTY))
         return v;
   }
   return first;
}

/* An all-day date's fields, and its midnight (UTC) as an epoch. */
static time_t date_epoch(icaltimetype t, char *date, size_t date_len) {
   snprintf(date, date_len, "%04d-%02d-%02d", t.year, t.month, t.day);
   struct tm tm_date = { 0 };
   tm_date.tm_year = t.year - 1900;
   tm_date.tm_mon = t.month - 1;
   tm_date.tm_mday = t.day;
   return timegm(&tm_date);
}

/* A timed value as an epoch: converted to UTC components, then timegm(),
 * which (unlike mktime()) the process-wide TZ doesn't affect. */
static time_t time_epoch(icaltimetype t) {
   icaltimetype utc = icaltime_convert_to_zone(t, icaltimezone_get_utc_timezone());
   struct tm tm_utc = { 0 };
   tm_utc.tm_year = utc.year - 1900;
   tm_utc.tm_mon = utc.month - 1;
   tm_utc.tm_mday = utc.day;
   tm_utc.tm_hour = utc.hour;
   tm_utc.tm_min = utc.minute;
   tm_utc.tm_sec = utc.second;
   return timegm(&tm_utc);
}

/* Fill @p evt from one resource's iCalendar text.  False when it holds no
 * event DAWN can read (then it is skipped). */
static bool read_ical_event(const char *ical_data, caldav_event_t *evt, tz_warned_t *warned) {
   if (too_many_vtimezones(ical_data)) {
      OLOG_WARNING("caldav: skipping '%s': more than %d time zones", evt->href,
                   CALDAV_MAX_VTIMEZONES);
      return false;
   }
   icalcomponent *comp = icalparser_parse_string(ical_data);
   if (!comp)
      return false;
   icalcomponent *vevent = master_event(comp);
   if (!vevent) {
      icalcomponent_free(comp);
      return false;
   }
   /* Before any time is read: libical resolves a TZID through the event's own
    * VTIMEZONEs, then the calendar's, then a zone file of that name. */
   set_aside_timezones(vevent, warned);
   set_aside_timezones(comp, warned);
   strip_unsafe_tzids(vevent, warned);

   const char *uid = icalcomponent_get_uid(vevent);
   if (uid)
      snprintf(evt->uid, sizeof(evt->uid), "%s", uid);
   const char *summary = icalcomponent_get_summary(vevent);
   if (summary)
      snprintf(evt->summary, sizeof(evt->summary), "%s", summary);
   const char *desc = icalcomponent_get_description(vevent);
   if (desc)
      snprintf(evt->description, sizeof(evt->description), "%s", desc);
   const char *loc = icalcomponent_get_location(vevent);
   if (loc)
      snprintf(evt->location, sizeof(evt->location), "%s", loc);

   icaltimetype dtstart = icalcomponent_get_dtstart(vevent);
   if (!icaltime_is_null_time(dtstart)) {
      if (dtstart.is_date) {
         evt->all_day = true;
         evt->dtstart = date_epoch(dtstart, evt->dtstart_date, sizeof(evt->dtstart_date));
      } else {
         evt->dtstart = time_epoch(dtstart);
         /* The zone it repeats in, when libical has it built in. */
         icaltimezone *zone = (icaltimezone *)dtstart.zone;
         const char *name = zone ? icaltimezone_get_location(zone) : NULL;
         if (name && tzid_is_safe(name) && icaltimezone_get_builtin_timezone(name))
            snprintf(evt->tzid, sizeof(evt->tzid), "%s", name);
      }
   }
   icaltimetype dtend = icalcomponent_get_dtend(vevent);
   if (!icaltime_is_null_time(dtend)) {
      evt->dtend = dtend.is_date ? date_epoch(dtend, evt->dtend_date, sizeof(evt->dtend_date))
                                 : time_epoch(dtend);
   }
   if (evt->dtstart && evt->dtend)
      evt->duration_sec = (int)(evt->dtend - evt->dtstart);

   icalproperty *rrprop = icalcomponent_get_first_property(vevent, ICAL_RRULE_PROPERTY);
   if (rrprop) {
      struct icalrecurrencetype rrule = icalproperty_get_rrule(rrprop);
      const char *rrstr = icalrecurrencetype_as_string(&rrule);
      /* A cut rule would change meaning (a lost COUNT or UNTIL never ends). */
      if (rrstr &&
          (size_t)snprintf(evt->rrule, sizeof(evt->rrule), "%s", rrstr) >= sizeof(evt->rrule)) {
         OLOG_WARNING("caldav: '%s' has a recurrence rule too long to keep; shown once", evt->href);
         evt->rrule[0] = '\0';
      }
   }
   icalcomponent_free(comp);
   return true;
}

static long elapsed_ms(const struct timespec *since) {
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return (long)(now.tv_sec - since->tv_sec) * 1000 + (now.tv_nsec - since->tv_nsec) / 1000000;
}
#endif

caldav_error_t caldav_parse_events(const char *xml,
                                   size_t len,
                                   const char *calendar_url,
                                   caldav_event_list_t *result) {
   result->events = NULL;
   result->count = 0;
   if (len > INT_MAX)
      return CALDAV_ERR_PARSE;

   /* Parse multistatus response */
   xmlDocPtr doc = xmlReadMemory(xml, (int)len, NULL, NULL, CALDAV_XML_FLAGS);
   if (!doc)
      return CALDAV_ERR_PARSE;

   xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
   if (!ctx) {
      xmlFreeDoc(doc);
      return CALDAV_ERR_ALLOC;
   }
   register_namespaces(ctx);

   xmlXPathObjectPtr responses = xmlXPathEvalExpression((const xmlChar *)"//d:response", ctx);

   if (!responses || !responses->nodesetval || responses->nodesetval->nodeNr == 0) {
      xmlXPathFreeObject(responses);
      xmlXPathFreeContext(ctx);
      xmlFreeDoc(doc);
      return CALDAV_OK; /* no events in range */
   }

   int cap = responses->nodesetval->nodeNr;
   caldav_event_t *events = calloc((size_t)cap, sizeof(caldav_event_t));
   if (!events) {
      xmlXPathFreeObject(responses);
      xmlXPathFreeContext(ctx);
      xmlFreeDoc(doc);
      return CALDAV_ERR_ALLOC;
   }

   int count = 0;
   bool budget_spent = false;
#ifdef HAVE_LIBICAL
   tz_warned_t warned = { 0 };
   long slow_ms = 0;
#endif
   for (int i = 0; i < responses->nodesetval->nodeNr && count < cap; i++) {
      xmlNodePtr resp_node = responses->nodesetval->nodeTab[i];
      caldav_event_t *evt = &events[count];
      memset(evt, 0, sizeof(*evt));

      /* href */
      char href[512] = { 0 };
      xpath_text(ctx, resp_node, "d:href", href, sizeof(href));
      resolve_href(calendar_url, href, evt->href, sizeof(evt->href));

      /* etag */
      xpath_text(ctx, resp_node, ".//d:getetag", evt->etag, sizeof(evt->etag));
      /* Strip quotes from etag */
      size_t elen = strlen(evt->etag);
      if (elen >= 2 && evt->etag[0] == '"' && evt->etag[elen - 1] == '"') {
         memmove(evt->etag, evt->etag + 1, elen - 2);
         evt->etag[elen - 2] = '\0';
      }

      /* calendar-data (raw iCalendar) */
      char *ical_data = NULL;
      {
         ctx->node = resp_node;
         xmlXPathObjectPtr cd = xmlXPathEvalExpression((const xmlChar *)".//c:calendar-data", ctx);
         if (cd && cd->nodesetval && cd->nodesetval->nodeNr > 0) {
            xmlChar *content = xmlNodeGetContent(cd->nodesetval->nodeTab[0]);
            if (content) {
               ical_data = strdup((const char *)content);
               xmlFree(content);
            }
         }
         xmlXPathFreeObject(cd);
      }

      if (!ical_data)
         continue;

      evt->raw_ical = ical_data;

#ifdef HAVE_LIBICAL
      struct timespec t0;
      clock_gettime(CLOCK_MONOTONIC, &t0);
      const bool readable = read_ical_event(ical_data, evt, &warned);
      const long ms = elapsed_ms(&t0);
      if (ms > CALDAV_SLOW_EVENT_MS) {
         OLOG_WARNING("caldav: '%s' took %ld ms to read", evt->href, ms);
         slow_ms += ms;
         if (slow_ms > CALDAV_SLOW_BUDGET_MS) {
            OLOG_WARNING("caldav: stopping: slow events took %ld ms; keeping the cached copy",
                         slow_ms);
            count++; /* evt owns ical_data now */
            budget_spent = true;
            break;
         }
      }
      if (!readable) {
         free(ical_data);
         evt->raw_ical = NULL;
         continue;
      }
#endif /* HAVE_LIBICAL */

      count++;
   }

   xmlXPathFreeObject(responses);
   xmlXPathFreeContext(ctx);
   xmlFreeDoc(doc);

   result->events = events;
   result->count = count;
   if (budget_spent) {
      caldav_event_list_free(result);
      return CALDAV_ERR_TOO_SLOW;
   }
   return CALDAV_OK;
}

void caldav_event_list_free(caldav_event_list_t *list) {
   if (list->events) {
      for (int i = 0; i < list->count; i++) {
         free(list->events[i].raw_ical);
      }
      free(list->events);
      list->events = NULL;
   }
   list->count = 0;
}

/* ============================================================================
 * Public: caldav_sync_collection (REPORT sync-collection, RFC 6578)
 * ============================================================================ */

#define CALDAV_SYNC_MAX_PAGES 64 /* runaway backstop for server-paged enumeration */
/* Cumulative-bytes backstop: a hostile/broken server can return more=true with a
 * fresh token every page, so the page-count cap alone still permits
 * CALDAV_SYNC_MAX_PAGES * CALDAV_MAX_RESPONSE_SIZE bytes per cycle. Cap the total. */
#define CALDAV_SYNC_MAX_TOTAL_BYTES (32 * 1024 * 1024)

/** Grow the changes array to hold at least `need` entries. Returns 0 on success. */
static int sync_changes_reserve(caldav_sync_change_t **arr, int *cap, int need) {
   if (need <= *cap)
      return 0;
   int new_cap = *cap ? *cap * 2 : 64;
   while (new_cap < need)
      new_cap *= 2;
   caldav_sync_change_t *grown = realloc(*arr, (size_t)new_cap * sizeof(**arr));
   if (!grown)
      return 1;
   *arr = grown;
   *cap = new_cap;
   return 0;
}

caldav_error_t caldav_parse_sync_page(const char *xml,
                                      int len,
                                      const char *base_url,
                                      caldav_sync_change_t **changes,
                                      int *count,
                                      int *cap,
                                      char *token_out,
                                      size_t token_len,
                                      bool *more_out) {
   *more_out = false;
   xmlDocPtr doc = xmlReadMemory(xml, len, NULL, NULL, CALDAV_XML_FLAGS);
   if (!doc)
      return CALDAV_ERR_PARSE;

   xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
   if (!ctx) {
      /* xpath_text() unconditionally derefs ctx->node; guard the OOM case. */
      xmlFreeDoc(doc);
      return CALDAV_ERR_PARSE;
   }
   register_namespaces(ctx);

   /* New sync-token: the top-level <d:sync-token> child of <d:multistatus>
    * (not the per-response ones, of which there are none). */
   char new_token[1024] = { 0 };
   xpath_text(ctx, xmlDocGetRootElement(doc), "/d:multistatus/d:sync-token", new_token,
              sizeof(new_token));
   if (new_token[0])
      snprintf(token_out, token_len, "%s", new_token);

   xmlXPathObjectPtr responses = xmlXPathEvalExpression((const xmlChar *)"//d:response", ctx);
   if (responses && responses->nodesetval) {
      for (int i = 0; i < responses->nodesetval->nodeNr; i++) {
         xmlNodePtr resp_node = responses->nodesetval->nodeTab[i];

         char href[512] = { 0 };
         xpath_text(ctx, resp_node, "d:href", href, sizeof(href));

         /* Status may live directly under <d:response> (removals) or under a
          * <d:propstat> (getetag props). Gather all status strings for this node. */
         char status_all[256] = { 0 };
         {
            ctx->node = resp_node;
            xmlXPathObjectPtr st = xmlXPathEvalExpression((const xmlChar *)".//d:status", ctx);
            if (st && st->nodesetval) {
               for (int k = 0; k < st->nodesetval->nodeNr; k++) {
                  xmlChar *c = xmlNodeGetContent(st->nodesetval->nodeTab[k]);
                  if (c) {
                     size_t used = strlen(status_all);
                     snprintf(status_all + used, sizeof(status_all) - used, " %s", (const char *)c);
                     xmlFree(c);
                  }
               }
            }
            xmlXPathFreeObject(st);
         }

         /* A 507 anywhere means the server truncated the result set — more pages. */
         if (strstr(status_all, "507"))
            *more_out = true;

         if (!href[0])
            continue; /* the collection's own truncation response has no member href */

         if (sync_changes_reserve(changes, cap, *count + 1) != 0) {
            xmlXPathFreeObject(responses);
            xmlXPathFreeContext(ctx);
            xmlFreeDoc(doc);
            return CALDAV_ERR_ALLOC;
         }
         caldav_sync_change_t *chg = &(*changes)[*count];
         memset(chg, 0, sizeof(*chg));
         resolve_href(base_url, href, chg->href, sizeof(chg->href));
         chg->gone = (strstr(status_all, "404") != NULL);
         (*count)++;
      }
   }
   xmlXPathFreeObject(responses);
   xmlXPathFreeContext(ctx);
   xmlFreeDoc(doc);
   return CALDAV_OK;
}

caldav_error_t caldav_sync_collection(const char *calendar_url,
                                      const caldav_auth_t *auth,
                                      const char *sync_token_in,
                                      caldav_sync_result_t *result_out) {
   if (!result_out)
      return CALDAV_ERR_NETWORK;
   memset(result_out, 0, sizeof(*result_out));
   if (!calendar_url || !auth)
      return CALDAV_ERR_NETWORK;

   const bool had_token = (sync_token_in && sync_token_in[0]);
   char token[1024] = { 0 };
   if (had_token)
      snprintf(token, sizeof(token), "%s", sync_token_in);

   caldav_sync_change_t *changes = NULL;
   int count = 0, cap = 0;
   char latest_token[1024] = { 0 };
   bool complete = false;
   size_t total_bytes = 0; /* cumulative response bytes across pages (amplification cap) */

   curl_buffer_t resp;
   curl_buffer_init_with_max(&resp, CALDAV_MAX_RESPONSE_SIZE);

   caldav_error_t rc = CALDAV_OK;
   for (int page = 0; page < CALDAV_SYNC_MAX_PAGES; page++) {
      /* Build the REPORT body. An empty <d:sync-token/> requests a baseline. */
      char body[1536]; /* holds the XML wrapper + a fully entity-escaped esc_token[1152] */
      if (token[0]) {
         /* token is server-opaque; XML-escape defensively (Google tokens are URL-safe,
          * but a stray &, <, or > would break the body). Fixed-length appends with an
          * explicit end pointer — no reliance on snprintf's return value. */
         char esc_token[1152];
         char *w = esc_token;
         char *const end = esc_token + sizeof(esc_token) -
                           6; /* room for the longest entity + NUL */
         for (const char *p = token; *p && w < end; p++) {
            switch (*p) {
               case '&':
                  memcpy(w, "&amp;", 5);
                  w += 5;
                  break;
               case '<':
                  memcpy(w, "&lt;", 4);
                  w += 4;
                  break;
               case '>':
                  memcpy(w, "&gt;", 4);
                  w += 4;
                  break;
               default:
                  *w++ = *p;
                  break;
            }
         }
         *w = '\0';
         snprintf(body, sizeof(body),
                  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                  "<d:sync-collection xmlns:d=\"DAV:\">"
                  "<d:sync-token>%s</d:sync-token>"
                  "<d:sync-level>1</d:sync-level>"
                  "<d:prop><d:getetag/></d:prop>"
                  "</d:sync-collection>",
                  esc_token);
      } else {
         snprintf(body, sizeof(body),
                  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                  "<d:sync-collection xmlns:d=\"DAV:\">"
                  "<d:sync-token/>"
                  "<d:sync-level>1</d:sync-level>"
                  "<d:prop><d:getetag/></d:prop>"
                  "</d:sync-collection>");
      }

      curl_buffer_reset(&resp);
      CURL *curl = caldav_curl_init(auth, &resp);
      if (!curl) {
         rc = CALDAV_ERR_NETWORK;
         break;
      }
      curl_easy_setopt(curl, CURLOPT_URL, calendar_url);
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "REPORT");
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
      struct curl_slist *headers = NULL;
      headers = curl_slist_append(headers, "Content-Type: application/xml; charset=utf-8");
      headers = curl_slist_append(headers, "Depth: 0");
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

      CURLcode cres = curl_easy_perform(curl);
      curl_slist_free_all(headers);
      if (cres != CURLE_OK) {
         OLOG_WARNING("caldav: sync-collection %s failed: %s", calendar_url,
                      curl_easy_strerror(cres));
         curl_easy_cleanup(curl);
         rc = CALDAV_ERR_NETWORK;
         break;
      }
      if (resp.truncated) {
         /* Response bigger than the cap — cannot trust completeness; leave complete=false. */
         OLOG_WARNING("caldav: sync-collection %s exceeded %d byte cap", calendar_url,
                      CALDAV_MAX_RESPONSE_SIZE);
         curl_easy_cleanup(curl);
         rc = CALDAV_OK; /* not a hard error; just incomplete */
         break;
      }

      long status = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
      curl_easy_cleanup(curl);

      total_bytes += resp.size;
      if (total_bytes > CALDAV_SYNC_MAX_TOTAL_BYTES) {
         /* Amplification backstop: stop as incomplete (token not persisted, so no
          * spin — the sentinel still covers the in-window case). */
         OLOG_WARNING("caldav: sync-collection %s exceeded %d total bytes across pages",
                      calendar_url, CALDAV_SYNC_MAX_TOTAL_BYTES);
         break;
      }

      if (status == 401) {
         rc = CALDAV_ERR_AUTH;
         break;
      }
      if (status >= 400 && status < 500) {
         /* A non-empty token rejected with a 4xx (403 valid-sync-token, 409, 410, 400…)
          * means the token is stale — signal re-baseline. A 4xx on the *baseline*
          * (empty-token) request means the server does not support sync-collection. */
         rc = had_token ? CALDAV_ERR_SYNC_TOKEN_INVALID : http_status_to_error(status);
         break;
      }
      if (status >= 500) {
         rc = CALDAV_ERR_SERVER;
         break;
      }
      if (status != 207 && status != 200) {
         rc = CALDAV_ERR_PARSE;
         break;
      }

      bool more = false;
      caldav_error_t prc = caldav_parse_sync_page(resp.data, (int)resp.size, calendar_url, &changes,
                                                  &count, &cap, latest_token, sizeof(latest_token),
                                                  &more);
      if (prc != CALDAV_OK) {
         rc = prc;
         break;
      }

      if (!more) {
         complete = true;
         break;
      }
      /* More pages: continue from the token this page returned. Stop as incomplete
       * (rather than spin) if the server signalled 507 but gave no continuation token,
       * OR returned the same token we just sent (no forward progress — a broken or
       * hostile server). Either way the token isn't persisted, so no re-fetch loop. */
      if (!latest_token[0] || strcmp(latest_token, token) == 0)
         break;
      snprintf(token, sizeof(token), "%s", latest_token);
   }

   curl_buffer_free(&resp);

   if (rc != CALDAV_OK) {
      free(changes);
      return rc;
   }

   result_out->changes = changes;
   result_out->count = count;
   result_out->complete = complete;
   snprintf(result_out->sync_token, sizeof(result_out->sync_token), "%s", latest_token);
   return CALDAV_OK;
}

void caldav_sync_result_free(caldav_sync_result_t *result) {
   if (!result)
      return;
   free(result->changes);
   result->changes = NULL;
   result->count = 0;
}

/* ============================================================================
 * Public: caldav_create_event (PUT with If-None-Match: *)
 * ============================================================================ */

caldav_error_t caldav_create_event(const char *calendar_url,
                                   const caldav_auth_t *auth,
                                   const char *uid,
                                   const char *ical_data,
                                   char *etag_out,
                                   size_t etag_len) {
   if (etag_out && etag_len > 0)
      etag_out[0] = '\0';

   /* Build resource URL: calendar_url + uid + ".ics" */
   char url[1024];
   size_t cal_len = strlen(calendar_url);
   const char *sep = (cal_len > 0 && calendar_url[cal_len - 1] == '/') ? "" : "/";
   snprintf(url, sizeof(url), "%s%s%s.ics", calendar_url, sep, uid);

   curl_buffer_t resp;
   curl_buffer_init_with_max(&resp, CALDAV_MAX_RESPONSE_SIZE);

   CURL *curl = caldav_curl_init(auth, &resp);
   if (!curl) {
      curl_buffer_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   etag_capture_t etag_cap = { { 0 } };
   curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_etag_cb);
   curl_easy_setopt(curl, CURLOPT_HEADERDATA, &etag_cap);

   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, ical_data);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(ical_data));

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Content-Type: text/calendar; charset=utf-8");
   headers = curl_slist_append(headers, "If-None-Match: *");
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

   CURLcode cres = curl_easy_perform(curl);
   curl_slist_free_all(headers);

   if (cres != CURLE_OK) {
      OLOG_ERROR("caldav: PUT create %s failed: %s", url, curl_easy_strerror(cres));
      curl_easy_cleanup(curl);
      curl_buffer_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   long status = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

   /* Capture ETag from response headers */
   if (etag_out && etag_len > 0 && etag_cap.etag[0])
      snprintf(etag_out, etag_len, "%s", etag_cap.etag);

   curl_easy_cleanup(curl);
   curl_buffer_free(&resp);
   return http_status_to_error(status);
}

/* ============================================================================
 * Public: caldav_update_event (PUT with If-Match: etag)
 * ============================================================================ */

caldav_error_t caldav_update_event(const char *href,
                                   const caldav_auth_t *auth,
                                   const char *etag,
                                   const char *ical_data,
                                   char *etag_out,
                                   size_t etag_len) {
   if (etag_out && etag_len > 0)
      etag_out[0] = '\0';

   curl_buffer_t resp;
   curl_buffer_init_with_max(&resp, CALDAV_MAX_RESPONSE_SIZE);

   CURL *curl = caldav_curl_init(auth, &resp);
   if (!curl) {
      curl_buffer_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   etag_capture_t etag_cap = { { 0 } };
   curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_etag_cb);
   curl_easy_setopt(curl, CURLOPT_HEADERDATA, &etag_cap);

   curl_easy_setopt(curl, CURLOPT_URL, href);
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, ical_data);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(ical_data));

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Content-Type: text/calendar; charset=utf-8");
   if (etag && etag[0]) {
      char if_match[256];
      snprintf(if_match, sizeof(if_match), "If-Match: \"%s\"", etag);
      headers = curl_slist_append(headers, if_match);
   }
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

   CURLcode cres = curl_easy_perform(curl);
   curl_slist_free_all(headers);

   if (cres != CURLE_OK) {
      OLOG_ERROR("caldav: PUT update %s failed: %s", href, curl_easy_strerror(cres));
      curl_easy_cleanup(curl);
      curl_buffer_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   long status = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

   /* Capture new ETag from response */
   if (etag_out && etag_len > 0 && etag_cap.etag[0])
      snprintf(etag_out, etag_len, "%s", etag_cap.etag);

   curl_easy_cleanup(curl);
   curl_buffer_free(&resp);
   return http_status_to_error(status);
}

/* ============================================================================
 * Public: caldav_delete_event (DELETE with If-Match: etag)
 * ============================================================================ */

caldav_error_t caldav_delete_event(const char *href, const caldav_auth_t *auth, const char *etag) {
   curl_buffer_t resp;
   curl_buffer_init_with_max(&resp, CALDAV_MAX_RESPONSE_SIZE);

   CURL *curl = caldav_curl_init(auth, &resp);
   if (!curl) {
      curl_buffer_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   curl_easy_setopt(curl, CURLOPT_URL, href);
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");

   struct curl_slist *headers = NULL;
   if (etag && etag[0]) {
      char if_match[256];
      snprintf(if_match, sizeof(if_match), "If-Match: \"%s\"", etag);
      headers = curl_slist_append(headers, if_match);
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   }

   CURLcode cres = curl_easy_perform(curl);
   curl_slist_free_all(headers);

   if (cres != CURLE_OK) {
      OLOG_ERROR("caldav: DELETE %s failed: %s", href, curl_easy_strerror(cres));
      curl_easy_cleanup(curl);
      curl_buffer_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   long status = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
   curl_easy_cleanup(curl);
   curl_buffer_free(&resp);
   return http_status_to_error(status);
}

/* ============================================================================
 * Public: caldav_strerror
 * ============================================================================ */

const char *caldav_strerror(caldav_error_t err) {
   switch (err) {
      case CALDAV_OK:
         return "success";
      case CALDAV_ERR_NETWORK:
         return "network error";
      case CALDAV_ERR_AUTH:
         return "authentication failed (401)";
      case CALDAV_ERR_FORBIDDEN:
         return "forbidden (403)";
      case CALDAV_ERR_NOT_FOUND:
         return "not found (404)";
      case CALDAV_ERR_CONFLICT:
         return "conflict/ETag mismatch (412)";
      case CALDAV_ERR_SERVER:
         return "server error (5xx)";
      case CALDAV_ERR_PARSE:
         return "XML/iCalendar parse error";
      case CALDAV_ERR_NO_CALENDARS:
         return "no calendar collections found";
      case CALDAV_ERR_ALLOC:
         return "memory allocation failure";
      case CALDAV_ERR_SYNC_TOKEN_INVALID:
         return "sync-token invalid/expired (re-baseline)";
      case CALDAV_ERR_TOO_SLOW:
         return "events too slow to read (cached copy kept)";
   }
   return "unknown error";
}

#endif /* DAWN_ENABLE_CALENDAR_TOOL */
