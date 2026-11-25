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
 * Unit test for the WebUI's message Markdown (www/js/ui/format.js): a trailing
 * memory-citation tag is hidden, a citation tag anywhere else in a reply shows as
 * written, HTML a user typed shows as typed, and ordinary Markdown renders as
 * before.
 *
 * Dependency-free: loads the vendored marked and the real format.js in a vm
 * context.  DOMPurify is an identity stub here: this checks what marked and the
 * renderers produce, not the sanitizer.
 * Run: node tests/test_format_markdown.js
 */
'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const WWW = path.join(__dirname, '..', 'www', 'js');
let sanitizeOpts = null; /* what the formatter last asked the sanitizer for */
const ctx = {
   DOMPurify: {
      sanitize: (html, opts) => {
         sanitizeOpts = opts;
         return html;
      },
   },
};
ctx.window = ctx;
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(WWW, 'vendor', 'marked.min.js'), 'utf8'), ctx);
vm.runInContext(fs.readFileSync(path.join(WWW, 'ui', 'format.js'), 'utf8'), ctx);
const md = (text, opts) => ctx.DawnFormat.markdown(text, opts);

/* A user's typed tag (or any HTML) shows as typed. */
assert.strictEqual(
   md('you tried to reference "<cited>" and', { user: true }),
   '<p>you tried to reference &quot;&lt;cited&gt;&quot; and</p>\n'
);
assert.strictEqual(
   md('I typed <foo>hello</foo> and **bold**', { user: true }),
   '<p>I typed &lt;foo&gt;hello&lt;/foo&gt; and <strong>bold</strong></p>\n'
);
/* A user's text is never stripped as a citation. */
assert.strictEqual(
   md('Here: <cited>M1</cited>', { user: true }),
   '<p>Here: &lt;cited&gt;M1&lt;/cited&gt;</p>\n'
);

/* Example tags mid-reply show; the reply goes on past them. */
assert.strictEqual(
   md(
      'The tag in backticks: `<cited>M1,M4</cited>` and bare mid-sentence: ' +
         '<cited>M1,M4</cited>\n\nIf you can read this line, the reply made it past both tags.'
   ),
   '<p>The tag in backticks: <code>&lt;cited&gt;M1,M4&lt;/cited&gt;</code> and bare ' +
      'mid-sentence: &lt;cited&gt;M1,M4&lt;/cited&gt;</p>\n<p>If you can read this line, ' +
      'the reply made it past both tags.</p>\n'
);

/* Text inside an inline <code> a user typed is text too, not markup. */
assert.strictEqual(
   md('use <code>a<b && c>d</code> here', { user: true }),
   '<p>use &lt;code&gt;a&lt;b &amp;&amp; c&gt;d&lt;/code&gt; here</p>\n'
);

/* Trailing tags (one, two, a cut-off one) are hidden. */
assert.strictEqual(md('Answer here.<cited>M2</cited>'), '<p>Answer here.</p>\n');
assert.strictEqual(md('Answer.<cited>M1</cited> <cited>ID:5</cited>\n'), '<p>Answer.</p>\n');
assert.strictEqual(md('Answer.<cited>M1,M'), '<p>Answer.</p>\n');

/* An <end_of_turn> the server kept in a reply (a mention) shows. */
assert.strictEqual(
   md("Gemma's template closes a turn with <end_of_turn>, then the next."),
   '<p>Gemma&#39;s template closes a turn with &lt;end_of_turn&gt;, then the next.</p>\n'
);

/* A piece of a message that isn't its end keeps a tag at its own end, and a tag
 * with more whitespace after it than the server allows stays, as on the server. */
assert.strictEqual(
   md('Example: <cited>M1</cited>', { inner: true }),
   '<p>Example: &lt;cited&gt;M1&lt;/cited&gt;</p>\n'
);
assert.ok(
   md('Example: <cited>M1</cited>' + ' '.repeat(33)).startsWith(
      '<p>Example: &lt;cited&gt;M1&lt;/cited&gt;'
   )
);

/* Ordinary Markdown, a fenced block holding HTML, an autolink and other HTML in
 * a reply render as before. */
assert.strictEqual(
   md('# Title\n\n- **one**\n- two\n\n```\n<div>x</div>\n```\n\nSee <https://example.com>.'),
   '<h1>Title</h1>\n<ul>\n<li><strong>one</strong></li>\n<li>two</li>\n</ul>\n' +
      '<pre><code>&lt;div&gt;x&lt;/div&gt;\n</code></pre>\n<p>See <a href="https://example.com" ' +
      'target="_blank" rel="noopener noreferrer">https://example.com</a>.</p>\n'
);
assert.strictEqual(md('a <b>bold</b> word'), '<p>a <b>bold</b> word</p>\n');

/* A rendered message goes to the sanitizer with no <style>, <form>, inline
 * style, class or id allowed (DOMPurify's defaults keep them all). */
md('plain');
assert.strictEqual(JSON.stringify(sanitizeOpts.FORBID_TAGS), '["style","form"]');
assert.strictEqual(JSON.stringify(sanitizeOpts.FORBID_ATTR), '["style","class","id"]');

console.log('test_format_markdown: OK');
