#!/bin/bash
# The X-Wing build of the 640x400 flight's sources (src/cpu/wcnet_t640_game.h): every name the sources share,
# tie640_..., as xw640_..., and the VGA side's three.  Writes src/cpu/wcnet_xw640_names.h.
cd "$(dirname "$0")/../src/cpu" || exit 1
{
  echo "// (made by scripts/t640-names.sh from the names in wcnet_tie640*.cpp and wcnet_tie640.h: do not edit)"
  echo "#ifndef WCNET_XW640_NAMES_H_"
  echo "#define WCNET_XW640_NAMES_H_"
  grep -oh "\btie640_[A-Za-z0-9_]*" wcnet_tie640*.cpp wcnet_tie640*.inc wcnet_tie640.h | sort -u | sed 's/^tie640_\(.*\)$/#define tie640_\1 xw640_\1/'
  echo "#define wc_hires_write xw640_hires_write"
  echo "#define wc_hires_read xw640_hires_read"
  echo "#define wc_hires_vsync xw640_hires_vsync"
  echo "#endif"
} > wcnet_xw640_names.h
wc -l wcnet_xw640_names.h
