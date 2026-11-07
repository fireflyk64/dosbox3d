/*
 *  TIE Fighter's Z-95 challenge in its own voices (docs/tiefighter.md, step 14; the user, 2026-11-07: "it would be nice
 *  to have the audio to match the text in the challenge. Can we make a script to duplicate them in? Especially the z95
 *  line"; "ideally all radio").
 *
 *  A flight's radio voices are a bank on the CD, VOICE\<name>\<name>.LFD (fsfx_loadvoicelfd in OpenTIE): an RMAP
 *  resource whose directory has a VOIC entry (a name, a size) for each clip, the clips after it one by one, each its
 *  entry again and a Creative Voice File.  The game gives the clips to the mission's slots that have text, in order:
 *  its radio messages (0..15), then its first and second win messages and its loss message.  The bank's clips are
 *  named for their slots (the developers' MAKEVOCE.BAT on the CD): <name>r<k> message k - 1 (a letter after it now and
 *  then), <name>w1, w2 and l1.  A chamber mission h<row><n> flies <ROW>M<n>, a battle's b<b>m<m> <b>M<m>.
 *
 *  The challenge's bank is made at its launch from the player's own CD: for each of its lines with text the clip of
 *  the mission the line is from (another mission's for a line replaced, its own for the rest), then its own win and
 *  loss clips; written to the game's drive as Z95VOICE.LFD.  The challenge's voice ids then name the bank GMZ (the
 *  gunboat's row, a mission past the digits), and the game's open of VOICE\GMZ\GMZ.LFD, on any drive, is that file
 *  (tie_voice_open, from DOS_OpenFile).  A clip not found is a tenth of a second of silence, so the others keep their
 *  slots.  Nothing of the CD's is in the program: only which mission's message each line is (wcnet_tie_session.cpp).
 */
#include <ctype.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>
#include "dosbox.h"
#include "dos_inc.h"
#include "wcnet_tie.h"
#include "wcnet_log.h"

namespace wc {

namespace {

const char kMadeName[] = "Z95VOICE.LFD";             // the bank made, on the game's drive
const char kOpenName[] = "VOICE\\GMZ\\GMZ.LFD";      // what the challenge's voice ids name (G, Z: tie_voice_ids)
bool g_made;

Bit32u le32(const Bit8u *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((Bit32u)p[3] << 24); }
void put32(std::vector<Bit8u> &v, Bit32u x) {
    for (int i = 0; i < 4; i++) v.push_back((Bit8u)(x >> (8 * i)));
}

// A file of whichever drive has it (the CD's VOICE\ is on its drive only), read in parts.
class DriveFile {
public:
    explicit DriveFile(const std::string &dosName) : f_(NULL) {
        std::vector<char> n(dosName.begin(), dosName.end());
        n.push_back(0);
        for (int d = 0; d < DOS_DRIVES && !f_; d++) {
            DOS_File *f = NULL;
            if (Drives[d] && Drives[d]->FileOpen(&f, &n[0], OPEN_READ) && f) {
                f->AddRef();                // (as DOS_OpenFile does: Close closes at one reference)
                f_ = f;
            }
        }
    }
    ~DriveFile() {
        if (f_) {
            f_->Close();
            if (f_->RemoveRef() <= 0) delete f_;
        }
    }
    bool read(Bit32u at, Bit32u size, std::vector<Bit8u> &out) {
        Bit32u pos = at;
        if (!f_ || !f_->Seek(&pos, DOS_SEEK_SET) || pos != at) return false;
        out.resize(size);
        for (Bit32u got = 0; got < size;) {
            Bit16u n = (Bit16u)std::min<Bit32u>(size - got, 0x8000);
            if (!f_->Read(&out[got], &n) || !n) return false;
            got += n;
        }
        return true;
    }

private:
    DriveFile(const DriveFile &);
    DriveFile &operator=(const DriveFile &);
    DOS_File *f_;
};

struct Clip {
    std::string name;       // its entry's name, lower case (gm1r3)
    Bit32u at, size;        // the Creative Voice File's place in the bank and its size
};

std::string upper(std::string s) {
    for (size_t i = 0; i < s.size(); i++) s[i] = (char)toupper((unsigned char)s[i]);
    return s;
}
std::string lower(std::string s) {
    for (size_t i = 0; i < s.size(); i++) s[i] = (char)tolower((unsigned char)s[i]);
    return s;
}

// The bank a mission's file flies ("hg1w": GM1, "b10m1gw": 10M1); empty if neither.
std::string bank_of(const std::string &mission) {
    const std::string m = lower(mission);
    if (m.size() >= 3 && m[0] == 'h' && strchr("fibagdm", m[1]) && m[1] && isdigit((unsigned char)m[2])) {
        return upper(m.substr(1, 1)) + "M" + m.substr(2, 1);
    }
    size_t i = 1, j;
    if (m.size() < 4 || m[0] != 'b' || !isdigit((unsigned char)m[1])) return std::string();
    while (i < m.size() && isdigit((unsigned char)m[i])) i++;
    if (i >= m.size() || m[i] != 'm') return std::string();
    for (j = i + 1; j < m.size() && isdigit((unsigned char)m[j]); j++) {}
    return j > i + 1 ? m.substr(1, i - 1) + "M" + m.substr(i + 1, j - i - 1) : std::string();
}

// A bank of the CD, its clips; false if not read.
bool read_bank(const std::string &bank, std::vector<Clip> &clips) {
    DriveFile f("VOICE\\" + bank + "\\" + bank + ".LFD");
    std::vector<Bit8u> h, dir;
    if (!f.read(0, 16, h) || memcmp(&h[0], "RMAP", 4)) return false;
    const Bit32u dirSize = le32(&h[12]);
    if (!dirSize || dirSize > 0x4000 || dirSize % 16 || !f.read(16, dirSize, dir)) return false;
    clips.clear();
    Bit32u at = 16 + dirSize;
    for (Bit32u i = 0; i < dirSize; i += 16) {
        Clip c;
        for (int k = 0; k < 8 && dir[i + 4 + k]; k++) c.name += (char)tolower(dir[i + 4 + k]);
        c.size = le32(&dir[i + 12]);
        c.at = at + 16;
        at = c.at + c.size;
        clips.push_back(c);
    }
    return true;
}

// Its clip for radio message `index`: <bank>r<index + 1>, a letter after it or none.
const Clip *message_clip(const std::string &bank, const std::vector<Clip> &clips, int index) {
    char want[24];
    snprintf(want, sizeof(want), "%sr%d", lower(bank).c_str(), index + 1);
    const size_t n = strlen(want);
    for (size_t i = 0; i < clips.size(); i++) {
        const std::string &c = clips[i].name;
        if (!c.compare(0, n, want) && (c.size() == n || (c.size() == n + 1 && isalpha((unsigned char)c[n])))) {
            return &clips[i];
        }
    }
    return NULL;
}

// A tenth of a second of silence (11,111 samples a second, as the game's clips): a Creative Voice File of our own.
void silence(std::vector<Bit8u> &v) {
    static const char kMagic[] = "Creative Voice File\x1A";
    const Bit32u n = 1111;
    v.assign(kMagic, kMagic + 20);
    const Bit8u head[] = { 0x1A, 0, 0x0A, 0x01, 0x29, 0x11, 1, (Bit8u)((n + 2) & 0xFF), (Bit8u)((n + 2) >> 8), 0, 0xA6, 0 };
    v.insert(v.end(), head, head + sizeof(head));
    v.insert(v.end(), n, 0x80);
    v.push_back(0);
}

}  // namespace

bool tie_voice_make(const std::string &own, const std::vector<TieVoiceLine> &lines) {
    g_made = false;
    const std::string ownBank = bank_of(own);
    std::vector<Clip> ownClips;
    if (ownBank.empty() || !read_bank(ownBank, ownClips)) {
        wclog(1, "TIE: the Z-95 challenge's voices: %s's bank %s not read (no CD?): no voices", own.c_str(), ownBank.c_str());
        return false;
    }
    std::vector<std::string> names;
    std::vector<std::vector<Bit8u> > voc;
    int others = 0, silent = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string bank = bank_of(lines[i].mission);
        std::vector<Clip> clips;
        const Clip *c = NULL;
        if (bank == ownBank) {
            c = message_clip(bank, ownClips, lines[i].index);
        } else if (!bank.empty() && read_bank(bank, clips)) {
            c = message_clip(bank, clips, lines[i].index);
        }
        voc.push_back(std::vector<Bit8u>());
        if (c && DriveFile("VOICE\\" + bank + "\\" + bank + ".LFD").read(c->at, c->size, voc.back())) {
            names.push_back(c->name);
            others += bank != ownBank;
        } else {
            silence(voc.back());
            names.push_back("silence");
            silent++;
            wclog(1, "TIE: the Z-95 challenge's voices: no clip for %s's message %d", lines[i].mission.c_str(), lines[i].index);
        }
    }
    // (then its win and loss clips, as the bank has them)
    const std::string prefix = lower(ownBank);
    for (size_t i = 0; i < ownClips.size(); i++) {
        const Clip &c = ownClips[i];
        if (c.name.size() > prefix.size() && (c.name[prefix.size()] == 'w' || c.name[prefix.size()] == 'l')) {
            voc.push_back(std::vector<Bit8u>());
            if (!DriveFile("VOICE\\" + ownBank + "\\" + ownBank + ".LFD").read(c.at, c.size, voc.back())) {
                silence(voc.back());
            }
            names.push_back(c.name);
        }
    }
    std::vector<Bit8u> bank;
    const char kHead[] = "RMAPresource";
    bank.insert(bank.end(), kHead, kHead + 12);
    put32(bank, (Bit32u)voc.size() * 16);
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < voc.size(); i++) {
            const char kTag[] = "VOIC";
            bank.insert(bank.end(), kTag, kTag + 4);
            for (size_t k = 0; k < 8; k++) bank.push_back(k < names[i].size() ? (Bit8u)names[i][k] : 0);
            put32(bank, (Bit32u)voc[i].size());
            if (pass) bank.insert(bank.end(), voc[i].begin(), voc[i].end());
        }
    }
    if (!tie_game_write(kMadeName, bank)) {
        wclog(1, "TIE: the Z-95 challenge's voices: %s not written: no voices", kMadeName);
        return false;
    }
    g_made = true;
    wclog(1, "TIE: the Z-95 challenge's voices: %u clips from the CD (%d of other missions', %d silent), %u KB in %s",
          (unsigned)voc.size(), others, silent, (unsigned)(bank.size() / 1024), kMadeName);
    return true;
}

bool tie_voice_made() {
    return g_made;
}

bool tie_voice_open(char *fullname, Bit8u *drive) {
    if (!g_made || strcasecmp(fullname, kOpenName)) {
        return false;
    }
    const int d = tie_game_drive();
    if (d < 0) {
        return false;
    }
    strcpy(fullname, kMadeName);
    *drive = (Bit8u)d;
    wclog(2, "TIE: the game's %s is the challenge's own, %c:\\%s", kOpenName, 'A' + d, kMadeName);
    return true;
}

}  // namespace wc
