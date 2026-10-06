// jsmn implementation lives in this translation unit only.
#include "jsmn.h"
#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool json_parse(json_doc_t *doc, const char *js, size_t len) {
    jsmn_parser p;
    jsmn_init(&p);
    doc->js = js;
    doc->count = jsmn_parse(&p, js, len, doc->tok, JSON_MAX_TOKENS);
    return doc->count > 0 && doc->tok[0].type == JSMN_OBJECT;
}

// Index of the token following t and all its children.
static int skip(const json_doc_t *doc, int t) {
    int end = doc->tok[t].end;
    t++;
    while (t < doc->count && doc->tok[t].start < end) t++;
    return t;
}

bool json_next_member(const json_doc_t *doc, int *i, int *key_tok, int *val_tok) {
    int t = *i == 0 ? 1 : *i;
    if (t >= doc->count || doc->tok[t].start >= doc->tok[0].end) return false;
    *key_tok = t;
    *val_tok = t + 1;
    if (*val_tok >= doc->count) return false;
    *i = skip(doc, *val_tok);
    return true;
}

bool json_tok_eq(const json_doc_t *doc, int t, const char *s) {
    const jsmntok_t *k = &doc->tok[t];
    size_t n = (size_t)(k->end - k->start);
    return k->type == JSMN_STRING && strlen(s) == n && strncmp(doc->js + k->start, s, n) == 0;
}

bool json_is_null(const json_doc_t *doc, int t) {
    const jsmntok_t *k = &doc->tok[t];
    return k->type == JSMN_PRIMITIVE && doc->js[k->start] == 'n';
}

bool json_is_string(const json_doc_t *doc, int t) {
    return doc->tok[t].type == JSMN_STRING;
}

void json_tok_str(const json_doc_t *doc, int t, char *out, size_t out_size) {
    const jsmntok_t *k = &doc->tok[t];
    const char *s = doc->js + k->start;
    const char *e = doc->js + k->end;
    size_t o = 0;
    if (!out_size) return;
    if (k->type != JSMN_STRING) {
        size_t n = (size_t)(e - s);
        if (n >= out_size) n = out_size - 1;
        memcpy(out, s, n);
        out[n] = '\0';
        return;
    }
    while (s < e && o + 1 < out_size) {
        char c = *s++;
        if (c == '\\' && s < e) {
            c = *s++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                // Keep ASCII, replace anything else with '?'.
                unsigned v = 0;
                for (int i = 0; i < 4 && s < e; i++, s++) {
                    char h = *s;
                    v = v * 16 + (unsigned)(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
                }
                c = v < 0x80 ? (char)v : '?';
                break;
            }
            default: break;  // \" \\ \/
            }
        }
        out[o++] = c;
    }
    out[o] = '\0';
}

bool json_tok_bool(const json_doc_t *doc, int t, bool *out) {
    const jsmntok_t *k = &doc->tok[t];
    if (k->type != JSMN_PRIMITIVE) return false;
    char c = doc->js[k->start];
    if (c == 't') *out = true;
    else if (c == 'f') *out = false;
    else return false;
    return true;
}

bool json_tok_double(const json_doc_t *doc, int t, double *out) {
    const jsmntok_t *k = &doc->tok[t];
    char buf[40];
    if (k->type != JSMN_PRIMITIVE && k->type != JSMN_STRING) return false;
    json_tok_str(doc, t, buf, sizeof(buf));
    char *end;
    double v = strtod(buf, &end);
    if (end == buf || *end) return false;
    *out = v;
    return true;
}

int json_find(const json_doc_t *doc, const char *key) {
    int i = 0, k, v;
    while (json_next_member(doc, &i, &k, &v)) {
        if (json_tok_eq(doc, k, key)) return v;
    }
    return -1;
}

size_t json_append_string(char *out, size_t pos, size_t out_size, const char *s) {
    if (pos >= out_size) return out_size;
    out[pos++] = '"';
    for (; s && *s; s++) {
        char esc = 0;
        switch (*s) {
        case '"': esc = '"'; break;
        case '\\': esc = '\\'; break;
        case '\n': esc = 'n'; break;
        case '\r': esc = 'r'; break;
        case '\t': esc = 't'; break;
        default: break;
        }
        if (esc) {
            if (pos + 2 >= out_size) return out_size;
            out[pos++] = '\\';
            out[pos++] = esc;
        } else if ((unsigned char)*s < 0x20) {
            if (pos + 6 >= out_size) return out_size;
            pos += (size_t)snprintf(out + pos, out_size - pos, "\\u%04x", (unsigned char)*s);
        } else {
            if (pos + 1 >= out_size) return out_size;
            out[pos++] = *s;
        }
    }
    if (pos + 1 >= out_size) return out_size;
    out[pos++] = '"';
    out[pos] = '\0';
    return pos;
}
