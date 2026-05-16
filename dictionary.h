#pragma once
#include <Arduino.h>

// Fetch definition of a single word from dictionaryapi.dev (free, no API key).
// Fills out_def with: "partOfSpeech: definition text"
// Returns false on network error or word not found.
bool dict_lookup(const char* word, char* out_def, size_t out_max);
