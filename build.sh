#!/bin/sh
# Build version.dll (Windows x64) with Zig. Install it with: pip install ziglang
set -e
cd "$(dirname "$0")"
mkdir -p dist
python3 -m ziglang cc -target x86_64-windows-gnu -O2 -shared -Wall -Wextra \
  -o dist/version.dll src/version_proxy.c src/fxcore.c src/version.def -ladvapi32 -s
rm -f dist/*.lib dist/*.pdb
echo "Built dist/version.dll"
