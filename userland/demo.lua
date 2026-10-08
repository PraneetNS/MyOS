-- /home/demo.lua: Demo of Lua 5.4 running on MyOS with Newlib and FPU/SSE support

print("=== MyOS Lua 5.4 Demo ===")

-- 1. Fibonacci computation
function fib(n)
    if n <= 1 then return n end
    local a, b = 0, 1
    for i = 2, n do
        a, b = b, a + b
    end
    return b
end

print(string.format("Fibonacci(10) = %d", fib(10)))
print(string.format("Fibonacci(20) = %d", fib(20)))

-- 2. Floating-point math (libm / FPU / SSE)
local pi = math.pi
local s = math.sin(pi / 2.0)
local sq = math.sqrt(144.0)
local p = (2.0 ^ 10.0)
local e = math.exp(1.0)
local l = math.log(e)

print(string.format("math: sin(pi/2)=%.4f, sqrt(144)=%.4f, 2^10=%.1f, log(e)=%.4f", s, sq, p, l))

-- 3. String operations
local str = "Hello, MyOS World! Lua 5.4 is running."
print("String upper: " .. string.upper(str))
print("String sub:   " .. string.sub(str, 1, 18))
local words = {}
for w in string.gmatch(str, "%a+") do
    table.insert(words, w)
end
print(string.format("Extracted %d words from string.", #words))

-- 4. Table sort benchmark
local t = {}
for i = 1, 100 do
    table.insert(t, (i * 37) % 100)
end
table.sort(t)
print(string.format("Sorted table of %d elements: min=%d, max=%d", #t, t[1], t[#t]))

-- 5. File I/O
local test_file = "/tmp/lua_demo.txt"
local f = io.open(test_file, "w")
if f then
    f:write("Lua 5.4 file I/O test on MyOS\n")
    f:write("Second line of test data.\n")
    f:close()
    
    local rf = io.open(test_file, "r")
    if rf then
        local line1 = rf:read("*l")
        local line2 = rf:read("*l")
        rf:close()
        print("File I/O read back line 1: " .. (line1 or "<nil>"))
        print("File I/O read back line 2: " .. (line2 or "<nil>"))
    else
        print("Failed to open file for reading.")
    end
else
    print("Failed to open file for writing.")
end

-- 6. Date & Time
local now = os.time()
local d = os.date("%Y-%m-%d %H:%M:%S", now)
print("Current time (os.time): " .. tostring(now))
print("Current date (os.date): " .. tostring(d))

print("=== Demo Complete ===")
