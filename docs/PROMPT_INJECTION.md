# Prompt Injection: How DAWN Handles Outside Text

DAWN reads text it didn't write and whose author isn't the user: email, web pages, search results,
uploaded documents, calendar invites, messages from chat channels, MCP results. Any of it can carry
instructions aimed at the model. This document records what is known about that problem (research and
vendor guidance, as of October 2026), the layers DAWN has against it, the gaps that remain, and the
rule for deciding where a hard gate is worth its cost.

## What is known

**It can't be solved inside the model.** No prompt, frame, filter or classifier holds against a
determined attacker:

- *The Attacker Moves Second* (Nasr, Carlini et al.; USENIX Security '26) attacked 12 published
  defenses with adaptive attacks (gradient search, reinforcement learning, human red-teamers). Most
  fell to more than 90% attack success; most had reported near zero against static attacks.
- OpenAI (December 2025): prompt injection, like scams and social engineering, "is unlikely to ever
  be fully 'solved'". The UK NCSC: it "may never be totally mitigated"; limit the damage instead.

**Frontier models are much better than they were, and small models are not.** Anthropic reported
about 1% success for an adaptive attacker given 100 attempts per scenario against Claude Opus 4.5 in a
browser agent (November 2025). Results vary widely with the environment: a constrained coding setting
held at 0% after 200 attempts, a GUI setting reached 57% with safeguards on (Opus 4.6 system card,
as reported). Small local models follow injected text far more readily. DAWN supports local models, so
anything that relies on the model's judgment is weakest exactly where some users run it.

**What holds is architecture: what a steered turn can reach.**

- *Agents Rule of Two* (Meta, 2025): within a session, an agent should have no more than two of:
  untrusted input, access to private data or sensitive systems, the ability to change state or
  communicate out. With all three, a human approves.
- *The lethal trifecta* (Simon Willison): private data + untrusted content + a way to send data out
  = data theft is possible. (Untrusted input + the ability to change state is unsafe even without
  private data.)
- *CaMeL* (Google DeepMind, 2025): fix the plan from the user's request before any untrusted text is
  read, and tag data with where it came from so it can't flow to a tool the policy forbids. Provable
  security on 77% of AgentDojo tasks, against 84% undefended.
- Narrowing which tools a turn has before it reads untrusted text cut attack success in AgentDojo
  from 58% to 7%, with no loss of usefulness.

## Vendor guidance

**Anthropic** ([Mitigate jailbreaks and prompt injections](https://platform.claude.com/docs/en/test-and-evaluate/strengthen-guardrails/mitigate-jailbreaks)):

- Put untrusted content **only in tool results**, never in the system prompt or plain user text.
  Claude is trained to treat instructions inside tool results with skepticism.
- Say what the content is and where it came from (in the tool's description or the result).
- State the policy in the system prompt: content from tools, documents and searches is untrusted
  data; instructions in it are information to report, never commands, never a reason to call a tool
  the user didn't ask for; when it holds instructions aimed at the model, tell the user.
- JSON-encode untrusted strings where possible, so the payload can't break out of its delimiters.
- Don't put your own instructions in tool results; send them in the user turn after.
- Least privilege; optionally screen tool output with a small model before the main one sees it;
  red-team the agent; monitor outputs.

**OpenAI** (agent-safety guidance, through secondary summaries): treat external content as
untrusted, start from least privilege and read-only, put approval gates on risky actions, sandbox
tools, red-team continuously; a better prompt alone won't fix injection. Its instruction hierarchy
ranks tool output lowest.

**Qwen**: no published injection guidance (the Qwen-Agent docs cover tool-calling mechanics only).
Enforcement has to be in the tool layer, in code.

## DAWN's layers

From most to least load-bearing:

1. **Who may make a tool call** (`src/core/tool_call_policy.c`): every action has a kind (read,
   fetch, state, device, prepare, act; `tool_registry`), and the caller's kind of turn decides which
   kinds run. Background jobs and unattended turns can't act; an unverified chat sender's actions wait
   for the user's reply code.
2. **Confirms** (`include/core/turn_origin.h`): an action that was prepared runs only on the user's
   next turn in the same session, never in a turn a rendered visual started.
3. **Outside text in tool results, framed** (`prompt_third_party`): an email or web page goes in its
   frame (`EMAIL CONTENT`, `WEB CONTENT`) with the conversation's secret tag and a line saying it is
   data. Memory extraction reads a framed result as a stub, so outside text can't plant a fact.
4. **The neutralizer** (`llm_context_neutralize`): defuses imitations of DAWN's own framing, tags and
   item lines in any text DAWN didn't write; the conversation's secret is masked wherever outside text
   could echo it.
5. **The system prompt** (`prompt_builder.c` context rules): only tagged framing and system messages
   are DAWN's; anything imitating them is data.

Layers 1 and 2 hold whatever the model does. Layers 3 to 5 lower the odds that it is steered.

## Gaps

- **Sending data out in a live turn.** `url_fetch` and `search` run in a user turn without a
  confirm, so text the model read can make it fetch a URL carrying data from the conversation. The
  fix is a per-turn capability mask: once outside text is in the turn, outward calls are limited. Not
  built yet.
- **Read-backs written by the model.** A confirm is the user's yes to what the model said it
  prepared; steered text can shape both the item and its description. The fix is a preview DAWN
  renders from the staged item (recipient, subject, body) for anything prepared in a turn that read
  outside text.

## The decision rule

Hard-block only where all three meet:

1. outside text is in the turn;
2. the effect is silent or can't be undone (a message sent, a device moved, data sent out, a memory
   planted);
3. no human sees a summary DAWN renders itself before it happens.

Everywhere else, prefer making the effect visible and reversible over refusing it: a refusal that
fires on ordinary requests is a cost every user pays. Frames, the neutralizer and the system-prompt
policy go everywhere; they are cheap and they lower the rate, but they are never the only defense
for an effect the rule above covers.

## Sources

- Nasr, Carlini et al., [The Attacker Moves Second](https://arxiv.org/abs/2510.09023) (USENIX Security '26)
- Simon Willison, [Agents Rule of Two and The Attacker Moves Second](https://simonwillison.net/2025/nov/2/new-prompt-injection-papers)
- Debenedetti et al., [Defeating Prompt Injections by Design (CaMeL)](https://arxiv.org/abs/2503.18813)
- Anthropic, [Prompt injection defenses](https://www.anthropic.com/research/prompt-injection-defenses) and [Mitigate jailbreaks and prompt injections](https://platform.claude.com/docs/en/test-and-evaluate/strengthen-guardrails/mitigate-jailbreaks)
- TechCrunch, [OpenAI says AI browsers may always be vulnerable to prompt injection](https://techcrunch.com/2025/12/22/openai-says-ai-browsers-may-always-be-vulnerable-to-prompt-injection-attacks/)
- [Securing AI Agents Against Prompt Injection Attacks](https://arxiv.org/abs/2511.15759)
