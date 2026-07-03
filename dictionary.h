#pragma once
#include <Arduino.h>

// Fetch a learner's-dictionary style definition + romanized Tamil meaning via OpenRouter LLM.
// Fills out_def with: "word\n(pos)\ndefinition\nTA: tamil meaning\neg. example"
// Returns false on network error or empty word.
bool dict_lookup(const char* word, char* out_def, size_t out_max);
