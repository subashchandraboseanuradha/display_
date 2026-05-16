#include "dictionary.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#define DICT_URL "https://api.dictionaryapi.dev/api/v2/entries/en/"

bool dict_lookup(const char* word, char* out_def, size_t out_max) {
    // IMPORTANT: copy + clean word BEFORE touching out_def.
    // Caller may pass the same buffer for both word and out_def.
    char clean[48] = {0};
    int j = 0;
    for (int i = 0; word[i] && j < 47; i++) {
        if (word[i] == ' ') break;          // first word only
        if (isalpha((uint8_t)word[i])) clean[j++] = tolower(word[i]);
    }
    clean[j] = '\0';

    out_def[0] = '\0';  // safe to zero now

    if (j == 0) {
        Serial.println("[DICT] Empty/invalid word after cleaning");
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[DICT] WiFi not connected");
        return false;
    }

    String url = String(DICT_URL) + clean;
    Serial.printf("[DICT] GET %s\n", url.c_str());

    WiFiClientSecure* sc = new WiFiClientSecure;
    if (!sc) { Serial.println("[DICT] OOM"); return false; }
    sc->setInsecure();

    int  code = 0;
    String body;
    {
        HTTPClient http;
        http.begin(*sc, url);
        http.setTimeout(12000);
        http.addHeader("User-Agent", "ESP32-IdeaCapture/1.0");
        code = http.GET();
        body = http.getString();
        http.end();
    }
    delete sc;

    Serial.printf("[DICT] HTTP %d  body_len=%d\n", code, body.length());
    if (body.length() > 0)
        Serial.println(body.substring(0, min((int)body.length(), 300)));

    if (code != 200) {
        Serial.printf("[DICT] API rejected word \"%s\" (HTTP %d)\n", clean, code);
        return false;
    }
    if (body.length() == 0) {
        Serial.println("[DICT] Empty response body");
        return false;
    }

    // Extract partOfSpeech
    char pos[24] = {0};
    int pi = body.indexOf("\"partOfSpeech\":\"");
    if (pi >= 0) {
        pi += 16;
        int pe = body.indexOf('"', pi);
        if (pe > pi) body.substring(pi, pe).toCharArray(pos, sizeof(pos));
    }

    // Extract first definition
    int di = body.indexOf("\"definition\":\"");
    if (di < 0) {
        Serial.println("[DICT] 'definition' field not found in response");
        return false;
    }
    di += 14;
    int de = di;
    while (de < (int)body.length()) {
        if (body[de] == '"' && (de == 0 || body[de-1] != '\\')) break;
        de++;
    }
    if (de <= di) { Serial.println("[DICT] Could not find end of definition"); return false; }

    String def = body.substring(di, de);
    def.replace("\\\"", "\""); def.replace("\\/", "/"); def.replace("\\n", " ");

    // Extract example sentence (within same definitions block, after definition field)
    String example = "";
    int ei = body.indexOf("\"example\":\"", di);
    if (ei >= 0 && ei < de + 300) {  // must be close to the definition
        ei += 11;
        int ee = ei;
        while (ee < (int)body.length()) {
            if (body[ee] == '"' && body[ee-1] != '\\') break;
            ee++;
        }
        if (ee > ei) {
            example = body.substring(ei, ee);
            example.replace("\\\"", "\""); example.replace("\\/", "/"); example.replace("\\n", " ");
        }
    }

    // Format: WORD\n(partOfSpeech)\ndefinition\neg. example
    if (example.length() > 0) {
        snprintf(out_def, out_max, "%s\n(%s)\n%s\neg. %s",
                 clean, pos, def.c_str(), example.c_str());
    } else {
        snprintf(out_def, out_max, "%s\n(%s)\n%s", clean, pos, def.c_str());
    }

    Serial.printf("[DICT] OK: %s | %s\n", clean, def.substring(0,60).c_str());
    if (example.length()) Serial.printf("[DICT] eg: %s\n", example.substring(0,60).c_str());
    return true;
}
