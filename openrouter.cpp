#include "openrouter.h"
#include "secrets.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "mbedtls/base64.h"

#define OR_URL          "https://openrouter.ai/api/v1/chat/completions"
#define OR_MODEL_VISION "nvidia/nemotron-nano-12b-v2-vl:free"
#define OR_MODEL_TEXT   "meta-llama/llama-3.2-3b-instruct:free"
#define OR_MAX_TOKENS   300
#define OR_TIMEOUT_MS   30000

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

// POST body to OpenRouter and extract the assistant content from the response.
static bool post_and_parse(const char* body, size_t body_len,
                            char* response, size_t max_len) {
    // Heap-allocate: WiFiClientSecure SSL context is too large for stack (~20KB)
    WiFiClientSecure* sc = new WiFiClientSecure;
    if (!sc) { Serial.println("[OR] SSL client OOM"); return false; }
    sc->setInsecure();

    int    code = 0;
    size_t nout = 0;
    String resp;

    // Scope HTTPClient so it destructs BEFORE delete sc (avoids use-after-free crash)
    {
        HTTPClient http;
        http.setTimeout(OR_TIMEOUT_MS);
        if (!http.begin(*sc, OR_URL)) {
            Serial.println("[OR] http.begin FAILED");
            delete sc;
            return false;
        }
        http.addHeader("Content-Type",  "application/json");
        http.addHeader("Authorization", "Bearer " OPENROUTER_API_KEY);
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

    if (code != 200) return false;
    Serial.printf("[OR] Response: %s\n", resp.substring(0, 400).c_str());

    // Find the first "content": in the response (inside choices[0].message)
    const char* p = strstr(resp.c_str(), "\"content\":");
    if (!p) { Serial.println("[OR] No 'content' key in response"); return false; }
    p += 10; // skip "content":
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') { Serial.println("[OR] content value not a string"); return false; }
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
    return (nout > 0);
}

bool openrouter_ask_text(const char* question, const char* context,
                          char* response, size_t max_len) {
    char q[256], ctx[512];
    json_sanitize(q,   question,              sizeof(q));
    json_sanitize(ctx, context ? context : "", sizeof(ctx));

    char* body = (char*)ps_malloc(1200);
    if (!body) { Serial.println("[OR] text body OOM"); return false; }

    size_t len;
    if (strlen(ctx) > 0) {
        len = snprintf(body, 1200,
            "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\","
            "\"content\":\"%s\\n\\nContext:\\n%s\"}],\"max_tokens\":%d,"
            "\"reasoning\":{\"enabled\":false}}",
            OR_MODEL_TEXT, q, ctx, OR_MAX_TOKENS);
    } else {
        len = snprintf(body, 1200,
            "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\","
            "\"content\":\"%s\"}],\"max_tokens\":%d,"
            "\"reasoning\":{\"enabled\":false}}",
            OR_MODEL_TEXT, q, OR_MAX_TOKENS);
    }

    bool ok = post_and_parse(body, len, response, max_len);
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

    // Build body: fixed prefix + base64 blob + suffix
    const char* prefix =
        "{\"model\":\"" OR_MODEL_VISION "\",\"messages\":[{\"role\":\"user\","
        "\"content\":[{\"type\":\"image_url\",\"image_url\":"
        "{\"url\":\"data:image/jpeg;base64,";

    char suffix[320];
    size_t slen = snprintf(suffix, sizeof(suffix),
        "\"}},{\"type\":\"text\",\"text\":\"%s\"}]}],\"max_tokens\":%d,"
        "\"reasoning\":{\"enabled\":false}}",
        q, OR_MAX_TOKENS);

    size_t prefix_len = strlen(prefix);
    size_t body_sz    = prefix_len + written + slen + 8;
    char* body = (char*)ps_malloc(body_sz);
    if (!body) { free(b64); Serial.println("[OR] vision body OOM"); return false; }

    char* cur = body;
    memcpy(cur, prefix, prefix_len); cur += prefix_len;
    memcpy(cur, b64, written);       cur += written;
    memcpy(cur, suffix, slen);       cur += slen;
    *cur = '\0';
    free(b64);

    size_t body_len = (size_t)(cur - body);
    Serial.printf("[OR] Vision body: %u bytes\n", (unsigned)body_len);

    bool ok = post_and_parse(body, body_len, response, max_len);
    free(body);
    return ok;
}
