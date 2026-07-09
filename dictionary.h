#pragma once
#include <Arduino.h>

// Fetch a learner's-dictionary style definition + romanized Tamil meaning via OpenRouter LLM.
// Fills out_def with: "word\n(pos)\ndefinition\nTA: tamil meaning\neg. example"
// Returns false on network error or empty word.
bool dict_lookup(const char* word, char* out_def, size_t out_max);

// Autocomplete from the offline SD dictionary (/dict_en.bin): fills `out`
// with up to `max_out` words starting with `prefix` (each entry 24 bytes,
// null-terminated). Returns the number found — 0 if the dict file is absent.
int dict_suggest(const char* prefix, char out[][24], int max_out);

// Translate an English sentence to Tamil via OpenRouter (WiFi required).
// Fills out with: "TA: tamil in English letters\nEN: simple English meaning"
bool translate_text(const char* english, char* out, size_t out_max);
