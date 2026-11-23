// Written by scripts/privtalon.py: hand-made 2x art for parts of the target display's ships.  Per part: its
// mask hash, the game pixels it stands for (u0, v0, w, h: 'x' drawn by the art, 'b' the game's block, '.' none
// of the part -- the part's lit pixels must be exactly these), the game pixels whose colours the art's 'o', 'k'
// and 'f' take, and the art (2w x 2h fine pixels).
#ifndef WCNET_PRIV_TALON_H_
#define WCNET_PRIV_TALON_H_

struct PrivPartArt {
    unsigned int part;
    int u0, v0, w, h;
    const char *game[24];
    int sample[3][2];
    const char *fine[48];
};
static const PrivPartArt kPrivPartArt[] = {
    { 0xC7EFE729u, 19, 29, 14, 17,
      {
        "...xxxxxxxx...",
        "..x........x..",
        ".x..........x.",
        ".x...xxxx...x.",
        ".x..xxxxxx..x.",
        "x...xxxxxx...x",
        ".x..xxxxxx..x.",
        ".bbb.xxxx.bbb.",
        ".b.b..xx..b.b.",
        ".bbb......bbb.",
        ".b.b......b.b.",
        ".b.bx....xb.b.",
        ".bbbx....xbbb.",
        "..b.x....x.b..",
        "..b..x..x..b..",
        ".....x..x.....",
        "......xx......",
      },
      { { 22, 29 }, { 24, 32 }, { 25, 34 } },
      {
        "......oooooooooooooooo......",
        ".....o................o.....",
        "....o..................o....",
        "...o....................o...",
        "..o......................o..",
        ".o........................o.",
        ".o.........kkkkkk.........o.",
        ".o.......kkffffffkk.......o.",
        "o.......kffffffffffk.......o",
        "o......kffffffffffffk......o",
        "o......kffffffffffffk......o",
        "o.......kffffffffffk.......o",
        ".o......kffffffffffk......o.",
        ".o......kffffffffffk......o.",
        ".........kffffffffk.........",
        "..........kffffffk..........",
        "...........kffffk...........",
        "............kkkk............",
        "............................",
        "............................",
        "............................",
        "............................",
        "........o..........o........",
        "........o..........o........",
        "........o..........o........",
        ".........o........o.........",
        ".........o........o.........",
        ".........o........o.........",
        ".........o........o.........",
        "..........o......o..........",
        "..........o......o..........",
        "...........o....o...........",
        "............o..o............",
        ".............oo.............",
      } },
};
enum { kPrivPartArtCount = sizeof(kPrivPartArt) / sizeof(kPrivPartArt[0]) };

#endif
