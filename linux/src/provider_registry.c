#include "provider_registry.h"

#include <string.h>

#define A (CODEXBAR_SOURCE_AUTO)
#define W (CODEXBAR_SOURCE_WEB)
#define C (CODEXBAR_SOURCE_CLI)
#define O (CODEXBAR_SOURCE_OAUTH)
#define P (CODEXBAR_SOURCE_API)

static const CodexBarProviderDescriptor providers[] = {
    {"codex", "Codex", "codex", NULL, A | W | C | O, TRUE, "https://chatgpt.com/codex/settings/usage", "https://status.openai.com/", CODEXBAR_NATIVE_CODEX},
    {"openai", "OpenAI", "openai", "openai-api", A | P, FALSE, "https://platform.openai.com/usage", "https://status.openai.com", CODEXBAR_NATIVE_OPENAI},
    {"azureopenai", "Azure OpenAI", "azure-openai", "azureopenai,aoai", A | P, FALSE, "https://ai.azure.com", "https://azure.status.microsoft/en-us/status", CODEXBAR_NATIVE_AZURE_OPENAI},
    {"claude", "Claude", "claude", NULL, A | P | W | C | O, FALSE, "https://console.anthropic.com/settings/billing", "https://status.claude.com/", CODEXBAR_NATIVE_CLAUDE},
    {"clinepass", "ClinePass", "clinepass", NULL, A | P, FALSE, "https://app.cline.bot/dashboard/subscription?personal=true", NULL, CODEXBAR_NATIVE_CLINEPASS},
    {"cursor", "Cursor", "cursor", NULL, A | W | C, FALSE, "https://cursor.com/dashboard?tab=usage", "https://status.cursor.com", CODEXBAR_NATIVE_CURSOR},
    {"opencode", "OpenCode", "opencode", NULL, A | W, FALSE, "https://opencode.ai", NULL, CODEXBAR_NATIVE_OPENCODE},
    {"opencodego", "OpenCode Go", "opencodego", NULL, A | W, FALSE, "https://opencode.ai", NULL, CODEXBAR_NATIVE_OPENCODE_GO},
    {"alibaba", "Alibaba", "alibaba-coding-plan", "alibaba,bailian", A | W | P, FALSE, "https://modelstudio.console.alibabacloud.com/ap-southeast-1/?tab=coding-plan#/efm/coding_plan", "https://status.aliyun.com", CODEXBAR_NATIVE_ALIBABA},
    {"alibabatokenplan", "Alibaba Token Plan", "alibaba-token-plan", "alibaba-token,bailian-token-plan", A | W, FALSE, "https://modelstudio.console.alibabacloud.com/ap-southeast-1/?tab=plan#/efm/subscription/token-plan", "https://status.aliyun.com", CODEXBAR_NATIVE_ALIBABA_TOKEN_PLAN},
    {"qwencloud", "Qwen Cloud", "qwen-cloud", "qwencloud,qwen,qwen-token-plan", A | W, FALSE, "https://home.qwencloud.com/billing/subscription/token-plan-individual", "https://status.alibabacloud.com", CODEXBAR_NATIVE_QWEN_CLOUD},
    {"factory", "Droid", "factory", NULL, A | P | W | C, FALSE, "https://app.factory.ai/settings/billing", "https://status.factory.ai", CODEXBAR_NATIVE_FACTORY},
    {"fireworks", "Fireworks", "fireworks", "fw", A | P, FALSE, "https://app.fireworks.ai", NULL, CODEXBAR_NATIVE_FIREWORKS},
    {"gemini", "Gemini", "gemini", NULL, A | P, FALSE, "https://gemini.google.com", "https://www.google.com/appsstatus/dashboard/products/npdyhgECDJ6tB66MxXyo/history", CODEXBAR_NATIVE_GEMINI},
    {"antigravity", "Antigravity", "antigravity", NULL, A | C | O, FALSE, NULL, "https://www.google.com/appsstatus/dashboard/products/npdyhgECDJ6tB66MxXyo/history", CODEXBAR_NATIVE_ANTIGRAVITY},
    {"copilot", "Copilot", "copilot", NULL, A | P, FALSE, "https://github.com/settings/copilot", "https://www.githubstatus.com/", CODEXBAR_NATIVE_COPILOT},
    {"devin", "Devin", "devin", NULL, A | W, FALSE, "https://app.devin.ai", NULL, CODEXBAR_NATIVE_DEVIN},
    {"zai", "z.ai", "zai", "z.ai", A | P, FALSE, "https://z.ai/manage-apikey/coding-plan/personal/my-plan", NULL, CODEXBAR_NATIVE_ZAI},
    {"minimax", "MiniMax", "minimax", "mini-max", A | W | P, FALSE, "https://platform.minimax.io/user-center/payment/coding-plan?cycle_type=3", NULL, CODEXBAR_NATIVE_MINIMAX},
    {"manus", "Manus", "manus", NULL, A | W, FALSE, "https://manus.im", NULL, CODEXBAR_NATIVE_MANUS},
    {"kimi", "Kimi", "kimi", "kimi-ai", A | W | P, FALSE, "https://www.kimi.com/code/console", NULL, CODEXBAR_NATIVE_KIMI},
    {"kilo", "Kilo", "kilo", "kilo-ai", A | P | C, FALSE, "https://app.kilo.ai/usage", NULL, CODEXBAR_NATIVE_KILO},
    {"kiro", "Kiro", "kiro", "kiro-cli", A | C, FALSE, "https://app.kiro.dev/account/usage", "https://health.aws.amazon.com/health/status", CODEXBAR_NATIVE_KIRO},
    {"vertexai", "Vertex AI", "vertexai", NULL, A | O, FALSE, "https://console.cloud.google.com/vertex-ai", "https://status.cloud.google.com", CODEXBAR_NATIVE_VERTEX},
    {"augment", "Augment", "augment", NULL, A | C, FALSE, "https://app.augmentcode.com/account/subscription", "https://status.augmentcode.com", CODEXBAR_NATIVE_AUGMENT},
    {"jetbrains", "JetBrains AI", "jetbrains", NULL, A | C, FALSE, NULL, NULL, CODEXBAR_NATIVE_JETBRAINS},
    {"moonshot", "Moonshot / Kimi API", "moonshot", NULL, A | P, FALSE, "https://platform.moonshot.ai/console/account", NULL, CODEXBAR_NATIVE_SIMPLE},
    {"amp", "Amp", "amp", NULL, A | P | W | C, FALSE, "https://ampcode.com/settings/usage", NULL, CODEXBAR_NATIVE_AMP},
    {"t3chat", "T3 Chat", "t3chat", "t3-chat,t3", A | W, FALSE, "https://t3.chat/settings/customization", NULL, CODEXBAR_NATIVE_T3CHAT},
    {"ollama", "Ollama", "ollama", NULL, A | W | P, FALSE, "https://ollama.com/settings", NULL, CODEXBAR_NATIVE_OLLAMA},
    {"synthetic", "Synthetic", "synthetic", "synthetic.new", A | P, FALSE, NULL, NULL, CODEXBAR_NATIVE_SYNTHETIC},
    {"warp", "Warp", "warp", "warp-ai,warp-terminal", A | P, FALSE, "https://docs.warp.dev/reference/cli/api-keys", NULL, CODEXBAR_NATIVE_WARP},
    {"openrouter", "OpenRouter", "openrouter", "or", A | P, FALSE, "https://openrouter.ai/settings/credits", "https://status.openrouter.ai", CODEXBAR_NATIVE_OPENROUTER},
    {"elevenlabs", "ElevenLabs", "elevenlabs", "11labs,eleven", A | P, FALSE, "https://elevenlabs.io/app/developers/usage", "https://status.elevenlabs.io", CODEXBAR_NATIVE_SIMPLE},
    {"windsurf", "Windsurf", "windsurf", NULL, A | W | C, FALSE, "https://windsurf.com/subscription/usage", NULL, CODEXBAR_NATIVE_WINDSURF},
    {"zed", "Zed", "zed", NULL, A | P, FALSE, NULL, NULL, CODEXBAR_NATIVE_ZED},
    {"perplexity", "Perplexity", "perplexity", NULL, A | W, FALSE, "https://www.perplexity.ai/account/usage", "https://status.perplexity.com/", CODEXBAR_NATIVE_PERPLEXITY},
    {"mimo", "Xiaomi MiMo", "mimo", "xiaomi-mimo", A | W, FALSE, "https://platform.xiaomimimo.com/#/console/balance", NULL, CODEXBAR_NATIVE_MIMO},
    {"doubao", "Doubao", "doubao", "volcengine,ark,bytedance", A | C | P, FALSE, "https://console.volcengine.com/ark/region:ark+cn-beijing/openManagement?LLM=%7B%7D&advancedActiveKey=subscribe", NULL, CODEXBAR_NATIVE_DOUBAO},
    {"sakana", "Sakana AI", "sakana", "sakana-ai", A | W, FALSE, "https://console.sakana.ai/billing", NULL, CODEXBAR_NATIVE_SAKANA},
    {"abacus", "Abacus AI", "abacusai", "abacus-ai", A | W, FALSE, "https://apps.abacus.ai/chatllm/admin/compute-points-usage", NULL, CODEXBAR_NATIVE_ABACUS},
    {"mistral", "Mistral", "mistral", "mistral-ai", A | W, FALSE, "https://admin.mistral.ai/organization/usage", "https://status.mistral.ai", CODEXBAR_NATIVE_MISTRAL},
    {"deepseek", "DeepSeek", "deepseek", "deep-seek,ds", A | W | P, FALSE, "https://platform.deepseek.com/usage", "https://status.deepseek.com", CODEXBAR_NATIVE_SIMPLE},
    {"deepinfra", "DeepInfra", "deepinfra", "deep-infra,di", A | P, FALSE, "https://deepinfra.com/dash", "https://status.deepinfra.com", CODEXBAR_NATIVE_DEEPINFRA},
    {"codebuff", "Codebuff", "codebuff", "manicode", A | P, FALSE, "https://www.codebuff.com/usage", NULL, CODEXBAR_NATIVE_CODEBUFF},
    {"crof", "Crof", "crof", "crofai", A | P, FALSE, "https://crof.ai/dashboard", NULL, CODEXBAR_NATIVE_SIMPLE},
    {"venice", "Venice", "venice", "ven", A | P, FALSE, "https://venice.ai/settings/api", NULL, CODEXBAR_NATIVE_SIMPLE},
    {"commandcode", "Command Code", "commandcode", "command-code", A | W, FALSE, "https://commandcode.ai/studio", NULL, CODEXBAR_NATIVE_COMMANDCODE},
    {"qoder", "Qoder", "qoder", NULL, A | W, FALSE, "https://qoder.com/account/usage", NULL, CODEXBAR_NATIVE_QODER},
    {"stepfun", "StepFun", "stepfun", "step-fun,sf", A | W, FALSE, "https://platform.stepfun.com/plan-usage", NULL, CODEXBAR_NATIVE_STEPFUN},
    {"bedrock", "AWS Bedrock", "bedrock", "aws-bedrock", A | P, FALSE, "https://console.aws.amazon.com/bedrock", "https://health.aws.amazon.com/health/status", CODEXBAR_NATIVE_BEDROCK},
    {"grok", "Grok", "grok", NULL, A | C | W, FALSE, "https://grok.com/?_s=usage", "https://status.x.ai", CODEXBAR_NATIVE_GROK},
    {"groq", "Groq", "groqcloud", "groq,groq-api", A | W | P, FALSE, "https://console.groq.com/dashboard/usage", "https://status.groq.com", CODEXBAR_NATIVE_GROQ},
    {"llmproxy", "LLM Proxy", "llmproxy", "llm-api-key-proxy,llm-proxy", A | P, FALSE, NULL, NULL, CODEXBAR_NATIVE_PROXY},
    {"litellm", "LiteLLM", "litellm", "litellm-proxy", A | P, FALSE, NULL, NULL, CODEXBAR_NATIVE_LITELLM},
    {"deepgram", "Deepgram", "deepgram", "dg", A | P, FALSE, "https://console.deepgram.com/project/", "https://status.deepgram.com", CODEXBAR_NATIVE_DEEPGRAM},
    {"poe", "Poe", "poe", NULL, A | P, FALSE, "https://poe.com/api/keys", NULL, CODEXBAR_NATIVE_POE},
    {"chutes", "Chutes", "chutes", "chutes.ai", A | P, FALSE, "https://chutes.ai", NULL, CODEXBAR_NATIVE_CHUTES},
    {"neuralwatt", "Neuralwatt", "neuralwatt", "nw,neural", A | P, FALSE, "https://portal.neuralwatt.com/dashboard", NULL, CODEXBAR_NATIVE_NEURALWATT},
    {"clawrouter", "ClawRouter", "clawrouter", "claw-router", A | P, FALSE, "https://clawrouter.openclaw.ai/dashboard/access", NULL, CODEXBAR_NATIVE_PROXY},
    {"longcat", "LongCat", "longcat", "long-cat,lc", A | W, FALSE, "https://longcat.chat/platform/", NULL, CODEXBAR_NATIVE_LONGCAT},
    {"sub2api", "sub2api", "sub2api", "sub-2-api", A | P, FALSE, NULL, NULL, CODEXBAR_NATIVE_SUB2API},
    {"wayfinder", "Wayfinder", "wayfinder", "wayfinder-router", A | P, FALSE, "http://127.0.0.1:8088/router", NULL, CODEXBAR_NATIVE_WAYFINDER},
    {"zenmux", "ZenMux", "zenmux", "zen-mux", A | P, FALSE, "https://zenmux.ai/platform/management", NULL, CODEXBAR_NATIVE_SIMPLE},
    {"aiand", "ai&", "aiand", "ai&,ai-and", A | P, FALSE, "https://console.aiand.com", NULL, CODEXBAR_NATIVE_AIAND},
    {"zoommate", "ZoomMate", "zoommate", NULL, A | W, FALSE, "https://zoommate.zoom.us/#/?settings=credit-usage", "https://www.zoomstatus.com/", CODEXBAR_NATIVE_ZOOMMATE},
    {"xai", "xAI", "xai", NULL, A | P, FALSE, "https://console.x.ai", "https://status.x.ai", CODEXBAR_NATIVE_XAI},
    {"notion", "Notion AI", "notion", "notion-ai,notionai", A | W, FALSE, "https://app.notion.com", "https://status.notion.so", CODEXBAR_NATIVE_NOTION},
    {"ibmbob", "IBM Bob", "ibmbob", "ibm-bob,bob,bobshell", A | P, FALSE, "https://bob.ibm.com", "https://status.bob.ibm.com", CODEXBAR_NATIVE_IBMBOB},
};

#undef A
#undef W
#undef C
#undef O
#undef P

guint codexbar_provider_registry_count(void) {
    return G_N_ELEMENTS(providers);
}

const CodexBarProviderDescriptor *codexbar_provider_registry_at(guint index) {
    return index < G_N_ELEMENTS(providers) ? &providers[index] : NULL;
}

static gboolean aliases_contain(const char *aliases, const char *name) {
    if (!aliases) return FALSE;
    size_t name_length = strlen(name);
    const char *start = aliases;
    while (*start != '\0') {
        const char *end = strchr(start, ',');
        size_t length = end ? (size_t)(end - start) : strlen(start);
        if (length == name_length && strncmp(start, name, length) == 0) return TRUE;
        if (!end) break;
        start = end + 1;
    }
    return FALSE;
}

const CodexBarProviderDescriptor *codexbar_provider_registry_find(const char *name) {
    if (!name || name[0] == '\0') return NULL;
    for (guint index = 0; index < G_N_ELEMENTS(providers); index++) {
        const CodexBarProviderDescriptor *provider = &providers[index];
        if (g_str_equal(name, provider->id) || g_str_equal(name, provider->cli_name) ||
            aliases_contain(provider->aliases, name)) {
            return provider;
        }
    }
    return NULL;
}

gboolean codexbar_provider_supports_source(const CodexBarProviderDescriptor *provider, const char *source) {
    if (!provider || !source) return FALSE;
    guint mode = g_str_equal(source, "auto")    ? CODEXBAR_SOURCE_AUTO
                 : g_str_equal(source, "web")  ? CODEXBAR_SOURCE_WEB
                 : g_str_equal(source, "cli")  ? CODEXBAR_SOURCE_CLI
                 : g_str_equal(source, "oauth") ? CODEXBAR_SOURCE_OAUTH
                 : g_str_equal(source, "api")  ? CODEXBAR_SOURCE_API
                                                 : 0;
    return mode != 0 && (provider->source_modes & mode) != 0;
}

static void append_source(const char *source, const char **sources, guint capacity, guint *count) {
    if (sources && *count < capacity) sources[*count] = source;
    (*count)++;
}

guint codexbar_provider_auto_source_plan(const CodexBarProviderDescriptor *provider,
                                         const char **sources,
                                         guint capacity) {
    if (!provider || !codexbar_provider_supports_source(provider, "auto")) return 0;
    guint count = 0;
    if (g_str_equal(provider->id, "codex")) {
        append_source("oauth", sources, capacity, &count);
        append_source("cli", sources, capacity, &count);
        return count;
    }
    if (g_str_equal(provider->id, "claude")) {
        append_source("web", sources, capacity, &count);
        append_source("cli", sources, capacity, &count);
        return count;
    }
    if (g_str_equal(provider->id, "cursor")) {
        append_source("web", sources, capacity, &count);
        return count;
    }
    if (g_str_equal(provider->id, "factory")) {
        append_source("api", sources, capacity, &count);
        append_source("web", sources, capacity, &count);
        return count;
    }
    if (g_str_equal(provider->id, "antigravity")) {
        append_source("cli", sources, capacity, &count);
        append_source("oauth", sources, capacity, &count);
        return count;
    }
    if (g_str_equal(provider->id, "amp")) {
        append_source("cli", sources, capacity, &count);
        append_source("api", sources, capacity, &count);
        append_source("web", sources, capacity, &count);
        return count;
    }
    if (g_str_equal(provider->id, "kilo")) {
        append_source("api", sources, capacity, &count);
        append_source("cli", sources, capacity, &count);
        return count;
    }
    if (g_str_equal(provider->id, "grok")) {
        append_source("cli", sources, capacity, &count);
        append_source("web", sources, capacity, &count);
        return count;
    }
    if (g_str_equal(provider->id, "ollama")) {
        append_source("web", sources, capacity, &count);
        append_source("api", sources, capacity, &count);
        return count;
    }
    if (g_str_equal(provider->id, "groq")) {
        append_source("web", sources, capacity, &count);
        append_source("api", sources, capacity, &count);
        return count;
    }
    if (g_str_equal(provider->id, "alibaba")) {
        append_source("web", sources, capacity, &count);
        append_source("api", sources, capacity, &count);
        return count;
    }
    if (g_str_equal(provider->id, "opencodego")) {
        append_source("local", sources, capacity, &count);
        return count;
    }
    const char *const candidates[] = {"api", "web", "cli", "oauth"};
    for (guint index = 0; index < G_N_ELEMENTS(candidates); index++) {
        if (codexbar_provider_supports_source(provider, candidates[index])) {
            append_source(candidates[index], sources, capacity, &count);
        }
    }
    return count;
}

char *codexbar_provider_supported_sources(const CodexBarProviderDescriptor *provider) {
    const char *const candidates[] = {"auto", "web", "cli", "oauth", "api"};
    GString *result = g_string_new(NULL);
    for (guint index = 0; index < G_N_ELEMENTS(candidates); index++) {
        if (!codexbar_provider_supports_source(provider, candidates[index])) continue;
        if (result->len > 0) g_string_append(result, ", ");
        g_string_append(result, candidates[index]);
    }
    return g_string_free(result, FALSE);
}

CodexBarProviderStatusSource codexbar_provider_status_source(const CodexBarProviderDescriptor *provider) {
    if (!provider) return CODEXBAR_PROVIDER_STATUS_NONE;
    const char *statuspage[] = {
        "codex", "openai", "claude", "cursor", "factory", "copilot", "augment", "zoommate"};
    for (guint index = 0; index < G_N_ELEMENTS(statuspage); index++) {
        if (g_str_equal(provider->id, statuspage[index])) return CODEXBAR_PROVIDER_STATUS_STATUSPAGE;
    }
    if (g_str_equal(provider->id, "gemini") || g_str_equal(provider->id, "antigravity")) {
        return CODEXBAR_PROVIDER_STATUS_GOOGLE_WORKSPACE;
    }
    return CODEXBAR_PROVIDER_STATUS_NONE;
}

const char *codexbar_provider_status_source_value(const CodexBarProviderDescriptor *provider) {
    switch (codexbar_provider_status_source(provider)) {
    case CODEXBAR_PROVIDER_STATUS_STATUSPAGE: return provider->status_url;
    case CODEXBAR_PROVIDER_STATUS_GOOGLE_WORKSPACE: return "npdyhgECDJ6tB66MxXyo";
    case CODEXBAR_PROVIDER_STATUS_NONE: return NULL;
    }
    return NULL;
}

gboolean codexbar_provider_status_is_pollable(const CodexBarProviderDescriptor *provider) {
    return codexbar_provider_status_source(provider) != CODEXBAR_PROVIDER_STATUS_NONE;
}

gboolean codexbar_provider_supports_config_api_key(const CodexBarProviderDescriptor *provider) {
    if (!provider) return FALSE;
    const char *supported[] = {
        "amp",       "openai",    "azureopenai", "claude",     "clinepass", "zai",
        "minimax",   "alibaba",   "kilo",        "synthetic",  "openrouter", "elevenlabs",
        "moonshot",  "kimi",      "ollama",      "venice",     "deepgram",   "groq",
        "llmproxy",  "chutes",    "poe",         "litellm",    "clawrouter", "factory",
        "sub2api",   "zenmux",    "copilot",     "warp",       "codebuff",   "crof",
        "doubao",    "deepinfra", "fireworks",   "ibmbob",    "neuralwatt", "aiand",
        "xai",
    };
    for (guint index = 0; index < G_N_ELEMENTS(supported); index++) {
        if (g_str_equal(provider->id, supported[index])) return TRUE;
    }
    return FALSE;
}
