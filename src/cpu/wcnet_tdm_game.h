/*
 *  The TIE Fighter demo's room and flight keys (wcnet_tiedemo_session.cpp, wcnet_tiedemo_controls.cpp) are compiled
 *  twice: for the demo (TIEMISS.EXE) and, with TDM_XWING defined (wcnet_xw_session.cpp, wcnet_xw_controls.cpp: each
 *  defines it and includes its twin), for X-Wing Collector's CD, the same engine in an earlier build (docs/xwing.md).
 *  One source, so a fix to the one game's room is the other's too (the user, 2026-11-24: "Tie fighter had to deal
 *  with a ton of these problems already and so we should ideally use the same code to do it, so the fixes are
 *  shared").
 *
 *  XG(tie, xwing): each build's own value.  TDM_NS: the namespace of the game's tables (tdm, xw: the same names for
 *  the fields the two share).  The names tdm_... are xw_... in X-Wing's build (wcnet_xw_tdm_names.h, made by
 *  scripts/t640-names.sh).
 */
#ifndef WCNET_TDM_GAME_H_
#define WCNET_TDM_GAME_H_

#ifdef TDM_XWING
#include "wcnet_xw.h"
#include "wcnet_xw_tdm_names.h"
#define TDM_NS xw
#define TDM_GAME "X-Wing"
#define XG(tie, xwing) (xwing)
#else
#include "wcnet_tiedemo.h"
#define TDM_NS tdm
#define TDM_GAME "TIE demo"
#define XG(tie, xwing) (tie)
#endif

#endif
