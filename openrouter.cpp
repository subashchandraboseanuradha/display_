#include "openrouter.h"
#include "secrets.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "mbedtls/base64.h"

#define OR_URL          "https://openrouter.ai/api/v1/chat/completions"
#define CB_URL          "https://api.cerebras.ai/v1/chat/completions"
#define OR_MAX_TOKENS   300
#define OR_TIMEOUT_MS   30000

// Cerebras: user's own key (secrets.h) — primary text backend when set.
// OpenAI-compatible endpoint, very fast, no shared free-tier limits.
// Text-only (no vision models) — vision always goes to OpenRouter.
// IDs verified live against /v1/models with the user's key (2026-07-09).
// gemma first: non-reasoning model, so max_tokens all go to the answer
// (gpt-oss is a reasoning model — thinking tokens can eat the budget).
static const char* CB_TEXT_MODELS[] = {
    "gemma-4-31b",
    "zai-glm-4.7",
    "gpt-oss-120b",
};

// Free models get rate-limited upstream (seen live: HTTP 429 "temporarily
// rate-limited upstream" on llama-3.2). Try each in order until one answers.
// A dead/renamed model just errors fast and falls through to the next.
static const char* OR_TEXT_MODELS[] = {
    "meta-llama/llama-3.2-3b-instruct:free",
    "mistralai/mistral-7b-instruct:free",
    "qwen/qwen-2.5-7b-instruct:free",
    "google/gemma-3-4b-it:free",
};
static const char* OR_VISION_MODELS[] = {
    "nvidia/nemotron-nano-12b-v2-vl:free",
    "meta-llama/llama-3.2-11b-vision-instruct:free",
};

// Escape a string for JSON: replace " → ' and \ → space and newline → space.
// Writes at most max-1 chars into dst (always null-terminates).
static void json_sanitize(char* dst, const char* src, size_t max) {
    size_t i = 0;
    for (; src[i] && i < max - 1; i++) {
        char c = src[i];
        if      (c == '"')  dst[i] = '\'';
        else if (c == '\\') dst[i] = ' ';
        else if (c == '\n') dst[i] = ' ';
        else if (c == '\r') dst[i] = ' ';
        else                dst[i] = c;
    }
    dst[i] = '\0';
}

// POST body to an OpenAI-compatible chat endpoint (OpenRouter or Cerebras)
// and extract the assistant content from the response.
// Returns the HTTP status code (negative = transport error, e.g. no route /
// TLS fail). Success = returns 200 AND response is non-empty; any other HTTP
// code is worth retrying on a different model, a transport error is not.
static int post_and_parse(const char* url, const char* api_key,
                           const char* body, size_t body_len,
                           char* response, size_t max_len) {
    // Heap-allocate: WiFiClientSecure SSL context is too large for stack (~20KB)
    response[0] = '\0';
    WiFiClientSecure* sc = new WiFiClientSecure;
    if (!sc) { Serial.println("[OR] SSL client OOM"); return -1000; }
    sc->setInsecure();

    int    code = 0;
    size_t nout = 0;
    String resp;

    // Scope HTTPClient so it destructs BEFORE delete sc (avoids use-after-free crash)
    {
        HTTPClient http;
        http.setTimeout(OR_TIMEOUT_MS);
        if (!http.begin(*sc, url)) {
            Serial.println("[OR] http.begin FAILED");
            delete sc;
            return -1001;
        }
        http.addHeader("Content-Type",  "application/json");
        char auth[240];
        snprintf(auth, sizeof(auth), "Bearer %s", api_key);
        http.addHeader("Authorization", auth);
        http.addHeader("HTTP-Referer",  "https://idea-capture.local");
        http.addHeader("X-Title",       "Idea Capture");

        Serial.printf("[OR] POST %u bytes...\n", (unsigned)body_len);
        code = http.POST((uint8_t*)body, body_len);
        Serial.printf("[OR] HTTP %d\n", code);

        if (code == 200) {
            resp = http.getString();
        } else {
            String err = http.getString();
            Serial.printf("[OR] Error: %s\n", err.substring(0, 300).c_str());
        }
        http.end();
    } // http destroyed here — sc still valid

    delete sc;

    if (code != 200) return code;
    Serial.printf("[OR] Response: %s\n", resp.substring(0, 400).c_str());

    // Find the first "content": in the response (inside choices[0].message)
    const char* p = strstr(resp.c_str(), "\"content\":");
    if (!p) { Serial.println("[OR] No 'content' key in response"); return code; }
    p += 10; // skip "content":
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') { Serial.println("[OR] content value not a string"); return code; }
    p++; // skip opening "

    // Copy until unescaped closing quote, expanding \n \t \" \\
    nout = 0;
    while (*p && nout < max_len - 1) {
        if (*p == '\\') {
            p++;
            if (!*p) break;
            switch (*p) {
                case 'n':  response[nout++] = '\n'; break;
                case 't':  response[nout++] = ' ';  break;
                case '"':  response[nout++] = '"';  break;
                case '\\': response[nout++] = '\\'; break;
                default:   response[nout++] = *p;   break;
            }
        } else if (*p == '"') {
            break;
        } else {
            response[nout++] = *p;
        }
        p++;
    }
    response[nout] = '\0';
    Serial.printf("[OR] Extracted (%u chars): %.200s\n", (unsigned)nout, response);
    return code;
}

bool openrouter_ask_text(const char* question, const char* context,
                          char* response, size_t max_len) {
    char q[256], ctx[512];
    json_sanitize(q,   question,              sizeof(q));
    json_sanitize(ctx, context ? context : "", sizeof(ctx));

    char* body = (char*)ps_malloc(1200);
    if (!body) { Serial.println("[OR] text body OOM"); return false; }

    bool ok = false;

    // ── Tier 1: Cerebras (own key — fast, no shared limits) ──────────────────
    // Plain OpenAI body: no "reasoning" field (OpenRouter-specific).
    if (!ok && strlen(CEREBRAS_API_KEY) > 0) {
        for (size_t m = 0; m < sizeof(CB_TEXT_MODELS)/sizeof(CB_TEXT_MODELS[0]); m++) {
            size_t len;
            if (strlen(ctx) > 0) {
                len = snprintf(body, 1200,
                    "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\","
                    "\"content\":\"%s\\n\\nContext:\\n%s\"}],\"max_tokens\":%d}",
                    CB_TEXT_MODELS[m], q, ctx, OR_MAX_TOKENS);
            } else {
                len = snprintf(body, 1200,
                    "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\","
                    "\"content\":\"%s\"}],\"max_tokens\":%d}",
                    CB_TEXT_MODELS[m], q, OR_MAX_TOKENS);
            }
            Serial.printf("[OR] Cerebras %s...\n", CB_TEXT_MODELS[m]);
            int code = post_and_parse(CB_URL, CEREBRAS_API_KEY, body, len, response, max_len);
            if (code == 200 && response[0]) { ok = true; break; }
            if (code < 0) break;  // transport dead — fall through to OpenRouter anyway
            Serial.printf("[OR] Cerebras %s failed (HTTP %d)\n", CB_TEXT_MODELS[m], code);
        }
    }

    // ── Tier 2: OpenRouter free models ────────────────────────────────────────
    if (!ok) {
        for (size_t m = 0; m < sizeof(OR_TEXT_MODELS)/sizeof(OR_TEXT_MODELS[0]); m++) {
            size_t len;
            if (strlen(ctx) > 0) {
                len = snprintf(body, 1200,
                    "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\","
                    "\"content\":\"%s\\n\\nContext:\\n%s\"}],\"max_tokens\":%d,"
                    "\"reasoning\":{\"enabled\":false}}",
                    OR_TEXT_MODELS[m], q, ctx, OR_MAX_TOKENS);
            } else {
                len = snprintf(body, 1200,
                    "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\","
                    "\"content\":\"%s\"}],\"max_tokens\":%d,"
                    "\"reasoning\":{\"enabled\":false}}",
                    OR_TEXT_MODELS[m], q, OR_MAX_TOKENS);
            }
            int code = post_and_parse(OR_URL, OPENROUTER_API_KEY, body, len, response, max_len);
            if (code == 200 && response[0]) { ok = true; break; }
            if (code < 0) break;  // transport dead — another model won't help
            Serial.printf("[OR] model %s failed (HTTP %d) — trying next\n",
                          OR_TEXT_MODELS[m], code);
        }
    }
    free(body);
    return ok;
}

bool openrouter_ask_vision(const char* question, const uint8_t* jpeg,
                            size_t jpeg_len, char* response, size_t max_len) {
    // Base64-encode the JPEG
    size_t b64_size = ((jpeg_len + 2) / 3) * 4 + 1;
    uint8_t* b64 = (uint8_t*)ps_malloc(b64_size + 1);
    if (!b64) { Serial.println("[OR] b64 OOM"); return false; }

    size_t written = 0;
    if (mbedtls_base64_encode(b64, b64_size + 1, &written, jpeg, jpeg_len) != 0) {
        free(b64);
        Serial.println("[OR] base64 encode FAILED");
        return false;
    }
    b64[written] = '\0';
    Serial.printf("[OR] Base64: %u bytes\n", (unsigned)written);

    char q[256];
    json_sanitize(q, question, sizeof(q));

    char suffix[320];
    size_t slen = snprintf(suffix, sizeof(suffix),
        "\"}},{\"type\":\"text\",\"text\":\"%s\"}]}],\"max_tokens\":%d,"
        "\"reasoning\":{\"enabled\":false}}",
        q, OR_MAX_TOKENS);

    char prefix[220];
    size_t body_sz = sizeof(prefix) + written + slen + 8;
    char* body = (char*)ps_malloc(body_sz);
    if (!body) { free(b64); Serial.println("[OR] vision body OOM"); return false; }

    bool ok = false;
    for (size_t m = 0; m < sizeof(OR_VISION_MODELS)/sizeof(OR_VISION_MODELS[0]); m++) {
        size_t prefix_len = snprintf(prefix, sizeof(prefix),
            "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\","
            "\"content\":[{\"type\":\"image_url\",\"image_url\":"
            "{\"url\":\"data:image/jpeg;base64,", OR_VISION_MODELS[m]);

        char* cur = body;
        memcpy(cur, prefix, prefix_len); cur += prefix_len;
        memcpy(cur, b64, written);       cur += written;
        memcpy(cur, suffix, slen);       cur += slen;
        *cur = '\0';

        size_t body_len = (size_t)(cur - body);
        Serial.printf("[OR] Vision body: %u bytes (model %s)\n",
                      (unsigned)body_len, OR_VISION_MODELS[m]);

        int code = post_and_parse(OR_URL, OPENROUTER_API_KEY, body, body_len, response, max_len);
        if (code == 200 && response[0]) { ok = true; break; }
        if (code < 0) break;  // transport dead — another model won't help
        Serial.printf("[OR] vision model failed (HTTP %d) — trying next\n", code);
    }
    free(b64);
    free(body);
    return ok;
}
