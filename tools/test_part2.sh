echo "[TEST] Session 19: The Agent Desktop, accessibility tree, window capabilities, trusted approval UI, and killswitch..."

# Part A: Low-memory -m 64 text-mode boot: desktest.elf and agentctl window caps
(
    sleep 3
    printf "shinfo\n"
    sleep 0.5
    printf "agentctl create winagent --window-observe uidemo --window-act uidemo\n"
    sleep 0.5
    printf "cat /proc/agents/1/caps\n"
    sleep 0.5
    printf "desktest.elf\n"
    sleep 2
    printf "poweroff\n"
) | timeout 35s qemu-system-x86_64 -vga std -m 64 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE19" 2>&1 || true

tr -d '\r' < "$LOGFILE19" > "${LOGFILE19}.tmp" && mv "${LOGFILE19}.tmp" "$LOGFILE19"
cat "$LOGFILE19"

echo "[TEST] Asserting Session 19 low-memory results..."
grep -q "ALL AGENT DESKTOP TESTS PASSED!" "$LOGFILE19" || { echo "FAIL: desktest suite failed"; rm -f "$LOGFILE19"; exit 1; }
grep -E -q "PASSED: Killswitch state transitions and (audit logging|timer-tick preemption) verified" "$LOGFILE19" || { echo "FAIL: Killswitch test failed"; rm -f "$LOGFILE19"; exit 1; }
grep -q "PASSED: Window capabilities and delegation narrowing verified" "$LOGFILE19" || { echo "FAIL: Window caps test failed"; rm -f "$LOGFILE19"; exit 1; }
grep -q "PASSED: Accessibility tree and action protocol round-trip verified" "$LOGFILE19" || { echo "FAIL: A11y protocol test failed"; rm -f "$LOGFILE19"; exit 1; }
grep -q "PASSED: Zero physical frame leaks after agent desktop operations" "$LOGFILE19" || { echo "FAIL: Frame leaks test failed"; rm -f "$LOGFILE19"; exit 1; }
grep -q "WindowObserve:	uidemo" "$LOGFILE19" || { echo "FAIL: WindowObserve cap missing in /proc/agents/1/caps"; rm -f "$LOGFILE19"; exit 1; }
grep -q "WindowAct:	uidemo" "$LOGFILE19" || { echo "FAIL: WindowAct cap missing in /proc/agents/1/caps"; rm -f "$LOGFILE19"; exit 1; }

# Part B: Preset configuration files on disk image
echo "[TEST] Verifying preset configuration files on disk image..."
mdir -i disk.img ::/etc/agents.d | grep -qi "organizer.conf" || { echo "FAIL: organizer.conf missing in /etc/agents.d"; rm -f "$LOGFILE19"; exit 1; }
mdir -i disk.img ::/etc/agents.d | grep -qi "explainer.conf" || { echo "FAIL: explainer.conf missing in /etc/agents.d"; rm -f "$LOGFILE19"; exit 1; }
mdir -i disk.img ::/etc/agents.d | grep -qi "terminal.*conf" || { echo "FAIL: terminal-helper.conf missing in /etc/agents.d"; rm -f "$LOGFILE19"; exit 1; }

# Part C: Full GUI compositor & visual killswitch verification via test_step6.py
echo "[TEST] Running Phase 4 graphical integration test (test_step6.py)..."
python3 tools/test_step6.py || { echo "FAIL: test_step6.py failed"; rm -f "$LOGFILE19"; exit 1; }

echo "[PASS] Session 19: The Agent Desktop, accessibility tree, window capabilities, trusted approval UI, and killswitch verified!"
rm -f "$LOGFILE19"

echo "[TEST] Session 20: Persistent agent memory (principals, memd, memctl) and semantic file index (indexd)..."
LOGFILE20=$(mktemp)
LOGFILE20_LOW=$(mktemp)

# Part A: -m 64 boot still works; memd and indexd register
(
    sleep 3
    printf "shinfo\n"
    sleep 0.5
    printf "cat /proc/meminfo\n"
    sleep 0.5
    printf "agentctl create pagent --principal alice --mem-read priv --mem-write priv --bus-call mem.\n"
    sleep 0.5
    printf "agentctl status 1\n"
    sleep 0.5
    printf "cat /proc/agents/1/caps\n"
    sleep 0.5
    printf "exit\n"
) | timeout 30s qemu-system-x86_64 -vga std -m 64 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE20_LOW" 2>&1 || true
tr -d '\r' < "$LOGFILE20_LOW" > "${LOGFILE20_LOW}.tmp" && mv "${LOGFILE20_LOW}.tmp" "$LOGFILE20_LOW"
grep -q "reg mem.put" "$LOGFILE20_LOW" || { echo "FAIL: memd did not register in -m 64 boot"; rm -f "$LOGFILE20" "$LOGFILE20_LOW"; exit 1; }
grep -q "reg fs.search" "$LOGFILE20_LOW" || { echo "FAIL: indexd did not register in -m 64 boot"; rm -f "$LOGFILE20" "$LOGFILE20_LOW"; exit 1; }
grep -q "MemTotal:       65020 kB" "$LOGFILE20_LOW" || { echo "FAIL: -m 64 MemTotal unexpected"; rm -f "$LOGFILE20" "$LOGFILE20_LOW"; exit 1; }
grep -q "Principal:.*alice" "$LOGFILE20_LOW" || { echo "FAIL: agentctl --principal / status principal missing"; rm -f "$LOGFILE20" "$LOGFILE20_LOW"; exit 1; }
grep -q "MemRead:	priv" "$LOGFILE20_LOW" || { echo "FAIL: MemRead cap missing from /proc/agents/1/caps"; rm -f "$LOGFILE20" "$LOGFILE20_LOW"; exit 1; }
grep -q "MemWrite:	priv" "$LOGFILE20_LOW" || { echo "FAIL: MemWrite cap missing from /proc/agents/1/caps"; rm -f "$LOGFILE20" "$LOGFILE20_LOW"; exit 1; }
echo "[PASS] -m 64 boot with memd/indexd verified!"
rm -f "$LOGFILE20_LOW"

# Part B: full memory + index test suite
(
    sleep 3
    printf "memdtest\n"
    sleep 60
    printf "memctl list alice\n"
    sleep 2
    printf "sync\n"
    sleep 2
    printf "exit\n"
) | timeout 120s qemu-system-x86_64 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE20" 2>&1 || true
tr -d '\r' < "$LOGFILE20" > "${LOGFILE20}.tmp" && mv "${LOGFILE20}.tmp" "$LOGFILE20"
cat "$LOGFILE20"

echo "[TEST] Asserting Session 20 results..."
grep -q "ALL MEMORY TESTS PASSED!" "$LOGFILE20" || { echo "FAIL: memdtest suite failed"; rm -f "$LOGFILE20"; exit 1; }
grep -q "FAILED:" "$LOGFILE20" && { echo "FAIL: memdtest reported a failure"; rm -f "$LOGFILE20"; exit 1; }
for t in "Stable principals" "Kernel-vouched principal" "memd put/search" "CAP_MEM_READ/WRITE namespace" "Provenance:" "Crash safety" "mem.forget physically" "indexd: fs.search filtered" "Audit chain records"; do
    grep -q "PASSED: $t" "$LOGFILE20" || { echo "FAIL: missing '$t'"; rm -f "$LOGFILE20"; exit 1; }
done
grep -q "memctl: error" "$LOGFILE20" && echo "(note: memctl error output present)"

# Raw-disk assertions: deleted memory must be gone from the image, live control record must be present.
if strings disk.img | grep -q "ROTATION123"; then echo "FAIL: deleted memory text still present on raw disk image"; rm -f "$LOGFILE20"; exit 1; fi
strings disk.img | grep -q "LIVEMARKER_KEEP_7788" || { echo "FAIL: control record not found on disk (grep not meaningful)"; rm -f "$LOGFILE20"; exit 1; }

# Config files shipped on the image
mdir -i disk.img ::/etc | grep -qi "memory.*conf" || { echo "FAIL: /etc/memory.conf missing"; rm -f "$LOGFILE20"; exit 1; }
mdir -i disk.img ::/etc | grep -qi "index.*conf" || { echo "FAIL: /etc/index.conf missing"; rm -f "$LOGFILE20"; exit 1; }

echo "[PASS] Session 20: Persistent memory, principals, memd, memctl, and indexd verified!"
