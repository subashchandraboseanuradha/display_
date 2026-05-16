#include "deepgram.h"
#include "secrets.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// Deepgram pre-recorded API — raw PCM, no WAV header needed
#define DG_URL "https://api.deepgram.com/v1/listen" \
               "?encoding=linear16&sample_rate=16000&channels=1" \
               "&model=nova-2&language=en"

bool deepgram_transcribe(const uint8_t* pcm_data, size_t pcm_size,
                         char* out_transcript, size_t out_max) {
    if (!pcm_data || pcm_size == 0) {
        Serial.println("[DG] No audio data");
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[DG] WiFi not connected");
        return false;
    }

    Serial.printf("[DG] Sending %u bytes to Deepgram...\n", (unsigned)pcm_size);

    // Heap-allocate so SSL context doesn't blow the stack
    WiFiClientSecure* sc = new WiFiClientSecure;
    if (!sc) { Serial.println("[DG] OOM"); return false; }
    sc->setInsecure(); // MVP: skip cert verification

    HTTPClient http;
    http.begin(*sc, DG_URL);
    http.setTimeout(25000); // large upload can take ~10s on slow WiFi
    http.addHeader("Authorization", "Token " DEEPGRAM_API_KEY);
    http.addHeader("Content-Type",  "audio/raw");

    // cast away const — HTTPClient::POST doesn't modify the buffer
    int code = http.POST(const_cast<uint8_t*>(pcm_data), pcm_size);

    if (code != 200) {
        Serial.printf("[DG] HTTP error: %d\n", code);
        Serial.println(http.getString().substring(0, 300));
        http.end();
        delete sc;
        return false;
    }

    String body = http.getString();
    http.end();
    delete sc;

    Serial.printf("[DG] Response %d bytes\n", body.length());

    // Parse: "alternatives":[{"transcript":"...","confidence":...}]
    int idx = body.indexOf("\"transcript\":\"");
    if (idx < 0) {
        Serial.println("[DG] No transcript field in response");
        Serial.println(body.substring(0, 400));
        return false;
    }
    idx += 14; // skip past  "transcript":"
    int end = body.indexOf('"', idx);
    if (end < 0) return false;

    body.substring(idx, end).toCharArray(out_transcript, out_max);
    Serial.printf("[DG] Transcript: \"%s\"\n", out_transcript);
    return true;
}
