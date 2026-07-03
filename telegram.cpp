#include "telegram.h"
#include "secrets.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#define TG_API "https://api.telegram.org/bot" TELEGRAM_BOT_TOKEN

// JSON-escape into dst: preserves newlines as \n (Telegram renders them),
// escapes quotes/backslashes, drops \r. Same spirit as openrouter.cpp's
// json_sanitize but keeps newlines instead of flattening them, since these
// messages (esp. "Q: ...\nA: ...") are meant to stay multi-line on the phone.
static void json_escape(char* dst, size_t dst_max, const char* src) {
    size_t o = 0;
    for (; *src && o + 2 < dst_max; src++) {
        char c = *src;
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = c; }
        else if (c == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
        else if (c == '\r') { /* skip */ }
        else dst[o++] = c;
    }
    dst[o] = '\0';
}

bool telegram_send_text(const char* text) {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (!text || !text[0]) return false;

    WiFiClientSecure* sc = new WiFiClientSecure;
    if (!sc) { Serial.println("[TG] SSL client OOM"); return false; }
    sc->setInsecure();

    char escaped[900];
    json_escape(escaped, sizeof(escaped), text);

    char* body = (char*)ps_malloc(1024);
    if (!body) {
        Serial.println("[TG] body OOM");
        delete sc;
        return false;
    }
    size_t len = snprintf(body, 1024,
        "{\"chat_id\":\"%s\",\"text\":\"%s\"}", TELEGRAM_CHAT_ID, escaped);

    bool ok = false;
    {
        HTTPClient http;
        http.setTimeout(10000);
        if (http.begin(*sc, TG_API "/sendMessage")) {
            http.addHeader("Content-Type", "application/json");
            int code = http.POST((uint8_t*)body, len);
            Serial.printf("[TG] sendMessage -> %d\n", code);
            ok = (code == 200);
            if (!ok) Serial.println(http.getString().substring(0, 200));
            http.end();
        } else {
            Serial.println("[TG] http.begin FAILED (text)");
        }
    }
    free(body);
    delete sc;
    return ok;
}

bool telegram_send_photo(const uint8_t* jpeg, size_t jpeg_len, const char* caption) {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (!jpeg || jpeg_len == 0) return false;

    WiFiClientSecure* sc = new WiFiClientSecure;
    if (!sc) { Serial.println("[TG] SSL client OOM"); return false; }
    sc->setInsecure();

    // Telegram's sendPhoto needs a real multipart/form-data upload (no
    // base64-in-JSON option for raw device bytes) — build it by hand, same
    // "assemble one buffer, single POST" approach openrouter.cpp uses for
    // its base64 vision body.
    const char* boundary = "----xioaesp32boundary";
    char cap_esc[200];
    json_escape(cap_esc, sizeof(cap_esc), caption ? caption : "");

    // Sized for worst case (boundary x3 + full 200-byte caption + literal
    // headers) with margin. snprintf's return value is the length it WOULD
    // write, not what actually fit — if this buffer were too small, the
    // memcpy below would read past the end using that inflated length. Sized
    // generously to make truncation essentially impossible, but clamp anyway.
    char prefix[700];
    int prefix_len = snprintf(prefix, sizeof(prefix),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n%s\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"caption\"\r\n\r\n%s\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"photo\"; filename=\"photo.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n",
        boundary, TELEGRAM_CHAT_ID, boundary, cap_esc, boundary);
    if (prefix_len < 0) prefix_len = 0;
    if (prefix_len >= (int)sizeof(prefix)) prefix_len = sizeof(prefix) - 1;

    char suffix[64];
    int suffix_len = snprintf(suffix, sizeof(suffix), "\r\n--%s--\r\n", boundary);
    if (suffix_len < 0) suffix_len = 0;
    if (suffix_len >= (int)sizeof(suffix)) suffix_len = sizeof(suffix) - 1;

    size_t body_len = (size_t)prefix_len + jpeg_len + (size_t)suffix_len;
    uint8_t* body = (uint8_t*)ps_malloc(body_len);
    if (!body) {
        Serial.println("[TG] photo body OOM");
        delete sc;
        return false;
    }
    memcpy(body, prefix, prefix_len);
    memcpy(body + prefix_len, jpeg, jpeg_len);
    memcpy(body + prefix_len + jpeg_len, suffix, suffix_len);

    bool ok = false;
    {
        HTTPClient http;
        http.setTimeout(20000);
        if (http.begin(*sc, TG_API "/sendPhoto")) {
            char ctype[64];
            snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", boundary);
            http.addHeader("Content-Type", ctype);
            int code = http.POST(body, body_len);
            Serial.printf("[TG] sendPhoto -> %d (%u bytes)\n", code, (unsigned)body_len);
            ok = (code == 200);
            if (!ok) Serial.println(http.getString().substring(0, 200));
            http.end();
        } else {
            Serial.println("[TG] http.begin FAILED (photo)");
        }
    }
    free(body);
    delete sc;
    return ok;
}
