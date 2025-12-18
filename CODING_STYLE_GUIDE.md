# C/C++ Coding Style Guide
## Project Code Style Standards

### Philosophy
This style guide balances professional best practices with hacker pragmatism. Code should be readable, maintainable, and elegant—but never at the expense of progress. When in doubt, favor clarity and function over formalism.

---

## 1. Indentation & Spacing

### Indentation
- **Use 3 spaces** for indentation (no tabs)
- Rationale: Provides good visual hierarchy without excessive horizontal space

```c
void example_function(void) {
   if (condition) {
      do_something();
      if (nested_condition) {
         do_nested_thing();
      }
   }
}
```

### Spacing Around Operators
- Space around binary operators: `a + b`, `x == y`, `ptr->field`
- No space for unary operators: `!flag`, `*ptr`, `&variable`
- Space after commas: `function(a, b, c)`
- Space after keywords: `if (`, `for (`, `while (`
- Two spaces before trailing comments

```c
int result = (a + b) * c;
if (x == 0 && y != NULL) {
   *ptr = &data;
}
```

### Pointer and Reference Alignment
- **Pointers and references align right** (to the variable name)
- Format: `int *ptr`, `const char *str`, `MyClass &ref`
- Enforced by clang-format

```c
int *pointer;
const char *string;
float *array[10];
void process_data(const uint8_t *buffer, size_t *length);
```

### Line Length
- **100 characters maximum** (enforced by clang-format)
- Break long lines at logical points
- Function calls/parameters are automatically wrapped by the formatter

---

## 2. Braces & Control Structures

### Brace Style (K&R Style)
- Opening brace on same line for functions, conditionals, loops
- Closing brace on own line
- **Always use braces for single-statement blocks** (enforced by clang-format)
- **No single-line control structures** (e.g., no `if (x) do_thing();`)

```c
int my_function(int param) {
   if (condition) {
      single_statement();
   } else {
      other_statement();
   }

   for (int i = 0; i < count; i++) {
      process(i);
   }

   while (running) {
      update();
   }
}

/* NOT ALLOWED - clang-format will expand these */
// if (x) return;                    // BAD
// for (int i = 0; i < 10; i++) x++; // BAD
```

### Switch Statements
- Indent case labels one level
- Always include `break` or `/* fall through */` comment

```c
switch (value) {
   case OPTION_A:
      handle_a();
      break;
   
   case OPTION_B:
      handle_b();
      /* fall through */
   
   case OPTION_C:
      handle_c();
      break;
   
   default:
      handle_default();
      break;
}
```

---

## 3. Naming Conventions

### Functions
- Use `snake_case`
- Descriptive names: `calculate_battery_voltage()` not `calc()`
- Static functions: prefix with module name or keep private

```c
void init_logging(void);
static void parse_config_line(char *line);
int get_battery_level(battery_t *battery);
```

### Variables
- Use `snake_case`
- Descriptive names for global/function scope
- Short names OK for tight loops: `i`, `j`, `k`

```c
int active_device_count;
float battery_voltage;

for (int i = 0; i < MAX_ITEMS; i++) {
   process_item(i);
}
```

### Constants & Macros
- Use `UPPER_CASE_WITH_UNDERSCORES`
- Prefer enums over `#define` for related constants

```c
#define MAX_BUFFER_SIZE 1024
#define LOG_TO_CONSOLE  0
#define LOG_TO_FILE     1

typedef enum {
   STATE_IDLE,
   STATE_ACTIVE,
   STATE_ERROR
} state_t;
```

### Types
- Typedef structs with `_t` suffix or descriptive name
- Use `snake_case` for type names

```c
typedef struct battery_config_t {
   int capacity_mah;
   float voltage_nominal;
} battery_config_t;

typedef enum {
   LOG_INFO,
   LOG_WARNING,
   LOG_ERROR
} log_level_t;
```

---

## 4. Comments & Documentation

### File Headers
- Include GPL license block (as in existing code)
- Brief file description after license

```c
/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * ...
 */

/**
 * @file battery_monitor.c
 * @brief Battery monitoring and telemetry implementation
 */
```

### Function Documentation
- Use Doxygen-style comments for public APIs
- Minimal comments for obvious private functions

```c
/**
 * @brief Initializes the battery monitoring system
 * 
 * Sets up I2C communication and calibrates sensors. Must be called
 * before any other battery monitoring functions.
 * 
 * @param config Pointer to battery configuration structure
 * @return 0 on success, positive error code on failure
 */
int init_battery_monitor(battery_config_t *config);
```

### Inline Comments
- Use `/* */` for multi-line comments
- Use `//` for single-line comments
- Comment the "why" not the "what"

```c
/* Initialize hardware before entering main loop */
init_i2c();

// HACK: Delay required for sensor stabilization
usleep(100000);

/* Check if voltage is within safe operating range */
if (voltage < MIN_SAFE_VOLTAGE) {
   trigger_alarm();
}
```

### TODO Comments
```c
// TODO: Add temperature compensation
// FIXME: Race condition in thread synchronization
// HACK: Workaround for hardware bug in revision A
// NOTE: This assumes little-endian architecture
```

---

## 5. Header Files

### Header Guards
- Use `#ifndef/#define/#endif` pattern
- Name: `<FILENAME>_H` in uppercase

```c
#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

/* Header content */

#endif /* BATTERY_MONITOR_H */
```

### Include Order
- **Automatically sorted by clang-format** with the following priority:
  1. System C headers (`<stdio.h>`, `<stdlib.h>`)
  2. System C++ and other headers (`<iostream>`, etc.)
  3. Local project headers (`"battery.h"`)
- Blank lines separate groups
- Headers alphabetically sorted within each group

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mosquitto.h>
#include <json-c/json.h>

#include "battery.h"
#include "logging.h"
```

### C++ Compatibility
```c
#ifdef __cplusplus
extern "C" {
#endif

/* C declarations */

#ifdef __cplusplus
}
#endif
```

---

## 6. Function Design

### Function Length
- Soft target: < 50 lines
- **No hard limits** - some complex functions may be longer (100+ lines is acceptable)
- Use your judgment: clarity and maintainability over arbitrary line counts
- Extract helpers when it improves readability, not just to meet a limit

### Parameter Order
- Inputs first, outputs last
- Context/state parameters first

```c
int read_sensor(i2c_bus_t *bus, uint8_t address, float *result);
```

### Return Values
- **Use `SUCCESS` (0) and `FAILURE` (1) macros** for return values
- Define specific error codes > 1 for detailed error reporting
- **DO NOT use negative return values** (no -1, no negative errno)
- Return 0 on success, positive values on error
- For count/size functions, use separate output parameter for errors

```c
#define SUCCESS 0
#define FAILURE 1

/* Simple success/failure */
int init_device(void) {
   if (!hardware_present()) {
      LOG_ERROR("Hardware not present");
      return FAILURE;
   }
   return SUCCESS;
}

/* Module-specific error codes */
#define ERR_INVALID_PARAM  2
#define ERR_TIMEOUT        3
#define ERR_NO_MEMORY      4
#define ERR_IO_FAILED      5

int configure_sensor(const sensor_config_t *config) {
   if (!config) {
      return ERR_INVALID_PARAM;
   }

   if (!allocate_buffers()) {
      return ERR_NO_MEMORY;
   }

   if (!wait_for_ready(1000)) {
      return ERR_TIMEOUT;
   }

   return SUCCESS;
}

/* For functions that return counts, use output parameter for status */
int read_data(uint8_t *buffer, size_t size, size_t *bytes_read) {
   if (!buffer || !bytes_read) {
      return ERR_INVALID_PARAM;
   }

   *bytes_read = do_read(buffer, size);

   if (*bytes_read == 0) {
      return ERR_IO_FAILED;
   }

   return SUCCESS;
}
```

---

## 7. Error Handling

### Check Return Values
- Always check return values from functions that can fail
- Log errors appropriately
- Check against SUCCESS (0) or specific error codes

```c
int ret = init_sensor();
if (ret != SUCCESS) {
   LOG_ERROR("Failed to initialize sensor: error code %d", ret);
   return FAILURE;
}

/* Check for specific errors */
ret = configure_device(&config);
if (ret == ERR_TIMEOUT) {
   LOG_WARNING("Device configuration timeout, retrying...");
   ret = configure_device(&config);
}
if (ret != SUCCESS) {
   LOG_ERROR("Device configuration failed: %d", ret);
   return ret;  /* Propagate specific error code */
}
```

### Cleanup and Resource Management
- **Prefer direct cleanup calls** over goto
- Use goto sparingly for complex cleanup scenarios
- When using goto: use descriptive labels and cleanup in reverse order

```c
/* Preferred: direct cleanup */
int simple_function(void) {
   void *buffer = malloc(SIZE);
   if (!buffer) {
      LOG_ERROR("Memory allocation failed");
      return FAILURE;
   }

   int result = process_buffer(buffer);
   free(buffer);

   return (result == 0) ? SUCCESS : FAILURE;
}

/* Acceptable for complex cleanup scenarios */
int complex_function(void) {
   int ret = SUCCESS;
   void *buffer = NULL;
   FILE *file = NULL;

   buffer = malloc(SIZE);
   if (!buffer) {
      ret = FAILURE;
      goto cleanup;
   }

   file = fopen("data.txt", "r");
   if (!file) {
      ret = FAILURE;
      goto cleanup_buffer;
   }

   /* Process data */

cleanup_buffer:
   free(buffer);
cleanup:
   if (file) {
      fclose(file);
   }
   return ret;
}
```

---

## 8. Memory Management

### Memory Allocation Strategy
- **Prefer static allocation** for embedded systems
- Use stack buffers with fixed sizes when possible
- Minimize dynamic allocation (malloc/calloc)
- When dynamic allocation is needed, always check for NULL and clean up properly

### Allocation Patterns
```c
/* Preferred: static allocation */
char buffer[MAX_SIZE];
memset(buffer, 0, sizeof(buffer));

/* When dynamic allocation is necessary */
char *buffer = malloc(size);
if (!buffer) {
   LOG_ERROR("Memory allocation failed");
   return FAILURE;
}

/* Initialize after allocation */
memset(buffer, 0, size);

/* Free and NULL */
free(buffer);
buffer = NULL;
```

### String Safety
```c
/* Use bounded string functions */
strncpy(dest, src, sizeof(dest) - 1);
dest[sizeof(dest) - 1] = '\0';

snprintf(buffer, sizeof(buffer), "Value: %d", value);
```

---

## 9. Best Practices (Hacker-Approved)

### Performance
- Measure before optimizing
- Prefer clarity over micro-optimizations
- Comment when doing something clever

```c
/* Fast integer square root using bit manipulation */
unsigned int isqrt(unsigned int n)
{
   /* Implementation */
}
```

### Portability
- Avoid compiler-specific extensions (unless necessary)
- Test on target platform

### Threading
- Document thread safety
- Use locks consistently
- Avoid globals when possible

```c
/* Thread-safe: protected by config_mutex */
static config_t global_config;
static pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;
```

### Thread Argument Allocation Pattern

When passing arguments to `pthread_create()`, follow these rules:

1. **Arguments MUST be heap-allocated (malloc) or NULL** - NEVER pass pointers to stack-allocated structures
2. **Thread function takes ownership** - The callee (thread function) is responsible for freeing the arguments when done
3. **Free on error** - If `pthread_create()` fails after allocation, the caller must free the arguments

**Valid patterns:**

```c
/* Pattern 1: Heap-allocated with ownership transfer */
typedef struct {
   const char *sink_name;
   const char *file_name;
} PlaybackArgs;

void *playback_thread(void *arg) {
   PlaybackArgs *args = (PlaybackArgs *)arg;

   /* Use the arguments */
   play_audio(args->sink_name, args->file_name);

   /* Thread owns args, must free when done */
   free(args);
   return NULL;
}

int start_playback(const char *sink, const char *file) {
   PlaybackArgs *args = malloc(sizeof(PlaybackArgs));
   if (!args) {
      return FAILURE;
   }

   args->sink_name = sink;
   args->file_name = file;

   pthread_t thread;
   if (pthread_create(&thread, NULL, playback_thread, args) != 0) {
      free(args);  /* Free on error - thread never started */
      return FAILURE;
   }

   return SUCCESS;
}

/* Pattern 2: NULL argument with shared state */
void *worker_thread(void *arg) {
   (void)arg;  /* Unused - data accessed via globals/shared state */

   pthread_mutex_lock(&work_mutex);
   /* Access shared data */
   pthread_mutex_unlock(&work_mutex);

   return NULL;
}
```

**Anti-pattern (FORBIDDEN):**

```c
/* BAD: Stack-allocated argument - use-after-free bug! */
int start_playback_WRONG(void) {
   PlaybackArgs args;  /* Stack allocated - WRONG! */
   args.sink_name = "default";
   args.file_name = "music.flac";

   pthread_t thread;
   /* args goes out of scope before thread reads it! */
   pthread_create(&thread, NULL, playback_thread, &args);

   return SUCCESS;  /* args is now invalid! */
}
```

---

## 10. Code Organization

### File Structure
```c
/*
 * License header
 */

/**
 * @file description
 */

/* System includes */
#include <stdio.h>

/* Library includes */
#include <libfoo.h>

/* Local includes */
#include "module.h"

/* Constants and macros */
#define MAX_SIZE 1024

/* Type definitions */
typedef struct { ... } foo_t;

/* Static/private function declarations */
static void helper_function(void);

/* Global variables (minimize!) */
static int module_state;

/* Public functions */
int public_api(void) { ... }

/* Private functions */
static void helper_function(void) { ... }
```

### Module Organization
- One .h file per .c file
- Keep related functionality together
- Minimize coupling between modules

---

## 11. Anti-Patterns to Avoid

### Don't
- ❌ Magic numbers: use constants
- ❌ Deeply nested logic: refactor
- ❌ Inconsistent error handling
- ❌ Memory leaks: always match malloc/free
- ❌ Ignoring return values

### Do
- ✅ Fail fast and loudly
- ✅ Log at appropriate levels
- ✅ Write testable code
- ✅ Document assumptions
- ✅ Keep it simple

---

## 12. Project-Specific Conventions

### Logging
```c
#include "logging.h"

LOG_INFO("System initialized");
LOG_WARNING("Battery voltage low: %.2fV", voltage);
LOG_ERROR("I2C communication failed: %d", error);
```

### Configuration
- Prefer runtime config over compile-time
- Validate all config values
- Provide sensible defaults

### MQTT Integration
```c
/* Publish state changes */
mqtt_publish("device/status", "{\"state\":\"active\"}");
```

---

## Appendix: Automated Formatting with clang-format

### What clang-format Enforces

The `.clang-format` configuration automatically handles:

- **Indentation**: 3 spaces, no tabs
- **Line length**: 100 character limit with automatic wrapping
- **Brace style**: K&R style (opening brace on same line)
- **Pointer alignment**: Right-aligned (`int *ptr`)
- **Include sorting**: Automatic grouping and alphabetizing
- **Spacing**: Consistent spacing around operators, keywords, and comments
- **No single-line blocks**: All control structures use braces on separate lines

### What clang-format Does NOT Enforce

The formatter cannot enforce:
- Function length limits
- Variable naming conventions (snake_case vs camelCase)
- Constant naming (UPPER_CASE)
- Comment quality and content
- Error handling patterns
- Memory management practices
- Code organization and structure

These require manual code review.

### Using the Formatter

```bash
# Format all code before committing (required)
./format_code.sh

# Preview changes without modifying files
./format_code.sh --dry-run

# Check if files need formatting (for CI)
./format_code.sh --check
```

### Configuration Details

See `.clang-format` for the complete configuration. Key settings:
- `BasedOnStyle: LLVM` - Foundation style
- `IndentWidth: 3` - 3-space indentation
- `ColumnLimit: 100` - Maximum line length
- `PointerAlignment: Right` - Pointer alignment style
- `BreakBeforeBraces: Custom` - K&R brace style
- `SortIncludes: true` - Automatic include sorting

---

**Philosophy**: This is a living document. When the style guide gets in the way of getting shit done, update the style guide—not the code. Consistency within a module matters more than consistency with the guide.

