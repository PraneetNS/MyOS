#include "json.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

void json_doc_init(json_doc_t* doc) {
    if (!doc) return;
    doc->node_count = 0;
    doc->str_pos = 0;
    doc->root = NULL;
}

static json_val_t* alloc_val(json_doc_t* doc) {
    if (doc->node_count >= JSON_MAX_NODES) return NULL;
    json_val_t* v = &doc->nodes[doc->node_count++];
    memset(v, 0, sizeof(*v));
    return v;
}

static const char* alloc_str(json_doc_t* doc, const char* s, size_t len) {
    if (doc->str_pos + len + 1 > JSON_STR_POOL) return NULL;
    char* dst = &doc->str_pool[doc->str_pos];
    memcpy(dst, s, len);
    dst[len] = '\0';
    doc->str_pos += len + 1;
    return dst;
}

static void skip_ws(const char** p, const char* end) {
    while (*p < end && (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n')) {
        (*p)++;
    }
}

static int parse_hex4(const char* p) {
    int val = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        val <<= 4;
        if (c >= '0' && c <= '9') val |= (c - '0');
        else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
        else return -1;
    }
    return val;
}

static json_err_t parse_string(json_doc_t* doc, const char** p, const char* end, const char** out_str, size_t* out_len) {
    if (*p >= end || **p != '"') return JSON_ERR_INVALID_SYNTAX;
    (*p)++; /* skip open quote */

    char temp[2048];
    size_t tlen = 0;

    while (*p < end) {
        char c = **p;
        if (c == '"') {
            (*p)++; /* skip close quote */
            const char* stored = alloc_str(doc, temp, tlen);
            if (!stored) return JSON_ERR_STRING_POOL_FULL;
            *out_str = stored;
            *out_len = tlen;
            return JSON_ERR_OK;
        }

        if ((unsigned char)c < 0x20) {
            /* unescaped control characters not allowed */
            return JSON_ERR_INVALID_SYNTAX;
        }

        if (c == '\\') {
            (*p)++;
            if (*p >= end) return JSON_ERR_UNEXPECTED_EOF;
            char esc = **p;
            char out_c = 0;
            switch (esc) {
                case '"': out_c = '"'; break;
                case '\\': out_c = '\\'; break;
                case '/': out_c = '/'; break;
                case 'b': out_c = '\b'; break;
                case 'f': out_c = '\f'; break;
                case 'n': out_c = '\n'; break;
                case 'r': out_c = '\r'; break;
                case 't': out_c = '\t'; break;
                case 'u': {
                    if (*p + 5 > end) return JSON_ERR_UNEXPECTED_EOF;
                    int uval = parse_hex4(*p + 1);
                    if (uval < 0) return JSON_ERR_INVALID_ESCAPE;
                    *p += 4;
                    if (uval < 0x80) {
                        out_c = (char)uval;
                    } else {
                        out_c = '?'; /* Basic ASCII fallback */
                    }
                    break;
                }
                default:
                    return JSON_ERR_INVALID_ESCAPE;
            }
            if (tlen + 1 >= sizeof(temp)) return JSON_ERR_STRING_POOL_FULL;
            temp[tlen++] = out_c;
            (*p)++;
        } else {
            if (tlen + 1 >= sizeof(temp)) return JSON_ERR_STRING_POOL_FULL;
            temp[tlen++] = c;
            (*p)++;
        }
    }

    return JSON_ERR_UNEXPECTED_EOF;
}

static json_err_t parse_value(json_doc_t* doc, const char** p, const char* end, int depth, json_val_t** out_val);

static json_err_t parse_number(json_doc_t* doc, const char** p, const char* end, json_val_t* v) {
    const char* start = *p;
    int is_int = 1;

    if (*p < end && **p == '-') (*p)++;
    if (*p >= end) return JSON_ERR_INVALID_NUMBER;

    if (**p == '0') {
        (*p)++;
    } else if (**p >= '1' && **p <= '9') {
        while (*p < end && isdigit((unsigned char)**p)) (*p)++;
    } else {
        return JSON_ERR_INVALID_NUMBER;
    }

    if (*p < end && **p == '.') {
        is_int = 0;
        (*p)++;
        if (*p >= end || !isdigit((unsigned char)**p)) return JSON_ERR_INVALID_NUMBER;
        while (*p < end && isdigit((unsigned char)**p)) (*p)++;
    }

    if (*p < end && (**p == 'e' || **p == 'E')) {
        is_int = 0;
        (*p)++;
        if (*p < end && (**p == '+' || **p == '-')) (*p)++;
        if (*p >= end || !isdigit((unsigned char)**p)) return JSON_ERR_INVALID_NUMBER;
        while (*p < end && isdigit((unsigned char)**p)) (*p)++;
    }

    size_t num_len = *p - start;
    if (num_len >= 64) return JSON_ERR_INVALID_NUMBER;
    char num_buf[64];
    memcpy(num_buf, start, num_len);
    num_buf[num_len] = '\0';

    v->type = JSON_TYPE_NUMBER;
    v->u.num.is_int = is_int;
    if (is_int) {
        v->u.num.int_val = strtoll(num_buf, NULL, 10);
        v->u.num.dbl_val = (double)v->u.num.int_val;
    } else {
        v->u.num.dbl_val = strtod(num_buf, NULL);
        v->u.num.int_val = (int64_t)v->u.num.dbl_val;
    }
    return JSON_ERR_OK;
}

static json_err_t parse_array(json_doc_t* doc, const char** p, const char* end, int depth, json_val_t* v) {
    if (**p != '[') return JSON_ERR_INVALID_SYNTAX;
    (*p)++; /* skip '[' */
    skip_ws(p, end);

    v->type = JSON_TYPE_ARRAY;
    v->u.arr.head = NULL;
    v->u.arr.count = 0;

    if (*p < end && **p == ']') {
        (*p)++;
        return JSON_ERR_OK;
    }

    json_val_t* tail = NULL;
    while (*p < end) {
        json_val_t* elem = NULL;
        json_err_t err = parse_value(doc, p, end, depth + 1, &elem);
        if (err != JSON_ERR_OK) return err;

        if (!v->u.arr.head) v->u.arr.head = elem;
        else tail->next = elem;
        tail = elem;
        v->u.arr.count++;

        skip_ws(p, end);
        if (*p >= end) return JSON_ERR_UNEXPECTED_EOF;

        if (**p == ']') {
            (*p)++;
            return JSON_ERR_OK;
        }
        if (**p == ',') {
            (*p)++;
            skip_ws(p, end);
            if (*p < end && **p == ']') {
                /* trailing comma disallowed in strict JSON */
                return JSON_ERR_INVALID_SYNTAX;
            }
        } else {
            return JSON_ERR_INVALID_SYNTAX;
        }
    }
    return JSON_ERR_UNEXPECTED_EOF;
}

static json_err_t parse_object(json_doc_t* doc, const char** p, const char* end, int depth, json_val_t* v) {
    if (**p != '{') return JSON_ERR_INVALID_SYNTAX;
    (*p)++; /* skip '{' */
    skip_ws(p, end);

    v->type = JSON_TYPE_OBJECT;
    v->u.obj.head = NULL;
    v->u.obj.count = 0;

    if (*p < end && **p == '}') {
        (*p)++;
        return JSON_ERR_OK;
    }

    json_val_t* tail = NULL;
    while (*p < end) {
        skip_ws(p, end);
        if (*p >= end) return JSON_ERR_UNEXPECTED_EOF;

        const char* key = NULL;
        size_t key_len = 0;
        json_err_t err = parse_string(doc, p, end, &key, &key_len);
        if (err != JSON_ERR_OK) return err;

        skip_ws(p, end);
        if (*p >= end || **p != ':') return JSON_ERR_INVALID_SYNTAX;
        (*p)++; /* skip ':' */
        skip_ws(p, end);

        json_val_t* member = NULL;
        err = parse_value(doc, p, end, depth + 1, &member);
        if (err != JSON_ERR_OK) return err;

        member->key = key;
        member->key_len = key_len;

        if (!v->u.obj.head) v->u.obj.head = member;
        else tail->next = member;
        tail = member;
        v->u.obj.count++;

        skip_ws(p, end);
        if (*p >= end) return JSON_ERR_UNEXPECTED_EOF;

        if (**p == '}') {
            (*p)++;
            return JSON_ERR_OK;
        }
        if (**p == ',') {
            (*p)++;
            skip_ws(p, end);
            if (*p < end && **p == '}') {
                /* trailing comma disallowed */
                return JSON_ERR_INVALID_SYNTAX;
            }
        } else {
            return JSON_ERR_INVALID_SYNTAX;
        }
    }
    return JSON_ERR_UNEXPECTED_EOF;
}

static json_err_t parse_value(json_doc_t* doc, const char** p, const char* end, int depth, json_val_t** out_val) {
    if (depth > JSON_MAX_DEPTH) return JSON_ERR_DEPTH_EXCEEDED;
    skip_ws(p, end);
    if (*p >= end) return JSON_ERR_UNEXPECTED_EOF;

    json_val_t* v = alloc_val(doc);
    if (!v) return JSON_ERR_NODE_LIMIT;

    char c = **p;
    if (c == '"') {
        const char* str = NULL;
        size_t slen = 0;
        json_err_t err = parse_string(doc, p, end, &str, &slen);
        if (err != JSON_ERR_OK) return err;
        v->type = JSON_TYPE_STRING;
        v->u.str.ptr = str;
        v->u.str.len = slen;
        *out_val = v;
        return JSON_ERR_OK;
    } else if (c == '{') {
        json_err_t err = parse_object(doc, p, end, depth, v);
        if (err != JSON_ERR_OK) return err;
        *out_val = v;
        return JSON_ERR_OK;
    } else if (c == '[') {
        json_err_t err = parse_array(doc, p, end, depth, v);
        if (err != JSON_ERR_OK) return err;
        *out_val = v;
        return JSON_ERR_OK;
    } else if (c == 't') {
        if (end - *p >= 4 && strncmp(*p, "true", 4) == 0) {
            *p += 4;
            v->type = JSON_TYPE_BOOL;
            v->u.boolean = true;
            *out_val = v;
            return JSON_ERR_OK;
        }
        return JSON_ERR_INVALID_SYNTAX;
    } else if (c == 'f') {
        if (end - *p >= 5 && strncmp(*p, "false", 5) == 0) {
            *p += 5;
            v->type = JSON_TYPE_BOOL;
            v->u.boolean = false;
            *out_val = v;
            return JSON_ERR_OK;
        }
        return JSON_ERR_INVALID_SYNTAX;
    } else if (c == 'n') {
        if (end - *p >= 4 && strncmp(*p, "null", 4) == 0) {
            *p += 4;
            v->type = JSON_TYPE_NULL;
            *out_val = v;
            return JSON_ERR_OK;
        }
        return JSON_ERR_INVALID_SYNTAX;
    } else if (c == '-' || isdigit((unsigned char)c)) {
        json_err_t err = parse_number(doc, p, end, v);
        if (err != JSON_ERR_OK) return err;
        *out_val = v;
        return JSON_ERR_OK;
    }

    return JSON_ERR_INVALID_SYNTAX;
}

json_err_t json_parse(json_doc_t* doc, const char* text, size_t len) {
    if (!doc || !text) return JSON_ERR_INVALID_SYNTAX;
    json_doc_init(doc);

    const char* p = text;
    const char* end = text + len;

    skip_ws(&p, end);
    if (p >= end) return JSON_ERR_UNEXPECTED_EOF;

    json_err_t err = parse_value(doc, &p, end, 1, &doc->root);
    if (err != JSON_ERR_OK) return err;

    skip_ws(&p, end);
    if (p < end) {
        /* Extra trailing junk */
        return JSON_ERR_INVALID_SYNTAX;
    }
    return JSON_ERR_OK;
}

const json_val_t* json_obj_get(const json_val_t* obj, const char* key) {
    if (!obj || obj->type != JSON_TYPE_OBJECT || !key) return NULL;
    json_val_t* curr = obj->u.obj.head;
    while (curr) {
        if (curr->key && strcmp(curr->key, key) == 0) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

const char* json_obj_get_str(const json_val_t* obj, const char* key) {
    const json_val_t* v = json_obj_get(obj, key);
    if (v && v->type == JSON_TYPE_STRING) return v->u.str.ptr;
    return NULL;
}

int json_obj_get_int(const json_val_t* obj, const char* key, int* out) {
    const json_val_t* v = json_obj_get(obj, key);
    if (v && v->type == JSON_TYPE_NUMBER) {
        if (out) *out = (int)v->u.num.int_val;
        return 0;
    }
    return -1;
}

int json_obj_get_bool(const json_val_t* obj, const char* key, bool* out) {
    const json_val_t* v = json_obj_get(obj, key);
    if (v && v->type == JSON_TYPE_BOOL) {
        if (out) *out = v->u.boolean;
        return 0;
    }
    return -1;
}

const json_val_t* json_obj_get_obj(const json_val_t* obj, const char* key) {
    const json_val_t* v = json_obj_get(obj, key);
    if (v && v->type == JSON_TYPE_OBJECT) return v;
    return NULL;
}

const json_val_t* json_obj_get_arr(const json_val_t* obj, const char* key) {
    const json_val_t* v = json_obj_get(obj, key);
    if (v && v->type == JSON_TYPE_ARRAY) return v;
    return NULL;
}

size_t json_arr_len(const json_val_t* arr) {
    if (!arr || arr->type != JSON_TYPE_ARRAY) return 0;
    return arr->u.arr.count;
}

const json_val_t* json_arr_get(const json_val_t* arr, size_t index) {
    if (!arr || arr->type != JSON_TYPE_ARRAY) return NULL;
    json_val_t* curr = arr->u.arr.head;
    size_t i = 0;
    while (curr) {
        if (i == index) return curr;
        curr = curr->next;
        i++;
    }
    return NULL;
}

/* Schema validation */
int json_validate_schema(const json_val_t* schema, const json_val_t* val, char* err_buf, size_t err_len) {
    if (!schema || schema->type != JSON_TYPE_OBJECT) {
        return 0; /* No schema or empty schema means anything is allowed */
    }
    if (!val) {
        if (err_buf && err_len > 0) snprintf(err_buf, err_len, "Missing value");
        return -1;
    }

    const char* expected_type = json_obj_get_str(schema, "type");
    if (expected_type) {
        if (strcmp(expected_type, "object") == 0 && val->type != JSON_TYPE_OBJECT) {
            if (err_buf && err_len > 0) snprintf(err_buf, err_len, "Expected object");
            return -1;
        }
        if (strcmp(expected_type, "string") == 0 && val->type != JSON_TYPE_STRING) {
            if (err_buf && err_len > 0) snprintf(err_buf, err_len, "Expected string");
            return -1;
        }
        if (strcmp(expected_type, "integer") == 0 && (val->type != JSON_TYPE_NUMBER || !val->u.num.is_int)) {
            if (err_buf && err_len > 0) snprintf(err_buf, err_len, "Expected integer");
            return -1;
        }
        if (strcmp(expected_type, "number") == 0 && val->type != JSON_TYPE_NUMBER) {
            if (err_buf && err_len > 0) snprintf(err_buf, err_len, "Expected number");
            return -1;
        }
        if (strcmp(expected_type, "boolean") == 0 && val->type != JSON_TYPE_BOOL) {
            if (err_buf && err_len > 0) snprintf(err_buf, err_len, "Expected boolean");
            return -1;
        }
        if (strcmp(expected_type, "array") == 0 && val->type != JSON_TYPE_ARRAY) {
            if (err_buf && err_len > 0) snprintf(err_buf, err_len, "Expected array");
            return -1;
        }
    }

    if (val->type == JSON_TYPE_OBJECT) {
        /* Check required properties */
        const json_val_t* req = json_obj_get_arr(schema, "required");
        if (req) {
            for (size_t i = 0; i < json_arr_len(req); i++) {
                const json_val_t* item = json_arr_get(req, i);
                if (item && item->type == JSON_TYPE_STRING) {
                    if (!json_obj_get(val, item->u.str.ptr)) {
                        if (err_buf && err_len > 0) {
                            snprintf(err_buf, err_len, "Missing required property '%s'", item->u.str.ptr);
                        }
                        return -1;
                    }
                }
            }
        }

        /* Check individual properties */
        const json_val_t* props = json_obj_get_obj(schema, "properties");
        if (props) {
            json_val_t* prop_schema = props->u.obj.head;
            while (prop_schema) {
                const json_val_t* prop_val = json_obj_get(val, prop_schema->key);
                if (prop_val) {
                    int rc = json_validate_schema(prop_schema, prop_val, err_buf, err_len);
                    if (rc != 0) return rc;
                }
                prop_schema = prop_schema->next;
            }
        }
    }

    return 0;
}

/* Serializer */
void json_buf_init(json_buf_t* jb, char* buf, size_t cap) {
    if (!jb) return;
    jb->buf = buf;
    jb->cap = cap;
    jb->len = 0;
    jb->error = 0;
    if (buf && cap > 0) buf[0] = '\0';
}

void json_buf_append_raw(json_buf_t* jb, const char* raw, size_t len) {
    if (!jb || jb->error) return;
    if (jb->len + len + 1 > jb->cap) {
        jb->error = 1;
        return;
    }
    memcpy(jb->buf + jb->len, raw, len);
    jb->len += len;
    jb->buf[jb->len] = '\0';
}

void json_buf_append_null(json_buf_t* jb) {
    json_buf_append_raw(jb, "null", 4);
}

void json_buf_append_bool(json_buf_t* jb, bool val) {
    if (val) json_buf_append_raw(jb, "true", 4);
    else json_buf_append_raw(jb, "false", 5);
}

void json_buf_append_int(json_buf_t* jb, int64_t val) {
    char s[32];
    int n = snprintf(s, sizeof(s), "%lld", (long long)val);
    if (n > 0) json_buf_append_raw(jb, s, (size_t)n);
}

void json_buf_append_str_n(json_buf_t* jb, const char* val, size_t n) {
    if (!jb || jb->error) return;
    json_buf_append_raw(jb, "\"", 1);
    if (val) {
        for (size_t i = 0; i < n; i++) {
            char c = val[i];
            switch (c) {
                case '"': json_buf_append_raw(jb, "\\\"", 2); break;
                case '\\': json_buf_append_raw(jb, "\\\\", 2); break;
                case '\b': json_buf_append_raw(jb, "\\b", 2); break;
                case '\f': json_buf_append_raw(jb, "\\f", 2); break;
                case '\n': json_buf_append_raw(jb, "\\n", 2); break;
                case '\r': json_buf_append_raw(jb, "\\r", 2); break;
                case '\t': json_buf_append_raw(jb, "\\t", 2); break;
                default:
                    if ((unsigned char)c < 0x20) {
                        char esc[8];
                        snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)c);
                        json_buf_append_raw(jb, esc, 6);
                    } else {
                        json_buf_append_raw(jb, &c, 1);
                    }
                    break;
            }
        }
    }
    json_buf_append_raw(jb, "\"", 1);
}

void json_buf_append_str(json_buf_t* jb, const char* val) {
    json_buf_append_str_n(jb, val, val ? strlen(val) : 0);
}

void json_buf_obj_start(json_buf_t* jb) {
    json_buf_append_raw(jb, "{", 1);
}

void json_buf_obj_key(json_buf_t* jb, const char* key) {
    if (!jb || jb->error) return;
    if (jb->len > 0 && jb->buf[jb->len - 1] != '{') {
        json_buf_append_raw(jb, ",", 1);
    }
    json_buf_append_str(jb, key);
    json_buf_append_raw(jb, ":", 1);
}

void json_buf_obj_end(json_buf_t* jb) {
    json_buf_append_raw(jb, "}", 1);
}

void json_buf_arr_start(json_buf_t* jb) {
    json_buf_append_raw(jb, "[", 1);
}

void json_buf_arr_end(json_buf_t* jb) {
    json_buf_append_raw(jb, "]", 1);
}

const char* json_strerror(json_err_t err) {
    switch (err) {
        case JSON_ERR_OK: return "OK";
        case JSON_ERR_INVALID_SYNTAX: return "Invalid syntax";
        case JSON_ERR_UNEXPECTED_EOF: return "Unexpected EOF";
        case JSON_ERR_DEPTH_EXCEEDED: return "Depth exceeded limit";
        case JSON_ERR_NODE_LIMIT: return "Node limit exceeded";
        case JSON_ERR_STRING_POOL_FULL: return "String pool full";
        case JSON_ERR_INVALID_NUMBER: return "Invalid number";
        case JSON_ERR_INVALID_ESCAPE: return "Invalid escape sequence";
        case JSON_ERR_SCHEMA_MISMATCH: return "Schema mismatch";
        default: return "Unknown JSON error";
    }
}
