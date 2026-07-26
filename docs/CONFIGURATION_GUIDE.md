# Adding a Configuration Setting

How to wire a new `dawn.toml` setting so it survives a WebUI settings save.

**Read this before adding any setting.** Wiring a setting touches up to nine places — a whole new section,
ten. Miss one and nothing fails to build, no test goes red, and the daemon starts fine; the damage shows up
later as a user's settings quietly reverting to defaults, or as a panel that simply is not there. Some
variant of that has now shipped **four times** (see [Why this guide exists](#why-this-guide-exists)).

---

## Table of Contents

- [Why this guide exists](#why-this-guide-exists)
- [Which mechanism do I want?](#which-mechanism-do-i-want)
- [The checklist](#the-checklist)
- [Worked example](#worked-example)
- [Adding a whole new section](#adding-a-whole-new-section)
- [When NOT to surface a setting in the WebUI](#when-not-to-surface-a-setting-in-the-webui)
- [Verifying your wiring](#verifying-your-wiring)

---

## Why this guide exists

A WebUI settings save does **not** patch `dawn.toml`. It rewrites the **entire file** from the in-memory
config struct:

```
browser "Save"  →  webui_config.c (POST handler)  →  config_write_toml()  →  dawn.toml (truncated + rewritten)
```

That has one brutal consequence:

> **Any section `config_parser.c` reads but `config_write_toml()` does not emit is silently DELETED from the
> user's `dawn.toml` the first time they save any setting — including settings in a completely unrelated
> panel.** On the next restart those settings come back as defaults.

Nothing warns you. The writer has no idea a section exists; it only knows what it was told to print. And
the client has a mirror-image gate of its own — `SECTION_CATEGORIES` (see step 6 of
[Adding a whole new section](#adding-a-whole-new-section)) — with exactly the same silence. The shipped
instances:

| What was lost | How it happened | Fixed |
|---|---|---|
| All four messaging bot tokens + `service_token` | `secrets_write_toml()` had no write lines for them | Messaging Phase 6.5 |
| Entire `[jobs]` section | Wired into the struct/defaults/parser and the WebUI *client* schema, but zero server-side support | 2026-07-24 |
| Entire `[scheduler]` section | Fully wired everywhere **except** the writer — edits applied at runtime, then vanished on restart | 2026-07-24 |
| `[attention]` + `[jobs]` panels | Wired through **every** step above, but neither section was listed in `SECTION_CATEGORIES` — the client-side render gate — so both panels simply never appeared | 2026-07-26 |

The `[jobs]` case is the instructive one: whoever wired it followed `ARCHITECTURE.md`, which said *"also add
corresponding entries to `SETTINGS_SCHEMA` to expose them in the WebUI."* That sentence names the **client**
touchpoint and none of the three server-side ones. Following the documentation produced the bug — which is
why this guide now exists and why that line points here.

---

## Which mechanism do I want?

DAWN has **two** configuration mechanisms. Only one of them has this hazard.

### Tool-owned config — use this for anything tool-specific

A tool in `src/tools/` declares `config_section` + `config_parser` + `config_writer` in its
`tool_metadata_t`, and `tool_registry_write_configs()` writes it back **automatically**. There is no
round-trip hazard: registering the tool wires the writer.

Examples: `[shutdown]`, `[home_assistant]`. See [TOOL_DEVELOPMENT_GUIDE.md](TOOL_DEVELOPMENT_GUIDE.md).

**Prefer this** when the setting belongs to one tool. It is less work and structurally safer.

### Global config — for daemon-wide subsystems

Fields on `dawn_config_t`, parsed in `src/config/config_parser.c`. Examples: `[llm]`, `[memory]`,
`[webui]`, `[jobs]`. **This is the one with the hazard** — you must wire the writer yourself. The rest of
this guide is about this path.

---

## The checklist

For a scalar setting in an **existing** section. Traced from a known-good setting
(`attention.max_alerts_per_hour`), so this list is complete as of schema v73.

| # | File | What to add | Miss it and… |
|---|------|-------------|--------------|
| 1 | `include/config/dawn_config.h` | Field on the section's `*_config_t` struct | won't compile |
| 2 | `src/config/config_defaults.c` | Default value in `config_set_defaults()` | field is garbage/0 |
| 3 | `src/config/config_parser.c` | Key in `known_keys[]`, a `PARSE_*` call, and any clamp | **setting in dawn.toml is ignored**, and the key warns as unknown |
| 4 | `src/config/config_env.c` → `config_to_json()` | `json_object_object_add(...)` | **panel shows the default, not the live value** |
| 5 | `src/config/config_env.c` → `config_write_toml()` | `fprintf(fp, ...)` | 🔴 **SILENT DATA LOSS — the value is erased from dawn.toml on any settings save** |
| 6 | `src/webui/webui_config.c` | `JSON_TO_CONFIG_*` in the section's block | **user's edit is silently ignored** |
| 7 | `www/js/ui/settings/schema.js` | Field in `SETTINGS_SCHEMA` | no UI control (may be intentional — see [exclusions](#when-not-to-surface-a-setting-in-the-webui)) |
| 8 | `dawn.toml.example` | Commented entry + explanation | undiscoverable for file-editing users |
| 9 | `src/config/config_validate.c` | Range/consistency rule | *optional* — only if a bad value is worse than a clamped one |

**#5 is the one that bites.** It is the only step whose omission destroys data the user already had, and the
only one with no symptom at the time you make the mistake.

> **Rule of thumb: #4, #5 and #6 always move together.** JSON out, TOML out, JSON in. If you touch one,
> touch all three. #5 must cover **every** field in the struct — including fields not yet exposed in the UI
> and not yet enforced by any code, because the writer emits from the in-memory config, so writing them is
> what preserves a hand-edited value across a save.

### Macros you'll use

| Step | Macros / helpers |
|---|---|
| 3 parse | `PARSE_STRING` · `PARSE_INT` · `PARSE_DOUBLE` · `PARSE_BOOL` · `PARSE_SIZE_T` |
| 5 write | numbers/bools: `fprintf(fp, "key = %d\n", ...)` — **strings: `write_toml_string()`**, or `write_toml_string_multiline()` for free text |
| 6 POST | `JSON_TO_CONFIG_STR` · `_INT` · `_BOOL` · `_DOUBLE` · `_SIZE_T` |

> ⚠ **Never `fprintf` a string value raw.** `fprintf(fp, "key = \"%s\"\n", value)` looks harmless and
> was the dominant pattern in this file, but it lets a value containing a quote or newline **forge TOML
> keys** — an admin could inject settings the POST handler deliberately refuses to accept — and an
> unescaped control character produces a `dawn.toml` the parser rejects on the next boot, wedging the
> config. `write_toml_string()` escapes quotes, backslashes, newlines and C0 controls;
> `write_toml_string_multiline()` additionally handles the `'''` literal-string breakout. Every string
> in `config_write_toml()` goes through one of the two.

---

## Worked example

Adding `max_widgets` to the existing `[jobs]` section.

**1. Struct** — `include/config/dawn_config.h`
```c
typedef struct {
   bool enabled;
   int max_widgets;   /* Widgets a job may allocate; 0 = unlimited */
} jobs_config_t;
```

**2. Default** — `src/config/config_defaults.c`
```c
config->jobs.max_widgets = 8;
```

**3. Parse** — `src/config/config_parser.c`, in `parse_jobs()`
```c
static const char *const known_keys[] = { "enabled", "max_widgets", /* … */ NULL };
...
PARSE_INT(table, "max_widgets", config->max_widgets);
```
Clamp in the section's shared clamp helper (e.g. `config_clamp_jobs()`) — **not** inline — so the WebUI POST
path gets the same bounds as the file path:
```c
if (config->max_widgets < 0)
   config->max_widgets = 0;
```

**4. JSON out** — `src/config/config_env.c`, `config_to_json()`
```c
json_object_object_add(jobs, "max_widgets", json_object_new_int(config->jobs.max_widgets));
```

**5. TOML out** — `src/config/config_env.c`, `config_write_toml()`
```c
fprintf(fp, "max_widgets = %d\n", config->jobs.max_widgets);
```

**6. POST in** — `src/webui/webui_config.c`, in the `"jobs"` block
```c
JSON_TO_CONFIG_INT(section, "max_widgets", config->jobs.max_widgets);
config_clamp_jobs(&config->jobs);   /* same bounds as the file path */
```

**7. UI** — `www/js/ui/settings/schema.js`, under `jobs.fields`
```js
max_widgets: {
   type: 'number', label: 'Max Widgets Per Job',
   min: 0, max: 256, default: 8, advanced: true,
   hint: 'Widgets a single job may allocate (0 = unlimited)',
},
```

**8. Document** — `dawn.toml.example`
```toml
# max_widgets = 8                    # Widgets a job may allocate. 0 = unlimited.
```

**9. Validate** *(optional)* — `src/config/config_validate.c`, only if an out-of-range value should be an
error rather than silently clamped.

---

## Adding a whole new section

Everything above, plus:

1. **Struct + member** — new `*_config_t` in `dawn_config.h` *and* a member on `dawn_config_t`.
2. **Parser dispatch** — `parse_mysection(toml_table_in(root, "mysection"), &config->mysection);` in
   `config_parse_file()`.
3. **Section header in the writer** — `fprintf(fp, "\n[mysection]\n");` before the fields.
4. **Section object in `config_to_json()`** — build the object, then
   `json_object_object_add(root, "mysection", mysection);`.
5. **POST block** — `if (json_object_object_get_ex(payload, "mysection", &section)) { … }`.
6. 🔴 **Assign the section to a category** — add its name to a `sections: [...]` array in
   `SECTION_CATEGORIES`, at the bottom of `www/js/ui/settings/schema.js`. **`SETTINGS_SCHEMA` alone is not
   enough.** `SECTION_CATEGORIES` is the render gate, and its own comment says it: *"Sections not listed
   here will not be rendered."* Miss this and your section is wired through every step above, parses,
   round-trips, saves — and is **completely invisible in the UI**, with nothing failing and no warning.
7. **Add the section to both CI guards** — the `required[]` list in `tests/test_config_roundtrip.c` (server
   side: proves the writer emits it) and, for the client side, `tests/check_settings_sections_rendered.sh`
   needs no edit but *will* fail by name if you skipped step 6. Together these make the next person's
   mistake fail loudly instead of silently.
8. **ARCHITECTURE.md** — add a row to the *WebUI Settings Panel Mapping* table.

> **Step 6 has already bitten twice.** `[attention]` (SAGE watches) and `[jobs]` (background jobs) were both
> fully wired end to end and both unreachable in the panel, found only when someone went looking for a
> settings page that should have existed. It is the client-side twin of the `config_write_toml` omission
> this guide opens with: same silence, same "everything is green", same user-visible result of *the setting
> isn't there*. `check_settings_sections_rendered.sh` now guards it.

**Conditional sections** (emitted only when they hold content — `[persona]`, `[url_fetcher]`) are fine and
safe *provided the emit condition derives from the parsed content*, so anything the user actually set is
written back. If you add one, give it a triggering value in the round-trip test so the guard stays strict.

---

## When NOT to surface a setting in the WebUI

`ARCHITECTURE.md` sets the default: every `dawn.toml` setting appears in the settings panel unless it is a
**filesystem path** (security), an **internal debug flag**, or **restart-only with no runtime effect**.

One more exclusion learned from `[jobs]`: **a knob that is parsed but not yet enforced should not appear in
the UI.** A control that silently does nothing is worse than no control. Keep it out of `schema.js`, but
still wire steps 4/5/6 so a hand-edited value round-trips, and label it clearly in `dawn.toml.example`:

```toml
# Parsed and round-tripped, but NOT yet enforced — reserved for Phase 3.
# max_spawn_depth = 3                # Phase 3: job-tree depth cap.
```

---

## Verifying your wiring

**0. Run the render-gate guard** (new sections only, but it is instant):

```bash
bash tests/check_settings_sections_rendered.sh
```

It fails by name if a `SETTINGS_SCHEMA` section is in no `SECTION_CATEGORIES` category — i.e. wired
correctly everywhere and invisible anyway.

**1. Run the round-trip guard.** `tests/test_config_roundtrip.c` writes the config, re-parses it, and checks
values survive — plus a section-coverage check that fails *by name* if a writer-owned section stops being
emitted:

```bash
make -C build-debug test_config_roundtrip && ./build-debug/tests/test_config_roundtrip
```

Add value assertions for your new setting, and add your section to `required[]` if it's new.

> Note: plain `make` does not rebuild test binaries. Use `make -C build-debug tests-ci` before trusting
> `ctest`, or you will run a stale test.

**2. Round-trip it by hand.** The end-to-end check the unit test can't do:

1. Set a **non-default** value in `dawn.toml`, start the daemon.
2. Confirm the WebUI settings panel shows **your value**, not the default → proves step 4.
3. Change it in the panel and save → proves step 6.
4. `grep` your key in `dawn.toml` → proves step 5.
5. Open **any other** settings panel and save → `grep` your section again. **Still there?** Then the writer
   really covers it. This is the step that catches the silent-deletion bug.
6. Restart and confirm the value persisted.

---

## References

- [ARCHITECTURE.md § Configuration Architecture](../ARCHITECTURE.md#configuration-architecture) — design
  principles, file hierarchy, panel mapping table
- [TOOL_DEVELOPMENT_GUIDE.md](TOOL_DEVELOPMENT_GUIDE.md) — the tool-owned config path
- `tests/test_config_roundtrip.c` — the CI guard
