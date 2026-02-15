#!/usr/bin/env python3
"""embed_html.py — Convert an HTML file into a C header with a string literal."""

import sys
import os

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.html> <output.h>", file=sys.stderr)
        sys.exit(1)

    src, dst = sys.argv[1], sys.argv[2]

    with open(src, 'r', encoding='utf-8') as f:
        html = f.read()

    with open(dst, 'w', encoding='utf-8') as f:
        f.write("/* web_bundle.h — auto-generated, do not edit */\n")
        f.write("#ifndef WEB_BUNDLE_H\n")
        f.write("#define WEB_BUNDLE_H\n\n")
        f.write("static const char index_html[] =\n")

        for line in html.splitlines():
            escaped = line.replace('\\', '\\\\').replace('"', '\\"')
            f.write(f'    "{escaped}\\n"\n')

        f.write(";\n\n")
        f.write("#endif /* WEB_BUNDLE_H */\n")

    print(f"[embed] {src} -> {dst} ({len(html)} bytes)")

if __name__ == '__main__':
    main()
