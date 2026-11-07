/* bot.h: the native game's test pilot (bot.c; RV_BOT=1) */
#ifndef RV_BOT_H
#define RV_BOT_H
#include <stdint.h>
void bot_tick(void);                    /* the host's events: the pilot's hands, once a frame */
void bot_thunk(uint16_t fn, const uint16_t *args);   /* a thunk called (its numbers) */
void bot_thunk_leave(uint16_t fn, const uint16_t *args);   /* and back from it */
#endif
