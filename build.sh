#!/bin/bash
set -e
g++ -O2 -fPIC main.cpp -o zer0-kristal $(pkg-config --cflags --libs Qt6Widgets Qt6Multimedia)
echo "OK -> ./zer0-kristal"
