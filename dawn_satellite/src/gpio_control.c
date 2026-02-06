/*
 * DAWN Satellite - GPIO Control (libgpiod)
 *
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
 */

#include "gpio_control.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_GPIOD
#include <gpiod.h>
#endif

#include "logging.h"

#ifdef HAVE_GPIOD

int gpio_init(gpio_control_t *ctx, const char *chip_path) {
   if (!ctx)
      return -1;

   memset(ctx, 0, sizeof(gpio_control_t));

   const char *path = chip_path ? chip_path : GPIO_DEFAULT_CHIP;

   /* Open GPIO chip */
   struct gpiod_chip *chip = gpiod_chip_open(path);
   if (!chip) {
      LOG_ERROR("Cannot open GPIO chip '%s'", path);
      return -1;
   }
   ctx->chip = chip;

   /* Request button input line with pull-up */
   struct gpiod_line *button = gpiod_chip_get_line(chip, GPIO_BUTTON_PIN);
   if (!button) {
      LOG_ERROR("Cannot get button line (GPIO %d)", GPIO_BUTTON_PIN);
      gpiod_chip_close(chip);
      return -1;
   }

   if (gpiod_line_request_input_flags(button, "dawn-satellite",
                                      GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP) < 0) {
      LOG_ERROR("Cannot request button input");
      gpiod_chip_close(chip);
      return -1;
   }
   ctx->button_line = button;

   /* Request LED output lines */
   struct gpiod_line *led_r = gpiod_chip_get_line(chip, GPIO_LED_RED_PIN);
   struct gpiod_line *led_g = gpiod_chip_get_line(chip, GPIO_LED_GREEN_PIN);
   struct gpiod_line *led_b = gpiod_chip_get_line(chip, GPIO_LED_BLUE_PIN);

   if (!led_r || !led_g || !led_b) {
      LOG_ERROR("Cannot get LED lines");
      gpiod_chip_close(chip);
      return -1;
   }

   if (gpiod_line_request_output(led_r, "dawn-satellite", 0) < 0 ||
       gpiod_line_request_output(led_g, "dawn-satellite", 0) < 0 ||
       gpiod_line_request_output(led_b, "dawn-satellite", 0) < 0) {
      LOG_ERROR("Cannot request LED outputs");
      gpiod_chip_close(chip);
      return -1;
   }

   ctx->led_red_line = led_r;
   ctx->led_green_line = led_g;
   ctx->led_blue_line = led_b;
   ctx->initialized = 1;
   ctx->led_state = LED_STATE_OFF;

   LOG_INFO("GPIO initialized: button=%d, LEDs=%d/%d/%d", GPIO_BUTTON_PIN, GPIO_LED_RED_PIN,
            GPIO_LED_GREEN_PIN, GPIO_LED_BLUE_PIN);

   return 0;
}

void gpio_cleanup(gpio_control_t *ctx) {
   if (ctx && ctx->chip) {
      gpio_led_off(ctx);
      gpiod_chip_close((struct gpiod_chip *)ctx->chip);
      ctx->chip = NULL;
      ctx->initialized = 0;
      LOG_INFO("GPIO cleaned up");
   }
}

int gpio_button_read(gpio_control_t *ctx) {
   if (!ctx || !ctx->initialized || !ctx->button_line) {
      return -1;
   }

   int value = gpiod_line_get_value((struct gpiod_line *)ctx->button_line);
   if (value < 0) {
      LOG_ERROR("Cannot read button value");
      return -1;
   }

   /* Button is active low (pulled high, pressed = low) */
   return (value == 0) ? 1 : 0;
}

int gpio_button_wait(gpio_control_t *ctx, int timeout_ms) {
   if (!ctx || !ctx->initialized || !ctx->button_line) {
      return -1;
   }

   struct gpiod_line *line = (struct gpiod_line *)ctx->button_line;

   /* Release and re-request for edge detection */
   gpiod_line_release(line);

   struct gpiod_line_request_config config = {
      .consumer = "dawn-satellite",
      .request_type = GPIOD_LINE_REQUEST_EVENT_FALLING_EDGE,
      .flags = GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP
   };

   if (gpiod_line_request(line, &config, 0) < 0) {
      LOG_ERROR("Cannot request button for edge detection");
      /* Re-request as input */
      gpiod_line_request_input_flags(line, "dawn-satellite", GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP);
      return -1;
   }

   struct timespec timeout;
   timeout.tv_sec = timeout_ms / 1000;
   timeout.tv_nsec = (timeout_ms % 1000) * 1000000;

   int ret = gpiod_line_event_wait(line, timeout_ms > 0 ? &timeout : NULL);

   /* Re-request as regular input */
   gpiod_line_release(line);
   gpiod_line_request_input_flags(line, "dawn-satellite", GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP);

   if (ret < 0) {
      LOG_ERROR("Button wait error");
      return -1;
   }

   return (ret > 0) ? 1 : 0;
}

void gpio_led_set_rgb(gpio_control_t *ctx, int red, int green, int blue) {
   if (!ctx || !ctx->initialized)
      return;

   if (ctx->led_red_line)
      gpiod_line_set_value((struct gpiod_line *)ctx->led_red_line, red ? 1 : 0);
   if (ctx->led_green_line)
      gpiod_line_set_value((struct gpiod_line *)ctx->led_green_line, green ? 1 : 0);
   if (ctx->led_blue_line)
      gpiod_line_set_value((struct gpiod_line *)ctx->led_blue_line, blue ? 1 : 0);
}

void gpio_led_off(gpio_control_t *ctx) {
   gpio_led_set_rgb(ctx, 0, 0, 0);
   if (ctx)
      ctx->led_state = LED_STATE_OFF;
}

void gpio_led_set_state(gpio_control_t *ctx, led_state_t state) {
   if (!ctx || !ctx->initialized)
      return;

   ctx->led_state = state;

   switch (state) {
      case LED_STATE_OFF:
         gpio_led_set_rgb(ctx, 0, 0, 0);
         break;
      case LED_STATE_IDLE:
         gpio_led_set_rgb(ctx, 0, 0, 1); /* Dim blue */
         break;
      case LED_STATE_RECORDING:
         gpio_led_set_rgb(ctx, 0, 0, 1); /* Blue */
         break;
      case LED_STATE_PROCESSING:
         gpio_led_set_rgb(ctx, 1, 1, 0); /* Yellow */
         break;
      case LED_STATE_PLAYING:
         gpio_led_set_rgb(ctx, 0, 1, 0); /* Green */
         break;
      case LED_STATE_ERROR:
         gpio_led_set_rgb(ctx, 1, 0, 0); /* Red */
         break;
   }
}

#else /* !HAVE_GPIOD */

/* Stub implementations when libgpiod is not available */

int gpio_init(gpio_control_t *ctx, const char *chip_path) {
   (void)chip_path;
   if (ctx) {
      memset(ctx, 0, sizeof(gpio_control_t));
      ctx->initialized = 0;
   }
   LOG_INFO("GPIO disabled (libgpiod not available)");
   return 0; /* Not an error, just disabled */
}

void gpio_cleanup(gpio_control_t *ctx) {
   if (ctx)
      ctx->initialized = 0;
}

int gpio_button_read(gpio_control_t *ctx) {
   (void)ctx;
   return 0;
}

int gpio_button_wait(gpio_control_t *ctx, int timeout_ms) {
   (void)ctx;
   /* Just sleep and return timeout */
   if (timeout_ms > 0) {
      usleep(timeout_ms * 1000);
   }
   return 0;
}

void gpio_led_set_state(gpio_control_t *ctx, led_state_t state) {
   (void)ctx;
   (void)state;
}

void gpio_led_set_rgb(gpio_control_t *ctx, int red, int green, int blue) {
   (void)ctx;
   (void)red;
   (void)green;
   (void)blue;
}

void gpio_led_off(gpio_control_t *ctx) {
   (void)ctx;
}

#endif /* HAVE_GPIOD */
