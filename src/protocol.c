/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "protocol.h"

#include <stdlib.h>
#include <string.h>

static const char *k_code_strings[] = {
    [IPMAN_ERR_INVALID_REQUEST]     = "invalid_request",
    [IPMAN_ERR_UNKNOWN_OP]          = "unknown_op",
    [IPMAN_ERR_VALIDATION_FAILED]   = "validation_failed",
    [IPMAN_ERR_NOT_FOUND]           = "not_found",
    [IPMAN_ERR_CONFLICT]            = "conflict",
    [IPMAN_ERR_INTERNAL]            = "internal_error",
};

const char *ipman_error_code_str(ipman_error_code_t code) {
    size_t n = sizeof k_code_strings / sizeof k_code_strings[0];
    if ((size_t)code < n && k_code_strings[code] != NULL) {
        return k_code_strings[code];
    }
    return "internal_error";
}

int ipman_error_is_fatal(ipman_error_code_t code) {
    return code == IPMAN_ERR_INVALID_REQUEST || code == IPMAN_ERR_INTERNAL;
}

static char *dup_cstr(const char *s) {
    if (s == NULL) return NULL;
    size_t n = strlen(s);
    char *r = malloc(n + 1);
    if (r == NULL) return NULL;
    memcpy(r, s, n + 1);
    return r;
}

static int is_valid_utf8(const unsigned char *s, size_t len) {
    const unsigned char *end = s + len;
    while (s < end) {
        unsigned char c = *s++;
        if (c < 0x80) continue;
        if (c >= 0xC2 && c <= 0xDF) {
            if (s >= end || (*s & 0xC0) != 0x80) return 0;
            ++s;
        } else if (c == 0xE0) {
            if (s + 1 >= end || s[0] < 0xA0 || s[0] > 0xBF) return 0;
            if ((s[1] & 0xC0) != 0x80) return 0;
            s += 2;
        } else if (c >= 0xE1 && c <= 0xEC) {
            if (s + 1 >= end || (s[0] & 0xC0) != 0x80) return 0;
            if ((s[1] & 0xC0) != 0x80) return 0;
            s += 2;
        } else if (c == 0xED) {
            if (s + 1 >= end || s[0] < 0x80 || s[0] > 0x9F) return 0;
            if ((s[1] & 0xC0) != 0x80) return 0;
            s += 2;
        } else if (c >= 0xEE && c <= 0xEF) {
            if (s + 1 >= end || (s[0] & 0xC0) != 0x80) return 0;
            if ((s[1] & 0xC0) != 0x80) return 0;
            s += 2;
        } else if (c == 0xF0) {
            if (s + 2 >= end || s[0] < 0x90 || s[0] > 0xBF) return 0;
            if ((s[1] & 0xC0) != 0x80) return 0;
            if ((s[2] & 0xC0) != 0x80) return 0;
            s += 3;
        } else if (c >= 0xF1 && c <= 0xF3) {
            if (s + 2 >= end || (s[0] & 0xC0) != 0x80) return 0;
            if ((s[1] & 0xC0) != 0x80) return 0;
            if ((s[2] & 0xC0) != 0x80) return 0;
            s += 3;
        } else if (c == 0xF4) {
            if (s + 2 >= end || s[0] < 0x80 || s[0] > 0x8F) return 0;
            if ((s[1] & 0xC0) != 0x80) return 0;
            if ((s[2] & 0xC0) != 0x80) return 0;
            s += 3;
        } else {
            return 0;
        }
    }
    return 1;
}

int ipman_request_parse(const char *body,
                       ipman_request_t *req,
                       ipman_error_code_t *err_code_out,
                       const char **err_msg_out,
                       char **request_id_out) {
    *request_id_out = NULL;
    memset(req, 0, sizeof *req);

    if (body == NULL || body[0] == '\0') {
        *err_code_out = IPMAN_ERR_INVALID_REQUEST;
        *err_msg_out  = "empty request body";
        return -1;
    }
    if (!is_valid_utf8((const unsigned char *)body, strlen(body))) {
        *err_code_out = IPMAN_ERR_INVALID_REQUEST;
        *err_msg_out  = "request body must be valid UTF-8";
        return -1;
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        *err_code_out = IPMAN_ERR_INVALID_REQUEST;
        *err_msg_out  = "request body is not valid JSON";
        return -1;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        *err_code_out = IPMAN_ERR_INVALID_REQUEST;
        *err_msg_out  = "request body must be a JSON object";
        return -1;
    }

    /* Extract request_id up front so we can echo it on later failures. */
    cJSON *jid = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    if (cJSON_IsString(jid) && jid->valuestring != NULL && jid->valuestring[0] != '\0') {
        *request_id_out = dup_cstr(jid->valuestring);
    }

    cJSON *jver = cJSON_GetObjectItemCaseSensitive(root, "protocol_version");
    if (!cJSON_IsNumber(jver) ||
        jver->valuedouble != (double)jver->valueint) {
        cJSON_Delete(root);
        *err_code_out = IPMAN_ERR_INVALID_REQUEST;
        *err_msg_out  = "protocol_version must be an integer";
        return -1;
    }
    int version = jver->valueint;
    if (version != 1) {
        cJSON_Delete(root);
        *err_code_out = IPMAN_ERR_INVALID_REQUEST;
        *err_msg_out  = "unsupported protocol_version";
        return -1;
    }

    if (*request_id_out == NULL) {
        cJSON_Delete(root);
        *err_code_out = IPMAN_ERR_INVALID_REQUEST;
        *err_msg_out  = "request_id must be a non-empty string";
        return -1;
    }

    cJSON *jactor = cJSON_GetObjectItemCaseSensitive(root, "actor");
    if (!cJSON_IsString(jactor) ||
        jactor->valuestring == NULL ||
        jactor->valuestring[0] == '\0') {
        cJSON_Delete(root);
        *err_code_out = IPMAN_ERR_INVALID_REQUEST;
        *err_msg_out  = "actor must be a non-empty string";
        return -1;
    }

    cJSON *jop = cJSON_GetObjectItemCaseSensitive(root, "op");
    if (!cJSON_IsString(jop) || jop->valuestring == NULL || jop->valuestring[0] == '\0') {
        cJSON_Delete(root);
        *err_code_out = IPMAN_ERR_INVALID_REQUEST;
        *err_msg_out  = "op must be a non-empty string";
        return -1;
    }

    cJSON *jparams = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (!cJSON_IsObject(jparams)) {
        cJSON_Delete(root);
        *err_code_out = IPMAN_ERR_INVALID_REQUEST;
        *err_msg_out  = "params must be a JSON object";
        return -1;
    }

    req->protocol_version = version;
    req->request_id       = jid->valuestring;
    req->actor            = jactor->valuestring;
    req->op               = jop->valuestring;
    req->params           = jparams;
    req->root             = root;
    return 0;
}

static cJSON *base_response(const char *request_id, int ok) {
    cJSON *r = cJSON_CreateObject();
    if (r == NULL) return NULL;
    if (request_id != NULL) {
        cJSON_AddStringToObject(r, "request_id", request_id);
    } else {
        cJSON_AddNullToObject(r, "request_id");
    }
    cJSON_AddBoolToObject(r, "ok", ok);
    return r;
}

cJSON *ipman_response_ok(const char *request_id, cJSON *result) {
    cJSON *r = base_response(request_id, 1);
    if (r == NULL) {
        if (result) cJSON_Delete(result);
        return NULL;
    }
    if (result == NULL) result = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "result", result);
    return r;
}

static cJSON *s_pending_err_details = NULL;

void ipman_error_attach_details(cJSON *details) {
    if (s_pending_err_details != NULL) {
        cJSON_Delete(s_pending_err_details);
    }
    s_pending_err_details = details;
}

cJSON *ipman_error_take_details(void) {
    cJSON *taken = s_pending_err_details;
    s_pending_err_details = NULL;
    return taken;
}

cJSON *ipman_response_err(const char *request_id,
                         ipman_error_code_t code,
                         const char *message,
                         cJSON *details) {
    cJSON *r = base_response(request_id, 0);
    if (r == NULL) {
        if (details) cJSON_Delete(details);
        return NULL;
    }
    cJSON *err = cJSON_CreateObject();
    if (err == NULL ||
        cJSON_AddStringToObject(err, "code", ipman_error_code_str(code)) == NULL ||
        cJSON_AddStringToObject(err, "message", message ? message : "") == NULL) {
        if (err) cJSON_Delete(err);
        if (details) cJSON_Delete(details);
        cJSON_Delete(r);
        return NULL;
    }
    if (details != NULL) {
        cJSON_AddItemToObject(err, "details", details);
    }
    cJSON_AddItemToObject(r, "error", err);
    return r;
}
