/* game.c: the first slice of the game (docs/engine.md).
 *
 * The player walks the carrier's rooms (bar, lounge, quarters, locker),
 * talks to whoever sits there, saves at the bunk, and goes through the
 * bar's hatch to the briefing room for the next mission: the XO speaks
 * over the situation display, then the launch deck, Kit and the fighter,
 * and the flight (flight.c: for now the drawing benchmark, 20 seconds in the
 * cockpit; the mission is still won 80% of the time at random).  The
 * debrief, the campaign's next move, and at the end "You win" or "You lose". */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rv.h"
#include "rvhooks.h"
#include "room.h"
#include "logic.h"
#include "freeze.h"
#include "campaign.h"
#include "lightfield.h"
#include "rocks.h"
#include "ships.h"
#include "gx.h"
#include "explode.h"
#include "lipsync.h"
#include "music.h"
#include "flight.h"
#include "simulator.h"
#include "game.h"

enum { MODE_ROOM, MODE_TALK, MODE_BRIEF, MODE_DECK, MODE_DEBRIEF, MODE_BUNK,
       MODE_NAME, MODE_LOG, MODE_MESSAGE, MODE_ENDING, MODE_BOARD, MODE_DEAD, MODE_HOME, MODE_QUIT };

extern int g_field;                     /* (flight.c: /field D, a test's) */
static int g_mode, g_back_mode;
static RvMission __far g_m;
static unsigned long g_now;
static char g_msg[400];
static int g_won;
static int g_base;                          /* the ship the player lives on: 0 the carrier */
static int change_base(int base);
static void mission_load_assets(void);
static void mission_free_assets(void);
int __far g_fly = -2;                   /* /fly [N]: -2 not asked; -1 the campaign's next mission; N that mission */
int __far g_at = -1;                    /* /at N: the rooms with the campaign at mission N */
int __far g_brief = -1;                 /* /brief N: the campaign at mission N, its briefing first (the page's picked
                                           mission: as Wing Commander's starts at its briefing) */
/* a shared flight (docs/multiplayer.md): the meeting point done for this flight (flight_prepare), and whether it is a
 * guest's (the host's mission, by the host's records: only the sortie and its splashes go into this save) */
static int g_prepared, g_guest;
static int g_rooms_shown;               /* a room has been on the screen (not so at a /fly or /brief boot) */
static uint16_t g_guest_splashes;
static void flight_prepare(void);
int g_winrate = -1;                         /* percent: a launch's chance of a win (/winrate N, a test's: the roll then
                                               decides, whatever the flight did); -1 not given: 80, the stand-in */

/* the speech in progress: shapes, when it began, the text */
#define MAX_STEPS 1600
static unsigned char __far g_lip[MAX_STEPS];
static int g_nlip;
static const char __far *g_said;
static int g_speaker;                       /* people.csv row */
static int g_mouth = -1, g_eyes = -1, g_gesture, g_shown_chars = -1;

static const char *ship_name(int type)
{
    static char name[RV_NAME];
    const RvShipType __far *t = ship_type(type);
    if (!t)
        return "fighter";
    _fstrncpy(name, t->name, sizeof name - 1);
    return name;
}

static unsigned rnd(void)
{
    g_camp.seed = g_camp.seed * 1103515245UL + 12345UL;
    return (unsigned)(g_camp.seed >> 16) & 0x7FFF;
}

/* The talk speed, the bunk menu's choice (kept in RVSETUP.CFG): a percentage
 * of the first pace, lipsync.c's 100 ms a shape.  The user found that pace
 * too slow to read; "normal" is twice it, and "slow" is it. */
static const unsigned g_talk_pcts[] = { 100, 200, 300, 400 };
static const char *g_talk_names[] = { "slow", "normal", "fast", "very fast" };
static int g_talk_speed = 1;
static int g_low_detail;                /* the ships' detail, the bunk menu's choice (0: high, gx.h; only asked with the card there) */
static const char *g_detail_names_[] = { "high", "low" };
static const char *g_diff_names[] = { "normal", "easy" };   /* the difficulty, the bunk menu's choice: g_easy (campaign.c) */
static const char *g_fx_names[] = { "off", "on" };   /* the extra effects, the bunk menu's choice: g_fx_extra (explode.h) */
#define SETUP_FILE "RVSETUP.CFG"

static void setup_load(void)
{
    unsigned char b[10];
    size_t n;
    FILE *f = fopen(SETUP_FILE, "rb");
    if (!f)
        return;
    if ((n = fread(b, 1, 10, f)) >= 8 && !memcmp(b, "RVCFG1", 6) && b[6] < RV_COUNT(g_talk_pcts)) {
        g_talk_speed = b[6];
        g_low_detail = b[7] == 1;       /* (older files have 0 here: high detail, the default) */
        g_easy = n >= 9 && b[8] == 1;   /* (older files end before it: normal) */
        g_fx_extra = n == 10 && b[9] == 1;   /* (and before this: off) */
    }
    gx_set(!g_low_detail);
    fclose(f);
}

static void setup_save(void)
{
    char b[10];
    FILE *f = fopen(SETUP_FILE, "wb");
    memcpy(b, "RVCFG1", 6);
    b[6] = (char)g_talk_speed;
    b[7] = (char)g_low_detail;
    b[8] = (char)g_easy;
    b[9] = (char)g_fx_extra;
    if (f) {
        fwrite(b, 1, 10, f);
        fclose(f);
    }
    RV_LOG("setup: talk speed %s, ship detail %s, difficulty %s, extra fx %s", g_talk_names[g_talk_speed],
           g_detail_names_[g_low_detail], g_diff_names[g_easy], g_fx_names[g_fx_extra]);
}

/* speech runs on the music clock (240 Hz): fine enough for a shape every 25 ms */
static unsigned long g_speech_t0;

static int speaking_step(void)
{
    /* clock ticks x 1000/240 ms x pct/100, in LIP_STEP_MS steps */
    return (int)((clock240() - g_speech_t0) * g_talk_pcts[g_talk_speed] / (24UL * LIP_STEP_MS));
}

static void say(int person, const char __far *text)
{
    static char __far words[RV_TALK + 48];   /* (its own copy: a mission's lines come out of XMS one at a time) */
    _fstrncpy(words, text, sizeof words - 1);
    words[sizeof words - 1] = 0;
    text = words;
    g_speaker = person;
    g_said = text;
    g_nlip = lip_steps(text, g_lip, MAX_STEPS);
    g_speech_t0 = clock240();
    RV_NOTE("talk: %d shapes, %lu ms at %s", g_nlip, (unsigned long)g_nlip * LIP_STEP_MS * 100 / g_talk_pcts[g_talk_speed],
           g_talk_names[g_talk_speed]);
    g_mouth = g_eyes = g_shown_chars = -1;
    g_gesture = 0;
}

static int speech_done(void) { return speaking_step() >= g_nlip; }

static int mouth_now(void)
{
    int s = speaking_step();
    return s < g_nlip ? g_lip[s] : 0;
}

/* --- panels and messages --- */

static void panel(int x, int y, int w, int h, int y0, int y1)
{
    gfx_fill(x, y, w, h, UI_PANEL, y0, y1);
    gfx_frame(x, y, w, h, UI_PANEL_EDGE, y0, y1);
    gfx_frame(x + 1, y + 1, w - 2, h - 2, UI_BLACK, y0, y1);
}

static void message_overlay(int y0, int y1)
{
    panel(110, 120, 420, 150, y0, y1);
    gfx_wrap(FONT_BODY, 126, 134, 388, g_msg, RV_TEXT_WHITE, 4, y0, y1);
    gfx_text(FONT_BODY, 126, 248, "click to close", RV_TEXT_GREY, y0, y1);
}

static void room_overlay(int y0, int y1);

static void message(const char *text)
{
    if (text != g_msg)                  /* (a message made in g_msg itself is there already: no copy onto itself) */
        strncpy(g_msg, text, sizeof g_msg - 1);
    g_back_mode = g_mode;
    g_mode = MODE_MESSAGE;
    g_scene.overlay = message_overlay;
    vid_dirty(110, 280);
}

/* --- rooms --- */

static unsigned long g_talk_at;
static int g_talk_line = -1;

static void talk_overlay(int y0, int y1)
{
    const RvTalk __far *t;
    char line[136];
    if (g_talk_line < 0)
        return;
    t = camp_line(&g_m, g_talk_line);
    sprintf(line, "%.10s: %.120Fs", g_pilots[t->speaker].callsign, t->text);
    /* (a long line on two rows, the second where a short one's only row is) */
    gfx_wrap(FONT_BODY, 16, gfx_text_width(FONT_BODY, line) > SCR_W - 32 ? SCR_H - LABEL_H - 44 : SCR_H - LABEL_H - 22,
             SCR_W - 32, line, RV_TEXT_WHITE | 0x10, 2, y0, y1);
}

/* --- the tally board: Tally's chalk --- */

/* The hero (the first of the role: the game's `you`) and the pilots with marks or a sortie, most splashes first.
 * Every pilot has a row, including each twin; the close-up fits the full squadron and the hero. */
static int person_of(int role);
static int tally_order(int *order)
{
    int i, k, n = 0, hero = person_of(RV_ROLE_HERO);
    for (i = 0; i < (int)g_npilots; i++)
        if (i == hero || (g_pilots[i].role == RV_ROLE_PILOT && (g_camp.pilot[i].kills || g_camp.pilot[i].missions)))
            order[n++] = i;
    for (i = 0; i < n; i++)
        for (k = i + 1; k < n; k++)
            if (g_camp.pilot[order[k]].kills > g_camp.pilot[order[i]].kills) {
                int t = order[i]; order[i] = order[k]; order[k] = t;
            }
    return n;
}

/* the hand's wobble: -range .. range, the same for the same seed and step */
static int wobble(unsigned short seed, int k, int range)
{
    unsigned short h = seed * 0x9E37u + (unsigned)k * 0x85EBu;   /* (a 16-bit hash, as on DOS) */
    h ^= h >> 7;
    h *= 0x2C1Bu;
    h ^= h >> 9;
    return (int)(h % (unsigned)(2 * range + 1)) - range;
}

/* a line of chalk too small to read: a hand's ups and downs from the
 * baseline y, w pixels long, x-height xh, a tall stroke now and then; a
 * heading (two words) breaks after three fifths */
static void scribble(int x, int y, int w, int xh, int words, unsigned seed, int y0, int y1)
{
    int k, px = x, py = y, gap = words > 1 ? x + w * 3 / 5 : x + w;
    for (k = 1; px < x + w; k++) {
        int nx = px + 1 + (wobble(seed, k, 1) >= 0);
        int ny = (k & 1) ? y - xh - (wobble(seed, k + 50, 2) == 2 ? 2 : 0) : y + (wobble(seed, k + 90, 1) > 0);
        gfx_chalk(px, py, nx, ny, 1, RV_TEXT_WHITE, y0, y1);
        px = nx;
        py = ny;
        if (px >= gap && px < x + w - 4) {      /* (a word's end) */
            px += 4;
            py = y;
            gap = x + w;
        }
    }
}

/* n tally marks from x: four strokes h tall and `gap` apart, the fifth across them */
static void tally_marks(int x, int y, int h, int gap, int width, unsigned n, unsigned seed, int y0, int y1)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        int m = (int)(i % 5), a = wobble(seed, (int)i, 1), b = wobble(seed, (int)i + 100, 1);
        if (m < 4)
            gfx_chalk(x + m * gap + a, y + b, x + m * gap - a, y + h + b, width, RV_TEXT_WHITE, y0, y1);
        else {
            gfx_chalk(x - gap / 2, y + h * 3 / 4 + a, x + 3 * gap + gap / 2, y + h / 4 + b, width, RV_TEXT_WHITE, y0, y1);
            x += 6 * gap;
        }
    }
}

/* in the bar: a few scribbles, a heading and the top four with their marks
 * (the user's call: the board's text was out of keeping; click it to read it) */
static void chalkboard(int y0, int y1)
{
    const RvRoom __far *r = dat_room(g_scene.room);
    int b, i, order[RV_PILOTS], n;
    for (b = 0; b < 4; b++) {
        const RvBoard __far *bd = &r->board[b];
        int cx = bd->x + bd->w / 2;
        if (bd->w == 0 || bd->kind != 0 || y1 <= bd->y || y0 >= bd->y + bd->h)
            continue;
        n = tally_order(order);
        scribble(cx - 20, bd->y + 9, 40, 3, 2, 1, y0, y1);
        gfx_chalk(cx - 23, bd->y + 12, cx + 22, bd->y + 11, 1, RV_TEXT_WHITE, y0, y1);
        for (i = 0; i < 4 && i < n; i++) {
            unsigned k = g_camp.pilot[order[i]].kills;
            int len = (int)strlen(g_pilots[order[i]].callsign), x = bd->x + 42;
            tally_marks(x, bd->y + 18 + i * 8, 4, 2, 1, k < 15 ? k : 15, 31 + order[i], y0, y1);
            scribble(bd->x + 8, bd->y + 22 + i * 8, 10 + len * 2, 2, 1, 7 + order[i], y0, y1);
        }
    }
}

/* the board up close: every pilot, in Tally's hand (or the title font, if
 * there was no room for the hand) */
static int g_hand = FONT_CHALK;

static void board_full(int y0, int y1)
{
    const RvDatHeader __far *h = dat_header();
    int sx = h->chalk_slate[0], sy = h->chalk_slate[1], sw = h->chalk_slate[2];
    int order[RV_PILOTS], n = tally_order(order), i, w, rows = (h->chalk_slate[3] - 40 - 12) / 24;
    gfx_image(h->chalk_back, 0, 0, y0, y1);
    if (rows > 0 && n > rows) {         /* (a smaller slate still keeps the hero up) */
        int hero = person_of(RV_ROLE_HERO), at = -1;
        for (i = 0; i < n; i++)
            if (order[i] == hero)
                at = i;
        if (at >= rows) {
            order[rows - 1] = hero;
        }
    }
    w = gfx_text_width(g_hand, "Splash Count");
    gfx_text(g_hand, sx + (sw - w) / 2, sy + 2, "Splash Count", RV_TEXT_WHITE | TEXT_CHALK, y0, y1);
    gfx_chalk(sx + (sw - w) / 2 - 8, sy + 37, sx + (sw + w) / 2 + 8, sy + 35, 3, RV_TEXT_WHITE, y0, y1);
    for (i = 0; i < n && i < rows; i++) {   /* (nine rows: all eight pilots and the hero) */
        int ry = sy + 40 + i * 24;
        unsigned k = g_camp.pilot[order[i]].kills;
        gfx_text(g_hand, sx + 28, ry, g_pilots[order[i]].callsign, RV_TEXT_WHITE | TEXT_CHALK, y0, y1);
        tally_marks(sx + 176, ry + 7, 18, 7, 3, k < 30 ? k : 30, 31 + order[i], y0, y1);
        if (!g_camp.pilot[order[i]].alive)   /* (lost for good: the name and the marks stay up, KIA beside them; the
                                           count itself is the marks: the user, "the tallies are cooler") */
            gfx_text(g_hand, sx + sw - 28 - gfx_text_width(g_hand, "KIA"), ry, "KIA", RV_TEXT_WHITE | TEXT_CHALK, y0, y1);
    }
    gfx_text(FONT_BODY, 14, SCR_H - 18, "click to close", RV_TEXT_GREY | 0x10, y0, y1);
}

static void open_board(void)
{
    vid_fade(0);
    g_hand = gfx_font_chalk(1) ? FONT_CHALK : FONT_TITLE;
    gfx_use_palette(dat_header()->chalk_pal);
    g_scene.full = board_full;
    g_scene.no_room = 1;
    g_mode = MODE_BOARD;
    if (g_hand != FONT_CHALK) {
        static const char __far s_hand[] = "board: no room for the chalk hand: the title font instead";
        mem_short(s_hand);
    }
    RV_NOTE("board: up close%s", g_hand == FONT_CHALK ? "" : " (the title font)");
    vid_dirty(0, SCR_H);
    scene_draw();
    vid_fade(64);
}

static void close_board(void)
{
    g_scene.full = 0;
    g_scene.no_room = 0;
    vid_fade(0);
    gfx_font_chalk(0);
    gfx_use_palette(dat_room(g_scene.room)->palette);
    g_mode = MODE_ROOM;
    vid_dirty(0, SCR_H);
    scene_draw();
    vid_fade(64);
}

static void room_overlay(int y0, int y1)
{
    if (g_scene.room == RV_R_BAR)
        chalkboard(y0, y1);
    talk_overlay(y0, y1);
}

/* the next flight's sets asked of the page (ships_want; it drops what is not listed), and two more for a window on space
 * (or -1) */
static void mission_want(int a, int b)
{
    unsigned char want[RV_MAX_SHIP_SLOT + 4];
    int i, nw = 0, enemy = 0;
    if (a >= 0) {
        want[nw++] = (unsigned char)a;
        want[nw++] = (unsigned char)b;
    }
    want[nw++] = g_camp.player_ship;
    for (i = 0; i < g_m.nship && nw < (int)sizeof want - 1; i++) {
        int type = g_m.ship[i].type;
        if (g_m.ship[i].side == 1)
            enemy = 1;
        want[nw++] = (unsigned char)(type == RV_SHIP_PLAYER || type == RV_SHIP_WINGMAN ? g_camp.player_ship : type);
    }
    if (!enemy)
        want[nw++] = 0x08;              /* (the flight's stand-in enemy type) */
    ships_want(want, nw, 0);
}

/* each room's song: the quarters and the locker, the lounge, the bar (and the rest) */
static int room_song(int room)
{
    return room == RV_R_QUARTERS || room == RV_R_LOCKER ? RV_MUSIC_QUARTERS
         : room == RV_R_LOUNGE ? RV_MUSIC_LOUNGE : room == RV_R_DECK ? RV_MUSIC_LAUNCH : RV_MUSIC_BAR;
}

static void enter_room(int room)
{
    unsigned char seats[3] = { 0xFF, 0xFF, 0xFF };
    g_rooms_shown = 1;
    if (room == RV_R_BAR || room == RV_R_LOUNGE)
        _fmemcpy(seats, g_m.seat[room == RV_R_BAR ? RV_ROOM_BAR : RV_ROOM_LOUNGE], 3);
    {
        int k;                          /* (a pilot the player lost never sits here again: the user, 2026-10-31) */
        for (k = 0; k < 3; k++)
            if (seats[k] < RV_PILOTS && !g_camp.pilot[seats[k]].alive)
                seats[k] = 0xFF;
    }
    logic_boost = 0;                    /* (Kit's was for the mission she said it about) */
    vid_fade(0);
    scene_enter(room, seats);
    {
        const RvRoom __far *r = dat_room(room);   /* (Tally, flown and lost) */
        g_scene.actor_gone = r->has_actor && r->actor.person < RV_PILOTS && !g_camp.pilot[r->actor.person].alive;
    }
    g_scene.overlay = room_overlay;
    g_mode = MODE_ROOM;
    g_talk_line = -1;
    g_talk_at = 0;
    if (dat_room(room)->win_ext != 0xFFFF) {   /* (a window on space: the system's sky, ships going by, or the rocks) */
        mission_want(0x00, 0x02);
        flight_window_open(room, g_camp.node, g_nodes[g_camp.node].outside);
        g_scene.underlay = flight_window_draw;
        g_scene.leave = flight_window_close;
    }
    music_cue(room_song(room), 250);
    scene_draw();
    vid_fade(64);
}

/* the people.csv row of whoever has this role (the XO), or -1 */
static int person_of(int role)
{
    int i;
    for (i = 0; i < (int)g_npilots; i++)
        if (g_pilots[i].role == role)
            return i;
    return -1;
}

/* the mission's lines for the room the player is in, or -1 */
static int talk_room(void)
{
    return g_scene.room == RV_R_BAR ? RV_ROOM_BAR : g_scene.room == RV_R_LOUNGE ? RV_ROOM_LOUNGE
         : g_scene.room == RV_R_DECK ? RV_ROOM_DECK : -1;
}

/* the table talk: this room's lines in turn, as subtitles (round and round; Kit on the deck says hers once) */
static void ambient_talk(void)
{
    int i, room = talk_room();
    if (room < 0 || g_now < g_talk_at)
        return;
    for (i = 1; i <= (int)g_m.ntalk; i++) {
        int k = (g_talk_line + i) % (g_m.ntalk ? g_m.ntalk : 1);
        const RvTalk __far *t;
        if (room == RV_ROOM_DECK && k <= g_talk_line)
            break;
        t = camp_line(&g_m, k);
        if (t->room == room && !(t->flags & RV_TALK_SAY) && camp_line_ok(t)) {
            g_talk_line = k;
            /* a second, then as long as reading it takes at the talk speed (8 letters a second at "slow") */
            g_talk_at = g_now + 18 + (unsigned long)_fstrlen(t->text) * 227 / g_talk_pcts[g_talk_speed];
            vid_dirty(SCR_H - LABEL_H - 46, SCR_H - LABEL_H);
            return;
        }
    }
    if (room == RV_ROOM_DECK && g_talk_line >= 0) {      /* (said: the last line off, and quiet until the launch) */
        g_talk_line = -1;
        vid_dirty(SCR_H - LABEL_H - 46, SCR_H - LABEL_H);
    }
    if (room == RV_ROOM_DECK)
        g_talk_at = 0xFFFFFFFFUL;
}

/* --- close-ups: a person speaking --- */

#define FACE_X RV_FACE_X
#define FACE_Y RV_FACE_Y

static int g_close_room, g_close_back;      /* the close-up's room, and the mode it goes back to */

/* the text a frame shows of the speech, copied out of g_said: said_so_far's, brief_overlay's (each drawn as soon
 * as copied, so the two share it) */
static char g_shown[RV_TALK + 48];

/* the speech so far; of a long one (a briefing), only the sentence being said */
static const char *said_so_far(void)
{
    int n = _fstrlen(g_said), k = speech_done() ? n : (int)((long)n * speaking_step() / (g_nlip ? g_nlip : 1)), from = 0;
    if (n > 240)
        for (from = k - 1; from > 0; from--)
            if (g_said[from] == ' ' && (g_said[from - 1] == '.' || g_said[from - 1] == '!' || g_said[from - 1] == '?')) {
                from++;
                break;
            }
    if (from < 0 || from > k)
        from = 0;
    _fstrncpy(g_shown, g_said + from, k - from);
    g_shown[k - from] = 0;
    return g_shown;
}

/* a close-up: the speaker's face over the room, blurred (the XO's too: her
 * gesture sheet twitched at this size; the user's call, 2026-10-21) */
static int eyes_now(void)
{
    return (g_now / 4) % 17 == 0 ? 2 : (g_now / 4) % 17 == 1 ? 1 : 0;   /* a blink every 4 s */
}

/* --- home: Kit in front of the armour plate as a fighter comes back (the user, 2026-11-01), the plate showing this flight's damage: a big
 * stamp a system hurt, a small one for each ten of the hull's hundred gone (for each quarter of the armour while the hull is whole), each
 * placed by a seed of what it stands for, so the same damage looks the same and other damage looks other.  At most six of each (the user:
 * "cap it after like 4 or 6").  The picture is put together once and kept in XMS, so that her mouth moving costs only the copy --- */
#define HOME_BIG_MOST 6
#define HOME_SMALL_MOST 6
#define HOME_WRECKED_BIG 4              /* a ship this far gone shows holes whichever systems they were */
#define PLATE_EXTRA 100                 /* the plate rolls into place behind her (the user: "slide into place", "about 100 pixels"): this much */
#define PLATE_W (SCR_W + PLATE_EXTRA)   /* of it, at its left, is only seen on the way */
#define PLATE_SLIDE 300                 /* clock240's: a second and a quarter, slowing to a stop */
#define PLATE_MOVES_Y 322               /* the rows it shows in: under them it has gone into the dark */
static XmsBlock g_plate;                /* (resident: taken once, when the game starts) */
static int g_plate_pass, g_plate_off;   /* the plate's left part, then the rest; how far from its place it is */
static unsigned long g_plate_t0;
static unsigned short __far g_stamp[HOME_BIG_MOST + HOME_SMALL_MOST];
static int g_nstamps;
static int g_plate_on;                  /* a close-up's backdrop is the plate */
static int g_home_tier;                 /* the state the fighter came home in */

void game_reserve(void)
{
    g_plate = xms_alloc((unsigned long)PLATE_W * SCR_H, XMS_RESIDENT);
}

static void home_stamp(unsigned first, unsigned count, unsigned seed, unsigned long *used)
{
    unsigned long x = (seed + 1UL) * 2654435761UL;
    unsigned i, tried;
    x ^= x >> 15;
    i = (unsigned)(x >> 9) % count;
    for (tried = 0; tried < count && (*used >> i & 1); tried++)
        i = (i + 1) % count;
    if (tried < count && g_nstamps < (int)RV_COUNT(g_stamp)) {
        *used |= 1UL << i;
        g_stamp[g_nstamps++] = (unsigned short)(first + i);
    }
}

/* straight after the flight, while its damage is still there */
static void home_note(void)
{
    const RvDatHeader __far *h = dat_header();
    unsigned long used = 0;
    int tens = damage_hull_tens(), n = tens ? tens : damage_armour_quarters(), k, big = 0;
    g_home_tier = damage_home_tier();
    g_nstamps = 0;
    for (k = 0; k < n && k < HOME_SMALL_MOST; k++)
        home_stamp(h->plate_small, h->plate_nsmall, (tens ? 100 : 200) + k, &used);
    used = 0;
    for (k = 0; k < SYS_COUNT && big < HOME_BIG_MOST; k++)
        if (damage_part_hurt((int16_t)k)) {
            home_stamp(h->plate_big, h->plate_nbig, k, &used);
            big++;
        }
    for (k = 0; g_home_tier == HOME_WRECKED && big < HOME_WRECKED_BIG; k++, big++)
        home_stamp(h->plate_big, h->plate_nbig, 300 + k, &used);
}

/* the picture in the plate's block, a row PLATE_W wide: a band is only a screen wide, so twice, the left part and then the rest */
static void plate_compose(int y0, int y1)
{
    int i, r, dx = g_plate_pass ? 0 : PLATE_EXTRA;
    gfx_image(dat_header()->plate_back, dx - PLATE_EXTRA, 0, y0, y1);
    for (i = 0; i < g_nstamps; i++)
        gfx_sprite(g_stamp[i], dx, 0, y0, y1);
    if (g_plate)
        for (r = y0; r < y1; r++)
            xms_put(g_plate, (unsigned long)r * PLATE_W + (g_plate_pass ? PLATE_EXTRA : 0), vid_row(r), g_plate_pass ? SCR_W : PLATE_EXTRA);
}

/* where it has got to; a new place, its rows to draw again */
static void plate_tick(void)
{
    unsigned long t = clock240() - g_plate_t0;
    int off = 0;
    if (g_plate && t < PLATE_SLIDE) {
        long left = PLATE_SLIDE - (long)t;
        off = (int)(PLATE_EXTRA * left / PLATE_SLIDE * left / PLATE_SLIDE) & ~1;
    }
    if (off != g_plate_off) {
        g_plate_off = off;
        vid_dirty(0, PLATE_MOVES_Y);
    }
}

static void talk_full(int y0, int y1)
{
    const RvRoom __far *r = dat_room(g_close_room);
    const RvPerson __far *p = dat_person(g_speaker);
    int mouth = mouth_now(), eyes = eyes_now();
    char title[40];
    if (!g_plate_on)
        gfx_image(r->closeup_back, 0, 0, y0, y1);
    else if (g_plate) {
        int row;
        for (row = y0; row < y1; row++)
            xms_get(g_plate, (unsigned long)row * PLATE_W + PLATE_EXTRA - g_plate_off, vid_row(row), SCR_W);
    } else
        plate_compose(y0, y1);
    gfx_sprite(p->face, FACE_X, FACE_Y, y0, y1);
    gfx_image(p->eyes0 + eyes, FACE_X + dat_asset(p->eyes0)->x, FACE_Y + dat_asset(p->eyes0)->y, y0, y1);
    gfx_image(p->mouth0 + mouth, FACE_X + dat_asset(p->mouth0)->x, FACE_Y + dat_asset(p->mouth0)->y, y0, y1);
    if (g_pilots[g_speaker].role == RV_ROLE_XO) {        /* (no callsign: her name and rank) */
        int w = gfx_text_width(FONT_TITLE, g_pilots[g_speaker].name);
        gfx_text(FONT_TITLE, w > SCR_W - 412 ? SCR_W - 12 - w : 400, 130, g_pilots[g_speaker].name, RV_TEXT_AMBER, y0, y1);
        gfx_text(FONT_BODY, 400, 160, "Executive Officer", RV_TEXT_WHITE, y0, y1);
    } else {
        sprintf(title, "\"%.10s\"", g_pilots[g_speaker].callsign);
        gfx_text(FONT_TITLE, 400, 130, title, RV_TEXT_AMBER, y0, y1);
        gfx_text(FONT_BODY, 400, 160, g_pilots[g_speaker].name, RV_TEXT_WHITE, y0, y1);
    }
    gfx_wrap(FONT_BODY, 400, 196, 220, said_so_far(), RV_TEXT_WHITE, 8, y0, y1);
}

/* what moves in a close-up: the mouth, the eyes, the words */
static void face_tick(void)
{
    static int said = -1;
    int step = speech_done() ? g_nlip : speaking_step();
    if (mouth_now() != g_mouth || eyes_now() != g_eyes || step != said) {
        g_mouth = mouth_now();
        g_eyes = eyes_now();
        said = step;
        vid_dirty(FACE_Y, SCR_H);
    }
}

/* the close-up of whoever is speaking (g_speaker), in this room */
static int show_face(void)
{
    const RvRoom __far *r = dat_room(g_scene.room);
    if (r->closeup_part == 0xFFFF || g_speaker < 0)
        return 0;
    g_close_room = g_scene.room;
    g_close_back = g_mode;
    vid_fade(0);
    gfx_use_part(r->closeup_part, 1);
    gfx_use_part(dat_person(g_speaker)->palpart, 0);
    g_scene.full = talk_full;
    g_scene.no_room = 1;
    vid_dirty(0, SCR_H);
    scene_draw();
    vid_fade(64);
    return 1;
}

static void open_talk(int person, const char __far *line)
{
    if (dat_room(g_scene.room)->closeup_part == 0xFFFF)
        return;
    say(person, line);
    if (!show_face())
        return;
    g_mode = MODE_TALK;
    music_cue(room_song(g_close_room), 150);     /* (the room's own: the same song goes on) */
}

static void close_talk(void)
{
    g_scene.full = 0;
    g_scene.no_room = 0;
    vid_fade(0);
    gfx_use_palette(dat_room(g_scene.room)->palette);
    g_mode = g_close_back;
    vid_dirty(0, SCR_H);
    scene_draw();
    vid_fade(64);
}

/* a pilot's line: theirs in this room's talk if they have one, else their own */
static const char __far *line_of(int person)
{
    int i;
    for (i = 0; i < (int)g_m.ntalk; i++) {
        const RvTalk __far *t = camp_line(&g_m, i);
        if (t->speaker == person && t->room < RV_ROOM_RADIO && camp_line_ok(t))
            return t->text;
    }
    return g_pilots[person].line;
}

/* --- a conversation: clicking a pilot plays their `say` lines in this room, a page a click, each in its speaker's face
 * (the player's own when it is a reply); Esc leaves.  None for them: their one line --- */

static int g_conv_owner = -1, g_conv_at;
static int g_clip_frame, g_deck_ship;   /* (the deck, below: Kit, her eyes open or shut, and the fighter on the pad) */

static int conv_next(void)
{
    int i, room = talk_room();
    for (i = g_conv_at + 1; i < (int)g_m.ntalk; i++) {
        const RvTalk __far *t = camp_line(&g_m, i);
        if ((t->flags & RV_TALK_SAY) && t->room == room && t->listener == g_conv_owner && camp_line_ok(t))
            return i;
    }
    return -1;
}

static void conv_page(int k)
{
    const RvTalk __far *t = camp_line(&g_m, k);
    int who = t->speaker == RV_SPEAKER_YOU ? person_of(RV_ROLE_HERO) : t->speaker, back = g_close_back, was = g_mode;
    g_conv_at = k;
    if (t->boost)                       /* (Kit's line that comes with a part) */
        logic_boost = t->boost;
    say(who, t->text);
    if (was == MODE_TALK) {             /* (the next page: the same room, this speaker's face) */
        show_face();
        g_close_back = back;
    } else {
        if (!show_face())
            return;
        g_mode = MODE_TALK;
        music_cue(room_song(g_close_room), 150);
    }
}

static void open_conv(int person)
{
    int k;
    if (dat_room(g_scene.room)->closeup_part == 0xFFFF)
        return;
    g_conv_owner = person;
    g_conv_at = -1;
    k = conv_next();
    if (k < 0) {
        g_conv_owner = -1;
        open_talk(person, line_of(person));
        return;
    }
    conv_page(k);
}

/* --- the briefing and the debrief: the XO at the lectern --- */

static int g_stage;
static unsigned long g_stage_at;

static void situation_display(int y0, int y1)
{
    const RvRoom __far *r = dat_room(RV_R_BRIEFING);
    const RvBoard __far *b = 0;
    int i, x0, z0, x1, z1, mx, my, mw, mh;
    long sx, sz;
    for (i = 0; i < 4; i++)
        if (r->board[i].kind == 2 && r->board[i].w)
            b = &r->board[i];
    if (!b || y1 <= b->y || y0 >= b->y + b->h)
        return;
    gfx_fill(b->x, b->y, b->w, b->h, UI_PANEL, y0, y1);
    gfx_frame(b->x, b->y, b->w, b->h, UI_HILITE, y0, y1);
    /* fit the carrier (0, 0), the nav points and the fields */
    x0 = x1 = z0 = z1 = 0;
    for (i = 0; i < g_m.nnav; i++) {
        if (g_m.nav[i].x < x0) x0 = g_m.nav[i].x;
        if (g_m.nav[i].x > x1) x1 = g_m.nav[i].x;
        if (g_m.nav[i].z < z0) z0 = g_m.nav[i].z;
        if (g_m.nav[i].z > z1) z1 = g_m.nav[i].z;
    }
    for (i = 0; i < g_m.nfield; i++) {
        const RvField __far *f = &g_m.field[i];
        if (f->x - f->radius < x0) x0 = f->x - f->radius;
        if (f->x + f->radius > x1) x1 = f->x + f->radius;
        if (f->z - f->radius < z0) z0 = f->z - f->radius;
        if (f->z + f->radius > z1) z1 = f->z + f->radius;
    }
    mx = b->x + 14; my = b->y + 14; mw = b->w - 28; mh = b->h - 28;
    sx = x1 - x0 + 1; sz = z1 - z0 + 1;
    if (sx * mh < sz * mw)
        sx = sz * mw / mh;
    else
        sz = sx * mh / mw;
#define MAPX(v) (mx + (int)(((long)(v) - x0) * mw / sx))
#define MAPY(v) (my + mh - (int)(((long)(v) - z0) * mh / sz))
#define MAPR(v) ((int)((long)(v) * mw / sx))
    for (i = 1; i < 4; i++) {
        gfx_line(b->x + 1, b->y + i * b->h / 4, b->x + b->w - 2, b->y + i * b->h / 4, UI_PANEL_EDGE, 0, y0, y1);
        gfx_line(b->x + i * b->w / 4, b->y + 1, b->x + i * b->w / 4, b->y + b->h - 2, UI_PANEL_EDGE, 0, y0, y1);
    }
    for (i = 0; i < g_m.nfield; i++) {
        const RvField __far *f = &g_m.field[i];
        int c = f->type == RV_FIELD_MINE ? UI_RED : UI_GREY4, rr = MAPR(f->radius);
        gfx_circle(MAPX(f->x), MAPY(f->z), rr < 3 ? 3 : rr, c, 1, y0, y1);
        gfx_text(FONT_BODY, MAPX(f->x) - 8, MAPY(f->z) - 6, f->type == RV_FIELD_MINE ? "MINES" : "DEBRIS",
                 RV_TEXT_GREY, y0, y1);
    }
    {
        int px = MAPX(0), py = MAPY(0);
        for (i = 0; i <= g_m.nnav; i++) {
            int nx, ny;
            if (i < g_m.nnav && (g_m.nav[i].flags & 1))
                continue;               /* (a hidden trigger point: the player's map has no word of it) */
            nx = i < g_m.nnav ? MAPX(g_m.nav[i].x) : MAPX(0);
            ny = i < g_m.nnav ? MAPY(g_m.nav[i].z) : MAPY(0);
            gfx_line(px, py, nx, ny, UI_HILITE, 3, y0, y1);
            px = nx; py = ny;
        }
    }
    gfx_line(MAPX(0) - 4, MAPY(0) + 3, MAPX(0), MAPY(0) - 4, UI_WHITE, 0, y0, y1);
    gfx_line(MAPX(0), MAPY(0) - 4, MAPX(0) + 4, MAPY(0) + 3, UI_WHITE, 0, y0, y1);
    gfx_line(MAPX(0) - 4, MAPY(0) + 3, MAPX(0) + 4, MAPY(0) + 3, UI_WHITE, 0, y0, y1);
    for (i = 0; i < g_m.nnav; i++) {
        char s[8];
        int k, foes = 0;
        if (g_m.nav[i].flags & 1)
            continue;
        for (k = 0; k < g_m.nship; k++)
            if (g_m.ship[k].nav == g_m.nav[i].id && g_m.ship[k].side == 1)
                foes += g_m.ship[k].count;
        gfx_circle(MAPX(g_m.nav[i].x), MAPY(g_m.nav[i].z), 4, UI_AMBER4, 0, y0, y1);
        sprintf(s, "NAV %u", g_m.nav[i].id);
        gfx_text(FONT_BODY, MAPX(g_m.nav[i].x) + 7, MAPY(g_m.nav[i].z) - 7, s, RV_TEXT_AMBER, y0, y1);
        for (k = 0; k < foes && k < 6; k++)
            gfx_fill(MAPX(g_m.nav[i].x) + 7 + k * 5, MAPY(g_m.nav[i].z) + 8, 3, 3, UI_RED, y0, y1);
    }
#undef MAPX
#undef MAPY
#undef MAPR
}

/* the briefing over, the screen is switched off: dimmed under the pilots
 * as they rise, who stand in front of it (the user's call, 2026-10-23) */
static void screen_off(int y0, int y1)
{
    const RvRoom __far *r = dat_room(RV_R_BRIEFING);
    int i;
    for (i = 0; i < 4; i++)
        if (r->board[i].kind == 2 && r->board[i].w)
            gfx_mix_to_black(r->board[i].x, r->board[i].y, r->board[i].w, r->board[i].h, y0, y1);
}

static void xo_underlay(int y0, int y1)
{
    const RvSpeaker __far *s = &dat_room(RV_R_BRIEFING)->speaker;
    int mouth = s->mouth_of_viseme[mouth_now()];
    if (g_mode == MODE_BRIEF && g_stage > 0)
        screen_off(y0, y1);
    gfx_sprite(s->cell0 + g_gesture * s->nmouths + mouth, 0, 0, y0, y1);
    if (g_stage > 0 && s->stage[g_stage - 1] != 0xFFFF)
        gfx_sprite(s->stage[g_stage - 1], 0, 0, y0, y1);
}

static void brief_overlay(int y0, int y1)
{
    char head[80];
    int n = _fstrlen(g_said), k = speech_done() ? n : (int)((long)n * speaking_step() / (g_nlip ? g_nlip : 1));
    if (g_mode == MODE_BRIEF && g_stage == 0)
        situation_display(y0, y1);
    sprintf(head, "%s  //  %.40Fs", g_mode == MODE_BRIEF ? "BRIEFING" : g_won ? "DEBRIEF: WON" : "DEBRIEF: LOST",
            (char __far *)g_m.title);
    gfx_text(FONT_TITLE, 14, 6, head, RV_TEXT_AMBER | 0x10, y0, y1);
    if (y1 > 290) {
        gfx_shade(0, 292, SCR_W, 86, y0, y1);
        _fstrncpy(g_shown, g_said, k);
        g_shown[k] = 0;
        gfx_wrap(FONT_BODY, 14, 298, SCR_W - 28, g_shown, RV_TEXT_WHITE, 3, y0, y1);
    }
}

/* the briefing and the debrief are lines (tools/rvdata.py: briefing, win, lose), in turn, as in a conversation: the
 * XO's at the lectern, anyone else's in their own close-up.  A line waits a second after it has been said, or for a
 * click; Esc leaves the rest.  With the wingman dead, " and NAME" is cut from "You and NAME ..." (rvdata holds every
 * briefing to that), so the mission is flown solo.  A debrief with the wingman lost in the flight says so first, and
 * that is where the loss becomes final (flying a mission again after dying undoes it, launch) */
static char __far g_xo_words[RV_TALK + 48];
static int g_lost_wing = -1;
static int g_line_room, g_line_at, g_lines_left, g_line_face;

static const char __far *line_words(const RvTalk __far *t)
{
    const char __far *from = t->text;
    char name[24];
    int i, n = 0;
    if (t->room != RV_ROOM_BRIEF)
        return from;
    for (i = 0; i < (int)g_m.nship; i++)
        if (g_m.ship[i].type == RV_SHIP_WINGMAN && g_m.ship[i].pilot < RV_PILOTS && !g_camp.pilot[g_m.ship[i].pilot].alive) {
            int cut;
            sprintf(name, " and %.10s", g_pilots[g_m.ship[i].pilot].callsign);
            cut = (int)strlen(name);
            while (from[n] && _fstrncmp(from + n, name, cut) != 0)
                n++;
            if (!from[n])
                return from;
            _fmemcpy(g_xo_words, from, n);
            _fstrcpy(g_xo_words + n, from + n + cut);
            return g_xo_words;
        }
    return from;
}

static int line_next(void)
{
    int i;
    for (i = g_line_at + 1; i < (int)g_m.ntalk; i++) {
        const RvTalk __far *t = camp_line(&g_m, i);
        if (t->room == g_line_room && camp_line_ok(t))
            return i;
    }
    return -1;
}

static void line_face_off(void)
{
    if (g_line_face) {
        g_line_face = 0;
        close_talk();
    }
}

static void line_page(int k)
{
    const RvTalk __far *t = camp_line(&g_m, k);
    int who = t->speaker == RV_SPEAKER_YOU ? person_of(RV_ROLE_HERO) : t->speaker;
    g_line_at = k;
    g_stage_at = 0;
    if (g_pilots[who].role == RV_ROLE_XO) {
        line_face_off();
        say(who, line_words(t));
        vid_dirty(292, 378);
    } else {
        say(who, t->text);
        g_line_face = show_face();
    }
}

/* Kit at the plate, saying something about it; 0 when she cannot (she flew, and the player lost her).  Not the same line twice running */
static int enter_home(void)
{
    static signed char __far last[4] = { -1, -1, -1, -1 };
    const RvDatHeader __far *h = dat_header();
    int kit = person_of(RV_ROLE_TECH), n, k;
    if (kit < 0 || !g_camp.pilot[kit].alive || g_lost_wing == kit)
        return 0;
    n = camp_home_count((int16_t)g_home_tier);
    k = (int)((ticks() * 25173UL >> 4) % n);
    if (k == last[g_home_tier])
        k = (k + 1) % n;
    last[g_home_tier] = (signed char)k;
    vid_fade(0);
    scene_enter(RV_R_BRIEFING, 0);
    gfx_use_part(h->plate_part, 1);
    gfx_use_part(dat_person(kit)->palpart, 0);
    g_close_room = RV_R_BRIEFING;
    g_scene.no_room = 1;
    g_scene.hide_pointer = 1;           /* (the plate put together, out of sight) */
    g_scene.full = plate_compose;
    for (g_plate_pass = 0; g_plate_pass < 2; g_plate_pass++) {
        vid_dirty(0, SCR_H);
        scene_draw();
    }
    g_scene.hide_pointer = 0;
    g_plate_on = 1;
    g_plate_off = g_plate ? PLATE_EXTRA : 0;
    say(kit, camp_home_line((int16_t)g_home_tier, (int16_t)k));
    g_scene.full = talk_full;
    g_stage_at = 0;
    g_mode = MODE_HOME;
    music_cue(g_won ? RV_MUSIC_WIN : RV_MUSIC_LOSE, 250);   /* (the debrief's: it carries on under what follows) */
    vid_dirty(0, SCR_H);
    scene_draw();
    vid_fade(64);
    g_plate_t0 = clock240();
    return 1;
}

static void start_briefing(int debrief)
{
    if (!debrief)
        flight_prepare();               /* (a guest's mission is the host's from here: its briefing, its sets) */
    vid_fade(0);
    g_plate_on = 0;
    scene_enter(RV_R_BRIEFING, 0);
    g_scene.underlay = xo_underlay;
    g_scene.overlay = brief_overlay;
    g_mode = debrief ? MODE_DEBRIEF : MODE_BRIEF;
    g_stage = 0;
    if (!debrief)                       /* the flight's sets asked of the page now, fetched while the briefing is read */
        mission_want(-1, 0);
    g_line_room = !debrief ? RV_ROOM_BRIEF : g_won ? RV_ROOM_WIN : RV_ROOM_LOSE;
    if (debrief)
        camp_way_out(g_won);
    g_line_at = -1;
    g_lines_left = 1;
    g_line_face = 0;
    g_stage_at = 0;
    if (debrief && g_lost_wing >= 0) {
        char name[24];
        sprintf(name, "We lost %.10s.", g_pilots[g_lost_wing].callsign);
        _fstrcpy(g_xo_words, name);
        g_camp.pilot[g_lost_wing].alive = 0;
        RV_LOG("campaign: pilot %d lost", g_lost_wing);
        g_lost_wing = -1;
        say(person_of(RV_ROLE_XO), g_xo_words);
    } else {
        int k = line_next();
        if (k >= 0)
            line_page(k);
        else {
            g_xo_words[0] = 0;
            say(person_of(RV_ROLE_XO), g_xo_words);
        }
    }
    if (!debrief)
        briefingStarted();
    music_cue(debrief ? (g_won ? RV_MUSIC_WIN : RV_MUSIC_LOSE) : RV_MUSIC_BRIEFING, 250);
    scene_draw();
    vid_fade(64);
}

/* the gesture changes with each sentence: hands, then open and raised in turn */
static void brief_tick(void)
{
    int step = speaking_step(), chars, i, sentences = 0, g;
    int n = _fstrlen(g_said);
    chars = speech_done() ? n : (int)((long)n * step / (g_nlip ? g_nlip : 1));
    for (i = 0; i < chars; i++)
        if (g_said[i] == '.' || g_said[i] == '!' || g_said[i] == '?')
            sentences++;
    g = sentences == 0 ? 0 : 1 + (sentences - 1) % 2;
    if (speech_done())
        g = 0;
    if (g != g_gesture || mouth_now() != g_mouth) {
        const RvSpeaker __far *s = &dat_room(RV_R_BRIEFING)->speaker;
        const RvAsset __far *a = dat_asset(s->cell0);
        g_gesture = g;
        g_mouth = mouth_now();
        vid_dirty(a->y, a->y + a->h);
    }
    if (chars != g_shown_chars) {
        g_shown_chars = chars;
        vid_dirty(292, 378);
    }
}

/* --- the deck: Kit and the fighter --- */

static unsigned long g_clip_at;

static void deck_underlay(int y0, int y1)
{
    const RvDeck __far *d = &dat_room(RV_R_DECK)->deck;
    gfx_image(d->ship[g_deck_ship].plate, 0, 0, y0, y1);
    if (g_scene.hot_gone)               /* (Kit flew, and the player lost her: the fighter, and nobody) */
        return;
    gfx_sprite(d->front, 0, 0, y0, y1);
    gfx_sprite(d->clip_first[0] + g_clip_frame, 0, 0, y0, y1);
}

static void deck_overlay(int y0, int y1)
{
    char s[108];                        /* (a 40-character title, the words, a name and what Kit did) */
    static const char *const s_boost[] = { "", " + armor", " + fuel", " + missile" };
    sprintf(s, "%.40Fs  //  your fighter: %s%s", (char __far *)g_m.title, ship_name(camp_flight_fighter()), s_boost[logic_boost & 3]);
    gfx_text(FONT_BODY, 14, 8, s, RV_TEXT_AMBER | 0x10, y0, y1);
    talk_overlay(y0, y1);
}

static void enter_deck(void)
{
    const RvDeck __far *d = &dat_room(RV_R_DECK)->deck;
    int i;
    g_rooms_shown = 1;
    vid_fade(0);
    scene_enter(RV_R_DECK, 0);
    g_deck_ship = 0;
    for (i = 0; i < d->nships; i++)
        if (d->ship[i].ship_type == camp_flight_fighter())
            g_deck_ship = i;
    g_scene.underlay = deck_underlay;
    g_scene.overlay = deck_overlay;
    i = person_of(RV_ROLE_TECH);
    if (i >= 0 && !g_camp.pilot[i].alive)
        for (i = 0; i < (int)dat_room(RV_R_DECK)->nhot; i++)
            if (dat_room(RV_R_DECK)->hot[i].action == RV_ACT_KIT)
                g_scene.hot_gone = i + 1;
    g_talk_line = -1;
    g_talk_at = g_now + 18;
    g_clip_frame = 0;
    g_clip_at = g_now;
    g_mode = MODE_DECK;
    music_cue(RV_MUSIC_LAUNCH, 250);
    scene_draw();
    vid_fade(64);
}

/* Kit stands still and blinks (the user, 2026-11-01: her breathing "looks zombie-like"): shut three ticks of 55 ms, then 2.5 to 7 s open */
static void deck_tick(void)
{
    const RvDeck __far *d = &dat_room(RV_R_DECK)->deck;
    if (g_now < g_clip_at)
        return;
    g_clip_frame = !g_clip_frame;
    g_clip_at = g_now + (g_clip_frame ? 3 : 45 + (g_now * 25173UL >> 4) % 82);
    vid_dirty(d->patch_y, d->patch_y + d->patch_h);
}

/* --- the flight --- */

/* --- the player's death: a word, and the choice of flying the mission again or carrying on with it lost (the
 * user, 2026-10-30: not straight back to the base with the mission failed) --- */

/* in a shared flight the host's R or C is everyone's (restartChoice): said under the choice */
static const char *shared_choice_line(void)
{
    return rvNetRole == 2 ? "The host chooses for everyone." : rvNetRole == 1 ? "Your choice is everyone's." : 0;
}

static void dead_full(int y0, int y1)
{
    const char *who = shared_choice_line();
    gfx_fill(0, 0, SCR_W, SCR_H, UI_BLACK, y0, y1);
    gfx_text(FONT_TITLE, 200, 140, "YOU HAVE DIED", RV_TEXT_WHITE, y0, y1);
    gfx_text(FONT_BODY, 150, 200, "R   fly the mission again", RV_TEXT_AMBER, y0, y1);
    gfx_text(FONT_BODY, 150, 222, "C   carry on: the mission is lost", RV_TEXT_GREY, y0, y1);
    if (who)
        gfx_text(FONT_BODY, 150, 254, who, RV_TEXT_GREY, y0, y1);
}

static void enter_dead(void)
{
    vid_fade(0);
    g_scene.full = dead_full;
    g_scene.no_room = 1;
    g_scene.overlay = 0;
    g_scene.label[0] = 0;
    g_mode = MODE_DEAD;
    music_cue(RV_MUSIC_LOSE, 300);
    vid_dirty(0, SCR_H);
    scene_draw();
    vid_fade(64);
}

static int g_lost;                      /* the dead screen is the carrier's */

static void leave_dead(void)
{
    g_scene.full = 0;
    g_scene.no_room = 0;
    g_scene.overlay = 0;
    g_scene.hide_pointer = 0;
    g_lost = 0;
}

/* --- the carrier lost (the user, 2026-10-31: "a haunting losing screen but just like you blew up"): the flight ends as
 * at the player's death, with the same choice, on the crew's lounge with nobody in it, gone cold: the room's colours
 * brought down from black to a dim blue, but for the game's greys and white, left whole for the words, which come one
 * at a time (the room's art uses the game's amber and blue entries too: they go down with the rest) --- */

#define LOST_FADE_TICKS 90              /* the room coming up: 5 s of ticks (18.2 a second) */
#define LOST_WAIT_TICKS 54              /* no key takes it away for 3 s: a finger on the trigger is still there */
static const unsigned long k_lost_line_at[3] = { 36, 90, 144 };   /* when each block of words comes: 2, 5, 8 s */
#define LOST_KEEP 8                     /* UI_BLACK .. UI_WHITE stay */
static unsigned char __far g_cold[(256 - LOST_KEEP) * 3];
static int g_lost_stage;                /* how many blocks of words show */
static int g_lost_level;
static unsigned long g_lost_t0;

static void lost_centred(int font, int y, const char *s, int colour, int y0, int y1)
{
    gfx_text(font, (SCR_W - gfx_text_width(font, s)) / 2, y, s, colour, y0, y1);
}

static void lost_overlay(int y0, int y1)
{
    if (g_lost_stage >= 1)
        lost_centred(FONT_TITLE, 26, "THE VIGILANCE IS GONE", RV_TEXT_WHITE, y0, y1);
    if (g_lost_stage >= 2) {
        lost_centred(FONT_BODY, 62, "Every bunk aboard her is empty.", RV_TEXT_GREY, y0, y1);
        lost_centred(FONT_BODY, 80, "No one is left to call you home.", RV_TEXT_GREY, y0, y1);
    }
    if (g_lost_stage >= 3) {
        const char *who = shared_choice_line();
        gfx_text(FONT_BODY, 150, 348, "R   fly the mission again", RV_TEXT_WHITE, y0, y1);
        gfx_text(FONT_BODY, 150, 370, "C   carry on: the mission is lost", RV_TEXT_GREY, y0, y1);
        if (who)
            gfx_text(FONT_BODY, 150, 392, who, RV_TEXT_GREY, y0, y1);
    }
}

static void enter_lost(void)
{
    const unsigned char *pal;
    int i;
    vid_fade(0);
    scene_enter(RV_R_LOUNGE, 0);        /* (nobody in the seats) */
    pal = shot_palette();
    for (i = 0; i < 256 - LOST_KEEP; i++) {   /* (the room's colours, drained and cold) */
        const unsigned char *c = pal + (i + LOST_KEEP) * 3;
        unsigned grey = (c[0] * 3U + c[1] * 6U + c[2]) / 10;
        g_cold[i * 3] = (unsigned char)((grey * 4U + c[0]) / 20);
        g_cold[i * 3 + 1] = (unsigned char)((grey * 5U + c[1]) / 20);
        g_cold[i * 3 + 2] = (unsigned char)((grey * 7U + c[2]) / 20);
    }
    vid_palette(g_cold, LOST_KEEP, 256 - LOST_KEEP);
    g_scene.no_room = 1;
    g_scene.hide_pointer = 1;
    g_scene.overlay = lost_overlay;
    g_scene.label[0] = 0;
    g_lost = 1;
    g_lost_stage = 0;
    g_lost_level = 0;
    g_lost_t0 = ticks();
    g_mode = MODE_DEAD;
    music_cue(RV_MUSIC_LOST, 1500);
    vid_dirty(0, SCR_H);
    scene_draw();
}

/* once a loop on the screen: the room coming up, the words coming */
static void lost_tick(void)
{
    unsigned long t = g_now - g_lost_t0;
    int level = t >= LOST_FADE_TICKS ? 64 : (int)(t * 64 / LOST_FADE_TICKS);
    if (level != g_lost_level) {
        g_lost_level = level;
        vid_fade(level);
    }
    while (g_lost_stage < 3 && t >= k_lost_line_at[g_lost_stage]) {
        g_lost_stage++;
        vid_dirty(0, SCR_H);
    }
}

/* --- the test rig (RV.EXE itself, so that what is tested is the game; tests/monkey.sh): ---
 *   /noflight       a launch does not fly: straight to its debrief, every room and screen and the campaign's way as ever
 *   /route WLW...   each mission's result in turn (then /winrate's or the roll)
 *   /kill P         with /noflight, P% a mission: its wingman lost, an escort lost; a quarter of that the player dead, the
 *                   carrier lost, so that the lines and the screens for them come up
 *   /monkey SEED N  no player: clicks and keys at random, known spots as often as not; after N actions a mission it heads
 *                   for the launch.  The seed is the campaign's too, so a run comes out the same (native build: the same
 *                   to the frame; DOSBox's clock has its own ideas) */
extern int g_can_quit;
long g_freeze_test;
int g_noflight, g_kill_pct;
const char *g_route;
static unsigned g_route_at;
static unsigned long __far g_mk;
static unsigned __far g_mk_budget = 200, g_mk_acts, g_mk_wait;
static unsigned long __far g_mk_total;

void monkey_start(unsigned long seed, unsigned budget)
{
    g_mk = seed ? seed : 1;
    if (budget)
        g_mk_budget = budget;
    g_can_quit = 0;
}

int monkey_on(void) { return g_mk != 0; }

static unsigned mk_r(unsigned n)
{
    g_mk ^= g_mk << 13;
    g_mk ^= g_mk >> 17;
    g_mk ^= g_mk << 5;
    return (unsigned)((g_mk >> 7) % n);
}

/* the way to the launch from a room: the hot spot that goes there, -1 none */
static int monkey_way(const RvRoom __far *r)
{
    static const unsigned char s_order[] = { RV_R_BRIEFING, RV_R_BAR, RV_R_LOUNGE, RV_R_QUARTERS };
    int i, k;
    for (i = 0; i < r->nhot; i++)
        if (r->hot[i].action == RV_ACT_LAUNCH)
            return i;
    for (k = 0; k < (int)sizeof s_order; k++)
        for (i = 0; i < r->nhot; i++)
            if (r->hot[i].action == RV_ACT_GOTO && r->hot[i].arg == s_order[k] && s_order[k] != g_scene.room)
                return i;
    return -1;
}

void monkey_step(Input *in, int *x, int *y)
{
    static const char s_chars[] = "sltrcqSLTRCQ abzAB123";
    unsigned k;
    if (g_mk_wait) {
        g_mk_wait--;
        return;
    }
    g_mk_wait = mk_r(10);
    g_mk_acts++;
    g_mk_total++;
    if (g_mode == MODE_ROOM || g_mode == MODE_DECK) {
        const RvRoom __far *r = dat_room(g_scene.room);
        int want = -1;
        if (g_mk_acts > g_mk_budget || mk_r(100) < 3)
            want = monkey_way(r);
        else if (r->nhot && mk_r(100) < 45)
            want = (int)mk_r(r->nhot);
        if (want >= 0) {
            /* (anywhere in it: a seat can lie over its middle, and the seat is what a click there takes) */
            *x = r->hot[want].x + (int)mk_r(r->hot[want].w ? r->hot[want].w : 1);
            *y = r->hot[want].y + (int)mk_r(r->hot[want].h ? r->hot[want].h : 1);
            in->moved = in->clicked = 1;
            return;
        }
    }
    k = mk_r(100);
    if (k < 55) {                       /* (anywhere: a seat, a menu's row, a button, nothing) */
        *x = (int)mk_r(SCR_W);
        *y = (int)mk_r(SCR_H);
        in->moved = 1;
        in->clicked = mk_r(5) != 0;
    } else if (k < 75) {
        switch (mk_r(7)) {
        case 0: in->key = KEY_ESC; break;
        case 1: in->key = KEY_ENTER; break;
        case 2: in->key = KEY_TAB; break;
        case 3: in->key = KEY_BACKTAB; break;
        case 4: in->key = KEY_BACKSPACE; break;
        case 5: in->key = KEY_F1; break;
        default: in->key = KEY_F5; break;
        }
    } else {
        in->key = KEY_CHAR;
        in->ch = s_chars[mk_r(sizeof s_chars - 1)];
    }
}

static int noflight(void)
{
    int i;
    g_lost_wing = -1;
    g_camp.escort_lost = 0;
    if (g_kill_pct && (int)(rnd() % 100) < g_kill_pct)
        for (i = 0; i < g_m.nship; i++)
            if (g_m.ship[i].type == RV_SHIP_WINGMAN && g_m.ship[i].pilot < RV_PILOTS
                && g_camp.pilot[g_m.ship[i].pilot].alive)
                g_lost_wing = g_m.ship[i].pilot;
    if (g_kill_pct && (int)(rnd() % 100) < g_kill_pct)
        g_camp.escort_lost = 1;
    if (g_kill_pct && (int)(rnd() % 100) < g_kill_pct / 4)
        return rnd() & 1 ? 4 : MISSION_CARRIER_LOST;
    return 1;
}

/* the flight's mission settled, once a flight, before its briefing (or its launch, /fly): the layer's meeting point.
 * missionStarting: a host takes its guests there; a guest's layer puts the host's mission in currentSeries and
 * currentMission (its campaign node, and its mission there) and the host's compiled records in the tables (a guest's
 * own save could number them otherwise: a wingman it lost is left out).  A guest then flies that mission by those
 * records, its own campaign where it was (docs/multiplayer.md).  Alone, nothing changes */
static void flight_prepare(void)
{
    if (g_prepared)
        return;
    g_prepared = 1;
    if (rvNetRole && g_rooms_shown && (g_mode == MODE_ROOM || g_mode == MODE_DECK)) {   /* (the wait can be long: say
                                           what for, over the room on the screen) */
        strcpy(g_scene.label, rvNetRole == 2 ? "Waiting for the host's mission" : "Waiting on the other player...");
        vid_dirty(SCR_H - LABEL_H, SCR_H);
        scene_draw();
    }
    missionStarting();
    g_scene.label[0] = 0;
    g_guest = rvNetRole == 2;
    if (g_guest) {
        uint16_t node = currentSeries, k = currentMission;
        if (node < g_nnodes && k < RV_MISSIONS_PER_NODE && g_nodes[node].missions[k] < g_nmissions) {
            camp_mission(g_nodes[node].missions[k], &g_m);
            camp_load_rules(&g_m);      /* (its rules; its tables are the host's already) */
            /* the fighter the host's records give this pilot: its cockpit, and its sets among the flight's (the user,
             * 2026-11-03: "in the set with the franklin the wingman kept the schroedinger cockpit and the first mission
             * was not able to see any franklins at all": its own save's fighter had been loaded, not the host's) */
            g_camp.player_ship = (uint8_t)camp_flight_fighter();
            camp_follow(node, k);       /* (and its story where the host's is: the debrief moves it on with the host's) */
            rocks_want(g_m.nfield > 0 || g_field > 0);
            RV_LOG("guest: the host's mission %.16Fs (node %u, mission %u there)", (char __far *)g_m.id, node, k);
        } else {
            RV_LOG("guest: the host's mission (node %u, mission %u) is not in this game's campaign", node, k);
        }
    }
}

static void launch(void)
{
    int won, status, landed = 0;
    freeze_hold();                      /* (the load, sets the page may be fetching: until the flight's first frame) */
    flight_prepare();
    mission_load_assets();
    g_scene.label[0] = 0;
    /* the mission's score, in the fixed block where the layer reads it and carries it to the other players
     * (MissionEnd, as in WC1): nonzero won.  The roll is the stand-in, made before the flight so the way home's
     * music knows it (score.c); the game's logic scores the mission over it (logic_start, docs/gameplay.md).  A
     * test's /winrate puts the roll back after the flight, so a script can steer the campaign whatever was flown */
    won = (int)(rnd() % 100) < (g_winrate < 0 ? 80 : g_winrate);
    if (g_route && g_route[g_route_at]) {
        won = g_route[g_route_at] == 'W';
        g_route_at++;
    }
    victoryPoints = won;
    if (g_noflight) {
        status = noflight();
        missionEnded();                 /* (as flight_fly's loop returning would) */
    } else {
        status = flight_fly(&g_m);
        landed = status == 1 && landedOn != 0xFFFF;
        if (landed)
            home_note();
        freeze_hold();                  /* (what is unloaded, until the game's loop is back) */
        if (g_guest)                    /* (the host's mission, the host's story: its losses this one's too) */
            g_guest_splashes = logic_splashes;
        g_lost_wing = logic_wingman_lost();
        g_camp.escort_lost = (uint8_t)logic_escort_lost();
    }
    g_prepared = 0;                     /* (the next flight meets again: a retry, the next mission) */
    if (g_winrate >= 0 || g_route || g_noflight)
        victoryPoints = won;
    g_mk_acts = 0;
    missionVictoryCalc();                           /* the layer's moment: a wingman's score becomes the host's (after
                                                       missionEnded, which flight_fly calls as the loop returns) */
    g_won = victoryPoints != 0;
    mission_free_assets();
    RV_LOG("mission %.16Fs %s", (char __far *)g_m.id, g_won ? "won" : "lost");
    g_now = ticks();
    if (status == 4 && !script_active()) {   /* the player died: the choice first (a test script flies on to the debrief) */
        enter_dead();
        return;
    }
    if (status == MISSION_CARRIER_LOST) {    /* the carrier: the same choice, on its own screen */
        enter_lost();
        return;
    }
    if (landed && enter_home())
        return;
    start_briefing(1);
}

/* --- the bunks --- */

static int g_bunk_sel = -1;
int g_can_quit = 1;                     /* RV.EXE /noquit clears it: no Q in the bunk menu (the page) */
static int g_quit_asked;
static RvSave __far g_bunk_info[BUNKS];
static int g_bunk_full[BUNKS];
static char g_name[24];

/* the bunk menu's row of talk speeds: where each choice is, for drawing and clicking */
#define SPEED_Y 292
#define SPEED_LABEL "TALK SPEED (T)"
static int speed_x(int i, int *w)
{
    int k, x = 110 + gfx_text_width(FONT_BODY, SPEED_LABEL) + 20;
    for (k = 0; k < i; k++)
        x += gfx_text_width(FONT_BODY, g_talk_names[k]) + 22;
    *w = gfx_text_width(FONT_BODY, g_talk_names[i]);
    return x;
}

static void talk_speed_row(int y0, int y1)
{
    int i;
    gfx_text(FONT_BODY, 110, SPEED_Y, SPEED_LABEL, RV_TEXT_AMBER, y0, y1);
    for (i = 0; i < (int)RV_COUNT(g_talk_pcts); i++) {
        int w, x = speed_x(i, &w);
        if (i == g_talk_speed)
            gfx_fill(x - 6, SPEED_Y - 3, w + 12, 20, UI_PANEL_EDGE, y0, y1);
        gfx_text(FONT_BODY, x, SPEED_Y, g_talk_names[i], i == g_talk_speed ? RV_TEXT_WHITE : RV_TEXT_GREY, y0, y1);
    }
}

/* the bunk menu's row of difficulties, between the bunks and the talk speeds (what easy changes is not said: the
 * user, 2026-11-03: "better to keep it magic") */
#define DIFF_Y 272
#define DIFF_LABEL "DIFFICULTY (D)"
static int diff_x(int i, int *w)
{
    int k, x = 110 + gfx_text_width(FONT_BODY, SPEED_LABEL) + 20;   /* (its choices under the talk speeds') */
    for (k = 0; k < i; k++)
        x += gfx_text_width(FONT_BODY, g_diff_names[k]) + 22;
    *w = gfx_text_width(FONT_BODY, g_diff_names[i]);
    return x;
}

static void difficulty_row(int y0, int y1)
{
    int i;
    gfx_text(FONT_BODY, 110, DIFF_Y, DIFF_LABEL, RV_TEXT_AMBER, y0, y1);
    for (i = 0; i < (int)RV_COUNT(g_diff_names); i++) {
        int w, x = diff_x(i, &w);
        if (i == g_easy)
            gfx_fill(x - 6, DIFF_Y - 3, w + 12, 20, UI_PANEL_EDGE, y0, y1);
        gfx_text(FONT_BODY, x, DIFF_Y, g_diff_names[i], i == g_easy ? RV_TEXT_WHITE : RV_TEXT_GREY, y0, y1);
    }
}

/* the bunk menu's extra effects, on the difficulty's row to its right: the wrecks' debris (explode.h; drawn only with
 * the card on DOS, by the native builds themselves), off by default */
#define FX_X 372                        /* (past "easy" and its highlight; "on"'s ends inside the panel) */
#define FX_LABEL "EXTRA FX (E)"
static int fx_x(int i, int *w)
{
    int k, x = FX_X + gfx_text_width(FONT_BODY, FX_LABEL) + 12;
    for (k = 0; k < i; k++)
        x += gfx_text_width(FONT_BODY, g_fx_names[k]) + 16;
    *w = gfx_text_width(FONT_BODY, g_fx_names[i]);
    return x;
}

static void fx_row(int y0, int y1)
{
    int i;
    gfx_text(FONT_BODY, FX_X, DIFF_Y, FX_LABEL, RV_TEXT_AMBER, y0, y1);
    for (i = 0; i < (int)RV_COUNT(g_fx_names); i++) {
        int w, x = fx_x(i, &w);
        if (i == g_fx_extra)
            gfx_fill(x - 6, DIFF_Y - 3, w + 12, 20, UI_PANEL_EDGE, y0, y1);
        gfx_text(FONT_BODY, x, DIFF_Y, g_fx_names[i], i == g_fx_extra ? RV_TEXT_WHITE : RV_TEXT_GREY, y0, y1);
    }
}

/* the bunk menu's row for the ships' detail, beside its title: only with a sprite accelerator to choose it for (gx.h) */
#define DETAIL_Y 56
#define DETAIL_X 282
#define DETAIL_LABEL "SHIP DETAIL (H)"
static int detail_x(int i, int *w)
{
    int k, x = DETAIL_X + gfx_text_width(FONT_BODY, DETAIL_LABEL) + 16;
    for (k = 0; k < i; k++)
        x += gfx_text_width(FONT_BODY, g_detail_names_[k]) + 18;
    *w = gfx_text_width(FONT_BODY, g_detail_names_[i]);
    return x;
}

static void detail_row(int y0, int y1)
{
    int i;
    gfx_text(FONT_BODY, DETAIL_X, DETAIL_Y, DETAIL_LABEL, RV_TEXT_AMBER, y0, y1);
    for (i = 0; i < (int)RV_COUNT(g_detail_names_); i++) {
        int w, x = detail_x(i, &w);
        if (i == g_low_detail)
            gfx_fill(x - 6, DETAIL_Y - 3, w + 12, 20, UI_PANEL_EDGE, y0, y1);
        gfx_text(FONT_BODY, x, DETAIL_Y, g_detail_names_[i], i == g_low_detail ? RV_TEXT_WHITE : RV_TEXT_GREY, y0, y1);
    }
}

/* the bunk menu's buttons: each does what its key does, so a touch screen (a phone, where there are no keys to
 * press) can work the whole menu; the close box at the top right, and a click outside the panel, close it */
#define BUNK_X0 90
#define BUNK_Y0 40
#define BUNK_W 460
#define BUNK_H 300
#define BTN_Y 313
#define BTN_H 21
#define CLOSE_X 522
#define CLOSE_Y 46
#define BUNK_ROW0 82                    /* the first bunk's row, and the rows apart (24: the difficulty's row fits under them) */
#define BUNK_ROW_H 24
typedef struct { const char *label; int key, ch; } Button;

static int bunk_buttons(Button *b)
{
    int n = 0;
    if (g_mode == MODE_NAME) {
        b[n].label = "SAVE (Enter)"; b[n].key = KEY_ENTER; b[n++].ch = 0;
        b[n].label = "CANCEL (Esc)"; b[n].key = KEY_ESC; b[n++].ch = 0;
        return n;
    }
    if (g_bunk_sel >= 0) {
        b[n].label = "SAVE (S)"; b[n].key = KEY_CHAR; b[n++].ch = 's';
        if (g_bunk_full[g_bunk_sel]) {
            b[n].label = "LOAD (L)"; b[n].key = KEY_CHAR; b[n++].ch = 'l';
        }
    }
    if (g_can_quit) {
        b[n].label = "QUIT (Q)"; b[n].key = KEY_CHAR; b[n++].ch = 'q';
    }
    b[n].label = g_bunk_sel >= 0 ? "BACK (Esc)" : "CLOSE (Esc)"; b[n].key = KEY_ESC; b[n++].ch = 0;
    return n;
}

static int button_x(const Button *b, int i, int *w)   /* left to right from the panel's text column */
{
    int k, x = 110;
    for (k = 0; k < i; k++)
        x += gfx_text_width(FONT_BODY, b[k].label) + 12 + 8;
    *w = gfx_text_width(FONT_BODY, b[i].label) + 12;
    return x;
}

static void close_bunks(void);

/* a click on the bunk menu as the key it stands for (KEY_NONE: a click on nothing of it) */
static void bunk_click(Input *in)
{
    Button b[5];
    int i, w, n = bunk_buttons(b);
    if (!in->clicked)
        return;
    in->clicked = 0;
    if ((in->x >= CLOSE_X && in->x < CLOSE_X + 20 && in->y >= CLOSE_Y && in->y < CLOSE_Y + 20)
        || in->x < BUNK_X0 || in->x >= BUNK_X0 + BUNK_W || in->y < BUNK_Y0 || in->y >= BUNK_Y0 + BUNK_H) {
        g_mode = MODE_BUNK;             /* (the close box, or outside: the whole menu closes) */
        close_bunks();
        in->key = KEY_NONE;
        return;
    }
    for (i = 0; i < n; i++) {
        int x = button_x(b, i, &w);
        if (in->x >= x && in->x < x + w && in->y >= BTN_Y && in->y < BTN_Y + BTN_H) {
            in->key = b[i].key;
            in->ch = b[i].ch;
            return;
        }
    }
    in->clicked = 1;                    /* (on a bunk's row or a talk speed: bunk_input's own) */
}

static void bunk_overlay(int y0, int y1)
{
    Button b[5];
    int i, w, n;
    panel(BUNK_X0, BUNK_Y0, BUNK_W, BUNK_H, y0, y1);
    gfx_frame(CLOSE_X, CLOSE_Y, 20, 20, UI_PANEL_EDGE, y0, y1);   /* the close box */
    gfx_text(FONT_BODY, CLOSE_X + 6, CLOSE_Y + 3, "X", RV_TEXT_AMBER, y0, y1);
    gfx_text(FONT_TITLE, 106, 50, "YOUR BUNK", RV_TEXT_AMBER, y0, y1);
    for (i = 0; i < BUNKS; i++) {
        char s[80];
        int y = BUNK_ROW0 + i * BUNK_ROW_H;
        if (g_bunk_full[i])
            sprintf(s, "%d  %.22Fs   layer %u, mission %u", i + 1, (char __far *)g_bunk_info[i].name,
                    (unsigned)g_nodes[g_bunk_info[i].node].layer, (unsigned)g_bunk_info[i].mission + 1);
        else
            sprintf(s, "%d  (empty)", i + 1);
        if (i == g_bunk_sel)
            gfx_fill(100, y - 3, 440, BUNK_ROW_H - 2, UI_PANEL_EDGE, y0, y1);
        gfx_text(FONT_BODY, 110, y, s, i == g_bunk_sel ? RV_TEXT_WHITE : RV_TEXT_GREY, y0, y1);
    }
    if (g_mode != MODE_NAME) {
        difficulty_row(y0, y1);
        fx_row(y0, y1);
        talk_speed_row(y0, y1);
        if (gx_present())
            detail_row(y0, y1);
    }
    n = bunk_buttons(b);
    for (i = 0; i < n; i++) {
        int x = button_x(b, i, &w);
        gfx_fill(x, BTN_Y, w, BTN_H, UI_BLACK, y0, y1);
        gfx_frame(x, BTN_Y, w, BTN_H, UI_PANEL_EDGE, y0, y1);
        gfx_text(FONT_BODY, x + 6, BTN_Y + 5, b[i].label, RV_TEXT_AMBER, y0, y1);
    }
    if (g_bunk_sel < 0)
        gfx_text(FONT_BODY, button_x(b, n - 1, &w) + w + 14, BTN_Y + 5, "pick a bunk", RV_TEXT_GREY, y0, y1);
    if (g_mode == MODE_NAME) {
        char s[48];
        if (g_name[0])
            sprintf(s, "name: %s_", g_name);
        else
            sprintf(s, "name: _   (Bunk %d if left empty)", g_bunk_sel + 1);
        gfx_fill(104, SPEED_Y - 4, 432, 22, UI_BLACK, y0, y1);      /* (in the talk speeds' place) */
        gfx_text(FONT_BODY, 110, SPEED_Y, s, g_name[0] ? RV_TEXT_WHITE : RV_TEXT_GREY, y0, y1);
    }
}

static void open_bunks(void)
{
    int i;
    for (i = 0; i < BUNKS; i++)
        g_bunk_full[i] = save_read(i, &g_bunk_info[i]);
    g_bunk_sel = -1;
    g_mode = MODE_BUNK;
    g_scene.overlay = bunk_overlay;
    vid_dirty(30, 350);
}

static void close_bunks(void)
{
    g_mode = MODE_ROOM;
    g_scene.overlay = room_overlay;
    vid_dirty(30, 350);
}

static void bunk_input(const Input *in0)
{
    Input c = *in0, *in = &c;
    int row = (in->y - (BUNK_ROW0 - 3)) / BUNK_ROW_H;
    bunk_click(in);
    if (g_mode != MODE_BUNK)
        return;
    if (in->key == KEY_ESC) {
        if (g_bunk_sel >= 0) g_bunk_sel = -1; else close_bunks();
        vid_dirty(30, 350);
        return;
    }
    if (g_can_quit && in->key == KEY_CHAR && (in->ch == 'q' || in->ch == 'Q') && g_mode != MODE_NAME) {
        g_quit_asked = 1;               /* the one way out of a DOS install: from the bunk, deliberately */
        return;
    }
    /* the ships' detail: H for the other, or a click on one */
    if (gx_present() && g_mode != MODE_NAME
        && ((in->key == KEY_CHAR && (in->ch == 'h' || in->ch == 'H'))
            || (in->clicked && in->y >= DETAIL_Y - 4 && in->y < DETAIL_Y + 18 && in->x >= DETAIL_X && in->x < CLOSE_X))) {
        int i, w, pick = -1;
        if (!in->clicked)
            pick = !g_low_detail;
        for (i = 0; in->clicked && i < (int)RV_COUNT(g_detail_names_); i++) {
            int x = detail_x(i, &w);
            if (in->x >= x - 8 && in->x < x + w + 8)
                pick = i;
        }
        if (pick >= 0) {
            g_low_detail = pick;
            gx_set(!g_low_detail);
            setup_save();
            vid_dirty(30, 350);
        }
        return;
    }
    /* the extra effects: E for the other, or a click on one (from the next flight) */
    if (g_mode != MODE_NAME && ((in->key == KEY_CHAR && (in->ch == 'e' || in->ch == 'E'))
                                || (in->clicked && in->y >= DIFF_Y - 3 && in->y < DIFF_Y + 16 && in->x >= FX_X - 4))) {
        int i, w, pick = -1;
        if (!in->clicked)
            pick = !g_fx_extra;
        for (i = 0; in->clicked && i < (int)RV_COUNT(g_fx_names); i++) {
            int x = fx_x(i, &w);
            if (in->x >= x - 8 && in->x < x + w + 8)
                pick = i;
        }
        if (pick >= 0) {
            g_fx_extra = (uint8_t)pick;
            setup_save();
            vid_dirty(30, 350);
        }
        return;
    }
    /* the difficulty: D for the other, or a click on one (from the next flight: camp_load_rules) */
    if ((in->key == KEY_CHAR && (in->ch == 'd' || in->ch == 'D')) || (in->clicked && in->y >= DIFF_Y - 3 && in->y < DIFF_Y + 16)) {
        int i, w, pick = -1;
        if (!in->clicked)
            pick = !g_easy;
        for (i = 0; in->clicked && i < (int)RV_COUNT(g_diff_names); i++) {
            int x = diff_x(i, &w);
            if (in->x >= x - 8 && in->x < x + w + 8)
                pick = i;
        }
        if (pick >= 0) {
            g_easy = (uint8_t)pick;
            setup_save();
            vid_dirty(30, 350);
        }
        return;
    }
    /* the talk speed: T for the next, or a click on one */
    if ((in->key == KEY_CHAR && (in->ch == 't' || in->ch == 'T')) || (in->clicked && in->y >= SPEED_Y - 4 && in->y < SPEED_Y + 18)) {
        int i, w, pick = -1;
        if (!in->clicked)
            pick = (g_talk_speed + 1) % (int)RV_COUNT(g_talk_pcts);
        for (i = 0; in->clicked && i < (int)RV_COUNT(g_talk_pcts); i++) {
            int x = speed_x(i, &w);
            if (in->x >= x - 8 && in->x < x + w + 8)
                pick = i;
        }
        if (pick >= 0) {
            g_talk_speed = pick;
            setup_save();
            vid_dirty(30, 350);
        }
        return;
    }
    if (in->key == KEY_UP && g_bunk_sel > 0) g_bunk_sel--, vid_dirty(30, 350);
    if (in->key == KEY_DOWN && g_bunk_sel < BUNKS - 1) g_bunk_sel++, vid_dirty(30, 350);
    if (in->key == KEY_DOWN && g_bunk_sel < 0) g_bunk_sel = 0, vid_dirty(30, 350);
    if (in->clicked && in->x > 100 && in->x < 540 && row >= 0 && row < BUNKS && in->y >= BUNK_ROW0 - 3) {
        g_bunk_sel = row;
        vid_dirty(30, 350);
    }
    if (g_bunk_sel < 0)
        return;
    if (in->key == KEY_CHAR && (in->ch == 's' || in->ch == 'S')) {
        /* an empty bunk starts with no name; a used one offers its own to keep */
        _fstrcpy(g_name, g_bunk_full[g_bunk_sel] ? g_bunk_info[g_bunk_sel].name : (char __far *)"");
        g_mode = MODE_NAME;
        vid_dirty(30, 350);
    } else if (in->key == KEY_CHAR && (in->ch == 'l' || in->ch == 'L') && g_bunk_full[g_bunk_sel]) {
        if (save_load(g_bunk_sel)) {
            g_prepared = 0;             /* (another campaign: its next briefing is a new meeting) */
            if (g_camp.base != g_base)
                change_base(g_camp.base);
            camp_mission(camp_mission_index(), &g_m);
            camp_load_units(&g_m);
            close_bunks();
            enter_room(RV_R_QUARTERS);
            sprintf(g_msg, "Bunk %d loaded: %s. Layer %u, mission %u.", g_bunk_sel + 1, g_camp.name,
                    g_nodes[g_camp.node].layer, g_camp.mission + 1);
            message(g_msg);
        }
    }
}

static void name_input(const Input *in0)
{
    Input c = *in0, *in = &c;
    int n = strlen(g_name);
    bunk_click(in);
    if (g_mode != MODE_NAME)
        return;
    if (in->key == KEY_ESC) {
        g_mode = MODE_BUNK;
    } else if (in->key == KEY_BACKSPACE && n) {
        g_name[n - 1] = 0;
    } else if (in->key == KEY_CHAR && n < (int)sizeof g_name - 1 && in->ch >= 32 && in->ch < 127) {
        g_name[n] = (char)in->ch;
        g_name[n + 1] = 0;
    } else if (in->key == KEY_ENTER) {
        if (!n)
            sprintf(g_name, "Bunk %d", g_bunk_sel + 1);
        save_write(g_bunk_sel, g_name);
        g_bunk_full[g_bunk_sel] = save_read(g_bunk_sel, &g_bunk_info[g_bunk_sel]);
        g_mode = MODE_BUNK;
    } else {
        return;
    }
    vid_dirty(30, 350);
}

/* --- the campaign log (the desk) --- */

/* the campaign log's lattice: a node's place, its layer down (the rows as far apart as the deepest layer lets them be,
 * the two endings a row below it) and its column across (the good ways to the left, the bad to the right: the
 * campaign's own columns, data/campaign.csv; the user, 2026-11-03: "the branching campaign viewable in the barracks
 * should actually branch like it did before--now somehow it's a long line": the 40 ported missions' 13 systems had
 * each a layer of its own in column 0) */
#define LOG_TOP 84
#define LOG_BOTTOM 312
#define LOG_MID_X 200
#define LOG_COL_W 40
static int log_row_h(void)
{
    int i, deepest = 1;
    for (i = 0; i < (int)g_nnodes; i++)
        if (g_nodes[i].layer > deepest)
            deepest = g_nodes[i].layer;
    return (LOG_BOTTOM - LOG_TOP) / deepest < 38 ? (LOG_BOTTOM - LOG_TOP) / deepest : 38;
}

static void log_place(int node, int row_h, int *x, int *y)   /* a node, or RV_NODE_GOOD / RV_NODE_BAD: the endings */
{
    int i, deepest = 1;
    if (node == RV_NODE_GOOD || node == RV_NODE_BAD) {
        for (i = 0; i < (int)g_nnodes; i++)
            if (g_nodes[i].layer > deepest)
                deepest = g_nodes[i].layer;
        *x = LOG_MID_X + (node == RV_NODE_GOOD ? -LOG_COL_W : LOG_COL_W);
        *y = LOG_TOP + deepest * row_h;
        return;
    }
    *x = LOG_MID_X + g_nodes[node].col * LOG_COL_W;
    *y = LOG_TOP + (g_nodes[node].layer - 1) * row_h;
}

static void log_overlay(int y0, int y1)
{
    int i, k, row_h = log_row_h(), x, y;
    char s[80];
    panel(40, 30, 560, 330, y0, y1);
    gfx_text(FONT_TITLE, 56, 40, "CAMPAIGN LOG", RV_TEXT_AMBER, y0, y1);
    /* the lattice: s at the top, v and l at the bottom */
    for (i = 0; i < (int)g_nnodes; i++) {
        int targets[2], t;
        log_place(i, row_h, &x, &y);
        targets[0] = g_nodes[i].good;
        targets[1] = g_nodes[i].bad;
        for (t = 0; t < 2; t++) {
            int tx, ty;
            log_place(targets[t], row_h, &tx, &ty);
            gfx_line(x, y, tx, ty, t ? UI_RED : UI_HILITE, 2, y0, y1);
        }
    }
    for (i = 0; i < (int)g_nnodes; i++) {
        log_place(i, row_h, &x, &y);
        gfx_fill(x - 4, y - 4, 9, 9, i == g_camp.node ? UI_AMBER4 : UI_GREY3, y0, y1);
    }
    log_place(RV_NODE_GOOD, row_h, &x, &y);
    gfx_text(FONT_BODY, x - 8, y + 6, "V", RV_TEXT_WHITE, y0, y1);
    log_place(RV_NODE_BAD, row_h, &x, &y);
    gfx_text(FONT_BODY, x - 6, y + 6, "L", RV_TEXT_WHITE, y0, y1);
    sprintf(s, "Layer %u, mission %u of %u.  Flown: %u.  Trend: %+d", g_nodes[g_camp.node].layer,
            g_camp.mission + 1, camp_node_missions(g_camp.node), g_camp.flown, g_camp.trend);
    gfx_text(FONT_BODY, 300, 84, s, RV_TEXT_WHITE, y0, y1);
    gfx_text(FONT_BODY, 300, 110, "SPLASH COUNT", RV_TEXT_AMBER, y0, y1);
    for (i = 0, k = 0; i < (int)g_npilots; i++) {
        if (g_pilots[i].role != RV_ROLE_PILOT)
            continue;
        sprintf(s, "%-10.10s %3u kills  %2u missions", g_pilots[i].callsign, g_camp.pilot[i].kills,
                g_camp.pilot[i].missions);
        gfx_text(FONT_BODY, 300, 130 + k * 18, s, RV_TEXT_GREY, y0, y1);
        k++;
    }
    gfx_text(FONT_BODY, 56, 336, "click to close", RV_TEXT_GREY, y0, y1);
}

/* --- endings --- */

static void ending_full(int y0, int y1)
{
    char s[80];
    gfx_fill(0, 0, SCR_W, SCR_H, UI_BLACK, y0, y1);
    gfx_text(FONT_TITLE, 250, 150, g_camp.ending == ENDING_GOOD ? "YOU WIN" : "YOU LOSE",
             g_camp.ending == ENDING_GOOD ? RV_TEXT_AMBER : RV_TEXT_WHITE, y0, y1);
    sprintf(s, "%u missions flown.  Press a key.", g_camp.flown);
    gfx_text(FONT_BODY, 210, 200, s, RV_TEXT_GREY, y0, y1);
}

/* --- bases: the ship the player lives on --- */

/* unload this base's rooms and load another's (the music plays on) */
static int change_base(int base)
{
    RV_LOG("base %d -> %d: %s", g_base, base, dat_base_file(base));
    vid_fade(0);
    dat_close();
    if (!dat_open(dat_base_file(base))) {
        RV_LOG("base %d: no %s, back to the carrier", base, dat_base_file(base));
        base = 0;
        if (!dat_open(dat_base_file(0)))
            rv_fail(__FILE__, __LINE__, "no RV.DAT", 0, 0);
    }
    gfx_font_load();
    g_base = base;
    g_camp.base = (unsigned char)base;
    baseChanged(base);
    return base;
}

/* --- missions: their own XMS level, freed when they end --- */

static void mission_load_assets(void)
{
    /* the mission's ships, sounds and cockpit come here with flight; the room's
     * pictures are given back first (the user: the briefing room is not held
     * through a flight), and the archive's assets the flight wants go to the
     * mission's level */
    xms_free_level(XMS_ROOM);
    {
        static const char __far s_when[] = "before a flight";
        mem_ledger(s_when);
    }
    dat_enter(XMS_MISSION);
    dat_log();
    RV_LOG("mission assets: %lu KB of XMS free", xms_free_kb());
}

static void mission_free_assets(void)
{
    xms_free_level(XMS_MISSION);
    {
        static const char __far s_when[] = "after a flight";   /* (every resource back: the user's rule) */
        mem_ledger(s_when);
    }
}

/* --- the next mission --- */

static void next_mission(void)
{
    int i;
    camp_mission(camp_mission_index(), &g_m);
    rocks_want(g_m.nfield > 0 || g_field > 0 || g_nodes[g_camp.node].outside == WINDOW_ROCKS);   /* (a field's art: listed with the
                                           ships', room kept for it; the bar's window has rocks in some systems) */
    for (i = 0; i < RV_PILOTS; i++)     /* (Tally wipes the board: everyone's marks, or one column; not the lost's) */
        if ((g_m.wipe == 0xFE || g_m.wipe == i) && g_camp.pilot[i].alive)
            g_camp.pilot[i].kills = 0;
    camp_load_units(&g_m);
}

/* the bar's simulator: a Schroedinger whatever the mission's fighter, away from the campaign: back in the bar with what
 * it came to, the next mission's tables as they were */
static void start_simulator(void)
{
    unsigned char ship = g_camp.player_ship;
    int status;
    freeze_hold();                      /* (the load, seven sets the page may be making: until the flight's first frame) */
    strcpy(g_scene.label, "Simulator: powering up");   /* (the page may be making the Rlaan's sets) */
    vid_dirty(SCR_H - LABEL_H, SCR_H);
    scene_draw();
    flight_window_close();
    g_scene.leave = 0;
    g_scene.underlay = 0;
    g_scene.overlay = 0;
    g_scene.label[0] = 0;
    logic_boost = 0;
    {
        const RvMission __far *sim = simulator_mission(&g_m);
        camp_load_tables(sim);
        mission_load_assets();
        status = g_noflight ? 0 : flight_simulator(sim);   /* (the test rig's /noflight: no flying here either) */
    }
    freeze_hold();                      /* (what is unloaded, until the game's loop is back) */
    mission_free_assets();
    g_camp.player_ship = ship;
    camp_load_tables(&g_m);
    g_now = ticks();
    enter_room(RV_R_BAR);
    if (simulator_won())
        sprintf(g_msg, "You beat the simulator: %u kills, up to %d Rlaan at a time. It has nothing left to throw at you.",
                simulator_kills(), simulator_count());
    else
        sprintf(g_msg, "%s at %d x %s. Kills: %u.", status == 4 ? "Simulator: shot down" : "Simulator: switched off",
                simulator_count(), ship_name(simulator_type()), simulator_kills());
    message(g_msg);
}

/* the flight just debriefed goes into the save: the campaign's move (camp_resolve; nonzero: the campaign has ended).  A
 * guest's campaign was taken to the host's mission (camp_follow), so it moves on as the host's does: the next mission's
 * bar, barracks and briefing are the host's story's (the user, 2026-11-03: "For player 2 the characters in the bar and
 * the barracks are still from mission 1--they never advance"); its own pilot's sortie and splashes go on its record,
 * and its bunk is saved when it has one (camp_guest_sortie had kept the campaign where it was) */
static int debrief_settle(void)
{
    if (g_guest) {
        g_guest = 0;
        camp_resolve(g_won, g_guest_splashes, 0);
        RV_LOG("guest: the host's mission %s, the campaign on with the host's: node %u, mission %u", g_won ? "won" : "lost",
               g_camp.node, g_camp.mission);
        if (g_camp.name[0])             /* (a campaign never saved has no bunk of its own) */
            (void)save_write(g_camp.bunk, g_camp.name);
        return g_camp.ending;
    }
    camp_resolve(g_won, logic_splashes, logic_pilot_kills);
    return g_camp.ending;
}

static void hot_action(const SceneAction *a)
{
    switch (a->action) {
    case RV_ACT_GOTO:
        if (a->arg == RV_R_BRIEFING)
            start_briefing(0);
        else
            enter_room(a->arg);
        break;
    case RV_ACT_BUNK: open_bunks(); break;
    case RV_ACT_LOG:
        g_mode = MODE_LOG;
        g_scene.overlay = log_overlay;
        vid_dirty(20, 370);
        break;
    case RV_ACT_SCORES: open_board(); break;
    case RV_ACT_SIMULATOR: start_simulator(); break;
    case RV_ACT_MEDALS: message("Your medal case. Medals and ribbons go here as you earn them."); break;
    case RV_ACT_LETTERS: message("Letters from home, read until the folds wore through."); break;
    case RV_ACT_PHOTOS: message("Photographs from before the war."); break;
    case RV_ACT_KILLS: message("Your flight record: kills and missions, one tally mark at a time."); break;
    case RV_ACT_KIT: open_conv(person_of(RV_ROLE_TECH)); break;
    case RV_ACT_LAUNCH: launch(); break;
    }
}

/* (choice): the death screen's key, 0 none; the choice that stands.  Alone, the key; in a shared flight the layer
 * answers for the thunk (the host's choice) */
uint16_t __far __cdecl restartChoice_impl(uint16_t choice)
{
    return choice;
}

/* /fly N: the campaign put where mission N is (its node and place in it), so the debriefing scores that one */
static void fly_mission(int n)
{
    static const char __far s_none[] = "game: /fly %d: no node has that mission";
    unsigned i, k;
    for (i = 0; i < g_nnodes; i++)
        for (k = 0; k < RV_MISSIONS_PER_NODE; k++)
            if (g_nodes[i].missions[k] == (unsigned)n) {
                g_camp.node = (unsigned char)i;
                g_camp.mission = (unsigned char)k;
                campaignNode(g_camp.node, g_camp.mission);
                next_mission();
                return;
            }
    rv_logf(s_none, n);
}

int game_run(void)
{
    Input in;
    SceneAction act;
    g_now = ticks();
    setup_load();
    camp_new();
    if (g_mk)
        g_camp.seed = g_mk;             /* (the run comes out the same: every roll from the seed) */
    next_mission();
    if (g_fly > -2) {                   /* /fly: the rooms skipped, straight out of the launch tube; after the flight
                                           the debriefing and the base as ever */
        if (g_fly >= 0)
            fly_mission(g_fly);
        launch();
    } else if (g_brief >= 0) {         /* (the campaign at mission N, its briefing first) */
        fly_mission(g_brief);
        start_briefing(0);
    } else {
        if (g_at >= 0)
            fly_mission(g_at);
        enter_room(RV_R_QUARTERS);
        message("Wildcard, welcome aboard the TCS Vigilance. Your bunk keeps your saves. "
                "The briefing room is through the big door in the bar.");
    }
    freeze_start();
    while (g_mode != MODE_QUIT) {
        freeze_pet();
        if (g_freeze_test && !--g_freeze_test)
            for (;;)
                ;                       /* (/freezetest N: what the watch is for) */
        rv_halt();                      /* once a music tick at most (1/240 s): the host idles between */
        g_now = ticks();
        in_steer(g_mode != MODE_BUNK && g_mode != MODE_NAME);     /* those menus take the arrows */
        in_poll(&in);
        music_poll();
        if (in.key == KEY_F12)
            shot_save("RVSHOT");
        /* Esc never quits (the user's rule, 2026-10-20: it is the browser's key, for leaving full screen);
         * a test script's quit, and the bunk menu's Q where RV.EXE runs without /noquit (a DOS install:
         * the page passes it, where the player would just reload), are the only ways out */
        if ((in.key == KEY_ESC && in.ch == 'q' && script_active()) || g_quit_asked)
            break;
        scene_update(&in, &act);
        flight_window_tick();
        switch (g_mode) {
        case MODE_ROOM:
            ambient_talk();
            if (act.kind == ACTION_HOT)
                hot_action(&act);
            else if (act.kind == ACTION_SEAT || act.kind == ACTION_ACTOR)
                open_conv(act.arg);
            break;
        case MODE_MESSAGE:
            if (in.clicked || in.key == KEY_ENTER || in.key == KEY_ESC) {
                g_mode = g_back_mode;
                g_scene.overlay = room_overlay;
                vid_dirty(110, 280);
            }
            break;
        case MODE_TALK:
            face_tick();
            if (in.clicked || in.key == KEY_ENTER || in.key == KEY_ESC) {
                int k = in.key == KEY_ESC || g_conv_owner < 0 ? -1 : conv_next();
                if (k >= 0)
                    conv_page(k);
                else {
                    g_conv_owner = -1;
                    close_talk();
                }
            }
            break;
        case MODE_BRIEF:
        case MODE_DEBRIEF:
            if (g_line_face)
                face_tick();
            else
                brief_tick();
            if (g_lines_left) {                 /* (a click anywhere: the pointer comes in over the XO) */
                int next = in.clicked || in.key == KEY_ENTER;
                if (in.key == KEY_ESC) {        /* (the rest of it unheard: straight on) */
                    g_lines_left = 0;
                    line_face_off();
                    g_stage = 2;
                    g_stage_at = g_now;
                } else {
                    if (!next && speech_done()) {
                        if (!g_stage_at)
                            g_stage_at = g_now + 18;
                        next = g_now >= g_stage_at;
                    }
                    if (next) {
                        int k = line_next();
                        if (k >= 0) {
                            line_page(k);
                        } else {                /* (the last line: a click goes on at once, the wait takes the screen off) */
                            g_lines_left = 0;
                            line_face_off();
                            g_stage_at = g_now + 18;
                            if (in.clicked || in.key == KEY_ENTER) {
                                g_stage = 2;
                                g_stage_at = g_now;
                            }
                        }
                    }
                }
            } else if (speech_done() || in.key == KEY_ESC || in.clicked || g_stage == 2) {
                if (!g_stage_at)
                    g_stage_at = g_now + 18;
                if (g_now >= g_stage_at || in.key == KEY_ESC || in.clicked) {
                    if (g_mode == MODE_BRIEF && g_stage < 2) {
                        g_stage++;
                        g_stage_at = g_now + 7;
                        vid_dirty(g_stage == 1 ? 0 : 100, SCR_H);   /* the first: the screen goes off too */
                    } else {
                        int brief = g_mode == MODE_BRIEF;
                        g_stage_at = 0;
                        if (brief) {
                            enter_deck();
                        } else if (debrief_settle()) {
                            g_scene.full = ending_full;
                            g_scene.no_room = 1;
                            g_mode = MODE_ENDING;
                            music_cue(g_camp.ending == ENDING_GOOD ? RV_MUSIC_WIN : RV_MUSIC_LOSE, 300);
                            vid_dirty(0, SCR_H);
                            RV_LOG("ending: %s after %u missions", g_camp.ending == ENDING_GOOD ? "victory" : "loss",
                                   g_camp.flown);
                            if (script_active()) {
                                scene_draw();
                                shot_save("ENDING");
                            }
                        } else {
                            next_mission();
                            enter_room(RV_R_BAR);
                        }
                    }
                }
            }
            break;
        case MODE_BOARD:
            if (in.clicked || in.key == KEY_ENTER || in.key == KEY_ESC)
                close_board();
            break;
        case MODE_DECK:
            deck_tick();
            ambient_talk();
            if (act.kind == ACTION_HOT)
                hot_action(&act);
            break;
        case MODE_BUNK:
            bunk_input(&in);
            break;
        case MODE_NAME:
            name_input(&in);
            break;
        case MODE_LOG:
            if (in.clicked || in.key == KEY_ESC || in.key == KEY_ENTER) {
                g_mode = MODE_ROOM;
                g_scene.overlay = room_overlay;
                vid_dirty(20, 370);
            }
            break;
        case MODE_ENDING:
            if (in.key != KEY_NONE || in.clicked)
                g_mode = MODE_QUIT;
            break;
        case MODE_HOME:                         /* (her say, and a moment to read it; a click goes on at once) */
            face_tick();
            plate_tick();
            if (speech_done() && !g_stage_at)
                g_stage_at = g_now + 40;
            if (in.clicked || in.key == KEY_ENTER || in.key == KEY_ESC || (g_stage_at && g_now >= g_stage_at))
                start_briefing(1);
            break;
        case MODE_DEAD:
            if (g_lost) {
                lost_tick();
                if (g_now - g_lost_t0 < LOST_WAIT_TICKS)
                    break;
            }
            {
                /* the key (1 fly it again, 2 carry on, 0 none), and the choice that stands: alone the key; in a shared
                 * flight the host's, for everyone (the restartChoice thunk: docs/multiplayer.md) */
                int key = in.ch == 'r' || in.ch == 'R' ? 1
                          : in.ch == 'c' || in.ch == 'C' || in.key == KEY_ENTER || in.clicked ? 2 : 0;
                int choice = (int)restartChoice((uint16_t)key);
                if (choice == 1) {
                    leave_dead();
                    if (!g_guest)       /* (a guest's tables come with the host's mission again: flight_prepare) */
                        camp_load_units(&g_m);   /* (the mission's tables as they were before the flight) */
                    launch();
                } else if (choice == 2) {
                    leave_dead();
                    start_briefing(1);
                }
            }
            break;
        }
        scene_draw();
        {
            static const char __far s_rooms[] = "the rooms";
            static unsigned __far s_frame;
            mem_watch(s_rooms, ++s_frame);
        }
    }
    freeze_stop();
    if (g_mk)
        RV_LOG("monkey: %lu actions, %u leaks on the ledger", g_mk_total, mem_bad());
    return 1;
}
