#!/usr/bin/env python3
"""Regenerates font_terminus.h from the Terminus PSF console fonts (package console-setup / kbd).
   usage: python3 tools/gen_font.py > font_terminus.h"""
import gzip, sys
def load(p):
    d = gzip.open(p).read()
    assert d[:2] == bytes([0x36, 0x04]) and d[3] == 16
    return d[4:4 + 256 * 16]
def arr(name, g):
    s = "static const unsigned char %s[224][16] = {\n" % name
    for c in range(0x20, 0x100):
        s += "  {" + ",".join("0x%02x" % b for b in g[c*16:c*16+16]) + "}, /* %02X */\n" % c
    return s + "};\n"
reg = load('/usr/share/consolefonts/Lat15-Terminus16.psf.gz')
bld = load('/usr/share/consolefonts/Lat15-TerminusBold16.psf.gz')
sys.stdout.write("/* 8x16 bitmap font, glyphs 0x20..0xFF (Latin-9). Terminus Font (OFL 1.1). Generated - do not edit. */\n"
                 "#ifndef FONT_TERMINUS_H\n#define FONT_TERMINUS_H\n" + arr('font_reg', reg) + arr('font_bold', bld) + "#endif\n")
