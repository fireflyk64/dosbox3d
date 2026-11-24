/*
 *  The 640x400 flight's sources (wcnet_tie640*.cpp) are compiled twice: for the TIE Fighter demo (TIEMISS.EXE) and,
 *  with T640_XWING defined (wcnet_xw640*.cpp, each a file that defines it and includes its twin), for X-Wing
 *  Collector's CD (BFLIGHT.OVL), whose renderer is the same code at other addresses (docs/xwing.md section 6).
 *
 *  XA(tie, xwing): a place or a variable of the game's, each build's own value.  The names the sources share
 *  (tie640_...) are the X-Wing build's xw640_... (wcnet_xw640_names.h, made from the sources by
 *  scripts/t640-names.sh).
 */
#ifndef WCNET_T640_GAME_H_
#define WCNET_T640_GAME_H_

#ifdef T640_XWING
#include "wcnet_xw640_names.h"
#define XA(tie, xwing) (xwing)
#define T640_GAME "X-Wing"
#else
#define XA(tie, xwing) (tie)
#define T640_GAME "TIE demo"
#endif

#endif
