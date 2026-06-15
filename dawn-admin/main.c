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
 * dawn-admin: Administrative CLI for Dawn daemon management.
 *
 * Implements user and session management commands for the Dawn daemon.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "password_prompt.h"
#include "socket_client.h"

#define VERSION "2.0.0"

static void print_usage(const char *prog) {
   fprintf(stderr, "Dawn Admin CLI v%s\n\n", VERSION);
   fprintf(stderr, "Usage: %s <command> [options]\n\n", prog);
   fprintf(stderr, "Commands:\n");
   fprintf(stderr, "  ping                              Test connection to daemon\n");
   fprintf(stderr, "\n");
   fprintf(stderr, "User Management:\n");
   fprintf(stderr, "  user list                         List all users\n");
   fprintf(stderr,
           "  user create <username> --admin    Create admin user (uses DAWN_SETUP_TOKEN)\n");
   fprintf(stderr, "  user delete <username> [--yes]    Delete a user account\n");
   fprintf(stderr, "  user passwd <username>            Change user password\n");
   fprintf(stderr, "  user unlock <username>            Unlock a locked account\n");
   fprintf(stderr, "\n");
   fprintf(stderr, "Session Management:\n");
   fprintf(stderr, "  session list                      List active sessions\n");
   fprintf(stderr, "  session revoke <token_prefix>     Revoke a specific session\n");
   fprintf(stderr, "  session revoke --user <username>  Revoke all sessions for a user\n");
   fprintf(stderr, "\n");
   fprintf(stderr, "Database Management:\n");
   fprintf(stderr, "  db status                         Show database statistics\n");
   fprintf(stderr, "  db compact                        Compact database (rate-limited)\n");
   fprintf(stderr, "  db backup <path>                  Backup database to file\n");
   fprintf(stderr, "\n");
   fprintf(stderr, "Audit Log:\n");
   fprintf(stderr, "  log show [options]                Show recent audit log entries\n");
   fprintf(stderr, "    --last N                        Show last N entries (default 50)\n");
   fprintf(stderr, "    --type <event>                  Filter by event type\n");
   fprintf(stderr, "    --user <username>               Filter by username\n");
   fprintf(stderr, "\n");
   fprintf(stderr, "IP Management:\n");
   fprintf(stderr, "  ip list                           List IPs with failed login attempts\n");
   fprintf(stderr, "  ip unblock <ip-address>           Unblock a rate-limited IP address\n");
   fprintf(stderr, "  ip unblock --all                  Unblock all IP addresses\n");
   fprintf(stderr, "\n");
   fprintf(stderr, "Session Metrics:\n");
   fprintf(stderr, "  metrics list [options]            List session metrics history\n");
   fprintf(stderr, "    --last N                        Show last N entries (default 20)\n");
   fprintf(stderr,
           "    --type <type>                   Filter by session type (LOCAL, WEBSOCKET)\n");
   fprintf(stderr, "  metrics summary                   Show aggregate metrics totals\n");
   fprintf(stderr, "\n");
   fprintf(stderr, "Conversations:\n");
   fprintf(stderr, "  conv list [options]               List conversations\n");
   fprintf(stderr, "    --last N                        Show last N entries (default 20)\n");
   fprintf(stderr, "    --archived, -a                  Include archived conversations\n");
   fprintf(stderr, "  conv show <id>                    Show conversation messages\n");
   fprintf(stderr, "  conv delete <id>                  Delete a conversation\n");
   fprintf(stderr, "\n");
   fprintf(stderr, "Music Database:\n");
   fprintf(stderr, "  music stats                       Show music database statistics\n");
   fprintf(stderr, "  music search <query>              Search by artist/title/album\n");
   fprintf(stderr, "  music list [--limit N]            List tracks (default: all)\n");
   fprintf(stderr, "  music rescan                      Trigger library rescan\n");
   fprintf(stderr, "\nMemory Management:\n");
   fprintf(stderr, "  memory recategorize-all <user>     LLM-classify 'general' facts\n");
   fprintf(stderr, "  memory backfill-note-glosses <user>  Create memory→note bridge glosses for\n"
                   "                                       existing notes (Phase 9, idempotent)\n");
   fprintf(stderr,
           "  memory rebuild-document-fts          Rebuild the document search (FTS) index\n"
           "                                       (v61 recovery; global, idempotent)\n");
   fprintf(stderr,
           "  memory cleanup-meta-facts --user <u> [--dry-run|--confirm-delete]\n"
           "                                     Bulk-delete pre-existing meta-fact rows\n");
   fprintf(stderr, "  memory summarize-missing --user <u> [--dry-run|--confirm] [--max N]\n"
                   "                                     Backfill summaries for conversations\n"
                   "                                     that never produced a summary row\n");
   fprintf(stderr,
           "  memory reextract --user <user> [--confirm] [--keep-summaries]\n"
           "                    [--backup-path <path>] [--max-cost-usd <X>]\n"
           "                                       Drop derived memory tables and re-extract.\n"
           "                                       Defaults to dry-run; --confirm executes.\n");
   fprintf(stderr,
           "  memory reextract-status --user <user>\n"
           "                                       Report progress of last reextract run\n");
   fprintf(stderr, "\nEntity Merge (v43):\n");
   fprintf(stderr,
           "  memory entity list [--user <u>] [--show-aliases]\n"
           "                                       List entities (canonical-only by default)\n"
           "  memory entity merge --user <u> --source <id> --target <id> [--reason <r>]\n"
           "                                       Soft-link source as alias of target\n"
           "  memory entity split --user <u> --link-id <id> [--reason <r>]\n"
           "                                       Reverse a soft alias link\n"
           "  memory entity aliases --user <u> --entity-id <id>\n"
           "                                       List active aliases of a canonical entity\n"
           "  memory entity history --user <u> --entity-id <id>\n"
           "                                       Show full audit timeline for an entity\n"
           "  memory entity link-user-self --user <u> [--dry-run]\n"
           "                                       Path B backfill: seed user-self + link\n"
           "                                       matching entities (dry-run by default)\n");
   fprintf(stderr, "\nMessaging Channels:\n");
   fprintf(stderr,
           "  messaging generate-link-code --user <u>\n"
           "                               [--provider telegram|discord|slack|sms]\n"
           "                                       Issue a one-time link code; the user sends\n"
           "                                       '/link <CODE>' from the chat client to bind\n"
           "                                       it to their DAWN account.\n"
           "  messaging list-channels --user <u>     List the user's linked channels (table).\n"
           "  messaging unlink --user <u> --name <c> Soft-delete (unlink) a channel by name.\n"
           "  messaging reenable --user <u> --name <c>\n"
           "                                       Re-enable a previously unlinked channel.\n"
           "  messaging link-attempts [--provider p] [--limit n]\n"
           "                                       Recent /link attempts (abuse review).\n");
   fprintf(stderr, "\nOTA Updates:\n");
   fprintf(stderr,
           "  ota list                               List available OTA releases.\n"
           "  ota push --uuid <uuid> --version <v> [--allow-downgrade]\n"
           "                                       Offer an update to one online satellite.\n");
   fprintf(stderr, "\nMCP Bridge (coding harness):\n");
   fprintf(stderr,
           "  mcp list                             List connected MCP servers + tool counts\n"
           "  mcp grant <user> <alias>             Grant a user access to an MCP server\n"
           "  mcp revoke <user> <alias>            Revoke a user's access\n"
           "  mcp reset                            Clear DISABLED + reconnect all servers\n");
   fprintf(stderr, "\n");
   fprintf(stderr, "Options:\n");
   fprintf(stderr, "  --yes, -y    Skip confirmation prompts\n");
   fprintf(stderr, "  help         Show this help message\n");
   fprintf(stderr, "\nExamples:\n");
   fprintf(stderr, "  %s user list\n", prog);
   fprintf(stderr, "  %s user delete guest\n", prog);
   fprintf(stderr, "  %s user passwd admin\n", prog);
   fprintf(stderr, "  %s session list\n", prog);
   fprintf(stderr, "  %s session revoke a1b2c3d4\n", prog);
   fprintf(stderr, "  %s session revoke --user guest\n", prog);
   fprintf(stderr, "  %s db status\n", prog);
   fprintf(stderr, "  %s db backup /var/lib/dawn/backup.db\n", prog);
   fprintf(stderr, "  %s log show\n", prog);
   fprintf(stderr, "  %s log show --last 100 --type LOGIN_FAILED\n", prog);
   fprintf(stderr, "  %s ip list\n", prog);
   fprintf(stderr, "  %s ip unblock 192.168.1.100\n", prog);
   fprintf(stderr, "  %s metrics list\n", prog);
   fprintf(stderr, "  %s metrics summary\n", prog);
   fprintf(stderr, "  %s conv list\n", prog);
   fprintf(stderr, "  %s conv show 123\n", prog);
   fprintf(stderr, "  %s music stats\n", prog);
   fprintf(stderr, "  %s music search \"pink floyd\"\n", prog);
   fprintf(stderr, "  %s music list --limit 50\n", prog);
}

static int cmd_ping(void) {
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }

   int result = admin_client_ping(fd);
   admin_client_disconnect(fd);

   if (result == 0) {
      printf("Dawn daemon is running and responsive.\n");
      return 0;
   } else {
      fprintf(stderr, "Failed to ping daemon.\n");
      return 1;
   }
}

static int cmd_user_create(const char *username, int is_admin) {
   if (!username || strlen(username) == 0) {
      fprintf(stderr, "Error: Username is required\n");
      return 1;
   }

   if (!is_admin) {
      fprintf(stderr, "Error: --admin flag is required for initial setup\n");
      fprintf(stderr, "Hint: Non-admin user creation will be available in Phase 2\n");
      return 1;
   }

   printf("Creating admin user: %s\n\n", username);

   /* Get token from env var or prompt */
   char token[64] = { 0 };
   if (prompt_input("Enter setup token: ", token, sizeof(token)) != 0) {
      fprintf(stderr, "Error: Failed to read setup token\n");
      return 1;
   }

   /* Validate token format (basic check) */
   if (strlen(token) != SETUP_TOKEN_LENGTH - 1 || strncmp(token, "DAWN-", 5) != 0) {
      fprintf(stderr, "Error: Invalid token format (expected DAWN-XXXX-XXXX-XXXX-XXXX)\n");
      secure_clear(token, sizeof(token));
      return 1;
   }

   printf("\n");

   /* Prompt for password */
   char password[PASSWORD_MAX_LENGTH] = { 0 };
   if (prompt_password_confirm(password, sizeof(password)) != 0) {
      secure_clear(token, sizeof(token));
      return 1;
   }

   /* Connect to daemon */
   int fd = admin_client_connect();
   if (fd < 0) {
      secure_clear(token, sizeof(token));
      secure_clear(password, sizeof(password));
      return 1;
   }

   /* Create user (atomic token validation + user creation) */
   printf("\nCreating user account...\n");
   admin_resp_code_t resp = admin_client_create_user(fd, token, username, password, is_admin != 0);

   /* Clear sensitive data from memory */
   secure_clear(token, sizeof(token));
   secure_clear(password, sizeof(password));

   admin_client_disconnect(fd);

   if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", admin_resp_strerror(resp));
      return 1;
   }

   printf("\n");
   printf("========================================\n");
   printf("  User created successfully!\n");
   printf("========================================\n");
   printf("\n");
   printf("  Username: %s\n", username);
   printf("  Role:     %s\n", is_admin ? "admin" : "user");
   printf("\n");
   printf("You can now log in to the WebUI with these credentials.\n");
   printf("\n");

   return 0;
}

/* Format relative time (e.g., "5m ago", "2h ago", "3d ago") */
static void format_relative_time(int64_t timestamp, char *buf, size_t buflen) {
   if (timestamp == 0) {
      snprintf(buf, buflen, "Never");
      return;
   }

   time_t now = time(NULL);
   int64_t diff = now - timestamp;

   if (diff < 0) {
      snprintf(buf, buflen, "Future");
   } else if (diff < 60) {
      snprintf(buf, buflen, "%lds ago", (long)diff);
   } else if (diff < 3600) {
      snprintf(buf, buflen, "%ldm ago", (long)(diff / 60));
   } else if (diff < 86400) {
      snprintf(buf, buflen, "%ldh ago", (long)(diff / 3600));
   } else {
      snprintf(buf, buflen, "%ldd ago", (long)(diff / 86400));
   }
}

/* User list callback context */
typedef struct {
   int count;
} user_list_print_ctx_t;

static int print_user_callback(const admin_user_entry_t *user, void *ctx) {
   user_list_print_ctx_t *pctx = (user_list_print_ctx_t *)ctx;

   const char *role = user->is_admin ? "Admin" : "User";
   const char *status = user->is_locked ? "Locked" : "Active";

   printf("  %-3d %-20s %-6s %-8s", user->id, user->username, role, status);
   if (user->failed_attempts > 0) {
      printf(" (%d failed)", user->failed_attempts);
   }
   printf("\n");

   pctx->count++;
   return 0;
}

static int cmd_user_list(void) {
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }

   printf("\nUsers:\n");
   printf("  %-3s %-20s %-6s %-8s\n", "ID", "Username", "Role", "Status");
   printf("  --- -------------------- ------ --------\n");

   user_list_print_ctx_t ctx = { .count = 0 };
   admin_resp_code_t resp = admin_client_list_users(fd, print_user_callback, &ctx);
   admin_client_disconnect(fd);

   if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", admin_resp_strerror(resp));
      return 1;
   }

   printf("\n%d user(s) total.\n\n", ctx.count);
   return 0;
}

static int cmd_user_delete(const char *username, int skip_confirm) {
   if (!username || strlen(username) == 0) {
      fprintf(stderr, "Error: Username is required\n");
      return 1;
   }

   /* Prompt for admin credentials */
   char admin_user[64] = { 0 };
   char admin_pass[PASSWORD_MAX_LENGTH] = { 0 };

   printf("Admin authentication required to delete user '%s'\n\n", username);

   if (prompt_input("Admin username: ", admin_user, sizeof(admin_user)) != 0) {
      fprintf(stderr, "Error: Failed to read admin username\n");
      return 1;
   }

   if (prompt_password("Admin password: ", admin_pass, sizeof(admin_pass)) != 0) {
      fprintf(stderr, "Error: Failed to read admin password\n");
      return 1;
   }

   /* Confirmation unless --yes */
   if (!skip_confirm) {
      char confirm[64] = { 0 };
      printf("\nDelete user '%s'? Type username to confirm: ", username);
      if (prompt_input("", confirm, sizeof(confirm)) != 0 || strcmp(confirm, username) != 0) {
         printf("Cancelled.\n");
         secure_clear(admin_pass, sizeof(admin_pass));
         return 1;
      }
   }

   int fd = admin_client_connect();
   if (fd < 0) {
      secure_clear(admin_pass, sizeof(admin_pass));
      return 1;
   }

   admin_resp_code_t resp = admin_client_delete_user(fd, admin_user, admin_pass, username);
   secure_clear(admin_pass, sizeof(admin_pass));
   admin_client_disconnect(fd);

   if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", admin_resp_strerror(resp));
      return 1;
   }

   printf("\nUser '%s' deleted successfully.\n\n", username);
   return 0;
}

static int cmd_user_passwd(const char *username) {
   if (!username || strlen(username) == 0) {
      fprintf(stderr, "Error: Username is required\n");
      return 1;
   }

   /* Prompt for admin credentials */
   char admin_user[64] = { 0 };
   char admin_pass[PASSWORD_MAX_LENGTH] = { 0 };

   printf("Admin authentication required to change password for '%s'\n\n", username);

   if (prompt_input("Admin username: ", admin_user, sizeof(admin_user)) != 0) {
      fprintf(stderr, "Error: Failed to read admin username\n");
      return 1;
   }

   if (prompt_password("Admin password: ", admin_pass, sizeof(admin_pass)) != 0) {
      fprintf(stderr, "Error: Failed to read admin password\n");
      return 1;
   }

   printf("\n");

   /* Prompt for new password with confirmation */
   char new_pass[PASSWORD_MAX_LENGTH] = { 0 };
   printf("New password for '%s':\n", username);
   if (prompt_password_confirm(new_pass, sizeof(new_pass)) != 0) {
      secure_clear(admin_pass, sizeof(admin_pass));
      return 1;
   }

   int fd = admin_client_connect();
   if (fd < 0) {
      secure_clear(admin_pass, sizeof(admin_pass));
      secure_clear(new_pass, sizeof(new_pass));
      return 1;
   }

   admin_resp_code_t resp = admin_client_change_password(fd, admin_user, admin_pass, username,
                                                         new_pass);
   secure_clear(admin_pass, sizeof(admin_pass));
   secure_clear(new_pass, sizeof(new_pass));
   admin_client_disconnect(fd);

   if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", admin_resp_strerror(resp));
      return 1;
   }

   printf("\nPassword changed for '%s'. All sessions invalidated.\n\n", username);
   return 0;
}

static int cmd_user_unlock(const char *username) {
   if (!username || strlen(username) == 0) {
      fprintf(stderr, "Error: Username is required\n");
      return 1;
   }

   /* Prompt for admin credentials */
   char admin_user[64] = { 0 };
   char admin_pass[PASSWORD_MAX_LENGTH] = { 0 };

   printf("Admin authentication required to unlock user '%s'\n\n", username);

   if (prompt_input("Admin username: ", admin_user, sizeof(admin_user)) != 0) {
      fprintf(stderr, "Error: Failed to read admin username\n");
      return 1;
   }

   if (prompt_password("Admin password: ", admin_pass, sizeof(admin_pass)) != 0) {
      fprintf(stderr, "Error: Failed to read admin password\n");
      return 1;
   }

   int fd = admin_client_connect();
   if (fd < 0) {
      secure_clear(admin_pass, sizeof(admin_pass));
      return 1;
   }

   admin_resp_code_t resp = admin_client_unlock_user(fd, admin_user, admin_pass, username);
   secure_clear(admin_pass, sizeof(admin_pass));
   admin_client_disconnect(fd);

   if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", admin_resp_strerror(resp));
      return 1;
   }

   printf("\nUser '%s' unlocked successfully.\n\n", username);
   return 0;
}

/* Session list callback context */
typedef struct {
   int count;
} session_list_print_ctx_t;

static int print_session_callback(const admin_session_entry_t *session, void *ctx) {
   session_list_print_ctx_t *pctx = (session_list_print_ctx_t *)ctx;

   char last_active[32];
   format_relative_time(session->last_activity, last_active, sizeof(last_active));

   printf("  %-10s %-16s %-18s %s\n", session->token_prefix, session->username,
          session->ip_address[0] ? session->ip_address : "(local)", last_active);

   pctx->count++;
   return 0;
}

static int cmd_session_list(void) {
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }

   printf("\nActive Sessions:\n");
   printf("  %-10s %-16s %-18s %s\n", "Token", "User", "IP Address", "Last Active");
   printf("  ---------- ---------------- ------------------ -----------\n");

   session_list_print_ctx_t ctx = { .count = 0 };
   admin_resp_code_t resp = admin_client_list_sessions(fd, print_session_callback, &ctx);
   admin_client_disconnect(fd);

   if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", admin_resp_strerror(resp));
      return 1;
   }

   printf("\n%d active session(s).\n\n", ctx.count);
   return 0;
}

static int cmd_session_revoke(const char *token_or_user, int is_user_mode) {
   if (!token_or_user || strlen(token_or_user) == 0) {
      fprintf(stderr, "Error: Token prefix or username is required\n");
      return 1;
   }

   /* Prompt for admin credentials */
   char admin_user[64] = { 0 };
   char admin_pass[PASSWORD_MAX_LENGTH] = { 0 };

   if (is_user_mode) {
      printf("Admin authentication required to revoke sessions for user '%s'\n\n", token_or_user);
   } else {
      printf("Admin authentication required to revoke session '%s...'\n\n", token_or_user);
   }

   if (prompt_input("Admin username: ", admin_user, sizeof(admin_user)) != 0) {
      fprintf(stderr, "Error: Failed to read admin username\n");
      return 1;
   }

   if (prompt_password("Admin password: ", admin_pass, sizeof(admin_pass)) != 0) {
      fprintf(stderr, "Error: Failed to read admin password\n");
      return 1;
   }

   int fd = admin_client_connect();
   if (fd < 0) {
      secure_clear(admin_pass, sizeof(admin_pass));
      return 1;
   }

   admin_resp_code_t resp;
   if (is_user_mode) {
      resp = admin_client_revoke_user_sessions(fd, admin_user, admin_pass, token_or_user);
   } else {
      resp = admin_client_revoke_session(fd, admin_user, admin_pass, token_or_user);
   }

   secure_clear(admin_pass, sizeof(admin_pass));
   admin_client_disconnect(fd);

   if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", admin_resp_strerror(resp));
      return 1;
   }

   if (is_user_mode) {
      printf("\nAll sessions revoked for user '%s'.\n\n", token_or_user);
   } else {
      printf("\nSession '%s...' revoked.\n\n", token_or_user);
   }
   return 0;
}

/* Format file size with human-readable units */
static void format_size(int64_t bytes, char *buf, size_t buflen) {
   if (bytes < 1024) {
      snprintf(buf, buflen, "%ld B", (long)bytes);
   } else if (bytes < 1024 * 1024) {
      snprintf(buf, buflen, "%.1f KB", (double)bytes / 1024);
   } else if (bytes < 1024 * 1024 * 1024) {
      snprintf(buf, buflen, "%.1f MB", (double)bytes / (1024 * 1024));
   } else {
      snprintf(buf, buflen, "%.1f GB", (double)bytes / (1024 * 1024 * 1024));
   }
}

static int cmd_db_status(void) {
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }

   admin_db_stats_t stats;
   admin_resp_code_t resp = admin_client_get_stats(fd, &stats);
   admin_client_disconnect(fd);

   if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", admin_resp_strerror(resp));
      return 1;
   }

   char size_str[32];
   format_size(stats.db_size_bytes, size_str, sizeof(size_str));

   printf("\nDatabase Statistics:\n\n");
   printf("  Users\n");
   printf("    Total:    %d\n", stats.user_count);
   printf("    Admins:   %d\n", stats.admin_count);
   printf("    Locked:   %d\n", stats.locked_user_count);
   printf("\n");
   printf("  Sessions\n");
   printf("    Active:   %d\n", stats.session_count);
   printf("\n");
   printf("  Security (last 24h)\n");
   printf("    Failed logins:  %d\n", stats.failed_attempts_24h);
   printf("\n");
   printf("  Database\n");
   printf("    Size:          %s\n", size_str);
   printf("    Audit entries: %d\n", stats.audit_log_count);
   printf("\n");

   return 0;
}

static int cmd_db_compact(void) {
   /* Prompt for admin credentials */
   char admin_user[64] = { 0 };
   char admin_pass[PASSWORD_MAX_LENGTH] = { 0 };

   printf("Admin authentication required for database compaction\n\n");

   if (prompt_input("Admin username: ", admin_user, sizeof(admin_user)) != 0) {
      fprintf(stderr, "Error: Failed to read admin username\n");
      return 1;
   }

   if (prompt_password("Admin password: ", admin_pass, sizeof(admin_pass)) != 0) {
      fprintf(stderr, "Error: Failed to read admin password\n");
      return 1;
   }

   int fd = admin_client_connect();
   if (fd < 0) {
      secure_clear(admin_pass, sizeof(admin_pass));
      return 1;
   }

   admin_resp_code_t resp = admin_client_db_compact(fd, admin_user, admin_pass);
   secure_clear(admin_pass, sizeof(admin_pass));
   admin_client_disconnect(fd);

   if (resp == ADMIN_RESP_RATE_LIMITED) {
      fprintf(stderr, "Error: Database was compacted recently. Try again in 24 hours.\n");
      return 1;
   } else if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", admin_resp_strerror(resp));
      return 1;
   }

   printf("\nDatabase compacted successfully.\n\n");
   return 0;
}

static int cmd_db_backup(const char *dest_path) {
   if (!dest_path || strlen(dest_path) == 0) {
      fprintf(stderr, "Error: Destination path is required\n");
      return 1;
   }

   /* Prompt for admin credentials */
   char admin_user[64] = { 0 };
   char admin_pass[PASSWORD_MAX_LENGTH] = { 0 };

   printf("Admin authentication required for database backup\n\n");

   if (prompt_input("Admin username: ", admin_user, sizeof(admin_user)) != 0) {
      fprintf(stderr, "Error: Failed to read admin username\n");
      return 1;
   }

   if (prompt_password("Admin password: ", admin_pass, sizeof(admin_pass)) != 0) {
      fprintf(stderr, "Error: Failed to read admin password\n");
      return 1;
   }

   int fd = admin_client_connect();
   if (fd < 0) {
      secure_clear(admin_pass, sizeof(admin_pass));
      return 1;
   }

   admin_resp_code_t resp = admin_client_db_backup(fd, admin_user, admin_pass, dest_path);
   secure_clear(admin_pass, sizeof(admin_pass));
   admin_client_disconnect(fd);

   if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", admin_resp_strerror(resp));
      return 1;
   }

   printf("\nDatabase backed up to: %s\n\n", dest_path);
   return 0;
}

/* Log show callback context */
typedef struct {
   int count;
} log_show_ctx_t;

static int print_log_callback(const admin_log_entry_t *entry, void *ctx) {
   log_show_ctx_t *lctx = (log_show_ctx_t *)ctx;

   char time_str[32];
   format_relative_time(entry->timestamp, time_str, sizeof(time_str));

   /* Print with color hints based on event type */
   printf("  %-12s %-20s %-16s %-18s", time_str, entry->event,
          entry->username[0] ? entry->username : "-",
          entry->ip_address[0] ? entry->ip_address : "-");

   if (entry->details[0]) {
      printf(" %s", entry->details);
   }
   printf("\n");

   lctx->count++;
   return 0;
}

static int cmd_log_show(int limit, const char *event_filter, const char *user_filter) {
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }

   admin_log_filter_t filter = { 0 };
   filter.limit = (limit > 0) ? limit : 50;
   filter.event = event_filter;
   filter.username = user_filter;

   printf("\nAudit Log:\n");
   printf("  %-12s %-20s %-16s %-18s %s\n", "Time", "Event", "User", "IP", "Details");
   printf("  ------------ -------------------- ---------------- ------------------ -------\n");

   log_show_ctx_t ctx = { .count = 0 };
   admin_resp_code_t resp = admin_client_query_log(fd, &filter, print_log_callback, &ctx);
   admin_client_disconnect(fd);

   if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", admin_resp_strerror(resp));
      return 1;
   }

   printf("\n%d log entries.\n\n", ctx.count);
   return 0;
}

/* IP list callback context */
typedef struct {
   int count;
   int blocked_count;
} ip_list_print_ctx_t;

static int print_ip_callback(const admin_ip_entry_t *entry, void *ctx) {
   ip_list_print_ctx_t *pctx = (ip_list_print_ctx_t *)ctx;

   char last_attempt[32];
   format_relative_time(entry->last_attempt, last_attempt, sizeof(last_attempt));

   /* Mark as "Blocked" if attempts >= 20 (rate limit threshold) */
   const char *status = (entry->failed_attempts >= 20) ? "Blocked" : "Warning";
   if (entry->failed_attempts >= 20) {
      pctx->blocked_count++;
   }

   printf("  %-40s %8d  %-12s  %s\n", entry->ip_address, entry->failed_attempts, last_attempt,
          status);

   pctx->count++;
   return 0;
}

static int cmd_ip_list(void) {
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }

   printf("\nRate-Limited IPs (last 15 minutes):\n");
   printf("  %-40s %8s  %-12s  %s\n", "IP Address", "Attempts", "Last Seen", "Status");
   printf("  ---------------------------------------- --------  ------------  -------\n");

   ip_list_print_ctx_t ctx = { .count = 0, .blocked_count = 0 };
   admin_resp_code_t resp = admin_client_list_blocked_ips(fd, print_ip_callback, &ctx);
   admin_client_disconnect(fd);

   if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", admin_resp_strerror(resp));
      return 1;
   }

   if (ctx.count == 0) {
      printf("  (no IPs with failed attempts)\n");
   }
   printf("\n%d IPs total, %d currently blocked (>= 20 attempts).\n\n", ctx.count,
          ctx.blocked_count);
   return 0;
}

static int cmd_ip_unblock(const char *ip_address) {
   if (!ip_address || strlen(ip_address) == 0) {
      fprintf(stderr, "Error: IP address is required (or use --all)\n");
      return 1;
   }

   /* Prompt for admin credentials */
   char admin_user[64] = { 0 };
   char admin_pass[PASSWORD_MAX_LENGTH] = { 0 };

   if (strcmp(ip_address, "--all") == 0) {
      printf("Admin authentication required to unblock all IPs\n\n");
   } else {
      printf("Admin authentication required to unblock IP '%s'\n\n", ip_address);
   }

   if (prompt_input("Admin username: ", admin_user, sizeof(admin_user)) != 0) {
      fprintf(stderr, "Error: Failed to read admin username\n");
      return 1;
   }

   if (prompt_password("Admin password: ", admin_pass, sizeof(admin_pass)) != 0) {
      fprintf(stderr, "Error: Failed to read admin password\n");
      return 1;
   }

   int fd = admin_client_connect();
   if (fd < 0) {
      secure_clear(admin_pass, sizeof(admin_pass));
      return 1;
   }

   admin_resp_code_t resp = admin_client_unblock_ip(fd, admin_user, admin_pass, ip_address);
   secure_clear(admin_pass, sizeof(admin_pass));
   admin_client_disconnect(fd);

   if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", admin_resp_strerror(resp));
      return 1;
   }

   if (strcmp(ip_address, "--all") == 0) {
      printf("\nAll IPs unblocked successfully.\n\n");
   } else {
      printf("\nIP '%s' unblocked successfully.\n\n", ip_address);
   }
   return 0;
}

/* =============================================================================
 * Phase 3: Metrics Commands
 * =============================================================================
 */

/* Metrics list callback context */
typedef struct {
   int count;
} metrics_list_ctx_t;

static int print_metrics_callback(const admin_metrics_entry_t *m, void *ctx) {
   metrics_list_ctx_t *mctx = (metrics_list_ctx_t *)ctx;

   char started[32];
   format_relative_time(m->started_at, started, sizeof(started));

   printf("  %-4lld %-10s %-12s %5u %5u %5u %5u %8.0f\n", (long long)m->id, m->session_type,
          started, m->queries_total, m->queries_cloud, m->queries_local, m->errors_count,
          m->avg_llm_total_ms);

   mctx->count++;
   return 0;
}

static int cmd_metrics_list(int limit, const char *type_filter) {
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }

   admin_metrics_filter_t filter = { 0 };
   filter.limit = limit;
   filter.type = type_filter;

   printf("\nSession Metrics:\n");
   printf("  %-4s %-10s %-12s %5s %5s %5s %5s %8s\n", "ID", "Type", "Started", "Total", "Cloud",
          "Local", "Errs", "Avg LLM");
   printf("  ---- ---------- ------------ ----- ----- ----- ----- --------\n");

   metrics_list_ctx_t ctx = { .count = 0 };
   admin_resp_code_t resp = admin_client_list_metrics(fd, &filter, print_metrics_callback, &ctx);
   admin_client_disconnect(fd);

   if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", admin_resp_strerror(resp));
      return 1;
   }

   printf("\n%d session(s).\n\n", ctx.count);
   return 0;
}

static int cmd_metrics_summary(void) {
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }

   admin_metrics_totals_t totals = { 0 };
   admin_resp_code_t resp = admin_client_get_metrics_totals(fd, NULL, &totals);
   admin_client_disconnect(fd);

   if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", admin_resp_strerror(resp));
      return 1;
   }

   printf("\nMetrics Summary (All Time):\n\n");
   printf("  Sessions:  %d\n", totals.session_count);
   printf("  Queries:   %lu total (%lu cloud, %lu local)\n", (unsigned long)totals.queries_total,
          (unsigned long)totals.queries_cloud, (unsigned long)totals.queries_local);
   printf("  Errors:    %lu\n", (unsigned long)totals.errors_total);
   printf("  Avg LLM:   %.1f ms\n", totals.avg_llm_ms);
   printf("\n");

   return 0;
}

/* =============================================================================
 * Phase 4: Conversation Commands
 * =============================================================================
 */

/* Conversation list callback context */
typedef struct {
   int count;
} conv_list_ctx_t;

static int print_conv_callback(const admin_conversation_entry_t *conv, void *ctx) {
   conv_list_ctx_t *cctx = (conv_list_ctx_t *)ctx;

   char updated[32];
   format_relative_time(conv->updated_at, updated, sizeof(updated));

   /* Truncate title for display */
   char title[41] = { 0 };
   strncpy(title, conv->title, 40);
   if (strlen(conv->title) > 40) {
      strcpy(title + 37, "...");
   }

   printf("  %-6lld %-40s %4d %-12s\n", (long long)conv->id, title, conv->message_count, updated);

   cctx->count++;
   return 0;
}

static int cmd_conv_list(int limit, bool include_archived) {
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }

   admin_conversation_filter_t filter = { 0 };
   filter.limit = limit;
   filter.include_archived = include_archived;

   printf("\nConversations%s:\n", include_archived ? " (including archived)" : "");
   printf("  %-6s %-40s %4s %-12s\n", "ID", "Title", "Msgs", "Updated");
   printf("  ------ ---------------------------------------- ---- ------------\n");

   conv_list_ctx_t ctx = { .count = 0 };
   admin_resp_code_t resp = admin_client_list_conversations(fd, &filter, print_conv_callback, &ctx);
   admin_client_disconnect(fd);

   if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", admin_resp_strerror(resp));
      return 1;
   }

   printf("\n%d conversation(s).\n\n", ctx.count);
   return 0;
}

/* Message print callback context */
typedef struct {
   int count;
} msg_list_ctx_t;

static int print_msg_callback(const admin_message_entry_t *msg, void *ctx) {
   msg_list_ctx_t *mctx = (msg_list_ctx_t *)ctx;

   /* Color code by role */
   const char *role_label;
   if (strcmp(msg->role, "user") == 0) {
      role_label = "User";
   } else if (strcmp(msg->role, "assistant") == 0) {
      role_label = "Assistant";
   } else if (strcmp(msg->role, "system") == 0) {
      role_label = "System";
   } else {
      role_label = msg->role;
   }

   printf("\n[%s]\n", role_label);

   /* Print content (truncate if very long) */
   size_t len = strlen(msg->content);
   if (len > 500) {
      printf("%.497s...\n", msg->content);
   } else {
      printf("%s\n", msg->content);
   }

   mctx->count++;
   return 0;
}

static int cmd_conv_show(int64_t conv_id) {
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }

   printf("\nConversation %lld:\n", (long long)conv_id);
   printf("========================================\n");

   msg_list_ctx_t ctx = { .count = 0 };
   admin_resp_code_t resp = admin_client_get_conversation(fd, conv_id, print_msg_callback, &ctx);
   admin_client_disconnect(fd);

   if (resp == ADMIN_RESP_NOT_FOUND) {
      fprintf(stderr, "Error: Conversation not found\n");
      return 1;
   } else if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", admin_resp_strerror(resp));
      return 1;
   }

   printf("\n========================================\n");
   printf("%d message(s).\n\n", ctx.count);
   return 0;
}

static int cmd_conv_delete(int64_t conv_id, int skip_confirm) {
   /* Prompt for admin credentials */
   char admin_user[64] = { 0 };
   char admin_pass[PASSWORD_MAX_LENGTH] = { 0 };

   printf("Admin authentication required to delete conversation %lld\n\n", (long long)conv_id);

   if (prompt_input("Admin username: ", admin_user, sizeof(admin_user)) != 0) {
      fprintf(stderr, "Error: Failed to read admin username\n");
      return 1;
   }

   if (prompt_password("Admin password: ", admin_pass, sizeof(admin_pass)) != 0) {
      fprintf(stderr, "Error: Failed to read admin password\n");
      return 1;
   }

   /* Confirmation unless --yes */
   if (!skip_confirm) {
      char confirm[16] = { 0 };
      printf("\nDelete conversation %lld? Type 'yes' to confirm: ", (long long)conv_id);
      if (prompt_input("", confirm, sizeof(confirm)) != 0 || strcmp(confirm, "yes") != 0) {
         printf("Cancelled.\n");
         secure_clear(admin_pass, sizeof(admin_pass));
         return 1;
      }
   }

   int fd = admin_client_connect();
   if (fd < 0) {
      secure_clear(admin_pass, sizeof(admin_pass));
      return 1;
   }

   admin_resp_code_t resp = admin_client_delete_conversation(fd, admin_user, admin_pass, conv_id);
   secure_clear(admin_pass, sizeof(admin_pass));
   admin_client_disconnect(fd);

   if (resp == ADMIN_RESP_NOT_FOUND) {
      fprintf(stderr, "Error: Conversation not found\n");
      return 1;
   } else if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", admin_resp_strerror(resp));
      return 1;
   }

   printf("\nConversation %lld deleted successfully.\n\n", (long long)conv_id);
   return 0;
}

/* =============================================================================
 * Music Commands
 * =============================================================================
 */

static int cmd_music_stats(void) {
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }

   char response[4096];
   admin_resp_code_t resp = admin_client_music_stats(fd, response, sizeof(response));
   admin_client_disconnect(fd);

   if (resp == ADMIN_RESP_SUCCESS) {
      printf("%s\n", response);
      return 0;
   } else {
      fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
      return 1;
   }
}

static int cmd_music_search(const char *query) {
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }

   char response[4096];
   admin_resp_code_t resp = admin_client_music_search(fd, query, response, sizeof(response));
   admin_client_disconnect(fd);

   if (resp == ADMIN_RESP_SUCCESS) {
      printf("%s\n", response);
      return 0;
   } else {
      fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
      return 1;
   }
}

static int cmd_music_list(int limit) {
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }

   char response[4096];
   admin_resp_code_t resp = admin_client_music_list(fd, limit, response, sizeof(response));
   admin_client_disconnect(fd);

   if (resp == ADMIN_RESP_SUCCESS) {
      printf("%s\n", response);
      return 0;
   } else {
      fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
      return 1;
   }
}

static int cmd_music_rescan(void) {
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }

   char response[256];
   admin_resp_code_t resp = admin_client_music_rescan(fd, response, sizeof(response));
   admin_client_disconnect(fd);

   if (resp == ADMIN_RESP_SUCCESS) {
      printf("%s\n", response);
      return 0;
   } else {
      fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
      return 1;
   }
}

static int cmd_memory_recategorize(const char *username) {
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }

   char response[256];
   admin_resp_code_t resp = admin_client_memory_recategorize(fd, username, response,
                                                             sizeof(response));
   admin_client_disconnect(fd);

   if (resp == ADMIN_RESP_SUCCESS) {
      printf("%s\n", response);
      return 0;
   } else {
      fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
      return 1;
   }
}

static int cmd_memory_backfill_note_glosses(const char *username) {
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }

   char response[256];
   admin_resp_code_t resp = admin_client_memory_backfill_note_glosses(fd, username, response,
                                                                      sizeof(response));
   admin_client_disconnect(fd);

   if (resp == ADMIN_RESP_SUCCESS) {
      printf("%s\n", response);
      return 0;
   } else {
      fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
      return 1;
   }
}

static int cmd_memory_rebuild_document_fts(void) {
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }

   char response[256];
   admin_resp_code_t resp = admin_client_memory_rebuild_document_fts(fd, response,
                                                                     sizeof(response));
   admin_client_disconnect(fd);

   if (resp == ADMIN_RESP_SUCCESS) {
      printf("%s\n", response);
      return 0;
   } else {
      fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
      return 1;
   }
}

static int cmd_memory_reextract(const char *username,
                                bool confirm,
                                bool keep_summaries,
                                const char *backup_path,
                                double max_cost_usd) {
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }

   char response[ADMIN_MSG_CONTENT_MAX + 1];
   admin_resp_code_t resp = admin_client_memory_reextract(fd, username, confirm, keep_summaries,
                                                          backup_path, max_cost_usd, response,
                                                          sizeof(response));
   admin_client_disconnect(fd);

   if (resp == ADMIN_RESP_SUCCESS) {
      printf("%s\n", response);
      return 0;
   } else {
      fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
      return 1;
   }
}

static int cmd_memory_reextract_status(const char *username) {
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }

   char response[ADMIN_MSG_CONTENT_MAX + 1];
   admin_resp_code_t resp = admin_client_memory_reextract_status(fd, username, response,
                                                                 sizeof(response));
   admin_client_disconnect(fd);

   if (resp == ADMIN_RESP_SUCCESS) {
      printf("%s\n", response);
      return 0;
   } else {
      fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
      return 1;
   }
}

/* =============================================================================
 * memory entity * subcommands (v43)
 *
 * One helper that wraps connect → call → disconnect → render-or-error for
 * the six text-response subcommands.  Each public cmd_* dispatches into
 * here with its own client wrapper invocation.
 * ============================================================================= */

typedef admin_resp_code_t (*entity_invoker_fn)(int fd, char *response, size_t resp_len, void *ctx);

static int run_entity_subcommand(entity_invoker_fn fn, void *ctx) {
   int fd = admin_client_connect();
   if (fd < 0)
      return 1;
   char response[ADMIN_MSG_CONTENT_MAX + 1];
   admin_resp_code_t resp = fn(fd, response, sizeof(response), ctx);
   admin_client_disconnect(fd);
   if (resp == ADMIN_RESP_SUCCESS) {
      printf("%s\n", response);
      return 0;
   }
   fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
   return 1;
}

typedef struct {
   const char *username;
   int64_t source_id;
   int64_t target_id;
   const char *reason;
} entity_merge_ctx_t;
static admin_resp_code_t invoke_entity_merge(int fd, char *response, size_t resp_len, void *ctx) {
   entity_merge_ctx_t *c = ctx;
   return admin_client_memory_entity_merge(fd, c->username, c->source_id, c->target_id, c->reason,
                                           response, resp_len);
}

typedef struct {
   const char *username;
   int64_t link_id;
   const char *reason;
} entity_split_ctx_t;
static admin_resp_code_t invoke_entity_split(int fd, char *response, size_t resp_len, void *ctx) {
   entity_split_ctx_t *c = ctx;
   return admin_client_memory_entity_split(fd, c->username, c->link_id, c->reason, response,
                                           resp_len);
}

typedef struct {
   const char *username;
   int64_t entity_id;
} entity_id_ctx_t;
static admin_resp_code_t invoke_entity_aliases(int fd, char *response, size_t resp_len, void *ctx) {
   entity_id_ctx_t *c = ctx;
   return admin_client_memory_entity_aliases(fd, c->username, c->entity_id, response, resp_len);
}
static admin_resp_code_t invoke_entity_history(int fd, char *response, size_t resp_len, void *ctx) {
   entity_id_ctx_t *c = ctx;
   return admin_client_memory_entity_history(fd, c->username, c->entity_id, response, resp_len);
}

typedef struct {
   const char *username;
   bool include_aliases;
} entity_list_ctx_t;
static admin_resp_code_t invoke_entity_list(int fd, char *response, size_t resp_len, void *ctx) {
   entity_list_ctx_t *c = ctx;
   return admin_client_memory_entity_list(fd, c->username, c->include_aliases, response, resp_len);
}

typedef struct {
   const char *username;
   bool dry_run;
} entity_link_user_self_ctx_t;
static admin_resp_code_t invoke_entity_link_user_self(int fd,
                                                      char *response,
                                                      size_t resp_len,
                                                      void *ctx) {
   entity_link_user_self_ctx_t *c = ctx;
   return admin_client_memory_entity_link_user_self(fd, c->username, c->dry_run, response,
                                                    resp_len);
}

int main(int argc, char *argv[]) {
   if (argc < 2) {
      print_usage(argv[0]);
      return 1;
   }

   const char *cmd = argv[1];

   /* Help command */
   if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
      print_usage(argv[0]);
      return 0;
   }

   /* Ping command */
   if (strcmp(cmd, "ping") == 0) {
      return cmd_ping();
   }

   /* User commands */
   if (strcmp(cmd, "user") == 0) {
      if (argc < 3) {
         fprintf(stderr, "Error: Missing user subcommand\n");
         fprintf(stderr, "Usage: %s user <list|create|delete|passwd|unlock>\n", argv[0]);
         return 1;
      }

      const char *subcmd = argv[2];

      if (strcmp(subcmd, "list") == 0) {
         return cmd_user_list();

      } else if (strcmp(subcmd, "create") == 0) {
         if (argc < 4) {
            fprintf(stderr, "Error: Missing username\n");
            fprintf(stderr, "Usage: %s user create <username> --admin\n", argv[0]);
            return 1;
         }

         const char *username = argv[3];
         int is_admin = 0;

         /* Check for --admin flag */
         for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--admin") == 0) {
               is_admin = 1;
               break;
            }
         }

         return cmd_user_create(username, is_admin);

      } else if (strcmp(subcmd, "delete") == 0) {
         if (argc < 4) {
            fprintf(stderr, "Error: Missing username\n");
            fprintf(stderr, "Usage: %s user delete <username> [--yes]\n", argv[0]);
            return 1;
         }

         const char *username = argv[3];
         int skip_confirm = 0;

         /* Check for --yes flag */
         for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0) {
               skip_confirm = 1;
               break;
            }
         }

         return cmd_user_delete(username, skip_confirm);

      } else if (strcmp(subcmd, "passwd") == 0) {
         if (argc < 4) {
            fprintf(stderr, "Error: Missing username\n");
            fprintf(stderr, "Usage: %s user passwd <username>\n", argv[0]);
            return 1;
         }

         return cmd_user_passwd(argv[3]);

      } else if (strcmp(subcmd, "unlock") == 0) {
         if (argc < 4) {
            fprintf(stderr, "Error: Missing username\n");
            fprintf(stderr, "Usage: %s user unlock <username>\n", argv[0]);
            return 1;
         }

         return cmd_user_unlock(argv[3]);

      } else {
         fprintf(stderr, "Error: Unknown user subcommand: %s\n", subcmd);
         fprintf(stderr, "Available: list, create, delete, passwd, unlock\n");
         return 1;
      }
   }

   /* Session commands */
   if (strcmp(cmd, "session") == 0) {
      if (argc < 3) {
         fprintf(stderr, "Error: Missing session subcommand\n");
         fprintf(stderr, "Usage: %s session <list|revoke>\n", argv[0]);
         return 1;
      }

      const char *subcmd = argv[2];

      if (strcmp(subcmd, "list") == 0) {
         return cmd_session_list();

      } else if (strcmp(subcmd, "revoke") == 0) {
         if (argc < 4) {
            fprintf(stderr, "Error: Missing token prefix or --user flag\n");
            fprintf(stderr, "Usage: %s session revoke <token_prefix>\n", argv[0]);
            fprintf(stderr, "       %s session revoke --user <username>\n", argv[0]);
            return 1;
         }

         /* Check for --user mode */
         if (strcmp(argv[3], "--user") == 0) {
            if (argc < 5) {
               fprintf(stderr, "Error: Missing username\n");
               fprintf(stderr, "Usage: %s session revoke --user <username>\n", argv[0]);
               return 1;
            }
            return cmd_session_revoke(argv[4], 1);
         }

         /* Token prefix mode - must be 8 characters */
         if (strlen(argv[3]) < 8) {
            fprintf(stderr, "Error: Token prefix must be at least 8 characters\n");
            return 1;
         }
         return cmd_session_revoke(argv[3], 0);

      } else {
         fprintf(stderr, "Error: Unknown session subcommand: %s\n", subcmd);
         fprintf(stderr, "Available: list, revoke\n");
         return 1;
      }
   }

   /* Database commands */
   if (strcmp(cmd, "db") == 0) {
      if (argc < 3) {
         fprintf(stderr, "Error: Missing db subcommand\n");
         fprintf(stderr, "Usage: %s db <status|compact|backup>\n", argv[0]);
         return 1;
      }

      const char *subcmd = argv[2];

      if (strcmp(subcmd, "status") == 0) {
         return cmd_db_status();

      } else if (strcmp(subcmd, "compact") == 0) {
         return cmd_db_compact();

      } else if (strcmp(subcmd, "backup") == 0) {
         if (argc < 4) {
            fprintf(stderr, "Error: Missing destination path\n");
            fprintf(stderr, "Usage: %s db backup <path>\n", argv[0]);
            return 1;
         }
         return cmd_db_backup(argv[3]);

      } else {
         fprintf(stderr, "Error: Unknown db subcommand: %s\n", subcmd);
         fprintf(stderr, "Available: status, compact, backup\n");
         return 1;
      }
   }

   /* Log commands */
   if (strcmp(cmd, "log") == 0) {
      if (argc < 3) {
         fprintf(stderr, "Error: Missing log subcommand\n");
         fprintf(stderr, "Usage: %s log show [options]\n", argv[0]);
         return 1;
      }

      const char *subcmd = argv[2];

      if (strcmp(subcmd, "show") == 0) {
         int limit = 50;
         const char *event_filter = NULL;
         const char *user_filter = NULL;

         /* Parse options */
         for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--last") == 0 && i + 1 < argc) {
               limit = atoi(argv[++i]);
               if (limit <= 0)
                  limit = 50;
            } else if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
               event_filter = argv[++i];
            } else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
               user_filter = argv[++i];
            }
         }

         return cmd_log_show(limit, event_filter, user_filter);

      } else {
         fprintf(stderr, "Error: Unknown log subcommand: %s\n", subcmd);
         fprintf(stderr, "Available: show\n");
         return 1;
      }
   }

   /* IP commands */
   if (strcmp(cmd, "ip") == 0) {
      if (argc < 3) {
         fprintf(stderr, "Error: Missing ip subcommand\n");
         fprintf(stderr, "Usage: %s ip list|unblock\n", argv[0]);
         return 1;
      }

      const char *subcmd = argv[2];

      if (strcmp(subcmd, "list") == 0) {
         return cmd_ip_list();

      } else if (strcmp(subcmd, "unblock") == 0) {
         if (argc < 4) {
            fprintf(stderr, "Error: Missing IP address\n");
            fprintf(stderr, "Usage: %s ip unblock <ip-address|--all>\n", argv[0]);
            return 1;
         }
         return cmd_ip_unblock(argv[3]);

      } else {
         fprintf(stderr, "Error: Unknown ip subcommand: %s\n", subcmd);
         fprintf(stderr, "Available: list, unblock\n");
         return 1;
      }
   }

   /* Metrics commands */
   if (strcmp(cmd, "metrics") == 0) {
      if (argc < 3) {
         fprintf(stderr, "Error: Missing metrics subcommand\n");
         fprintf(stderr, "Usage: %s metrics <list|summary>\n", argv[0]);
         return 1;
      }

      const char *subcmd = argv[2];

      if (strcmp(subcmd, "list") == 0) {
         int limit = 20;
         const char *type_filter = NULL;

         /* Parse options */
         for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--last") == 0 && i + 1 < argc) {
               limit = atoi(argv[++i]);
               if (limit <= 0)
                  limit = 20;
            } else if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
               type_filter = argv[++i];
            }
         }

         return cmd_metrics_list(limit, type_filter);

      } else if (strcmp(subcmd, "summary") == 0) {
         return cmd_metrics_summary();

      } else {
         fprintf(stderr, "Error: Unknown metrics subcommand: %s\n", subcmd);
         fprintf(stderr, "Available: list, summary\n");
         return 1;
      }
   }

   /* Conversation commands */
   if (strcmp(cmd, "conv") == 0) {
      if (argc < 3) {
         fprintf(stderr, "Error: Missing conv subcommand\n");
         fprintf(stderr, "Usage: %s conv <list|show|delete>\n", argv[0]);
         return 1;
      }

      const char *subcmd = argv[2];

      if (strcmp(subcmd, "list") == 0) {
         int limit = 20;
         bool include_archived = false;

         /* Parse options */
         for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--last") == 0 && i + 1 < argc) {
               limit = atoi(argv[++i]);
               if (limit <= 0)
                  limit = 20;
            } else if (strcmp(argv[i], "--archived") == 0 || strcmp(argv[i], "-a") == 0) {
               include_archived = true;
            }
         }

         return cmd_conv_list(limit, include_archived);

      } else if (strcmp(subcmd, "show") == 0) {
         if (argc < 4) {
            fprintf(stderr, "Error: Missing conversation ID\n");
            fprintf(stderr, "Usage: %s conv show <id>\n", argv[0]);
            return 1;
         }

         char *endptr;
         errno = 0;
         int64_t conv_id = strtoll(argv[3], &endptr, 10);
         if (errno != 0 || endptr == argv[3] || *endptr != '\0' || conv_id <= 0) {
            fprintf(stderr, "Error: Invalid conversation ID\n");
            return 1;
         }

         return cmd_conv_show(conv_id);

      } else if (strcmp(subcmd, "delete") == 0) {
         if (argc < 4) {
            fprintf(stderr, "Error: Missing conversation ID\n");
            fprintf(stderr, "Usage: %s conv delete <id> [--yes]\n", argv[0]);
            return 1;
         }

         char *endptr;
         errno = 0;
         int64_t conv_id = strtoll(argv[3], &endptr, 10);
         if (errno != 0 || endptr == argv[3] || *endptr != '\0' || conv_id <= 0) {
            fprintf(stderr, "Error: Invalid conversation ID\n");
            return 1;
         }

         int skip_confirm = 0;
         for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0) {
               skip_confirm = 1;
               break;
            }
         }

         return cmd_conv_delete(conv_id, skip_confirm);

      } else {
         fprintf(stderr, "Error: Unknown conv subcommand: %s\n", subcmd);
         fprintf(stderr, "Available: list, show, delete\n");
         return 1;
      }
   }

   /* Music commands */
   if (strcmp(cmd, "music") == 0) {
      if (argc < 3) {
         fprintf(stderr, "Error: Missing music subcommand\n");
         fprintf(stderr, "Usage: %s music <stats|search|list|rescan>\n", argv[0]);
         return 1;
      }

      const char *subcmd = argv[2];

      if (strcmp(subcmd, "stats") == 0) {
         return cmd_music_stats();

      } else if (strcmp(subcmd, "search") == 0) {
         if (argc < 4) {
            fprintf(stderr, "Error: Missing search query\n");
            fprintf(stderr, "Usage: %s music search <query>\n", argv[0]);
            return 1;
         }
         return cmd_music_search(argv[3]);

      } else if (strcmp(subcmd, "list") == 0) {
         int limit = 0; /* 0 = show all */

         /* Parse options */
         for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
               char *endptr;
               long val = strtol(argv[++i], &endptr, 10);
               if (*endptr != '\0' || val < 0) {
                  limit = 0; /* Invalid input: show all */
               } else {
                  limit = (int)val;
               }
            }
         }

         return cmd_music_list(limit);

      } else if (strcmp(subcmd, "rescan") == 0) {
         return cmd_music_rescan();

      } else {
         fprintf(stderr, "Error: Unknown music subcommand: %s\n", subcmd);
         fprintf(stderr, "Available: stats, search, list, rescan\n");
         return 1;
      }
   }

   /* === Memory Management === */
   if (strcmp(cmd, "memory") == 0) {
      if (argc < 3) {
         fprintf(stderr, "Error: Missing memory subcommand\n");
         fprintf(stderr, "Usage: %s memory recategorize-all <username>\n", argv[0]);
         fprintf(stderr,
                 "       %s memory reextract --user <username> [--confirm] [--keep-summaries] "
                 "[--backup-path <path>] [--max-cost-usd <X>]\n",
                 argv[0]);
         fprintf(stderr, "       %s memory reextract-status --user <username>\n", argv[0]);
         fprintf(stderr,
                 "       %s memory cleanup-meta-facts --user <u> [--dry-run|--confirm-delete]\n",
                 argv[0]);
         fprintf(stderr,
                 "       %s memory summarize-missing --user <u> [--dry-run|--confirm] "
                 "[--max N]\n",
                 argv[0]);
         return 1;
      }
      const char *subcmd = argv[2];

      if (strcmp(subcmd, "recategorize-all") == 0) {
         if (argc < 4) {
            fprintf(stderr, "Error: Missing username\n");
            fprintf(stderr, "Usage: %s memory recategorize-all <username>\n", argv[0]);
            return 1;
         }
         return cmd_memory_recategorize(argv[3]);
      }

      if (strcmp(subcmd, "backfill-note-glosses") == 0) {
         if (argc < 4) {
            fprintf(stderr, "Error: Missing username\n");
            fprintf(stderr, "Usage: %s memory backfill-note-glosses <username>\n", argv[0]);
            return 1;
         }
         return cmd_memory_backfill_note_glosses(argv[3]);
      }

      if (strcmp(subcmd, "rebuild-document-fts") == 0) {
         return cmd_memory_rebuild_document_fts();
      }

      if (strcmp(subcmd, "cleanup-meta-facts") == 0) {
         const char *username = NULL;
         bool dry_run = true; /* default: dry-run, require --confirm-delete to execute */
         for (int i = 3; i < argc; i++) {
            const char *arg = argv[i];
            if (strcmp(arg, "--user") == 0 && i + 1 < argc) {
               username = argv[++i];
            } else if (strcmp(arg, "--dry-run") == 0) {
               dry_run = true;
            } else if (strcmp(arg, "--confirm-delete") == 0) {
               dry_run = false;
            } else {
               fprintf(stderr, "Error: Unknown option for memory cleanup-meta-facts: %s\n", arg);
               return 1;
            }
         }
         if (!username || !username[0]) {
            fprintf(stderr, "Error: --user <username> is required\n");
            fprintf(stderr,
                    "Usage: %s memory cleanup-meta-facts --user <u> [--dry-run|"
                    "--confirm-delete]\n",
                    argv[0]);
            return 1;
         }

         int fd = admin_client_connect();
         if (fd < 0)
            return 1;
         char response[1024];
         admin_resp_code_t resp = admin_client_memory_cleanup_meta_facts(fd, username, dry_run,
                                                                         response,
                                                                         sizeof(response));
         admin_client_disconnect(fd);
         if (resp == ADMIN_RESP_SUCCESS) {
            printf("%s\n", response);
            return 0;
         }
         fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
         return 1;
      }

      if (strcmp(subcmd, "summarize-missing") == 0) {
         const char *username = NULL;
         bool dry_run = true; /* default: count-only, require --confirm to start worker */
         uint32_t max_count = 0;
         for (int i = 3; i < argc; i++) {
            const char *arg = argv[i];
            if (strcmp(arg, "--user") == 0 && i + 1 < argc) {
               username = argv[++i];
            } else if (strcmp(arg, "--dry-run") == 0) {
               dry_run = true;
            } else if (strcmp(arg, "--confirm") == 0) {
               dry_run = false;
            } else if (strcmp(arg, "--max") == 0 && i + 1 < argc) {
               char *endp = NULL;
               unsigned long v = strtoul(argv[++i], &endp, 10);
               if (!endp || *endp != '\0' || v > UINT32_MAX) {
                  fprintf(stderr, "Error: --max requires a non-negative integer\n");
                  return 1;
               }
               max_count = (uint32_t)v;
            } else {
               fprintf(stderr, "Error: Unknown option for memory summarize-missing: %s\n", arg);
               return 1;
            }
         }
         if (!username || !username[0]) {
            fprintf(stderr, "Error: --user <username> is required\n");
            fprintf(stderr,
                    "Usage: %s memory summarize-missing --user <u> [--dry-run|--confirm] "
                    "[--max N]\n",
                    argv[0]);
            return 1;
         }

         int fd = admin_client_connect();
         if (fd < 0)
            return 1;
         char response[1024];
         admin_resp_code_t resp = admin_client_memory_summarize_missing(fd, username, dry_run,
                                                                        max_count, response,
                                                                        sizeof(response));
         admin_client_disconnect(fd);
         if (resp == ADMIN_RESP_SUCCESS) {
            printf("%s\n", response);
            return 0;
         }
         fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
         return 1;
      }

      if (strcmp(subcmd, "reextract") == 0 || strcmp(subcmd, "reextract-status") == 0) {
         bool is_status = (strcmp(subcmd, "reextract-status") == 0);
         const char *username = NULL;
         bool confirm = false;
         bool keep_summaries = false;
         const char *backup_path = NULL;
         double max_cost_usd = 0.0;

         for (int i = 3; i < argc; i++) {
            const char *arg = argv[i];
            if (strcmp(arg, "--user") == 0 && i + 1 < argc) {
               username = argv[++i];
            } else if (!is_status && strcmp(arg, "--confirm") == 0) {
               confirm = true;
            } else if (!is_status && strcmp(arg, "--dry-run") == 0) {
               confirm = false;
            } else if (!is_status && strcmp(arg, "--keep-summaries") == 0) {
               keep_summaries = true;
            } else if (!is_status && strcmp(arg, "--backup-path") == 0 && i + 1 < argc) {
               backup_path = argv[++i];
            } else if (!is_status && strcmp(arg, "--max-cost-usd") == 0 && i + 1 < argc) {
               char *endp = NULL;
               double v = strtod(argv[++i], &endp);
               if (!endp || *endp != '\0' || v < 0.0) {
                  fprintf(stderr, "Error: --max-cost-usd requires a non-negative number\n");
                  return 1;
               }
               max_cost_usd = v;
            } else {
               fprintf(stderr, "Error: Unknown option for memory %s: %s\n", subcmd, arg);
               return 1;
            }
         }
         if (!username || !username[0]) {
            fprintf(stderr, "Error: --user <username> is required\n");
            return 1;
         }
         if (is_status)
            return cmd_memory_reextract_status(username);
         return cmd_memory_reextract(username, confirm, keep_summaries, backup_path, max_cost_usd);
      }

      if (strcmp(subcmd, "entity") == 0) {
         if (argc < 4) {
            fprintf(stderr, "Error: Missing entity subcommand\n");
            fprintf(stderr,
                    "Available: merge | split | aliases | history | list | link-user-self\n");
            fprintf(stderr,
                    "Usage:\n"
                    "  %s memory entity merge --user <u> --source <id> --target <id> "
                    "[--reason <r>]\n"
                    "  %s memory entity split --user <u> --link-id <id> [--reason <r>]\n"
                    "  %s memory entity aliases --user <u> --entity-id <id>\n"
                    "  %s memory entity history --user <u> --entity-id <id>\n"
                    "  %s memory entity list --user <u> [--show-aliases]\n"
                    "  %s memory entity link-user-self --user <u> [--dry-run]\n",
                    argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
            return 1;
         }
         const char *ent_subcmd = argv[3];
         const char *username = NULL;
         int64_t source_id = 0, target_id = 0, link_id = 0, entity_id = 0;
         const char *reason = NULL;
         bool include_aliases = false;
         bool dry_run = false;
         for (int i = 4; i < argc; i++) {
            const char *arg = argv[i];
            if (strcmp(arg, "--user") == 0 && i + 1 < argc) {
               username = argv[++i];
            } else if (strcmp(arg, "--source") == 0 && i + 1 < argc) {
               source_id = (int64_t)strtoll(argv[++i], NULL, 10);
            } else if (strcmp(arg, "--target") == 0 && i + 1 < argc) {
               target_id = (int64_t)strtoll(argv[++i], NULL, 10);
            } else if (strcmp(arg, "--link-id") == 0 && i + 1 < argc) {
               link_id = (int64_t)strtoll(argv[++i], NULL, 10);
            } else if (strcmp(arg, "--entity-id") == 0 && i + 1 < argc) {
               entity_id = (int64_t)strtoll(argv[++i], NULL, 10);
            } else if (strcmp(arg, "--reason") == 0 && i + 1 < argc) {
               reason = argv[++i];
            } else if (strcmp(arg, "--show-aliases") == 0) {
               include_aliases = true;
            } else if (strcmp(arg, "--dry-run") == 0) {
               dry_run = true;
            } else {
               fprintf(stderr, "Error: Unknown option for memory entity %s: %s\n", ent_subcmd, arg);
               return 1;
            }
         }
         if (!username || !username[0]) {
            fprintf(stderr, "Error: --user <username> is required\n");
            return 1;
         }
         if (strcmp(ent_subcmd, "merge") == 0) {
            if (source_id <= 0 || target_id <= 0) {
               fprintf(stderr, "Error: --source <id> and --target <id> are required\n");
               return 1;
            }
            entity_merge_ctx_t ctx = { username, source_id, target_id, reason };
            return run_entity_subcommand(invoke_entity_merge, &ctx);
         }
         if (strcmp(ent_subcmd, "split") == 0) {
            if (link_id <= 0) {
               fprintf(stderr, "Error: --link-id <id> is required\n");
               return 1;
            }
            entity_split_ctx_t ctx = { username, link_id, reason };
            return run_entity_subcommand(invoke_entity_split, &ctx);
         }
         if (strcmp(ent_subcmd, "aliases") == 0) {
            if (entity_id <= 0) {
               fprintf(stderr, "Error: --entity-id <id> is required\n");
               return 1;
            }
            entity_id_ctx_t ctx = { username, entity_id };
            return run_entity_subcommand(invoke_entity_aliases, &ctx);
         }
         if (strcmp(ent_subcmd, "history") == 0) {
            if (entity_id <= 0) {
               fprintf(stderr, "Error: --entity-id <id> is required\n");
               return 1;
            }
            entity_id_ctx_t ctx = { username, entity_id };
            return run_entity_subcommand(invoke_entity_history, &ctx);
         }
         if (strcmp(ent_subcmd, "list") == 0) {
            entity_list_ctx_t ctx = { username, include_aliases };
            return run_entity_subcommand(invoke_entity_list, &ctx);
         }
         if (strcmp(ent_subcmd, "link-user-self") == 0) {
            entity_link_user_self_ctx_t ctx = { username, dry_run };
            return run_entity_subcommand(invoke_entity_link_user_self, &ctx);
         }
         fprintf(stderr, "Error: Unknown entity subcommand: %s\n", ent_subcmd);
         fprintf(stderr, "Available: merge, split, aliases, history, list, link-user-self\n");
         return 1;
      }

      fprintf(stderr, "Error: Unknown memory subcommand: %s\n", subcmd);
      fprintf(stderr, "Available: recategorize-all, reextract, reextract-status, entity\n");
      return 1;
   }

   /* === Messaging Channels === */
   if (strcmp(cmd, "messaging") == 0) {
      if (argc < 3) {
         fprintf(stderr, "Error: Missing messaging subcommand\n");
         fprintf(stderr,
                 "Usage: %s messaging generate-link-code --user <username> "
                 "[--provider telegram|discord|slack|sms]\n",
                 argv[0]);
         return 1;
      }
      const char *subcmd = argv[2];

      if (strcmp(subcmd, "generate-link-code") == 0) {
         const char *username = NULL;
         const char *provider = NULL;
         for (int i = 3; i < argc; i++) {
            const char *arg = argv[i];
            if (strcmp(arg, "--user") == 0 && i + 1 < argc) {
               username = argv[++i];
            } else if (strcmp(arg, "--provider") == 0 && i + 1 < argc) {
               provider = argv[++i];
            } else {
               fprintf(stderr, "Error: Unknown option for messaging generate-link-code: %s\n", arg);
               return 1;
            }
         }
         if (!username || !username[0]) {
            fprintf(stderr, "Error: --user <username> is required\n");
            fprintf(stderr,
                    "Usage: %s messaging generate-link-code --user <username> "
                    "[--provider telegram|discord|slack|sms]\n",
                    argv[0]);
            return 1;
         }

         int fd = admin_client_connect();
         if (fd < 0) {
            return 1;
         }
         char response[512];
         admin_resp_code_t resp = admin_client_messaging_generate_link_code(fd, username, provider,
                                                                            response,
                                                                            sizeof(response));
         admin_client_disconnect(fd);
         if (resp == ADMIN_RESP_SUCCESS) {
            printf("%s\n", response);
            return 0;
         }
         fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
         return 1;
      }

      if (strcmp(subcmd, "list-channels") == 0) {
         const char *username = NULL;
         for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
               username = argv[++i];
            } else {
               fprintf(stderr, "Error: Unknown option for messaging list-channels: %s\n", argv[i]);
               return 1;
            }
         }
         if (!username || !username[0]) {
            fprintf(stderr, "Error: --user <username> is required\n");
            return 1;
         }
         int fd = admin_client_connect();
         if (fd < 0) {
            return 1;
         }
         char response[ADMIN_MSG_CONTENT_MAX + 1];
         admin_resp_code_t resp = admin_client_messaging_list_channels(fd, username, response,
                                                                       sizeof(response));
         admin_client_disconnect(fd);
         if (resp == ADMIN_RESP_SUCCESS) {
            printf("%s\n", response);
            return 0;
         }
         fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
         return 1;
      }

      if (strcmp(subcmd, "unlink") == 0) {
         const char *username = NULL;
         const char *name = NULL;
         for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
               username = argv[++i];
            } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
               name = argv[++i];
            } else {
               fprintf(stderr, "Error: Unknown option for messaging unlink: %s\n", argv[i]);
               return 1;
            }
         }
         if (!username || !username[0] || !name || !name[0]) {
            fprintf(stderr, "Error: --user <username> and --name <channel> are required\n");
            return 1;
         }
         int fd = admin_client_connect();
         if (fd < 0) {
            return 1;
         }
         char response[512];
         admin_resp_code_t resp = admin_client_messaging_unlink_channel(fd, username, name,
                                                                        response, sizeof(response));
         admin_client_disconnect(fd);
         if (resp == ADMIN_RESP_SUCCESS) {
            printf("%s\n", response);
            return 0;
         }
         fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
         return 1;
      }

      if (strcmp(subcmd, "reenable") == 0) {
         const char *username = NULL;
         const char *name = NULL;
         for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
               username = argv[++i];
            } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
               name = argv[++i];
            } else {
               fprintf(stderr, "Error: Unknown option for messaging reenable: %s\n", argv[i]);
               return 1;
            }
         }
         if (!username || !username[0] || !name || !name[0]) {
            fprintf(stderr, "Error: --user <username> and --name <channel> are required\n");
            return 1;
         }
         int fd = admin_client_connect();
         if (fd < 0) {
            return 1;
         }
         char response[512];
         admin_resp_code_t resp = admin_client_messaging_reenable_channel(fd, username, name,
                                                                          response,
                                                                          sizeof(response));
         admin_client_disconnect(fd);
         if (resp == ADMIN_RESP_SUCCESS) {
            printf("%s\n", response);
            return 0;
         }
         fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
         return 1;
      }

      if (strcmp(subcmd, "link-attempts") == 0) {
         const char *provider = NULL;
         int limit = 0;
         for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc) {
               provider = argv[++i];
            } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
               limit = atoi(argv[++i]);
            } else {
               fprintf(stderr, "Error: Unknown option for messaging link-attempts: %s\n", argv[i]);
               return 1;
            }
         }
         int fd = admin_client_connect();
         if (fd < 0) {
            return 1;
         }
         char response[ADMIN_MSG_CONTENT_MAX + 1];
         admin_resp_code_t resp = admin_client_messaging_link_attempts(fd, provider, limit,
                                                                       response, sizeof(response));
         admin_client_disconnect(fd);
         if (resp == ADMIN_RESP_SUCCESS) {
            printf("%s", response); /* table already newline-terminated */
            return 0;
         }
         fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
         return 1;
      }

      fprintf(stderr, "Error: Unknown messaging subcommand: %s\n", subcmd);
      fprintf(stderr,
              "Available: generate-link-code, list-channels, unlink, reenable, link-attempts\n");
      return 1;
   }

   /* OTA updates */
   if (strcmp(cmd, "ota") == 0) {
      if (argc < 3) {
         fprintf(stderr, "Error: Missing ota subcommand\n");
         fprintf(stderr, "Usage: %s ota list\n", argv[0]);
         fprintf(stderr, "       %s ota rescan\n", argv[0]);
         fprintf(stderr, "       %s ota push --uuid <uuid> --version <v> [--allow-downgrade]\n",
                 argv[0]);
         fprintf(
             stderr,
             "       %s ota push-all --platform <rpi|esp32> --version <v> [--allow-downgrade]\n",
             argv[0]);
         fprintf(stderr, "       %s ota rollout-status | rollout-abort\n", argv[0]);
         return 1;
      }
      const char *subcmd = argv[2];

      if (strcmp(subcmd, "list") == 0) {
         int fd = admin_client_connect();
         if (fd < 0) {
            return 1;
         }
         char response[ADMIN_MSG_CONTENT_MAX + 1];
         admin_resp_code_t resp = admin_client_ota_list(fd, response, sizeof(response));
         admin_client_disconnect(fd);
         if (resp == ADMIN_RESP_SUCCESS) {
            printf("%s\n", response);
            return 0;
         }
         fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
         return 1;
      }

      if (strcmp(subcmd, "push") == 0) {
         const char *uuid = NULL;
         const char *version = NULL;
         bool allow_downgrade = false;
         for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--uuid") == 0 && i + 1 < argc) {
               uuid = argv[++i];
            } else if (strcmp(argv[i], "--version") == 0 && i + 1 < argc) {
               version = argv[++i];
            } else if (strcmp(argv[i], "--allow-downgrade") == 0) {
               allow_downgrade = true;
            } else {
               fprintf(stderr, "Error: Unknown option for ota push: %s\n", argv[i]);
               return 1;
            }
         }
         if (!uuid || !uuid[0] || !version || !version[0]) {
            fprintf(stderr, "Error: --uuid <uuid> and --version <v> are required\n");
            fprintf(stderr, "Usage: %s ota push --uuid <uuid> --version <v> [--allow-downgrade]\n",
                    argv[0]);
            return 1;
         }
         int fd = admin_client_connect();
         if (fd < 0) {
            return 1;
         }
         char response[512];
         admin_resp_code_t resp = admin_client_ota_push(fd, uuid, version, allow_downgrade,
                                                        response, sizeof(response));
         admin_client_disconnect(fd);
         if (resp == ADMIN_RESP_SUCCESS) {
            printf("%s\n", response);
            return 0;
         }
         fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
         return 1;
      }

      if (strcmp(subcmd, "rescan") == 0) {
         int fd = admin_client_connect();
         if (fd < 0) {
            return 1;
         }
         char response[256];
         admin_resp_code_t resp = admin_client_ota_rescan(fd, response, sizeof(response));
         admin_client_disconnect(fd);
         if (resp == ADMIN_RESP_SUCCESS) {
            printf("%s\n", response);
            return 0;
         }
         fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
         return 1;
      }

      if (strcmp(subcmd, "push-all") == 0) {
         const char *platform = NULL;
         const char *version = NULL;
         bool allow_downgrade = false;
         for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--platform") == 0 && i + 1 < argc) {
               platform = argv[++i];
            } else if (strcmp(argv[i], "--version") == 0 && i + 1 < argc) {
               version = argv[++i];
            } else if (strcmp(argv[i], "--allow-downgrade") == 0) {
               allow_downgrade = true;
            } else {
               fprintf(stderr, "Error: Unknown option for ota push-all: %s\n", argv[i]);
               return 1;
            }
         }
         int tier = platform && strcmp(platform, "esp32") == 0 ? 2
                    : platform && strcmp(platform, "rpi") == 0 ? 1
                                                               : 0;
         if (tier == 0 || !version || !version[0]) {
            fprintf(stderr, "Error: --platform <rpi|esp32> and --version <v> are required\n");
            fprintf(
                stderr,
                "Usage: %s ota push-all --platform <rpi|esp32> --version <v> [--allow-downgrade]\n",
                argv[0]);
            return 1;
         }
         int fd = admin_client_connect();
         if (fd < 0) {
            return 1;
         }
         char response[512];
         admin_resp_code_t resp = admin_client_ota_push_all(fd, tier, version, allow_downgrade,
                                                            response, sizeof(response));
         admin_client_disconnect(fd);
         if (resp == ADMIN_RESP_SUCCESS) {
            printf("%s\n", response);
            return 0;
         }
         fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
         return 1;
      }

      if (strcmp(subcmd, "rollout-status") == 0 || strcmp(subcmd, "rollout-abort") == 0) {
         bool abort_it = (strcmp(subcmd, "rollout-abort") == 0);
         int fd = admin_client_connect();
         if (fd < 0) {
            return 1;
         }
         char response[512];
         admin_resp_code_t resp = abort_it ? admin_client_ota_rollout_abort(fd, response,
                                                                            sizeof(response))
                                           : admin_client_ota_rollout_status(fd, response,
                                                                             sizeof(response));
         admin_client_disconnect(fd);
         if (resp == ADMIN_RESP_SUCCESS) {
            printf("%s\n", response);
            return 0;
         }
         fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
         return 1;
      }

      fprintf(stderr, "Error: Unknown ota subcommand: %s\n", subcmd);
      fprintf(stderr, "Available: list, rescan, push, push-all, rollout-status, rollout-abort\n");
      return 1;
   }

   if (strcmp(cmd, "mcp") == 0) {
      if (argc < 3) {
         fprintf(stderr,
                 "Usage: %s mcp {list|status|reset|grant <user> <alias>|revoke <user> <alias>}\n",
                 argv[0]);
         return 1;
      }
      const char *subcmd = argv[2];
      int fd = admin_client_connect();
      if (fd < 0) {
         return 1;
      }
      char response[2048];
      admin_resp_code_t resp;
      if (strcmp(subcmd, "list") == 0 || strcmp(subcmd, "status") == 0) {
         resp = admin_client_mcp_list(fd, response, sizeof(response));
      } else if (strcmp(subcmd, "reset") == 0) {
         resp = admin_client_mcp_reset(fd, response, sizeof(response));
      } else if (strcmp(subcmd, "grant") == 0 || strcmp(subcmd, "revoke") == 0) {
         if (argc < 5) {
            admin_client_disconnect(fd);
            fprintf(stderr, "Usage: %s mcp %s <user> <alias>\n", argv[0], subcmd);
            return 1;
         }
         resp = (strcmp(subcmd, "grant") == 0)
                    ? admin_client_mcp_grant(fd, argv[3], argv[4], response, sizeof(response))
                    : admin_client_mcp_revoke(fd, argv[3], argv[4], response, sizeof(response));
      } else {
         admin_client_disconnect(fd);
         fprintf(stderr, "Error: Unknown mcp subcommand: %s\n", subcmd);
         return 1;
      }
      admin_client_disconnect(fd);
      if (resp == ADMIN_RESP_SUCCESS) {
         printf("%s\n", response);
         return 0;
      }
      fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
      return 1;
   }

   if (strcmp(cmd, "code-project") == 0) {
      if (argc < 3) {
         fprintf(stderr,
                 "Usage: %s code-project {list | import <url> [--name n] [--branch b] [--global] | "
                 "link <path> [--name n] | refresh <name> | rebuild <name> | "
                 "set-branch <name> <branch> | delete <name>}\n",
                 argv[0]);
         return 1;
      }
      const char *subcmd = argv[2];
      int fd = admin_client_connect();
      if (fd < 0) {
         return 1;
      }
      char response[3072];
      admin_resp_code_t resp;
      if (strcmp(subcmd, "list") == 0) {
         resp = admin_client_code_proj_list(fd, response, sizeof(response));
      } else if (strcmp(subcmd, "import") == 0) {
         const char *url = NULL;
         const char *name = NULL;
         const char *branch = NULL;
         bool global = false;
         for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
               name = argv[++i];
            } else if (strcmp(argv[i], "--branch") == 0 && i + 1 < argc) {
               branch = argv[++i];
            } else if (strcmp(argv[i], "--global") == 0) {
               global = true;
            } else if (argv[i][0] != '-') {
               url = argv[i];
            }
         }
         if (url == NULL) {
            admin_client_disconnect(fd);
            fprintf(stderr,
                    "Usage: %s code-project import <url> [--name n] [--branch b] [--global]\n",
                    argv[0]);
            return 1;
         }
         /* Derive name from the URL's last path component (strip .git) if absent. */
         char derived[64] = { 0 };
         if (name == NULL) {
            const char *slash = strrchr(url, '/');
            const char *base = slash ? slash + 1 : url;
            snprintf(derived, sizeof(derived), "%s", base);
            char *dot = strstr(derived, ".git");
            if (dot != NULL) {
               *dot = '\0';
            }
            name = derived;
         }
         resp = admin_client_code_proj_import(fd, url, name, branch, global, response,
                                              sizeof(response));
      } else if (strcmp(subcmd, "link") == 0) {
         const char *path = NULL;
         const char *name = NULL;
         for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
               name = argv[++i];
            } else if (argv[i][0] != '-') {
               path = argv[i];
            }
         }
         if (path == NULL) {
            admin_client_disconnect(fd);
            fprintf(stderr, "Usage: %s code-project link <path> [--name n]\n", argv[0]);
            return 1;
         }
         resp = admin_client_code_proj_link(fd, path, name, response, sizeof(response));
      } else if (strcmp(subcmd, "set-branch") == 0) {
         if (argc < 5) {
            admin_client_disconnect(fd);
            fprintf(stderr, "Usage: %s code-project set-branch <name> <branch>\n", argv[0]);
            return 1;
         }
         resp = admin_client_code_proj_set_branch(fd, argv[3], argv[4], response, sizeof(response));
      } else if (strcmp(subcmd, "refresh") == 0 || strcmp(subcmd, "rebuild") == 0 ||
                 strcmp(subcmd, "delete") == 0) {
         if (argc < 4) {
            admin_client_disconnect(fd);
            fprintf(stderr, "Usage: %s code-project %s <name>\n", argv[0], subcmd);
            return 1;
         }
         if (strcmp(subcmd, "refresh") == 0) {
            resp = admin_client_code_proj_refresh(fd, argv[3], response, sizeof(response));
         } else if (strcmp(subcmd, "rebuild") == 0) {
            resp = admin_client_code_proj_rebuild(fd, argv[3], response, sizeof(response));
         } else {
            resp = admin_client_code_proj_delete(fd, argv[3], response, sizeof(response));
         }
      } else {
         admin_client_disconnect(fd);
         fprintf(stderr, "Error: Unknown code-project subcommand: %s\n", subcmd);
         return 1;
      }
      admin_client_disconnect(fd);
      if (resp == ADMIN_RESP_SUCCESS) {
         printf("%s\n", response);
         return 0;
      }
      fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
      return 1;
   }

   fprintf(stderr, "Error: Unknown command: %s\n", cmd);
   print_usage(argv[0]);
   return 1;
}
