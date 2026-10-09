-- alloc.lua: allocate 20MB of tables and strings, then garbage collect
local t = {}
for i = 1, 20 do
    t[i] = string.rep("A", 1000000)
end
print("LUA_20MB_ALLOC_OK")
t = nil
collectgarbage("collect")
