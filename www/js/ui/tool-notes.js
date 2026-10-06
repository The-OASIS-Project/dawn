/**
 * DAWN Tool Notes
 * Reads the text notes some saved conversations hold in place of tool calls,
 * so the history view can draw them as tool pills like any other tool turn.
 *
 * Older voice conversations were saved without their tool calls' structure;
 * the database migrations rewrote those turns as text:
 *   - an assistant message ending in "[Tool Call: <name>]" notes (one per
 *     call, separated by a blank line), or "[Tool Call: its name and arguments
 *     weren't saved]" when the call itself was never saved;
 *   - one or more assistant messages after it holding "[Tool Result: <text>]"
 *     notes (several results may share one message).
 * There is no call id, name or arguments beyond what the notes say, and none is
 * invented: a pill shows the name when the note has one, and says the tool's
 * details weren't saved with the conversation otherwise.
 *
 * Usage:
 *   DawnToolNotes.collect(messages, i)
 *     -> null when messages[i] isn't a tool-call note, else
 *        { prose, items: [{id, name, args, result}], consumed }
 *        where `consumed` counts the result messages after i that were read.
 */
(function (global) {
   'use strict';

   var CALL = '[Tool Call: ';
   var RESULT = '[Tool Result: ';
   var UNSAVED = "its name and arguments weren't saved";
   var SEP = '\n\n';
   var uid = 0;

   function isAssistantText(msg) {
      return (
         msg &&
         msg.role === 'assistant' &&
         typeof msg.content === 'string' &&
         !(Array.isArray(msg.tool_calls) && msg.tool_calls.length > 0)
      );
   }

   // The prose before a message's call notes, and the notes' names, or null
   // when the message doesn't end in call notes.
   function parseCalls(text) {
      var start = text.indexOf(CALL) === 0 ? 0 : text.indexOf(SEP + CALL);
      if (start < 0) return null;
      var prose = start === 0 ? '' : text.slice(0, start);
      var rest = start === 0 ? text : text.slice(start + SEP.length);
      var names = [];
      var parts = rest.split(SEP);
      for (var k = 0; k < parts.length; k++) {
         var p = parts[k];
         if (p.indexOf(CALL) !== 0 || p.charAt(p.length - 1) !== ']') return null;
         names.push(p.slice(CALL.length, -1));
      }
      return { prose: prose, names: names };
   }

   // The results a result-note message holds, in order, or null when it isn't one.
   function parseResults(text) {
      if (text.indexOf(RESULT) !== 0 || text.charAt(text.length - 1) !== ']') return null;
      var body = text.slice(RESULT.length, -1);
      // Results joined as "…]\n\n[Tool Result: …"; a result's own text may hold
      // anything, so split only on that exact seam.
      return body.split(']' + SEP + RESULT);
   }

   function collect(messages, i) {
      var msg = messages[i];
      if (!isAssistantText(msg)) return null;
      var calls = parseCalls(msg.content);
      if (!calls) return null;

      var results = [];
      var consumed = 0;
      for (var j = i + 1; j < messages.length; j++) {
         var next = messages[j];
         var parsed = isAssistantText(next) ? parseResults(next.content) : null;
         if (!parsed) break;
         results = results.concat(parsed);
         consumed++;
      }

      var n = Math.max(calls.names.length, results.length);
      var items = [];
      for (var k = 0; k < n; k++) {
         var name = calls.names[k];
         var known = name && name !== UNSAVED;
         items.push({
            id: 'note-' + ++uid,
            name: known ? name : 'tool',
            args: known ? '' : 'Tool details weren’t saved with this conversation',
            result: k < results.length ? results[k] : '',
         });
      }
      return { prose: calls.prose, items: items, consumed: consumed };
   }

   global.DawnToolNotes = { collect: collect };
})(window);
