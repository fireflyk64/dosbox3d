/*
 *  Privateer's hot leaf routines made natively, its frame wait idled, and its bases' wait for a key idled
 *  (wcnet_priv_native.cpp, docs/privateer.md sections 21, 27).
 */
#ifndef WCNET_PRIV_NATIVE_H_
#define WCNET_PRIV_NATIVE_H_

#include "dosbox.h"

namespace wc {

void priv_native_watch(void (*watch)(Bit16u ip));   // the places hooked; called last, for what only they watch
bool priv_native_hook(Bit32u ip);                   // one came up: true when nothing else is to look at it (never waits)
void priv_dos_keyboard_poll();                      // DOS's INT 21h AH=0Ch was called (src/dos/dos.cpp): a base's wait idled

}  // namespace wc

#endif
