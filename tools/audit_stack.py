import os, re

pattern = re.compile(r'\b(char|uint8_t|uint16_t|int|uint32_t|uint64_t)\s+([a-zA-Z0-9_]+)\[([0-9]+)\]')
for root, dirs, files in os.walk('src'):
    for f in files:
        if f.endswith('.c'):
            p = os.path.join(root, f)
            with open(p, 'r', errors='ignore') as fh:
                for idx, line in enumerate(fh, 1):
                    if 'static' in line:
                        continue
                    m = pattern.search(line)
                    if m:
                        t, var, sz_str = m.group(1), m.group(2), m.group(3)
                        sz = int(sz_str)
                        mult = 2 if t == 'uint16_t' else (4 if t in ('int', 'uint32_t') else (8 if t == 'uint64_t' else 1))
                        total = sz * mult
                        if total >= 512:
                            print(f"{p}:{idx}: {t} {var}[{sz}] ({total} bytes)")

