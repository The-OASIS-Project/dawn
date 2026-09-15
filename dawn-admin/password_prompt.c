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
 * Secure password prompting implementation.
 */

#define _GNU_SOURCE

#include "password_prompt.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "utils/string_utils.h"

/* Saved terminal settings for signal handler restoration */
static struct termios s_saved_term;
static volatile sig_atomic_t s_term_modified = 0;

/**
 * @brief Signal handler to restore terminal on Ctrl+C.
 */
static void signal_handler(int sig) {
   if (s_term_modified) {
      tcsetattr(STDIN_FILENO, TCSANOW, &s_saved_term);
      s_term_modified = 0;
   }
   /* Re-raise signal for default handling */
   signal(sig, SIG_DFL);
   raise(sig);
}

void secure_clear(void *buf, size_t buflen) {
#ifdef __GLIBC__
   explicit_bzero(buf, buflen);
#else
   /* Fallback: volatile prevents compiler from optimizing away */
   volatile unsigned char *p = buf;
   while (buflen--) {
      *p++ = 0;
   }
#endif
}

/**
 * @brief Check for password from environment variable.
 *
 * @param env_var  Name of environment variable to check.
 * @param buf      Buffer to store password.
 * @param buflen   Size of buffer.
 *
 * @return 0 if password read from env, 1 if not present, -1 on error.
 */
static int check_password_env(const char *env_var, char *buf, size_t buflen) {
   const char *env_password = getenv(env_var);
   if (env_password == NULL) {
      return 1; /* Not present, caller should prompt interactively */
   }

   size_t len = strlen(env_password);

   /* Validate length */
   if (len < PASSWORD_MIN_LENGTH) {
      fprintf(stderr, "Error: Password from %s too short (minimum %d characters)\n", env_var,
              PASSWORD_MIN_LENGTH);
      return -1;
   }

   if (len >= buflen) {
      fprintf(stderr, "Error: Password from %s too long\n", env_var);
      return -1;
   }

   /* Copy password */
   memcpy(buf, env_password, len + 1);

   /* Security notice for audit trail */
   fprintf(stderr, "Note: Using password from %s environment variable\n", env_var);

   return 0;
}

int prompt_password(const char *prompt, char *buf, size_t buflen) {
   if (!buf || buflen < PASSWORD_MIN_LENGTH) {
      return -1;
   }

   /* Check for environment variable override (for automation) */
   /* Determine which env var to check based on prompt content */
   const char *env_var = NULL;
   if (strstr(prompt, "Admin") != NULL || strstr(prompt, "admin") != NULL) {
      env_var = "DAWN_ADMIN_PASSWORD";
   } else {
      env_var = "DAWN_PASSWORD";
   }

   int env_result = check_password_env(env_var, buf, buflen);
   if (env_result == 0) {
      return 0; /* Got password from environment */
   } else if (env_result < 0) {
      return -1; /* Environment password invalid */
   }

   /* Fall through to interactive prompt */
   if (!isatty(STDIN_FILENO)) {
      fprintf(stderr, "Error: Password input requires a terminal\n");
      fprintf(stderr, "Hint: Set %s environment variable for automation\n", env_var);
      return -1;
   }

   struct termios new_term;
   struct sigaction sa_old_int, sa_old_term;
   struct sigaction sa_new = { 0 };

   /* Save current terminal settings */
   if (tcgetattr(STDIN_FILENO, &s_saved_term) != 0) {
      fprintf(stderr, "Error: Failed to get terminal settings\n");
      return -1;
   }

   /* Install signal handlers BEFORE modifying terminal */
   sa_new.sa_handler = signal_handler;
   sa_new.sa_flags = 0;
   sigemptyset(&sa_new.sa_mask);
   sigaction(SIGINT, &sa_new, &sa_old_int);
   sigaction(SIGTERM, &sa_new, &sa_old_term);

   /* Disable echo */
   new_term = s_saved_term;
   new_term.c_lflag &= ~(ECHO | ECHOE | ECHOK);

   if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &new_term) != 0) {
      sigaction(SIGINT, &sa_old_int, NULL);
      sigaction(SIGTERM, &sa_old_term, NULL);
      fprintf(stderr, "Error: Failed to set terminal settings\n");
      return -1;
   }
   s_term_modified = 1;

   /* Print prompt */
   fprintf(stderr, "%s", prompt);
   fflush(stderr);

   /* Read password */
   char *result = fgets(buf, buflen, stdin);

   /* ALWAYS restore terminal - even on read error */
   tcsetattr(STDIN_FILENO, TCSANOW, &s_saved_term);
   s_term_modified = 0;

   /* Restore signal handlers */
   sigaction(SIGINT, &sa_old_int, NULL);
   sigaction(SIGTERM, &sa_old_term, NULL);

   /* Print newline (since echo was disabled) */
   fprintf(stderr, "\n");

   if (result == NULL) {
      return -1;
   }

   /* Strip trailing newline */
   size_t len = strlen(buf);
   if (len > 0 && buf[len - 1] == '\n') {
      buf[len - 1] = '\0';
      len--;
   }

   /* Validate minimum length */
   if (len < PASSWORD_MIN_LENGTH) {
      secure_clear(buf, buflen);
      fprintf(stderr, "Error: Password must be at least %d characters\n", PASSWORD_MIN_LENGTH);
      return -1;
   }

   return 0;
}

int prompt_password_confirm(char *buf, size_t buflen) {
   char confirm[PASSWORD_MAX_LENGTH];

   /* Check for environment variable - skip confirmation if present */
   int env_result = check_password_env("DAWN_PASSWORD", buf, buflen);
   if (env_result == 0) {
      return 0; /* Got password from environment, no confirmation needed */
   } else if (env_result < 0) {
      return -1; /* Environment password invalid */
   }

   /* Show requirements before prompting */
   fprintf(stderr, "Password requirements: minimum %d characters\n\n", PASSWORD_MIN_LENGTH);

   /* First password prompt */
   if (prompt_password("Enter password: ", buf, buflen) != 0) {
      return -1;
   }

   /* Confirmation prompt */
   if (prompt_password("Confirm password: ", confirm, sizeof(confirm)) != 0) {
      secure_clear(buf, buflen);
      return -1;
   }

   /* Compare passwords */
   if (strcmp(buf, confirm) != 0) {
      secure_clear(buf, buflen);
      secure_clear(confirm, sizeof(confirm));
      fprintf(stderr, "Error: Passwords do not match\n");
      return -1;
   }

   /* Clear confirmation buffer */
   secure_clear(confirm, sizeof(confirm));
   return 0;
}

int prompt_input(const char *prompt, char *buf, size_t buflen) {
   if (!buf || buflen < 2) {
      return -1;
   }

   /* Check for environment variable override (for automation) */
   /* Determine which env var to check based on prompt content */
   const char *env_var = NULL;
   if (strstr(prompt, "Admin username") != NULL || strstr(prompt, "admin username") != NULL) {
      env_var = "DAWN_ADMIN_USER";
   } else if (strstr(prompt, "token") != NULL || strstr(prompt, "Token") != NULL) {
      env_var = "DAWN_SETUP_TOKEN";
   }

   if (env_var != NULL) {
      const char *env_value = getenv(env_var);
      if (env_value != NULL) {
         safe_strncpy(buf, env_value, buflen);
         fprintf(stderr, "Note: Using value from %s environment variable\n", env_var);
         return 0;
      }
   }

   /* Fall through to interactive prompt */
   if (!isatty(STDIN_FILENO)) {
      fprintf(stderr, "Error: Input requires a terminal\n");
      if (env_var != NULL) {
         fprintf(stderr, "Hint: Set %s environment variable for automation\n", env_var);
      }
      return -1;
   }

   fprintf(stderr, "%s", prompt);
   fflush(stderr);

   char *result = fgets(buf, buflen, stdin);
   if (result == NULL) {
      return -1;
   }

   /* Strip trailing newline */
   size_t len = strlen(buf);
   if (len > 0 && buf[len - 1] == '\n') {
      buf[len - 1] = '\0';
   }

   return 0;
}
