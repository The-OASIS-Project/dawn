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
 */

#include "llm/llm_interface.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include "config/dawn_config.h"
#include "core/curl_buffer.h"
#include "core/session_manager.h"
#include "dawn.h"
#include "dawn_error.h"
#include "llm/llm_context.h"
#include "llm/llm_tool_loop.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "tts/text_to_speech.h"
#include "ui/metrics.h"
#include "utils/sentence_buffer.h"

// Provider implementations - include if compile-time keys exist OR if we might have runtime keys
// Note: The actual provider files (llm_openai.c, llm_claude.c) are always compiled
#include "llm/llm_claude.h"
#include "llm/llm_openai.h"
#include "llm/llm_rate_limit.h"

// LLM cloud base URLs (CLOUDAI_URL/CLAUDE_URL/GEMINI_URL/OPENROUTER_URL) are
// defined in llm_interface.h so the auxiliary resolvers share them.
// Local endpoint comes from g_config.llm.local.endpoint.

/* Thread-local timeout override for per-request timeout control.
 * Set by llm_chat_completion_with_config() when config->timeout_ms > 0,
 * read by llm_openai.c and llm_claude.c via llm_get_effective_timeout_ms().
 * Avoids racing on g_config.network.llm_timeout_ms from concurrent threads. */
static __thread int s_tl_timeout_ms = 0;

int llm_get_effective_timeout_ms(void) {
   if (s_tl_timeout_ms > 0) {
      return s_tl_timeout_ms;
   }
   return g_config.network.llm_timeout_ms;
}

/* Thread-local last-error code (see llm_interface.h LLM_ERR_*).  Provider
 * layers (claude/openai/etc.) set this when a known failure mode like
 * pre-flight unreachable is detected; callers read via llm_last_error()
 * after a NULL return to distinguish transient from genuine failures. */
static __thread int s_tl_last_error = LLM_ERR_NONE;

void llm_set_last_error(int code) {
   s_tl_last_error = code;
}

int llm_last_error(void) {
   return s_tl_last_error;
}

void llm_set_timeout_override(int timeout_ms) {
   s_tl_timeout_ms = timeout_ms;
}

// Helper functions to get API keys from runtime config (secrets.toml)
static const char *get_openai_api_key(void) {
   if (g_secrets.openai_api_key[0] != '\0') {
      return g_secrets.openai_api_key;
   }
   return NULL;
}

static const char *get_claude_api_key(void) {
   if (g_secrets.claude_api_key[0] != '\0') {
      return g_secrets.claude_api_key;
   }
   return NULL;
}

static const char *get_gemini_api_key(void) {
   if (g_secrets.gemini_api_key[0] != '\0') {
      return g_secrets.gemini_api_key;
   }
   return NULL;
}

static const char *get_openrouter_api_key(void) {
   if (g_secrets.openrouter_api_key[0] != '\0') {
      return g_secrets.openrouter_api_key;
   }
   return NULL;
}

static bool is_openai_available(void) {
   return get_openai_api_key() != NULL;
}

static bool is_claude_available(void) {
   return get_claude_api_key() != NULL;
}

static bool is_gemini_available(void) {
   return get_gemini_api_key() != NULL;
}

static bool is_openrouter_available(void) {
   return get_openrouter_api_key() != NULL;
}

// Public API key availability functions (wrappers for static helpers)
bool llm_has_openai_key(void) {
   return is_openai_available();
}

bool llm_has_claude_key(void) {
   return is_claude_available();
}

bool llm_has_gemini_key(void) {
   return is_gemini_available();
}

bool llm_has_openrouter_key(void) {
   return is_openrouter_available();
}

bool llm_openrouter_gateway_enabled(void) {
   return g_config.llm.cloud.use_openrouter;
}

bool llm_apply_openrouter_gateway(cloud_provider_t *provider,
                                  const char **endpoint,
                                  const char **api_key) {
   if (!provider || !llm_openrouter_gateway_enabled()) {
      return false;
   }
   /* Only rewrite cloud targets; local stays local. */
   if (*provider == CLOUD_PROVIDER_NONE) {
      return false;
   }
   *provider = CLOUD_PROVIDER_OPENROUTER;
   if (endpoint) {
      *endpoint = OPENROUTER_URL; /* unconditional — do not rely on downstream fallback */
   }
   if (api_key) {
      *api_key = get_openrouter_api_key();
   }
   return true;
}

cloud_provider_t llm_detect_available_provider(void) {
   /* Gateway mode is the single authority for bool→enum: when on, OpenRouter
    * (if keyed) wins over any direct provider. */
   if (llm_openrouter_gateway_enabled()) {
      return is_openrouter_available() ? CLOUD_PROVIDER_OPENROUTER : CLOUD_PROVIDER_NONE;
   }
   if (is_claude_available())
      return CLOUD_PROVIDER_CLAUDE;
   if (is_openai_available())
      return CLOUD_PROVIDER_OPENAI;
   if (is_gemini_available())
      return CLOUD_PROVIDER_GEMINI;
   return CLOUD_PROVIDER_NONE;
}

// Global state
// current_type: written on the main/config thread, read on the mosquitto
// HUD-discovery thread — atomic so those cross-thread accesses are race-free.
static _Atomic llm_type_t current_type = LLM_UNDEFINED;
static cloud_provider_t current_cloud_provider = CLOUD_PROVIDER_NONE;
static char llm_url[2048] = "";

// Global interrupt flag - set by main thread (signal handler / wake word) during
// LLM processing, read by the curl progress callback on worker threads.  _Atomic
// sig_atomic_t keeps it async-signal-safe (lock-free) AND race-free under TSan.
static _Atomic sig_atomic_t llm_interrupt_requested = 0;

// Thread-local cancel flag for per-session cancellation (WebUI multi-user support)
// When set, this takes precedence over the global interrupt flag
static __thread _Atomic bool *tls_cancel_flag = NULL;

int llm_get_current_resolved_config(llm_resolved_config_t *config_out) {
   if (!config_out) {
      return 1;
   }

   // Check for session context (set during streaming calls)
   session_t *session = session_get_command_context();
   if (session) {
      session_llm_config_t session_config;
      session_get_llm_config(session, &session_config);

      if (llm_resolve_config(&session_config, config_out) == 0) {
         return 0;
      }
   }

   // No session or resolve failed
   return 1;
}

// Legacy compatibility: MemoryStruct is now implemented using curl_buffer_t from curl_buffer.h
// The llm_openai.c and llm_claude.c files still use the old naming, so we provide this wrapper.
// Note: curl_buffer_t uses 'data' field, MemoryStruct used 'memory' field.
// Both are handled by the common curl_buffer_write_callback().

/**
 * @brief Extracts the host and port from a URL, removing protocol and paths.
 *
 * This function handles stripping protocols (http, https) and extracts the host and port from
 * URLs. If no port is provided, it defaults to port 80 for http and 443 for https.
 *
 * @param url The input URL string.
 * @param host Output buffer to store the extracted host (must be pre-allocated).
 * @param port Output buffer to store the extracted port (must be large enough for the port
 * number).
 * @return int Returns 0 on success, 1 on failure.
 */
static int extract_host_and_port(const char *url, char *host, char *port) {
   // Validate the input arguments
   if (url == NULL || host == NULL || port == NULL) {
      OLOG_ERROR("Error: NULL argument passed to extract_host_and_port.");
      return 1;
   }

   if (strlen(url) == 0) {
      OLOG_ERROR("Error: Empty URL provided.");
      return 1;
   }

   const char *start = url;

   // Determine protocol and set default port
   if (strncmp(url, "http://", 7) == 0) {
      start = url + 7;     // Skip "http://"
      strcpy(port, "80");  // Default port for http
   } else if (strncmp(url, "https://", 8) == 0) {
      start = url + 8;      // Skip "https://"
      strcpy(port, "443");  // Default port for https
   } else {
      // If no recognizable protocol, assume http and continue
      strcpy(port, "80");
   }

   // Find the end of the host part (either ':' for port or '/' for path)
   const char *end = strpbrk(start, ":/");
   if (end == NULL) {
      // No port or path, the host is the entire remaining string
      strcpy(host, start);
   } else if (*end == ':') {
      // Extract the host and port
      strncpy(host, start, end - start);
      host[end - start] = '\0';  // Null-terminate the host
      strcpy(port, end + 1);     // Port starts after ':'
   } else {
      // Extract the host only (no port, but has a path)
      strncpy(host, start, end - start);
      host[end - start] = '\0';  // Null-terminate the host
   }

   return 0;
}

int llm_check_connection(const char *url, int timeout_seconds) {
   char host[2048];
   char port[6];

   // Extract host from the URL (ignores path and protocol)
   if (extract_host_and_port(url, host, port) != 0) {
      OLOG_ERROR("Error: Invalid URL format");
      return 0;
   }

   // Set up address resolution hints
   struct addrinfo hints, *res;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_INET;        // Use IPv4
   hints.ai_socktype = SOCK_STREAM;  // TCP stream sockets

   // Resolve host (works for both hostnames and IP addresses)
   int status = getaddrinfo(host, port, &hints, &res);
   if (status != 0) {
      OLOG_ERROR("getaddrinfo: %s", gai_strerror(status));
      return 0;
   }

   // Create a socket
   int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
   if (sock == -1) {
      OLOG_ERROR("socket: %s", strerror(errno));
      freeaddrinfo(res);
      return 0;
   }

   // Set socket as non-blocking
   fcntl(sock, F_SETFL, O_NONBLOCK);

   // Attempt to connect
   int result = connect(sock, res->ai_addr, res->ai_addrlen);
   if (result == -1 && errno != EINPROGRESS) {
      OLOG_ERROR("connect: %s", strerror(errno));
      close(sock);
      freeaddrinfo(res);
      return 0;
   }

   // Set up the file descriptor set for select()
   fd_set write_fds;
   FD_ZERO(&write_fds);
   FD_SET(sock, &write_fds);

   // Set the timeout value
   struct timeval timeout;
   timeout.tv_sec = timeout_seconds;
   timeout.tv_usec = 0;

   // Wait for the socket to become writable within the timeout
   result = select(sock + 1, NULL, &write_fds, NULL, &timeout);
   if (result == 1) {
      // Socket is writable, check for connection success
      int error;
      socklen_t error_len = sizeof(error);
      if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &error_len) == 0 && error == 0) {
         result = 1;  // Connection successful
      } else {
         result = 0;  // Connection failed
      }
   } else {
      result = 0;  // Timeout or error
   }

   // Clean up
   close(sock);
   freeaddrinfo(res);

   return result;
}

void llm_init(const char *cloud_provider_override) {
   // Initialize tool calling system first - needed for both local and cloud LLMs
   // Must happen before any early returns so tools are available for local mode
   llm_tools_init();

   // Initialize rate limiter (pass 0 to disable)
   llm_rate_limit_init(g_config.llm.rate_limit_enabled ? g_config.llm.rate_limit_rpm : 0);

   // Apply per-tool enable/disable config from TOML (do this early too).
   // Apply when ANY of the four lists (enable/disable × local/remote) is set;
   // otherwise leave registration defaults (all non-dangerous on) in place.
   if (g_config.llm.tools.local_enabled_configured ||
       g_config.llm.tools.remote_enabled_configured ||
       g_config.llm.tools.local_disabled_configured ||
       g_config.llm.tools.remote_disabled_configured) {
      llm_tools_apply_config(&g_config.llm.tools);
   }

   /* CLI alias: "-P openrouter" enables the gateway by flipping the config bool,
    * so the bool remains the single authority for the bool->enum conversion
    * (see llm_openrouter_gateway_enabled / the single-authority rule). */
   if (cloud_provider_override != NULL && strcmp(cloud_provider_override, "openrouter") == 0) {
      g_config.llm.cloud.use_openrouter = true;
   }

   /* OpenRouter gateway: SINGLE AUTHORITY for use_openrouter -> CLOUD_PROVIDER_OPENROUTER.
    * When on, force the provider and skip the direct-provider CLI>config>auto-detect
    * ladder entirely.  Mirrored in llm_refresh_providers().  A direct-provider CLI
    * override (-P claude, etc.) does NOT override the gateway. */
   if (llm_openrouter_gateway_enabled()) {
      if (is_openrouter_available()) {
         current_cloud_provider = CLOUD_PROVIDER_OPENROUTER;
         OLOG_INFO("Cloud provider set to OpenRouter (gateway mode)");
      } else {
         OLOG_WARNING("OpenRouter gateway enabled but openrouter_api_key not configured — "
                      "staying on local LLM");
         current_cloud_provider = CLOUD_PROVIDER_NONE;
         llm_set_type(LLM_LOCAL);
      }
      return;
   }

   // Detect available providers from runtime config (secrets.toml)
   bool openai_available = is_openai_available();
   bool claude_available = is_claude_available();
   bool gemini_available = is_gemini_available();

   if (!openai_available && !claude_available && !gemini_available) {
      OLOG_WARNING("No cloud LLM providers configured (check secrets.toml)");
      current_cloud_provider = CLOUD_PROVIDER_NONE;
      llm_set_type(LLM_LOCAL);
      return;
   }

   // Priority: CLI override > config file > auto-detect
   const char *provider_source = NULL;
   if (cloud_provider_override != NULL) {
      provider_source = cloud_provider_override;
   } else if (g_config.llm.cloud.provider[0] != '\0') {
      provider_source = g_config.llm.cloud.provider;
   }

   if (provider_source != NULL) {
      bool provider_ok = false;
      if (strcmp(provider_source, "openai") == 0 && openai_available) {
         current_cloud_provider = CLOUD_PROVIDER_OPENAI;
         provider_ok = true;
         OLOG_INFO("Cloud provider set to OpenAI (%s)",
                   cloud_provider_override ? "CLI override" : "config file");
      } else if (strcmp(provider_source, "claude") == 0 && claude_available) {
         current_cloud_provider = CLOUD_PROVIDER_CLAUDE;
         provider_ok = true;
         OLOG_INFO("Cloud provider set to Claude (%s)",
                   cloud_provider_override ? "CLI override" : "config file");
      } else if (strcmp(provider_source, "gemini") == 0 && gemini_available) {
         current_cloud_provider = CLOUD_PROVIDER_GEMINI;
         provider_ok = true;
         OLOG_INFO("Cloud provider set to Gemini (%s)",
                   cloud_provider_override ? "CLI override" : "config file");
      }

      if (!provider_ok) {
         /* Configured provider unavailable — fall through to auto-detect */
         OLOG_WARNING("Cloud provider '%s' has no API key — auto-detecting available provider",
                      provider_source);
         provider_source = NULL;
      }
   }

   if (provider_source == NULL) {
      /* Auto-detect: pick the first provider with an API key */
      current_cloud_provider = llm_detect_available_provider();
      if (current_cloud_provider != CLOUD_PROVIDER_NONE) {
         OLOG_INFO("Cloud provider auto-detected: %s",
                   cloud_provider_to_string(current_cloud_provider));
      }
   }

   // Note: LLM type (local/cloud) is set by dawn.c after this function returns,
   // allowing proper TTS announcement after TTS is initialized.
}

/* Re-derive the global llm_url from current_cloud_provider + config endpoint (no TTS).
 * llm_refresh_providers() can change the provider at runtime (e.g. a WebUI gateway
 * toggle or secrets reload) but historically left llm_url stale — the non-session
 * global chat path (MQTT replies, search/summary fallback) uses llm_url + the live
 * provider together, so they must stay in sync.  Custom llm.cloud.endpoint still wins. */
static void refresh_cloud_llm_url(void) {
   if (current_type != LLM_CLOUD) {
      return;
   }
   const char *cfg_ep = g_config.llm.cloud.endpoint[0] != '\0' ? g_config.llm.cloud.endpoint : NULL;
   const char *def_url = NULL;
   switch (current_cloud_provider) {
      case CLOUD_PROVIDER_CLAUDE:
         def_url = CLAUDE_URL;
         break;
      case CLOUD_PROVIDER_GEMINI:
         def_url = GEMINI_URL;
         break;
      case CLOUD_PROVIDER_OPENROUTER:
         def_url = OPENROUTER_URL;
         break;
      case CLOUD_PROVIDER_OPENAI:
         def_url = CLOUDAI_URL;
         break;
      default:
         return; /* NONE — about to fall back to local; leave llm_url unchanged */
   }
   snprintf(llm_url, sizeof(llm_url), "%s", cfg_ep ? cfg_ep : def_url);
}

int llm_refresh_providers(void) {
   /* OpenRouter gateway short-circuit (mirrors llm_init): the gateway bool is the
    * single authority, so skip the direct-provider availability dance entirely.
    * Without this, a secrets/settings reload would silently un-set OpenRouter. */
   if (llm_openrouter_gateway_enabled()) {
      if (is_openrouter_available()) {
         current_cloud_provider = CLOUD_PROVIDER_OPENROUTER;
         refresh_cloud_llm_url();
         OLOG_INFO("LLM refresh: OpenRouter gateway ready");
         return 1;
      }
      OLOG_INFO("LLM refresh: OpenRouter gateway on but no key available");
      current_cloud_provider = CLOUD_PROVIDER_NONE;
      return 0;
   }

   bool openai_available = is_openai_available();
   bool claude_available = is_claude_available();
   bool gemini_available = is_gemini_available();

   if (!openai_available && !claude_available && !gemini_available) {
      OLOG_INFO("LLM refresh: No cloud providers available");
      current_cloud_provider = CLOUD_PROVIDER_NONE;
      return 0;
   }

   // Check if current provider is still valid
   if (current_cloud_provider == CLOUD_PROVIDER_OPENAI && !openai_available) {
      // OpenAI key removed, switch to Claude or Gemini if available
      if (claude_available) {
         current_cloud_provider = CLOUD_PROVIDER_CLAUDE;
         OLOG_INFO("LLM refresh: Switched to Claude (OpenAI key removed)");
      } else if (gemini_available) {
         current_cloud_provider = CLOUD_PROVIDER_GEMINI;
         OLOG_INFO("LLM refresh: Switched to Gemini (OpenAI key removed)");
      } else {
         current_cloud_provider = CLOUD_PROVIDER_NONE;
         OLOG_INFO("LLM refresh: No cloud providers available");
         return 0;
      }
   } else if (current_cloud_provider == CLOUD_PROVIDER_CLAUDE && !claude_available) {
      // Claude key removed, switch to OpenAI or Gemini if available
      if (openai_available) {
         current_cloud_provider = CLOUD_PROVIDER_OPENAI;
         OLOG_INFO("LLM refresh: Switched to OpenAI (Claude key removed)");
      } else if (gemini_available) {
         current_cloud_provider = CLOUD_PROVIDER_GEMINI;
         OLOG_INFO("LLM refresh: Switched to Gemini (Claude key removed)");
      } else {
         current_cloud_provider = CLOUD_PROVIDER_NONE;
         OLOG_INFO("LLM refresh: No cloud providers available");
         return 0;
      }
   } else if (current_cloud_provider == CLOUD_PROVIDER_GEMINI && !gemini_available) {
      // Gemini key removed, switch to OpenAI or Claude if available
      if (openai_available) {
         current_cloud_provider = CLOUD_PROVIDER_OPENAI;
         OLOG_INFO("LLM refresh: Switched to OpenAI (Gemini key removed)");
      } else if (claude_available) {
         current_cloud_provider = CLOUD_PROVIDER_CLAUDE;
         OLOG_INFO("LLM refresh: Switched to Claude (Gemini key removed)");
      } else {
         current_cloud_provider = CLOUD_PROVIDER_NONE;
         OLOG_INFO("LLM refresh: No cloud providers available");
         return 0;
      }
   } else if (current_cloud_provider == CLOUD_PROVIDER_NONE) {
      // No provider was set, auto-detect (prefer OpenAI)
      if (openai_available) {
         current_cloud_provider = CLOUD_PROVIDER_OPENAI;
         OLOG_INFO("LLM refresh: OpenAI now available");
      } else if (claude_available) {
         current_cloud_provider = CLOUD_PROVIDER_CLAUDE;
         OLOG_INFO("LLM refresh: Claude now available");
      } else {
         current_cloud_provider = CLOUD_PROVIDER_GEMINI;
         OLOG_INFO("LLM refresh: Gemini now available");
      }
   }

   refresh_cloud_llm_url();
   OLOG_INFO("LLM refresh: Cloud provider ready (%s)", llm_get_cloud_provider_name());
   return 1;
}

int llm_set_type(llm_type_t type) {
   if (type == LLM_CLOUD) {
      // Check if API key is available for the current cloud provider
      int has_api_key = 0;
      const char *provider_name = "unknown";

      if (current_cloud_provider == CLOUD_PROVIDER_CLAUDE) {
         has_api_key = is_claude_available();
         provider_name = "Claude";
      } else if (current_cloud_provider == CLOUD_PROVIDER_GEMINI) {
         has_api_key = is_gemini_available();
         provider_name = "Gemini";
      } else if (current_cloud_provider == CLOUD_PROVIDER_OPENROUTER) {
         has_api_key = is_openrouter_available();
         provider_name = "OpenRouter";
      } else {
         has_api_key = is_openai_available();
         provider_name = "OpenAI";
      }

      if (!has_api_key) {
         OLOG_WARNING("Cannot switch to cloud LLM: %s API key not configured in secrets.toml - "
                      "staying on local LLM",
                      provider_name);
         text_to_speech("Cannot switch to cloud. API key not configured. Staying on local.");
         // Don't change current_type, stay on whatever we were using
         return 1; /* Failure - API key not configured */
      }

      // API key available, proceed with switch
      current_type = type;

      // Use config endpoint if set, otherwise use default provider URLs
      const char *cloud_endpoint = g_config.llm.cloud.endpoint[0] != '\0'
                                       ? g_config.llm.cloud.endpoint
                                       : NULL;
      if (current_cloud_provider == CLOUD_PROVIDER_CLAUDE) {
         snprintf(llm_url, sizeof(llm_url), "%s", cloud_endpoint ? cloud_endpoint : CLAUDE_URL);
         text_to_speech("Setting AI to cloud LLM using Claude.");
      } else if (current_cloud_provider == CLOUD_PROVIDER_GEMINI) {
         snprintf(llm_url, sizeof(llm_url), "%s", cloud_endpoint ? cloud_endpoint : GEMINI_URL);
         text_to_speech("Setting AI to cloud LLM using Gemini.");
      } else if (current_cloud_provider == CLOUD_PROVIDER_OPENROUTER) {
         snprintf(llm_url, sizeof(llm_url), "%s", cloud_endpoint ? cloud_endpoint : OPENROUTER_URL);
         text_to_speech("Setting AI to cloud LLM using OpenRouter.");
      } else {
         snprintf(llm_url, sizeof(llm_url), "%s", cloud_endpoint ? cloud_endpoint : CLOUDAI_URL);
         text_to_speech("Setting AI to cloud LLM using OpenAI.");
      }
      if (cloud_endpoint) {
         OLOG_INFO("LLM set to CLOUD (%s) via custom endpoint: %s", llm_get_cloud_provider_name(),
                   cloud_endpoint);
      } else {
         OLOG_INFO("LLM set to CLOUD (%s)", llm_get_cloud_provider_name());
      }
   } else if (type == LLM_LOCAL) {
      current_type = type;
      snprintf(llm_url, sizeof(llm_url), "%s", g_config.llm.local.endpoint);
      text_to_speech("Setting AI to local LLM.");
      OLOG_INFO("LLM set to LOCAL (%s)", g_config.llm.local.endpoint);
   }

   // Update metrics with current LLM configuration
   metrics_update_llm_config(current_type, current_cloud_provider);
   return 0; /* Success */
}

llm_type_t llm_get_type(void) {
   /* Check session context first (set during streaming calls) */
   session_t *session = session_get_command_context();
   if (session) {
      session_llm_config_t config;
      session_get_llm_config(session, &config);
      return config.type;
   }

   /* Fallback to global state for paths without session context */
   return current_type;
}

const char *llm_get_cloud_provider_name(void) {
   cloud_provider_t provider = current_cloud_provider;

   /* Check session context first (set during streaming calls) */
   session_t *session = session_get_command_context();
   if (session) {
      session_llm_config_t config;
      session_get_llm_config(session, &config);
      provider = config.cloud_provider;
   }

   switch (provider) {
      case CLOUD_PROVIDER_OPENAI:
         return "OpenAI";
      case CLOUD_PROVIDER_CLAUDE:
         return "Claude";
      case CLOUD_PROVIDER_GEMINI:
         return "Gemini";
      case CLOUD_PROVIDER_OPENROUTER:
         return "OpenRouter";
      case CLOUD_PROVIDER_NONE:
         return "None";
      default:
         return "Unknown";
   }
}

const char *cloud_provider_to_string(cloud_provider_t provider) {
   switch (provider) {
      case CLOUD_PROVIDER_OPENAI:
         return "openai";
      case CLOUD_PROVIDER_CLAUDE:
         return "claude";
      case CLOUD_PROVIDER_GEMINI:
         return "gemini";
      case CLOUD_PROVIDER_OPENROUTER:
         return "openrouter";
      case CLOUD_PROVIDER_NONE:
      default:
         return "none";
   }
}

int cloud_provider_from_string(const char *str,
                               llm_type_t *type_out,
                               cloud_provider_t *provider_out) {
   if (str == NULL || type_out == NULL || provider_out == NULL) {
      return FAILURE;
   }

   /* "local"/"ollama" -> local server; the rest are cloud providers.  Accepts the
    * "anthropic" alias for Claude (kept for parity with resolve_silent_observe_config). */
   if (strcmp(str, "local") == 0 || strcmp(str, "ollama") == 0) {
      *type_out = LLM_LOCAL;
      *provider_out = CLOUD_PROVIDER_NONE;
   } else if (strcmp(str, "openai") == 0) {
      *type_out = LLM_CLOUD;
      *provider_out = CLOUD_PROVIDER_OPENAI;
   } else if (strcmp(str, "claude") == 0 || strcmp(str, "anthropic") == 0) {
      *type_out = LLM_CLOUD;
      *provider_out = CLOUD_PROVIDER_CLAUDE;
   } else if (strcmp(str, "gemini") == 0) {
      *type_out = LLM_CLOUD;
      *provider_out = CLOUD_PROVIDER_GEMINI;
   } else if (strcmp(str, "openrouter") == 0) {
      *type_out = LLM_CLOUD;
      *provider_out = CLOUD_PROVIDER_OPENROUTER;
   } else {
      return FAILURE; /* unknown -> leave outputs untouched */
   }

   return SUCCESS;
}

int llm_set_cloud_provider(cloud_provider_t provider) {
   if (provider == CLOUD_PROVIDER_OPENAI) {
      if (!is_openai_available()) {
         OLOG_ERROR("Cannot switch to OpenAI: API key not configured");
         return 1;
      }
      current_cloud_provider = CLOUD_PROVIDER_OPENAI;
      // Update URL if we're currently in cloud mode
      if (current_type == LLM_CLOUD) {
         const char *cloud_endpoint = g_config.llm.cloud.endpoint[0] != '\0'
                                          ? g_config.llm.cloud.endpoint
                                          : CLOUDAI_URL;
         snprintf(llm_url, sizeof(llm_url), "%s", cloud_endpoint);
      }
      OLOG_INFO("Cloud provider set to OpenAI");
      return 0;
   } else if (provider == CLOUD_PROVIDER_CLAUDE) {
      if (!is_claude_available()) {
         OLOG_ERROR("Cannot switch to Claude: API key not configured");
         return 1;
      }
      current_cloud_provider = CLOUD_PROVIDER_CLAUDE;
      // Update URL if we're currently in cloud mode
      if (current_type == LLM_CLOUD) {
         const char *cloud_endpoint = g_config.llm.cloud.endpoint[0] != '\0'
                                          ? g_config.llm.cloud.endpoint
                                          : CLAUDE_URL;
         snprintf(llm_url, sizeof(llm_url), "%s", cloud_endpoint);
      }
      OLOG_INFO("Cloud provider set to Claude");
      return 0;
   } else if (provider == CLOUD_PROVIDER_GEMINI) {
      if (!is_gemini_available()) {
         OLOG_ERROR("Cannot switch to Gemini: API key not configured");
         return 1;
      }
      current_cloud_provider = CLOUD_PROVIDER_GEMINI;
      // Update URL if we're currently in cloud mode
      if (current_type == LLM_CLOUD) {
         const char *cloud_endpoint = g_config.llm.cloud.endpoint[0] != '\0'
                                          ? g_config.llm.cloud.endpoint
                                          : GEMINI_URL;
         snprintf(llm_url, sizeof(llm_url), "%s", cloud_endpoint);
      }
      OLOG_INFO("Cloud provider set to Gemini");
      return 0;
   } else if (provider == CLOUD_PROVIDER_OPENROUTER) {
      if (!is_openrouter_available()) {
         OLOG_ERROR("Cannot switch to OpenRouter: API key not configured");
         return 1;
      }
      current_cloud_provider = CLOUD_PROVIDER_OPENROUTER;
      // Update URL if we're currently in cloud mode
      if (current_type == LLM_CLOUD) {
         const char *cloud_endpoint = g_config.llm.cloud.endpoint[0] != '\0'
                                          ? g_config.llm.cloud.endpoint
                                          : OPENROUTER_URL;
         snprintf(llm_url, sizeof(llm_url), "%s", cloud_endpoint);
      }
      OLOG_INFO("Cloud provider set to OpenRouter");
      return 0;
   }
   return 1;
}

cloud_provider_t llm_get_cloud_provider(void) {
   /* Check session context first (set during streaming calls) */
   session_t *session = session_get_command_context();
   if (session) {
      session_llm_config_t config;
      session_get_llm_config(session, &config);
      return config.cloud_provider;
   }

   /* Fallback to global state for paths without session context */
   return current_cloud_provider;
}

const char *llm_get_model_name(void) {
   static char session_model_buf[LLM_MODEL_NAME_MAX]; /* Static buffer for session model */
   llm_type_t type = current_type;
   cloud_provider_t provider = current_cloud_provider;

   /* Check session context first (set during streaming calls) */
   session_t *session = session_get_command_context();
   if (session) {
      session_llm_config_t config = { 0 }; /* Zero-init for safety */
      session_get_llm_config(session, &config);
      type = config.type;
      provider = config.cloud_provider;

      /* If session has a custom model, copy to static buffer and return */
      if (config.model[0] != '\0') {
         strncpy(session_model_buf, config.model, sizeof(session_model_buf) - 1);
         session_model_buf[sizeof(session_model_buf) - 1] = '\0';
         return session_model_buf;
      }
   }

   /* Return model name based on type and provider */
   if (type == LLM_LOCAL) {
      if (g_config.llm.local.model[0] != '\0') {
         return g_config.llm.local.model;
      }
      return "local";
   }

   /* Cloud LLM - return model from config based on provider */
   if (provider == CLOUD_PROVIDER_OPENAI) {
      return llm_get_default_openai_model();
   } else if (provider == CLOUD_PROVIDER_CLAUDE) {
      return llm_get_default_claude_model();
   } else if (provider == CLOUD_PROVIDER_GEMINI) {
      return llm_get_default_gemini_model();
   } else if (provider == CLOUD_PROVIDER_OPENROUTER) {
      return llm_get_default_openrouter_model();
   }
   return "None";
}

const char *llm_get_default_openai_model(void) {
   int idx = g_config.llm.cloud.openai_default_model_idx;
   int count = g_config.llm.cloud.openai_models_count;

   /* Fallback to first model if index out of bounds or no models */
   if (count <= 0) {
      return LLM_DEFAULT_OPENAI_MODEL;
   }
   if (idx < 0 || idx >= count) {
      idx = 0;
   }
   return g_config.llm.cloud.openai_models[idx];
}

const char *llm_get_default_claude_model(void) {
   int idx = g_config.llm.cloud.claude_default_model_idx;
   int count = g_config.llm.cloud.claude_models_count;

   /* Fallback to first model if index out of bounds or no models */
   if (count <= 0) {
      return LLM_DEFAULT_CLAUDE_MODEL;
   }
   if (idx < 0 || idx >= count) {
      idx = 0;
   }
   return g_config.llm.cloud.claude_models[idx];
}

const char *llm_get_default_gemini_model(void) {
   int idx = g_config.llm.cloud.gemini_default_model_idx;
   int count = g_config.llm.cloud.gemini_models_count;

   /* Fallback to first model if index out of bounds or no models */
   if (count <= 0) {
      return LLM_DEFAULT_GEMINI_MODEL;
   }
   if (idx < 0 || idx >= count) {
      idx = 0;
   }
   return g_config.llm.cloud.gemini_models[idx];
}

const char *llm_get_default_openrouter_model(void) {
   int idx = g_config.llm.cloud.openrouter_default_model_idx;
   int count = g_config.llm.cloud.openrouter_models_count;

   /* Fallback to first model if index out of bounds or no models */
   if (count <= 0) {
      return LLM_DEFAULT_OPENROUTER_MODEL;
   }
   if (idx < 0 || idx >= count) {
      idx = 0;
   }
   return g_config.llm.cloud.openrouter_models[idx];
}

bool llm_openrouter_slug_for(cloud_provider_t provider,
                             const char *bare_model,
                             char *out,
                             size_t out_len) {
   if (!bare_model || bare_model[0] == '\0' || !out || out_len == 0) {
      return false;
   }

   /* Already a vendor/model slug — pass through unchanged. */
   if (strchr(bare_model, '/') != NULL) {
      int n = snprintf(out, out_len, "%s", bare_model);
      return (n >= 0 && (size_t)n < out_len); /* truncated -> unresolvable, not a corrupt id */
   }

   /* Provider hint -> OpenRouter vendor prefix. */
   const char *prefix = NULL;
   switch (provider) {
      case CLOUD_PROVIDER_OPENAI:
         prefix = "openai/";
         break;
      case CLOUD_PROVIDER_CLAUDE:
         prefix = "anthropic/";
         break;
      case CLOUD_PROVIDER_GEMINI:
         prefix = "google/";
         break;
      default:
         prefix = NULL;
         break;
   }

   /* Prefer an exact match in the operator's configured openrouter_models[] catalog:
    * a slug whose tail (after '/') equals the bare model.  When a provider hint is
    * present, require the vendor prefix to match so "gpt-5.5" under OpenAI resolves to
    * "openai/gpt-5.5" and not some other vendor's identically-named tail. */
   for (int i = 0; i < g_config.llm.cloud.openrouter_models_count; i++) {
      const char *slug = g_config.llm.cloud.openrouter_models[i];
      const char *slash = strchr(slug, '/');
      const char *tail = slash ? slash + 1 : slug;
      if (strcmp(tail, bare_model) != 0) {
         continue;
      }
      if (prefix != NULL && strncmp(slug, prefix, strlen(prefix)) != 0) {
         continue; /* tail matches but wrong vendor */
      }
      int n = snprintf(out, out_len, "%s", slug);
      return (n >= 0 && (size_t)n < out_len);
   }

   /* No catalog entry: synthesize from the provider prefix if we have one. */
   if (prefix != NULL) {
      int n = snprintf(out, out_len, "%s%s", prefix, bare_model);
      return (n >= 0 && (size_t)n < out_len); /* truncated -> unresolvable */
   }

   return false; /* no '/', no provider hint, no catalog match — cannot resolve */
}

void llm_request_interrupt(void) {
   llm_interrupt_requested = 1;
}

void llm_clear_interrupt(void) {
   llm_interrupt_requested = 0;
}

int llm_is_interrupt_requested(void) {
   return llm_interrupt_requested;
}

void llm_set_cancel_flag(void *flag) {
   tls_cancel_flag = (_Atomic bool *)flag;
}

void *llm_get_cancel_flag(void) {
   return (void *)tls_cancel_flag;
}

/**
 * @brief CURL progress callback to check for interruption requests
 *
 * Called periodically by CURL during transfer. Returns non-zero to abort.
 * Checks per-session cancel flag first (for multi-user WebUI support),
 * then falls back to global interrupt flag (for local wake word detection).
 *
 * @param clientp User data pointer (unused)
 * @param dltotal Total bytes to download
 * @param dlnow Bytes downloaded so far
 * @param ultotal Total bytes to upload
 * @param ulnow Bytes uploaded so far
 * @return 0 to continue transfer, non-zero to abort
 */
int llm_curl_progress_callback(void *clientp,
                               curl_off_t dltotal,
                               curl_off_t dlnow,
                               curl_off_t ultotal,
                               curl_off_t ulnow) {
   (void)clientp;  // Unused
   (void)dltotal;
   (void)dlnow;
   (void)ultotal;
   (void)ulnow;

   // Check per-session cancel flag first (multi-user WebUI support)
   if (tls_cancel_flag && atomic_load(tls_cancel_flag)) {
      OLOG_INFO("LLM transfer cancelled by session");
      return 1;  // Non-zero aborts transfer
   }

   // Fall back to global interrupt flag (local wake word detection)
   if (llm_interrupt_requested) {
      OLOG_INFO("LLM transfer interrupted by wake word");
      return 1;  // Non-zero aborts transfer
   }
   return 0;  // Zero continues transfer
}

char *llm_chat_completion(struct json_object *conversation_history,
                          const char *input_text,
                          const char **vision_images,
                          const size_t *vision_image_sizes,
                          int vision_image_count,
                          bool allow_fallback) {
   llm_set_last_error(LLM_ERR_NONE); /* see contract on llm_chat_completion_with_config */
   char *response = NULL;
   llm_type_t type = current_type;
   cloud_provider_t provider = current_cloud_provider;
   const char *url = llm_url;
   const char *api_key = NULL;
   char model_buf[LLM_MODEL_NAME_MAX] = ""; /* Buffer to hold model name (survives scope) */
   const char *model = NULL;

   /* Check session context first (set during streaming calls) */
   session_t *session = session_get_command_context();
   if (session) {
      session_llm_config_t session_config = { 0 }; /* Zero-init for safety */
      session_get_llm_config(session, &session_config);

      llm_resolved_config_t resolved = { 0 }; /* Zero-init for safety */
      if (llm_resolve_config(&session_config, &resolved) == 0) {
         type = resolved.type;
         provider = resolved.cloud_provider;
         url = resolved.endpoint ? resolved.endpoint : llm_url;
         api_key = resolved.api_key;
         /* Copy model to local buffer (resolved.model may be dangling pointer) */
         LLM_COPY_MODEL_SAFE(model_buf, resolved.model);
         if (model_buf[0] != '\0') {
            model = model_buf;
         }
      }
   } else {
      /* Use global state for non-session context */
      if (provider == CLOUD_PROVIDER_OPENAI) {
         api_key = get_openai_api_key();
      } else if (provider == CLOUD_PROVIDER_CLAUDE) {
         api_key = get_claude_api_key();
      } else if (provider == CLOUD_PROVIDER_GEMINI) {
         api_key = get_gemini_api_key();
      } else if (provider == CLOUD_PROVIDER_OPENROUTER) {
         api_key = get_openrouter_api_key();
      }
   }

   /* Gate cloud API calls through rate limiter */
   if (type != LLM_LOCAL) {
      if (llm_rate_limit_wait())
         return NULL; /* interrupted */
   }

   if (type == LLM_LOCAL) {
      /* Local LLM uses OpenAI-compatible API (no API key needed) */
      response = llm_openai_chat_completion(conversation_history, input_text, vision_images,
                                            vision_image_sizes, vision_image_count, url, NULL,
                                            model);
   } else {
      /* Route to cloud provider */
      switch (provider) {
         case CLOUD_PROVIDER_OPENAI:
            response = llm_openai_chat_completion(conversation_history, input_text, vision_images,
                                                  vision_image_sizes, vision_image_count, url,
                                                  api_key, model);
            break;

         case CLOUD_PROVIDER_CLAUDE:
            response = llm_claude_chat_completion(conversation_history, input_text, vision_images,
                                                  vision_image_sizes, vision_image_count, url,
                                                  api_key, model);
            break;

         case CLOUD_PROVIDER_GEMINI:
         case CLOUD_PROVIDER_OPENROUTER:
            /* Gemini and OpenRouter both use the OpenAI-compatible API */
            response = llm_openai_chat_completion(conversation_history, input_text, vision_images,
                                                  vision_image_sizes, vision_image_count, url,
                                                  api_key, model);
            break;

         default:
            OLOG_ERROR("No cloud provider configured");
            return NULL;
      }
   }

   /* If cloud LLM failed (but not interrupted by user), try falling back to local */
   if (response == NULL && type == LLM_CLOUD && allow_fallback && !llm_is_interrupt_requested()) {
      if (strcmp(CLOUDAI_URL, url) == 0 || strcmp(CLAUDE_URL, url) == 0 ||
          strcmp(GEMINI_URL, url) == 0 || strcmp(OPENROUTER_URL, url) == 0) {
         OLOG_WARNING("Falling back to local LLM due to connection failure.");
         text_to_speech("Unable to contact cloud LLM.");
         llm_set_type(LLM_LOCAL);

         /* Retry with local LLM (uses OpenAI-compatible API without auth) */
         response = llm_openai_chat_completion(conversation_history, input_text, vision_images,
                                               vision_image_sizes, vision_image_count, llm_url,
                                               NULL, NULL);
      }
   }

   return response;
}

char *llm_chat_completion_streaming(struct json_object *conversation_history,
                                    const char *input_text,
                                    const char **vision_images,
                                    const size_t *vision_image_sizes,
                                    int vision_image_count,
                                    llm_text_chunk_callback chunk_callback,
                                    void *callback_userdata,
                                    bool allow_fallback) {
   llm_set_last_error(LLM_ERR_NONE); /* see contract on llm_chat_completion_with_config */
   // Clear any previous interrupt flag from cancelled requests
   llm_clear_interrupt();

   char *response = NULL;
   llm_type_t type = current_type;
   cloud_provider_t provider = current_cloud_provider;
   const char *url = llm_url;
   const char *api_key = NULL;
   char model_buf[LLM_MODEL_NAME_MAX] = ""; /* Buffer to hold model name (survives scope) */
   const char *model = NULL;

   /* Check session context first (set during streaming calls) */
   session_t *session = session_get_command_context();
   if (session) {
      session_llm_config_t session_config = { 0 }; /* Zero-init for safety */
      session_get_llm_config(session, &session_config);

      llm_resolved_config_t resolved = { 0 }; /* Zero-init for safety */
      if (llm_resolve_config(&session_config, &resolved) == 0) {
         type = resolved.type;
         provider = resolved.cloud_provider;
         url = resolved.endpoint ? resolved.endpoint : llm_url;
         api_key = resolved.api_key;
         /* Copy model to local buffer (resolved.model may be dangling pointer) */
         LLM_COPY_MODEL_SAFE(model_buf, resolved.model);
         if (model_buf[0] != '\0') {
            model = model_buf;
         }
      }
   } else {
      /* Use global state for non-session context */
      if (provider == CLOUD_PROVIDER_OPENAI) {
         api_key = get_openai_api_key();
      } else if (provider == CLOUD_PROVIDER_CLAUDE) {
         api_key = get_claude_api_key();
      } else if (provider == CLOUD_PROVIDER_GEMINI) {
         api_key = get_gemini_api_key();
      } else if (provider == CLOUD_PROVIDER_OPENROUTER) {
         api_key = get_openrouter_api_key();
      }
   }

   /* Track LLM total time */
   struct timeval start_time, end_time;
   gettimeofday(&start_time, NULL);

   /* Record query metrics */
   metrics_record_llm_query(type);

   /* Determine provider function and history format */
   uint32_t session_id = session ? session->session_id : 0;
   llm_single_shot_fn provider_fn;
   llm_history_format_t history_format;

   if (type == LLM_CLOUD && provider == CLOUD_PROVIDER_CLAUDE) {
      provider_fn = (llm_single_shot_fn)llm_claude_streaming_single_shot;
      history_format = LLM_HISTORY_CLAUDE;
   } else {
      /* OpenAI, Gemini, OpenRouter, and local all use the OpenAI-compatible API
       * (including Anthropic models served via OpenRouter — they use OpenAI wire
       * format, not the native Claude path). */
      provider_fn = (llm_single_shot_fn)llm_openai_streaming_single_shot;
      history_format = LLM_HISTORY_OPENAI;
      if (type == LLM_LOCAL) {
         api_key = NULL; /* Local LLM needs no API key */
      }
   }

   /* Run the central tool iteration loop */
   llm_tool_loop_params_t loop_params = {
      .conversation_history = conversation_history,
      .input_text = input_text,
      .vision_images = vision_images,
      .vision_image_sizes = vision_image_sizes,
      .vision_image_count = vision_image_count,
      .base_url = url,
      .api_key = api_key,
      .model = model,
      .chunk_callback = (void *)chunk_callback,
      .callback_userdata = callback_userdata,
      .provider_fn = provider_fn,
      .history_format = history_format,
      .session_id = session_id,
      .llm_type = type,
      .cloud_provider = provider,
   };

   response = llm_tool_iteration_loop(&loop_params);

   /* If cloud LLM failed (but not interrupted by user), try falling back to local */
   if (response == NULL && type == LLM_CLOUD && allow_fallback && !llm_is_interrupt_requested()) {
      if (strcmp(CLOUDAI_URL, url) == 0 || strcmp(CLAUDE_URL, url) == 0 ||
          strcmp(GEMINI_URL, url) == 0 || strcmp(OPENROUTER_URL, url) == 0) {
         OLOG_WARNING("Falling back to local LLM due to connection failure.");
         text_to_speech("Unable to contact cloud LLM.");
         llm_set_type(LLM_LOCAL);

         /* Retry with local LLM via tool iteration loop */
         loop_params.base_url = llm_url;
         loop_params.api_key = NULL;
         loop_params.model = NULL;
         loop_params.provider_fn = (llm_single_shot_fn)llm_openai_streaming_single_shot;
         loop_params.history_format = LLM_HISTORY_OPENAI;
         loop_params.llm_type = LLM_LOCAL;
         loop_params.cloud_provider = CLOUD_PROVIDER_NONE;
         response = llm_tool_iteration_loop(&loop_params);
         /* Record fallback event */
         metrics_record_fallback();
      }
   }

   /* Trigger async compaction for next turn (WebUI sessions) */
   {
      session_t *trigger_session = session_get(loop_params.session_id);
      if (trigger_session) {
         llm_context_async_trigger(trigger_session, loop_params.conversation_history,
                                   loop_params.llm_type, loop_params.cloud_provider,
                                   loop_params.model);
         session_release(trigger_session);
      }
   }

   /* Record LLM total time */
   if (response != NULL) {
      gettimeofday(&end_time, NULL);
      double total_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 +
                        (end_time.tv_usec - start_time.tv_usec) / 1000.0;
      metrics_record_llm_total_time(total_ms);
   }

   return response;
}

/**
 * @brief Context for TTS streaming
 *
 * THREADING SAFETY CONTRACT:
 * This structure is stack-allocated in llm_chat_completion_streaming_tts() and relies
 * on synchronous CURL execution. The context remains valid throughout the entire CURL
 * request because curl_easy_perform() blocks until completion, invoking all callbacks
 * from the calling thread before returning.
 *
 * IMPORTANT: If migrating to asynchronous CURL (curl_easy_multi) or background threads:
 * 1. This structure MUST be heap-allocated with explicit lifetime management
 * 2. Add reference counting or retain until all callbacks complete
 * 3. Callbacks may execute after the original function returns
 * 4. Consider using atomic reference counts for multi-threaded access
 *
 * CURRENT IMPLEMENTATION STATUS:
 * - Single-threaded network client processing (dawn.c state machine)
 * - Synchronous CURL execution (safe with stack allocation)
 * - Per-request contexts (no sharing between requests)
 *
 * FUTURE MULTI-CLIENT ARCHITECTURE:
 * - Worker threads will each have their own stack and contexts
 * - No shared state between workers (inherently thread-safe)
 * - Each worker calls this function independently
 * - Stack allocation remains safe as long as CURL stays synchronous
 */
typedef struct {
   sentence_buffer_t *sentence_buffer;
   llm_sentence_callback user_callback;
   void *user_userdata;
} tts_streaming_context_t;

/**
 * @brief Chunk callback that feeds to sentence buffer
 */
static void tts_chunk_callback(const char *chunk, void *userdata) {
   tts_streaming_context_t *ctx = (tts_streaming_context_t *)userdata;
   sentence_buffer_feed(ctx->sentence_buffer, chunk);
}

/**
 * @brief Sentence callback wrapper
 */
static void tts_sentence_callback(const char *sentence, void *userdata) {
   tts_streaming_context_t *ctx = (tts_streaming_context_t *)userdata;
   ctx->user_callback(sentence, ctx->user_userdata);
}

char *llm_chat_completion_streaming_tts(struct json_object *conversation_history,
                                        const char *input_text,
                                        const char **vision_images,
                                        const size_t *vision_image_sizes,
                                        int vision_image_count,
                                        llm_sentence_callback sentence_callback,
                                        void *callback_userdata,
                                        bool allow_fallback) {
   llm_set_last_error(LLM_ERR_NONE); /* see contract on llm_chat_completion_with_config */
   char *response = NULL;
   tts_streaming_context_t ctx;

   // Create sentence buffer
   ctx.sentence_buffer = sentence_buffer_create(tts_sentence_callback, &ctx);
   if (!ctx.sentence_buffer) {
      OLOG_ERROR("Failed to create sentence buffer for TTS streaming");
      return NULL;
   }

   ctx.user_callback = sentence_callback;
   ctx.user_userdata = callback_userdata;

   // Call streaming with chunk callback that feeds sentence buffer
   response = llm_chat_completion_streaming(conversation_history, input_text, vision_images,
                                            vision_image_sizes, vision_image_count,
                                            tts_chunk_callback, &ctx, allow_fallback);

   // Flush any remaining sentence
   sentence_buffer_flush(ctx.sentence_buffer);

   // Cleanup
   sentence_buffer_free(ctx.sentence_buffer);

   return response;
}

/* ============================================================================
 * Per-Session LLM Configuration Support
 * ============================================================================ */

void llm_get_default_config(session_llm_config_t *config) {
   if (!config) {
      return;
   }

   memset(config, 0, sizeof(*config));

   // Determine default LLM type from g_config
   if (strcmp(g_config.llm.type, "local") == 0) {
      config->type = LLM_LOCAL;
   } else {
      config->type = LLM_CLOUD;
   }

   // Set default cloud provider (used when switching from local to cloud).
   // OpenRouter gateway is the single authority: when on, the default provider is
   // always OpenRouter regardless of the provider string (mirrors llm_init).
   if (llm_openrouter_gateway_enabled()) {
      config->cloud_provider = CLOUD_PROVIDER_OPENROUTER;
   } else {
      // If config specifies a provider AND it has a key, use it; otherwise auto-detect
      cloud_provider_t configured = CLOUD_PROVIDER_NONE;
      bool configured_has_key = false;
      if (strcasecmp(g_config.llm.cloud.provider, "claude") == 0) {
         configured = CLOUD_PROVIDER_CLAUDE;
         configured_has_key = is_claude_available();
      } else if (strcasecmp(g_config.llm.cloud.provider, "gemini") == 0) {
         configured = CLOUD_PROVIDER_GEMINI;
         configured_has_key = is_gemini_available();
      } else if (strcasecmp(g_config.llm.cloud.provider, "openai") == 0) {
         configured = CLOUD_PROVIDER_OPENAI;
         configured_has_key = is_openai_available();
      }

      if (configured != CLOUD_PROVIDER_NONE && configured_has_key) {
         config->cloud_provider = configured;
      } else {
         // Config provider unavailable or empty — use the auto-detected provider
         config->cloud_provider = llm_get_cloud_provider();
      }
   }

   // Endpoint and model are empty by default (resolved at call time from config)
   config->endpoint[0] = '\0';
   config->model[0] = '\0';

   // Copy tool mode from global config
   if (g_config.llm.tools.mode[0] != '\0') {
      strncpy(config->tool_mode, g_config.llm.tools.mode, sizeof(config->tool_mode) - 1);
      config->tool_mode[sizeof(config->tool_mode) - 1] = '\0';
   } else {
      strncpy(config->tool_mode, "native", sizeof(config->tool_mode) - 1);
   }

   // Copy thinking mode from global config
   if (g_config.llm.thinking.mode[0] != '\0') {
      strncpy(config->thinking_mode, g_config.llm.thinking.mode, sizeof(config->thinking_mode) - 1);
      config->thinking_mode[sizeof(config->thinking_mode) - 1] = '\0';
   } else {
      strncpy(config->thinking_mode, "disabled", sizeof(config->thinking_mode) - 1);
   }

   // Copy reasoning effort from global config
   if (g_config.llm.thinking.reasoning_effort[0] != '\0') {
      strncpy(config->reasoning_effort, g_config.llm.thinking.reasoning_effort,
              sizeof(config->reasoning_effort) - 1);
      config->reasoning_effort[sizeof(config->reasoning_effort) - 1] = '\0';
   } else {
      strncpy(config->reasoning_effort, "medium", sizeof(config->reasoning_effort) - 1);
   }

   OLOG_INFO("Default LLM config: type=%s, provider=%s",
             config->type == LLM_LOCAL ? "local" : "cloud",
             cloud_provider_to_string(config->cloud_provider));
}

int llm_resolve_config(const session_llm_config_t *session_config,
                       llm_resolved_config_t *resolved) {
   if (!resolved || !session_config) {
      return 1;
   }

   // Use session config directly (sessions own their config)
   resolved->type = session_config->type;
   resolved->cloud_provider = session_config->cloud_provider;
   resolved->endpoint = session_config->endpoint[0] != '\0' ? session_config->endpoint : NULL;
   resolved->model = session_config->model[0] != '\0' ? session_config->model : NULL;
   resolved->api_key = NULL;

   /* OpenRouter gateway: canonical, inescapable enforcement of the single-authority
    * rule.  Even a session whose stored cloud_provider predates a runtime gateway
    * toggle resolves to OpenRouter here.  Forcing the enum (not the helper) keeps the
    * existing per-provider endpoint resolution below intact, so a custom
    * llm.cloud.endpoint still overrides the default OpenRouter URL.  The model is left
    * as-is (session model, or the OpenRouter default filled in below). */
   if (resolved->type == LLM_CLOUD && llm_openrouter_gateway_enabled()) {
      cloud_provider_t provider_hint = resolved->cloud_provider; /* stored provider, pre-force */
      resolved->cloud_provider = CLOUD_PROVIDER_OPENROUTER;
      /* Canonical bare-model remap — the single choke point every cloud request passes
       * through.  Any session model that isn't already a "vendor/model" slug (legacy
       * conversations, non-gateway-aware clients, or a future call site that forgot to
       * remap) is mapped to the right OpenRouter slug here, so a bare id is never sent
       * verbatim nor silently swapped for the gateway default.  The catalog tail-match in
       * the helper resolves it even when the provider hint has already been forced to
       * OPENROUTER upstream.  Transient: the session's stored model is untouched. */
      if (resolved->model != NULL && strchr(resolved->model, '/') == NULL) {
         if (llm_openrouter_slug_for(provider_hint, resolved->model, resolved->model_buf,
                                     sizeof(resolved->model_buf))) {
            resolved->model = resolved->model_buf;
         } else {
            resolved->model = NULL; /* unresolvable — default filled in below */
         }
      }
   }

   // Validate and get API key for cloud providers
   if (resolved->type == LLM_CLOUD) {
      if (resolved->cloud_provider == CLOUD_PROVIDER_OPENAI) {
         if (!is_openai_available()) {
            OLOG_ERROR("Session config requests OpenAI but no API key configured");
            return 1;
         }
         resolved->api_key = get_openai_api_key();
      } else if (resolved->cloud_provider == CLOUD_PROVIDER_CLAUDE) {
         if (!is_claude_available()) {
            OLOG_ERROR("Session config requests Claude but no API key configured");
            return 1;
         }
         resolved->api_key = get_claude_api_key();
      } else if (resolved->cloud_provider == CLOUD_PROVIDER_GEMINI) {
         if (!is_gemini_available()) {
            OLOG_ERROR("Session config requests Gemini but no API key configured");
            return 1;
         }
         resolved->api_key = get_gemini_api_key();
      } else if (resolved->cloud_provider == CLOUD_PROVIDER_OPENROUTER) {
         if (!is_openrouter_available()) {
            OLOG_ERROR("Session config requests OpenRouter but no API key configured");
            return 1;
         }
         resolved->api_key = get_openrouter_api_key();
      } else {
         OLOG_ERROR("Session config requests cloud but no provider specified");
         return 1;
      }
   }

   // Resolve endpoint if not specified in session config
   if (resolved->endpoint == NULL || resolved->endpoint[0] == '\0') {
      if (resolved->type == LLM_LOCAL) {
         resolved->endpoint = g_config.llm.local.endpoint;
      } else if (resolved->cloud_provider == CLOUD_PROVIDER_OPENAI) {
         resolved->endpoint = CLOUDAI_URL;
      } else if (resolved->cloud_provider == CLOUD_PROVIDER_CLAUDE) {
         resolved->endpoint = CLAUDE_URL;
      } else if (resolved->cloud_provider == CLOUD_PROVIDER_GEMINI) {
         resolved->endpoint = GEMINI_URL;
      } else if (resolved->cloud_provider == CLOUD_PROVIDER_OPENROUTER) {
         resolved->endpoint = OPENROUTER_URL;
      }
   }

   // Resolve model if not specified in session config
   if (resolved->model == NULL || resolved->model[0] == '\0') {
      if (resolved->type == LLM_LOCAL) {
         resolved->model = g_config.llm.local.model;
      } else if (resolved->cloud_provider == CLOUD_PROVIDER_OPENAI) {
         resolved->model = llm_get_default_openai_model();
      } else if (resolved->cloud_provider == CLOUD_PROVIDER_CLAUDE) {
         resolved->model = llm_get_default_claude_model();
      } else if (resolved->cloud_provider == CLOUD_PROVIDER_GEMINI) {
         resolved->model = llm_get_default_gemini_model();
      } else if (resolved->cloud_provider == CLOUD_PROVIDER_OPENROUTER) {
         resolved->model = llm_get_default_openrouter_model();
      }
   }

   // Resolve tool_mode - use session config if set, otherwise global config
   if (session_config->tool_mode[0] != '\0') {
      strncpy(resolved->tool_mode, session_config->tool_mode, sizeof(resolved->tool_mode) - 1);
      resolved->tool_mode[sizeof(resolved->tool_mode) - 1] = '\0';
   } else if (g_config.llm.tools.mode[0] != '\0') {
      strncpy(resolved->tool_mode, g_config.llm.tools.mode, sizeof(resolved->tool_mode) - 1);
      resolved->tool_mode[sizeof(resolved->tool_mode) - 1] = '\0';
   } else {
      strncpy(resolved->tool_mode, "native", sizeof(resolved->tool_mode) - 1);
   }

   // Resolve thinking_mode - use session config if set, otherwise global config
   if (session_config->thinking_mode[0] != '\0') {
      strncpy(resolved->thinking_mode, session_config->thinking_mode,
              sizeof(resolved->thinking_mode) - 1);
      resolved->thinking_mode[sizeof(resolved->thinking_mode) - 1] = '\0';
   } else if (g_config.llm.thinking.mode[0] != '\0') {
      strncpy(resolved->thinking_mode, g_config.llm.thinking.mode,
              sizeof(resolved->thinking_mode) - 1);
      resolved->thinking_mode[sizeof(resolved->thinking_mode) - 1] = '\0';
   } else {
      strncpy(resolved->thinking_mode, "auto", sizeof(resolved->thinking_mode) - 1);
   }

   // Resolve reasoning_effort - use session config if set, otherwise global config
   if (session_config->reasoning_effort[0] != '\0') {
      strncpy(resolved->reasoning_effort, session_config->reasoning_effort,
              sizeof(resolved->reasoning_effort) - 1);
      resolved->reasoning_effort[sizeof(resolved->reasoning_effort) - 1] = '\0';
   } else if (g_config.llm.thinking.reasoning_effort[0] != '\0') {
      strncpy(resolved->reasoning_effort, g_config.llm.thinking.reasoning_effort,
              sizeof(resolved->reasoning_effort) - 1);
      resolved->reasoning_effort[sizeof(resolved->reasoning_effort) - 1] = '\0';
   } else {
      strncpy(resolved->reasoning_effort, "medium", sizeof(resolved->reasoning_effort) - 1);
   }

   /* Stabilize `model` into the config's own inline model_buf.  Until here it may
    * still alias the CALLER's session_config->model — a stack local for every
    * caller (llm_call_prepare, llm_get_current_resolved_config, ...) that is
    * popped the moment this function's caller returns, leaving resolved->model
    * dangling.  The next strncpy() from it then reads clobbered stack as a
    * garbage model name (observed as a Claude 400 "str is not valid UTF-8" on a
    * multi-iteration turn that survives a mid-turn client disconnect — the
    * disconnect-survival path lets iteration 1 run, where before it was aborted).
    * session_config is still alive HERE (caller passed it by pointer), so the
    * copy is valid; afterward the resolved config is self-contained for `model`.
    * (endpoint/api_key alias global config or secret buffers and stay valid; only
    * `model` can alias a per-call local — a custom-endpoint sibling gap remains,
    * but no struct buffer backs endpoint today.) */
   if (resolved->model != NULL && resolved->model != resolved->model_buf) {
      strncpy(resolved->model_buf, resolved->model, sizeof(resolved->model_buf) - 1);
      resolved->model_buf[sizeof(resolved->model_buf) - 1] = '\0';
      resolved->model = resolved->model_buf;
   }

   return 0;
}

char *llm_chat_completion_with_config(struct json_object *conversation_history,
                                      const char *input_text,
                                      const char **vision_images,
                                      const size_t *vision_image_sizes,
                                      int vision_image_count,
                                      const llm_resolved_config_t *config) {
   /* Reset per-call so callers reading llm_last_error() after a NULL return
    * see only THIS call's outcome, not a stale signal from an earlier call
    * on the same thread.  Mirrored at every public chat-completion entry
    * point so the contract holds regardless of which variant the caller
    * uses. */
   llm_set_last_error(LLM_ERR_NONE);

   if (!config) {
      // No config provided, use global (with fallback enabled)
      return llm_chat_completion(conversation_history, input_text, vision_images,
                                 vision_image_sizes, vision_image_count, true);
   }

   char *response = NULL;

   // Resolve default endpoint for cloud providers if not specified
   const char *endpoint = config->endpoint;
   if (endpoint == NULL || endpoint[0] == '\0') {
      if (config->type == LLM_LOCAL) {
         endpoint = g_config.llm.local.endpoint;
      } else if (config->cloud_provider == CLOUD_PROVIDER_OPENAI) {
         endpoint = CLOUDAI_URL;
      } else if (config->cloud_provider == CLOUD_PROVIDER_CLAUDE) {
         endpoint = CLAUDE_URL;
      } else if (config->cloud_provider == CLOUD_PROVIDER_GEMINI) {
         endpoint = GEMINI_URL;
      } else if (config->cloud_provider == CLOUD_PROVIDER_OPENROUTER) {
         endpoint = OPENROUTER_URL;
      }
   }

   // Set thread-local config so llm_tools_enabled() can check session-specific tool_mode
   llm_tools_set_current_config(config);

   /* Set thread-local timeout override if specified (avoids global config race) */
   int saved_tl_timeout = s_tl_timeout_ms;
   if (config->timeout_ms > 0) {
      s_tl_timeout_ms = config->timeout_ms;
   }

   /* Gate cloud API calls through rate limiter */
   if (config->type != LLM_LOCAL) {
      if (llm_rate_limit_wait())
         return NULL; /* interrupted */
   }

   if (config->type == LLM_LOCAL) {
      // Local LLM uses OpenAI-compatible API (no API key needed)
      response = llm_openai_chat_completion(conversation_history, input_text, vision_images,
                                            vision_image_sizes, vision_image_count, endpoint, NULL,
                                            config->model);
   } else {
      // Route to cloud provider
      switch (config->cloud_provider) {
         case CLOUD_PROVIDER_OPENAI:
            response = llm_openai_chat_completion(conversation_history, input_text, vision_images,
                                                  vision_image_sizes, vision_image_count, endpoint,
                                                  config->api_key, config->model);
            break;

         case CLOUD_PROVIDER_CLAUDE:
            response = llm_claude_chat_completion(conversation_history, input_text, vision_images,
                                                  vision_image_sizes, vision_image_count, endpoint,
                                                  config->api_key, config->model);
            break;

         case CLOUD_PROVIDER_GEMINI:
         case CLOUD_PROVIDER_OPENROUTER:
            /* Gemini and OpenRouter both use the OpenAI-compatible API */
            response = llm_openai_chat_completion(conversation_history, input_text, vision_images,
                                                  vision_image_sizes, vision_image_count, endpoint,
                                                  config->api_key, config->model);
            break;

         default:
            OLOG_ERROR("No cloud provider configured in session config");
            llm_tools_set_current_config(NULL);
            return NULL;
      }
   }

   // Clear thread-local state
   llm_tools_set_current_config(NULL);
   s_tl_timeout_ms = saved_tl_timeout;

   // Note: No automatic fallback for per-session config - caller handles this

   return response;
}

char *llm_chat_completion_streaming_with_config(struct json_object *conversation_history,
                                                const char *input_text,
                                                const char **vision_images,
                                                const size_t *vision_image_sizes,
                                                int vision_image_count,
                                                llm_text_chunk_callback chunk_callback,
                                                void *callback_userdata,
                                                const llm_resolved_config_t *config) {
   llm_set_last_error(LLM_ERR_NONE); /* see contract on llm_chat_completion_with_config */
   // Clear any previous interrupt flag from cancelled requests
   llm_clear_interrupt();

   if (!config) {
      // No config provided, use global (with fallback enabled)
      return llm_chat_completion_streaming(conversation_history, input_text, vision_images,
                                           vision_image_sizes, vision_image_count, chunk_callback,
                                           callback_userdata, true);
   }

   char *response = NULL;

   // Track LLM total time
   struct timeval start_time, end_time;
   gettimeofday(&start_time, NULL);

   // Record query metrics
   metrics_record_llm_query(config->type);

   // Set thread-local config so llm_tools_enabled() can check session-specific tool_mode
   llm_tools_set_current_config(config);

   // Determine provider function and history format
   session_t *session = session_get_command_context();
   uint32_t session_id = session ? session->session_id : 0;
   llm_single_shot_fn provider_fn;
   llm_history_format_t history_format;

   if (config->type == LLM_CLOUD && config->cloud_provider == CLOUD_PROVIDER_CLAUDE) {
      provider_fn = (llm_single_shot_fn)llm_claude_streaming_single_shot;
      history_format = LLM_HISTORY_CLAUDE;
   } else {
      provider_fn = (llm_single_shot_fn)llm_openai_streaming_single_shot;
      history_format = LLM_HISTORY_OPENAI;
   }

   // Run the central tool iteration loop
   llm_tool_loop_params_t loop_params = {
      .conversation_history = conversation_history,
      .input_text = input_text,
      .vision_images = vision_images,
      .vision_image_sizes = vision_image_sizes,
      .vision_image_count = vision_image_count,
      .base_url = config->endpoint,
      .api_key = (config->type == LLM_LOCAL) ? NULL : config->api_key,
      .model = config->model,
      .chunk_callback = (void *)chunk_callback,
      .callback_userdata = callback_userdata,
      .provider_fn = provider_fn,
      .history_format = history_format,
      .session_id = session_id,
      .llm_type = config->type,
      .cloud_provider = config->cloud_provider,
   };

   response = llm_tool_iteration_loop(&loop_params);

   /* Trigger async compaction for next turn (WebUI sessions) */
   {
      session_t *trigger_session = session_get(loop_params.session_id);
      if (trigger_session) {
         llm_context_async_trigger(trigger_session, loop_params.conversation_history,
                                   loop_params.llm_type, loop_params.cloud_provider,
                                   loop_params.model);
         session_release(trigger_session);
      }
   }

   // Clear thread-local config
   llm_tools_set_current_config(NULL);

   // Record LLM total time
   if (response != NULL) {
      gettimeofday(&end_time, NULL);
      double total_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 +
                        (end_time.tv_usec - start_time.tv_usec) / 1000.0;
      metrics_record_llm_total_time(total_ms);
   }

   return response;
}

char *llm_chat_completion_streaming_tts_with_config(struct json_object *conversation_history,
                                                    const char *input_text,
                                                    const char **vision_images,
                                                    const size_t *vision_image_sizes,
                                                    int vision_image_count,
                                                    llm_sentence_callback sentence_callback,
                                                    void *callback_userdata,
                                                    const llm_resolved_config_t *config) {
   llm_set_last_error(LLM_ERR_NONE); /* see contract on llm_chat_completion_with_config */
   if (!config) {
      // No config provided, use global (with fallback enabled)
      return llm_chat_completion_streaming_tts(conversation_history, input_text, vision_images,
                                               vision_image_sizes, vision_image_count,
                                               sentence_callback, callback_userdata, true);
   }

   char *response = NULL;
   tts_streaming_context_t ctx;

   // Create sentence buffer
   ctx.sentence_buffer = sentence_buffer_create(tts_sentence_callback, &ctx);
   if (!ctx.sentence_buffer) {
      OLOG_ERROR("Failed to create sentence buffer for TTS streaming");
      return NULL;
   }

   ctx.user_callback = sentence_callback;
   ctx.user_userdata = callback_userdata;

   // Call streaming with chunk callback that feeds sentence buffer
   response = llm_chat_completion_streaming_with_config(conversation_history, input_text,
                                                        vision_images, vision_image_sizes,
                                                        vision_image_count, tts_chunk_callback,
                                                        &ctx, config);

   // Flush any remaining sentence
   sentence_buffer_flush(ctx.sentence_buffer);

   // Cleanup
   sentence_buffer_free(ctx.sentence_buffer);

   return response;
}
