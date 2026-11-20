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
 * Unit test for the WebUI LLM picker's provider list (www/js/ui/settings/llm.js):
 * the current provider is always listed (marked "(no API key)" without one), the
 * other keyed providers beside it, a "Pick a provider" placeholder with none
 * selected, and a pick sent as one message with its provider and model.
 *
 * Dependency-free: loads the real llm.js in a vm context with a minimal DOM.
 * Run: node tests/test_llm_picker.js
 */
'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

/* Values made inside the vm context have its prototypes; compare plain copies. */
const plain = (x) => JSON.parse(JSON.stringify(x));

const LLM_JS = path.join(__dirname, '..', 'www', 'js', 'ui', 'settings', 'llm.js');

class El {
   constructor(tag, id) {
      this.tagName = tag;
      this.id = id;
      this.children = [];
      this._value = '';
      this.disabled = false;
      this._selected = false;
      this.title = '';
      this.textContent = '';
      this.listeners = {};
      const classes = new Set();
      this.classList = {
         add: (c) => classes.add(c),
         remove: (c) => classes.delete(c),
         toggle: (c, on) =>
            (on === undefined ? !classes.has(c) : on) ? classes.add(c) : classes.delete(c),
         contains: (c) => classes.has(c),
      };
      this.style = {};
      this.dataset = {};
   }
   set innerHTML(v) {
      this.children = [];
   }
   get innerHTML() {
      return '';
   }
   /* A select's options as a browser keeps them: one selected at most, and
    * with none, the first enabled one when an option is inserted. */
   get selected() {
      return this._selected;
   }
   set selected(v) {
      if (v && this.parent) this.parent.options.forEach((o) => (o._selected = false));
      this._selected = !!v;
   }
   inserted(c) {
      c.parent = this;
      if (c.tagName !== 'option') return;
      if (c._selected) c.selected = true;
      if (!this.options.some((o) => o._selected)) {
         const first = this.options.find((o) => !o.disabled);
         if (first) first._selected = true;
      }
   }
   appendChild(c) {
      this.children.push(c);
      this.inserted(c);
      return c;
   }
   prepend(c) {
      this.children.unshift(c);
      this.inserted(c);
   }
   remove() {
      if (this.parent) this.parent.children = this.parent.children.filter((x) => x !== this);
   }
   setAttribute() {}
   focus() {}
   removeAttribute() {}
   addEventListener(t, f) {
      (this.listeners[t] = this.listeners[t] || []).push(f);
   }
   dispatch(t) {
      (this.listeners[t] || []).forEach((f) => f({ target: this }));
   }
   get options() {
      return this.children.filter((c) => c.tagName === 'option');
   }
   querySelector(sel) {
      const m = /option\[value="([^"]*)"\]/.exec(sel);
      return m ? this.options.find((o) => o.value === m[1]) || null : null;
   }
   get value() {
      if (this.tagName !== 'select') return this._value;
      const sel = this.options.find((o) => o.selected);
      return sel ? sel.value : '';
   }
   set value(v) {
      if (this.tagName !== 'select') {
         this._value = v;
         return;
      }
      this.options.forEach((o) => (o._selected = o.value === v));
   }
}
const els = {};
for (const id of [
   'llm-controls-grid',
   'llm-type-select',
   'llm-provider-select',
   'llm-model-select',
   'reasoning-mode-select',
   'reasoning-effort-select',
   'privacy-toggle',
   'provider-hint',
   'model-hint',
   'llm-follow-default',
])
   els[id] = new El(id.endsWith('select') ? 'select' : 'div', id);
for (const v of ['local', 'cloud']) {
   const o = new El('option');
   o.value = v;
   els['llm-type-select'].appendChild(o);
}
const sent = [];
const stub = () => new Proxy({}, { get: () => () => null });
const ctx = {
   document: {
      getElementById: (id) => els[id] || null,
      createElement: (t) => new El(t),
      querySelector: () => null,
      querySelectorAll: () => [],
   },
   DawnWS: { isConnected: () => true, send: (m) => sent.push(m) },
   DawnReasoning: stub(),
   DawnDialog: stub(),
   DawnHistory: stub(),
   DawnState: stub(),
   DawnToast: stub(),
   console,
   setTimeout: () => 0,
   clearTimeout: () => {},
};
ctx.window = ctx;
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(LLM_JS, 'utf8'), ctx, { filename: LLM_JS });
const L = ctx.DawnSettingsLlm;
L.initLlmControls();
const ps = els['llm-provider-select'];
const show = () =>
   ps.options
      .map((o) => (o.selected ? '*' : '') + (o.disabled ? '!' : '') + o.value + '=' + o.textContent)
      .join(' | ');
L.updateCloudModelLists({
   llm: {
      cloud: {
         openai_models: ['gpt-5.5'],
         claude_models: ['claude-haiku-5-5'],
         gemini_models: ['gemini-3.8-flash'],
      },
   },
});

// 1. Session on keyless Claude, only OpenAI keyed: Claude listed and marked, selected.
L.updateLlmControls({
   type: 'cloud',
   provider: 'Claude',
   model: 'claude-haiku-5-5',
   openai_available: true,
   claude_available: false,
   gemini_available: false,
   openrouter_available: false,
});
console.log('1', show());
assert.deepStrictEqual(plain(ps.options.map((o) => o.value)), ['openai', 'claude']);
assert.strictEqual(ps.value, 'claude');
assert.ok(ps.options[1].textContent.includes('(no API key)'));
assert.strictEqual(ps.disabled, false);
assert.strictEqual(els['provider-hint'].textContent, 'No API key'); // the select is narrow

// 2. Picking OpenAI sends one message with provider and model.
sent.length = 0;
ps.value = 'openai';
ps.dispatch('change');
console.log('2', JSON.stringify(sent));
assert.strictEqual(sent.length, 1);
assert.deepStrictEqual(plain(sent[0].payload), {
   type: 'cloud',
   provider: 'openai',
   model: 'gpt-5.5',
});

// 3. No provider selected ("None"), two keyed: placeholder first, selected, disabled option.
L.updateLlmControls({
   type: 'cloud',
   provider: 'None',
   model: '',
   openai_available: true,
   claude_available: true,
   gemini_available: false,
   openrouter_available: false,
});
console.log('3', show());
assert.strictEqual(ps.options[0].textContent, 'Pick a provider');
assert.ok(ps.options[0].disabled && ps.options[0].selected);
assert.deepStrictEqual(plain(ps.options.slice(1).map((o) => o.value)), ['openai', 'claude']);
assert.strictEqual(ps.disabled, false);

// 4. Nothing keyed, nothing selected.
L.updateLlmControls({
   type: 'cloud',
   provider: 'none',
   model: '',
   openai_available: false,
   claude_available: false,
   gemini_available: false,
   openrouter_available: false,
});
console.log('4', show());
assert.strictEqual(ps.options.length, 1);
assert.strictEqual(ps.options[0].textContent, 'No API keys configured');
assert.strictEqual(ps.disabled, true);
assert.strictEqual(els['provider-hint'].textContent, 'Add a key in Settings');

// 5. Local session, dawn.toml provider gemini (keyless), switch type to cloud: Gemini marked, sent.
L.extractGlobalDefaults({
   llm: { type: 'local', cloud: { provider: 'gemini', gemini_models: ['gemini-3.8-flash'] } },
});
L.updateLlmControls({
   type: 'local',
   provider: 'none',
   model: '',
   openai_available: true,
   claude_available: false,
   gemini_available: false,
   openrouter_available: false,
});
sent.length = 0;
els['llm-type-select'].value = 'cloud';
els['llm-type-select'].dispatch('change');
console.log('5', show(), JSON.stringify(sent.map((m) => m.payload)));
assert.strictEqual(ps.value, 'gemini');
assert.ok(ps.querySelector('option[value="gemini"]').textContent.includes('(no API key)'));
assert.deepStrictEqual(
   plain(sent.filter((m) => m.type === 'set_session_llm').map((m) => m.payload)),
   [{ type: 'cloud', provider: 'gemini', model: 'gemini-3.8-flash' }]
);

// 6. OpenRouter session with no key is kept (was the old special case).
L.updateLlmControls({
   type: 'cloud',
   provider: 'openrouter',
   model: 'x/y',
   openai_available: true,
   claude_available: false,
   gemini_available: false,
   openrouter_available: false,
});
console.log('6', show());
assert.strictEqual(ps.value, 'openrouter');
// 7. dawn.toml names no cloud provider: switching to cloud shows the placeholder and
// sends nothing; picking a provider then sends the whole pick.
L.extractGlobalDefaults({ llm: { type: 'local', cloud: { provider: '' } } });
L.updateLlmControls({
   type: 'local',
   provider: 'none',
   model: '',
   openai_available: true,
   claude_available: true,
   gemini_available: false,
   openrouter_available: false,
});
sent.length = 0;
els['llm-type-select'].value = 'cloud';
els['llm-type-select'].dispatch('change');
console.log('7', show(), JSON.stringify(sent.map((m) => m.payload)));
assert.strictEqual(ps.value, '');
assert.strictEqual(ps.options[0].textContent, 'Pick a provider');
assert.strictEqual(sent.filter((m) => m.type === 'set_session_llm').length, 0);
assert.strictEqual(els['llm-model-select'].options[0].textContent, 'Pick a provider first');
ps.value = 'claude';
ps.dispatch('change');
assert.deepStrictEqual(
   plain(sent.filter((m) => m.type === 'set_session_llm').map((m) => m.payload)),
   [{ type: 'cloud', provider: 'claude', model: 'claude-haiku-5-5' }]
);

// 8. A provider cleared in Settings doesn't linger in the defaults.
L.extractGlobalDefaults({ llm: { cloud: { provider: 'claude' } } });
L.extractGlobalDefaults({ llm: { cloud: { provider: '' } } });
assert.strictEqual(L.getGlobalDefaults().provider, '');

// 9. "anthropic" is Claude; a provider the picker doesn't offer shows the placeholder.
const keyed = {
   type: 'cloud',
   model: '',
   openai_available: true,
   claude_available: true,
   gemini_available: false,
   openrouter_available: false,
};
L.updateLlmControls({ ...keyed, provider: 'anthropic' });
assert.strictEqual(ps.value, 'claude');
L.updateLlmControls({ ...keyed, provider: 'mistral' });
console.log('9', show());
assert.strictEqual(ps.value, '');
assert.strictEqual(ps.options[0].textContent, 'Pick a provider');

// 10. A new conversation on a cloud default rebuilds the list from a local session.
L.extractGlobalDefaults({
   llm: { type: 'cloud', cloud: { provider: 'claude', claude_models: ['claude-haiku-5-5'] } },
});
L.updateLlmControls({ ...keyed, type: 'local', provider: 'none' });
L.resetConversationLlmControls();
console.log('10', show(), els['llm-model-select'].value);
assert.strictEqual(ps.value, 'claude');
assert.strictEqual(els['llm-model-select'].value, 'claude-haiku-5-5');

// 11. A keyless current provider alone: disabled, and the hint says why.
L.updateLlmControls({
   ...keyed,
   openai_available: false,
   claude_available: false,
   provider: 'claude',
});
assert.strictEqual(ps.options.length, 1);
assert.strictEqual(ps.disabled, true);
assert.strictEqual(els['provider-hint'].textContent, 'No API key');

// 12. Switching to local replaces the cloud list and its hint at once.
els['llm-type-select'].value = 'local';
els['llm-type-select'].dispatch('change');
assert.strictEqual(ps.value, 'detecting');
assert.strictEqual(els['provider-hint'].textContent, 'Detecting...');

// 13. A new conversation on a cloud default with no usable provider shows it at once.
L.extractGlobalDefaults({ llm: { type: 'cloud', cloud: { provider: 'gemini' } } });
L.updateLlmControls({ ...keyed, type: 'local', provider: 'none', model: 'qwen3-4b' });
L.resetConversationLlmControls();
console.log('13', show());
assert.strictEqual(ps.value, 'gemini');
assert.strictEqual(els['provider-hint'].textContent, 'No API key');
assert.notStrictEqual(els['llm-model-select'].value, 'qwen3-4b'); // not the old session's

// 14. "Use the default" shows only while the conversation has its own model, and
// sends a saved reset that keeps the reasoning.
const follow = els['llm-follow-default'];
L.updateLlmControls({ ...keyed, provider: 'claude', follows_default: true });
assert.ok(follow.classList.contains('hidden'));
L.updateLlmControls({ ...keyed, provider: 'claude', follows_default: false });
assert.ok(!follow.classList.contains('hidden'));
sent.length = 0;
follow.dispatch('click');
const reset = plain(sent.filter((m) => m.type === 'set_session_llm').map((m) => m.payload));
assert.strictEqual(reset.length, 1);
assert.strictEqual(reset[0].type, 'reset');
assert.strictEqual(reset[0].from_restore, undefined); // saved with the conversation
assert.ok('thinking_mode' in reset[0] && 'reasoning_effort' in reset[0]);
assert.ok(follow.classList.contains('hidden'));

console.log('ALL OK');
