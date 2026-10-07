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
 * Shared by the email backends' transfers (email_transfer.h).
 */

#include "tools/email_transfer.h"

const char *email_error_name(email_err_t err) {
   switch (err) {
      case EMAIL_ERR_NONE:
         return "";
      case EMAIL_ERR_AUTH_FAILED:
         return "AUTH_FAILED";
      case EMAIL_ERR_AUTH_REVOKED:
         return "AUTH_REVOKED";
      case EMAIL_ERR_UNREACHABLE:
         return "UNREACHABLE";
      case EMAIL_ERR_TIMEOUT:
         return "TIMEOUT";
      case EMAIL_ERR_RATE_LIMITED:
         return "RATE_LIMITED";
      case EMAIL_ERR_NOT_FOUND:
         return "NOT_FOUND";
      case EMAIL_ERR_READ_ONLY:
         return "READ_ONLY";
      case EMAIL_ERR_FOLDER_MISSING:
         return "FOLDER_MISSING";
      case EMAIL_ERR_NO_TRASH:
         return "NO_TRASH";
      case EMAIL_ERR_NO_ACCOUNT:
         return "NO_ACCOUNT";
      case EMAIL_ERR_ACCOUNT_NOT_FOUND:
         return "ACCOUNT_NOT_FOUND";
      case EMAIL_ERR_CANCELLED:
         return "CANCELLED";
      case EMAIL_ERR_CURSOR_STALE:
         return "CURSOR_STALE";
      case EMAIL_ERR_SUPERSEDED:
         return "SUPERSEDED";
      case EMAIL_ERR_UNSUPPORTED_QUERY:
         return "UNSUPPORTED_QUERY";
      case EMAIL_ERR_BUSY:
         return "BUSY";
      case EMAIL_ERR_SHUTTING_DOWN:
         return "SHUTTING_DOWN";
      case EMAIL_ERR_CANNOT_CALCULATE:
         return "CANNOT_CALCULATE";
      case EMAIL_ERR_INVALID_REQUEST:
         return "INVALID_REQUEST";
      case EMAIL_ERR_UNAVAILABLE:
         return "UNAVAILABLE";
      case EMAIL_ERR_FAILED:
         break;
   }
   return "FAILED";
}

email_err_t email_err_from_curl(CURLcode res) {
   switch (res) {
      case CURLE_OK:
         return EMAIL_ERR_NONE;
      case CURLE_LOGIN_DENIED:
         return EMAIL_ERR_AUTH_FAILED;
      case CURLE_COULDNT_RESOLVE_HOST:
      case CURLE_COULDNT_RESOLVE_PROXY:
      case CURLE_COULDNT_CONNECT:
      case CURLE_SSL_CONNECT_ERROR:
      case CURLE_PEER_FAILED_VERIFICATION:
         return EMAIL_ERR_UNREACHABLE;
      case CURLE_OPERATION_TIMEDOUT:
         return EMAIL_ERR_TIMEOUT;
      case CURLE_REMOTE_FILE_NOT_FOUND:
         /* IMAP: no such message (a FETCH with no answer) or no such folder */
         return EMAIL_ERR_NOT_FOUND;
      case CURLE_ABORTED_BY_CALLBACK:
         return EMAIL_ERR_CANCELLED;
      default:
         return EMAIL_ERR_FAILED;
   }
}

static int cancel_progress(void *clientp,
                           curl_off_t dltotal,
                           curl_off_t dlnow,
                           curl_off_t ultotal,
                           curl_off_t ulnow) {
   (void)dltotal;
   (void)dlnow;
   (void)ultotal;
   (void)ulnow;
   return atomic_load((const atomic_bool *)clientp) ? 1 : 0;
}

void email_transfer_set_cancel(CURL *curl, const atomic_bool *cancel) {
   if (!curl || !cancel)
      return;
   curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancel_progress);
   curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void *)cancel);
   curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
}

void email_transfer_clear_cancel(CURL *curl) {
   if (!curl)
      return;
   curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
   curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, NULL);
   curl_easy_setopt(curl, CURLOPT_XFERINFODATA, NULL);
}

/* Thread-local, so a worker serving one request can't stop another's transfers. */
static __thread const atomic_bool *s_thread_cancel;

const atomic_bool *email_transfer_scope_cancel(const atomic_bool *cancel) {
   const atomic_bool *prev = s_thread_cancel;
   s_thread_cancel = cancel;
   return prev;
}

const atomic_bool *email_transfer_thread_cancel(void) {
   return s_thread_cancel;
}
