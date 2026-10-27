#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <intent.h>
#include <json.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static int s_lua_intent_fd = -1;
static lua_State* s_main_lua_state = NULL;

typedef struct {
    char name[64];
    int lua_func_ref;
    bool active;
} lua_skill_binding_t;

#define MAX_LUA_SKILLS 32
static lua_skill_binding_t s_lua_skills[MAX_LUA_SKILLS];
static int s_num_lua_skills = 0;

static int ensure_connected(void) {
    if (s_lua_intent_fd < 0) {
        s_lua_intent_fd = intent_connect();
    }
    return s_lua_intent_fd;
}

/* Helper to convert JSON value to Lua value on stack */
static void push_json_to_lua(lua_State* L, const json_val_t* v) {
    if (!v || v->type == JSON_TYPE_NULL) {
        lua_pushnil(L);
        return;
    }
    switch (v->type) {
        case JSON_TYPE_BOOL:
            lua_pushboolean(L, v->u.boolean);
            break;
        case JSON_TYPE_NUMBER:
            if (v->u.num.is_int) lua_pushinteger(L, (lua_Integer)v->u.num.int_val);
            else lua_pushnumber(L, (lua_Number)v->u.num.dbl_val);
            break;
        case JSON_TYPE_STRING:
            lua_pushlstring(L, v->u.str.ptr, v->u.str.len);
            break;
        case JSON_TYPE_ARRAY: {
            lua_newtable(L);
            size_t len = json_arr_len(v);
            for (size_t i = 0; i < len; i++) {
                push_json_to_lua(L, json_arr_get(v, i));
                lua_rawseti(L, -2, (lua_Integer)(i + 1));
            }
            break;
        }
        case JSON_TYPE_OBJECT: {
            lua_newtable(L);
            json_val_t* curr = v->u.obj.head;
            while (curr) {
                if (curr->key) {
                    lua_pushstring(L, curr->key);
                    push_json_to_lua(L, curr);
                    lua_settable(L, -3);
                }
                curr = curr->next;
            }
            break;
        }
        default:
            lua_pushnil(L);
            break;
    }
}

/* Helper to convert Lua table/value to JSON */
static void serialize_lua_to_json(lua_State* L, int idx, json_buf_t* jb, int depth) {
    if (depth > 16 || jb->error) return;
    int t = lua_type(L, idx);
    switch (t) {
        case LUA_TNIL:
            json_buf_append_null(jb);
            break;
        case LUA_TBOOLEAN:
            json_buf_append_bool(jb, lua_toboolean(L, idx));
            break;
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) {
                json_buf_append_int(jb, (int64_t)lua_tointeger(L, idx));
            } else {
                char s[32];
                snprintf(s, sizeof(s), "%g", (double)lua_tonumber(L, idx));
                json_buf_append_raw(jb, s, strlen(s));
            }
            break;
        case LUA_TSTRING: {
            size_t len = 0;
            const char* str = lua_tolstring(L, idx, &len);
            json_buf_append_str_n(jb, str, len);
            break;
        }
        case LUA_TTABLE: {
            /* Check if array or object */
            lua_Integer len = (lua_Integer)lua_rawlen(L, idx);
            if (len > 0) {
                json_buf_arr_start(jb);
                for (lua_Integer i = 1; i <= len; i++) {
                    if (i > 1) json_buf_append_raw(jb, ",", 1);
                    lua_rawgeti(L, idx, i);
                    serialize_lua_to_json(L, -1, jb, depth + 1);
                    lua_pop(L, 1);
                }
                json_buf_arr_end(jb);
            } else {
                json_buf_obj_start(jb);
                lua_pushnil(L);
                int abs_idx = (idx > 0) ? idx : (lua_gettop(L) + idx);
                while (lua_next(L, abs_idx) != 0) {
                    if (lua_type(L, -2) == LUA_TSTRING) {
                        const char* k = lua_tostring(L, -2);
                        json_buf_obj_key(jb, k);
                        serialize_lua_to_json(L, -1, jb, depth + 1);
                    }
                    lua_pop(L, 1);
                }
                json_buf_obj_end(jb);
            }
            break;
        }
        default:
            json_buf_append_null(jb);
            break;
    }
}

static int lua_skill_c_handler(const char* skill, const char* args_json,
                               char* result_json, size_t result_cap,
                               char* err_buf, size_t err_cap,
                               void* udata) {
    (void)skill;
    int binding_idx = (int)(intptr_t)udata;
    if (binding_idx < 0 || binding_idx >= MAX_LUA_SKILLS || !s_lua_skills[binding_idx].active) {
        snprintf(err_buf, err_cap, "Invalid Lua skill binding");
        return -1;
    }

    lua_State* L = s_main_lua_state;
    if (!L) {
        snprintf(err_buf, err_cap, "Lua runtime not initialized");
        return -1;
    }

    int ref = s_lua_skills[binding_idx].lua_func_ref;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        snprintf(err_buf, err_cap, "Target Lua reference is not a function");
        return -1;
    }

    /* Parse args JSON and pass to Lua */
    json_doc_t doc;
    if (json_parse(&doc, args_json, strlen(args_json)) == JSON_ERR_OK && doc.root) {
        push_json_to_lua(L, doc.root);
    } else {
        lua_newtable(L);
    }

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        snprintf(err_buf, err_cap, "%s", err ? err : "Lua handler error");
        lua_pop(L, 1);
        return -1;
    }

    /* Convert return value to JSON */
    if (lua_isstring(L, -1)) {
        const char* res = lua_tostring(L, -1);
        if (res[0] == '{' || res[0] == '[' || res[0] == '"') {
            snprintf(result_json, result_cap, "%s", res);
        } else {
            json_buf_t jb;
            json_buf_init(&jb, result_json, result_cap);
            json_buf_append_str(&jb, res);
        }
    } else if (lua_istable(L, -1)) {
        json_buf_t jb;
        json_buf_init(&jb, result_json, result_cap);
        serialize_lua_to_json(L, -1, &jb, 0);
        if (jb.error) {
            snprintf(err_buf, err_cap, "Failed to serialize Lua table to JSON");
            lua_pop(L, 1);
            return -1;
        }
    } else if (lua_isnil(L, -1)) {
        snprintf(result_json, result_cap, "{}");
    } else {
        json_buf_t jb;
        json_buf_init(&jb, result_json, result_cap);
        serialize_lua_to_json(L, -1, &jb, 0);
    }

    lua_pop(L, 1);
    return 0;
}

static int l_intent_connect(lua_State* L) {
    int fd = ensure_connected();
    if (fd < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "Failed to connect to intent bus");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_intent_register(lua_State* L) {
    /* args: name, schema_or_nil, func, sensitive_bool_or_nil */
    const char* name = luaL_checkstring(L, 1);
    if (!lua_isfunction(L, 3)) {
        return luaL_error(L, "Third argument must be handler function");
    }
    bool sensitive = false;
    if (lua_isboolean(L, 4)) {
        sensitive = lua_toboolean(L, 4);
    }

    char schema_json[2048];
    schema_json[0] = '\0';
    if (lua_istable(L, 2)) {
        json_buf_t jb;
        json_buf_init(&jb, schema_json, sizeof(schema_json));
        serialize_lua_to_json(L, 2, &jb, 0);
    } else if (lua_isstring(L, 2)) {
        strncpy(schema_json, lua_tostring(L, 2), sizeof(schema_json) - 1);
        schema_json[sizeof(schema_json) - 1] = '\0';
    }

    int fd = ensure_connected();
    if (fd < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "Cannot connect to intent bus");
        return 2;
    }

    int slot = -1;
    for (int i = 0; i < MAX_LUA_SKILLS; i++) {
        if (!s_lua_skills[i].active || strcmp(s_lua_skills[i].name, name) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "Max Lua skills limit reached");
        return 2;
    }

    /* Save Lua function in registry */
    lua_pushvalue(L, 3);
    int func_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    strncpy(s_lua_skills[slot].name, name, sizeof(s_lua_skills[slot].name) - 1);
    s_lua_skills[slot].lua_func_ref = func_ref;
    s_lua_skills[slot].active = true;
    if (slot >= s_num_lua_skills) s_num_lua_skills = slot + 1;

    int rc = intent_register_skill(fd, name, name, schema_json, NULL, sensitive,
                                  lua_skill_c_handler, (void*)(intptr_t)slot);
    if (rc != 0) {
        s_lua_skills[slot].active = false;
        luaL_unref(L, LUA_REGISTRYINDEX, func_ref);
        lua_pushnil(L);
        lua_pushstring(L, "Skill registration failed");
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int l_intent_serve(lua_State* L) {
    (void)L;
    int fd = ensure_connected();
    if (fd < 0) return 0;
    intent_serve(fd);
    return 0;
}

static int l_intent_call(lua_State* L) {
    /* args: skill, args_table_or_str, timeout_ms */
    const char* skill = luaL_checkstring(L, 1);
    char args_json[4096];
    args_json[0] = '\0';

    if (lua_istable(L, 2)) {
        json_buf_t jb;
        json_buf_init(&jb, args_json, sizeof(args_json));
        serialize_lua_to_json(L, 2, &jb, 0);
    } else if (lua_isstring(L, 2)) {
        strncpy(args_json, lua_tostring(L, 2), sizeof(args_json) - 1);
        args_json[sizeof(args_json) - 1] = '\0';
    } else {
        strcpy(args_json, "{}");
    }

    int timeout = (int)luaL_optinteger(L, 3, 5000);

    int fd = ensure_connected();
    if (fd < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "Not connected to intent bus");
        return 2;
    }

    char result_buf[8192];
    int rc = intent_call(fd, skill, args_json, result_buf, sizeof(result_buf), timeout);
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, result_buf[0] ? result_buf : "Call failed");
        return 2;
    }

    /* Parse result into Lua value */
    json_doc_t doc;
    if (json_parse(&doc, result_buf, strlen(result_buf)) == JSON_ERR_OK && doc.root) {
        push_json_to_lua(L, doc.root);
    } else {
        lua_pushstring(L, result_buf);
    }
    return 1;
}

static int l_intent_list(lua_State* L) {
    int fd = ensure_connected();
    if (fd < 0) {
        lua_pushnil(L);
        return 1;
    }
    char buf[8192];
    if (intent_list_skills(fd, buf, sizeof(buf)) != 0) {
        lua_pushnil(L);
        return 1;
    }
    json_doc_t doc;
    if (json_parse(&doc, buf, strlen(buf)) == JSON_ERR_OK && doc.root) {
        push_json_to_lua(L, doc.root);
    } else {
        lua_pushstring(L, buf);
    }
    return 1;
}

static const luaL_Reg intentlib[] = {
    {"connect", l_intent_connect},
    {"register", l_intent_register},
    {"serve", l_intent_serve},
    {"call", l_intent_call},
    {"list", l_intent_list},
    {NULL, NULL}
};

LUAMOD_API int luaopen_intent(lua_State* L) {
    s_main_lua_state = L;
    luaL_newlib(L, intentlib);
    return 1;
}
