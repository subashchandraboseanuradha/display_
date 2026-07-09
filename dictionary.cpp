#include "dictionary.h"
#include "openrouter.h"
#include "secrets.h"
#include "sdcard.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD.h>
#define USE_TFT_ESPI_LIBRARY
#include <TFT_eSPI.h>
// lv_xiao_round_screen.h has inline function definitions — including it in
// more than one .cpp causes duplicate symbol errors (see camera.cpp). Declare
// tft directly, same pattern, needed for sd_reinit(tft.getSPIinstance()).
extern TFT_eSPI tft;

#define MW_LEARNERS_URL   "https://www.dictionaryapi.com/api/v3/references/learners/json/"
#define MW_COLLEGIATE_URL "https://www.dictionaryapi.com/api/v3/references/collegiate/json/"

// ── Offline dictionary (SD card) ───────────────────────────────────────────────
// /dict_en.bin, /dict_ta.bin — sorted, fixed-length-record binary files
// produced offline (see design_language/project_state memory for the prep
// pipeline). Not present until the user copies them onto the card; every
// lookup here degrades gracefully (SD.exists() guard) if they're missing.
#define EN_WORD_LEN   24
#define EN_POS_LEN    16   // some Wiktextract pos tags run past 7 chars (e.g. "particle")
#define EN_DEF_LEN   216
#define EN_RECORD_LEN (EN_WORD_LEN + EN_POS_LEN + EN_DEF_LEN)   // 256

#define TA_WORD_LEN   24
#define TA_TAMIL_LEN  64
#define TA_ROMAN_LEN  40
#define TA_RECORD_LEN (TA_WORD_LEN + TA_TAMIL_LEN + TA_ROMAN_LEN)  // 128

// Binary search over a sorted file of fixed-length records. `word` is
// compared against the first `word_field_len` bytes of each record (a
// null-padded C string, so strncmp works directly). Reused for both
// dict_en.bin and dict_ta.bin — only the record layout differs per caller.
static bool sd_binary_search(File& f, const char* word, size_t record_len,
                              size_t word_field_len, uint8_t* out_record) {
    uint32_t file_size = f.size();
    if (record_len == 0 || file_size % record_len != 0) return false;
    long lo = 0, hi = (long)(file_size / record_len) - 1;

    char key[EN_WORD_LEN] = {0};  // EN_WORD_LEN == TA_WORD_LEN, both 24
    strncpy(key, word, sizeof(key) - 1);

    while (lo <= hi) {
        long mid = lo + (hi - lo) / 2;
        if (!f.seek((uint32_t)mid * record_len)) return false;
        if (f.read(out_record, record_len) != (int)record_len) return false;
        int cmp = strncmp(key, (const char*)out_record, word_field_len);
        if (cmp == 0) return true;
        if (cmp < 0) hi = mid - 1; else lo = mid + 1;
    }
    return false;
}

// Looks up `word` in both offline files under a single SD mount. Each output
// is only written if that file's lookup hits — check *en_found/*ta_found.
static void sd_lookup(const char* word,
                       char* pos_out, size_t pos_max, char* def_out, size_t def_max, bool* en_found,
                       char* tamil_out, size_t tamil_max, char* roman_out, size_t roman_max, bool* ta_found) {
    *en_found = false;
    *ta_found = false;
    sd_reinit(tft.getSPIinstance());

    if (SD.exists("/dict_en.bin")) {
        File f = SD.open("/dict_en.bin", FILE_READ);
        if (f) {
            uint8_t rec[EN_RECORD_LEN];
            if (sd_binary_search(f, word, EN_RECORD_LEN, EN_WORD_LEN, rec)) {
                strncpy(pos_out, (const char*)(rec + EN_WORD_LEN), pos_max - 1);
                pos_out[pos_max - 1] = '\0';
                strncpy(def_out, (const char*)(rec + EN_WORD_LEN + EN_POS_LEN), def_max - 1);
                def_out[def_max - 1] = '\0';
                *en_found = true;
            }
            f.close();
        }
    }

    if (SD.exists("/dict_ta.bin")) {
        File f = SD.open("/dict_ta.bin", FILE_READ);
        if (f) {
            uint8_t rec[TA_RECORD_LEN];
            if (sd_binary_search(f, word, TA_RECORD_LEN, TA_WORD_LEN, rec)) {
                strncpy(tamil_out, (const char*)(rec + TA_WORD_LEN), tamil_max - 1);
                tamil_out[tamil_max - 1] = '\0';
                strncpy(roman_out, (const char*)(rec + TA_WORD_LEN + TA_TAMIL_LEN), roman_max - 1);
                roman_out[roman_max - 1] = '\0';
                *ta_found = true;
            }
            f.close();
        }
    }

    sd_release();
    if (*en_found) Serial.printf("[DICT-SD] en hit: %s (%s): %s\n", word, pos_out, def_out);
    if (*ta_found) Serial.printf("[DICT-SD] ta hit: %s -> %s (%s)\n", word, tamil_out, roman_out);
    if (!*en_found && !*ta_found) Serial.printf("[DICT-SD] miss (or dict files absent): %s\n", word);
}

// Autocomplete: lower-bound binary search for `prefix` in /dict_en.bin, then
// collect consecutive records that still start with it. Same fixed-record
// layout as sd_binary_search — only the compare length differs (prefix, not
// the full 24-byte field).
int dict_suggest(const char* prefix, char out[][24], int max_out) {
    size_t plen = strlen(prefix);
    if (plen == 0 || max_out <= 0) return 0;

    int found = 0;
    sd_reinit(tft.getSPIinstance());
    if (SD.exists("/dict_en.bin")) {
        File f = SD.open("/dict_en.bin", FILE_READ);
        if (f) {
            uint32_t file_size = f.size();
            long n = (long)(file_size / EN_RECORD_LEN);
            uint8_t rec[EN_RECORD_LEN];

            // Lower bound: first record whose word >= prefix
            long lo = 0, hi = n;
            while (lo < hi) {
                long mid = lo + (hi - lo) / 2;
                if (!f.seek((uint32_t)mid * EN_RECORD_LEN)) { lo = n; break; }
                if (f.read(rec, EN_RECORD_LEN) != EN_RECORD_LEN) { lo = n; break; }
                if (strncmp((const char*)rec, prefix, plen) < 0) lo = mid + 1;
                else hi = mid;
            }

            // Collect matches from the lower bound forward
            for (long i = lo; i < n && found < max_out; i++) {
                if (!f.seek((uint32_t)i * EN_RECORD_LEN)) break;
                if (f.read(rec, EN_RECORD_LEN) != EN_RECORD_LEN) break;
                if (strncmp((const char*)rec, prefix, plen) != 0) break;
                strncpy(out[found], (const char*)rec, 23);
                out[found][23] = '\0';
                found++;
            }
            f.close();
        }
    }
    sd_release();
    return found;
}

// English sentence → Tamil, LLM only (no offline path for sentences).
bool translate_text(const char* english, char* out, size_t out_max) {
    out[0] = '\0';
    if (!english || english[0] == '\0') return false;
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[TRANS] WiFi not connected");
        return false;
    }
    char prompt[560];
    snprintf(prompt, sizeof(prompt),
        "Translate this English text to Tamil for a Tamil-speaking English learner: \"%.320s\". "
        "Reply in EXACTLY 2 lines, nothing else:\n"
        "TA: Tamil translation written in English letters\n"
        "EN: the same meaning in very simple English words",
        english);
    if (!openrouter_ask_text(prompt, nullptr, out, out_max)) {
        Serial.println("[TRANS] OpenRouter call failed");
        return false;
    }
    Serial.printf("[TRANS] OK: %s\n", out);
    return true;
}

// Strip {xx}...{/xx}-style formatting tokens Merriam-Webster sometimes embeds
// in definition text (e.g. "{bc}", "{it}...{/it}"), copying everything else.
static void mw_strip_tokens(char* dst, const char* src, size_t max) {
    size_t o = 0;
    while (*src && o < max - 1) {
        if (*src == '{') {
            const char* end = strchr(src, '}');
            if (!end) break;
            src = end + 1;
            continue;
        }
        dst[o++] = *src++;
    }
    dst[o] = '\0';
}

// Extract the string value of a "key":"value" pair, searching from `from`.
static bool mw_extract_kv(const char* from, const char* key, char* out, size_t out_max) {
    const char* p = strstr(from, key);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ') p++;
    if (*p != '"') return false;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o < out_max - 1) {
        if (*p == '\\' && p[1]) p++;
        out[o++] = *p++;
    }
    out[o] = '\0';
    return o > 0;
}

// Extract the first quoted string in a JSON array, given a pointer just past
// its opening '[' (e.g. from strstr(body, "\"shortdef\":[") + 12).
static bool mw_first_array_string(const char* from, char* out, size_t out_max) {
    const char* p = strchr(from, '"');
    if (!p) return false;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o < out_max - 1) {
        if (*p == '\\' && p[1]) p++;
        out[o++] = *p++;
    }
    out[o] = '\0';
    return o > 0;
}

// Real dictionary lookup via a Merriam-Webster API (Learner's or Collegiate —
// same response shape, just different endpoint/key, so one function serves
// both tiers). A "not found" response is a bare array of spelling-suggestion
// strings (no "fl"/"shortdef" objects), so requiring both of those to parse
// doubles as the not-found check.
static bool mw_lookup(const char* word, const char* url_prefix, const char* api_key,
                       char* fl_out, size_t fl_max, char* def_out, size_t def_max) {
    WiFiClientSecure* sc = new WiFiClientSecure;
    if (!sc) { Serial.println("[MW] SSL client OOM"); return false; }
    sc->setInsecure();

    char url[160];
    snprintf(url, sizeof(url), "%s%s?key=%s", url_prefix, word, api_key);

    bool ok = false;
    {
        HTTPClient http;
        http.setTimeout(10000);
        if (!http.begin(*sc, url)) {
            Serial.println("[MW] http.begin FAILED");
            delete sc;
            return false;
        }
        int code = http.GET();
        Serial.printf("[MW] GET %s -> %d\n", word, code);
        if (code == 200) {
            String resp = http.getString();
            const char* body = resp.c_str();

            char fl_raw[24] = {0}, def_raw[220] = {0};
            const char* sd = strstr(body, "\"shortdef\":[");
            if (mw_extract_kv(body, "\"fl\":", fl_raw, sizeof(fl_raw)) &&
                sd && mw_first_array_string(sd + 12, def_raw, sizeof(def_raw))) {
                mw_strip_tokens(fl_out, fl_raw, fl_max);
                mw_strip_tokens(def_out, def_raw, def_max);
                Serial.printf("[MW] %s (%s): %s\n", word, fl_out, def_out);
                ok = true;
            } else {
                Serial.println("[MW] word not found (or unexpected response shape)");
            }
        } else {
            Serial.printf("[MW] HTTP error %d\n", code);
        }
        http.end();
    }
    delete sc;
    return ok;
}

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

    // ── Offline SD dictionaries first — instant, free, works with no WiFi ──
    char sd_pos[EN_POS_LEN] = {0}, sd_def[EN_DEF_LEN] = {0};
    char sd_tamil[TA_TAMIL_LEN] = {0}, sd_roman[TA_ROMAN_LEN] = {0};
    bool en_found = false, ta_found = false;
    sd_lookup(clean, sd_pos, sizeof(sd_pos), sd_def, sizeof(sd_def), &en_found,
              sd_tamil, sizeof(sd_tamil), sd_roman, sizeof(sd_roman), &ta_found);
    if (ta_found && sd_roman[0] == '\0') ta_found = false;  // no font for native script

    if (en_found) {
        if (ta_found) {
            // Fully offline hit — zero network calls. No example sentence in
            // the offline data (only in the LLM-augmented paths below).
            snprintf(out_def, out_max, "%s\n(%s)\n%s\nTA: %s\neg. -",
                     clean, sd_pos, sd_def, sd_roman);
            Serial.printf("[DICT] OK (offline SD, en+ta): %s\n", clean);
            return true;
        }
        // English is offline but no local Tamil translation — ask the LLM
        // just for that, same as the MW-hit path always has.
        if (WiFi.status() == WL_CONNECTED) {
            char llm_out[400];
            char prompt[320];
            snprintf(prompt, sizeof(prompt),
                "The English word '%s' (%s) means: %s. "
                "Reply in EXACTLY 2 lines, nothing else:\n"
                "TA: Tamil meaning written in English letters\n"
                "eg. short example sentence using the word",
                clean, sd_pos, sd_def);
            if (openrouter_ask_text(prompt, nullptr, llm_out, sizeof(llm_out))) {
                snprintf(out_def, out_max, "%s\n(%s)\n%s\n%s", clean, sd_pos, sd_def, llm_out);
                Serial.printf("[DICT] OK (offline SD en + AI Tamil): %s\n", clean);
                return true;
            }
        }
        snprintf(out_def, out_max, "%s\n(%s)\n%s\nTA: -\neg. -", clean, sd_pos, sd_def);
        Serial.printf("[DICT] OK (offline SD, en only): %s\n", clean);
        return true;
    }

    // No local English entry — everything from here on needs the network.
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[DICT] Not in offline dictionary and WiFi not connected");
        return false;
    }

    char llm_out[400];
    char fl[24] = {0}, def[220] = {0};

    // Learner's API first (simpler wording), Collegiate as a broader-coverage
    // fallback if Learner's doesn't have the word.
    bool mw_hit = mw_lookup(clean, MW_LEARNERS_URL, MERRIAM_WEBSTER_API_KEY,
                             fl, sizeof(fl), def, sizeof(def));
    if (!mw_hit) {
        Serial.println("[DICT] Learner's miss — trying Collegiate...");
        mw_hit = mw_lookup(clean, MW_COLLEGIATE_URL, MERRIAM_WEBSTER_COLLEGIATE_API_KEY,
                            fl, sizeof(fl), def, sizeof(def));
    }

    if (mw_hit) {
        char prompt[320];
        snprintf(prompt, sizeof(prompt),
            "The English word '%s' (%s) means: %s. "
            "Reply in EXACTLY 2 lines, nothing else:\n"
            "TA: Tamil meaning written in English letters\n"
            "eg. short example sentence using the word",
            clean, fl, def);

        if (openrouter_ask_text(prompt, nullptr, llm_out, sizeof(llm_out))) {
            snprintf(out_def, out_max, "%s\n(%s)\n%s\n%s", clean, fl, def, llm_out);
        } else {
            // Real definition still beats nothing — show it without Tamil/example.
            Serial.println("[DICT] AI Tamil gloss failed — showing MW definition only");
            snprintf(out_def, out_max, "%s\n(%s)\n%s\nTA: -\neg. -", clean, fl, def);
        }
        Serial.printf("[DICT] OK (Merriam-Webster): %s\n", clean);
        return true;
    }

    // Both MW tiers missed — fall back to a fully LLM-guessed entry.
    Serial.println("[DICT] Merriam-Webster miss (both tiers) — falling back to AI-only definition");
    char prompt[256];
    snprintf(prompt, sizeof(prompt),
        "Define the English word '%s' for a language learner. "
        "Reply in EXACTLY 4 lines, nothing else:\n"
        "(part of speech)\n"
        "simple definition, max 15 words\n"
        "TA: Tamil meaning written in English letters\n"
        "eg. short example sentence",
        clean);

    if (!openrouter_ask_text(prompt, nullptr, llm_out, sizeof(llm_out))) {
        Serial.println("[DICT] OpenRouter lookup failed");
        return false;
    }

    // Format: WORD\n<llm 4-line response>
    snprintf(out_def, out_max, "%s\n%s", clean, llm_out);

    Serial.printf("[DICT] OK (AI-only): %s\n", clean);
    return true;
}
