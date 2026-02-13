/**
 * DAWN Settings - Schema and Rendering
 * Settings schema definition and section/field rendering
 */
(function () {
   'use strict';

   const Utils = window.DawnSettingsUtils;

   // Dependencies (injected by main settings module)
   let sectionsContainer = null;
   let handleSettingChangeCallback = null;
   let getCurrentConfigFn = null;
   let getRestartRequiredFieldsFn = null;
   let getDynamicOptionsFn = null;

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
         label: 'Persona',
         icon: '&#x1F464;',
         fields: {
            description: {
               type: 'textarea',
               label: 'AI Description',
               rows: 3,
               placeholder: 'Custom personality description',
               hint: 'Personality and behavior instructions for the AI. Changes apply to new conversations only.',
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
            bargein: {
               type: 'group',
               label: 'Barge-In',
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
            },
            model: {
               type: 'dynamic_select',
               label: 'Model',
               restart: true,
               hint: 'Whisper model size',
               dynamicKey: 'asr_models',
               allowCustom: true,
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
            },
            cloud: {
               type: 'group',
               label: 'Cloud Settings',
               fields: {
                  provider: {
                     type: 'select',
                     label: 'Provider',
                     options: ['openai', 'claude', 'gemini'],
                     hint: 'Cloud LLM provider',
                  },
                  endpoint: {
                     type: 'text',
                     label: 'Custom Endpoint',
                     placeholder: 'Leave empty for default',
                     hint: 'Override API endpoint (for proxies or compatible APIs)',
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
                  },
                  openai_default_model_idx: {
                     type: 'model_default_select',
                     label: 'Default OpenAI Model',
                     sourceKey: 'llm.cloud.openai_models',
                     hint: 'Default model for new conversations',
                  },
                  claude_models: {
                     type: 'model_list',
                     label: 'Claude Models',
                     rows: 8, // LLM_CLOUD_MAX_MODELS
                     placeholder: 'claude-sonnet-4-20250514\nclaude-opus-4-20250514',
                     hint: 'Available models for quick controls (one per line)',
                  },
                  claude_default_model_idx: {
                     type: 'model_default_select',
                     label: 'Default Claude Model',
                     sourceKey: 'llm.cloud.claude_models',
                     hint: 'Default model for new conversations',
                  },
                  gemini_models: {
                     type: 'model_list',
                     label: 'Gemini Models',
                     rows: 8, // LLM_CLOUD_MAX_MODELS
                     placeholder: 'gemini-2.5-flash\ngemini-2.5-pro\ngemini-3-flash-preview',
                     hint: 'Available models for quick controls (one per line). 2.5+ and 3.x support reasoning.',
                  },
                  gemini_default_model_idx: {
                     type: 'model_default_select',
                     label: 'Default Gemini Model',
                     sourceKey: 'llm.cloud.gemini_models',
                     hint: 'Default model for new conversations',
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
                     options: ['low', 'medium', 'high'],
                     hint: 'Controls which budget level is used (see below)',
                  },
                  budget_low: {
                     type: 'number',
                     label: 'Budget Low',
                     min: 256,
                     step: 256,
                     hint: 'Token budget for "low" reasoning effort (default: 1024)',
                  },
                  budget_medium: {
                     type: 'number',
                     label: 'Budget Medium',
                     min: 512,
                     step: 512,
                     hint: 'Token budget for "medium" reasoning effort (default: 8192)',
                  },
                  budget_high: {
                     type: 'number',
                     label: 'Budget High',
                     min: 1024,
                     step: 1024,
                     hint: 'Token budget for "high" reasoning effort (default: 16384)',
                  },
               },
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
            },
         },
         customContent: 'tools_list', // Special marker for injecting tools list
      },
      search: {
         label: 'Web Search',
         icon: '&#x1F50D;',
         adminOnly: true,
         fields: {
            engine: {
               type: 'select',
               label: 'Engine',
               options: ['searxng', 'disabled'],
               hint: 'Search engine for web queries (SearXNG is privacy-focused)',
            },
            endpoint: {
               type: 'text',
               label: 'Endpoint',
               hint: 'SearXNG instance URL (e.g., http://localhost:8888)',
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
         fields: {
            flaresolverr: {
               type: 'group',
               label: 'FlareSolverr',
               fields: {
                  enabled: {
                     type: 'checkbox',
                     label: 'Enable FlareSolverr',
                     hint: 'Auto-fallback for sites with Cloudflare protection (requires FlareSolverr service)',
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
               max: 4000,
               step: 100,
               hint: 'Max tokens for memory context injected into system prompt (~800 recommended)',
            },
            extraction_provider: {
               type: 'select',
               label: 'Extraction Provider',
               options: ['local', 'openai', 'claude'],
               hint: 'LLM provider for extracting facts from conversations',
               id: 'memory-extraction-provider',
            },
            extraction_model: {
               type: 'dynamic_select',
               label: 'Extraction Model',
               hint: 'Model for memory extraction (populated based on provider)',
               dynamicKey: 'memory_extraction_models',
               id: 'memory-extraction-model',
            },
            pruning_enabled: {
               type: 'checkbox',
               label: 'Enable Pruning',
               hint: 'Automatically remove old superseded and stale facts',
            },
            prune_superseded_days: {
               type: 'number',
               label: 'Superseded Retention (days)',
               min: 1,
               max: 365,
               hint: 'Keep superseded facts for this many days before deletion',
            },
            prune_stale_days: {
               type: 'number',
               label: 'Stale Threshold (days)',
               min: 7,
               max: 730,
               hint: 'Delete low-confidence facts not accessed in this many days',
            },
            prune_stale_min_confidence: {
               type: 'number',
               label: 'Stale Min Confidence',
               min: 0,
               max: 1,
               step: 0.1,
               hint: 'Only prune stale facts with confidence below this value (0-1)',
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
            },
            default_voice_user_id: {
               type: 'dynamic_select',
               label: 'Default Voice User',
               dynamicKey: 'users',
               hint: 'User account for local microphone and DAP voice conversations',
            },
         },
      },
      mqtt: {
         label: 'MQTT',
         icon: '&#x1F4E1;',
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
               hint: 'MQTT broker port (default: 1883)',
            },
         },
      },
      network: {
         label: 'Network (DAP)',
         description: 'Dawn Audio Protocol server for ESP32 and other remote voice clients',
         icon: '&#x1F4F6;',
         fields: {
            enabled: {
               type: 'checkbox',
               label: 'Enable DAP Server',
               restart: true,
               hint: 'Accept connections from remote voice clients',
            },
            host: {
               type: 'dynamic_select',
               label: 'Bind Address',
               restart: true,
               hint: 'Network interface to listen on',
               dynamicKey: 'bind_addresses',
            },
            port: {
               type: 'number',
               label: 'Port',
               min: 1,
               max: 65535,
               restart: true,
               hint: 'TCP port for DAP connections (default: 5000)',
            },
            workers: {
               type: 'number',
               label: 'Workers',
               min: 1,
               max: 8,
               restart: true,
               hint: 'Concurrent client processing threads',
            },
         },
      },
      webui: {
         label: 'WebUI',
         icon: '&#x1F310;',
         fields: {
            enabled: {
               type: 'checkbox',
               label: 'Enable WebUI',
               hint: 'Browser-based interface for voice interaction',
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
            },
            workers: {
               type: 'number',
               label: 'ASR Workers',
               min: 1,
               max: 8,
               restart: true,
               hint: 'Parallel speech recognition threads',
            },
            bind_address: {
               type: 'dynamic_select',
               label: 'Bind Address',
               restart: true,
               hint: 'Network interface to listen on',
               dynamicKey: 'bind_addresses',
            },
            https: {
               type: 'checkbox',
               label: 'Enable HTTPS',
               restart: true,
               hint: 'Required for microphone access on remote connections',
            },
         },
      },
      shutdown: {
         label: 'Shutdown',
         icon: '&#x1F512;',
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
         label: 'Image Storage',
         icon: '&#x1F5BC;',
         adminOnly: true,
         description: 'Settings for uploaded images in vision conversations',
         fields: {
            retention_days: {
               type: 'number',
               label: 'Retention Period (days)',
               min: 0,
               hint: 'Auto-delete images after this many days (0 = keep forever)',
            },
            max_size_mb: {
               type: 'number',
               label: 'Max Size (MB)',
               min: 1,
               max: 50,
               hint: 'Maximum allowed image upload size (1-50 MB)',
            },
            max_per_user: {
               type: 'number',
               label: 'Max Images Per User',
               min: 1,
               max: 10000,
               hint: 'Maximum stored images per user account (1-10,000)',
            },
         },
      },
      music: {
         label: 'Music Streaming',
         icon: '&#x1F3B5;',
         adminOnly: true,
         description: 'Settings for WebUI music streaming to browsers',
         fields: {
            scan_interval_minutes: {
               type: 'number',
               label: 'Library Rescan Interval (minutes)',
               min: 0,
               hint: 'Minutes between automatic library rescans (0 = disabled)',
            },
            streaming: {
               type: 'group',
               label: 'Streaming',
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
   };

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
    * Render all settings sections into the container
    */
   function renderSettingsSections() {
      if (!sectionsContainer) return;

      const currentConfig = getCurrentConfigFn ? getCurrentConfigFn() : null;
      if (!currentConfig) return;

      sectionsContainer.innerHTML = '';

      // Create virtual tool_calling config from llm.tools.mode
      // The tool_calling section is a UI convenience - backend uses llm.tools.mode
      const virtualConfig = { ...currentConfig };
      if (currentConfig.llm && currentConfig.llm.tools && currentConfig.llm.tools.mode) {
         virtualConfig.tool_calling = {
            mode: currentConfig.llm.tools.mode,
         };
      } else {
         virtualConfig.tool_calling = { mode: 'native' }; // Default
      }

      for (const [sectionKey, sectionDef] of Object.entries(SETTINGS_SCHEMA)) {
         const configSection = virtualConfig[sectionKey] || {};
         const sectionEl = createSettingsSection(sectionKey, sectionDef, configSection);
         sectionsContainer.appendChild(sectionEl);
      }
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
         header.classList.toggle('collapsed');
         content.classList.toggle('collapsed');
         // Update aria-expanded for accessibility (M14)
         const isExpanded = !header.classList.contains('collapsed');
         header.setAttribute('aria-expanded', isExpanded);
      };
      header.addEventListener('click', toggleSection);
      header.addEventListener('keydown', (e) => {
         if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            toggleSection();
         }
      });
      header.setAttribute('aria-expanded', 'true'); // Expanded by default

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
      <button id="save-tools-btn" class="save-btn">Save Tool Settings</button>
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
    * Create a setting field element
    * @param {string} sectionKey - Section key
    * @param {string} fieldKey - Field key
    * @param {Object} def - Field definition
    * @param {*} value - Current value
    * @returns {HTMLElement} Field element
    */
   function createSettingField(sectionKey, fieldKey, def, value) {
      const fullKey = `${sectionKey}.${fieldKey}`;
      const currentConfig = getCurrentConfigFn ? getCurrentConfigFn() : {};
      const restartRequiredFields = getRestartRequiredFieldsFn ? getRestartRequiredFieldsFn() : [];
      const dynamicOptions = getDynamicOptionsFn ? getDynamicOptionsFn() : {};

      // Handle nested groups
      if (def.type === 'group') {
         const groupEl = document.createElement('div');
         groupEl.className = 'setting-group';
         groupEl.innerHTML = `<div class="group-label">${escapeHtml(def.label || '')}</div>`;

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
            const numAttrs = [
               def.min !== undefined ? `min="${def.min}"` : '',
               def.max !== undefined ? `max="${def.max}"` : '',
               def.step !== undefined ? `step="${def.step}"` : '',
            ]
               .filter(Boolean)
               .join(' ');
            inputHtml = `<input type="number" id="${inputId}" value="${Utils.formatNumber(value)}" ${numAttrs} data-key="${fullKey}">`;
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
         input.addEventListener('change', () => handleSettingChangeCallback(fullKey, input));
         input.addEventListener('input', () => handleSettingChangeCallback(fullKey, input));
      }

      // Special handling for range inputs - update display value
      if (def.type === 'range' && input) {
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
      createSettingsSection,
      createSettingField,
      updateDependentModelSelects,
      createToolsListContent,
   };
})();
