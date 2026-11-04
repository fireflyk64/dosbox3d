# RVGX: the sprite accelerator

An imaginary graphics card with one job: draw a ship's picture, turned and scaled, into the game's frame buffer, from
images it fetches by itself. Only our DOSBox has one (`src/hardware/rvgx.cpp`). On real DOS or a stock DOSBox the game
finds no card, makes no further calls, and draws its ships as blocks, as it always has.

With one, the ships are drawn a pixel a pixel from the same full-size views the native build uses, and they stream in
over the network while the game carries on. Any draw the card cannot do (the picture has not come, or never will) it
says so in a word, and the game draws that ship the old way that frame. The `` ` `` key (or `~`) flips between the two at
any moment; the bunk menu's SHIP DETAIL row is the same setting, kept in RVSETUP.CFG, high by default.

The point is memory more than speed. A mission's full-size views are tens of megabytes, far past what a DOS machine can
hold beside everything else, and a capital ship's alone are why the game draws those as 4 x 4 blocks. On the card they
live outside the emulated machine altogether: in the browser's cache and the card's own memory, costing the game nothing,
so a capital ship is drawn at full resolution however many are about. The DOS machine still holds what it needs to draw
the blocks, because any picture may be missing at any moment.

## Calling it from DOS

Everything goes through `INT 2Fh`, DOS's multiplex interrupt, because every DOS since 3.0 answers it: an unclaimed
function comes back with AL as it was, 0. There are no other registers to set up for a call: the arguments live in one
block at a fixed address, told to the card once.

```c
/* is there a card? */
r.x.ax = 0xE700;  r.x.bx = 'RV';  r.x.cx = 'GX';  int86(0x2F, &r, &r);
present = r.h.al == 0xFF && r.x.bx == 'gx' && r.h.ch == 1;

/* where the block is: once */
s.es = FP_SEG(&block);  r.x.bx = FP_OFF(&block);  r.x.ax = 0xE701;  int86x(0x2F, &r, &r, &s);

/* a draw, ever after: fill the block, then */
block.result = -100;
_asm { mov ax, 0xE720; int 0x2F }      /* (gx.c: #pragma aux gx_int) */
if (block.result < 0)
    draw_it_the_old_way();
```

Setting `result` before the call means a card that did nothing counts as a failure. The signature means some other
program that happens to use AH=E7h cannot be mistaken for a card.

| AX | what it does |
|---|---|
| `E700h` | Install check. In: BX `'RV'` (5256h), CX `'GX'` (4758h). With a card: AL FFh, BX `'gx'` (7867h), CH.CL the version (1.0). Without: AL 0, nothing else changed. |
| `E701h` | Attach the block at ES:BX. AX 0. |
| `E710h` | Name a slot (below): what the slot `bundle` is to hold. Returns at once; nothing waits for the network. |
| `E711h` | Status: fills the `stat_` fields. |
| `E720h` | Draw. `result` 0 when drawn. |
| `E730h` | (1.2) A wreck's debris: the block's frame buffer fields (16 to 38) and its debris part (64 to 127). `result` 0. |

### The block (64 bytes, packed, little-endian)

| Offset | Field | Meaning |
|---|---|---|
| 0 | `int16 result` | written by the card; negative: why not (below) |
| 2 | `uint16 bundle` | a slot, 0 to 255 |
| 4 | `uint16 view` | which picture in the bundle |
| 6 | `uint16 flags` | bit 0: mirrored left to right |
| 8, 10 | `int16 x, y` | where the picture's origin lands on the screen |
| 12 | `int16 scale` | 256 = as stored |
| 14 | `int16 roll` | clockwise, 1024 a turn |
| 16 | `far fb` | offset then segment: the first pixel of screen row `fb_y0` |
| 20 | `uint16 stride` | bytes a row |
| 22 | `int16 fb_y0` | the screen row `fb` points at |
| 24, 26 | `int16 y0, y1` | the rows it may write, y0 .. y1-1 |
| 28, 30 | `int16 xa, xb` | the columns it may write, xa .. xb-1 |
| 32 | `far ext` | 0:0, or a table of two int16 a row: each row's first column and one past its last (the cockpit's opening) |
| 36, 38 | `int16 ext_y0, ext_rows` | the rows that table covers |
| 40 | `far name` | `E710h`: offset then segment of the name, NUL-terminated; 0:0 or an empty name empties the slot |
| 48, 52, 56 | `uint32 stat_draws, stat_px, stat_us` | `E711h`: draws, pixels, host microseconds since the emulator started |
| 60, 62 | `uint16 stat_ready, stat_waiting` | `E711h`: bundles in, and on the way |
| 64 .. 127 | `RvDebris` | (1.2, `E730h`) the cloud, as `retrovega/src/debris.h` has it packed: `uint16 seed, age, life, count` (64), `int16 focal, cx, cy` (72), `int32 x, y, z` (78: its centre in camera space, 16ths of a km), `int32 spread, piece` (90), `int16 m[9]` (98: world to camera, 256 = 1), `uint8 metal[8], hot[4]` (116, 124: palette entries, dark to light) |

From 1.1 the block is 128 bytes. A 1.0 card reads none of the second half; 1.1 (a night's deploy, 2026-11-03) had six
colours in each ramp, so the game draws debris only when the card says 1.2 or later (`gx_debris_on`).

The frame buffer must be ordinary conventional or upper memory (not video memory, not an EMS page frame): anything else
is `-5` and the game draws for itself. Pixels of value 0 in a picture are clear.

### The result word

| Value | Meaning | What the game does |
|---|---|---|
| 0 | drawn | nothing |
| -1 | that slot is empty | draws its blocks |
| -2 | it is not to be had (not on the site, damaged) | draws its blocks, for good |
| -3 | it has not come yet | draws its blocks; the next frame may find it |
| -4 | no such view in it, or a damaged one | draws its blocks |
| -5 | the frame buffer is not memory the card can write | draws its blocks |

The game treats them alike and only counts them: a flight's end logs `gx: this flight N ships by the card, M fell
back: ...`.

### The slots

The card has 256 slots, each holding one bundle, and is told about them one at a time. There is no list to build: the game
keeps nothing, and passes the name from the stack.

```c
for (i = 0; i < n; i++) {
    char name[16];
    sprintf(name, "SHIP%02X.RV", types[i]);
    block.bundle = types[i];  block.name = name;
    gx_int(0xE710);
}
```

The game's convention is that a ship type's slot is its number, so a draw needs no table to find it. A slot keeps what it
holds until it is named otherwise, across missions, in the card's memory rather than the machine's. Naming a slot with the
name it already holds does nothing, so the game simply repeats the loop at the briefing and the launch (`ships_want`). A
bundle that could not be had is tried again when it is next named. A different name replaces the slot's bundle, and any
answer still on its way for the old one is dropped. Names hold only letters, digits, `.`, `_` and `-`, up to 64 of them;
anything else is `-2`.

A bundle is a ship's `SHIPnn.RV`, the same file the page already fetches to make its block sets, in one view numbering:
the game places a ship with `lf_place` on the file's own lattice, and sends the stored view (a symmetric ship's
mirrored headings are their pair's, flag 1).

## Inside the card

- `src/hardware/rvgx.cpp` is the DOS device: the handshake, the block, the slots, the cache of expanded views (48 MB,
  least recently used out). It lives on the multiplex chain, added in `DOS_SetupMisc`.
- `src/hardware/rvgx_kernel.h` is the pixel code, plain C++ with no DOSBox in it: the file parsed and each record of a
  view checked (the data comes over a network), and `rvgx_draw`, the turn and scale by inverse mapping.
- It takes `lf_rotate_setup` and `lf_rotate_span` from the game's own `src/lightfield.c`, so a ship lands exactly where
  the native build's does.
- A wreck's debris (`E730h`) is the game's own `src/debris.c` as well, the same code the native builds draw it with:
  forty chunks of armour cut from the plate's damage stamps (`src/debris_art.h`, made by `tools/debris.py`), each a
  picture a tumble frame drawn upright and scaled as the rocks are, glowing while hot, and embers, all from the cloud's
  seed and age, clipped to the rows, the columns and the cockpit's opening like a ship. The tumble's pictures (8 chunks,
  16 frames, 48, 24 and 12 pixels) are made once when the card is installed (`rv_debris_prepare`, 5 ms natively). Only
  the card draws it: DOS alone shows no debris (the user's design, 2026-11-03). The game makes the call only for the bands
  the cloud's box reaches, and only with the bunk menu's EXTRA FX on (off by default since the user found the kills
  stutter with it, 2026-11-03: `g_fx_extra`, explode.h).
- A draw takes no emulated time, and it writes straight into the emulated memory. The game's clock does not see it; if
  it ever cost too much, the emulator would fall behind real time rather than the game slowing down.

The browser gets its bundles from the page: the device calls `Module.rvgxWant(id, name)`, `web/wc.js` fetches through
`web/rvsets.js` (the site's list `rv/ships.json`, the Cache API by hash, the same downloads as the block sets, fetched
once if both ask), copies the file into the wasm heap and calls `wc_web_rvgx_loaded`. Natively the card is installed only
when `RVGX_DIR` names a directory; `RVGX_DELAY_MS` pretends the transfer takes that long. `RVGX=0` removes the card from
either (`?env.RVGX=0` on the page). `RV.EXE /nogx` does not ask for it, whatever is there.

## How fast

Measured on this box's i7-6800K core, `tests/host/gx_kernel_test.sh` (`bench`), the Gawain's 131 x 104 views:

| Ship on screen | Native (g++ -O2) | Wasm (em++ -O2, node) |
|---|---|---|
| quarter size | 2.9 us | 6.6 us |
| half size | 9.1 us | 9.5 us |
| full size | 20.4 us | 19.9 us |
| 1.56 x full size | 43.1 us | 48.6 us |

About 480 million pixels a second in wasm: some 1,600 full-size fighters in a 33 ms frame if they had all of it, so
several dozen ships are nothing. A view is expanded once (4-9 us) and kept. There is no case yet for SIMD or canvas.

## Checks

- A ship's box in the zoom scene, this card against the native build's picture: identical in 142 of 142 frames with the
  ship logged at one place (2026-11-01; both builds, `/zoom 1 /bench 0 /record 2`, compare `R*.BMP` where `lightfield:` agrees).
- `tests/host/gx_kernel_test.sh`: every draw path against the game's `lf_rotate_row` on real art (clips, extents, mirror,
  rolls, scales), damaged files decoded and drawn under ASan and UBSan, then the table above; and `debris N`: N random
  clouds, wild values among them, each into a buffer of exactly its rows, with no pixel outside the rows, the columns and
  the extents (50,000 clean, 2026-11-03), then a cloud's time (4.3 us natively).
- `GX=1 EXE=RVT.EXE tests/run.sh tests/gxzoom.txt /zoom 1 /bench 0`: the zoom scene with the key flipping, a shot of each.
  `GXDELAY=4000` shows the blocks giving way when the bundle lands; `RVGX_DIR=/empty` stays blocks.
- `DOSBOX=/usr/bin/dosbox tests/run.sh tests/gx.txt`: a stock DOSBox: `gx: no sprite accelerator`, and the flight as ever.
