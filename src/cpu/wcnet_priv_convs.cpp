/*
 *  Privateer's added ships: the conversations of their sale (wcnet_priv_ships.h, docs/privateer.md section 42).  The
 *  user (2026-11-27): "you can purchase the stiletto in privateer after talking to admiral terrell after winning the
 *  original campaign.  He goes through his credits spiel and then I want an accept or reject where you can buy the
 *  Stiletto for 500,000 credits ... Maybe we can clip together some words from terrel's speeches to make it
 *  plausible", and later "get the buy script and story right clipping together Terrell and ship dealer words".
 *
 *  A conversation is two files of DATA\CONV: <name>.PFC, its lines, and <name>.VPK, the recorded speech of each.
 *
 *  The PFC is a run of records, each begun by a byte:
 *    0  a line: the speaker (9 bytes: a face of OPTIONS\FACES.IFF, "pc" the player, "rand_npc" a face of the room's
 *       people), the mood (9: "normal", "angry", "surprise" ...), the close-up's backdrop (9: OPTIONS\CU.IFF's,
 *       "adoff_1" Terrell at his desk, "adoff_2" the player there, "mrchcu" the Merchants' Guild's desk), four bytes
 *       (the text's colour, 0, a number that goes with the speaker, 0), the text ended by a zero, and one byte more;
 *    7  the player is asked: Accept or Reject (speaker, backdrop, the four bytes; no text);
 *    5  the end of what follows an Accept (what follows a Reject begins here);
 *    8  the end.
 *  The text's $NM is the player's name, $CS the callsign, and $U1-$U4 numbers the dealer's routine gives it: the old
 *  ship's value, the extras', the new ship's price, and what is left (or lacking).
 *
 *  The VPK: its length, a table of offsets (24 bits; the top byte 0x20: compressed), and per line of the PFC, in
 *  order, the plain length and a Creative Voice File compressed with the game's LZW (codes of 9 to 12 bits, low bit
 *  first; 256 empties the dictionary, 257 ends).  The speech is 8 bits at 11,111 a second (the time constant 0xA6).
 *
 *  Nothing recorded is in this program.  A line's speech is a list of pieces: a stretch (in milliseconds) of a line
 *  of one of the game's own conversations, read from the player's TRE.  The stretches were found with a speech
 *  recogniser's word times over those files (scripts/privships/convwords.py prints them), and what is SAID is not
 *  always what is WRITTEN: the dealer's lines with numbers in them are spoken without ("I'll use your old ship as a
 *  trade-in, including any extras"), which is why they can be used for any price.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include "wcnet_priv_ships.h"
#include "wcnet_log.h"

namespace wc {

namespace {

typedef std::vector<Bit8u> Bytes;

// ---- The game's LZW -------------------------------------------------------------------------------------------------

bool lzw_decode(const Bit8u *src, size_t n, size_t expect, Bytes *out) {
    struct Entry { Bit16u prev; Bit8u last; };
    std::vector<Entry> dict;
    dict.reserve(4096);
    out->clear();
    out->reserve(expect);
    int bits = 9, prev = -1;
    size_t at = 0;
    Bytes word;
    for (;;) {
        if (out->size() >= expect) return true;
        if ((at + bits + 7) / 8 > n) return false;
        unsigned code = 0;
        for (int i = 0; i < bits; i++) code |= (unsigned)((src[(at + i) >> 3] >> ((at + i) & 7)) & 1) << i;
        at += bits;
        if (code == 256) { dict.clear(); bits = 9; prev = -1; continue; }
        if (code == 257) return out->size() == expect;
        const unsigned next = 258 + (unsigned)dict.size();
        if (code > next || (code == next && prev < 0)) return false;
        // the code's bytes, last first
        word.clear();
        unsigned c = code;
        Bit8u first;
        if (code == next) {                 // (the entry about to be made: the previous word and its own first byte)
            c = (unsigned)prev;
            word.push_back(0);
        }
        while (c >= 258) { word.push_back(dict[c - 258].last); c = dict[c - 258].prev; }
        word.push_back((Bit8u)c);
        first = (Bit8u)c;
        if (code == next) word[0] = first;
        for (size_t i = word.size(); i-- > 0;) out->push_back(word[i]);
        if (prev >= 0) {
            Entry e; e.prev = (Bit16u)prev; e.last = first;
            dict.push_back(e);
            if (258 + dict.size() == (1u << bits) && bits < 12) bits++;
        }
        prev = (int)code;
    }
}

// Written the plain way: every byte its own 9-bit code, the dictionary emptied before the reader's would outgrow 9
// bits (it adds an entry a code).  An eighth longer than the bytes; no reader's corner is leaned on.
struct BitOut {
    Bytes *out; Bit32u acc; int have;
    explicit BitOut(Bytes *o) : out(o), acc(0), have(0) {}
    void code(unsigned c) { acc |= (Bit32u)c << have; have += 9; while (have >= 8) { out->push_back((Bit8u)acc); acc >>= 8; have -= 8; } }
    void end() { if (have) out->push_back((Bit8u)acc); }
};
void lzw_plain(const Bytes &in, Bytes *out) {
    BitOut bits(out);
    int since = 0;
    for (size_t i = 0; i < in.size(); i++) {
        if (since == 200) { bits.code(256); since = 0; }
        bits.code(in[i]);
        since++;
    }
    bits.code(257);
    bits.end();
}

void put32(Bytes &v, Bit32u x) { v.push_back((Bit8u)x); v.push_back((Bit8u)(x >> 8)); v.push_back((Bit8u)(x >> 16)); v.push_back((Bit8u)(x >> 24)); }
Bit32u get32(const Bit8u *p) { return ((Bit32u)p[3] << 24) | ((Bit32u)p[2] << 16) | ((Bit32u)p[1] << 8) | p[0]; }

// ---- A recorded line of the game's ----------------------------------------------------------------------------------

enum { kRate = 11111, kTimeConstant = 0xA6 };

// Line `index` of a VPK as its samples (false: not there, or not speech of the kind every line of the game's is)
bool vpk_line(const Bytes &vpk, int index, Bytes *pcm) {
    if (vpk.size() < 8) return false;
    const Bit32u first = get32(&vpk[4]) & 0xFFFFFF;
    if (first < 8 || first > vpk.size() || (first & 3)) return false;
    const int count = (int)((first - 4) / 4);
    if (index < 0 || index >= count) return false;
    const Bit32u word = get32(&vpk[4 + 4 * index]);
    const Bit32u at = word & 0xFFFFFF, end = index + 1 < count ? (get32(&vpk[8 + 4 * index]) & 0xFFFFFF) : (Bit32u)vpk.size();
    if (at + 4 > end || end > vpk.size()) return false;
    Bytes voc;
    if ((word >> 24) == 0x20) {
        const Bit32u plain = get32(&vpk[at]);
        if (plain > (4u << 20) || !lzw_decode(&vpk[at + 4], end - at - 4, plain, &voc)) return false;
    } else {
        voc.assign(vpk.begin() + at, vpk.begin() + end);
    }
    if (voc.size() < 26 || memcmp(&voc[0], "Creative Voice File\x1a", 20) != 0) return false;
    size_t p = voc[20] | (voc[21] << 8);
    pcm->clear();
    while (p + 4 <= voc.size() && voc[p] != 0) {
        const size_t n = voc[p + 1] | (voc[p + 2] << 8) | ((size_t)voc[p + 3] << 16);
        if (p + 4 + n > voc.size()) return false;
        if (voc[p] == 1) {
            if (n < 2 || voc[p + 4] != kTimeConstant || voc[p + 5] != 0) return false;     // (another rate or packing: not ours to cut)
            pcm->insert(pcm->end(), voc.begin() + p + 6, voc.begin() + p + 4 + n);
        } else if (voc[p] == 2) {
            pcm->insert(pcm->end(), voc.begin() + p + 4, voc.begin() + p + 4 + n);
        }
        p += 4 + n;
    }
    return !pcm->empty();
}

void voc_build(const Bytes &pcm, Bytes *out) {
    static const Bit8u head[] = { 'C','r','e','a','t','i','v','e',' ','V','o','i','c','e',' ','F','i','l','e', 0x1A, 0x1A, 0x00, 0x0A, 0x01, 0x29, 0x11 };
    out->assign(head, head + sizeof(head));
    const Bit32u n = (Bit32u)pcm.size() + 2;
    out->push_back(1); out->push_back((Bit8u)n); out->push_back((Bit8u)(n >> 8)); out->push_back((Bit8u)(n >> 16));
    out->push_back(kTimeConstant); out->push_back(0);
    out->insert(out->end(), pcm.begin(), pcm.end());
    out->push_back(0);
}

// ---- The script -----------------------------------------------------------------------------------------------------

// A piece of a line's speech: of conversation `conv`, line `line`, from `from` to `to` milliseconds (to 0: its end),
// then `gap` milliseconds of quiet.
struct Piece { const char *conv; int line, from, to, gap; };
enum { kLine = 0, kAsk = 7, kElse = 5, kEnd = 8 };
enum Who { kTerrell, kPlayer, kDealer, kBarman, kPlayerBar };
struct Line {
    int op;
    Who who;
    const char *mood;
    const char *text;
    Piece voice[4];             // (a conv of 0 ends them)
};
// Who speaks: the face, the backdrop, the four bytes and the last one, as the game's own lines of that speaker in
// that place have them (Terrell's in S7MBC4, the player's there, the guild clerk's in MRCHGLD1)
struct Speaker { const char *face, *place; Bit8u look[4]; Bit8u tail; };
const Speaker kSpeakers[] = {
    { "terrel", "adoff_1", { 159, 0, 11, 0 }, 200 },
    { "pc", "adoff_2", { 172, 0, 15, 0 }, 160 },
    // (the ship dealer's own face is "rand_npc", one of the people of a dealer's room: anywhere else it is the
    // game's error 70C8.  The Merchants' Guild's clerk is a face of his own, with the salesman's voice here.)
    { "merchguy", "mrchcu", { 159, 0, 20, 0 }, 245 },
    // (a bar's: the bartender is one of the room's people, as in the game's own rumours; the player at the bar)
    { "rand_npc", "randcu_3", { 159, 0, 9, 0 }, 224 },
    { "pc", "randcu_2", { 172, 0, 15, 0 }, 160 },
};

#include "wcnet_priv_convs.inc"

void put_name9(Bytes &v, const char *s) { for (int i = 0; i < 9; i++) { v.push_back(*s ? (Bit8u)*s : 0); if (*s) s++; } }

// Every "%s" of a text is the ship's name
std::string named(const char *text, const char *name) {
    std::string out;
    for (const char *p = text; *p; p++) {
        if (p[0] == '%' && p[1] == 's') { out += name; p++; } else out += *p;
    }
    return out;
}

// The salesman's voice made rougher and sharper for a pirate bar's fence (the user: "The barman went from normal man
// to ship dealer ... filter those on the fly to make it sound more raspy or sharp than the rounded voice of the ship
// dealer trying to make a sleazy deal").  On the 8-bit samples, 11,111 a second: the lows thinned (each sample less
// most of the one before: a first-order high-pass, mixed with a little of the plain voice), a growl (the loudness
// wavering some 70 times a second, unevenly), and the peaks pressed flat (a soft clip, which adds the rasp's edge).
void voice_rasp(Bytes *pcm) {
    double before = 0, phase = 0;
    Bit32u seed = 12345;
    for (size_t i = 0; i < pcm->size(); i++) {
        const double x = (double)(*pcm)[i] - 128.0;
        const double sharp = 0.25 * x + 1.9 * (x - 0.82 * before);
        before = x;
        seed = seed * 1103515245u + 12345u;
        phase += (70.0 + (double)((seed >> 16) & 31) - 15.0) / kRate;      // (55-86 a second, never steady)
        if (phase >= 1.0) phase -= 1.0;
        const double growl = 1.0 - 0.45 * (phase < 0.5 ? phase * 2.0 : 2.0 - phase * 2.0);
        double y = sharp * growl * 1.5 / 100.0;
        y = y / (1.0 + (y < 0 ? -y : y));                                   // (soft clip)
        int v = (int)(128.5 + y * 150.0);
        (*pcm)[i] = (Bit8u)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

bool conv_build(const PrivAddedShip &ship, const Line *lines, size_t count, bool rasp, Who dealerAs, PrivTreFile file, Bytes *pfc, Bytes *vpk) {
    std::map<std::string, Bytes> sources;
    std::vector<Bytes> speech;
    pfc->clear();
    for (size_t i = 0; i < count; i++) {
        const Line &l = lines[i];
        pfc->push_back((Bit8u)l.op);
        if (l.op == kElse || l.op == kEnd) continue;
        // (the salesman's lines are said by the face that makes the offer: the user, "the face of terrell changes to the
        // ship dealer during the conversation.  Can we keep the audio as is but keep the face terrell the whole way?")
        const Speaker &s = kSpeakers[l.who == kDealer ? dealerAs : l.who];
        put_name9(*pfc, s.face);
        if (l.op == kLine) put_name9(*pfc, l.mood);
        put_name9(*pfc, s.place);
        pfc->insert(pfc->end(), s.look, s.look + 4);
        if (l.op != kLine) continue;
        const std::string text = named(l.text, ship.name);
        pfc->insert(pfc->end(), text.begin(), text.end());
        pfc->push_back(0);
        pfc->push_back(s.tail);

        Bytes pcm;
        for (int k = 0; k < 4 && l.voice[k].conv; k++) {
            const Piece &p = l.voice[k];
            if (!sources.count(p.conv)) {
                Bytes v;
                if (!file(std::string("..\\..\\DATA\\CONV\\") + p.conv + ".VPK", &v)) {
                    wclog(1, "Privateer: added ships: no %s.VPK in the TRE (a line's speech is cut from it)", p.conv);
                    return false;
                }
                sources[p.conv].swap(v);
            }
            Bytes all;
            if (!vpk_line(sources[p.conv], p.line, &all)) {
                wclog(1, "Privateer: added ships: %s.VPK's line %d is not speech as expected", p.conv, p.line);
                return false;
            }
            size_t a = (size_t)p.from * kRate / 1000, b = p.to ? (size_t)p.to * kRate / 1000 : all.size();
            if (b > all.size()) b = all.size();
            if (a > b) a = b;
            // a cut's ends eased over 6 ms (a cut in the middle of a sound would click)
            const size_t ease = 64, start = pcm.size();
            pcm.insert(pcm.end(), all.begin() + a, all.begin() + b);
            const size_t len = b - a;
            for (size_t e = 0; e < ease && e < len / 2; e++) {
                if (p.from) pcm[start + e] = (Bit8u)(128 + ((int)pcm[start + e] - 128) * (int)e / (int)ease);
                if (p.to) pcm[start + len - 1 - e] = (Bit8u)(128 + ((int)pcm[start + len - 1 - e] - 128) * (int)e / (int)ease);
            }
            pcm.insert(pcm.end(), (size_t)p.gap * kRate / 1000, 128);
        }
        if (l.who == kDealer && rasp) voice_rasp(&pcm);
        if (pcm.empty()) pcm.assign(kRate / 10, 128);       // (a line with nothing said: a tenth of a second of quiet)
        Bytes voc, packed;
        voc_build(pcm, &voc);
        put32(packed, (Bit32u)voc.size());
        lzw_plain(voc, &packed);
        speech.push_back(packed);
    }
    vpk->clear();
    Bit32u at = 4 + 4 * (Bit32u)speech.size(), total = at;
    for (size_t i = 0; i < speech.size(); i++) total += (Bit32u)speech[i].size();
    put32(*vpk, total);
    for (size_t i = 0; i < speech.size(); i++) { put32(*vpk, at | 0x20000000u); at += (Bit32u)speech[i].size(); }
    for (size_t i = 0; i < speech.size(); i++) vpk->insert(vpk->end(), speech[i].begin(), speech[i].end());
    return true;
}

}   // namespace

// The four conversations of an added ship's sale, under the names the sale gives the dealer's routine
// (priv_ship_sale_names): the offer, the cargo that does not fit, too poor, and the ship already owned.
bool priv_ship_convs(const PrivAddedShip &ship, PrivTreFile file, std::vector<std::pair<std::string, std::vector<Bit8u> > > *files) {
    const ShipTalk *talk = 0;
    for (size_t i = 0; i < sizeof(kTalks) / sizeof(kTalks[0]); i++) if (kTalks[i].ship == ship.id) talk = &kTalks[i];
    if (!talk) return false;
    char names[4][9];
    priv_ship_sale_names(ship, names);
    for (int k = 0; k < 4; k++) {
        Bytes pfc, vpk;
        if (!conv_build(ship, talk->convs[k].lines, talk->convs[k].count, talk->rasp, talk->dealerAs, file, &pfc, &vpk)) return false;
        std::string name = names[k];
        for (size_t i = 0; i < name.size(); i++) if (name[i] >= 'a' && name[i] <= 'z') name[i] -= 32;
        files->push_back(std::make_pair(name + ".PFC", pfc));
        files->push_back(std::make_pair(name + ".VPK", vpk));
    }
    return true;
}

}
