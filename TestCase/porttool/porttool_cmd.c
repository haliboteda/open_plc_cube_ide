// porttool_cmd.c
//
// Command line parsing - see porttool_cmd.h.

#include "porttool_cmd.h"
#include "porttool.h"       /* PORTTOOL_ENABLE */

#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Finds "<key>=" as a whole token, so period= does not match myperiod=. */
static const char *find_key(const char *args, const char *key)
{
    size_t klen = strlen(key);
    const char *p = args;

    while (*p != '\0') {
        while (is_space(*p)) { p++; }
        if (*p == '\0') { break; }

        const char *tok = p;
        /* Step over this token, honouring quotes so a quoted value that
         * contains "key=" is not mistaken for the next parameter. */
        int quoted = 0;
        while (*p != '\0' && (quoted || !is_space(*p))) {
            if (*p == '"') { quoted = !quoted; }
            p++;
        }

        if ((size_t)(p - tok) > klen &&
            strncmp(tok, key, klen) == 0 && tok[klen] == '=') {
            return tok + klen + 1;
        }
    }
    return NULL;
}

int PortCmd_GetStr(const char *args, const char *key, char *out, uint32_t out_len)
{
    const char *v = find_key(args, key);
    uint32_t n = 0;

    if (v == NULL || out_len == 0U) {
        return 0;
    }

    if (*v == '"') {
        v++;
        while (*v != '\0' && *v != '"' && n + 1U < out_len) { out[n++] = *v++; }
    } else {
        while (*v != '\0' && !is_space(*v) && n + 1U < out_len) { out[n++] = *v++; }
    }
    out[n] = '\0';
    return 1;
}

int PortCmd_GetU32(const char *args, const char *key, uint32_t *out)
{
    char buf[24];
    uint32_t v = 0;
    int digits = 0;

    if (!PortCmd_GetStr(args, key, buf, sizeof(buf))) {
        return 0;
    }
    for (const char *p = buf; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
        v = v * 10U + (uint32_t)(*p - '0');
        digits++;
    }
    if (digits == 0) {
        return 0;
    }
    *out = v;
    return 1;
}

int PortCmd_GetMask(const char *args, const char *key, uint32_t max_ch, uint32_t *mask)
{
    char buf[64];
    uint32_t m = 0;
    uint32_t ch = 0;
    int digits = 0;

    if (!PortCmd_GetStr(args, key, buf, sizeof(buf))) {
        return 0;
    }

    for (const char *p = buf; ; p++) {
        if (*p >= '0' && *p <= '9') {
            ch = ch * 10U + (uint32_t)(*p - '0');
            digits++;
        } else if (*p == ',' || *p == '\0') {
            if (digits == 0 || ch < 1U || ch > max_ch) {
                return 0;
            }
            m |= 1U << (ch - 1U);
            ch = 0;
            digits = 0;
            if (*p == '\0') { break; }
        } else {
            return 0;
        }
    }

    *mask = m;
    return 1;
}

int PortCmd_GetPairs(const char *args, const char *key, uint32_t max_ch,
                     uint32_t val_max, uint32_t *vals, uint32_t *touched)
{
    char buf[96];
    /* Parsed into a scratch copy and only committed once the whole value is
     * known good. Writing as we go would leave the channels named before a bad
     * entry already changed, and "the command was refused" would no longer be
     * the same thing as "nothing moved". */
    uint32_t scratch[PORTTOOL_MAX_CHANNELS];
    uint32_t ch = 0, val = 0, seen = 0;
    int ch_digits = 0, val_digits = 0;
    int in_value = 0;

    if (max_ch == 0U || max_ch > PORTTOOL_MAX_CHANNELS) {
        return 0;
    }
    if (!PortCmd_GetStr(args, key, buf, sizeof(buf))) {
        return 0;
    }

    for (const char *p = buf; ; p++) {
        if (*p >= '0' && *p <= '9') {
            if (in_value) {
                val = val * 10U + (uint32_t)(*p - '0');
                val_digits++;
            } else {
                ch = ch * 10U + (uint32_t)(*p - '0');
                ch_digits++;
            }
        } else if (*p == ':') {
            /* A second colon in one entry, or a value before a channel. */
            if (in_value || ch_digits == 0) {
                return 0;
            }
            in_value = 1;
        } else if (*p == ',' || *p == '\0') {
            if (!in_value || ch_digits == 0 || val_digits == 0) {
                return 0;
            }
            if (ch < 1U || ch > max_ch || val > val_max) {
                return 0;
            }
            /* Naming the same channel twice means the caller does not know
             * what it asked for; picking one is worse than saying no. */
            if ((seen & (1U << (ch - 1U))) != 0U) {
                return 0;
            }
            seen |= 1U << (ch - 1U);
            scratch[ch - 1U] = val;

            ch = val = 0;
            ch_digits = val_digits = 0;
            in_value = 0;
            if (*p == '\0') { break; }
        } else {
            return 0;
        }
    }

    if (seen == 0U) {
        return 0;
    }

    for (uint32_t i = 0; i < max_ch; i++) {
        if ((seen & (1U << i)) != 0U) {
            vals[i] = scratch[i];
        }
    }
    if (touched != NULL) {
        *touched = seen;
    }
    return 1;
}

void PortCmd_FormatPairs(const uint32_t *vals, uint32_t mask, uint32_t max_ch,
                         char *out, uint32_t out_len)
{
    uint32_t n = 0;
    int first = 1;

    if (out_len == 0U) {
        return;
    }
    out[0] = '\0';

    for (uint32_t i = 0; i < max_ch; i++) {
        if ((mask & (1U << i)) == 0U) { continue; }
        int w = snprintf(out + n, out_len - n, "%s%lu:%lu",
                         first ? "" : ",",
                         (unsigned long)(i + 1U), (unsigned long)vals[i]);
        if (w < 0 || (uint32_t)w >= out_len - n) { break; }
        n += (uint32_t)w;
        first = 0;
    }
}

void PortCmd_FormatMask(uint32_t mask, uint32_t max_ch, char *out, uint32_t out_len)
{
    uint32_t n = 0;
    int first = 1;

    if (out_len == 0U) {
        return;
    }
    out[0] = '\0';

    for (uint32_t i = 0; i < max_ch; i++) {
        if ((mask & (1U << i)) == 0U) { continue; }
        int w = snprintf(out + n, out_len - n, "%s%lu",
                         first ? "" : ",", (unsigned long)(i + 1U));
        if (w < 0 || (uint32_t)w >= out_len - n) { break; }
        n += (uint32_t)w;
        first = 0;
    }
}

const char *PortCmd_Word(const char *line, char *out, uint32_t out_len, const char **rest)
{
    uint32_t n = 0;

    while (is_space(*line)) { line++; }
    while (*line != '\0' && !is_space(*line) && n + 1U < out_len) {
        out[n++] = *line++;
    }
    out[n] = '\0';

    while (is_space(*line)) { line++; }
    if (rest != NULL) { *rest = line; }
    return out;
}

#endif /* PORTTOOL_ENABLE */
