/* room.h: a room on screen (room.c) */
#ifndef ROOM_H
#define ROOM_H

#include "rvhooks.h"

#define LABEL_H 20                      /* the hover label's strip at the bottom */
enum { HOVER_NONE, HOVER_SEAT, HOVER_ACTOR, HOVER_HOT };
enum { ACTION_NONE, ACTION_HOT, ACTION_SEAT, ACTION_ACTOR };

typedef struct {
    int kind, index;                    /* ACTION_*, and which seat / hotspot */
    int action, arg;                    /* a hotspot's RV_ACT_* and its room; a person's row */
} SceneAction;

typedef struct {
    int room;
    unsigned char seat[3];              /* people.csv row per seat, 0xFF empty */
    unsigned char turn[3];              /* 0 talk pose, 1 between, 2 facing the player */
    int facing;                         /* 1-3: that seat faces the player anyway; 9: the actor */
    unsigned long turn_next;
    int actor_frame, actor_step;
    unsigned long actor_next, glance_at;
    unsigned char actor_gone;           /* the room's actor was lost in a mission: not drawn, not pointed at */
    unsigned char hot_gone;             /* 1 + a hotspot that went with them; 0 none */
    int hover_kind, hover_index;
    int px, py, pointer, hide_pointer;
    char label[40];
    void (*underlay)(int y0, int y1);   /* room-specific pictures, under the people */
    void (*overlay)(int y0, int y1);    /* the game's text and panels, over everything but the pointer */
    void (*full)(int y0, int y1);       /* set: the game draws the whole screen (close-ups, endings) */
    void (*leave)(void);                /* set: called as the next room is entered (what the room holds in XMS goes then) */
    int no_room;                        /* no room's hotspots or people: only the game's screen */
} Scene;

extern Scene g_scene;
extern char __far g_people_names[16][28];     /* "Name (Callsign)" per people.csv row, for labels */

void scene_enter(int room, const unsigned char *seats);
int scene_update(const Input *in, SceneAction *act);
void scene_draw(void);

/* a shared flight's chat (the 0 key's prompt, another player's line or the layer's notice; the layer writes it into the
 * fixed block: commGlobalTxt, commSpeaker, commTextTimer counted on).  chat_take: the next line not yet shown, into out
 * (n bytes), its speaker's slot (0xFFFF a notice): 1, or 0 none.  scene_chat: once a pass of the rooms' loop, a line
 * come is shown as a caption across the top of whatever room screen is up, for as long as reading it takes */
#define CHAT_H 22
int chat_take(char *out, int n, uint16_t *speaker);
void scene_chat(void);

/* a shared flight's base: where the other pilots are (the layer's rvPeerWhere by player number, RV_WHERE_*: a line
 * in the top right corner of every room, the user's 2026-11-05: "Users complain they don't know what the other
 * player is doing"), and a guest's briefing door shut while its host is neither at its briefing nor in its mission
 * (rvBriefingLock).  presence_tick once a pass of the rooms' loop: what changed drawn again */
#define RV_WHERE_FLYING 7
#define RV_WHERE_SIMULATOR 8
#define RV_WHERE_DEBRIEF 9
#define RV_WHERE_DOWN 10
void presence_tick(void);
void scene_caption(const char __far *text);   /* a line of the game's own in the chat's strip, a few seconds */
void scene_caption_off(const char __far *text);   /* that line taken off now, if it is still the one up */
int peer_where(int player);                   /* RV_WHERE_* of that pilot, 0 not known */
int briefing_locked(void);
const char __far *peer_where_phrase(int player);   /* "in the crew lounge": where that pilot is, "" not known */

#endif
