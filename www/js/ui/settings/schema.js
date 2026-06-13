/**
 * DAWN Settings - Schema and Rendering
 * Settings schema definition and section/field rendering
 */
(function () {
   'use strict';

   const Utils = window.DawnSettingsUtils;

   // Schema version — bump when adding/removing/reordering fields or sections.
   // When this changes, the settings DOM is rebuilt from scratch.
   const SCHEMA_VERSION = 8;

   // Track whether DOM has been rendered and which schema version it represents
   let renderedSchemaVersion = null;

   // Dependencies (injected by main settings module)
   let sectionsContainer = null;
   let handleSettingChangeCallback = null;
   let getCurrentConfigFn = null;
   let getRestartRequiredFieldsFn = null;
   let getDynamicOptionsFn = null;

   // showWhen dependency map: changedKey -> [element, ...]
   let showWhenDependents = {};

   /**
    * Set dependencies for schema module
    * @param {Object} deps - Dependencies object
    */
   function setDependencies(deps) {
      if (deps.sectionsContainer) sectionsContainer = deps.sectionsContainer;
      if (deps.handleSettingChange) handleSettingChangeCallback = deps.handleSettingChange;
      if (deps.getCurrentConfig) getCurrentConfigFn = deps.getCurrentConfig;
      if (deps.getRestartRequiredFields) getRestartRequiredFieldsFn = deps.getRestartRequiredFields;
      if (deps.getDynamicOptions) getDynamicOptionsFn = deps.getDynamicOptions;
   }

   /**
    * Settings schema definition
    */
   const SETTINGS_SCHEMA = {
      general: {
         label: 'General',
         icon: '&#x2699;',
         fields: {
            ai_name: {
               type: 'text',
               label: 'AI Name / Wake Word',
               hint: 'Wake word to activate voice input',
            },
            log_file: {
               type: 'text',
               label: 'Log File Path',
               placeholder: 'Leave empty for stdout',
               hint: 'Path to log file, or empty for console output',
               advanced: true,
            },
            room: {
               type: 'text',
               label: 'Room Name',
               placeholder: 'e.g., office, kitchen, bedroom',
               hint: 'Room where this device is located (for voice command context)',
            },
         },
      },
      persona: {
         label: 'Default Persona',
         icon: '&#x1F464;',
         fields: {
            description: {
               type: 'textarea',
               label: 'AI Description',
               rows: 5,
               placeholder:
                  'Leave empty to use built-in default:\n"Your name is {AI Name}. Iron-Man-style AI assistant. Female voice; witty, playful, and kind..."',
               hint: 'System-wide base persona for all users. Individual users can append to or replace this via My Settings. Leave empty for the built-in default.',
            },
         },
      },
      localization: {
         label: 'Localization',
         icon: '&#x1F30D;',
         fields: {
            location: {
               type: 'text',
               label: 'Location',
               placeholder: 'e.g., San Francisco, CA',
               hint: 'Default location for weather and local queries',
            },
            timezone: {
               type: 'select',
               label: 'Timezone',
               hint: 'IANA timezone for time-related responses',
               options: [
                  { value: '', label: '(System default)' },
                  // Sorted by UTC offset (west to east)
                  { value: 'America/Honolulu', label: '(UTC-10:00) America/Honolulu' },
                  { value: 'America/Anchorage', label: '(UTC-09:00) America/Anchorage' },
                  { value: 'America/Los_Angeles', label: '(UTC-08:00) America/Los_Angeles' },
                  { value: 'America/Vancouver', label: '(UTC-08:00) America/Vancouver' },
                  { value: 'America/Denver', label: '(UTC-07:00) America/Denver' },
                  { value: 'America/Phoenix', label: '(UTC-07:00) America/Phoenix' },
                  { value: 'America/Chicago', label: '(UTC-06:00) America/Chicago' },
                  { value: 'America/Mexico_City', label: '(UTC-06:00) America/Mexico_City' },
                  { value: 'America/New_York', label: '(UTC-05:00) America/New_York' },
                  { value: 'America/Toronto', label: '(UTC-05:00) America/Toronto' },
                  { value: 'America/Sao_Paulo', label: '(UTC-03:00) America/Sao_Paulo' },
                  { value: 'America/Buenos_Aires', label: '(UTC-03:00) America/Buenos_Aires' },
                  { value: 'UTC', label: '(UTC+00:00) UTC' },
                  { value: 'Europe/London', label: '(UTC+00:00) Europe/London' },
                  { value: 'Europe/Paris', label: '(UTC+01:00) Europe/Paris' },
                  { value: 'Europe/Berlin', label: '(UTC+01:00) Europe/Berlin' },
                  { value: 'Europe/Rome', label: '(UTC+01:00) Europe/Rome' },
                  { value: 'Europe/Madrid', label: '(UTC+01:00) Europe/Madrid' },
                  { value: 'Europe/Amsterdam', label: '(UTC+01:00) Europe/Amsterdam' },
                  { value: 'Europe/Moscow', label: '(UTC+03:00) Europe/Moscow' },
                  { value: 'Asia/Dubai', label: '(UTC+04:00) Asia/Dubai' },
                  { value: 'Asia/Mumbai', label: '(UTC+05:30) Asia/Mumbai' },
                  { value: 'Asia/Bangkok', label: '(UTC+07:00) Asia/Bangkok' },
                  { value: 'Asia/Shanghai', label: '(UTC+08:00) Asia/Shanghai' },
                  { value: 'Asia/Hong_Kong', label: '(UTC+08:00) Asia/Hong_Kong' },
                  { value: 'Asia/Singapore', label: '(UTC+08:00) Asia/Singapore' },
                  { value: 'Australia/Perth', label: '(UTC+08:00) Australia/Perth' },
                  { value: 'Asia/Tokyo', label: '(UTC+09:00) Asia/Tokyo' },
                  { value: 'Asia/Seoul', label: '(UTC+09:00) Asia/Seoul' },
                  { value: 'Australia/Sydney', label: '(UTC+10:00) Australia/Sydney' },
                  { value: 'Australia/Melbourne', label: '(UTC+10:00) Australia/Melbourne' },
                  { value: 'Pacific/Auckland', label: '(UTC+12:00) Pacific/Auckland' },
                  { value: 'Pacific/Fiji', label: '(UTC+12:00) Pacific/Fiji' },
               ],
            },
            units: {
               type: 'select',
               label: 'Units',
               options: ['imperial', 'metric'],
               hint: 'Measurement system for weather and calculations',
            },
         },
      },
      audio: {
         label: 'Audio',
         icon: '&#x1F50A;',
         fields: {
            backend: {
               type: 'select',
               label: 'Backend',
               options: ['auto', 'pulse', 'alsa'],
               restart: true,
               hint: 'Audio system: auto detects PulseAudio or falls back to ALSA',
            },
            capture_device: {
               type: 'text',
               label: 'Capture Device',
               restart: true,
               hint: 'Microphone device name (e.g., default, hw:0,0)',
            },
            playback_device: {
               type: 'text',
               label: 'Playback Device',
               restart: true,
               hint: 'Speaker device name (e.g., default, hw:0,0)',
            },
            output_rate: {
               type: 'number',
               label: 'Output Sample Rate',
               min: 8000,
               max: 96000,
               step: 100,
               hint: 'Audio output sample rate in Hz (default: 44100)',
               advanced: true,
            },
            output_channels: {
               type: 'number',
               label: 'Output Channels',
               min: 1,
               max: 2,
               hint: 'Audio output channels: 1 = mono, 2 = stereo (default: 2)',
               advanced: true,
            },
            bargein: {
               type: 'group',
               label: 'Barge-In',
               advanced: true,
               fields: {
                  enabled: {
                     type: 'checkbox',
                     label: 'Enable Barge-In',
                     hint: 'Allow interrupting AI speech with new voice input',
                  },
                  cooldown_ms: {
                     type: 'number',
                     label: 'Cooldown (ms)',
                     min: 0,
                     hint: 'Minimum time between barge-in events',
                  },
                  startup_cooldown_ms: {
                     type: 'number',
                     label: 'Startup Cooldown (ms)',
                     min: 0,
                     hint: 'Block barge-in when TTS first starts speaking',
                  },
               },
            },
         },
      },
      commands: {
         label: 'Commands',
         icon: '&#x2328;',
         advanced: true,
         fields: {
            processing_mode: {
               type: 'select',
               label: 'Processing Mode',
               options: ['direct_only', 'llm_only', 'direct_first'],
               restart: true,
               hint: 'direct_only: pattern matching only, llm_only: AI interprets all commands, direct_first: try patterns then AI',
            },
         },
      },
      vad: {
         label: 'Voice Activity Detection',
         icon: '&#x1F3A4;',
         advanced: true,
         fields: {
            speech_threshold: {
               type: 'number',
               label: 'Speech Threshold',
               min: 0,
               max: 1,
               step: 0.05,
               hint: 'VAD confidence to start listening (higher = less sensitive)',
            },
            speech_threshold_tts: {
               type: 'number',
               label: 'Speech Threshold (TTS)',
               min: 0,
               max: 1,
               step: 0.05,
               hint: 'VAD threshold during TTS playback (higher to ignore echo)',
            },
            silence_threshold: {
               type: 'number',
               label: 'Silence Threshold',
               min: 0,
               max: 1,
               step: 0.05,
               hint: 'VAD confidence to detect end of speech',
            },
            end_of_speech_duration: {
               type: 'number',
               label: 'End of Speech (sec)',
               min: 0,
               step: 0.1,
               hint: 'Silence duration before processing speech',
            },
            max_recording_duration: {
               type: 'number',
               label: 'Max Recording (sec)',
               min: 1,
               step: 1,
               hint: 'Maximum single utterance length',
            },
            preroll_ms: {
               type: 'number',
               label: 'Preroll (ms)',
               min: 0,
               hint: 'Audio captured before VAD trigger (catches word beginnings)',
            },
            chunking: {
               type: 'group',
               label: 'Audio Chunking',
               fields: {
                  enabled: {
                     type: 'checkbox',
                     label: 'Enable Chunking',
                     hint: 'Split long utterances into chunks for incremental ASR processing',
                  },
                  pause_duration: {
                     type: 'number',
                     label: 'Pause Duration (sec)',
                     min: 0.1,
                     max: 2.0,
                     step: 0.05,
                     hint: 'Silence duration to trigger a chunk boundary (default: 0.3)',
                  },
                  min_duration: {
                     type: 'number',
                     label: 'Min Chunk (sec)',
                     min: 0.1,
                     max: 10,
                     step: 0.1,
                     hint: 'Minimum audio duration before a chunk is sent (default: 1.0)',
                  },
                  max_duration: {
                     type: 'number',
                     label: 'Max Chunk (sec)',
                     min: 1,
                     max: 60,
                     step: 1,
                     hint: 'Force chunk boundary at this duration (default: 10.0)',
                  },
               },
            },
         },
      },
      asr: {
         label: 'Speech Recognition',
         icon: '&#x1F4DD;',
         fields: {
            models_path: {
               type: 'text',
               label: 'Models Path',
               restart: true,
               hint: 'Directory containing Whisper model files',
               advanced: true,
            },
            model: {
               type: 'dynamic_select',
               label: 'Model',
               restart: true,
               hint: 'Whisper model size',
               dynamicKey: 'asr_models',
               allowCustom: true,
            },
            dedup_window_sec: {
               type: 'number',
               label: 'Cross-Device Dedup Window (sec)',
               // min/max mirror ASR_DEDUP_WINDOW_SEC_* in include/config/dawn_config.h
               min: 0,
               max: 60,
               step: 1,
               hint: 'Suppress duplicate responses when two devices hear one command within this window (0 disables; default: 4)',
            },
         },
      },
      tts: {
         label: 'Text-to-Speech',
         icon: '&#x1F5E3;',
         fields: {
            models_path: {
               type: 'text',
               label: 'Models Path',
               restart: true,
               hint: 'Directory containing Piper voice model files',
               advanced: true,
            },
            voice_model: {
               type: 'dynamic_select',
               label: 'Voice Model',
               restart: true,
               hint: 'Piper voice model',
               dynamicKey: 'tts_voices',
               allowCustom: true,
            },
            length_scale: {
               type: 'number',
               label: 'Speed (0.5-2.0)',
               min: 0.5,
               max: 2.0,
               step: 0.05,
               hint: 'Speaking rate: <1.0 = faster, >1.0 = slower',
               advanced: true,
            },
         },
      },
      llm: {
         label: 'Language Model',
         icon: '&#x1F916;',
         fields: {
            type: {
               type: 'select',
               label: 'Default Mode',
               options: ['cloud', 'local'],
               hint: 'Server default: cloud uses APIs, local uses llama-server. Users can override per-session.',
            },
            max_tokens: {
               type: 'number',
               label: 'Max Tokens',
               min: 100,
               hint: 'Maximum tokens in LLM response',
               advanced: true,
            },
            cloud: {
               type: 'group',
               label: 'Cloud Settings',
               advanced: true,
               fields: {
                  use_openrouter: {
                     type: 'checkbox',
                     label: 'Route all cloud LLMs through OpenRouter',
                     hint: 'When on, all cloud traffic (chat plus memory/compaction/observe) routes through OpenRouter using openrouter_api_key in secrets.toml. The direct openai/claude/gemini settings below are hidden but preserved — turn this off to restore them.',
                  },
                  provider: {
                     type: 'select',
                     label: 'Provider',
                     options: ['openai', 'claude', 'gemini'],
                     hint: 'Cloud LLM provider',
                     showWhen: { key: 'llm.cloud.use_openrouter', value: false },
                  },
                  endpoint: {
                     type: 'text',
                     label: 'Custom Endpoint',
                     placeholder: 'Leave empty for default',
                     hint: 'Override API endpoint (for proxies or compatible APIs)',
                     showWhen: { key: 'llm.cloud.use_openrouter', value: false },
                  },
                  vision_enabled: {
                     type: 'checkbox',
                     label: 'Enable Vision',
                     hint: 'Allow image analysis with vision-capable models',
                  },
                  openai_models: {
                     type: 'model_list',
                     label: 'OpenAI Models',
                     rows: 8, // LLM_CLOUD_MAX_MODELS
                     placeholder: 'gpt-4o\ngpt-4-turbo\ngpt-4o-mini',
                     hint: 'Available models for quick controls (one per line)',
                     showWhen: { key: 'llm.cloud.use_openrouter', value: false },
                  },
                  openai_default_model_idx: {
                     type: 'model_default_select',
                     label: 'Default OpenAI Model',
                     sourceKey: 'llm.cloud.openai_models',
                     hint: 'Default model for new conversations',
                     showWhen: { key: 'llm.cloud.use_openrouter', value: false },
                  },
                  openai_use_responses_api: {
                     type: 'select',
                     label: 'OpenAI Responses API',
                     options: ['auto', 'always', 'never'],
                     hint: 'Route reasoning-capable OpenAI models to /v1/responses. "auto" is recommended.',
                     showWhen: [
                        { key: 'llm.cloud.provider', value: 'openai' },
                        { key: 'llm.cloud.use_openrouter', value: false },
                     ],
                  },
                  claude_models: {
                     type: 'model_list',
                     label: 'Claude Models',
                     rows: 8, // LLM_CLOUD_MAX_MODELS
                     placeholder: 'claude-sonnet-4-20250514\nclaude-opus-4-20250514',
                     hint: 'Available models for quick controls (one per line)',
                     showWhen: { key: 'llm.cloud.use_openrouter', value: false },
                  },
                  claude_default_model_idx: {
                     type: 'model_default_select',
                     label: 'Default Claude Model',
                     sourceKey: 'llm.cloud.claude_models',
                     hint: 'Default model for new conversations',
                     showWhen: { key: 'llm.cloud.use_openrouter', value: false },
                  },
                  gemini_models: {
                     type: 'model_list',
                     label: 'Gemini Models',
                     rows: 8, // LLM_CLOUD_MAX_MODELS
                     placeholder: 'gemini-2.5-flash\ngemini-2.5-pro\ngemini-3-flash-preview',
                     hint: 'Available models for quick controls (one per line). 2.5+ and 3.x support reasoning.',
                     showWhen: { key: 'llm.cloud.use_openrouter', value: false },
                  },
                  gemini_default_model_idx: {
                     type: 'model_default_select',
                     label: 'Default Gemini Model',
                     sourceKey: 'llm.cloud.gemini_models',
                     hint: 'Default model for new conversations',
                     showWhen: { key: 'llm.cloud.use_openrouter', value: false },
                  },
                  openrouter_models: {
                     type: 'model_list',
                     label: 'OpenRouter Models',
                     rows: 8, // LLM_CLOUD_MAX_MODELS
                     placeholder:
                        'anthropic/claude-sonnet-4\nopenai/gpt-5.4\ngoogle/gemini-2.5-flash',
                     hint: 'Curated shortlist shown in the header model switcher. One OpenRouter model ID per line, "vendor/model" (e.g. anthropic/claude-sonnet-4).',
                     showWhen: { key: 'llm.cloud.use_openrouter', value: true },
                  },
                  openrouter_default_model_idx: {
                     type: 'model_default_select',
                     label: 'Default OpenRouter Model',
                     sourceKey: 'llm.cloud.openrouter_models',
                     hint: 'Default model for new conversations',
                     showWhen: { key: 'llm.cloud.use_openrouter', value: true },
                  },
               },
            },
            local: {
               type: 'group',
               label: 'Local Settings',
               fields: {
                  endpoint: {
                     type: 'text',
                     label: 'Endpoint',
                     hint: 'llama-server URL (e.g., http://127.0.0.1:8080)',
                  },
                  model: {
                     type: 'text',
                     label: 'Model',
                     placeholder: 'Leave empty for server default',
                     hint: 'Model name if server hosts multiple models',
                  },
                  vision_enabled: {
                     type: 'checkbox',
                     label: 'Enable Vision',
                     hint: 'Enable for multimodal models like LLaVA',
                  },
               },
            },
            thinking: {
               type: 'group',
               label: 'Extended Thinking',
               advanced: true,
               fields: {
                  mode: {
                     type: 'select',
                     label: 'Mode',
                     options: ['disabled', 'enabled'],
                     hint: 'Enable extended thinking for complex queries',
                  },
                  reasoning_effort: {
                     type: 'select',
                     label: 'Reasoning Effort',
                     options: ['none', 'low', 'medium', 'high', 'xhigh'],
                     hint: 'Effort level (gpt-5.4: none/low/medium/high/xhigh; Claude maps low/medium/high to budgets below)',
                  },
                  budget_low: {
                     type: 'number',
                     label: 'Budget Low',
                     min: 1024,
                     step: 256,
                     hint: 'Token budget for "low" reasoning effort (default: 1024, minimum: 1024)',
                  },
                  budget_medium: {
                     type: 'number',
                     label: 'Budget Medium',
                     min: 1024,
                     step: 512,
                     hint: 'Token budget for "medium" reasoning effort (default: 8192, minimum: 1024)',
                  },
                  budget_high: {
                     type: 'number',
                     label: 'Budget High',
                     min: 1024,
                     step: 1024,
                     hint: 'Token budget for "high" reasoning effort (default: 16384)',
                  },
                  budget_xhigh: {
                     type: 'number',
                     label: 'Budget XHigh',
                     min: 1024,
                     step: 1024,
                     hint: 'Token budget for "xhigh" reasoning effort (Claude only — gpt-5.2/gpt-5.4 use the API\'s xhigh natively; older OpenAI/Gemini get clamped to high). Default: 32768.',
                  },
               },
            },
            compact_soft_threshold: {
               type: 'number',
               label: 'Background Compaction Threshold',
               min: 0.3,
               max: 0.9,
               step: 0.05,
               hint: 'Trigger invisible background compaction at this fraction of context (default: 0.60)',
               advanced: true,
            },
            compact_hard_threshold: {
               type: 'number',
               label: 'Blocking Compaction Threshold',
               min: 0.5,
               max: 0.95,
               step: 0.05,
               hint: 'Force blocking compaction at this fraction of context — safety net (default: 0.85)',
               advanced: true,
            },
            compact_use_session: {
               type: 'checkbox',
               label: 'Use Session Provider for Compaction',
               hint: 'When checked, compaction uses the same LLM as the conversation. Uncheck to use a dedicated provider.',
               advanced: true,
            },
            compact_provider: {
               type: 'select',
               label: 'Compaction Provider',
               options: ['claude', 'openai', 'gemini', 'local'],
               hint: 'Provider to use for context compaction summaries',
               advanced: true,
               showWhen: [
                  { key: 'llm.compact_use_session', value: false },
                  { key: 'llm.cloud.use_openrouter', value: false },
               ],
            },
            compact_model: {
               type: 'text',
               label: 'Compaction Model',
               hint:
                  'Model name for compaction (e.g., claude-haiku-4-5 — the same ' +
                  'tier validated for memory extraction). Leave empty for provider default.',
               advanced: true,
               showWhen: [
                  { key: 'llm.compact_use_session', value: false },
                  { key: 'llm.cloud.use_openrouter', value: false },
               ],
            },
            compact_openrouter_model: {
               type: 'model_source_select',
               sourceKey: 'llm.cloud.openrouter_models',
               label: 'Compaction Model (OpenRouter)',
               hint:
                  'OpenRouter model for compaction when the gateway is on. Chosen from your ' +
                  'OpenRouter Models list (Language Model settings). "(Use default)" = the main ' +
                  'OpenRouter default model.',
               advanced: true,
               showWhen: [
                  { key: 'llm.compact_use_session', value: false },
                  { key: 'llm.cloud.use_openrouter', value: true },
               ],
            },
            conversation_logging: {
               type: 'checkbox',
               label: 'Conversation File Logging',
               hint: 'Save chat history to log files on disk (debug use — WebUI saves to DB regardless)',
               advanced: true,
            },
            rate_limit_enabled: {
               type: 'checkbox',
               label: 'API Rate Limiting',
               hint: 'Throttle outbound API calls to avoid provider rate limits (429 errors)',
            },
            rate_limit_rpm: {
               type: 'number',
               label: 'Max Requests/Min',
               min: 1,
               max: 128,
               hint: 'Maximum cloud LLM API calls per minute (default: 40)',
            },
            llm_timeout_ms: {
               type: 'number',
               label: 'LLM Request Timeout',
               min: 5000,
               hint: 'Per-request timeout in seconds (default: 60)',
               advanced: true,
               configPath: 'network.llm_timeout_ms',
               multiplier: 1000,
            },
            summarization_timeout_ms: {
               type: 'number',
               label: 'Summarization Timeout',
               min: 10000,
               hint: 'Context compaction timeout in seconds (default: 180)',
               advanced: true,
               configPath: 'network.summarization_timeout_ms',
               multiplier: 1000,
            },
         },
      },
      tool_calling: {
         label: 'Tool Calling',
         icon: '&#x1F527;',
         adminOnly: true,
         fields: {
            mode: {
               type: 'select',
               label: 'Mode',
               options: [
                  { value: 'native', label: 'Native Tools' },
                  { value: 'command_tags', label: 'Command Tags (Legacy)' },
                  { value: 'disabled', label: 'Disabled' },
               ],
               hint: 'Native Tools: LLM function calling, Command Tags: XML-style <command> tags, Disabled: no tool use',
               configPath: 'llm.tools.mode',
            },
         },
         customContent: 'tools_list', // Special marker for injecting tools list
      },
      search: {
         label: 'Web Search',
         icon: '&#x1F50D;',
         adminOnly: true,
         advanced: true,
         fields: {
            engine: {
               type: 'select',
               label: 'Engine',
               options: ['searxng', 'tavily', 'disabled'],
               hint: 'SearXNG (local/free, privacy-focused) or Tavily (commercial API, more reliable; requires tavily_api_key in secrets.toml)',
            },
            endpoint: {
               type: 'text',
               label: 'SearXNG Endpoint',
               hint: 'SearXNG instance URL (e.g., http://localhost:8888)',
               showWhen: { key: 'search.engine', value: 'searxng' },
            },
            summarizer: {
               type: 'group',
               label: 'Result Summarizer',
               fields: {
                  backend: {
                     type: 'select',
                     label: 'Backend',
                     options: ['disabled', 'local', 'default', 'tfidf'],
                     hint: 'disabled: pass-through, local: local LLM, default: active LLM, tfidf: fast extractive',
                  },
                  threshold_bytes: {
                     type: 'number',
                     label: 'Threshold (bytes)',
                     min: 0,
                     step: 512,
                     hint: 'Summarize results larger than this (0 = always summarize)',
                  },
                  target_words: {
                     type: 'number',
                     label: 'Target Words',
                     min: 50,
                     step: 50,
                     hint: 'Target word count for LLM summarization',
                  },
                  target_ratio: {
                     type: 'number',
                     label: 'TF-IDF Ratio',
                     min: 0.1,
                     max: 1.0,
                     step: 0.05,
                     hint: 'Ratio of sentences to keep for TF-IDF (0.2 = 20%)',
                  },
               },
            },
            title_filters: {
               type: 'string_list',
               label: 'Title Filters',
               rows: 8, // SEARCH_MAX_TITLE_FILTERS=16, but 8 rows is reasonable visible height
               placeholder: 'wordle\nconnections hints\nnyt connections',
               hint: 'Exclude results containing these terms in title (one per line, case-insensitive)',
            },
         },
      },
      url_fetcher: {
         label: 'URL Fetcher',
         icon: '&#x1F310;',
         adminOnly: true,
         advanced: true,
         fields: {
            whitelist: {
               type: 'string_list',
               label: 'URL Whitelist',
               rows: 4,
               placeholder: 'api.example.com\ninternal.corp.com',
               hint: 'Only allow fetching from these domains (one per line, empty = allow all)',
            },
            fallback: {
               type: 'select',
               label: 'Bypass engine (when blocked)',
               options: ['flaresolverr', 'tavily', 'none'],
               hint: 'Direct fetch is always tried first (free, fast). This engine kicks in when the site returns 403 / 401 / bot challenge / JS-only content. FlareSolverr is local + free (needs the Docker service running). Tavily is a commercial API (requires tavily_api_key in secrets.toml) but works without a separate service.',
            },
            flaresolverr: {
               type: 'group',
               label: 'FlareSolverr',
               showWhen: { key: 'url_fetcher.fallback', value: 'flaresolverr' },
               fields: {
                  enabled: {
                     type: 'checkbox',
                     label: 'Enable FlareSolverr',
                     hint: 'Required when bypass engine = flaresolverr. Service must be running at the endpoint below.',
                  },
                  endpoint: {
                     type: 'text',
                     label: 'Endpoint',
                     hint: 'FlareSolverr API URL (e.g., http://localhost:8191/v1)',
                  },
                  timeout_sec: {
                     type: 'number',
                     label: 'Timeout (sec)',
                     min: 1,
                     max: 120,
                     hint: 'Request timeout for FlareSolverr',
                  },
                  max_response_bytes: {
                     type: 'number',
                     label: 'Max Response (bytes)',
                     min: 1024,
                     step: 1024,
                     hint: 'Maximum response size to accept',
                  },
               },
            },
            tavily: {
               type: 'group',
               label: 'Tavily',
               showWhen: { key: 'url_fetcher.fallback', value: 'tavily' },
               fields: {
                  extract_depth: {
                     type: 'select',
                     label: 'Extract Depth',
                     options: ['advanced', 'basic'],
                     hint: '"advanced" (2 credits/call) runs smart article-body extraction and strips nav/sidebar chrome — recommended. "basic" (1 credit/call) returns faithful page text including all navigation menus.',
                  },
                  timeout_sec: {
                     type: 'number',
                     label: 'Timeout (sec)',
                     min: 1,
                     max: 120,
                     hint: 'Request timeout for Tavily /extract',
                  },
                  max_response_bytes: {
                     type: 'number',
                     label: 'Max Response (bytes)',
                     min: 1024,
                     step: 1024,
                     hint: 'Maximum raw_content size to keep from Tavily',
                  },
                  rate_limit_per_minute: {
                     type: 'number',
                     label: 'Rate Limit per Minute (per user)',
                     min: 1,
                     max: 10000,
                     hint: "Caps any one user's tool-loop burst. Default 10. Both /search and /extract count against this bucket.",
                  },
                  rate_limit_per_hour: {
                     type: 'number',
                     label: 'Rate Limit per Hour (per user)',
                     min: 1,
                     max: 100000,
                     hint: 'Sustained per-user ceiling. Default 100.',
                  },
                  rate_limit_per_day: {
                     type: 'number',
                     label: 'Global Daily Ceiling (all users)',
                     min: 1,
                     max: 1000000,
                     hint: "Monthly-quota backstop across the entire deployment. Default 500 — sized for Tavily's 1000/mo free tier with cushion. Raise if you're on a paid plan.",
                  },
               },
            },
         },
      },
      memory: {
         label: 'Memory System',
         icon: '&#x1F9E0;',
         adminOnly: true,
         fields: {
            enabled: {
               type: 'checkbox',
               label: 'Enable Memory',
               hint: 'Store and recall facts, preferences, and conversation summaries per user',
            },
            context_budget_tokens: {
               type: 'number',
               label: 'Context Budget (tokens)',
               min: 100,
               max: 2000,
               step: 100,
               hint: 'Max tokens for memory context injected into system prompt (~800 recommended)',
               advanced: true,
            },
            extraction_provider: {
               type: 'select',
               label: 'Extraction Provider',
               options: ['local', 'openai', 'claude'],
               hint: 'LLM provider for extracting facts from conversations',
               id: 'memory-extraction-provider',
               advanced: true,
               showWhen: { key: 'llm.cloud.use_openrouter', value: false },
            },
            extraction_model: {
               type: 'dynamic_select',
               label: 'Extraction Model',
               hint:
                  'Model for memory extraction (populated based on provider). ' +
                  'Haiku-tier validated as the sweet spot — larger Claude models do ' +
                  'not produce better extraction (see benchmarks/README.md).',
               dynamicKey: 'memory_extraction_models',
               id: 'memory-extraction-model',
               advanced: true,
               showWhen: { key: 'llm.cloud.use_openrouter', value: false },
            },
            extraction_openrouter_model: {
               type: 'model_source_select',
               sourceKey: 'llm.cloud.openrouter_models',
               label: 'Extraction Model (OpenRouter)',
               hint:
                  'OpenRouter model for memory extraction when the gateway is on. Chosen from your ' +
                  'OpenRouter Models list (Language Model settings). "(Use default)" = the main ' +
                  'OpenRouter default model.',
               advanced: true,
               showWhen: { key: 'llm.cloud.use_openrouter', value: true },
            },
            extraction_timeout_ms: {
               type: 'number',
               label: 'Extraction Timeout (ms)',
               min: 10000,
               max: 600000,
               step: 1000,
               hint: 'LLM timeout for memory fact extraction (default 120s)',
               advanced: true,
            },
            silent_observe_provider: {
               type: 'select',
               label: 'Silent-Observe Provider',
               options: ['local', 'ollama', 'openai', 'claude', 'anthropic', 'gemini'],
               hint:
                  'LLM provider for the Silent-Observe primitive (Phase 0 of ' +
                  'Dynamic Context Injection). Background observations are non-streaming, ' +
                  'no tools, no TTS. Defaults to local for cost.',
               configPath: 'llm.silent_observe.provider',
               advanced: true,
               showWhen: { key: 'llm.cloud.use_openrouter', value: false },
            },
            silent_observe_model: {
               type: 'text',
               label: 'Silent-Observe Model',
               hint:
                  'Model name for silent observations. Empty = let provider pick. ' +
                  'A small/fast model is fine.',
               configPath: 'llm.silent_observe.model',
               advanced: true,
               showWhen: { key: 'llm.cloud.use_openrouter', value: false },
            },
            silent_observe_openrouter_model: {
               type: 'model_source_select',
               sourceKey: 'llm.cloud.openrouter_models',
               label: 'Silent-Observe Model (OpenRouter)',
               hint:
                  'OpenRouter model for silent observations when the gateway is on. Chosen from ' +
                  'your OpenRouter Models list (Language Model settings). "(Use default)" = the ' +
                  'main OpenRouter default model.',
               configPath: 'llm.silent_observe.openrouter_model',
               advanced: true,
               showWhen: { key: 'llm.cloud.use_openrouter', value: true },
            },
            note_extraction_guard: {
               type: 'checkbox',
               label: 'Guard Filed Notes from Memory',
               hint: 'Keep reference text filed via save_note/save_text out of extracted memory facts so it lives only in the note store',
               advanced: true,
            },
            pruning_enabled: {
               type: 'checkbox',
               label: 'Enable Pruning',
               hint: 'Automatically remove old superseded and stale facts',
               advanced: true,
            },
            prune_superseded_days: {
               type: 'number',
               label: 'Superseded Retention (days)',
               min: 1,
               max: 365,
               hint: 'Keep superseded facts for this many days before deletion',
               advanced: true,
            },
            prune_stale_days: {
               type: 'number',
               label: 'Stale Threshold (days)',
               min: 7,
               max: 730,
               hint: 'Delete low-confidence facts not accessed in this many days',
               advanced: true,
            },
            prune_stale_min_confidence: {
               type: 'number',
               label: 'Stale Min Confidence',
               min: 0,
               max: 1,
               step: 0.1,
               hint: 'Only prune stale facts with confidence below this value (0-1)',
               advanced: true,
            },
            expire_enabled: {
               type: 'checkbox',
               label: 'Enable Fact Expiry',
               hint: 'Auto-expire transient dated facts (weather forecasts, scheduled-not-yet-happened events). Leave off until validated on your data.',
               advanced: true,
            },
            expire_grace_days: {
               type: 'number',
               label: 'Expiry Grace (days)',
               min: 0,
               max: 365,
               hint: 'Keep an expiring fact visible this many days past its reference date',
               advanced: true,
            },
            prune_expired_days: {
               type: 'number',
               label: 'Expired Retention (days)',
               min: 0,
               max: 365,
               hint: 'Recoverable buffer before an expired fact is hard-deleted (0 = delete on the reference date)',
               advanced: true,
            },
            conversation_idle_timeout_min: {
               type: 'range',
               label: 'Voice Conversation Idle Timeout',
               min: 0,
               max: 60,
               step: 5,
               hint: 'Minutes of idle time before auto-saving voice conversations (0 = disabled, 10+ to enable)',
               ariaLabel: 'Voice conversation idle timeout in minutes',
               displayValue: (val) => (val === 0 ? 'Disabled' : `${val} minutes`),
               advanced: true,
            },
            default_voice_user_id: {
               type: 'dynamic_select',
               label: 'Default Voice User',
               dynamicKey: 'users',
               hint: 'User account for local microphone and DAP voice conversations',
               advanced: true,
            },
            decay_enabled: {
               type: 'checkbox',
               label: 'Enable Confidence Decay',
               hint: 'Nightly decay reduces confidence of unused memories over time',
               advanced: true,
            },
            decay_hour: {
               type: 'number',
               label: 'Decay Hour (0-23)',
               min: 0,
               max: 23,
               hint: 'Hour of day to run nightly decay (default: 2 AM)',
               advanced: true,
            },
            decay_inferred_weekly: {
               type: 'number',
               label: 'Inferred Decay Rate',
               min: 0.5,
               max: 1,
               step: 0.01,
               hint: 'Weekly confidence multiplier for inferred facts (0.95 = 5% decay/week)',
               advanced: true,
            },
            decay_explicit_weekly: {
               type: 'number',
               label: 'Explicit Decay Rate',
               min: 0.5,
               max: 1,
               step: 0.01,
               hint: 'Weekly confidence multiplier for explicit facts (0.98 = 2% decay/week)',
               advanced: true,
            },
            decay_preference_weekly: {
               type: 'number',
               label: 'Preference Decay Rate',
               min: 0.5,
               max: 1,
               step: 0.01,
               hint: 'Weekly confidence multiplier for preferences (0.97 = 3% decay/week)',
               advanced: true,
            },
            decay_inferred_floor: {
               type: 'number',
               label: 'Inferred Confidence Floor',
               min: 0,
               max: 1,
               step: 0.05,
               hint: 'Minimum confidence for inferred facts after decay (0 = can decay to zero)',
               advanced: true,
            },
            decay_explicit_floor: {
               type: 'number',
               label: 'Explicit Confidence Floor',
               min: 0,
               max: 1,
               step: 0.05,
               hint: 'Minimum confidence for explicit facts after decay',
               advanced: true,
            },
            decay_preference_floor: {
               type: 'number',
               label: 'Preference Confidence Floor',
               min: 0,
               max: 1,
               step: 0.05,
               hint: 'Minimum confidence for preferences after decay',
               advanced: true,
            },
            decay_prune_threshold: {
               type: 'number',
               label: 'Prune Threshold',
               min: 0,
               max: 0.5,
               step: 0.05,
               hint: 'Delete facts with confidence below this value after decay',
               advanced: true,
            },
            summary_retention_days: {
               type: 'number',
               label: 'Summary Retention (days)',
               min: 7,
               max: 365,
               hint: 'Delete conversation summaries older than this',
               advanced: true,
            },
            access_reinforcement_boost: {
               type: 'number',
               label: 'Access Reinforcement Boost',
               min: 0,
               max: 0.5,
               step: 0.01,
               hint: 'Confidence boost when a fact is accessed (time-gated to once per hour)',
               advanced: true,
            },
            embedding_settings: {
               type: 'group',
               label: 'Semantic Search',
               description:
                  'Embedding-based semantic memory search. ONNX runs locally with no external service needed.',
               fields: {
                  embedding_provider: {
                     type: 'select',
                     label: 'Embedding Provider',
                     hint: 'ONNX runs locally (~23MB model). Ollama/OpenAI require a running service.',
                     configPath: 'memory.embedding_provider',
                     options: [
                        { value: 'onnx', label: 'ONNX (Built-in)' },
                        { value: 'ollama', label: 'Ollama' },
                        { value: 'openai', label: 'OpenAI-compatible' },
                        { value: '', label: 'Disabled' },
                     ],
                  },
                  embedding_model: {
                     type: 'text',
                     label: 'Model',
                     hint: 'Model name for the embedding provider (e.g., "all-minilm" for Ollama)',
                     placeholder: 'all-minilm',
                     configPath: 'memory.embedding_model',
                     advanced: true,
                     showWhen: { key: 'memory.embedding_provider', notValue: ['onnx', ''] },
                  },
                  embedding_endpoint: {
                     type: 'text',
                     label: 'Endpoint URL',
                     hint: 'Base URL for the embedding service',
                     placeholder: 'http://localhost:11434',
                     configPath: 'memory.embedding_endpoint',
                     advanced: true,
                     showWhen: { key: 'memory.embedding_provider', notValue: ['onnx', ''] },
                  },
                  embedding_keyword_weight: {
                     type: 'range',
                     label: 'Keyword Match Strength',
                     hint: 'How much exact word matching influences search results (independent of semantic strength)',
                     configPath: 'memory.embedding_keyword_weight',
                     min: 0,
                     max: 1,
                     step: 0.05,
                     displayValue: (v) => Math.round(v * 100) + '%',
                  },
                  embedding_vector_weight: {
                     type: 'range',
                     label: 'Semantic Match Strength',
                     hint: 'How much meaning-based similarity influences search results (independent of keyword strength)',
                     configPath: 'memory.embedding_vector_weight',
                     min: 0,
                     max: 1,
                     step: 0.05,
                     displayValue: (v) => Math.round(v * 100) + '%',
                  },
                  temporal_weight: {
                     type: 'range',
                     label: 'Temporal Match Strength',
                     hint: 'Boost when the query mentions a time ("last week", "in September", "in 2023") and the memory has a matching date. 0 disables. Default 0.20.',
                     configPath: 'memory.temporal_weight',
                     min: 0,
                     max: 1,
                     step: 0.05,
                     displayValue: (v) => Math.round(v * 100) + '%',
                     advanced: true,
                  },
                  search_score_floor: {
                     type: 'range',
                     label: 'Memory Search Score Floor',
                     hint: 'Drops marginal-cosine hits from memory-tool search results before they reach the LLM. 0 disables. Default 0.30 (the natural keyword-weight boundary — preserves single-token keyword matches, filters hybrid-vector-only marginal hits). Drop to 0.10-0.20 if over-filtering, push higher if weak fabrications persist.',
                     configPath: 'memory.search_score_floor',
                     min: 0,
                     max: 1,
                     step: 0.05,
                     displayValue: (v) => Math.round(v * 100) + '%',
                     advanced: true,
                  },
                  bm25_enabled: {
                     type: 'checkbox',
                     label: 'BM25 Keyword Ranking (Experimental)',
                     hint: 'Use BM25 ranking with stemming for the keyword half of memory search. Matches plurals and tense variants ("memories" finds "memory", "attending" finds "attend"), and ranks rare words above common ones. Off-by-default while it bench-validates against the existing path.',
                     configPath: 'memory.bm25_enabled',
                     advanced: true,
                  },
                  graph_use_query_scoring: {
                     type: 'checkbox',
                     label: 'Query-Aware Graph Scoring',
                     hint: 'Re-score entity-graph candidates against the query using cosine similarity, instead of a flat entity bonus. ON (default): graph candidates compete with hybrid hits on the same semantic-relevance scale. OFF: legacy flat-bonus scoring.',
                     configPath: 'memory.graph_retrieval.use_query_scoring',
                     advanced: true,
                  },
                  graph_entity_bonus: {
                     type: 'range',
                     label: 'Entity Bonus (Query-Scored Graph)',
                     hint: 'Additive bonus on top of cosine for query-scored graph candidates. Default 0 (recommended) — values >0 apply asymmetrically (only graph-branch facts get the lift, displacing hybrid candidates), which regressed cat-3 in testing. Experimental knob; leave at 0 unless a symmetric-application design is in place.',
                     configPath: 'memory.graph_retrieval.entity_bonus',
                     min: 0,
                     max: 1,
                     step: 0.05,
                     displayValue: (v) => v.toFixed(2),
                     advanced: true,
                  },
                  category_threshold: {
                     type: 'range',
                     label: 'Category Classification Threshold',
                     hint: 'Cosine similarity threshold for auto-classifying memories into categories. Calibrated for MiniLM-L6 at 0.25. Higher = more selective.',
                     configPath: 'memory.category_threshold',
                     min: 0.05,
                     max: 0.95,
                     step: 0.05,
                     displayValue: (v) => v.toFixed(2),
                     advanced: true,
                  },
                  embedding_backfill_on_startup: {
                     type: 'checkbox',
                     label: 'Index Existing Memories',
                     hint: 'Generate search embeddings for memories stored before semantic search was enabled. Runs once at startup.',
                     configPath: 'memory.embedding_backfill_on_startup',
                     advanced: true,
                  },
                  model_id: {
                     type: 'text',
                     label: 'Embedding Model ID',
                     hint: 'Identity string for the current embedding model. Bump this whenever the model file changes to trigger automatic re-indexing of stored embeddings.',
                     placeholder: 'bge-small-en-v1.5-int8',
                     configPath: 'memory.model_id',
                     advanced: true,
                  },
                  recompute_on_model_change: {
                     type: 'checkbox',
                     label: 'Re-index on Model Change',
                     hint: 'Automatically re-embed all stored memories when the embedding model ID changes. Disable only as an emergency ops escape hatch.',
                     configPath: 'memory.recompute_on_model_change',
                     advanced: true,
                  },
                  recompute_batch_size: {
                     type: 'number',
                     label: 'Re-index Batch Size',
                     hint: 'Number of memories to re-embed per batch during model change re-indexing. Default 50.',
                     min: 1,
                     max: 500,
                     step: 1,
                     configPath: 'memory.recompute_batch_size',
                     advanced: true,
                  },
                  recompute_batch_sleep_ms: {
                     type: 'number',
                     label: 'Re-index Batch Sleep (ms)',
                     hint: 'Milliseconds to sleep between re-indexing batches. Throttles CPU usage during background re-indexing. Default 100.',
                     min: 0,
                     max: 5000,
                     step: 10,
                     configPath: 'memory.recompute_batch_sleep_ms',
                     advanced: true,
                  },
               },
            },
            recovery_settings: {
               type: 'group',
               label: 'Extraction Recovery',
               description:
                  'Retries memory extraction on conversations left unconsolidated by daemon crashes or extraction failures. Runs once at startup and on the configured recurring interval.',
               fields: {
                  recovery_enabled: {
                     type: 'checkbox',
                     label: 'Enable Recovery',
                     hint: "Retry memory extraction on conversations the daemon couldn't consolidate (e.g., after a crash).",
                     configPath: 'memory.recovery_enabled',
                     advanced: true,
                  },
                  recovery_idle_threshold_seconds: {
                     type: 'number',
                     label: 'Idle Threshold (minutes)',
                     min: 60,
                     max: 30 * 86400,
                     step: 60,
                     multiplier: 60,
                     hint: 'Minimum minutes since the last conversation activity before a conversation is considered stuck. Default 60 (1 hour).',
                     configPath: 'memory.recovery_idle_threshold_seconds',
                     advanced: true,
                  },
                  recovery_max_attempts: {
                     type: 'number',
                     label: 'Max Attempts',
                     min: 0,
                     max: 20,
                     hint: 'Stop retrying after this many failed extraction attempts on the same conversation. 0 = unlimited. Default 2.',
                     configPath: 'memory.recovery_max_attempts',
                     advanced: true,
                  },
                  recovery_recurring_interval_seconds: {
                     type: 'number',
                     label: 'Recurring Interval (days)',
                     min: 0,
                     max: 30 * 86400,
                     step: 86400,
                     multiplier: 86400,
                     hint: 'How often (after the startup pass) to scan for stuck conversations. 0 = startup-only. Default 1 (daily).',
                     configPath: 'memory.recovery_recurring_interval_seconds',
                     advanced: true,
                  },
               },
            },
            focus_injection_settings: {
               type: 'group',
               label: 'Per-Turn Context Injection',
               description:
                  'Pulls memory, document, calendar, and email content above a relevance threshold and injects it into each user turn. Disabled by default until source adapters ship in 1c/1d.',
               fields: {
                  focus_enabled: {
                     type: 'checkbox',
                     label: 'Enable per-turn context injection',
                     hint: 'Master switch for the focus-injection framework. No effect until adapters register.',
                     configPath: 'memory.focus_injection.enabled',
                  },
                  focus_budget_bytes: {
                     type: 'number',
                     label: 'Focus block byte budget',
                     min: 1024,
                     max: 65536,
                     step: 256,
                     hint: 'Maximum bytes of injected context per turn (≈ 4 bytes/token). Default 10240.',
                     configPath: 'memory.focus_injection.focus_budget_bytes',
                  },
                  focus_top_k: {
                     type: 'number',
                     label: 'Top-K candidates',
                     min: 1,
                     max: 64,
                     hint: 'How many ranked candidates to retain after scoring. Default 8.',
                     configPath: 'memory.focus_injection.top_k',
                  },
                  focus_min_score: {
                     type: 'range',
                     label: 'Minimum relevance score',
                     min: 0,
                     max: 1,
                     step: 0.05,
                     hint: 'Drop candidates below this final score. Default 0.40.',
                     configPath: 'memory.focus_injection.min_score',
                     displayValue: (v) => v.toFixed(2),
                  },
                  focus_weight_semantic: {
                     type: 'range',
                     label: 'Weight: semantic similarity',
                     min: 0,
                     max: 5,
                     step: 0.05,
                     hint: 'Ranker weight for embedding cosine similarity. Default 1.00.',
                     configPath: 'memory.focus_injection.weight_semantic',
                     displayValue: (v) => v.toFixed(2),
                     advanced: true,
                  },
                  focus_weight_recency: {
                     type: 'range',
                     label: 'Weight: recency',
                     min: 0,
                     max: 5,
                     step: 0.05,
                     hint: 'Ranker weight for time-decayed recency score. Default 0.30.',
                     configPath: 'memory.focus_injection.weight_recency',
                     displayValue: (v) => v.toFixed(2),
                     advanced: true,
                  },
                  focus_weight_importance: {
                     type: 'range',
                     label: 'Weight: importance',
                     min: 0,
                     max: 5,
                     step: 0.05,
                     hint: 'Ranker weight for source-specific importance (confidence, explicit-flag). Default 0.20.',
                     configPath: 'memory.focus_injection.weight_importance',
                     displayValue: (v) => v.toFixed(2),
                     advanced: true,
                  },
                  focus_weight_source: {
                     type: 'range',
                     label: 'Weight: per-source',
                     min: 0,
                     max: 5,
                     step: 0.05,
                     hint: 'Multiplier applied to the per-source weights below. Default 1.00.',
                     configPath: 'memory.focus_injection.weight_source',
                     displayValue: (v) => v.toFixed(2),
                     advanced: true,
                  },
                  focus_classifier_enabled: {
                     type: 'checkbox',
                     label: 'Enable source classifier',
                     hint: 'RAGRoute-style classifier picks a subset of sources per query. Off by default; opt-in only after probe validates.',
                     configPath: 'memory.focus_injection.classifier_enabled',
                     advanced: true,
                  },
                  focus_dedup_recent_window_turns: {
                     type: 'number',
                     label: 'Dedup window (turns)',
                     min: 0,
                     max: 32,
                     hint: 'A previously-injected item is suppressed for this many turns before it can re-surface. 0 = time-based dedup disabled (uplift only).',
                     configPath: 'memory.focus_injection.dedup.recent_window_turns',
                     advanced: true,
                  },
                  focus_dedup_score_uplift_factor: {
                     type: 'range',
                     label: 'Re-inject uplift factor',
                     min: 1.0,
                     max: 3.0,
                     step: 0.05,
                     hint: 'Previously-injected item must score at least N× its prior score to re-surface within the dedup window.',
                     configPath: 'memory.focus_injection.dedup.score_uplift_factor',
                     displayValue: (v) => v.toFixed(2),
                     advanced: true,
                  },
                  focus_dominant_token_enabled: {
                     type: 'checkbox',
                     label: 'Dominant-token heuristic',
                     hint: 'Structural fix for cosine over-inclusion when the query has a low-IDF dominant token (e.g. "favorite restaurant"), most candidates share it, and the right answer uses specific vocabulary instead. Downweights candidates whose only query-overlap is via dominant tokens.',
                     configPath: 'memory.focus_injection.dominant_token_heuristic.enabled',
                     advanced: true,
                  },
                  focus_dominant_token_threshold: {
                     type: 'range',
                     label: 'Dominance threshold',
                     min: 0.3,
                     max: 0.9,
                     step: 0.05,
                     hint: 'Fraction of the candidate pool a query token must appear in to be flagged as "dominant". Lower = more aggressive detection. Bench-validated default 0.60.',
                     configPath: 'memory.focus_injection.dominant_token_heuristic.threshold',
                     displayValue: (v) => v.toFixed(2),
                     advanced: true,
                  },
                  focus_dominant_token_base_penalty: {
                     type: 'range',
                     label: 'Dominant-token penalty',
                     min: 0.1,
                     max: 0.7,
                     step: 0.05,
                     hint: 'Maximum composite-score penalty applied to a candidate whose only query-overlap is via dominant tokens. Higher = stronger suppression. Bench-robust across 0.30-0.50.',
                     configPath: 'memory.focus_injection.dominant_token_heuristic.base_penalty',
                     displayValue: (v) => v.toFixed(2),
                     advanced: true,
                  },
               },
            },
            entity_merge_settings: {
               type: 'group',
               label: 'Entity Merge (Phase 2)',
               fields: {
                  entity_merge_enabled: {
                     type: 'checkbox',
                     label: 'Auto-evaluate entity merges',
                     hint: 'When enabled, every freshly-extracted entity is scored against existing canonicals.  Strong matches soft-link automatically; mid-confidence matches surface in the Memory → Graph "Suggested merges" panel for your approval.  Operator-driven merges (link-user-self, manual merge button) work regardless of this setting.',
                     configPath: 'memory.entity_merge.enabled',
                  },
                  entity_merge_auto_threshold: {
                     type: 'range',
                     label: 'Auto-merge threshold',
                     min: 0.7,
                     max: 1.0,
                     step: 0.01,
                     hint: 'Composite score at or above which Phase 2 silently soft-links the new entity to an existing canonical.  Keep conservative — auto-merges land with no human approval.',
                     configPath: 'memory.entity_merge.auto_threshold',
                     displayValue: (v) => v.toFixed(2),
                     advanced: true,
                  },
                  entity_merge_review_threshold: {
                     type: 'range',
                     label: 'Review threshold',
                     min: 0.2,
                     max: 0.9,
                     step: 0.01,
                     hint: 'Composite score at or above which Phase 2 queues a proposal for operator approval via the Suggested-Merges panel.  Lower = catches more duplicates but more clicks to triage.  Defaults to 0.50 — calibrate against your corpus.',
                     configPath: 'memory.entity_merge.review_threshold',
                     displayValue: (v) => v.toFixed(2),
                     advanced: true,
                  },
               },
            },
         },
      },
      mqtt: {
         label: 'MQTT',
         icon: '&#x1F4E1;',
         advanced: true,
         fields: {
            enabled: {
               type: 'checkbox',
               label: 'Enable MQTT',
               hint: 'Connect to MQTT broker for smart home control',
            },
            broker: {
               type: 'text',
               label: 'Broker Address',
               hint: 'MQTT broker hostname or IP address',
            },
            port: {
               type: 'number',
               label: 'Port',
               min: 1,
               max: 65535,
               hint: 'MQTT broker port (default: 1883, TLS: 8883)',
            },
            tls_group: {
               type: 'group',
               label: 'TLS Encryption',
               fields: {
                  tls: {
                     type: 'checkbox',
                     label: 'Enable TLS',
                     restart: true,
                     hint: 'Encrypt MQTT connection with TLS/SSL',
                     configPath: 'mqtt.tls',
                  },
                  tls_ca_cert: {
                     type: 'text',
                     label: 'CA Certificate Path',
                     hint: 'Path to CA certificate file (leave empty for system default)',
                     configPath: 'mqtt.tls_ca_cert',
                  },
                  tls_cert_path: {
                     type: 'text',
                     label: 'Client Certificate Path',
                     hint: 'Path to client certificate for mutual TLS (optional)',
                     configPath: 'mqtt.tls_cert_path',
                  },
                  tls_key_path: {
                     type: 'text',
                     label: 'Client Key Path',
                     hint: 'Path to client private key for mutual TLS (optional)',
                     configPath: 'mqtt.tls_key_path',
                  },
               },
            },
         },
      },
      webui: {
         label: 'Server',
         icon: '&#x1F310;',
         fields: {
            enabled: {
               type: 'checkbox',
               label: 'Enable Server',
               hint: 'HTTP/WebSocket server for browser and satellite clients',
            },
            port: {
               type: 'number',
               label: 'Port',
               min: 1,
               max: 65535,
               restart: true,
               hint: 'HTTP/WebSocket port (default: 3000)',
            },
            max_clients: {
               type: 'number',
               label: 'Max Clients',
               min: 1,
               restart: true,
               hint: 'Maximum concurrent browser connections',
               advanced: true,
            },
            bind_address: {
               type: 'dynamic_select',
               label: 'Bind Address',
               restart: true,
               hint: 'Network interface to listen on',
               dynamicKey: 'bind_addresses',
               advanced: true,
            },
            https: {
               type: 'checkbox',
               label: 'Enable HTTPS',
               restart: true,
               hint: 'Required for microphone access on remote connections',
               advanced: true,
            },
            ssl_cert_path: {
               type: 'text',
               label: 'SSL Certificate Path',
               restart: true,
               hint: 'Path to PEM certificate file for HTTPS',
               advanced: true,
               showWhen: { key: 'webui.https', value: true },
            },
            ssl_key_path: {
               type: 'text',
               label: 'SSL Key Path',
               restart: true,
               hint: 'Path to PEM private key file for HTTPS',
               advanced: true,
               showWhen: { key: 'webui.https', value: true },
            },
            audio_chunk_ms: {
               type: 'number',
               label: 'Audio Chunk Size (ms)',
               min: 10,
               max: 500,
               step: 10,
               hint: 'WebSocket audio chunk duration — lower = less VAD latency (default: 100)',
               advanced: true,
            },
            export_max_messages: {
               type: 'number',
               label: 'Export Message Limit',
               min: 0,
               hint: 'Max messages per conversation export (0 = unlimited)',
               advanced: true,
            },
            export_format: {
               type: 'select',
               label: 'Export Format',
               options: ['json', 'html'],
               hint: 'Default format for conversation exports',
               advanced: true,
            },
            workers: {
               type: 'number',
               label: 'ASR Workers',
               min: 1,
               max: 8,
               restart: true,
               hint: 'Concurrent voice processing threads — each loads an ASR model copy (default: 2)',
               advanced: true,
               configPath: 'network.workers',
            },
            session_timeout_sec: {
               type: 'number',
               label: 'Session Timeout',
               min: 60,
               hint: 'Seconds of inactivity before session context is cleared (default: 1800 / 30 min)',
               advanced: true,
               configPath: 'network.session_timeout_sec',
            },
         },
      },
      shutdown: {
         label: 'Shutdown',
         icon: '&#x1F512;',
         advanced: true,
         fields: {
            enabled: {
               type: 'checkbox',
               label: 'Enable Voice Shutdown',
               hint: 'Allow system shutdown via voice command (disabled by default for security)',
            },
            passphrase: {
               type: 'text',
               label: 'Passphrase',
               hint: 'Secret phrase required to authorize shutdown (leave empty for no passphrase)',
            },
         },
      },
      debug: {
         label: 'Debug',
         icon: '&#x1F41B;',
         advanced: true,
         fields: {
            mic_record: { type: 'checkbox', label: 'Record Microphone' },
            asr_record: { type: 'checkbox', label: 'Record ASR Input' },
            aec_record: { type: 'checkbox', label: 'Record AEC' },
            record_path: { type: 'text', label: 'Recording Path' },
         },
      },
      tui: {
         label: 'Terminal UI',
         icon: '&#x1F5A5;',
         advanced: true,
         fields: {
            enabled: {
               type: 'checkbox',
               label: 'Enable TUI',
               restart: true,
               hint: 'Show terminal dashboard with real-time metrics',
            },
         },
      },
      paths: {
         label: 'Paths',
         icon: '&#x1F4C1;',
         advanced: true,
         fields: {
            data_dir: {
               type: 'text',
               label: 'Data Directory',
               restart: true,
               hint: 'Directory for databases (auth.db, music.db). Supports ~ for home. Default: ~/.local/share/dawn',
            },
            music_dir: {
               type: 'text',
               label: 'Music Directory',
               hint: 'Path to music library for playback commands',
            },
         },
      },
      images: {
         label: 'Images & Vision',
         icon: '&#x1F5BC;',
         adminOnly: true,
         description: 'Image upload limits, vision settings, and stored image retention',
         fields: {
            max_image_size_kb: {
               type: 'number',
               label: 'Max Image Size (KB)',
               min: 512,
               max: 16384,
               step: 512,
               hint: 'Maximum image upload size in kilobytes (512-16384)',
               configPath: 'vision.max_image_size_kb',
            },
            max_dimension: {
               type: 'number',
               label: 'Max Image Dimension (px)',
               min: 256,
               max: 4096,
               step: 256,
               hint: 'Maximum image width/height in pixels (256-4096)',
               configPath: 'vision.max_dimension',
            },
            max_images: {
               type: 'number',
               label: 'Max Images Per Message',
               min: 1,
               max: 10,
               hint: 'Maximum images per message (1-10)',
               configPath: 'vision.max_images',
            },
            storage: {
               type: 'group',
               label: 'Storage',
               advanced: true,
               fields: {
                  retention_days: {
                     type: 'number',
                     label: 'Retention Period (days)',
                     min: 0,
                     hint: 'Auto-delete images after this many days (0 = keep forever)',
                     configPath: 'images.retention_days',
                  },
                  max_size_mb: {
                     type: 'number',
                     label: 'Max Stored Image Size (MB)',
                     min: 1,
                     max: 50,
                     hint: 'Maximum size per stored image on disk (1-50 MB)',
                     configPath: 'images.max_size_mb',
                  },
                  max_per_user: {
                     type: 'number',
                     label: 'Max Stored Per User',
                     min: 1,
                     max: 10000,
                     hint: 'Maximum stored images per user account (1-10,000)',
                     configPath: 'images.max_per_user',
                  },
               },
            },
         },
      },
      documents: {
         label: 'Documents',
         icon: '&#x1F4C4;',
         adminOnly: true,
         description: 'Document upload, extraction, and RAG indexing limits',
         fields: {
            max_file_size_kb: {
               type: 'number',
               label: 'Max Upload Size (KB)',
               min: 64,
               max: 10240,
               step: 64,
               hint: 'Maximum document upload size in kilobytes (64-10240)',
            },
            max_documents: {
               type: 'number',
               label: 'Max Documents Per Message',
               min: 1,
               max: 20,
               hint: 'Maximum documents that can be attached to a single message (1-20)',
            },
            max_pages: {
               type: 'number',
               label: 'Max PDF Pages',
               min: 1,
               max: 500,
               hint: 'Maximum pages to extract from PDF documents (1-500)',
            },
            max_extracted_size_kb: {
               type: 'number',
               label: 'Max Extracted Text (KB)',
               min: 128,
               max: 4096,
               advanced: true,
               hint: 'Maximum size of extracted text content in kilobytes (128-4096)',
            },
            max_index_size_kb: {
               type: 'number',
               label: 'Max Document Size for Indexing (KB)',
               min: 128,
               max: 10240,
               step: 128,
               advanced: true,
               hint: 'Maximum document text size for RAG indexing in kilobytes (128-10240)',
            },
            max_indexed_documents: {
               type: 'number',
               label: 'Max Indexed Documents Per User',
               min: 1,
               max: 500,
               advanced: true,
               hint: 'Maximum documents a user can index for RAG search (1-500)',
            },
            fts_label_weight: {
               type: 'number',
               label: 'Label Match Weight',
               min: 0,
               max: 100,
               step: 0.5,
               advanced: true,
               hint: 'Hybrid search: BM25 weight for a note label / filename match (default: 3.0). Raise if exact-label lookups under-rank.',
            },
            fts_body_weight: {
               type: 'number',
               label: 'Body Match Weight',
               min: 0,
               max: 100,
               step: 0.5,
               advanced: true,
               hint: 'Hybrid search: BM25 weight for a body-text match (default: 1.0)',
            },
            hybrid_keyword_weight: {
               type: 'number',
               label: 'Keyword Channel Weight',
               min: 0,
               max: 1,
               step: 0.05,
               advanced: true,
               hint: 'Hybrid search: lexical (BM25) share of the fused score (default: 0.3)',
            },
            hybrid_vector_weight: {
               type: 'number',
               label: 'Semantic Channel Weight',
               min: 0,
               max: 1,
               step: 0.05,
               advanced: true,
               hint: 'Hybrid search: semantic (embedding) share of the fused score (default: 0.7)',
            },
            phrase_bonus_weight: {
               type: 'number',
               label: 'Phrase Bonus Weight',
               min: 0,
               max: 1,
               step: 0.05,
               advanced: true,
               hint: 'Hybrid search: extra credit when query words match in order / contiguously (default: 0.25)',
            },
            search_min_score: {
               type: 'number',
               label: 'Relevance Threshold',
               min: 0,
               max: 1,
               step: 0.05,
               advanced: true,
               hint: 'Hybrid search: drop results scoring below this after fusion (default: 0.3). Raise to suppress weak matches.',
            },
            version_retention_days: {
               type: 'number',
               label: 'Version History (days)',
               min: 0,
               max: 3650,
               advanced: true,
               hint: 'How long to keep the pre-change snapshot of an edited/overwritten/deleted note or document, for undo/restore (default: 14; 0 = disable versioning).',
            },
            version_keep_per_doc: {
               type: 'number',
               label: 'Versions Kept Per Item',
               min: 1,
               max: 1000,
               advanced: true,
               hint: 'Maximum saved versions to retain per note/document (the older of this and the day limit wins; default: 10).',
            },
         },
      },
      music: {
         label: 'Music Streaming',
         icon: '&#x1F3B5;',
         adminOnly: true,
         description:
            'Music sources are auto-detected. Local library uses the Music Directory from Paths. Plex is active when host and token are configured.',
         fields: {
            scan_interval_minutes: {
               type: 'number',
               label: 'Library Rescan Interval (minutes)',
               min: 0,
               hint: 'Minutes between automatic library rescans (0 = disabled)',
            },
            plex: {
               type: 'group',
               label: 'Plex Connection',
               description:
                  'Plex authentication token must also be set in the Secrets section below.',
               fields: {
                  host: {
                     type: 'text',
                     label: 'Host',
                     placeholder: 'e.g., 192.168.1.100',
                     hint: 'Plex server hostname or IP address',
                  },
                  port: {
                     type: 'number',
                     label: 'Port',
                     min: 1,
                     max: 65535,
                     hint: 'Plex server port (default: 32400)',
                  },
                  music_section_id: {
                     type: 'number',
                     label: 'Music Section ID',
                     min: 0,
                     hint: 'Library section ID for music (0 = auto-discover)',
                  },
                  ssl: {
                     type: 'checkbox',
                     label: 'Use HTTPS',
                     hint: 'Connect to Plex over HTTPS',
                  },
                  ssl_verify: {
                     type: 'checkbox',
                     label: 'Verify SSL Certificate',
                     hint: 'Verify Plex server SSL certificate (disable for self-signed)',
                  },
               },
            },
            streaming: {
               type: 'group',
               label: 'Streaming',
               advanced: true,
               fields: {
                  enabled: {
                     type: 'checkbox',
                     label: 'Enable Music Streaming',
                     hint: 'Allow streaming music to WebUI browsers',
                  },
                  default_quality: {
                     type: 'select',
                     label: 'Default Quality',
                     options: [
                        { value: 'voice', label: 'Voice (48 kbps)' },
                        { value: 'standard', label: 'Standard (96 kbps)' },
                        { value: 'high', label: 'High (128 kbps)' },
                        { value: 'hifi', label: 'Hi-Fi (256 kbps)' },
                     ],
                     hint: 'Default audio quality for music streaming',
                  },
                  bitrate_mode: {
                     type: 'select',
                     label: 'Bitrate Mode',
                     options: [
                        { value: 'vbr', label: 'VBR (Variable)' },
                        { value: 'cbr', label: 'CBR (Constant)' },
                     ],
                     hint: 'VBR optimizes quality, CBR ensures consistent bandwidth',
                  },
               },
            },
         },
      },
      scheduler: {
         label: 'Scheduler',
         icon: '&#x23F0;',
         fields: {
            enabled: {
               type: 'checkbox',
               label: 'Enable Scheduler',
               hint: 'Enable timers, alarms, reminders, and scheduled tasks',
            },
            default_snooze_minutes: {
               type: 'number',
               label: 'Default Snooze (minutes)',
               min: 1,
               max: 60,
               hint: 'Default snooze duration when user says "snooze"',
               advanced: true,
            },
            max_snooze_count: {
               type: 'number',
               label: 'Max Snooze Count',
               min: 1,
               max: 20,
               hint: 'Auto-cancel after this many snoozes',
               advanced: true,
            },
            alarm_timeout_sec: {
               type: 'number',
               label: 'Alarm Timeout (seconds)',
               min: 10,
               max: 300,
               hint: 'Stop ringing after this duration',
               advanced: true,
            },
            alarm_volume: {
               type: 'number',
               label: 'Alarm Volume',
               min: 0,
               max: 100,
               hint: 'Alarm sound volume (0-100)',
               advanced: true,
            },
            max_events_per_user: {
               type: 'number',
               label: 'Max Events Per User',
               min: 1,
               max: 1000,
               hint: 'Limit active events per user',
               advanced: true,
            },
            max_events_total: {
               type: 'number',
               label: 'Max Events Total',
               min: 1,
               max: 10000,
               hint: 'Hard cap across all users',
               advanced: true,
            },
            missed_event_recovery: {
               type: 'checkbox',
               label: 'Missed Event Recovery',
               hint: 'Handle missed events after daemon restart',
               advanced: true,
            },
            missed_task_policy: {
               type: 'select',
               label: 'Missed Task Policy',
               options: ['skip', 'execute'],
               hint: 'What to do with missed scheduled tasks after restart',
               advanced: true,
            },
            missed_task_max_age_sec: {
               type: 'number',
               label: 'Missed Task Max Age (seconds)',
               min: 60,
               max: 86400,
               hint: 'Skip missed tasks older than this even if policy=execute',
               advanced: true,
            },
            event_retention_days: {
               type: 'number',
               label: 'Event Retention (days)',
               min: 1,
               max: 365,
               hint: 'Clean up completed/cancelled events after this many days',
               advanced: true,
            },
            briefing_speak_aloud_on_webui_source: {
               type: 'checkbox',
               label: 'Speak WebUI-Created Briefings Aloud',
               hint: 'Voice-created briefings always speak.  Briefings created via WebUI text are silent by default — enable this to speak them too.',
               advanced: true,
            },
         },
      },
      calendar: {
         label: 'Calendar',
         icon: '&#x1F4C5;',
         fields: {
            enabled: {
               type: 'checkbox',
               label: 'Enable Calendar',
               hint: 'Enable CalDAV calendar integration',
               default: false,
            },
            sync_interval_sec: {
               type: 'number',
               label: 'Sync Interval (seconds)',
               min: 60,
               max: 86400,
               default: 900,
               hint: 'Background sync interval (default: 900 = 15 min)',
            },
            cache_past_days: {
               type: 'number',
               label: 'Cache Past Days',
               min: 0,
               max: 365,
               default: 30,
               hint: 'Days of past events to keep cached',
               advanced: true,
            },
            cache_future_days: {
               type: 'number',
               label: 'Cache Future Days',
               min: 7,
               max: 730,
               default: 365,
               hint: 'Days of future events to cache',
               advanced: true,
            },
            default_event_duration_min: {
               type: 'number',
               label: 'Default Event Duration (minutes)',
               min: 5,
               max: 480,
               default: 60,
               hint: 'Duration when no end time is specified',
               advanced: true,
            },
         },
      },
      /* Home Assistant config is managed by its dedicated admin panel section */

      mcp: {
         label: 'MCP Bridge',
         icon: '&#x1F50C;',
         fields: {
            enabled: {
               type: 'checkbox',
               label: 'Enable MCP Bridge',
               hint: 'Connect to operator-launched MCP servers and expose their tools (requires restart). Servers are configured in dawn.toml.',
               restart: true,
            },
            dev_mode: {
               type: 'checkbox',
               label: 'Allow Insecure TLS (dev mode)',
               hint: 'Permit MCP servers with tls_verify = false. Leave off in production.',
               advanced: true,
            },
         },
      },

      code_projects: {
         label: 'Code Projects',
         icon: '&#x1F4BB;',
         fields: {
            enabled: {
               type: 'checkbox',
               label: 'Enable Code Projects',
               hint: 'Import GitHub-style repos and index them via the MCP bridge for code Q&A (requires the MCP bridge).',
            },
            source_root: {
               type: 'text',
               label: 'Source Root',
               hint: 'Filesystem directory where imported repos are cloned.',
               advanced: true,
            },
            import_user_required: {
               type: 'select',
               label: 'Who Can Import',
               options: [
                  { value: 'admin', label: 'Admins only' },
                  { value: '', label: 'Any authenticated user' },
               ],
               hint: 'Restrict who may import new projects.',
            },
            default_global: {
               type: 'checkbox',
               label: 'Default New Projects to Global',
               hint: 'New imports are shared with all users instead of private to the importer.',
               advanced: true,
            },
            max_repo_size_mb: {
               type: 'number',
               label: 'Max Repo Size (MB)',
               min: 1,
               max: 1048576,
               hint: 'Reject clones whose working tree exceeds this size.',
               advanced: true,
            },
            max_file_count: {
               type: 'number',
               label: 'Max File Count',
               min: 1,
               max: 100000000,
               hint: 'Reject clones with more files than this.',
               advanced: true,
            },
            max_path_depth: {
               type: 'number',
               label: 'Max Path Depth',
               min: 1,
               max: 255,
               hint: 'Reject clones nesting deeper than this.',
               advanced: true,
            },
            clone_depth: {
               type: 'number',
               label: 'Clone Depth',
               min: 0,
               max: 1,
               hint: '0 = full history, 1 = shallow clone.',
               advanced: true,
            },
            allowed_host_pattern: {
               type: 'text',
               label: 'Allowed Host Pattern',
               hint: 'POSIX regex matched against the clone URL host (auto-anchored). Blank = allow any HTTPS host.',
               advanced: true,
            },
            default_active: {
               type: 'text',
               label: 'Default Active Project',
               hint: 'Fallback active project name for new sessions.',
               advanced: true,
            },
         },
      },
   };

   /**
    * Section categories for visual grouping (render order)
    * Sections not listed here will not be rendered.
    */
   const SECTION_CATEGORIES = [
      {
         id: 'identity',
         label: 'Identity & Basics',
         icon: '&#x2699;',
         sections: ['general', 'persona', 'localization'],
      },
      {
         id: 'voice',
         label: 'Voice & Audio',
         icon: '&#x1F50A;',
         sections: ['audio', 'vad', 'asr', 'tts'],
      },
      {
         id: 'ai',
         label: 'AI Engine',
         icon: '&#x1F916;',
         sections: ['llm', 'commands'],
      },
      {
         id: 'tools',
         label: 'Tools & Extensions',
         icon: '&#x1F527;',
         sections: ['tool_calling', 'search', 'url_fetcher'],
      },
      {
         id: 'coding',
         label: 'Coding',
         icon: '&#x1F4BB;',
         sections: ['mcp', 'code_projects'],
      },
      {
         id: 'memory',
         label: 'Memory',
         icon: '&#x1F9E0;',
         sections: ['memory'],
      },
      {
         id: 'media',
         label: 'Music & Media',
         icon: '&#x1F3B5;',
         sections: ['music', 'images', 'documents'],
      },
      {
         id: 'scheduling',
         label: 'Scheduling',
         icon: '&#x23F0;',
         sections: ['scheduler', 'calendar'],
      },
      {
         id: 'network',
         label: 'Network',
         icon: '&#x1F310;',
         sections: ['webui', 'mqtt'],
      },
      {
         id: 'system',
         label: 'System',
         icon: '&#x1F5A5;',
         sections: ['paths', 'shutdown', 'debug', 'tui'],
      },
   ];

   /**
    * Create a category header element
    * @param {Object} category - Category definition
    * @returns {HTMLElement} Category header element
    */
   function createCategoryHeader(category) {
      const header = document.createElement('div');
      header.className = 'settings-category-header';
      header.dataset.category = category.id;
      header.innerHTML =
         '<span class="category-icon">' +
         category.icon +
         '</span>' +
         '<span class="category-label">' +
         escapeHtml(category.label) +
         '</span>';
      return header;
   }

   /**
    * Escape HTML for display
    * Uses shared implementation from format.js
    * @param {string} str - String to escape
    * @returns {string} Escaped string
    */
   function escapeHtml(str) {
      return DawnFormat.escapeHtml(str);
   }

   /**
    * Render all settings sections into the container, grouped by category
    */
   function renderSettingsSections() {
      if (!sectionsContainer) return;

      const currentConfig = getCurrentConfigFn ? getCurrentConfigFn() : null;
      if (!currentConfig) return;

      sectionsContainer.innerHTML = '';
      showWhenDependents = {};

      const virtualConfig = { ...currentConfig };

      // Populate persona description with built-in default if not set
      const Config = window.DawnSettingsConfig;
      if (
         Config &&
         Config.getDefaultPersona &&
         (!virtualConfig.persona || !virtualConfig.persona.description)
      ) {
         if (!virtualConfig.persona) virtualConfig.persona = {};
         virtualConfig.persona.description = Config.getDefaultPersona();
      }

      // Render sections grouped by category
      for (const category of SECTION_CATEGORIES) {
         const headerEl = createCategoryHeader(category);
         let allAdminOnly = true;
         let allAdvanced = true;

         const sectionEls = [];
         for (const sectionKey of category.sections) {
            const sectionDef = SETTINGS_SCHEMA[sectionKey];
            if (!sectionDef) continue;

            const configSection = virtualConfig[sectionKey] || {};
            const sectionEl = createSettingsSection(sectionKey, sectionDef, configSection);
            sectionEl.dataset.category = category.id;
            sectionEls.push(sectionEl);

            if (!sectionDef.adminOnly) allAdminOnly = false;
            if (!sectionDef.advanced) allAdvanced = false;
         }

         // Mark category header if all its sections are admin-only or advanced
         if (allAdminOnly && sectionEls.length > 0) {
            headerEl.classList.add('admin-only');
         }
         if (allAdvanced && sectionEls.length > 0) {
            headerEl.classList.add('all-advanced');
         }

         sectionsContainer.appendChild(headerEl);
         for (const el of sectionEls) {
            sectionsContainer.appendChild(el);
         }
      }

      renderedSchemaVersion = SCHEMA_VERSION;
   }

   /**
    * Create a settings section element
    * @param {string} key - Section key
    * @param {Object} def - Section definition
    * @param {Object} configData - Config data for this section
    * @returns {HTMLElement} Section element
    */
   function createSettingsSection(key, def, configData) {
      const section = document.createElement('div');
      section.className = 'settings-section';
      section.dataset.section = key;

      // Add admin-only class if applicable
      if (def.adminOnly) {
         section.classList.add('admin-only');
      }

      // Add advanced-field class if section is marked advanced
      if (def.advanced) {
         section.classList.add('advanced-field');
      }

      // Header
      const header = document.createElement('h3');
      header.className = 'section-header';
      header.setAttribute('tabindex', '0'); // Make focusable for keyboard nav
      header.innerHTML = `
      <span class="section-icon">${def.icon || ''}</span>
      ${escapeHtml(def.label || '')}
      <span class="section-toggle">&#9660;</span>
    `;
      const toggleSection = () => {
         section.classList.toggle('collapsed');
         // Update aria-expanded for accessibility (M14)
         const isExpanded = !section.classList.contains('collapsed');
         header.setAttribute('aria-expanded', isExpanded);
      };
      header.addEventListener('click', toggleSection);
      header.addEventListener('keydown', (e) => {
         if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            toggleSection();
         }
      });

      // Start collapsed by default
      section.classList.add('collapsed');
      header.setAttribute('aria-expanded', 'false');

      // Content
      const content = document.createElement('div');
      content.className = 'section-content';

      // Add section description if present
      if (def.description) {
         const desc = document.createElement('p');
         desc.className = 'section-description';
         desc.textContent = def.description;
         content.appendChild(desc);
      }

      // Render fields
      for (const [fieldKey, fieldDef] of Object.entries(def.fields)) {
         const value = configData[fieldKey];
         const fieldEl = createSettingField(key, fieldKey, fieldDef, value);
         content.appendChild(fieldEl);
      }

      // Inject custom content if specified
      if (def.customContent === 'tools_list') {
         content.appendChild(createToolsListContent());
      }

      section.appendChild(header);
      section.appendChild(content);

      return section;
   }

   /**
    * Create the tools list content (previously hardcoded in HTML)
    * @returns {HTMLElement} Tools list container element
    */
   function createToolsListContent() {
      const container = document.createElement('div');
      container.className = 'tools-container';
      container.innerHTML = `
      <div class="tools-info">
        <p>Configure which tools are available for LLM function calling.</p>
        <div class="tools-token-estimate">
          <span>Estimated tokens:</span>
          <span id="tools-tokens-local">Local: --</span>
          <span id="tools-tokens-remote">Remote: --</span>
        </div>
      </div>
      <div class="tools-header-row">
        <span class="tools-header-name">Tool</span>
        <span class="tools-header-local">Local</span>
        <span class="tools-header-remote">Remote</span>
      </div>
      <div id="tools-list" class="tools-list">
        <div class="tools-loading">Loading tools...</div>
      </div>
      <p class="tools-save-hint">Tool changes saved with main configuration.</p>
    `;

      // Re-initialize tools module with new button after DOM insertion
      setTimeout(() => {
         if (typeof DawnTools !== 'undefined') {
            DawnTools.reinitButton();
            DawnTools.requestConfig();
         }
      }, 0);

      return container;
   }

   /**
    * Apply showWhen visibility to an element based on current config
    * @param {HTMLElement} el - Element to show/hide
    * @param {Object} condition - { key, value } condition
    */
   /**
    * Evaluate a single showWhen condition against a value-getter.
    * @param {Object} cond - { key, value } or { key, notValue }
    * @param {Function} getVal - (key) => current value
    * @returns {boolean} true if the condition is satisfied
    */
   function showWhenCondMatches(cond, getVal) {
      const v = getVal(cond.key);
      if (cond.notValue) {
         const blocked = Array.isArray(cond.notValue) ? cond.notValue : [cond.notValue];
         return !blocked.includes(v);
      }
      return v === cond.value;
   }

   /**
    * Read a config key's current LIVE value: prefer the rendered input's state
    * (so unsaved edits are honored), falling back to the loaded config. Used for
    * re-evaluating multi-condition (AND) showWhen when any dependency changes.
    */
   function getLiveConfigValue(key, changedKey, changedValue) {
      if (key === changedKey) return changedValue;
      // Resolve by data-key (which IS the config key / fullKey), not by reconstructing
      // the element id — element ids are "setting-<section>-<field>" and diverge from the
      // key path for any field that sets configPath, which would silently miss live edits.
      const inputEl = sectionsContainer
         ? sectionsContainer.querySelector('[data-key="' + key + '"]')
         : null;
      if (inputEl) {
         return inputEl.type === 'checkbox' ? inputEl.checked : inputEl.value;
      }
      const currentConfig = getCurrentConfigFn ? getCurrentConfigFn() : {};
      return Utils.getNestedValue(currentConfig, key);
   }

   function applyShowWhen(el, condition) {
      const currentConfig = getCurrentConfigFn ? getCurrentConfigFn() : {};
      const getVal = (k) => Utils.getNestedValue(currentConfig, k);
      // Multi-condition (AND): array of { key, value } — element visible iff all match.
      if (Array.isArray(condition)) {
         el.style.display = condition.every((c) => showWhenCondMatches(c, getVal)) ? '' : 'none';
         return;
      }
      if (getVal(condition.key) === undefined && condition.key) {
         console.warn('showWhen: config key not found:', condition.key);
      }
      el.style.display = showWhenCondMatches(condition, getVal) ? '' : 'none';
   }

   /**
    * Register an element in the showWhen dependency map
    * @param {string} key - The config key this element depends on
    * @param {HTMLElement} el - The element to show/hide
    */
   function registerShowWhenDependent(key, el) {
      if (!showWhenDependents[key]) {
         showWhenDependents[key] = [];
      }
      showWhenDependents[key].push(el);
   }

   /**
    * Update visibility of showWhen elements when a config key changes (O(1) lookup)
    * @param {string} changedKey - The config key that changed
    * @param {*} newValue - The new value (string or boolean from checkbox)
    */
   function updateShowWhenVisibility(changedKey, newValue) {
      const dependents = showWhenDependents[changedKey];
      if (!dependents) return;
      dependents.forEach((el) => {
         if (el.__showWhenAll) {
            // Multi-condition (AND): re-evaluate every sub-condition against live input
            // state (the changed key uses newValue; others are read from their inputs).
            const getVal = (k) => getLiveConfigValue(k, changedKey, newValue);
            el.style.display = el.__showWhenAll.every((c) => showWhenCondMatches(c, getVal))
               ? ''
               : 'none';
         } else if (el.dataset.showWhenNotValue) {
            const blocked = JSON.parse(el.dataset.showWhenNotValue);
            el.style.display = blocked.includes(newValue) ? 'none' : '';
         } else {
            // Compare with type coercion for booleans stored as strings in dataset
            el.style.display = String(newValue) === el.dataset.showWhenValue ? '' : 'none';
         }
      });
   }

   /**
    * Create a setting field element
    * @param {string} sectionKey - Section key
    * @param {string} fieldKey - Field key
    * @param {Object} def - Field definition
    * @param {*} value - Current value
    * @returns {HTMLElement} Field element
    */
   function createSettingField(sectionKey, fieldKey, def, value) {
      const fullKey = def.configPath || `${sectionKey}.${fieldKey}`;
      const currentConfig = getCurrentConfigFn ? getCurrentConfigFn() : {};
      const restartRequiredFields = getRestartRequiredFieldsFn ? getRestartRequiredFieldsFn() : [];
      const dynamicOptions = getDynamicOptionsFn ? getDynamicOptionsFn() : {};

      // Resolve value from configPath if field maps to a different config section
      if (def.configPath) {
         value = Utils.getNestedValue(currentConfig, def.configPath);
      }

      // Handle nested groups
      if (def.type === 'group') {
         const groupEl = document.createElement('div');
         groupEl.className = 'setting-group';
         if (def.advanced) {
            groupEl.classList.add('advanced-field');
         }
         // Apply showWhen to groups
         if (def.showWhen) {
            groupEl.dataset.showWhenKey = def.showWhen.key;
            if (def.showWhen.notValue) {
               groupEl.dataset.showWhenNotValue = JSON.stringify(
                  Array.isArray(def.showWhen.notValue)
                     ? def.showWhen.notValue
                     : [def.showWhen.notValue]
               );
            } else {
               groupEl.dataset.showWhenValue = def.showWhen.value;
            }
            applyShowWhen(groupEl, def.showWhen);
            registerShowWhenDependent(def.showWhen.key, groupEl);
         }
         groupEl.innerHTML = `<div class="group-label">${escapeHtml(def.label || '')}</div>`;

         // Add group description hint if present
         if (def.description) {
            const desc = document.createElement('p');
            desc.className = 'group-description';
            desc.style.cssText =
               'font-size: 0.8em; opacity: 0.7; margin: 0.25em 0 0.5em 0; padding: 0;';
            desc.textContent = def.description;
            groupEl.appendChild(desc);
         }

         for (const [subKey, subDef] of Object.entries(def.fields)) {
            const subValue = value ? value[subKey] : undefined;
            const fieldEl = createSettingField(
               `${sectionKey}.${fieldKey}`,
               subKey,
               subDef,
               subValue
            );
            groupEl.appendChild(fieldEl);
         }

         return groupEl;
      }

      const item = document.createElement('div');
      item.className = def.type === 'checkbox' ? 'setting-item setting-item-row' : 'setting-item';

      // Add advanced-field class if field is marked advanced
      if (def.advanced) {
         item.classList.add('advanced-field');
      }

      // Apply showWhen conditional visibility
      if (def.showWhen) {
         if (Array.isArray(def.showWhen)) {
            // Multi-condition (AND): store on the element and depend on every key.
            item.__showWhenAll = def.showWhen;
            applyShowWhen(item, def.showWhen);
            def.showWhen.forEach((c) => registerShowWhenDependent(c.key, item));
         } else {
            item.dataset.showWhenKey = def.showWhen.key;
            if (def.showWhen.notValue) {
               item.dataset.showWhenNotValue = JSON.stringify(
                  Array.isArray(def.showWhen.notValue)
                     ? def.showWhen.notValue
                     : [def.showWhen.notValue]
               );
            } else {
               item.dataset.showWhenValue = def.showWhen.value;
            }
            applyShowWhen(item, def.showWhen);
            registerShowWhenDependent(def.showWhen.key, item);
         }
      }

      // Add tooltip hint if defined
      if (def.hint) {
         item.title = def.hint;
      }

      const needsRestart = def.restart || restartRequiredFields.includes(fullKey);
      const restartBadge = needsRestart ? '<span class="restart-badge">restart</span>' : '';

      let inputHtml = '';
      const inputId = `setting-${sectionKey}-${fieldKey}`.replace(/\./g, '-');

      switch (def.type) {
         case 'text':
            inputHtml = `<input type="text" id="${inputId}" value="${Utils.escapeAttr(value || '')}" placeholder="${def.placeholder || ''}" data-key="${fullKey}">`;
            break;
         case 'number':
            {
               const m = def.multiplier || 1;
               const displayMin = def.min !== undefined ? def.min / m : undefined;
               const displayMax = def.max !== undefined ? def.max / m : undefined;
               const displayStep = def.step !== undefined ? def.step / m : undefined;
               const displayValue = value != null ? value / m : value;
               const numAttrs = [
                  displayMin !== undefined ? `min="${displayMin}"` : '',
                  displayMax !== undefined ? `max="${displayMax}"` : '',
                  displayStep !== undefined ? `step="${displayStep}"` : '',
                  m !== 1 ? `data-multiplier="${m}"` : '',
               ]
                  .filter(Boolean)
                  .join(' ');
               inputHtml = `<input type="number" id="${inputId}" value="${Utils.formatNumber(displayValue)}" ${numAttrs} data-key="${fullKey}">`;
            }
            break;
         case 'checkbox':
            inputHtml = `<input type="checkbox" id="${inputId}" ${value ? 'checked' : ''} data-key="${fullKey}">`;
            break;
         case 'select':
            const options = def.options
               .map((opt) => {
                  // Support both string options and {value, label} objects
                  const optValue = typeof opt === 'object' ? opt.value : opt;
                  const optLabel =
                     typeof opt === 'object' ? opt.label : opt === '' ? '(System default)' : opt;
                  return `<option value="${optValue}" ${value === optValue ? 'selected' : ''}>${optLabel}</option>`;
               })
               .join('');
            inputHtml = `<select id="${inputId}" data-key="${fullKey}">${options}</select>`;
            break;
         case 'dynamic_select':
            // Dynamic select that gets options from server (e.g., model lists, user lists)
            // Supports both plain strings and {value, label} objects
            const dynKey = def.dynamicKey;
            const dynOptions = dynamicOptions[dynKey] || [];
            const currentVal = value ?? '';

            // Find label for current value (for object options)
            let currentLabel = String(currentVal) || '(none)';
            dynOptions.forEach((opt) => {
               const optVal = typeof opt === 'object' ? opt.value : opt;
               if (String(optVal) === String(currentVal)) {
                  currentLabel = typeof opt === 'object' ? opt.label : opt;
               }
            });

            // Start with current value as first option
            let dynOptionsHtml = `<option value="${Utils.escapeAttr(currentVal)}" selected>${escapeHtml(currentLabel)}</option>`;

            // Add options from server if available
            dynOptions.forEach((opt) => {
               const optVal = typeof opt === 'object' ? opt.value : opt;
               const optLabel = typeof opt === 'object' ? opt.label : opt;
               if (String(optVal) !== String(currentVal)) {
                  dynOptionsHtml += `<option value="${Utils.escapeAttr(optVal)}">${escapeHtml(optLabel)}</option>`;
               }
            });

            inputHtml = `<select id="${inputId}" data-key="${fullKey}" data-dynamic-key="${dynKey}">${dynOptionsHtml}</select>`;
            break;
         case 'textarea':
            inputHtml = `<textarea id="${inputId}" rows="${def.rows || 3}" placeholder="${def.placeholder || ''}" data-key="${fullKey}">${escapeHtml(value || '')}</textarea>`;
            break;
         case 'model_list':
            // Convert array to newline-separated string for editing
            const listValue = Array.isArray(value) ? value.join('\n') : value || '';
            inputHtml = `<textarea id="${inputId}" rows="${def.rows || 5}" placeholder="${def.placeholder || ''}" data-key="${fullKey}" data-type="model_list">${escapeHtml(listValue)}</textarea>`;
            break;
         case 'string_list':
            // Generic string array - same as model_list but without dropdown update behavior
            const strListValue = Array.isArray(value) ? value.join('\n') : value || '';
            inputHtml = `<textarea id="${inputId}" rows="${def.rows || 4}" placeholder="${def.placeholder || ''}" data-key="${fullKey}" data-type="string_list">${escapeHtml(strListValue)}</textarea>`;
            break;
         case 'model_default_select':
            // Dropdown populated from a model_list textarea
            // Get current options from the source model list
            const sourceKey = def.sourceKey;
            const sourceModels = Utils.getNestedValue(currentConfig, sourceKey) || [];
            const selectedIdx = parseInt(value, 10) || 0;
            let defaultSelectHtml = sourceModels
               .map(
                  (model, idx) =>
                     `<option value="${idx}" ${idx === selectedIdx ? 'selected' : ''}>${escapeHtml(model)}</option>`
               )
               .join('');
            if (sourceModels.length === 0) {
               defaultSelectHtml = '<option value="0">No models configured</option>';
            }
            inputHtml = `<select id="${inputId}" data-key="${fullKey}" data-type="model_default_select" data-source-key="${sourceKey}">${defaultSelectHtml}</select>`;
            break;
         case 'model_source_select': {
            // String-valued dropdown sourced from a config array (e.g. openrouter_models).
            // Unlike model_default_select (which stores an index), this stores the selected
            // model string. Includes a "(Use default)" empty option and preserves a current
            // value that isn't in the list.
            const srcModels = Utils.getNestedValue(currentConfig, def.sourceKey) || [];
            const curVal = value || '';
            const emptyLabel = def.emptyLabel || '(Use default)';
            let optsHtml = `<option value="" ${curVal === '' ? 'selected' : ''}>${escapeHtml(emptyLabel)}</option>`;
            let inList = false;
            srcModels.forEach((m) => {
               const sel = String(m) === String(curVal);
               if (sel) inList = true;
               optsHtml += `<option value="${Utils.escapeAttr(m)}" ${sel ? 'selected' : ''}>${escapeHtml(m)}</option>`;
            });
            if (curVal !== '' && !inList) {
               optsHtml += `<option value="${Utils.escapeAttr(curVal)}" selected>${escapeHtml(curVal)} (custom)</option>`;
            }
            // data-source-key opts this select into updateDependentModelSelects so it
            // live-refreshes when the source list changes; data-type tells that function
            // to rebuild with STRING values (not indices like model_default_select).
            inputHtml = `<select id="${inputId}" data-key="${fullKey}" data-type="model_source_select" data-source-key="${def.sourceKey}">${optsHtml}</select>`;
            break;
         }
         case 'range':
            // Range slider with value display
            const rangeAttrs = [
               def.min !== undefined ? `min="${def.min}"` : '',
               def.max !== undefined ? `max="${def.max}"` : '',
               def.step !== undefined ? `step="${def.step}"` : '',
               def.ariaLabel ? `aria-label="${Utils.escapeAttr(def.ariaLabel)}"` : '',
            ]
               .filter(Boolean)
               .join(' ');
            const currentRangeValue = value !== undefined ? value : def.min || 0;
            const displayValue = def.displayValue
               ? def.displayValue(currentRangeValue)
               : currentRangeValue;
            inputHtml = `
               <div class="range-input-wrapper">
                  <input type="range" id="${inputId}" value="${currentRangeValue}" ${rangeAttrs} data-key="${fullKey}" aria-valuemin="${def.min || 0}" aria-valuemax="${def.max || 100}" aria-valuenow="${currentRangeValue}">
                  <span class="range-value" id="${inputId}-value">${escapeHtml(String(displayValue))}</span>
               </div>`;
            break;
         default:
            // Unhandled type — fail loudly so a typo'd or unrenderable field
            // can't silently ship as a label-only orphan with no input.
            console.error(
               `settings/schema: unhandled field type "${def.type}" for key "${fullKey}". ` +
                  `Field will not render a control.`
            );
            inputHtml = `<span class="settings-error">unhandled type: ${escapeHtml(String(def.type))}</span>`;
      }

      if (def.type === 'checkbox') {
         item.innerHTML = `
        ${inputHtml}
        <label for="${inputId}">${def.label} ${restartBadge}</label>
      `;
      } else {
         item.innerHTML = `
        <label for="${inputId}">${def.label} ${restartBadge}</label>
        ${inputHtml}
      `;
      }

      // Add change listener
      const input = item.querySelector('input, select, textarea');
      if (input && handleSettingChangeCallback) {
         input.addEventListener('change', () => {
            handleSettingChangeCallback(fullKey, input);
            // Update showWhen visibility for dependent fields
            const newVal = input.type === 'checkbox' ? input.checked : input.value;
            updateShowWhenVisibility(fullKey, newVal);
         });
         // Debounce the input handler — change event already fires on blur/commit
         // Range inputs get an immediate handler (cheap, needed for live showWhen updates)
         if (input.type === 'range') {
            input.addEventListener('input', () => {
               handleSettingChangeCallback(fullKey, input);
               updateShowWhenVisibility(fullKey, input.value);
            });
         } else {
            const debouncedInput = Utils.debounce(() => {
               handleSettingChangeCallback(fullKey, input);
               const newVal = input.type === 'checkbox' ? input.checked : input.value;
               updateShowWhenVisibility(fullKey, newVal);
            }, 150);
            input.addEventListener('input', debouncedInput);
         }
      }

      // Special handling for range inputs - update display value
      if (def.type === 'range' && input) {
         // Stash displayValue callback for updateSettingsValues() fast path
         if (def.displayValue) input._displayValue = def.displayValue;
         const valueDisplay = item.querySelector('.range-value');
         if (valueDisplay) {
            input.addEventListener('input', () => {
               const newValue = parseFloat(input.value);
               const displayText = def.displayValue ? def.displayValue(newValue) : newValue;
               valueDisplay.textContent = String(displayText);
               // Update ARIA attributes
               input.setAttribute('aria-valuenow', newValue);
            });
         }
      }

      return item;
   }

   /**
    * Update model_default_select dropdowns when their source model_list changes
    * @param {string} sourceKey - Source field key
    * @param {string} textareaValue - Current textarea value
    */
   function updateDependentModelSelects(sourceKey, textareaValue) {
      if (!sectionsContainer) return;

      // Parse models from textarea (same logic as collectConfigValues)
      const models = textareaValue
         .split('\n')
         .map((line) => line.trim())
         .filter((line) => line.length > 0);

      // Find all selects that depend on this source
      const selects = sectionsContainer.querySelectorAll(`select[data-source-key="${sourceKey}"]`);

      selects.forEach((select) => {
         // String-valued source select (aux OpenRouter models): rebuild with string
         // options, an empty "(Use default)" entry, and preserve the current selection
         // (kept as a "(custom)" option if it's no longer in the list).
         if (select.dataset.type === 'model_source_select') {
            const cur = select.value || '';
            let inList = false;
            let html = `<option value="" ${cur === '' ? 'selected' : ''}>(Use default)</option>`;
            models.forEach((m) => {
               const sel = m === cur;
               if (sel) inList = true;
               html += `<option value="${Utils.escapeAttr(m)}" ${sel ? 'selected' : ''}>${escapeHtml(m)}</option>`;
            });
            if (cur !== '' && !inList) {
               html += `<option value="${Utils.escapeAttr(cur)}" selected>${escapeHtml(cur)} (custom)</option>`;
            }
            select.innerHTML = html;
            return;
         }

         const currentIdx = parseInt(select.value, 10) || 0;
         // Preserve selection if still valid, otherwise reset to 0
         const newIdx = currentIdx < models.length ? currentIdx : 0;

         // Rebuild options
         select.innerHTML =
            models.length > 0
               ? models
                    .map(
                       (model, idx) =>
                          `<option value="${idx}" ${idx === newIdx ? 'selected' : ''}>${escapeHtml(model)}</option>`
                    )
                    .join('')
               : '<option value="0">No models configured</option>';

         // Mark as changed if index changed
         if (newIdx !== currentIdx && handleSettingChangeCallback) {
            // Just mark as changed via the callback system
            const Config = window.DawnSettingsConfig;
            if (Config && Config.markFieldChanged) {
               Config.markFieldChanged(select.dataset.key);
            }
         }
      });
   }

   /**
    * Check if the settings DOM needs a full rebuild
    * @returns {boolean} True if DOM needs rebuilding
    */
   function needsRebuild() {
      return (
         renderedSchemaVersion !== SCHEMA_VERSION ||
         !sectionsContainer ||
         !sectionsContainer.hasChildNodes()
      );
   }

   /**
    * Update existing settings DOM values in-place without rebuilding.
    * Walks all [data-key] inputs and updates their values from config.
    * @param {Object} config - Current config object
    */
   function updateSettingsValues(config) {
      if (!sectionsContainer || !config) return;

      // Populate persona description with built-in default if not set
      const Config = window.DawnSettingsConfig;
      const virtualConfig = { ...config };
      if (
         Config &&
         Config.getDefaultPersona &&
         (!virtualConfig.persona || !virtualConfig.persona.description)
      ) {
         if (!virtualConfig.persona) virtualConfig.persona = {};
         virtualConfig.persona.description = Config.getDefaultPersona();
      }

      const inputs = sectionsContainer.querySelectorAll('[data-key]');
      inputs.forEach((input) => {
         const key = input.dataset.key;
         const value = Utils.getNestedValue(virtualConfig, key);

         if (input.type === 'checkbox') {
            input.checked = !!value;
         } else if (input.dataset.type === 'model_list' || input.dataset.type === 'string_list') {
            input.value = Array.isArray(value) ? value.join('\n') : value || '';
         } else if (input.dataset.type === 'model_default_select') {
            input.value = parseInt(value, 10) || 0;
         } else if (input.type === 'number' || input.type === 'range') {
            // Skip if config has no value — preserve the schema default from initial render
            if (value == null) return;
            const m = parseFloat(input.dataset.multiplier) || 1;
            const displayValue = value / m;
            input.value = input.type === 'number' ? Utils.formatNumber(displayValue) : displayValue;
            // Update range display value (use stashed displayValue callback if available)
            if (input.type === 'range') {
               const valueDisplay = input.parentElement?.querySelector('.range-value');
               if (valueDisplay) {
                  const formatted = input._displayValue
                     ? input._displayValue(displayValue)
                     : displayValue;
                  valueDisplay.textContent = String(formatted);
               }
               input.setAttribute('aria-valuenow', displayValue);
            }
         } else {
            input.value = value ?? '';
         }
      });

      // Re-apply showWhen visibility based on updated values
      for (const key in showWhenDependents) {
         const val = Utils.getNestedValue(virtualConfig, key);
         if (val !== undefined) {
            updateShowWhenVisibility(key, val);
         }
      }
   }

   /**
    * Get the settings schema
    * @returns {Object} Settings schema
    */
   function getSchema() {
      return SETTINGS_SCHEMA;
   }

   // Export for settings module
   window.DawnSettingsSchema = {
      setDependencies,
      getSchema,
      renderSettingsSections,
      updateSettingsValues,
      needsRebuild,
      createSettingsSection,
      createSettingField,
      updateDependentModelSelects,
      updateShowWhenVisibility,
      createToolsListContent,
   };
})();
