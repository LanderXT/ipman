#include "io.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int ipman_read_all(FILE *fp, size_t max_bytes,
                  char **out_buf, size_t *out_len) {
    /* hard_cap is one past the last accepted payload byte; 0 means unbounded.
     * The buffer always reserves one slot for the trailing NUL, so the
     * allocation ceiling is hard_cap + 1 when capped. */
    size_t hard_cap = max_bytes;
    size_t cap = 4096;
    if (hard_cap != 0 && cap > hard_cap + 1) cap = hard_cap + 1;
    size_t len = 0;
    char *buf = malloc(cap);
    if (buf == NULL) return -1;

    for (;;) {
        if (len == cap - 1) {
            size_t new_cap = cap * 2;
            if (hard_cap != 0 && new_cap > hard_cap + 1) new_cap = hard_cap + 1;
            if (cap == new_cap) {
                /* Buffer can't grow but we still want to know whether the next
                 * read would have produced more bytes — so peek one byte before
                 * deciding it's overflow. */
                int extra = fgetc(fp);
                if (extra == EOF) {
                    if (ferror(fp)) { free(buf); return -1; }
                    break; /* exact-fit input, treat as success */
                }
                free(buf);
                if (out_buf != NULL) *out_buf = NULL;
                if (out_len != NULL) *out_len = len + 1;
                return -2;
            }
            char *nb = realloc(buf, new_cap);
            if (nb == NULL) { free(buf); return -1; }
            buf = nb;
            cap = new_cap;
        }
        size_t room = cap - 1 - len;
        size_t got  = fread(buf + len, 1, room, fp);
        len += got;
        if (got < room) {
            if (ferror(fp)) { free(buf); return -1; }
            break; /* feof */
        }
    }

    buf[len] = '\0';
    *out_buf = buf;
    *out_len = len;
    return 0;
}

int ipman_base64_encode(const unsigned char *in, size_t in_len, char **out) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    if (out == NULL) return -1;
    *out = NULL;

    if (in_len > (SIZE_MAX / 4) * 3) return -1;
    size_t out_len = ((in_len + 2) / 3) * 4;
    char *buf = malloc(out_len + 1);
    if (buf == NULL) return -1;

    size_t i = 0;
    size_t j = 0;
    while (i < in_len) {
        size_t rem = in_len - i;
        unsigned int b0 = in[i++];
        unsigned int b1 = rem > 1 ? in[i++] : 0;
        unsigned int b2 = rem > 2 ? in[i++] : 0;

        buf[j++] = table[b0 >> 2];
        buf[j++] = table[((b0 & 0x03) << 4) | (b1 >> 4)];
        buf[j++] = rem > 1 ? table[((b1 & 0x0f) << 2) | (b2 >> 6)] : '=';
        buf[j++] = rem > 2 ? table[b2 & 0x3f] : '=';
    }

    buf[j] = '\0';
    *out = buf;
    return 0;
}

static int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int is_base64_space(unsigned char c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t' ||
           c == '\f' || c == '\v';
}

int ipman_base64_decode(const char *in, size_t in_len, char **out, size_t *out_len) {
    if (out == NULL || out_len == NULL) return -1;
    *out = NULL;
    *out_len = 0;

    size_t clean_len = 0;
    for (size_t i = 0; i < in_len; ++i) {
        if (!is_base64_space((unsigned char)in[i])) ++clean_len;
    }
    if (clean_len == 0 || clean_len % 4 != 0) return -1;

    size_t max_len = (clean_len / 4) * 3;
    char *buf = malloc(max_len + 1);
    if (buf == NULL) return -1;

    int quad[4];
    size_t q = 0;
    size_t j = 0;
    int seen_pad = 0;

    for (size_t i = 0; i < in_len; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (is_base64_space(c)) continue;

        int v;
        if (c == '=') {
            v = -2;
            seen_pad = 1;
        } else {
            if (seen_pad) {
                free(buf);
                return -1;
            }
            v = base64_value(c);
            if (v < 0) {
                free(buf);
                return -1;
            }
        }

        quad[q++] = v;
        if (q != 4) continue;

        if (quad[0] < 0 || quad[1] < 0) {
            free(buf);
            return -1;
        }
        if (quad[2] == -2 && quad[3] != -2) {
            free(buf);
            return -1;
        }
        if (quad[2] == -2 && (quad[1] & 0x0f) != 0) {
            free(buf);
            return -1;
        }
        if (quad[3] == -2 && quad[2] >= 0 && (quad[2] & 0x03) != 0) {
            free(buf);
            return -1;
        }

        buf[j++] = (char)((quad[0] << 2) | (quad[1] >> 4));
        if (quad[2] != -2) {
            buf[j++] = (char)(((quad[1] & 0x0f) << 4) | (quad[2] >> 2));
            if (quad[3] != -2) {
                buf[j++] = (char)(((quad[2] & 0x03) << 6) | quad[3]);
            }
        }
        q = 0;
    }

    if (q != 0) {
        free(buf);
        return -1;
    }

    buf[j] = '\0';
    *out = buf;
    *out_len = j;
    return 0;
}
