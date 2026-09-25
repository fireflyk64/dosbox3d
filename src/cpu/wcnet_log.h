#ifndef WCNET_LOG_H_
#define WCNET_LOG_H_

// Multiplayer logging.  Level 0 = errors, 1 = lifecycle (default),
// 2 = per-event, 3 = per-frame chatter.  Set WCNET_LOG=<n> to change.
void wclog(int level, const char *fmt, ...);
int wclog_level();

#endif
