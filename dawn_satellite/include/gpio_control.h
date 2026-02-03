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

#ifndef GPIO_CONTROL_H
#define GPIO_CONTROL_H

#include <stdint.h>

/* Default GPIO chip for Pi Zero 2 */
#define GPIO_DEFAULT_CHIP "/dev/gpiochip0"

/* Default GPIO pin assignments (BCM numbering) */
#define GPIO_BUTTON_PIN 17    /* Push button for PTT */
#define GPIO_LED_RED_PIN 22   /* Red status LED */
#define GPIO_LED_GREEN_PIN 23 /* Green status LED */
#define GPIO_LED_BLUE_PIN 24  /* Blue status LED */

/* LED states */
typedef enum {
   LED_STATE_OFF,
   LED_STATE_IDLE,       /* Slow breathing / pulse */
   LED_STATE_RECORDING,  /* Blue solid */
   LED_STATE_PROCESSING, /* Yellow pulsing */
   LED_STATE_PLAYING,    /* Green solid */
   LED_STATE_ERROR       /* Red solid */
} led_state_t;

/**
 * GPIO control context
 */
typedef struct {
   void *chip;            /* gpiod_chip pointer */
   void *button_line;     /* Button input line */
   void *led_red_line;    /* Red LED output line */
   void *led_green_line;  /* Green LED output line */
   void *led_blue_line;   /* Blue LED output line */
   int initialized;       /* Initialization state */
   led_state_t led_state; /* Current LED state */
} gpio_control_t;

/**
 * Initialize GPIO control
 *
 * @param ctx Pointer to GPIO context
 * @param chip_path GPIO chip path (NULL for default)
 * @return 0 on success, -1 on error
 */
int gpio_init(gpio_control_t *ctx, const char *chip_path);

/**
 * Clean up GPIO control
 *
 * @param ctx Pointer to GPIO context
 */
void gpio_cleanup(gpio_control_t *ctx);

/**
 * Read button state
 *
 * @param ctx Pointer to GPIO context
 * @return 1 if pressed, 0 if not pressed, -1 on error
 */
int gpio_button_read(gpio_control_t *ctx);

/**
 * Wait for button press with timeout
 *
 * @param ctx Pointer to GPIO context
 * @param timeout_ms Timeout in milliseconds (0 = no timeout)
 * @return 1 if button pressed, 0 if timeout, -1 on error
 */
int gpio_button_wait(gpio_control_t *ctx, int timeout_ms);

/**
 * Set LED state
 *
 * @param ctx Pointer to GPIO context
 * @param state LED state to set
 */
void gpio_led_set_state(gpio_control_t *ctx, led_state_t state);

/**
 * Set individual LED colors (for custom patterns)
 *
 * @param ctx Pointer to GPIO context
 * @param red Red LED state (0 = off, 1 = on)
 * @param green Green LED state (0 = off, 1 = on)
 * @param blue Blue LED state (0 = off, 1 = on)
 */
void gpio_led_set_rgb(gpio_control_t *ctx, int red, int green, int blue);

/**
 * Turn off all LEDs
 *
 * @param ctx Pointer to GPIO context
 */
void gpio_led_off(gpio_control_t *ctx);

#endif /* GPIO_CONTROL_H */
