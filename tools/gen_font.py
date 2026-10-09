import gzip
import struct

def main():
    psf1 = gzip.open('/usr/share/consolefonts/Uni2-Terminus16.psf.gz').read()
    glyphs = psf1[4:4+8192]
    # PSF2 header: magic, version=0, headersize=32, flags=0, length=512, charsize=16, height=16, width=8
    hdr = struct.pack('<4sIIIIIII', b'\x72\xb5\x4a\x86', 0, 32, 0, 512, 16, 16, 8)
    psf2 = hdr + glyphs

    with open('src/font.psf', 'wb') as f:
        f.write(psf2)

    with open('src/font.c', 'w') as f:
        f.write('/* Terminus 8x16 font embedded in PSF2 format\n')
        f.write(' * Copyright (c) 2011-2020 Dimitar Toshkov Zhekov\n')
        f.write(' * Licensed under the SIL Open Font License, Version 1.1 (OFL)\n')
        f.write(' */\n\n')
        f.write('#include "font.h"\n\n')
        f.write('const uint8_t g_font_terminus16[8224] = {\n')
        for i in range(0, len(psf2), 16):
            chunk = psf2[i:i+16]
            f.write('    ' + ', '.join(f'0x{b:02x}' for b in chunk) + ',\n')
        f.write('};\n\n')
        f.write('const size_t g_font_terminus16_size = sizeof(g_font_terminus16);\n\n')
        f.write('const struct psf2_header* font_get_header(void) {\n')
        f.write('    return (const struct psf2_header*) g_font_terminus16;\n')
        f.write('}\n\n')
        f.write('const uint8_t* font_get_glyph(unsigned char c) {\n')
        f.write('    uint32_t idx = (uint32_t) c;\n')
        f.write('    if (idx >= 512) idx = \'?\';\n')
        f.write('    return g_font_terminus16 + 32 + (idx * 16);\n')
        f.write('}\n')

    print('Generated src/font.c and src/font.psf successfully!')

if __name__ == '__main__':
    main()
