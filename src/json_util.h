// Minimal JSON helpers on top of jsmn (flat objects only).
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifndef JSMN_H
#define JSMN_HEADER
#endif
#include "jsmn.h"

#define JSON_MAX_TOKENS 96

typedef struct {
    const char *js;
    jsmntok_t tok[JSON_MAX_TOKENS];
    int count;
} json_doc_t;

// Parse; returns false unless the top level is an object.
bool json_parse(json_doc_t *doc, const char *js, size_t len);

// Iterate top-level members: i starts at 0, returns false when done.
// On success *key_tok and *val_tok index into doc->tok.
bool json_next_member(const json_doc_t *doc, int *i, int *key_tok, int *val_tok);

bool json_tok_eq(const json_doc_t *doc, int t, const char *s);
bool json_is_null(const json_doc_t *doc, int t);
bool json_is_string(const json_doc_t *doc, int t);
// Copy a value (unescaping strings) into out. Primitives are copied as-is.
void json_tok_str(const json_doc_t *doc, int t, char *out, size_t out_size);
bool json_tok_bool(const json_doc_t *doc, int t, bool *out);
bool json_tok_double(const json_doc_t *doc, int t, double *out);

// Find a top-level member by key; returns value token or -1.
int json_find(const json_doc_t *doc, const char *key);

// Append s to out as a JSON string literal (with quotes and escaping).
// Returns new length, or out_size on truncation.
size_t json_append_string(char *out, size_t pos, size_t out_size, const char *s);
