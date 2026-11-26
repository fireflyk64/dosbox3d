#!/usr/bin/env python3
"""Inno Setup installers written the way GOG builds its offline installers, for the page's tests.

The page unpacks a GOG installer in the browser with innoextract (web/inno-worker.js: --gog --exclude-temp
--language en-US).  To try that path with a game nobody has GOG's installer of yet, this writes an installer of the
same make from files we do have: the setup data of Inno Setup 5.6.2 (unicode), the version GOG's TIE Fighter
Collector's CD installer (setup_star_warstm_tie_fighter_collectors_cd_(1995)_1.0_gog_v2_(81631).exe) carries, laid
out as GOG lays it out, read off that installer's headers:

- every game file is a "GOG Galaxy" file: cut into parts of 8 MiB, each part deflated (zlib) and stored in a chunk
  of its own (no Inno compression on top), installed under {tmp} by the MD5 of its bytes; the real name is in the
  first part's `before_install('<md5 of the file>', '<path>', <parts>)`, each part's sizes in its
  `after_install('<md5 of the part>', <deflated>, <size>)`, its languages in `check_if_install('en-US#', '32#64#', '')`
  (innoextract --gog puts the parts back together and turns the languages into Inno's own);
- GOG's redistributables (its DOSBox, its script interpreter) are Galaxy files too, `..._dependency` calls;
- the installer's own furniture (EULAs, pictures, DLLs) are plain Inno files under {tmp}, delete-after-install and
  don't-copy, LZMA2-compressed; a few under {app} ({app}\\webcache.zip) and {commonappdata}.

The loader in front is not GOG's (nor Inno's): a PE32 file of headers and one resource, RCDATA 11111, the setup
loader's offset table (rDlPtS, revision 1) that innoextract looks for.  The file is an installer to innoextract, not
to Windows (it holds no program).  Reading formats: innoextract 1.9's sources (setup/header.cpp, setup/file.cpp,
setup/data.cpp, stream/block.cpp, cli/goggalaxy.cpp).

    from innosetup import Installer
    inst = Installer(app_name="...", app_id="...")
    inst.game("BWING.EXE", data)                       # a Galaxy file (languages "en-US#" unless given)
    inst.dependency("DOSBOX/DOSBox.exe", data, "f6e534c171c324e4ac7abb8e0f900770")
    inst.temp("EULA_de-DE.txt", data)                  # {tmp}, excluded by --exclude-temp
    inst.app("webcache.zip", data)                     # {app}\\..., innoextract's "app/..."
    inst.write("out.exe")
"""
import hashlib
import lzma
import os
import struct
import time
import zlib

VERSION = b"Inno Setup Setup Data (5.6.2) (u)"
LOADER_MAGIC = b"rDlPtS\xcd\xe6\xd7{\x0b*"       # setup loader offset table, 5.1.5 and later
PART = 8 * 1024 * 1024                           # GOG Galaxy part size
FILETIME_EPOCH = 116444736000000000              # 1601-01-01 to 1970-01-01 in 100 ns

# The numbers after the header's strings and counts, as GOG's TIE Fighter installer has them (93 bytes):
WINVER = bytes(4) + bytes([0, 0, 0, 5]) + bytes(2) + bytes(10)    # from NT 5.0 (Windows 2000), no end
HEADER_TAIL = (WINVER
               + struct.pack("<II", 0xFF0000, 0)       # back colours
               + bytes([0])                            # image alpha format: ignored
               + bytes(20) + bytes(8)                  # no password (SHA-1, salt)
               + struct.pack("<qI", 0, 1)              # extra disk space, slices per disk
               + bytes([0, 0, 2, 0, 1, 4])             # uninstall log: new, dir-exists warning: auto, privileges:
                                                       # admin, language dialog: yes, detection: UI, compression: LZMA2
               + bytes([0, 0, 0, 0])                   # architectures allowed / 64-bit, dir page, group page: auto
               + struct.pack("<Q", 0)                  # uninstall display size
               + bytes.fromhex("c3017b142f0e"))        # the setup's options (45 flags) as GOG sets them
assert len(HEADER_TAIL) == 93

# File entry options (32 flags, in innoextract's order for 5.6.2): the ones used here
F_NEVER_UNINSTALL = 1 << 1
F_DELETE_AFTER_INSTALL = 1 << 3          # innoextract's [temp]
F_OVERWRITE_SAME_VERSION = 1 << 11
F_CUSTOM_DEST_NAME = 1 << 12
F_ONLY_IF_DOESNT_EXIST = 1 << 16
F_DONT_COPY = 1 << 19
# Data entry options (11 flags)
D_VERSION_INFO_NOT_VALID = 1 << 1
D_CHUNK_COMPRESSED = 1 << 7


def _w(s):
    b = s.encode("utf-16-le")
    return struct.pack("<I", len(b)) + b


def _a(b):
    return struct.pack("<I", len(b)) + b


def _crc(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def _block(raw, compress=True):
    """A setup-0 block: CRC of (size, compressed), then the stream in 4 KiB pieces, each after its CRC."""
    if compress:
        dict_size = 1 << 21
        props = (2 * 5 + 0) * 9 + 3                    # pb 2, lp 0, lc 3: 93
        payload = bytes([props]) + struct.pack("<I", dict_size) + lzma.compress(
            raw, format=lzma.FORMAT_RAW, filters=[{"id": lzma.FILTER_LZMA1, "dict_size": dict_size, "lc": 3, "lp": 0, "pb": 2}])
    else:
        payload = raw
    body = b"".join(struct.pack("<I", _crc(payload[i:i + 4096])) + payload[i:i + 4096] for i in range(0, len(payload), 4096))
    head = struct.pack("<IB", len(body), 1 if compress else 0)
    return struct.pack("<I", _crc(head)) + head + body


def _lzma2(data):
    """An Inno LZMA2 chunk's stream: one byte for the dictionary's size, then raw LZMA2."""
    dict_size = 1 << 21                               # (2 | 0) << (18 / 2 + 11)
    return bytes([18]) + lzma.compress(data, format=lzma.FORMAT_RAW, filters=[{"id": lzma.FILTER_LZMA2, "dict_size": dict_size}])


def _pe(resource):
    """A PE32 file of headers and a .rsrc section holding `resource` as RCDATA 11111 (language 0)."""
    rva = 0x1000
    tree = bytearray()
    tree += struct.pack("<IIHHHH", 0, 0, 0, 0, 0, 1) + struct.pack("<II", 10, 0x80000000 | 24)        # types: RT_RCDATA
    tree += struct.pack("<IIHHHH", 0, 0, 0, 0, 0, 1) + struct.pack("<II", 11111, 0x80000000 | 48)     # names: 11111
    tree += struct.pack("<IIHHHH", 0, 0, 0, 0, 0, 1) + struct.pack("<II", 0, 72)                      # languages: 0
    tree += struct.pack("<IIII", rva + 88, len(resource), 0, 0)                                      # the data
    tree += resource
    raw_size = (len(tree) + 0x1FF) & ~0x1FF
    dos = bytearray(64)
    dos[0:2] = b"MZ"
    struct.pack_into("<HHHHH", dos, 2, 0x90, 3, 0, 4, 0)
    struct.pack_into("<I", dos, 0x3C, 64)
    coff = b"PE\0\0" + struct.pack("<HHIIIHH", 0x14C, 1, int(time.time()), 0, 0, 224, 0x0102)
    opt = bytearray(224)
    struct.pack_into("<HBBIIIIII", opt, 0, 0x10B, 1, 0, 0, raw_size, 0, 0, rva, rva)
    struct.pack_into("<IIIHHHHHHIIIIHHIIIIII", opt, 28, 0x400000, 0x1000, 0x200, 4, 0, 0, 0, 4, 0, 0,
                     rva + ((len(tree) + 0xFFF) & ~0xFFF), 0x200, 0, 2, 0, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    struct.pack_into("<II", opt, 96 + 2 * 8, rva, len(tree))                                         # resource directory
    section = struct.pack("<8sIIIIIIHHI", b".rsrc", len(tree), rva, raw_size, 0x200, 0, 0, 0, 0, 0x40000040)
    head = bytes(dos) + coff + bytes(opt) + section
    return head + bytes(0x200 - len(head)) + bytes(tree) + bytes(raw_size - len(tree))


class Installer:
    def __init__(self, app_name, app_id, app_version="test", publisher="", default_dir="{autopf}\\Game"):
        self.strings = dict(app_name=app_name, app_versioned_name=app_name, app_id=app_id, publisher=publisher,
                            app_version=app_version, default_dir=default_dir)
        self.files = []        # (entry fields...)
        self.data = []         # (chunk bytes, file size, sha1, flags, mtime)
        # GOG's first entry: the uninstaller, no data of its own
        self.files.append(dict(dest="", cond=[""] * 6, location=-1, flags=F_OVERWRITE_SAME_VERSION, type=1))

    def _galaxy(self, path, data, check, after, before, mtime):
        whole = hashlib.md5(data).hexdigest()
        parts = [data[i:i + PART] for i in range(0, len(data), PART)] or [b""]
        for n, part in enumerate(parts):
            packed = zlib.compress(part, 6)
            name = hashlib.md5(packed).hexdigest()
            loc = len(self.data)
            self.data.append((packed, len(packed), hashlib.sha1(packed).digest(), D_VERSION_INFO_NOT_VALID, mtime))
            cond = ["", "", "", check, f"{after}('{hashlib.md5(part).hexdigest()}', {len(packed)}, {len(part)})",
                    f"{before}('{whole}', '{path}', {len(parts)})" if n == 0 else ""]
            self.files.append(dict(dest=f"{{tmp}}/{name[0:2]}/{name[2:4]}\\{name}", cond=cond, location=loc, flags=0, type=0))

    def game(self, path, data, languages="en-US#", mtime=None):
        """A file of the game: GOG Galaxy parts, languages as GOG writes them ("en-US#", "en-US#de-DE#")."""
        self._galaxy(path, data, f"check_if_install('{languages}','32#64#','')", "after_install", "before_install", mtime)

    def dependency(self, path, data, dep_id, mtime=None):
        """A GOG redistributable (its DOSBox, its script interpreter): Galaxy parts of a dependency."""
        self._galaxy(path, data, f"check_if_install_dependency('{dep_id}', False)", "after_install_dependency",
                     "before_install_dependency", mtime)

    def _plain(self, dest, data, flags, mtime):
        loc = len(self.data)
        self.data.append((_lzma2(data), len(data), hashlib.sha1(data).digest(), D_VERSION_INFO_NOT_VALID | D_CHUNK_COMPRESSED, mtime))
        self.files.append(dict(dest=dest, cond=[""] * 6, location=loc, flags=flags, type=0))

    def temp(self, name, data, mtime=None):
        """One of the installer's own files ({tmp}: delete after install, don't copy)."""
        self._plain(f"{{tmp}}\\{name}", data, F_NEVER_UNINSTALL | F_DELETE_AFTER_INSTALL | F_DONT_COPY, mtime)

    def app(self, path, data, mtime=None):
        """A plain file under {app} (GOG's webcache.zip, its icon): innoextract's app/..."""
        self._plain(f"{{app}}\\{path}", data, F_ONLY_IF_DOESNT_EXIST, mtime)

    def common(self, path, data, mtime=None):
        """A plain file under {commonappdata} (GOG's support installer's DLL)."""
        self._plain(f"{{commonappdata}}\\{path}", data, F_NEVER_UNINSTALL | F_ONLY_IF_DOESNT_EXIST, mtime)

    def _header(self):
        s = self.strings
        out = bytearray()
        for v in [s["app_name"], s["app_versioned_name"], s["app_id"], "", s["publisher"], "", "", "", "", s["app_version"],
                  s["default_dir"], "", "", "{app}", "", "{app}\\gog.ico", "", "{sysuserinfoname}", "{sysuserinfoorg}", "",
                  "", "", "", "", "yes", "yes", "*.exe,*.dll,*.chm", "", "no", "no"]:
            out += _w(v)
        out += _a(b"") * 3          # license, info before, info after (ANSI)
        out += _a(b"")              # compiled code: none
        counts = dict(language=2, message=0, permission=0, type=0, component=0, task=0, directory=1,
                      file=len(self.files), data=len(self.data))
        out += struct.pack("<16I", counts["language"], counts["message"], counts["permission"], counts["type"],
                           counts["component"], counts["task"], counts["directory"], counts["file"], counts["data"],
                           0, 0, 0, 0, 0, 0, 0)
        out += HEADER_TAIL
        for name, shown, lid in (("english", "English", 1033), ("german", "Deutsch", 1031)):
            out += _w(name) + _w(shown) + _w("Tahoma") + _w("Arial") + _w("Verdana") + _w("Arial")
            out += _a(b"") + _a(b"") + _a(b"") + _a(b"")          # messages data, license, info before / after
            out += struct.pack("<IIIIIB", lid, 8, 29, 12, 8, 0)
        out += _w("{app}") + _w("") * 6 + struct.pack("<I", 0) + WINVER + struct.pack("<hB", -1, 0)    # [Dirs] {app}
        for f in self.files:
            out += _w("") + _w(f["dest"]) + _w("") + _w("")       # source, destination, font, assembly
            out += b"".join(_w(c) for c in f["cond"])             # components, tasks, languages, check, after, before
            out += WINVER + struct.pack("<iIQhIB", f["location"], 0, 0, -1, f["flags"], f["type"])
        out += struct.pack("<I", 0) + struct.pack("<I", 0)        # wizard images, small images: none
        return bytes(out)

    def write(self, path):
        stamp = int(time.time())
        chunks, entries = bytearray(), bytearray()
        for packed, size, sha1, flags, mtime in self.data:
            at = len(chunks)
            chunks += b"zlb\x1a" + packed
            ft = int((mtime if mtime is not None else stamp) * 10_000_000) + FILETIME_EPOCH
            entries += struct.pack("<IIIQQQ", 0, 0, at, 0, size, len(packed)) + sha1 + struct.pack("<qIIH", ft, 0, 0, flags)
        setup0 = VERSION + bytes(64 - len(VERSION)) + _block(self._header()) + _block(bytes(entries))
        # (the loader, then the files' chunks, then the headers, as GOG's installers lie)
        stub_size = len(_pe(bytes(44)))
        data_offset = stub_size
        header_offset = data_offset + len(chunks)
        total = header_offset + len(setup0)
        table = LOADER_MAGIC + struct.pack("<IIIIIII", 1, total, 0, 0, 0, header_offset, data_offset)
        table += struct.pack("<I", _crc(table))
        stub = _pe(table)
        assert len(stub) == stub_size
        with open(path, "wb") as out:
            out.write(stub)
            out.write(chunks)
            out.write(setup0)
        return total


if __name__ == "__main__":
    import sys
    sys.exit("a library: scripts/xwing-gog-exe.py writes an installer with it")
