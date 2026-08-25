# Visual rendering design system

## Quoting (your code is embedded in JSON)
The `code` you send is carried inside a JSON string, so every double-quote in it
must be escaped — a miscount produces "invalid JSON" and the render fails. In
HTML-type visuals, prefer SINGLE quotes for HTML attributes and JS string
literals so there is nothing to escape: `<canvas id='c'></canvas>`,
`<script src='/js/vendor/chart.umd.js'></script>`, `const labels=['a','b']`.
(Single- and double-quoted `/js/vendor/*` script tags both inline correctly.)

## SVG Setup
- ViewBox: `<svg width="100%" viewBox="0 0 680 H">` — 680px wide, H computed to fit content.
- Safe area: x=40 to x=640, y=40 to y=(H-40).
- Background: transparent. Do not add background colors to the SVG or wrapper divs.
- One SVG per tool call.

## Typography
- Two sizes only: 14px for node labels, 12px for subtitles and annotations.
- Two weights only: 400 (regular) and 500 (bold/headings).
- No font-size below 11px.
- Sentence case always. Never Title Case or ALL CAPS.

## Font Width Calibration
At 14px, Inter renders approximately:
- Lowercase characters: ~7.5px average width
- Uppercase characters: ~9px average width
- Practical estimate: chars x 8px = rendered width at 14px
- At 12px: chars x 6.8px = rendered width

Before placing text in a box: box_width must be >= (char_count x 8) + 24px padding.
If the text doesn't fit, either shorten the label or widen the box.

SVG `<text>` never auto-wraps. Every line break needs an explicit
`<tspan x="..." dy="1.2em">`. If your subtitle needs wrapping, it's too long.

## Colors
Use CSS variables for text. Never hardcode text colors.
- `var(--color-text-primary)`: main text
- `var(--color-text-secondary)`: muted/subtitle text
- `var(--color-text-tertiary)`: hints, annotations

For fills, use the named color ramp classes. Apply to a `<g>` wrapping
shapes and text. Child text colors auto-adjust for the fill.

### Color Ramp Reference
9 ramps, each with 7 stops (lightest to darkest):

| Class      | 50 (fill)  | 200 (light) | 600 (stroke) | 800 (text on fill) |
|------------|-----------|-------------|--------------|-------------------|
| c-purple   | #EEEDFE   | #AFA9EC     | #534AB7      | #3C3489           |
| c-teal     | #E1F5EE   | #5DCAA5     | #0F6E56      | #085041           |
| c-coral    | #FAECE7   | #F0997B     | #993C1D      | #712B13           |
| c-pink     | #FBEAF0   | #ED93B1     | #993556      | #72243E           |
| c-gray     | #F1EFE8   | #B4B2A9     | #5F5E5A      | #444441           |
| c-blue     | #E6F1FB   | #85B7EB     | #185FA5      | #0C447C           |
| c-green    | #EAF3DE   | #97C459     | #3B6D11      | #27500A           |
| c-amber    | #FAEEDA   | #EF9F27     | #854F0B      | #633806           |
| c-red      | #FCEBEB   | #F09595     | #A32D2D      | #791F1F           |

Color assignment rules:
- Color encodes MEANING, not sequence. Don't cycle through colors.
- Group nodes by category — same type = same color.
- Use gray for neutral/structural nodes (start, end, generic steps).
- Limit to 2-3 colors per diagram. More = visual noise.
- Reserve red/green/amber for actual error/success/warning semantics.

Light mode: 50 fill + 600 stroke + 800 text.
Dark mode: 800 fill + 200 stroke + 100 text.
The `c-{ramp}` classes handle this automatically.

## Dark Mode
Mandatory. Every visual must work in both light and dark themes.
- In SVG: use `c-{ramp}` classes for colored nodes. They auto-adapt.
- In SVG: every `<text>` must have a class (`t`, `ts`, `th`). Never omit fill.
- In HTML: always use CSS variables for text and background colors.
- Never hardcode colors like `#333` or `#fff` for text.
- Mental test: if the background were near-black, would every element be visible?

## Arrow Marker
Include this `<defs>` block at the start of every SVG:

```xml
<defs>
  <marker id="arrow" viewBox="0 0 10 10" refX="8" refY="5"
          markerWidth="6" markerHeight="6" orient="auto-start-reverse">
    <path d="M2 1L8 5L2 9" fill="none" stroke="context-stroke"
          stroke-width="1.5" stroke-linecap="round"
          stroke-linejoin="round"/>
  </marker>
</defs>
```

Use `marker-end="url(#arrow)"` on lines. The head inherits line color
via `context-stroke`.

## Pre-built CSS Classes (injected by the WebUI)
- `class="t"` = sans 14px, primary color
- `class="ts"` = sans 12px, secondary color
- `class="th"` = sans 14px, font-weight 500 (bold)
- `class="box"` = neutral rect (bg-secondary fill, border stroke)
- `class="arr"` = arrow line (1.5px, open chevron head)
- `class="node"` = clickable group (cursor pointer, hover dim effect)

## Stroke Width
Use 0.5px strokes for borders and edges.

## Interaction
Make nodes clickable by default:
```xml
<g class="node" onclick="sendPrompt('Tell me about X')">
  <rect .../>
  <text .../>
</g>
```

## Hard Rules
- No gradients, drop shadows, blur, glow, or neon effects.
- No comments in SVG/HTML (waste tokens).
- No emoji — use CSS shapes or SVG paths.
- No rotated text.
- No font-size below 11px.
- No dark/colored backgrounds on outer containers.
- Connector paths need `fill="none"` (SVG defaults to fill:black).
- All text in boxes needs `dominant-baseline="central"` and proper y centering.
