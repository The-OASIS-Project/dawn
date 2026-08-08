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
 * the project author(s).
 *
 * OTA device-apply (Tier-1) — see ota_apply.h.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ota_apply.h"

#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <pthread.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "core/ota_manifest.h"
#include "dawn_error.h"
#include "logging.h"
#include "ota_apply_internal.h"
#include "ota_marker.h"
#include "satellite_version.h"

/* The offer accept/reject decision + the keyring/abi helpers live in
 * ota_apply_decide.c (pure, host-testable); OTA_KEYRING_*, OTA_DETAIL_BUF,
 * OTA_OFFER_TOKEN_HEX and OTA_URL_PATH_MAX come from ota_apply_internal.h. */

/* The marker's version fields must mirror the manifest's so a target_version
 * written from m.version never truncates.  Pinned here (the one TU that includes
 * both headers) — the launcher's marker codec stays dependency-free. */
_Static_assert(OTA_VERSION_STR_MAX == OTA_VERSION_MAX,
               "ota_marker version field must mirror OTA_VERSION_MAX");

/* Device-reported OTA states (must match the server's OTA_STATE_* strings). */
#define OTA_ST_DOWNLOADING "downloading"
#define OTA_ST_VERIFYING "verifying"
#define OTA_ST_APPLYING "applying"
#define OTA_ST_REBOOTING "rebooting"
#define OTA_ST_FAILED "failed"

#define OTA_STATE_BUF 24
/* Free-space preflight reserves two binary-sized slots: the staged download
 * AND the rollback copy of the running binary (plus OTA_FREE_SLACK_BYTES). */
#define OTA_APPLY_BIN_COPIES 2

/* ---- Shared state: single-flight guard + status mailbox (worker → WS thread) ---- */
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_inflight = false;
static struct {
   char state[OTA_STATE_BUF];
   char detail[OTA_DETAIL_BUF];
   bool pending;
} g_status;

/* Worker context — owns COPIES of everything; never derefs the ws_client. */
typedef struct {
   char host[256];
   char ca_cert_path[256];
   char uuid[64];
   char url_path[OTA_URL_PATH_MAX];
   char token[OTA_OFFER_TOKEN_HEX + 1];
   char target_version[OTA_VERSION_STR_MAX];
   uint16_t port;
   bool use_ssl;
   bool ssl_verify; /* Honor the satellite's ssl_verify flag on the HTTPS pull too */
   uint64_t image_size;
   uint8_t sha256[OTA_SHA256_BYTES];
} ota_work_t;

/* Record a status transition for the WS thread to flush (latest-wins). */
static void set_status(const char *state, const char *detail) {
   pthread_mutex_lock(&g_mutex);
   snprintf(g_status.state, sizeof(g_status.state), "%s", state ? state : "");
   snprintf(g_status.detail, sizeof(g_status.detail), "%s", detail ? detail : "");
   g_status.pending = true;
   pthread_mutex_unlock(&g_mutex);
   OLOG_INFO("OTA: %s%s%s", state ? state : "", (detail && detail[0]) ? ": " : "",
             (detail && detail[0]) ? detail : "");
}

static void clear_inflight(void) {
   pthread_mutex_lock(&g_mutex);
   g_inflight = false;
   pthread_mutex_unlock(&g_mutex);
}

/* ---- abi_tag: "<os_id>-<version_codename>-<machine>", e.g. debian-trixie-aarch64 ---- */
/* ---- libcurl download into a held fd, size-capped ---- */
typedef struct {
   int fd;
   uint64_t written;
   uint64_t cap;
} dl_sink_t;

static size_t dl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
   dl_sink_t *s = (dl_sink_t *)userdata;
   uint64_t n = (uint64_t)size * nmemb;
   if (s->written + n > s->cap) {
      return 0; /* exceed signed image_size → abort transfer */
   }
   size_t off = 0;
   while (off < n) {
      ssize_t w = write(s->fd, ptr + off, (size_t)(n - off));
      if (w <= 0) {
         return 0;
      }
      off += (size_t)w;
   }
   s->written += n;
   return (size_t)n; /* == size*nmemb → curl treats the chunk as fully consumed */
}

static int download_to_fd(const ota_work_t *w, int fd) {
   /* Sized to hold scheme + host(256) + port + url_path(256) + uuid(64) +
    * token(64) + separators with margin, so a valid offer never truncates. */
   char url[1024];
   snprintf(url, sizeof(url), "%s://%s:%u%s?uuid=%s&token=%s", w->use_ssl ? "https" : "http",
            w->host, (unsigned)w->port, w->url_path, w->uuid, w->token);

   CURL *c = curl_easy_init();
   if (!c) {
      return FAILURE;
   }
   dl_sink_t sink = { .fd = fd, .written = 0, .cap = w->image_size };
   curl_easy_setopt(c, CURLOPT_URL, url);
   curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, dl_write_cb);
   curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
   curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
   curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
   /* Honor the satellite's ssl_verify flag, same as the WebSocket connection. OTA
    * images are independently libsodium-signature-verified, so TLS verification is
    * defense-in-depth here — a self-signed / verify-off dev deployment can still OTA
    * (previously the hardcoded verify made ssl_verify=false silently un-OTA-able). */
   curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, w->ssl_verify ? 1L : 0L);
   curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, w->ssl_verify ? 2L : 0L);
   curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
   curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
   if (w->ca_cert_path[0]) {
      curl_easy_setopt(c, CURLOPT_CAINFO, w->ca_cert_path);
   }
   CURLcode rc = curl_easy_perform(c);
   if (rc != CURLE_OK) {
      /* Log the real cause — the caller only reports a generic "download failed".
       * Omit the query string (it carries the one-time token). */
      OLOG_WARNING("OTA: image download failed: %s (%s://%s:%u%s, verify=%s)",
                   curl_easy_strerror(rc), w->use_ssl ? "https" : "http", w->host,
                   (unsigned)w->port, w->url_path, w->ssl_verify ? "on" : "off");
   }
   curl_easy_cleanup(c);
   return (rc == CURLE_OK) ? SUCCESS : FAILURE;
}

/* ---- fd-based helpers ---- */
/* Durable-rename dir fsync lives in the shared marker codec (ota_fsync_dir_of). */

/* Copy /proc/self/exe (the running binary) → dst atomically (tmp+rename+fsync).
 * Returns SUCCESS or FAILURE. */
static int copy_self_to(const char *dst) {
   int src = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
   if (src < 0) {
      return FAILURE;
   }
   char tmp[600];
   snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
   unlink(tmp);
   int out = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0755);
   if (out < 0) {
      close(src);
      return FAILURE;
   }
   char buf[OTA_IO_CHUNK_BYTES];
   ssize_t r;
   int ok = 1;
   while ((r = read(src, buf, sizeof(buf))) > 0) {
      ssize_t off = 0;
      while (off < r) {
         ssize_t w = write(out, buf + off, (size_t)(r - off));
         if (w <= 0) {
            ok = 0;
            break;
         }
         off += w;
      }
      if (!ok) {
         break;
      }
   }
   if (r < 0) {
      ok = 0;
   }
   close(src);
   if (ok && fsync(out) != 0) {
      ok = 0;
   }
   close(out);
   if (!ok || rename(tmp, dst) != 0) {
      unlink(tmp);
      return FAILURE;
   }
   ota_fsync_dir_of(dst);
   return SUCCESS;
}

/* Hash the staged file by streaming it through the held fd in fixed chunks (no
 * reopen-by-path → no TOCTOU vs the rename of this same inode; no whole-image
 * heap allocation — peak RAM stays a single 64 KB buffer). */
static int hash_fd(int fd, uint64_t size, uint8_t out[OTA_SHA256_BYTES]) {
   if (lseek(fd, 0, SEEK_SET) != 0) {
      return FAILURE;
   }
   crypto_hash_sha256_state st;
   if (crypto_hash_sha256_init(&st) != 0) {
      return FAILURE;
   }
   uint8_t buf[OTA_IO_CHUNK_BYTES];
   uint64_t got = 0;
   while (got < size) {
      size_t want = (size - got < sizeof(buf)) ? (size_t)(size - got) : sizeof(buf);
      ssize_t r = read(fd, buf, want);
      if (r <= 0) {
         return FAILURE;
      }
      crypto_hash_sha256_update(&st, buf, (size_t)r);
      got += (uint64_t)r;
   }
   return (crypto_hash_sha256_final(&st, out) == 0) ? SUCCESS : FAILURE;
}

/* Worker failure epilogue: report `failed`, optionally unlink the staged file,
 * release single-flight, free the ctx.  Use as `return worker_fail(...)`. */
static void *worker_fail(ota_work_t *w, const char *staged, const char *msg) {
   if (staged) {
      unlink(staged);
   }
   set_status(OTA_ST_FAILED, msg);
   clear_inflight();
   free(w);
   return NULL;
}

/* ---- the detached apply worker ---- */
static void *apply_worker(void *arg) {
   ota_work_t *w = (ota_work_t *)arg;

   set_status(OTA_ST_DOWNLOADING, NULL);

   mkdir(OTA_TMP_DIR, 0700);
   mkdir(OTA_ROLLBACK_DIR, 0700);

   /* Free-space preflight: need room for the staged download + the rollback
    * copy of the running binary, plus FTL slack. */
   struct stat selfst;
   uint64_t self_sz = (stat("/proc/self/exe", &selfst) == 0) ? (uint64_t)selfst.st_size : 0;
   uint64_t need = ((w->image_size > self_sz ? w->image_size : self_sz) * OTA_APPLY_BIN_COPIES) +
                   OTA_FREE_SLACK_BYTES;
   struct statvfs vfs;
   if (statvfs(OTA_BIN_DIR, &vfs) == 0) {
      uint64_t avail = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
      if (avail < need) {
         return worker_fail(w, NULL, "insufficient disk space");
      }
   }

   char staged[600];
   snprintf(staged, sizeof(staged), "%s/dawn_satellite.XXXXXX", OTA_TMP_DIR);
   int fd = mkstemp(staged);
   if (fd < 0) {
      return worker_fail(w, NULL, "cannot create staging file");
   }

   int failed = 1;
   const char *failmsg = "download failed";
   if (download_to_fd(w, fd) == SUCCESS) {
      set_status(OTA_ST_VERIFYING, NULL);
      struct stat st;
      uint8_t got[OTA_SHA256_BYTES];
      if (fstat(fd, &st) == 0 && (uint64_t)st.st_size == w->image_size &&
          hash_fd(fd, w->image_size, got) == SUCCESS &&
          sodium_memcmp(got, w->sha256, OTA_SHA256_BYTES) == 0) {
         if (fchmod(fd, 0755) == 0) {
            failed = 0;
         } else {
            failmsg = "chmod failed";
         }
      } else {
         failmsg = "image hash mismatch";
      }
   }
   close(fd);

   if (failed) {
      return worker_fail(w, staged, failmsg);
   }

   set_status(OTA_ST_APPLYING, NULL);

   /* Save a rollback copy of the CURRENT binary, then validate it before we
    * commit to swapping (a corrupt rollback copy must not reach the launcher). */
   if (copy_self_to(OTA_ROLLBACK_BIN) != SUCCESS) {
      return worker_fail(w, staged, "could not save rollback copy");
   }
   struct stat rb;
   if (stat(OTA_ROLLBACK_BIN, &rb) != 0 || self_sz == 0 || (uint64_t)rb.st_size != self_sz ||
       !(rb.st_mode & S_IXUSR)) {
      return worker_fail(w, staged, "rollback copy validation failed");
   }

   /* Write the pending marker (durable) BEFORE the swap so a power-cut between
    * swap and first boot is still seen as a pending OTA. */
   ota_marker_t m = { 0 };
   m.marker_version = OTA_MARKER_VERSION;
   snprintf(m.target_version, sizeof(m.target_version), "%s", w->target_version);
   snprintf(m.prev_version, sizeof(m.prev_version), "%s", DAWN_SATELLITE_FIRMWARE_VERSION);
   m.boots = 0;
   m.created = (int64_t)time(NULL);
   m.ran_ok = 0;
   if (ota_marker_write(OTA_PENDING_MARKER, &m) != SUCCESS) {
      return worker_fail(w, staged, "could not write pending marker");
   }

   /* Atomic swap: same filesystem → rename is atomic; Linux allows renaming
    * over a running ELF (this process keeps its mapped text). */
   if (rename(staged, OTA_BIN_PATH) != 0) {
      ota_marker_remove(OTA_PENDING_MARKER); /* undo the marker — no swap happened */
      return worker_fail(w, staged, "binary swap failed");
   }
   ota_fsync_dir_of(OTA_BIN_PATH);

   set_status(OTA_ST_REBOOTING, NULL);
   free(w);
   /* Give the WS service thread (100 ms flush cadence) a beat to push the last
    * status; delivery isn't guaranteed — the server finalizes on re-register. */
   usleep(400000);
   OLOG_INFO("OTA: applied %s — exiting for launcher re-exec", m.target_version);
   _exit(0); /* launcher re-execs the new binary */
   return NULL;
}

/* ---- offer handling (WS thread) ---- */
void ota_apply_handle_offer(ws_client_t *client,
                            const char *uuid,
                            const char *host,
                            uint16_t port,
                            bool use_ssl,
                            bool ssl_verify,
                            const char *ca_cert_path,
                            struct json_object *payload) {
   /* uuid is interpolated raw into the download query string, so constrain it to
    * a safe charset (it is our own registered UUID, but defense in depth keeps a
    * stray '&'/'#' from splitting query params). */
   if (!uuid || uuid[0] == '\0' || strlen(uuid) >= 64) {
      ws_client_send_ota_reject(client, "malformed offer");
      return;
   }
   for (const char *c = uuid; *c; c++) {
      if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
            *c == '-')) {
         ws_client_send_ota_reject(client, "malformed offer");
         return;
      }
   }

   /* I/O the pure decision needs: load the operator keyring + compute our abi. */
   uint8_t keyring[OTA_KEYRING_MAX][OTA_PK_BYTES];
   size_t n_keys = load_keyring(keyring, OTA_KEYRING_MAX);
   char abi[OTA_ABI_TAG_MAX];
   compute_abi_tag(abi, sizeof(abi));

   /* Accept/reject is decided by the pure, host-tested ota_apply_decide(). */
   ota_manifest_t m;
   char reason[OTA_DETAIL_BUF];
   if (ota_apply_decide(payload, DAWN_SATELLITE_FIRMWARE_VERSION, abi, keyring, n_keys, &m, reason,
                        sizeof(reason)) != OTA_DECIDE_ACCEPT) {
      ws_client_send_ota_reject(client, reason);
      return;
   }

   /* Accepted — the transport fields are validated; re-read them for the worker. */
   const char *url_path = ota_jstr(payload, "url_path");
   const char *token = ota_jstr(payload, "token");

   /* Single-flight: one apply at a time. */
   pthread_mutex_lock(&g_mutex);
   if (g_inflight) {
      pthread_mutex_unlock(&g_mutex);
      ws_client_send_ota_reject(client, "busy");
      return;
   }
   g_inflight = true;
   pthread_mutex_unlock(&g_mutex);

   ota_work_t *w = calloc(1, sizeof(*w));
   if (!w) {
      clear_inflight();
      ws_client_send_ota_reject(client, "out of memory");
      return;
   }
   snprintf(w->host, sizeof(w->host), "%s", host ? host : "");
   snprintf(w->ca_cert_path, sizeof(w->ca_cert_path), "%s", ca_cert_path ? ca_cert_path : "");
   snprintf(w->uuid, sizeof(w->uuid), "%s", uuid);
   snprintf(w->url_path, sizeof(w->url_path), "%s", url_path);
   snprintf(w->token, sizeof(w->token), "%s", token);
   snprintf(w->target_version, sizeof(w->target_version), "%s", m.version);
   w->port = port;
   w->use_ssl = use_ssl;
   w->ssl_verify = ssl_verify;
   w->image_size = m.image_size;
   memcpy(w->sha256, m.sha256, OTA_SHA256_BYTES);

   ws_client_send_ota_ack(client);

   pthread_t tid;
   if (pthread_create(&tid, NULL, apply_worker, w) != 0) {
      clear_inflight();
      free(w);
      set_status(OTA_ST_FAILED, "could not start worker");
      return;
   }
   pthread_detach(tid);
}

void ota_apply_flush_status(ws_client_t *client) {
   char state[OTA_STATE_BUF], detail[OTA_DETAIL_BUF];
   pthread_mutex_lock(&g_mutex);
   if (!g_status.pending) {
      pthread_mutex_unlock(&g_mutex);
      return;
   }
   memcpy(state, g_status.state, sizeof(state));
   memcpy(detail, g_status.detail, sizeof(detail));
   g_status.pending = false;
   pthread_mutex_unlock(&g_mutex);
   ws_client_send_ota_status(client, state, detail[0] ? detail : NULL);
}

void ota_apply_mark_healthy(void) {
   /* Commit point: clear probation.  Idempotent (no marker → no-op). */
   if (ota_marker_read(OTA_PENDING_MARKER, NULL) == OTA_MARKER_OK) {
      ota_marker_remove(OTA_PENDING_MARKER);
      OLOG_INFO("OTA: registered healthy — pending update committed");
   }
}

#define OTA_LIVENESS_SEC 60

void ota_apply_mark_running(void) {
   /* Liveness: once the process has run ≥60 s, set ran_ok=1 so the launcher
    * commits even without a server register (server-outage case).  Self-
    * throttled: a cheap time-compare until the window elapses, then exactly one
    * marker action, then a permanent no-op.  Safe to call at high frequency. */
   static time_t s_start = 0;
   static bool s_done = false;
   if (s_done) {
      return;
   }
   time_t now = time(NULL);
   if (s_start == 0) {
      s_start = now;
      return;
   }
   if (now - s_start < OTA_LIVENESS_SEC) {
      return;
   }
   /* Window elapsed — act once regardless of outcome. */
   s_done = true;
   ota_marker_t m;
   if (ota_marker_read(OTA_PENDING_MARKER, &m) != OTA_MARKER_OK || m.ran_ok) {
      return; /* no pending update, or already flagged */
   }
   m.ran_ok = 1;
   if (ota_marker_write(OTA_PENDING_MARKER, &m) == SUCCESS) {
      OLOG_INFO("OTA: new build stable ≥%ds — marked ran_ok", OTA_LIVENESS_SEC);
   }
}

void ota_apply_cleanup_tmp(void) {
   /* Best-effort sweep of stale staged downloads from a prior interrupted apply. */
   DIR *d = opendir(OTA_TMP_DIR);
   if (!d) {
      return;
   }
   struct dirent *e;
   while ((e = readdir(d)) != NULL) {
      if (strncmp(e->d_name, "dawn_satellite.", 15) != 0) {
         continue;
      }
      char path[600];
      snprintf(path, sizeof(path), "%s/%s", OTA_TMP_DIR, e->d_name);
      unlink(path);
   }
   closedir(d);
}
