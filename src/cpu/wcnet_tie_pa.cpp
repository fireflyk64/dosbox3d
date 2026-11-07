/*
 *  The ship's address system's call to the briefing room, played by the emulator itself for TIE Fighter's guests
 *  (docs/tiefighter.md, step 13; the user, 2026-11-07: "send a message to the other player (or also play a
 *  loudspeaker sound we can copy from retrovega in dosbox itself) every 20 seconds to guide them to the briefing
 *  room").  The sound is RetroVega's pa_report (wcnet_tie_pa.h, made by scripts/tf-pa-sound.py), on a mixer channel of
 *  its own, enabled while it plays.
 */
#include "dosbox.h"
#include "mixer.h"
#include "wcnet_tie.h"
#include "wcnet_tie_pa.h"

namespace wc {

namespace {

MixerChannel *g_pa = NULL;
unsigned g_paPos = 0;
bool g_paOn = false;

void pa_callback(Bitu len) {
    Bit16s buf[512];
    while (len > 0) {
        const Bitu n = len > 512 ? 512 : len;
        for (Bitu i = 0; i < n; i++) {
            buf[i] = g_paOn && g_paPos < kPaSamples ? (Bit16s)(kPaSound[g_paPos++] * 180) : 0;   // (some 70% of full)
        }
        g_pa->AddSamples_m16(n, buf);
        len -= n;
    }
    if (g_paPos >= kPaSamples && g_paOn) {
        g_paOn = false;
        g_pa->Enable(false);
    }
}

}  // namespace

void tie_pa_play() {
    if (!g_pa) {
        g_pa = MIXER_AddChannel(pa_callback, kPaRate, "PA");
    }
    g_paPos = 0;
    g_paOn = true;
    g_pa->Enable(true);
}

}  // namespace wc
