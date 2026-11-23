/*
 *  Privateer's hot leaf routines made natively, and its frame wait idled (wcnet_priv_native.cpp, docs/privateer.md
 *  section 21).
 */
#ifndef WCNET_PRIV_NATIVE_H_
#define WCNET_PRIV_NATIVE_H_

#include "dosbox.h"

namespace wc {

void priv_native_watch(void (*watch)(Bit16u ip));   // the places hooked; called last, for what only they watch
bool priv_native_hook(Bit32u ip);                   // one came up: true when nothing else is to look at it (never waits)

}  // namespace wc

#endif
