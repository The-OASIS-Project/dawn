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

/**
 * @file tui.c
 * @brief Terminal User Interface implementation for DAWN
 *
 * Provides ncurses-based dashboard for real-time DAWN monitoring.
 */

#include "ui/tui.h"

#include <ctype.h>
#include <fcntl.h>
#include <locale.h>
#include <ncurses.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#include "conversation_manager.h"
#include "core/session_manager.h"
#include "input_queue.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "ui/metrics.h"

/* ============================================================================
 * Color Pair Definitions
 * ============================================================================ */

/* Color pairs for each theme element */
#define COLOR_PAIR_BORDER 1
#define COLOR_PAIR_TITLE 2
#define COLOR_PAIR_TEXT 3
#define COLOR_PAIR_HIGHLIGHT 4
#define COLOR_PAIR_DIM 5
#define COLOR_PAIR_VALUE 6
#define COLOR_PAIR_BAR 7
#define COLOR_PAIR_WARNING 8

/* ============================================================================
 * Static Variables
 * ============================================================================ */

static int g_tui_initialized = 0;
static tui_mode_t g_tui_mode = TUI_MODE_OFF;
static tui_theme_t g_current_theme = TUI_THEME_BLUE;
static int g_show_help = 0;
static int g_term_rows = 0;
static int g_term_cols = 0;
static int g_saved_stderr = -1; /* Saved stderr fd for restoration */
static int g_dev_null_fd = -1;  /* /dev/null fd */

/* Text input mode state */
static int g_input_mode = 0;                       /* 1 when in input mode */
static char g_input_buffer[TUI_INPUT_MAX_LEN + 1]; /* Current input text */
static int g_input_len = 0;                        /* Current input length */
static int g_input_cursor = 0;                     /* Cursor position */

/* Text input now uses unified input_queue.h instead of internal queue */

/* Use ASCII box drawing for maximum compatibility */
#define USE_ASCII_BOX 1

/**
 * @brief Redirect stderr to /dev/null to suppress library output
 */
static void redirect_stderr_to_null(void) {
   if (g_saved_stderr == -1) {
      g_saved_stderr = dup(STDERR_FILENO);
      g_dev_null_fd = open("/dev/null", O_WRONLY);
      if (g_dev_null_fd != -1) {
         dup2(g_dev_null_fd, STDERR_FILENO);
      }
   }
}

/**
 * @brief Restore stderr from saved fd
 */
static void restore_stderr(void) {
   if (g_saved_stderr != -1) {
      dup2(g_saved_stderr, STDERR_FILENO);
      close(g_saved_stderr);
      g_saved_stderr = -1;
   }
   if (g_dev_null_fd != -1) {
      close(g_dev_null_fd);
      g_dev_null_fd = -1;
   }
}

/* ============================================================================
 * Theme Configuration
 * ============================================================================ */

static void setup_theme_green(void) {
   /* Apple ][ Classic Green theme */
   if (can_change_color()) {
      init_color(COLOR_GREEN, 0, 1000, 0);    /* Bright green */
      init_color(COLOR_YELLOW, 0, 800, 0);    /* Medium green for borders */
      init_color(COLOR_CYAN, 200, 1000, 200); /* Light green for highlights */
      init_color(COLOR_BLUE, 0, 533, 0);      /* Dark green for dim text */
   }

   init_pair(COLOR_PAIR_BORDER, COLOR_GREEN, COLOR_BLACK);
   init_pair(COLOR_PAIR_TITLE, COLOR_GREEN, COLOR_BLACK);
   init_pair(COLOR_PAIR_TEXT, COLOR_GREEN, COLOR_BLACK);
   init_pair(COLOR_PAIR_HIGHLIGHT, COLOR_GREEN, COLOR_BLACK);
   init_pair(COLOR_PAIR_DIM, COLOR_GREEN, COLOR_BLACK);
   init_pair(COLOR_PAIR_VALUE, COLOR_GREEN, COLOR_BLACK);
   init_pair(COLOR_PAIR_BAR, COLOR_GREEN, COLOR_BLACK);
   init_pair(COLOR_PAIR_WARNING, COLOR_BLACK, COLOR_GREEN);
}

static void setup_theme_blue(void) {
   /* JARVIS-inspired Blue theme */
   if (can_change_color()) {
      init_color(COLOR_CYAN, 0, 850, 1000);    /* Cyan-blue primary */
      init_color(COLOR_BLUE, 300, 850, 1000);  /* Light sky blue for borders */
      init_color(COLOR_WHITE, 0, 1000, 1000);  /* Bright cyan highlights */
      init_color(COLOR_MAGENTA, 0, 800, 640);  /* Teal accent */
      init_color(COLOR_YELLOW, 900, 750, 300); /* Soft amber for dim text */
   }

   init_pair(COLOR_PAIR_BORDER, COLOR_BLUE, COLOR_BLACK);
   init_pair(COLOR_PAIR_TITLE, COLOR_CYAN, COLOR_BLACK);
   init_pair(COLOR_PAIR_TEXT, COLOR_CYAN, COLOR_BLACK);
   init_pair(COLOR_PAIR_HIGHLIGHT, COLOR_WHITE, COLOR_BLACK);
   init_pair(COLOR_PAIR_DIM, COLOR_YELLOW, COLOR_BLACK);
   init_pair(COLOR_PAIR_VALUE, COLOR_CYAN, COLOR_BLACK);
   init_pair(COLOR_PAIR_BAR, COLOR_CYAN, COLOR_BLACK);
   init_pair(COLOR_PAIR_WARNING, COLOR_BLACK, COLOR_CYAN);
}

static void setup_theme_bw(void) {
   /* High Contrast Black/White theme */
   init_pair(COLOR_PAIR_BORDER, COLOR_WHITE, COLOR_BLACK);
   init_pair(COLOR_PAIR_TITLE, COLOR_WHITE, COLOR_BLACK);
   init_pair(COLOR_PAIR_TEXT, COLOR_WHITE, COLOR_BLACK);
   init_pair(COLOR_PAIR_HIGHLIGHT, COLOR_WHITE, COLOR_BLACK);
   init_pair(COLOR_PAIR_DIM, COLOR_WHITE, COLOR_BLACK);
   init_pair(COLOR_PAIR_VALUE, COLOR_WHITE, COLOR_BLACK);
   init_pair(COLOR_PAIR_BAR, COLOR_WHITE, COLOR_BLACK);
   init_pair(COLOR_PAIR_WARNING, COLOR_BLACK, COLOR_WHITE);
}

static void apply_theme(tui_theme_t theme) {
   switch (theme) {
      case TUI_THEME_GREEN:
         setup_theme_green();
         break;
      case TUI_THEME_BLUE:
         setup_theme_blue();
         break;
      case TUI_THEME_BW:
         setup_theme_bw();
         break;
      default:
         setup_theme_blue();
         break;
   }
}

/* ============================================================================
 * Drawing Helpers
 * ============================================================================ */

/**
 * @brief Draw a box with title using simple ASCII characters
 *
 * Uses +, -, | for maximum terminal compatibility
 */
static void draw_box(int y, int x, int height, int width, const char *title) {
   attron(COLOR_PAIR(COLOR_PAIR_BORDER));

   /* Top border with title */
   mvaddch(y, x, '+');
   if (title && strlen(title) > 0) {
      addch('-');
      addch('[');
      attroff(COLOR_PAIR(COLOR_PAIR_BORDER));
      attron(COLOR_PAIR(COLOR_PAIR_TITLE) | A_BOLD);
      printw("%s", title);
      attroff(COLOR_PAIR(COLOR_PAIR_TITLE) | A_BOLD);
      attron(COLOR_PAIR(COLOR_PAIR_BORDER));
      addch(']');
      int title_len = strlen(title) + 4;
      for (int i = title_len; i < width - 2; i++) {
         addch('-');
      }
   } else {
      for (int i = 0; i < width - 2; i++) {
         addch('-');
      }
   }
   addch('+');

   /* Side borders */
   for (int i = 1; i < height - 1; i++) {
      mvaddch(y + i, x, '|');
      mvaddch(y + i, x + width - 1, '|');
   }

   /* Bottom border */
   mvaddch(y + height - 1, x, '+');
   for (int i = 0; i < width - 2; i++) {
      addch('-');
   }
   addch('+');

   attroff(COLOR_PAIR(COLOR_PAIR_BORDER));
}

/**
 * @brief Draw a horizontal progress bar using ASCII
 */
static void draw_bar(int y, int x, int width, float percentage) {
   int filled = (int)(percentage * width);
   if (filled > width)
      filled = width;
   if (filled < 0)
      filled = 0;

   attron(COLOR_PAIR(COLOR_PAIR_BAR));
   move(y, x);
   addch('[');
   for (int i = 0; i < width - 2; i++) {
      if (i < filled) {
         addch('#'); /* Hash for filled */
      } else {
         attron(A_DIM);
         addch('.'); /* Dot for empty */
         attroff(A_DIM);
      }
   }
   addch(']');
   attroff(COLOR_PAIR(COLOR_PAIR_BAR));
}

/**
 * @brief Format uptime as HH:MM:SS
 */
static void format_uptime(time_t seconds, char *buf, size_t buf_size) {
   int hours = seconds / 3600;
   int mins = (seconds % 3600) / 60;
   int secs = seconds % 60;
   snprintf(buf, buf_size, "%02d:%02d:%02d", hours, mins, secs);
}

/**
 * @brief Format large number with commas
 */
static void format_number(uint64_t num, char *buf, size_t buf_size) {
   if (num >= 1000000) {
      snprintf(buf, buf_size, "%lu,%03lu,%03lu", (unsigned long)(num / 1000000),
               (unsigned long)((num / 1000) % 1000), (unsigned long)(num % 1000));
   } else if (num >= 1000) {
      snprintf(buf, buf_size, "%lu,%03lu", (unsigned long)(num / 1000),
               (unsigned long)(num % 1000));
   } else {
      snprintf(buf, buf_size, "%lu", (unsigned long)num);
   }
}

/* ============================================================================
 * Panel Drawing Functions
 * ============================================================================ */

static void draw_status_panel(int y, int x, int width, dawn_metrics_t *metrics) {
   /* Build title with version info */
   char title[64];
   snprintf(title, sizeof(title), "DAWN v%s (%s)", VERSION_NUMBER, GIT_SHA);
   draw_box(y, x, 5, width, title);

   char uptime_str[16];
   format_uptime(metrics_get_uptime(), uptime_str, sizeof(uptime_str));

   /* Line 1: State, Uptime, Mode */
   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   mvprintw(y + 1, x + 2, "State: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   attron(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT) | A_BOLD);
   printw("%-18s", dawn_state_name(metrics->current_state));
   attroff(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT) | A_BOLD);

   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   printw("  Uptime: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   printw("%s", uptime_str);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));

   /* Line 2: LLM info */
   const char *llm_type = (metrics->current_llm_type == LLM_LOCAL)
                              ? "Local"
                              : ((metrics->current_llm_type == LLM_CLOUD) ? "Cloud" : "N/A");
   const char *provider_name = llm_get_cloud_provider_name();
   const char *model_name = llm_get_model_name();

   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   mvprintw(y + 2, x + 2, "LLM: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   if (metrics->current_cloud_provider != CLOUD_PROVIDER_NONE) {
      printw("%s (%s: %s)", llm_type, provider_name, model_name);
   } else {
      printw("%s", llm_type);
   }
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));

   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   printw("    ASR: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   printw("Whisper");
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));

   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   printw("    VAD: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   printw("Silero");
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));

   /* Line 3: AEC status */
   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   mvprintw(y + 3, x + 2, "AEC: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));

   /* AEC backend is WebRTC when enabled */
   const char *aec_backend = "WebRTC";

   if (metrics->aec_enabled) {
      if (metrics->aec_calibrated) {
         attron(COLOR_PAIR(COLOR_PAIR_VALUE));
         printw("%s (%dms, corr:%.2f)", aec_backend, metrics->aec_delay_ms,
                metrics->aec_correlation);
         attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
      } else {
         attron(COLOR_PAIR(COLOR_PAIR_WARNING));
         printw("%s (not calibrated)", aec_backend);
         attroff(COLOR_PAIR(COLOR_PAIR_WARNING));
      }
   } else {
      attron(COLOR_PAIR(COLOR_PAIR_DIM));
#if defined(ENABLE_AEC)
      printw("%s (disabled)", aec_backend);
#else
      printw("Not compiled");
#endif
      attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   }

   /* Barge-in count */
   if (metrics->bargein_count > 0) {
      attron(COLOR_PAIR(COLOR_PAIR_TEXT));
      printw("    Barge-ins: ");
      attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
      attron(COLOR_PAIR(COLOR_PAIR_VALUE));
      printw("%u", metrics->bargein_count);
      attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   }

   /* Summarizer status */
   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   printw("    Sum: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   if (strcmp(metrics->summarizer_backend, "disabled") == 0 ||
       metrics->summarizer_backend[0] == '\0') {
      attron(COLOR_PAIR(COLOR_PAIR_DIM));
      printw("off");
      attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   } else {
      attron(COLOR_PAIR(COLOR_PAIR_VALUE));
      printw("%s", metrics->summarizer_backend);
      attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
      if (metrics->summarizer_call_count > 0) {
         attron(COLOR_PAIR(COLOR_PAIR_TEXT));
         printw(" (%u: ", metrics->summarizer_call_count);
         attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
         attron(COLOR_PAIR(COLOR_PAIR_VALUE));
         printw("%zu", metrics->summarizer_last_in_bytes);
         attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
         attron(COLOR_PAIR(COLOR_PAIR_TEXT));
         printw("→");
         attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
         attron(COLOR_PAIR(COLOR_PAIR_VALUE));
         printw("%zu", metrics->summarizer_last_out_bytes);
         attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
         attron(COLOR_PAIR(COLOR_PAIR_TEXT));
         printw(")");
         attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
      }
   }
}

static void draw_session_stats_panel(int y, int x, int width, dawn_metrics_t *metrics) {
   draw_box(y, x, 8, width, "Session Stats");

   char num_buf[32];

   /* Line 1: Query counts */
   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   mvprintw(y + 1, x + 2, "Queries: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   attron(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT) | A_BOLD);
   printw("%u total", metrics->queries_total);
   attroff(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT) | A_BOLD);
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   printw("  (%u cloud, %u local)", metrics->queries_cloud, metrics->queries_local);
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));

   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   printw("    Errors: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   if (metrics->errors_count > 0) {
      attron(COLOR_PAIR(COLOR_PAIR_WARNING));
      printw("%u", metrics->errors_count);
      attroff(COLOR_PAIR(COLOR_PAIR_WARNING));
   } else {
      attron(COLOR_PAIR(COLOR_PAIR_VALUE));
      printw("0");
      attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   }

   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   printw("    Fallbacks: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   printw("%u", metrics->fallbacks_count);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));

   /* Line 3: Token header */
   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   mvprintw(y + 3, x + 2, "Token Usage (This Session):");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));

   /* Line 4: Cloud tokens */
   uint64_t cloud_total = metrics->tokens_cloud_input + metrics->tokens_cloud_output;
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   mvprintw(y + 4, x + 4, "Cloud:  ");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   format_number(metrics->tokens_cloud_input, num_buf, sizeof(num_buf));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   printw("%s input", num_buf);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   printw("  +  ");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   format_number(metrics->tokens_cloud_output, num_buf, sizeof(num_buf));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   printw("%s output", num_buf);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   printw("  =  ");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   format_number(cloud_total, num_buf, sizeof(num_buf));
   attron(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
   printw("%s total", num_buf);
   attroff(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));

   /* Line 5: Local tokens */
   uint64_t local_total = metrics->tokens_local_input + metrics->tokens_local_output;
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   mvprintw(y + 5, x + 4, "Local:  ");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   format_number(metrics->tokens_local_input, num_buf, sizeof(num_buf));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   printw("%s input", num_buf);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   printw("  +  ");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   format_number(metrics->tokens_local_output, num_buf, sizeof(num_buf));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   printw("%s output", num_buf);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   printw("  =  ");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   format_number(local_total, num_buf, sizeof(num_buf));
   attron(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
   printw("%s total", num_buf);
   attroff(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));

   /* Line 6: Cached tokens */
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   mvprintw(y + 6, x + 4, "Cached: ");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   format_number(metrics->tokens_cached, num_buf, sizeof(num_buf));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   printw("%s tokens", num_buf);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
}

static void draw_performance_panel(int y, int x, int width, dawn_metrics_t *metrics) {
   draw_box(y, x, 10, width, "Last Query Performance");

   /* Show last command (truncated) */
   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   mvprintw(y + 1, x + 2, "Command: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   if (strlen(metrics->last_user_command) > 0) {
      /* Truncate if too long */
      int max_cmd_len = width - 14;
      if ((int)strlen(metrics->last_user_command) > max_cmd_len) {
         char truncated[256];
         strncpy(truncated, metrics->last_user_command, max_cmd_len - 3);
         truncated[max_cmd_len - 3] = '\0';
         printw("\"%s...\"", truncated);
      } else {
         printw("\"%s\"", metrics->last_user_command);
      }
   } else {
      printw("(none)");
   }
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));

   /* Latency breakdown header */
   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   mvprintw(y + 3, x + 2, "Latency Breakdown:");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));

   /* Calculate max for bar scaling (use LLM total as reference) */
   double max_ms = metrics->last_llm_total_ms;
   if (max_ms < 100)
      max_ms = 1000; /* Default scale */

   int bar_width = 20;
   int label_col = x + 4;
   int value_col = x + 26;
   int bar_col = x + 38;

   /* ASR */
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   mvprintw(y + 4, label_col, "ASR (RTF: %.3f):", metrics->last_asr_rtf);
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   mvprintw(y + 4, value_col, "%6.0f ms", metrics->last_asr_time_ms);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   draw_bar(y + 4, bar_col, bar_width, (float)(metrics->last_asr_time_ms / max_ms));

   /* LLM TTFT */
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   mvprintw(y + 5, label_col, "LLM TTFT:");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   mvprintw(y + 5, value_col, "%6.0f ms", metrics->last_llm_ttft_ms);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   draw_bar(y + 5, bar_col, bar_width, (float)(metrics->last_llm_ttft_ms / max_ms));

   /* LLM Total */
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   mvprintw(y + 6, label_col, "LLM Total:");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   mvprintw(y + 6, value_col, "%6.0f ms", metrics->last_llm_total_ms);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   draw_bar(y + 6, bar_col, bar_width, (float)(metrics->last_llm_total_ms / max_ms));

   /* TTS */
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   mvprintw(y + 7, label_col, "TTS Generation:");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   mvprintw(y + 7, value_col, "%6.0f ms", metrics->last_tts_time_ms);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   draw_bar(y + 7, bar_col, bar_width, (float)(metrics->last_tts_time_ms / max_ms));

   /* Total Pipeline */
   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   mvprintw(y + 8, label_col, "Total Pipeline:");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   attron(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT) | A_BOLD);
   mvprintw(y + 8, value_col, "%6.0f ms", metrics->last_total_pipeline_ms);
   attroff(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT) | A_BOLD);
}

static void draw_realtime_panel(int y, int x, int width, dawn_metrics_t *metrics) {
   draw_box(y, x, 5, width, "Real-Time Audio");

   /* VAD probability bar */
   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   mvprintw(y + 1, x + 2, "VAD Speech Probability: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));

   int bar_width = 20;
   draw_bar(y + 1, x + 26, bar_width, metrics->current_vad_probability);

   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   printw(" %3.0f%%", metrics->current_vad_probability * 100);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));

   /* State indicator */
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   mvprintw(y + 2, x + 2, "State: ");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));

   /* Color state based on activity */
   if (metrics->current_state == DAWN_STATE_COMMAND_RECORDING ||
       metrics->current_state == DAWN_STATE_PROCESS_COMMAND) {
      attron(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT) | A_BOLD | A_BLINK);
   } else if (metrics->current_state == DAWN_STATE_WAKEWORD_LISTEN) {
      attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   } else {
      attron(COLOR_PAIR(COLOR_PAIR_DIM));
   }
   printw("%s", dawn_state_name(metrics->current_state));
   attroff(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT) | A_BOLD | A_BLINK);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));

   /* Last ASR text (what was heard) */
   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   mvprintw(y + 3, x + 2, "Heard: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   if (strlen(metrics->last_asr_text) > 0) {
      attron(COLOR_PAIR(COLOR_PAIR_VALUE));
      /* Truncate to fit in panel width */
      int max_text_len = width - 12;
      if ((int)strlen(metrics->last_asr_text) > max_text_len) {
         printw("%.*s...", max_text_len - 3, metrics->last_asr_text);
      } else {
         printw("%s", metrics->last_asr_text);
      }
      attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   } else {
      attron(COLOR_PAIR(COLOR_PAIR_DIM));
      printw("(listening...)");
      attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   }
}

/**
 * @brief Find word boundary for wrapping text
 *
 * @param text Text to search
 * @param max_chars Maximum characters to consider
 * @return Number of characters to print (breaks at last space if possible)
 */
static int find_word_break(const char *text, int max_chars) {
   int text_len = strlen(text);
   if (text_len <= max_chars) {
      return text_len;
   }

   /* Look for last space within the max_chars range */
   int break_pos = max_chars;
   for (int j = max_chars - 1; j >= max_chars / 2; j--) {
      if (text[j] == ' ') {
         break_pos = j + 1; /* Include the space, next line starts after it */
         break;
      }
   }
   return break_pos;
}

/**
 * @brief Count lines needed to display text with word wrapping
 */
static int count_wrapped_lines(const char *text, int text_width) {
   int text_len = strlen(text);
   if (text_len == 0)
      return 1;

   int lines = 0;
   int pos = 0;
   while (pos < text_len) {
      int remaining = text_len - pos;
      if (remaining <= text_width) {
         lines++;
         break;
      }
      int chars = find_word_break(text + pos, text_width);
      pos += chars;
      lines++;
   }
   return lines;
}

static void draw_activity_panel(int y, int x, int width, int height, dawn_metrics_t *metrics) {
   draw_box(y, x, height, width, "Recent Activity");

   int max_lines = height - 2;
   int text_width = width - 4; /* Account for borders and padding */
   int current_line = 0;

   if (metrics->log_count == 0 || text_width <= 0) {
      return;
   }

   /* Calculate how many lines each entry needs, working backwards from newest */
   /* We'll collect entries to display, then render them in chronological order */
   int entries_to_show[METRICS_MAX_LOG_ENTRIES];
   int entry_count = 0;
   int total_lines_needed = 0;

   /* Start from most recent entry (one before head) and work backwards */
   for (int i = 0; i < metrics->log_count && total_lines_needed < max_lines; i++) {
      int idx = (metrics->log_head - 1 - i + METRICS_MAX_LOG_ENTRIES) % METRICS_MAX_LOG_ENTRIES;
      int lines_for_entry = count_wrapped_lines(metrics->activity_log[idx], text_width);

      if (total_lines_needed + lines_for_entry <= max_lines) {
         entries_to_show[entry_count++] = idx;
         total_lines_needed += lines_for_entry;
      } else {
         break; /* Can't fit this entry */
      }
   }

   /* Render entries in chronological order (reverse of how we collected them) */
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   for (int i = entry_count - 1; i >= 0; i--) {
      int idx = entries_to_show[i];
      const char *text = metrics->activity_log[idx];
      int text_len = strlen(text);
      int pos = 0;

      while (pos < text_len && current_line < max_lines) {
         int remaining = text_len - pos;
         int chars_to_print;
         if (remaining <= text_width) {
            chars_to_print = remaining;
         } else {
            chars_to_print = find_word_break(text + pos, text_width);
         }
         mvprintw(y + 1 + current_line, x + 2, "%.*s", chars_to_print, text + pos);
         pos += chars_to_print;
         current_line++;
      }
   }
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
}

static void draw_footer(int y, int width) {
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   if (g_input_mode) {
      mvprintw(y, 1, " [Enter] Send  [Esc] Cancel  [Backspace] Delete");
   } else {
      mvprintw(y, 1, " [I]nput  [L]ocal  [C]loud  [D]ebug  [R]eset Context  [1/2/3] Theme  [Q]uit");
   }
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));

   /* Right-aligned help hint */
   const char *help_text = g_input_mode ? "Type your message" : "Press ? for help";
   mvprintw(y, width - strlen(help_text) - 1, "%s", help_text);
}

/**
 * @brief Draw the text input panel overlay
 */
static void draw_input_panel(void) {
   int panel_height = 3;
   int panel_width = g_term_cols - 4;
   int start_y = g_term_rows - 5; /* Just above footer */
   int start_x = 2;

   /* Draw panel box */
   attron(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));

   /* Top border with title */
   mvaddch(start_y, start_x, '+');
   for (int i = 1; i < panel_width - 1; i++) {
      addch('-');
   }
   addch('+');

   /* Title */
   char title[64];
   snprintf(title, sizeof(title), " Text Input ");
   mvprintw(start_y, start_x + 2, "%s", title);

   /* Character count on right side of title bar */
   char count_str[16];
   snprintf(count_str, sizeof(count_str), " %d/%d ", g_input_len, TUI_INPUT_MAX_LEN);
   mvprintw(start_y, start_x + panel_width - strlen(count_str) - 2, "%s", count_str);

   /* Input line */
   mvaddch(start_y + 1, start_x, '|');
   attron(COLOR_PAIR(COLOR_PAIR_TEXT));

   /* Clear input area */
   for (int i = 1; i < panel_width - 1; i++) {
      mvaddch(start_y + 1, start_x + i, ' ');
   }

   /* Draw prompt and input text */
   mvprintw(start_y + 1, start_x + 2, "> ");

   /* Calculate visible portion of input (scroll if needed) */
   int visible_width = panel_width - 7; /* Account for borders, prompt, cursor */
   int display_start = 0;
   if (g_input_cursor > visible_width - 1) {
      display_start = g_input_cursor - visible_width + 1;
   }

   /* Display input text */
   int display_len = g_input_len - display_start;
   if (display_len > visible_width) {
      display_len = visible_width;
   }
   if (display_len > 0) {
      mvprintw(start_y + 1, start_x + 4, "%.*s", display_len, g_input_buffer + display_start);
   }

   /* Draw cursor */
   int cursor_x = start_x + 4 + (g_input_cursor - display_start);
   attron(A_REVERSE);
   if (g_input_cursor < g_input_len) {
      mvaddch(start_y + 1, cursor_x, g_input_buffer[g_input_cursor]);
   } else {
      mvaddch(start_y + 1, cursor_x, ' ');
   }
   attroff(A_REVERSE);

   attron(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
   mvaddch(start_y + 1, start_x + panel_width - 1, '|');

   /* Bottom border */
   mvaddch(start_y + 2, start_x, '+');
   for (int i = 1; i < panel_width - 1; i++) {
      addch('-');
   }
   addch('+');

   attroff(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
}

static void draw_help_overlay(void) {
   int height = 18;
   int width = 50;
   int start_y = (g_term_rows - height) / 2;
   int start_x = (g_term_cols - width) / 2;

   /* Draw help box */
   attron(COLOR_PAIR(COLOR_PAIR_WARNING));
   for (int i = 0; i < height; i++) {
      mvhline(start_y + i, start_x, ' ', width);
   }

   mvprintw(start_y + 1, start_x + 2, "DAWN TUI Help");
   mvprintw(start_y + 2, start_x + 2, "---------------------------------------------");

   mvprintw(start_y + 4, start_x + 4, "Q/Esc    Quit DAWN");
   mvprintw(start_y + 5, start_x + 4, "I        Enter text input mode");
   mvprintw(start_y + 6, start_x + 4, "D        Toggle Debug Log mode");
   mvprintw(start_y + 7, start_x + 4, "R        Reset conversation (save & clear)");
   mvprintw(start_y + 8, start_x + 4, "L        Switch to Local LLM (session)");
   mvprintw(start_y + 9, start_x + 4, "C        Switch to Cloud LLM (session)");
   mvprintw(start_y + 10, start_x + 4, "1        Green (Apple ][) theme");
   mvprintw(start_y + 11, start_x + 4, "2        Blue (JARVIS) theme");
   mvprintw(start_y + 12, start_x + 4, "3        B/W (High Contrast) theme");
   mvprintw(start_y + 13, start_x + 4, "?        Toggle this help");

   mvprintw(start_y + 15, start_x + 2, "Press any key to close...");
   attroff(COLOR_PAIR(COLOR_PAIR_WARNING));
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

int tui_init(tui_theme_t theme) {
   /* Check if stdout is a TTY */
   if (!isatty(STDOUT_FILENO)) {
      LOG_WARNING("TUI: stdout is not a TTY, TUI disabled");
      return 1;
   }

   /* Redirect stderr FIRST to suppress library output during initialization */
   redirect_stderr_to_null();

   /* Set locale for Unicode box drawing */
   setlocale(LC_ALL, "");

   /* Initialize ncurses */
   if (initscr() == NULL) {
      restore_stderr(); /* Restore on failure */
      LOG_ERROR("TUI: Failed to initialize ncurses");
      return 1;
   }

   /* Get terminal size */
   getmaxyx(stdscr, g_term_rows, g_term_cols);

   /* Check minimum size */
   if (g_term_cols < TUI_MIN_COLS || g_term_rows < TUI_MIN_ROWS) {
      endwin();
      restore_stderr(); /* Restore on failure */
      LOG_WARNING("TUI: Terminal too small (%dx%d), need at least %dx%d", g_term_cols, g_term_rows,
                  TUI_MIN_COLS, TUI_MIN_ROWS);
      return 1;
   }

   /* Configure ncurses */
   cbreak();              /* Disable line buffering */
   noecho();              /* Don't echo input */
   keypad(stdscr, TRUE);  /* Enable special keys */
   nodelay(stdscr, TRUE); /* Non-blocking input */
   curs_set(0);           /* Hide cursor */

   /* Initialize colors */
   if (has_colors()) {
      start_color();
      use_default_colors();
      apply_theme(theme);
      g_current_theme = theme;
   }

   g_tui_initialized = 1;
   g_tui_mode = TUI_MODE_DISPLAY;

   /* Note: stderr already redirected at start of function */

   LOG_INFO("TUI initialized (%dx%d)", g_term_cols, g_term_rows);
   return 0;
}

void tui_cleanup(void) {
   if (!g_tui_initialized) {
      return;
   }

   /* Restore stderr before cleanup messages */
   restore_stderr();

   /* Restore terminal */
   curs_set(1);
   echo();
   nocbreak();
   endwin();

   g_tui_initialized = 0;
   g_tui_mode = TUI_MODE_OFF;
   LOG_INFO("TUI cleaned up");
}

int tui_is_active(void) {
   return g_tui_initialized && (g_tui_mode != TUI_MODE_OFF);
}

tui_mode_t tui_get_mode(void) {
   return g_tui_mode;
}

void tui_set_mode(tui_mode_t mode) {
   g_tui_mode = mode;
}

void tui_toggle_debug_mode(void) {
   if (g_tui_mode == TUI_MODE_DISPLAY) {
      g_tui_mode = TUI_MODE_DEBUG_LOG;
      /* Temporarily disable TUI display */
      endwin();
      /* Restore stderr for debug output */
      restore_stderr();
      /* Re-enable console logging for debug mode */
      logging_suppress_console(0);
      LOG_INFO("Switched to debug log mode (press D to return to TUI)");
   } else if (g_tui_mode == TUI_MODE_DEBUG_LOG) {
      /* Suppress console logging again */
      logging_suppress_console(1);
      /* Redirect stderr again */
      redirect_stderr_to_null();
      /* Re-enable TUI */
      refresh();
      g_tui_mode = TUI_MODE_DISPLAY;
   }
}

void tui_set_theme(tui_theme_t theme) {
   if (theme >= TUI_THEME_GREEN && theme <= TUI_THEME_BW) {
      g_current_theme = theme;
      apply_theme(theme);
   }
}

tui_theme_t tui_get_theme(void) {
   return g_current_theme;
}

void tui_update(void) {
   if (!g_tui_initialized || g_tui_mode != TUI_MODE_DISPLAY) {
      return;
   }

   /* Get current metrics snapshot */
   dawn_metrics_t metrics;
   metrics_get_snapshot(&metrics);

   /* Clear screen */
   erase();

   /* Calculate layout */
   int panel_width = g_term_cols - 2;
   int y = 0;

   /* Draw panels */
   draw_status_panel(y, 1, panel_width, &metrics);
   y += 5; /* Status panel: 5 lines (State, LLM/ASR/VAD, AEC) */

   draw_session_stats_panel(y, 1, panel_width, &metrics);
   y += 8;

   draw_performance_panel(y, 1, panel_width, &metrics);
   y += 10;

   draw_realtime_panel(y, 1, panel_width, &metrics);
   y += 5; /* Real-Time Audio panel: 5 lines (VAD, State, Heard) */

   /* Activity panel fills remaining space */
   int activity_height = g_term_rows - y - 2;
   if (activity_height < 4)
      activity_height = 4;
   draw_activity_panel(y, 1, panel_width, activity_height, &metrics);

   /* Footer */
   draw_footer(g_term_rows - 1, g_term_cols);

   /* Input panel if in input mode */
   if (g_input_mode) {
      draw_input_panel();
   }

   /* Help overlay if active */
   if (g_show_help) {
      draw_help_overlay();
   }

   /* Refresh display */
   refresh();
}

/**
 * @brief Handle input mode keyboard events
 * @param ch The character/key pressed
 * @return 1 if input was submitted, 0 otherwise
 */
static int handle_input_mode_key(int ch) {
   switch (ch) {
      case 27: /* ESC - cancel input */
         g_input_mode = 0;
         g_input_len = 0;
         g_input_cursor = 0;
         g_input_buffer[0] = '\0';
         break;

      case '\n':
      case '\r':
      case KEY_ENTER: /* Enter - submit input */
         if (g_input_len > 0) {
            /* Push to unified input queue */
            input_queue_push(INPUT_SOURCE_TUI, g_input_buffer);

            /* Log activity */
            metrics_log_activity("USER (text input): %s", g_input_buffer);
         }
         /* Clear input state */
         g_input_mode = 0;
         g_input_len = 0;
         g_input_cursor = 0;
         g_input_buffer[0] = '\0';
         break;

      case KEY_BACKSPACE:
      case 127:
      case 8: /* Backspace */
         if (g_input_cursor > 0) {
            /* Delete char before cursor */
            memmove(&g_input_buffer[g_input_cursor - 1], &g_input_buffer[g_input_cursor],
                    g_input_len - g_input_cursor + 1);
            g_input_cursor--;
            g_input_len--;
         }
         break;

      case KEY_DC: /* Delete key */
         if (g_input_cursor < g_input_len) {
            /* Delete char at cursor */
            memmove(&g_input_buffer[g_input_cursor], &g_input_buffer[g_input_cursor + 1],
                    g_input_len - g_input_cursor);
            g_input_len--;
         }
         break;

      case KEY_LEFT:
         if (g_input_cursor > 0) {
            g_input_cursor--;
         }
         break;

      case KEY_RIGHT:
         if (g_input_cursor < g_input_len) {
            g_input_cursor++;
         }
         break;

      case KEY_HOME:
         g_input_cursor = 0;
         break;

      case KEY_END:
         g_input_cursor = g_input_len;
         break;

      default:
         /* Printable character */
         if (isprint(ch) && g_input_len < TUI_INPUT_MAX_LEN) {
            /* Insert character at cursor */
            memmove(&g_input_buffer[g_input_cursor + 1], &g_input_buffer[g_input_cursor],
                    g_input_len - g_input_cursor + 1);
            g_input_buffer[g_input_cursor] = (char)ch;
            g_input_cursor++;
            g_input_len++;
         }
         break;
   }
   return 0;
}

int tui_handle_input(void) {
   if (!g_tui_initialized) {
      return 0;
   }

   int ch = getch();
   if (ch == ERR) {
      return 0; /* No input */
   }

   /* Handle resize in any mode */
   if (ch == KEY_RESIZE) {
      tui_handle_resize();
      return 0;
   }

   /* If in input mode, handle input keys */
   if (g_input_mode) {
      handle_input_mode_key(ch);
      return 0;
   }

   /* If help is showing, any key closes it */
   if (g_show_help) {
      g_show_help = 0;
      return 0;
   }

   /* Normal mode key handling */
   switch (ch) {
      case 'q':
      case 'Q':
      case 27:     /* ESC key */
         return 1; /* Request quit */

      case 'i':
      case 'I':
         /* Enter text input mode */
         g_input_mode = 1;
         g_input_len = 0;
         g_input_cursor = 0;
         g_input_buffer[0] = '\0';
         break;

      case 'd':
      case 'D':
         tui_toggle_debug_mode();
         break;

      case 'r':
      case 'R':
         reset_conversation();
         break;

      case '1':
         tui_set_theme(TUI_THEME_GREEN);
         break;

      case '2':
         tui_set_theme(TUI_THEME_BLUE);
         break;

      case '3':
         tui_set_theme(TUI_THEME_BW);
         break;

      case '?':
         g_show_help = !g_show_help;
         break;

      case 'l':
      case 'L': {
         /* Switch LOCAL session to Local LLM */
         session_t *local_session = session_get_local();
         session_llm_config_t config;
         session_get_llm_config(local_session, &config);
         if (config.type != LLM_LOCAL) {
            config.type = LLM_LOCAL;
            config.cloud_provider = CLOUD_PROVIDER_NONE;
            session_set_llm_config(local_session, &config);
            metrics_log_activity("Switched to Local LLM");
         }
         break;
      }

      case 'c':
      case 'C': {
         /* Switch LOCAL session to Cloud LLM */
         session_t *local_session = session_get_local();
         session_llm_config_t config;
         session_get_llm_config(local_session, &config);
         if (config.type != LLM_CLOUD) {
            config.type = LLM_CLOUD;
            if (session_set_llm_config(local_session, &config) == 0) {
               llm_resolved_config_t resolved;
               llm_resolve_config(&config, &resolved);
               metrics_log_activity("Switched to Cloud LLM (%s)",
                                    resolved.model ? resolved.model : "unknown");
            } else {
               metrics_log_activity("Failed to switch to Cloud LLM (no API key)");
            }
         }
         break;
      }

      default:
         break;
   }

   return 0;
}

void tui_show_help(void) {
   g_show_help = 1;
}

void tui_hide_help(void) {
   g_show_help = 0;
}

int tui_check_terminal_size(void) {
   getmaxyx(stdscr, g_term_rows, g_term_cols);
   return (g_term_cols >= TUI_MIN_COLS && g_term_rows >= TUI_MIN_ROWS);
}

void tui_handle_resize(void) {
   getmaxyx(stdscr, g_term_rows, g_term_cols);

   if (!tui_check_terminal_size()) {
      /* Terminal too small - switch to debug mode */
      LOG_WARNING("TUI: Terminal resized to %dx%d (too small), switching to debug mode",
                  g_term_cols, g_term_rows);
      tui_toggle_debug_mode();
   } else {
      /* Redraw with new size */
      clear();
      tui_update();
   }
}

int tui_has_text_input(void) {
   /* Delegate to unified input queue */
   return input_queue_has_item();
}

int tui_get_text_input(char *buffer) {
   queued_input_t input;

   if (buffer == NULL) {
      return 0;
   }

   /* Delegate to unified input queue */
   if (input_queue_pop(&input)) {
      strncpy(buffer, input.text, TUI_INPUT_MAX_LEN);
      buffer[TUI_INPUT_MAX_LEN] = '\0';
      return 1;
   }

   return 0;
}

int tui_is_input_mode(void) {
   return g_input_mode;
}
