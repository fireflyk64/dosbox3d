/* room.c: a room on screen (docs/engine.md): its picture, who sits where,
 * the actor, the hotspots, the pointer and its label.
 *
 * Nothing is drawn twice: changes mark rows dirty (the pointer's old and new
 * rows, a pilot turning, the actor's next frame), and scene_draw redraws
 * just those rows, back to front, then flips them.  The game draws its own
 * things (text, boards, menus) through the overlay hook, called for every
 * band drawn. */
#include <stdio.h>
#include <string.h>
#include "rv.h"
#include "room.h"

Scene g_scene;
/* the actor's frame before this one, while the two are stippled together for a step, as a seated pilot's turn is (the
 * user, 2026-11-03: "the barkeep should also have the transitional stipple pattern like other characters"); -1 none */
static int __far g_actor_was = -1;
static unsigned long __far g_actor_mix_end;

static const RvRoom __far *room(void) { return dat_room(g_scene.room); }

static void dirty_asset(unsigned a)
{
    const RvAsset __far *as = dat_asset(a);
    vid_dirty(as->y, as->y + as->h);
}

static unsigned pose_asset(int seat, int which)
{
    const RvRoom __far *r = room();
    int i;
    for (i = 0; i < r->nposes; i++)
        if (r->pose[i].seat == seat && r->pose[i].pilot == g_scene.seat[seat])
            return which ? r->pose[i].face : r->pose[i].talk;
    return 0xFFFF;
}

static void dirty_pointer(void)
{
    vid_dirty(g_scene.py - 2, g_scene.py + 20);
}

void scene_enter(int room_id, const unsigned char *seats)
{
    const RvRoom __far *r;
    int i;
    if (g_scene.leave)
        g_scene.leave();
    dat_enter(XMS_ROOM);                /* (the last room's pictures given back: this one's read as drawn) */
    memset(&g_scene, 0, sizeof g_scene);
    g_scene.room = room_id;
    r = room();
    for (i = 0; i < 3; i++) {
        g_scene.seat[i] = seats ? seats[i] : 0xFF;
        if (g_scene.seat[i] != 0xFF && pose_asset(i, 0) == 0xFFFF) {
            RV_LOG("room %s: no art of pilot %u in seat %d", r->key, g_scene.seat[i], i);
            g_scene.seat[i] = 0xFF;
        }
    }
    g_scene.hover_kind = HOVER_NONE;
    g_actor_was = -1;                   /* (no frame half in from the last room's actor) */
    g_scene.actor_next = ticks();
    g_scene.glance_at = ticks() + 180;
    gfx_use_palette(r->palette);
    vid_dirty(0, SCR_H);
    rvWhere = (uint8_t)(room_id + 1);   /* (RV_WHERE_*: a room is its number + 1) */
    roomEntered(room_id);
}

extern uint8_t g_relabel;

/* what is under the pointer */
static void find_hover(int x, int y, int *kind, int *index)
{
    const RvRoom __far *r = room();
    int i;
    for (i = 0; i < 3; i++) {
        unsigned a;
        if (g_scene.seat[i] == 0xFF)
            continue;
        a = pose_asset(i, g_scene.turn[i] >= 2);
        if (gfx_sprite_hit(a, 0, 0, x, y)) {
            *kind = HOVER_SEAT;
            *index = i;
            return;
        }
    }
    for (i = 0; i < r->nhot; i++) {     /* (before the actor: its rectangle is wider than the figure, and the hatch behind
                                           the bar's Tally is mostly inside it) */
        const RvHot __far *h = &r->hot[i];
        if (i + 1 == g_scene.hot_gone)
            continue;
        if (x >= h->x && y >= h->y && x < h->x + h->w && y < h->y + h->h) {
            *kind = HOVER_HOT;
            *index = i;
            return;
        }
    }
    if (r->has_actor && !g_scene.actor_gone && x >= r->actor.x && y >= r->actor.y && x < r->actor.x + r->actor.w
        && y < r->actor.y + r->actor.h) {
        *kind = HOVER_ACTOR;
        *index = 0;
        return;
    }
    *kind = HOVER_NONE;
    *index = 0;
}

static void set_label(const char __far *s)
{
    if (_fstrcmp(g_scene.label, s) == 0)
        return;
    _fstrncpy(g_scene.label, s, sizeof g_scene.label - 1);
    vid_dirty(SCR_H - LABEL_H, SCR_H);
}

/* the pointer's centre on a hotspot, for Tab */
static void hot_centre(int kind, int index, int *x, int *y)
{
    const RvRoom __far *r = room();
    if (kind == HOVER_SEAT) {
        const RvAsset __far *as = dat_asset(pose_asset(index, 0));
        *x = as->x + as->w / 2;
        *y = as->y + as->h / 3;
    } else if (kind == HOVER_ACTOR) {
        *x = r->actor.x + r->actor.w / 2;
        *y = r->actor.y + r->actor.h / 3;
    } else {
        *x = r->hot[index].x + r->hot[index].w / 2;
        *y = r->hot[index].y + r->hot[index].h / 2;
    }
}

/* where the last Tab put the pointer, and on what: while the pointer stays there, that is the target (its label, its
 * pointer, Enter), and the next Tab goes on from it: not whatever is under the pointer, as a seated pilot's sprite over
 * a hatch's centre would open the pilot on Enter and send the round back to the hatch for ever, the targets beyond
 * unreachable */
static int s_tab_x = -1, s_tab_y = -1, s_tab_kind, s_tab_index;

/* Tab and Shift+Tab: the next target to the right (or left), wrapping round */
static void tab_move(int dir)
{
    const RvRoom __far *r = room();
    int best = -1, bkind = 0, first = -1, fkind = 0, i, k;
    long bkey = 0, fkey = 0, here;
    /* targets in order of their centres, left to right, then top to bottom */
    if (s_tab_x >= 0 && g_scene.px == s_tab_x && g_scene.py == s_tab_y) {
        here = (long)s_tab_x * 1024 + s_tab_y;
    } else if (g_scene.hover_kind != HOVER_NONE) {
        int x, y;
        hot_centre(g_scene.hover_kind, g_scene.hover_index, &x, &y);
        here = (long)x * 1024 + y;
    } else {
        here = (long)g_scene.px * 1024 + g_scene.py;
    }
    for (k = HOVER_SEAT; k <= HOVER_HOT; k++) {
        int n = k == HOVER_SEAT ? 3 : k == HOVER_ACTOR ? r->has_actor && !g_scene.actor_gone : r->nhot;
        for (i = 0; i < n; i++) {
            int x, y;
            long key;
            if ((k == HOVER_SEAT && g_scene.seat[i] == 0xFF) || (k == HOVER_HOT && i + 1 == g_scene.hot_gone))
                continue;
            hot_centre(k, i, &x, &y);
            key = ((long)x * 1024 + y) * dir;
            if (first < 0 || key < fkey)
                first = i, fkind = k, fkey = key;     /* where the round starts again */
            if (key > here * dir && (best < 0 || key < bkey))
                best = i, bkind = k, bkey = key;
        }
    }
    if (best < 0)
        best = first, bkind = fkind;
    if (best >= 0) {
        int x, y;
        hot_centre(bkind, best, &x, &y);
        RV_NOTE("tab: to %d,%d", x, y);
        in_warp(x, y);
        dirty_pointer();
        g_scene.px = x;
        g_scene.py = y;
        s_tab_x = x;
        s_tab_y = y;
        s_tab_kind = bkind;
        s_tab_index = best;
        dirty_pointer();
    }
}

int scene_update(const Input *in, SceneAction *act)
{
    const RvRoom __far *r = room();
    int kind, index, i;
    unsigned long now = ticks();
    act->kind = ACTION_NONE;
    if (g_scene.no_room) {
        if (in->x != g_scene.px || in->y != g_scene.py) {
            dirty_pointer();
            g_scene.px = in->x;
            g_scene.py = in->y;
            dirty_pointer();
        }
        if (in->clicked || in->key == KEY_ENTER)
            act->kind = ACTION_HOT, act->index = -1, act->action = RV_ACT_NONE, act->arg = 0;
        return act->kind != ACTION_NONE;
    }
    if (in->key == KEY_TAB || in->key == KEY_BACKTAB)
        tab_move(in->key == KEY_TAB ? 1 : -1);
    else if (in->x != g_scene.px || in->y != g_scene.py) {
        dirty_pointer();
        g_scene.px = in->x;
        g_scene.py = in->y;
        dirty_pointer();
    }
    find_hover(g_scene.px, g_scene.py, &kind, &index);
    if (s_tab_x >= 0 && g_scene.px == s_tab_x && g_scene.py == s_tab_y) {   /* (the Tab's target, while it stays) */
        kind = s_tab_kind;
        index = s_tab_index;
    }
    if (kind != g_scene.hover_kind || index != g_scene.hover_index || g_relabel) {
        g_relabel = 0;
        g_scene.hover_kind = kind;
        g_scene.hover_index = index;
        g_scene.pointer = kind == HOVER_HOT && r->hot[index].action == RV_ACT_GOTO;
        if (kind == HOVER_HOT && r->hot[index].action == RV_ACT_GOTO && r->hot[index].arg == RV_R_BRIEFING
            && briefing_locked()) {
            static char s_shut[40];      /* ("Briefing: host in the crew lounge") */
            const char __far *w = peer_where_phrase(0);
            strcpy(s_shut, "Briefing: host ");
            _fstrncat(s_shut, w[0] ? w : (const char __far *)"not in yet", sizeof s_shut - 16);
            g_scene.pointer = 0;         /* (no door's pointer: it does not open) */
            set_label(s_shut);
        } else if (kind == HOVER_HOT)
            set_label(r->hot[index].label);
        else if (kind == HOVER_SEAT)
            set_label(g_people_names[g_scene.seat[index]]);
        else if (kind == HOVER_ACTOR)
            set_label(g_people_names[r->actor.person]);
        else
            set_label("");
        dirty_pointer();
    }
    /* pilots turn towards the pointer, a step every two ticks */
    if (now >= g_scene.turn_next) {
        g_scene.turn_next = now + 2;
        for (i = 0; i < 3; i++) {
            int want = (kind == HOVER_SEAT && index == i) || g_scene.facing == i + 1 ? 2 : 0;
            if (g_scene.seat[i] == 0xFF || g_scene.turn[i] == want)
                continue;
            g_scene.turn[i] += g_scene.turn[i] < want ? 1 : -1;
            dirty_asset(pose_asset(i, 0));
            dirty_asset(pose_asset(i, 1));
        }
    }
    /* the actor: its idle loop, the hover frame, now and then a glance */
    if (r->has_actor && !g_scene.actor_gone) {
        const RvActor __far *a = &r->actor;
        int frame = g_scene.actor_frame;
        if (kind == HOVER_ACTOR || g_scene.facing == 9) {
            frame = a->hover;
        } else if (now >= g_scene.glance_at) {
            frame = a->glance;
            if (now >= g_scene.glance_at + 27) {
                g_scene.glance_at = now + 160 + (now % 90);
                g_scene.actor_next = now;
            }
        } else if (now >= g_scene.actor_next) {
            g_scene.actor_step = (g_scene.actor_step + 1) % a->nidle;
            frame = a->idle[g_scene.actor_step][0];
            g_scene.actor_next = now + a->idle[g_scene.actor_step][1];
        } else if (frame == a->hover || frame == a->glance) {
            frame = a->idle[g_scene.actor_step][0];
        }
        if (frame != g_scene.actor_frame) {
            g_actor_was = g_scene.actor_frame;   /* (the two mixed for two ticks, a seat's turn step) */
            g_actor_mix_end = now + 2;
            g_scene.actor_frame = frame;
            vid_dirty(a->y, a->y + a->h);
        } else if (g_actor_was >= 0 && now >= g_actor_mix_end) {
            g_actor_was = -1;
            vid_dirty(a->y, a->y + a->h);
        }
    }
    if (in->clicked || in->key == KEY_ENTER) {
        act->kind = kind == HOVER_HOT ? ACTION_HOT : kind == HOVER_SEAT ? ACTION_SEAT
                  : kind == HOVER_ACTOR ? ACTION_ACTOR : ACTION_NONE;
        act->index = index;
        if (kind == HOVER_HOT) {
            act->action = r->hot[index].action;
            act->arg = r->hot[index].arg;
        } else if (kind == HOVER_SEAT) {
            act->arg = g_scene.seat[index];
        } else if (kind == HOVER_ACTOR) {
            act->arg = r->actor.person;
        }
    }
    return act->kind != ACTION_NONE;
}

static void draw_pointer(int y0, int y1)
{
    if (!g_scene.hide_pointer) {
        const RvDatHeader __far *h = dat_header();
        gfx_sprite(g_scene.pointer ? h->ptr_door : h->ptr_arrow, g_scene.px, g_scene.py, y0, y1);
    }
}

static char g_chat[84];                 /* the chat's caption (below) */

/* --- where the other pilots are (room.h) --- */
static const char __far s_w1[] = "BAR", s_w2[] = "CREW LOUNGE", s_w3[] = "BRIEFING", s_w4[] = "QUARTERS",
    s_w5[] = "LOCKER", s_w6[] = "FLIGHT DECK", s_w7[] = "FLYING", s_w8[] = "SIMULATOR", s_w9[] = "DEBRIEFING",
    s_w10[] = "SHOT DOWN";
static const char __far *const __far s_where[] = { 0, s_w1, s_w2, s_w3, s_w4, s_w5, s_w6, s_w7, s_w8, s_w9, s_w10 };
static const char __far s_p1[] = "in the bar", s_p2[] = "in the crew lounge", s_p3[] = "in the briefing",
    s_p4[] = "in quarters", s_p5[] = "at the locker", s_p6[] = "on the deck", s_p7[] = "flying",
    s_p8[] = "in the simulator", s_p9[] = "at the debriefing", s_p10[] = "shot down";
static const char __far *const __far s_phrase[] = { 0, s_p1, s_p2, s_p3, s_p4, s_p5, s_p6, s_p7, s_p8, s_p9, s_p10 };
#define WHERE_N 11
static uint8_t g_where_seen[4], g_lock_seen, g_where_chat;
uint8_t g_relabel;                      /* (scene_update: the hover's label made again) */
#define PEER_WHERE(k) (((const uint8_t *)&rvPeerWhere)[k])

int peer_where(int player)
{
    return player >= 0 && player < 4 ? PEER_WHERE(player) : 0;
}

int briefing_locked(void)
{
    return rvNetRole == 2 && rvBriefingLock;
}

const char __far *peer_where_phrase(int player)
{
    uint8_t w = player >= 0 && player < 4 ? PEER_WHERE(player) : 0;
    return w && w < WHERE_N ? s_phrase[w] : (const char __far *)"";
}

/* the corner's line: "HOST: CREW LOUNGE", a guest's "GUEST: BRIEFING" (two guests: "GUEST 1", "GUEST 2"); "" none */
static int presence_line(char *out, int n)
{
    int k, guests = 0, len = 0;
    out[0] = 0;
    if (!rvNetRole)
        return 0;
    for (k = 1; k < 4; k++)
        guests += PEER_WHERE(k) && PEER_WHERE(k) < WHERE_N;
    for (k = 0; k < 4; k++) {
        uint8_t w = PEER_WHERE(k);
        char who[10];
        if (!w || w >= WHERE_N)
            continue;
        if (k == 0)
            strcpy(who, "HOST");
        else if (guests > 1)
            sprintf(who, "GUEST %d", k);
        else
            strcpy(who, "GUEST");
        if (len + (int)strlen(who) + 2 + (int)_fstrlen(s_where[w]) + 3 >= n)
            break;
        len += sprintf(out + len, "%s%s: %Fs", len ? "   " : "", who, s_where[w]);
    }
    return len;
}

void presence_tick(void)
{
    int k, changed = rvBriefingLock != g_lock_seen || (g_chat[0] != 0) != g_where_chat;
    for (k = 0; k < 4; k++)
        if (PEER_WHERE(k) != g_where_seen[k]) {
            g_where_seen[k] = PEER_WHERE(k);
            changed = 1;
        }
    if (rvBriefingLock != g_lock_seen) {
        g_lock_seen = rvBriefingLock;
        g_relabel = 1;                  /* (the briefing door's label and pointer again at the next update) */
    }
    if (changed) {
        g_where_chat = g_chat[0] != 0;
        vid_dirty(0, CHAT_H + 24);
        vid_dirty(SCR_H - LABEL_H, SCR_H);
    }
}

/* in the top right corner, below the chat's caption while it shows; in the briefing and on the deck, whose own titles
 * run across the top, the bottom right */
static void draw_presence(int y0, int y1)
{
    char line[64];
    int w, y = g_scene.room == RV_R_BRIEFING || g_scene.room == RV_R_DECK ? SCR_H - LABEL_H + 3 : g_chat[0] ? CHAT_H + 2 : 4;
    if (!presence_line(line, sizeof line) || y1 <= y - 2 || y0 >= y + 18)
        return;
    w = gfx_text_width(FONT_BODY, line);
    gfx_shade(SCR_W - w - 16, y - 2, w + 12, 18, y0, y1);
    gfx_text(FONT_BODY, SCR_W - w - 10, y, line, RV_TEXT_AMBER | 0x10, y0, y1);
}

/* --- the chat's caption (room.h) --- */
static uint16_t g_chat_seen;            /* commTextTimer as last taken */
static unsigned long g_chat_until;      /* ticks(): the caption off then */

int chat_take(char *out, int n, uint16_t *speaker)
{
    if (commTextTimer == g_chat_seen)
        return 0;
    g_chat_seen = commTextTimer;
    if (n > commGlobalTxt_SIZE + 1)
        n = commGlobalTxt_SIZE + 1;
    memcpy(out, commGlobalTxt, n - 1);  /* (near to near: a fixed-block address is a constant, and as a far pointer
                                           Watcom makes it segment 0) */
    out[n - 1] = 0;
    *speaker = commSpeaker;
    return out[0] != 0;
}

void scene_caption(const char __far *text)
{
    _fstrncpy(g_chat, text, sizeof g_chat - 1);
    g_chat[sizeof g_chat - 1] = 0;
    g_chat_until = ticks() + 55 + strlen(g_chat) * 2;
    vid_dirty(0, CHAT_H + 24);
}

void scene_caption_off(const char __far *text)
{
    if (g_chat[0] && !_fstrcmp(g_chat, text)) {
        g_chat[0] = 0;
        vid_dirty(0, CHAT_H + 24);
    }
}

void scene_chat(void)
{
    uint16_t who;
    if (chat_take(g_chat, sizeof g_chat, &who)) {
        g_chat_until = ticks() + 55 + strlen(g_chat) * 2;   /* (3 s, and a letter's reading on top) */
        vid_dirty(0, CHAT_H);
    } else if (g_chat[0] && ticks() > g_chat_until) {
        g_chat[0] = 0;
        vid_dirty(0, CHAT_H);
    }
}

static void draw_chat(int y0, int y1)
{
    if (g_chat[0] && y0 < CHAT_H) {
        int w = gfx_text_width(FONT_BODY, g_chat);
        gfx_shade(0, 0, SCR_W, CHAT_H, y0, y1);
        gfx_text(FONT_BODY, w < SCR_W - 16 ? (SCR_W - w) / 2 : 8, 4, g_chat, RV_TEXT_AMBER | 0x10, y0, y1);
    }
}

static void draw_band(int y0, int y1)
{
    const RvRoom __far *r = room();
    int i;
    if (g_scene.full) {
        g_scene.full(y0, y1);
        draw_chat(y0, y1);
        draw_pointer(y0, y1);
        return;
    }
    gfx_image(r->back, 0, 0, y0, y1);
    if (g_scene.underlay)
        g_scene.underlay(y0, y1);
    for (i = 0; i < 3; i++) {
        if (g_scene.seat[i] == 0xFF)
            continue;
        if (g_scene.turn[i] == 0)
            gfx_sprite(pose_asset(i, 0), 0, 0, y0, y1);
        else if (g_scene.turn[i] == 2)
            gfx_sprite(pose_asset(i, 1), 0, 0, y0, y1);
        else {
            gfx_sprite(pose_asset(i, 0), 0, 0, y0, y1);
            gfx_sprite_dither(pose_asset(i, 1), 0, 0, y0, y1, 1);
        }
    }
    if (r->has_actor && !g_scene.actor_gone) {
        if (g_actor_was >= 0) {         /* (between two frames: the one before, the new one stippled over it) */
            gfx_image(r->actor.frame0 + g_actor_was, AT_ASSET, AT_ASSET, y0, y1);
            gfx_image_dither(r->actor.frame0 + g_scene.actor_frame, AT_ASSET, AT_ASSET, y0, y1, 1);
        } else
            gfx_image(r->actor.frame0 + g_scene.actor_frame, AT_ASSET, AT_ASSET, y0, y1);
    }
    if (g_scene.overlay)
        g_scene.overlay(y0, y1);
    if (g_scene.label[0] && y1 > SCR_H - LABEL_H) {
        int w = gfx_text_width(FONT_BODY, g_scene.label);
        gfx_shade(0, SCR_H - LABEL_H, SCR_W, LABEL_H, y0, y1);
        gfx_text(FONT_BODY, (SCR_W - w) / 2, SCR_H - LABEL_H + 3, g_scene.label, RV_TEXT_AMBER | 0x10, y0, y1);
    }
    draw_chat(y0, y1);
    draw_presence(y0, y1);
    draw_pointer(y0, y1);
}

void scene_draw(void)
{
    int y0, y1;
    while (vid_dirty_band(&y0, &y1)) {
        draw_band(y0, y1);
        vid_flip();
    }
}
