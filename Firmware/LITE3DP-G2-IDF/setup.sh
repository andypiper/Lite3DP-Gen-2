#!/usr/bin/env bash
# Extracts Arduino library ZIPs into the component directories.
# Run once before 'idf.py build'.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LIB_DIR="$SCRIPT_DIR/../Libraries"
COMP_DIR="$SCRIPT_DIR/components"

require_zip() {
    local path="$LIB_DIR/$1"
    if [[ ! -f "$path" ]]; then
        echo "ERROR: $path not found" >&2
        exit 1
    fi
}

require_zip "TFT_eSPI-master.zip"
require_zip "PNGdec-master.zip"
require_zip "SDFat.zip"
require_zip "XPT2046_Touchscreen-master.zip"

echo "Extracting TFT_eSPI..."
unzip -q -o "$LIB_DIR/TFT_eSPI-master.zip" -d "$COMP_DIR/tft_espi/"
# Our configured User_Setup.h overwrites the default
cp "$COMP_DIR/tft_espi/User_Setup.h" \
   "$COMP_DIR/tft_espi/TFT_eSPI-master/User_Setup.h"

echo "Extracting PNGdec..."
unzip -q -o "$LIB_DIR/PNGdec-master.zip" -d "$COMP_DIR/pngdec/"

echo "Extracting SdFat..."
unzip -q -o "$LIB_DIR/SDFat.zip" -d "$COMP_DIR/sdfat/"

echo "Extracting XPT2046_Touchscreen..."
unzip -q -o "$LIB_DIR/XPT2046_Touchscreen-master.zip" -d "$COMP_DIR/xpt2046/"

echo ""
echo "Done. Next steps:"
echo "  cd $(basename "$SCRIPT_DIR")"
echo "  idf.py set-target esp32"
echo "  idf.py build"
echo "  idf.py -p /dev/ttyUSB0 flash monitor"
