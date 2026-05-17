#include "deepgram.h"
#include "secrets.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// Deepgram pre-recorded API — raw PCM, no WAV header needed
#define DG_URL "https://api.deepgram.com/v1/listen" \
               "?encoding=linear16&sample_rate=8000&channels=1" \
               "&model=nova-2&language=en"

bool deepgram_transcribe(const uint8_t* pcm_data, size_t pcm_size,
                         char* out_transcript, size_t out_max) {
    out_transcript[0] = '\0';

    if (!pcm_data || pcm_size == 0) {
        Serial.println("[DG] No audio data");
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[DG] WiFi not connected");
        return false;
    }

    Serial.printf("[DG][T+%lums] Sending %u bytes...\n", millis(), (unsigned)pcm_size);

    // Heap-allocate: WiFiClientSecure SSL context is too large for stack (~20KB)
    WiFiClientSecure* sc = new WiFiClientSecure;
    if (!sc) { Serial.println("[DG] OOM allocating SSL client"); return false; }
    sc->setInsecure(); // MVP: skip cert verification

    int  code = 0;
    String body;

    // Scope HTTPClient so it destructs BEFORE delete sc.
    // If http destructs AFTER delete sc, it accesses freed memory (sc's vtable) → crash PC=0x0.
    {
        HTTPClient http;
        http.begin(*sc, DG_URL);
        http.setTimeout(60000);  // 60s — large payload on slow WiFi needs headroom
        http.addHeader("Authorization", "Token " DEEPGRAM_API_KEY);
        http.addHeader("Content-Type",  "audio/raw");

        code = http.POST(const_cast<uint8_t*>(pcm_data), pcm_size);

        if (code == 200) {
            body = http.getString();
        } else {
            Serial.printf("[DG] HTTP error: %d\n", code);
            Serial.println(http.getString().substring(0, 300));
        }
        http.end();
    } // http destroyed here — sc still valid

    delete sc;  // safe: http is gone

    if (code != 200) return false;

    Serial.printf("[DG][T+%lums] Response %d bytes\n", millis(), body.length());

    // Parse: find "transcript":"<text>"
    int idx = body.indexOf("\"transcript\":\"");
    if (idx < 0) {
        Serial.println("[DG] No transcript in response. First 500 chars:");
        Serial.println(body.substring(0, 500));
        return false;
    }
    idx += 14; // skip past  "transcript":"
    int endq = body.indexOf('"', idx);
    if (endq < 0) { Serial.println("[DG] Malformed JSON"); return false; }

    body.substring(idx, endq).toCharArray(out_transcript, out_max);
    Serial.printf("[DG] Transcript (%d chars): \"%s\"\n",
                  strlen(out_transcript), out_transcript);
    return strlen(out_transcript) > 0;
}
