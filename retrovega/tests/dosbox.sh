# tests/dosbox.sh, sourced by the rigs: DOSBOX, the emulator they run RV.EXE in.
#   $DOSBOX when set; else the DOSBox built in the tree RetroVega sits in, when it sits inside dosbox3d
#   (build-native/src/dosbox: it has the RVGX sprite card, GX=1, and the multiplayer layer); else a dosbox on the PATH
#   (any stock DOSBox runs RV.EXE; without the RVGX card the game draws its ships itself, as on any PC).
if [ -z "${DOSBOX:-}" ]; then
    _rv_root=$(cd "$(dirname "$0")" && git rev-parse --show-toplevel 2> /dev/null || pwd)
    for _d in "$_rv_root/build-native/src/dosbox" "$_rv_root/../build-native/src/dosbox"; do
        if [ -x "$_d" ]; then DOSBOX=$_d; break; fi
    done
    [ -n "${DOSBOX:-}" ] || DOSBOX=$(command -v dosbox 2> /dev/null || true)
    [ -n "${DOSBOX:-}" ] || { echo "no DOSBox: install one (any stock DOSBox runs RV.EXE), or set DOSBOX=path" >&2; exit 2; }
fi
[ -n "${RV_DOSBOX_SAID:-}" ] || echo "DOSBox: $DOSBOX" >&2
export DOSBOX RV_DOSBOX_SAID=1
