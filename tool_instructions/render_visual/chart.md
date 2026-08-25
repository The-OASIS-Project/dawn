# Chart guidelines

Use Chart.js for data visualization. Load from the local server inside HTML type visuals.

Use SINGLE quotes for HTML attributes and JS strings (the code is embedded in
JSON — see `_core.md` § Quoting); the examples below already do.

## Setup
```html
<script src='/js/vendor/chart.umd.js'></script>

<canvas id='myChart'></canvas>
<script>
const ctx = document.getElementById('myChart');
new Chart(ctx, {
   type: 'bar',
   data: { ... },
   options: {
      responsive: true,
      maintainAspectRatio: false,
      plugins: { legend: { position: 'top' } }
   }
});
</script>
```

## Sizing (IMPORTANT)
The visual is rendered in a frame with a fixed height. Always set
`maintainAspectRatio: false` and do NOT put a `height` or `max-height` on the
`<canvas>`. With `maintainAspectRatio: true` (the Chart.js default) plus a
`max-height`-only canvas, the chart resolves its height from a content-sized
container, collapses to 0×0, and renders as a black box. `responsive: true` +
`maintainAspectRatio: false` lets the chart fill the frame correctly.

## Theming
Read CSS variables and apply to Chart.js:
```javascript
const style = getComputedStyle(document.documentElement);
const textColor = style.getPropertyValue('--color-text-primary').trim();
const gridColor = style.getPropertyValue('--color-border').trim();

Chart.defaults.color = textColor;
Chart.defaults.borderColor = gridColor;
```

## Color Mapping for Datasets
Use color ramp 600 stops for dataset colors:
- Dataset 1: #534AB7 (purple-600)
- Dataset 2: #0F6E56 (teal-600)
- Dataset 3: #993C1D (coral-600)
- Dataset 4: #185FA5 (blue-600)

## Chart Type Selection
- Comparison across categories → bar (horizontal if long labels)
- Trend over time → line
- Part of whole → doughnut (not pie — doughnut is more readable)
- Correlation → scatter
- Multi-variable comparison → radar

## Worked example: time-series line with a min–max band
Some tools (e.g. `system_status action="trend"`) hand you a ready-made series as
`"labels":[…]`, `"avg":[…]`, and — for temperature/battery — `"min":[…]` and
`"max":[…]`. **Copy those arrays verbatim** into the chart; do not recompute or
re-round them. Use a category x-axis (the labels), plot `avg` as the line, and
shade `min`→`max` as a band. Missing points arrive as `null`; keep
`spanGaps: true` so a data gap is bridged rather than dropping to zero.

The band is two datasets (`min`, then `max` with `fill: '-1'` to fill down to
`min`) drawn under the `avg` line. Omit the band when the series has no
`min`/`max` (cpu/memory/fan/power/voltage) — just plot `avg`.

```html
<script src='/js/vendor/chart.umd.js'></script>
<canvas id='c'></canvas>
<script>
const style = getComputedStyle(document.documentElement);
Chart.defaults.color = style.getPropertyValue('--color-text-primary').trim();
Chart.defaults.borderColor = style.getPropertyValue('--color-border').trim();

// Paste the arrays from the tool result exactly as given:
const labels = ['00:00','00:30','01:00'];   // 'labels'
const avg    = [44.1, 45.2, 46.0];          // 'avg'
const min    = [39.1, 40.0, 41.2];          // 'min'  (omit if absent)
const max    = [47.2, 49.1, 50.3];          // 'max'  (omit if absent)

new Chart(document.getElementById('c'), {
   type: 'line',
   data: { labels, datasets: [
      { label: 'min', data: min, borderWidth: 0, pointRadius: 0, fill: false },
      { label: 'range', data: max, borderWidth: 0, pointRadius: 0,
        backgroundColor: 'rgba(83,74,183,0.15)', fill: '-1' },   // shade max→min
      { label: 'avg', data: avg, borderColor: '#534AB7', borderWidth: 2,
        pointRadius: 0, tension: 0.25 }
   ]},
   options: {
      responsive: true, maintainAspectRatio: false, spanGaps: true,
      scales: { x: { ticks: { maxTicksLimit: 8 } } },
      plugins: { legend: { position: 'top' } }
   }
});
</script>
```
