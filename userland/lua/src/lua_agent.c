#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <agent.h>
#include <unistd.h>

static int l_agent_tokens_left(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    int left = agent_tokens_left(id);
    lua_pushinteger(L, left);
    return 1;
}

static int l_agent_charge_tokens(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    uint32_t count = (uint32_t)luaL_checkinteger(L, 2);
    int rc = agent_charge_tokens(id, count);
    if (rc == 0) {
        lua_pushboolean(L, 1);
        return 1;
    } else {
        lua_pushboolean(L, 0);
        lua_pushinteger(L, rc);
        return 2;
    }
}

static int l_agent_attach(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    int rc = agent_attach(id);
    if (rc == 0) {
        lua_pushboolean(L, 1);
        return 1;
    } else {
        lua_pushboolean(L, 0);
        lua_pushinteger(L, rc);
        return 2;
    }
}

static const luaL_Reg agentlib[] = {
    {"tokens_left", l_agent_tokens_left},
    {"charge_tokens", l_agent_charge_tokens},
    {"attach", l_agent_attach},
    {NULL, NULL}
};

LUAMOD_API int luaopen_agent(lua_State* L) {
    luaL_newlib(L, agentlib);
    return 1;
}
