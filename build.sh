#!/bin/sh
# Cross-compile NX Control for 64-bit Windows with MinGW-w64.
set -e
cd "$(dirname "$0")/src"
x86_64-w64-mingw32-windres nxcontrol.rc -O coff -o res.o
x86_64-w64-mingw32-gcc -O2 -municode -mwindows -Wall -Wextra \
  -o ../NXControl.exe nxcontrol.c res.o \
  -lws2_32 -lbthprops -lshell32 -luser32 -ladvapi32 -lgdi32 -static -s
rm -f res.o
echo "Built NXControl.exe"
