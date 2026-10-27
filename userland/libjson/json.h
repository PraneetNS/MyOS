#ifndef _LIBJSON_JSON_H
#define _LIBJSON_JSON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define JSON_MAX_NODES  512
#define JSON_MAX_DEPTH  16
#define JSON_STR_POOL   8192

typedef enum {
    JSON_ERR_OK = 0,
    JSON_ERR_INVALID_SYNTAX,
    JSON_ERR_UNEXPECTED_EOF,
    JSON_ERR_DEPTH_EXCEEDED,
    JSON_ERR_NODE_LIMIT,
    JSON_ERR_STRING_POOL_FULL,
    JSON_ERR_INVALID_NUMBER,
    JSON_ERR_INVALID_ESCAPE,
    JSON_ERR_SCHEMA_MISMATCH
} json_err_t;

typedef enum {
    JSON_TYPE_NULL = 0,
    JSON_TYPE_BOOL,
    JSON_TYPE_NUMBER,
    JSON_TYPE_STRING,
    JSON_TYPE_ARRAY,
    JSON_TYPE_OBJECT
} json_type_t;

typedef struct json_val json_val_t;

struct json_val {
    json_type_t type;
    const char* key; /* only valid if parent is OBJECT */
    size_t key_len;

    union {
        bool boolean;
        struct {
            int64_t int_val;
            double dbl_val;
            bool is_int;
        } num;
        struct {
            const char* ptr;
            size_t len;
        } str;
        struct {
            json_val_t* head;
            size_t count;
        } arr;
        struct {
            json_val_t* head;
            size_t count;
        } obj;
    } u;

    json_val_t* next; /* sibling in array or object */
};

typedef struct {
    json_val_t nodes[JSON_MAX_NODES];
    size_t node_count;
    char str_pool[JSON_STR_POOL];
    size_t str_pos;
    json_val_t* root;
} json_doc_t;

/* Parsing */
void json_doc_init(json_doc_t* doc);
json_err_t json_parse(json_doc_t* doc, const char* text, size_t len);

/* Accessors */
const json_val_t* json_obj_get(const json_val_t* obj, const char* key);
const char* json_obj_get_str(const json_val_t* obj, const char* key);
int json_obj_get_int(const json_val_t* obj, const char* key, int* out);
int json_obj_get_bool(const json_val_t* obj, const char* key, bool* out);
const json_val_t* json_obj_get_obj(const json_val_t* obj, const char* key);
const json_val_t* json_obj_get_arr(const json_val_t* obj, const char* key);

size_t json_arr_len(const json_val_t* arr);
const json_val_t* json_arr_get(const json_val_t* arr, size_t index);

/* Schema Validation */
int json_validate_schema(const json_val_t* schema, const json_val_t* val, char* err_buf, size_t err_len);

/* Stringifying / Serializing */
typedef struct {
    char* buf;
    size_t cap;
    size_t len;
    int error;
} json_buf_t;

void json_buf_init(json_buf_t* jb, char* buf, size_t cap);
void json_buf_append_null(json_buf_t* jb);
void json_buf_append_bool(json_buf_t* jb, bool val);
void json_buf_append_int(json_buf_t* jb, int64_t val);
void json_buf_append_str(json_buf_t* jb, const char* val);
void json_buf_append_str_n(json_buf_t* jb, const char* val, size_t n);
void json_buf_append_raw(json_buf_t* jb, const char* raw, size_t len);

void json_buf_obj_start(json_buf_t* jb);
void json_buf_obj_key(json_buf_t* jb, const char* key);
void json_buf_obj_end(json_buf_t* jb);

void json_buf_arr_start(json_buf_t* jb);
void json_buf_arr_end(json_buf_t* jb);

const char* json_strerror(json_err_t err);

#endif /* _LIBJSON_JSON_H */
