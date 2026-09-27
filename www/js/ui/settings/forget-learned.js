/**
 * DAWN Settings - Forget what was learned
 * After a conversation is marked private the server reports what memory had
 * already learned from it (conversation_learned); this offers to forget it and
 * tracks the forget request (forget_conversation_memories).
 */
(function () {
   'use strict';

   /** Plural helper: "1 memory" / "3 memories". */
   function countNoun(n, one, many) {
      return `${n} ${n === 1 ? one : many}`;
   }

   /** Toast shown while a forget runs; replaced by the result. */
   let forgetProgressToast = null;
   let forgetProgressTimer = null;
   /* The server waits up to 120 s for an in-flight memory save before forgetting;
    * past this with no reply the outcome is unknown (e.g. the connection dropped). */
   const FORGET_REPLY_TIMEOUT_MS = 150000;

   function clearForgetProgress() {
      clearTimeout(forgetProgressTimer);
      forgetProgressTimer = null;
      if (forgetProgressToast && forgetProgressToast.dismiss) forgetProgressToast.dismiss();
      forgetProgressToast = null;
   }

   /**
    * Send a forget request; failures offer Retry.
    * @param {number} conversationId - Conversation whose memories to forget
    */
   function sendForget(conversationId) {
      if (typeof DawnToast === 'undefined') return;
      clearForgetProgress();
      if (!DawnWS || !DawnWS.isConnected()) {
         DawnToast.show('Cannot forget memories: not connected', 'error', 0, {
            actions: [{ label: 'Retry', onClick: () => sendForget(conversationId) }],
         });
         return;
      }
      forgetProgressToast = DawnToast.show('Forgetting…', 'info', 0);
      forgetProgressTimer = setTimeout(() => {
         clearForgetProgress();
         DawnToast.show(
            'No reply to the forget request; check the Memory panel before retrying',
            'warning',
            0
         );
      }, FORGET_REPLY_TIMEOUT_MS);
      DawnWS.send({
         type: 'forget_conversation_memories',
         payload: { conversation_id: conversationId },
      });
   }

   /**
    * Ask whether to forget memories already learned from a conversation that was
    * just marked private.
    * @param {number} conversationId - Conversation just marked private
    * @param {Object} learned - conversation_learned payload (memories, relations)
    */
   async function offerForgetLearned(conversationId, learned) {
      const memories = learned.memories || 0;
      const outdated = learned.outdated || 0;
      const links = learned.relations || 0;
      const name =
         typeof DawnFormat !== 'undefined' && DawnFormat.assistantName
            ? DawnFormat.assistantName()
            : 'The assistant';
      let what = countNoun(memories, 'memory', 'memories');
      if (outdated > 0) {
         what += `, ${countNoun(outdated, 'older version', 'older versions')} of memories`;
      }
      if (links > 0) {
         what += ` and ${countNoun(links, 'connection', 'connections')} between people and things`;
      }
      const confirmed = await DawnDialog.confirm(
         `${name} already learned ${what} from this conversation (and any continuation of it) ` +
            'before it was private. Forget them too? This cannot be undone.',
         {
            title: 'Forget what was learned?',
            okText: 'Forget',
            cancelText: 'Keep',
            danger: true,
            detail:
               'This includes anything saved here with "remember". What another ' +
               'conversation also taught stays, as do imported memories. An older ' +
               'version a removed memory had replaced is removed too, so it does not ' +
               'come back as current. Memories saved before September 2026 may not ' +
               'record where they came from; check the Memory panel for those.',
         }
      );
      if (confirmed) sendForget(conversationId);
   }

   /**
    * Handle conversation_learned from server: what memory had already learned
    * from a conversation that was just marked private.
    * @param {Object} payload - {conversation_id, success, memories, relations, ...}
    */
   /** Busy re-asks so far, per conversation (another count or forget was running). */
   const learnedRetries = new Map();
   const LEARNED_RETRY_MS = 5000;
   const LEARNED_RETRY_MAX = 12;

   function handleConversationLearned(payload) {
      if (payload && !payload.success && payload.busy && payload.conversation_id) {
         // Another count or forget was running for this user: ask again shortly.
         const id = payload.conversation_id;
         const tries = (learnedRetries.get(id) || 0) + 1;
         if (tries <= LEARNED_RETRY_MAX) {
            learnedRetries.set(id, tries);
            setTimeout(() => {
               if (DawnWS && DawnWS.isConnected()) {
                  DawnWS.send({
                     type: 'conversation_learned_request',
                     payload: { conversation_id: id },
                  });
               }
            }, LEARNED_RETRY_MS);
            return;
         }
      }
      if (payload && payload.conversation_id) {
         learnedRetries.delete(payload.conversation_id);
      }
      if (!payload || !payload.success) {
         // Fails toward not asking; the user can still forget from a later toggle.
         console.warn('conversation_learned: could not count learned memories', payload);
         return;
      }
      // The user may have made it public again while the count ran.
      const conv =
         typeof DawnSettingsLlm !== 'undefined' ? DawnSettingsLlm.getConversationLlmState() : null;
      if (conv && conv.conversation_id === payload.conversation_id && !conv.is_private) {
         return;
      }
      if ((payload.memories || 0) + (payload.outdated || 0) + (payload.relations || 0) > 0) {
         offerForgetLearned(payload.conversation_id, payload);
      }
   }

   /**
    * Handle forget_conversation_memories_response from server
    * @param {Object} payload - Response payload
    */
   function handleForgetConversationMemoriesResponse(payload) {
      clearForgetProgress();
      if (typeof DawnToast === 'undefined') {
         return;
      }
      if (payload.success) {
         const n = payload.memories || 0;
         if (n + (payload.outdated || 0) + (payload.relations || 0) === 0) {
            DawnToast.show('Nothing left to forget', 'info');
         } else {
            DawnToast.show(`Forgot ${countNoun(n, 'memory', 'memories')}`, 'success');
         }
      } else {
         const id = payload.conversation_id;
         DawnToast.show(payload.error || 'Could not forget memories', 'error', 0, {
            actions: id ? [{ label: 'Retry', onClick: () => sendForget(id) }] : [],
         });
      }
   }

   window.DawnForgetLearned = {
      handleConversationLearned,
      handleForgetConversationMemoriesResponse,
   };
})();
