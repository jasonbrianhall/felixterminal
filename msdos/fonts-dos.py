#!/usr/bin/env python3
"""fonts-dos.py OUTDIR HEADER... : the fonts FLT.EXE embeds, as their bytes.

felixterminal's font headers (../DejaVuMono.h, ...) hold each TTF as a base64
string, which the other systems decode into memory at startup. On DOS that
would be a second copy of every font (about 2 MB) on a heap that's never
given back, so the Makefile links the decoded bytes in instead and FreeType
reads them where they are. For each header this writes OUTDIR/NAME.ttf, and
OUTDIR/fonts.S, which .incbin's them as font_NAME (and font_NAME_size), NAME
being the header's name in lower case: font_dejavumono, font_freemono, ...
"""
import base64, os, re, sys

out, headers = sys.argv[1], sys.argv[2:]
os.makedirs(out, exist_ok=True)
asm = ["# made by fonts-dos.py from the font headers: don't edit", "\t.text"]
for h in headers:
    name = os.path.splitext(os.path.basename(h))[0].lower()
    src = open(h, encoding="ascii").read()
    m = re.search(r'static const char \w+_B64\[\]\s*=\s*((?:\s*"[^"]*")+)\s*;', src)
    if not m:
        sys.exit(f"fonts-dos.py: no base64 font in {h}")
    data = base64.b64decode("".join(re.findall(r'"([^"]*)"', m.group(1))))
    size = re.search(r'_ORIGINAL_SIZE\s*=\s*(\d+)', src)
    if size and int(size.group(1)) != len(data):
        sys.exit(f"fonts-dos.py: {h}: {len(data)} bytes, the header says {size.group(1)}")
    with open(os.path.join(out, name + ".ttf"), "wb") as f:
        f.write(data)
    asm += [f"\t.p2align 2", f"\t.globl _font_{name}", f"\t.globl _font_{name}_size",
            f"_font_{name}_size:\t.long {len(data)}",
            f"_font_{name}:\t.incbin \"{name}.ttf\""]
with open(os.path.join(out, "fonts.S"), "w") as f:
    f.write("\n".join(asm) + "\n")
