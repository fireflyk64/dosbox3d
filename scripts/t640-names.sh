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
# ... and the room's and the flight keys' (src/cpu/wcnet_tdm_game.h): tdm_... as xw_..., the calls' kinds and types
{
  echo "// (made by scripts/t640-names.sh from the names in wcnet_tiedemo_session.cpp and wcnet_tiedemo_controls.cpp: do not edit)"
  echo "#ifndef WCNET_XW_TDM_NAMES_H_"
  echo "#define WCNET_XW_TDM_NAMES_H_"
  grep -oh "\btdm_[A-Za-z0-9_]*" wcnet_tiedemo_session.cpp wcnet_tiedemo_controls.cpp | grep -v "^tdm_hello$\|^tdm_state$" | sort -u | sed 's/^tdm_\(.*\)$/#define tdm_\1 xw_\1/'
  for n in CREATE_PROJECTILE LASER_HIT DAMAGE UPDATE_KILLS BOARD SOLO HOST GUEST; do echo "#define TDM_$n XW_$n"; done
  for n in Fn CallBefore CallDone Role; do echo "#define Tdm$n Xw$n"; done
  echo "#endif"
} > wcnet_xw_tdm_names.h
wc -l wcnet_xw_tdm_names.h
