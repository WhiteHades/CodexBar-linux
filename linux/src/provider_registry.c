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
    {"azureopenai", "Azure OpenAI", "azure-openai", "azureopenai,aoai", A | P, FALSE, "https://ai.azure.com", "https://azure.status.microsoft/en-us/status", CODEXBAR_NATIVE_UNAVAILABLE},
    {"claude", "Claude", "claude", NULL, A | P | W | C | O, FALSE, "https://console.anthropic.com/settings/billing", "https://status.claude.com/", CODEXBAR_NATIVE_CLAUDE},
    {"cursor", "Cursor", "cursor", NULL, A | C | W, FALSE, "https://cursor.com/dashboard?tab=usage", "https://status.cursor.com", CODEXBAR_NATIVE_UNAVAILABLE},
    {"opencode", "OpenCode", "opencode", NULL, A | W, FALSE, "https://opencode.ai", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"opencodego", "OpenCode Go", "opencodego", NULL, A | W, FALSE, "https://opencode.ai", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"alibaba", "Alibaba", "alibaba-coding-plan", "alibaba,bailian", A | W | P, FALSE, "https://modelstudio.console.alibabacloud.com/ap-southeast-1/?tab=coding-plan#/efm/coding_plan", "https://status.aliyun.com", CODEXBAR_NATIVE_UNAVAILABLE},
    {"alibabatokenplan", "Alibaba Token Plan", "alibaba-token-plan", "alibaba-token,bailian-token-plan", A | W, FALSE, "https://modelstudio.console.alibabacloud.com/ap-southeast-1/?tab=plan#/efm/subscription/token-plan", "https://status.aliyun.com", CODEXBAR_NATIVE_UNAVAILABLE},
    {"factory", "Droid", "factory", NULL, A | P | W | C, FALSE, "https://app.factory.ai/settings/billing", "https://status.factory.ai", CODEXBAR_NATIVE_UNAVAILABLE},
    {"gemini", "Gemini", "gemini", NULL, A | P, FALSE, "https://gemini.google.com", "https://www.google.com/appsstatus/dashboard/products/npdyhgECDJ6tB66MxXyo/history", CODEXBAR_NATIVE_UNAVAILABLE},
    {"antigravity", "Antigravity", "antigravity", NULL, A | C | O, FALSE, NULL, "https://www.google.com/appsstatus/dashboard/products/npdyhgECDJ6tB66MxXyo/history", CODEXBAR_NATIVE_UNAVAILABLE},
    {"copilot", "Copilot", "copilot", NULL, A | P, FALSE, "https://github.com/settings/copilot", "https://www.githubstatus.com/", CODEXBAR_NATIVE_COPILOT},
    {"devin", "Devin", "devin", NULL, A | W, FALSE, "https://app.devin.ai", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"zai", "z.ai", "zai", "z.ai", A | P, FALSE, "https://z.ai/manage-apikey/coding-plan/personal/my-plan", NULL, CODEXBAR_NATIVE_ZAI},
    {"minimax", "MiniMax", "minimax", "mini-max", A | W | P, FALSE, "https://platform.minimax.io/user-center/payment/coding-plan?cycle_type=3", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"manus", "Manus", "manus", NULL, A | W, FALSE, "https://manus.im", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"kimi", "Kimi", "kimi", "kimi-ai", A | P | W, FALSE, "https://www.kimi.com/code/console", NULL, CODEXBAR_NATIVE_KIMI},
    {"kilo", "Kilo", "kilo", "kilo-ai", A | P | C, FALSE, "https://app.kilo.ai/usage", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"kiro", "Kiro", "kiro", "kiro-cli", A | C, FALSE, "https://app.kiro.dev/account/usage", "https://health.aws.amazon.com/health/status", CODEXBAR_NATIVE_UNAVAILABLE},
    {"vertexai", "Vertex AI", "vertexai", NULL, A | O, FALSE, "https://console.cloud.google.com/vertex-ai", "https://status.cloud.google.com", CODEXBAR_NATIVE_UNAVAILABLE},
    {"augment", "Augment", "augment", NULL, A | C, FALSE, "https://app.augmentcode.com/account/subscription", "https://status.augmentcode.com", CODEXBAR_NATIVE_UNAVAILABLE},
    {"jetbrains", "JetBrains AI", "jetbrains", NULL, A | C, FALSE, NULL, NULL, CODEXBAR_NATIVE_JETBRAINS},
    {"kimik2", "Kimi K2 (unofficial)", "kimik2", "kimi-k2,kimiK2", A | P, FALSE, "https://kimrel.com/my-credits", NULL, CODEXBAR_NATIVE_KIMI_K2},
    {"moonshot", "Moonshot / Kimi API", "moonshot", NULL, A | P, FALSE, "https://platform.moonshot.ai/console/account", NULL, CODEXBAR_NATIVE_SIMPLE},
    {"amp", "Amp", "amp", NULL, A | P | W | C, FALSE, "https://ampcode.com/settings/usage", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"t3chat", "T3 Chat", "t3chat", "t3-chat,t3", A | W, FALSE, "https://t3.chat/settings/customization", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"ollama", "Ollama", "ollama", NULL, A | W | P, FALSE, "https://ollama.com/settings", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"synthetic", "Synthetic", "synthetic", "synthetic.new", A | P, FALSE, NULL, NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"warp", "Warp", "warp", "warp-ai,warp-terminal", A | P, FALSE, "https://docs.warp.dev/reference/cli/api-keys", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"openrouter", "OpenRouter", "openrouter", "or", A | P, FALSE, "https://openrouter.ai/settings/credits", "https://status.openrouter.ai", CODEXBAR_NATIVE_OPENROUTER},
    {"elevenlabs", "ElevenLabs", "elevenlabs", "11labs,eleven", A | P, FALSE, "https://elevenlabs.io/app/developers/usage", "https://status.elevenlabs.io", CODEXBAR_NATIVE_SIMPLE},
    {"windsurf", "Windsurf", "windsurf", NULL, A | W | C, FALSE, "https://windsurf.com/subscription/usage", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"zed", "Zed", "zed", NULL, A | P, FALSE, NULL, NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"perplexity", "Perplexity", "perplexity", NULL, A | W, FALSE, "https://www.perplexity.ai/account/usage", "https://status.perplexity.com/", CODEXBAR_NATIVE_UNAVAILABLE},
    {"mimo", "Xiaomi MiMo", "mimo", "xiaomi-mimo", A | W, FALSE, "https://platform.xiaomimimo.com/#/console/balance", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"doubao", "Doubao", "doubao", "volcengine,ark,bytedance", A | P, FALSE, "https://console.volcengine.com/ark/region:ark+cn-beijing/openManagement?LLM=%7B%7D&advancedActiveKey=subscribe", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"sakana", "Sakana AI", "sakana", "sakana-ai", A | W, FALSE, "https://console.sakana.ai/billing", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"abacus", "Abacus AI", "abacusai", "abacus-ai", A | W, FALSE, "https://apps.abacus.ai/chatllm/admin/compute-points-usage", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"mistral", "Mistral", "mistral", "mistral-ai", A | W, FALSE, "https://admin.mistral.ai/organization/usage", "https://status.mistral.ai", CODEXBAR_NATIVE_UNAVAILABLE},
    {"deepseek", "DeepSeek", "deepseek", "deep-seek,ds", A | P, FALSE, "https://platform.deepseek.com/usage", "https://status.deepseek.com", CODEXBAR_NATIVE_SIMPLE},
    {"codebuff", "Codebuff", "codebuff", "manicode", A | P, FALSE, "https://www.codebuff.com/usage", NULL, CODEXBAR_NATIVE_CODEBUFF},
    {"crof", "Crof", "crof", "crofai", A | P, FALSE, "https://crof.ai/dashboard", NULL, CODEXBAR_NATIVE_SIMPLE},
    {"venice", "Venice", "venice", "ven", A | P, FALSE, "https://venice.ai/settings/api", NULL, CODEXBAR_NATIVE_SIMPLE},
    {"commandcode", "Command Code", "commandcode", "command-code", A | W, FALSE, "https://commandcode.ai/studio", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"qoder", "Qoder", "qoder", NULL, A | W, FALSE, "https://qoder.com/account/usage", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"stepfun", "StepFun", "stepfun", "step-fun,sf", A | W, FALSE, "https://platform.stepfun.com/plan-usage", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"bedrock", "AWS Bedrock", "bedrock", "aws-bedrock", A | P, FALSE, "https://console.aws.amazon.com/bedrock", "https://health.aws.amazon.com/health/status", CODEXBAR_NATIVE_UNAVAILABLE},
    {"grok", "Grok", "grok", NULL, A | C | W, FALSE, "https://grok.com/?_s=usage", "https://status.x.ai", CODEXBAR_NATIVE_UNAVAILABLE},
    {"groq", "Groq", "groqcloud", "groq,groq-api", A | W | P, FALSE, "https://console.groq.com/dashboard/usage", "https://status.groq.com", CODEXBAR_NATIVE_UNAVAILABLE},
    {"llmproxy", "LLM Proxy", "llmproxy", "llm-api-key-proxy,llm-proxy", A | P, FALSE, NULL, NULL, CODEXBAR_NATIVE_PROXY},
    {"litellm", "LiteLLM", "litellm", "litellm-proxy", A | P, FALSE, NULL, NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"deepgram", "Deepgram", "deepgram", "dg", A | P, FALSE, "https://console.deepgram.com/project/", "https://status.deepgram.com", CODEXBAR_NATIVE_UNAVAILABLE},
    {"poe", "Poe", "poe", NULL, A | P, FALSE, "https://poe.com/api/keys", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"chutes", "Chutes", "chutes", "chutes.ai", A | P, FALSE, "https://chutes.ai", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"crossmodel", "CrossModel", "crossmodel", "cm", A | P, FALSE, "https://crossmodel.ai/console/usage", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"clawrouter", "ClawRouter", "clawrouter", "claw-router", A | P, FALSE, "https://clawrouter.openclaw.ai/dashboard/access", NULL, CODEXBAR_NATIVE_PROXY},
    {"sub2api", "sub2api", "sub2api", "sub-2-api", A | P, FALSE, NULL, NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"wayfinder", "Wayfinder", "wayfinder", "wayfinder-router", A | P, FALSE, "http://127.0.0.1:8088/router", NULL, CODEXBAR_NATIVE_UNAVAILABLE},
    {"zenmux", "ZenMux", "zenmux", "zen-mux", A | P, FALSE, "https://zenmux.ai/platform/management", NULL, CODEXBAR_NATIVE_SIMPLE},
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

gboolean codexbar_provider_status_is_pollable(const CodexBarProviderDescriptor *provider) {
    if (!provider) return FALSE;
    const char *pollable[] = {"codex", "openai", "claude", "cursor", "factory", "copilot", "augment"};
    for (guint index = 0; index < G_N_ELEMENTS(pollable); index++) {
        if (g_str_equal(provider->id, pollable[index])) return TRUE;
    }
    return FALSE;
}

gboolean codexbar_provider_supports_config_api_key(const CodexBarProviderDescriptor *provider) {
    if (!provider) return FALSE;
    const char *supported[] = {
        "amp",       "openai",    "azureopenai", "claude",     "zai",       "minimax",
        "alibaba",   "kilo",      "synthetic",   "openrouter", "elevenlabs", "moonshot",
        "kimi",      "ollama",    "venice",      "deepgram",   "groq",      "llmproxy",
        "chutes",    "poe",       "litellm",     "crossmodel", "clawrouter", "factory",
        "sub2api",   "zenmux",    "copilot",     "kimik2",    "warp",      "codebuff",
        "crof",      "doubao",
    };
    for (guint index = 0; index < G_N_ELEMENTS(supported); index++) {
        if (g_str_equal(provider->id, supported[index])) return TRUE;
    }
    return FALSE;
}
