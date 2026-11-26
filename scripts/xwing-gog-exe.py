#!/usr/bin/env python3
"""X-Wing Collector's CD laid out as GOG's installer of it, from your own CD: a test installer for the page.

GOG sells the 1994 Collector's CD-ROM as "STAR WARS: X-Wing Collector's CD (1994)" (product 1207667213, installed in
"Star Wars - X-Wing CD (1994)/").  Its Galaxy build of 2025-04-10 ("1.01 GOG V3"; gogdb.org lists its depots) holds,
at the top of the game's folder, the 55 files a hard-disk install of the CD leaves (BWING.EXE, BFLIGHT.OVL,
BFRONT.OVL, the sound drivers, the .CFG and .XID files, XWINGCD.BAT ...) with game.gog (the disc, 90 MB: an image
of raw 2352-byte sectors, as TIE Fighter's game.gog is) and game.ins (its cue sheet), GOG's DOSBox settings in
__support/app (dosbox_xwingcd.conf, dosbox_xwingcd_single.conf), the pilot TOPACE5.PLT in __support/save, and GOG's
goggame-1207667213.* files; its offline installer adds GOG's DOSBox (DOSBOX/) and its own furniture as TIE
Fighter's does.  The same as TIE Fighter's Collector's CD, except that TIE's installer also puts the disc's
folders beside the game (MISSION/, RESOURCE/ ...); X-Wing's leaves them in game.gog.

This writes that installer (scripts/innosetup.py: GOG's make of installer, read off its TIE Fighter one) from:
  --hd DIR    the installed game's folder from the old days (xwingcd/XWING): the hard-disk files GOG has, those
              it has not (saved pilots, replays) left out; any it lacks are taken from the disc's top
  --iso FILE  the disc's image (2048-byte sectors, or raw 2352): game.gog, made raw 2352-byte MODE1 sectors (sync and
              header written, EDC/ECC left zero: nothing here reads them) unless --cooked
GOG's own files it cannot have (its DOSBox, the launcher's files, the EULAs, EMUSET.EXE and MEGAEM.EXE, which are not
on this CD) are placeholders or left out: what matters is where everything lies and how it is packed.

  scripts/xwing-gog-exe.py --hd xwingcd/XWING --iso xwingcd/xwing.iso -o xwing-gog-test.exe
  options: --image-name NAME (game.gog), --subdir DIR (the game's files one folder down, the image with them),
           --image-at DIR (the image and its cue sheet in that folder instead, "" the top), --cooked (game.gog of
           2048-byte sectors), --verify (innoextract lists, tests and unpacks it as the page does)
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from innosetup import Installer  # noqa: E402

GAME_ID = "1207667213"
# The game's folder in GOG's depot (en-US, 2025-04-10), besides game.gog and game.ins
GOG_HD = ("AWEMID.MDR AWEWAV.WDR BFLIGHT.OVL BFRONT.OVL BOOTDISK.EXE BWING.EXE CORLINST.BAT DEMOS.EXE DETECT.EXE "
          "EMUSET.EXE FASTNO.CFG FASTYES.CFG FM2.MDR FM4.MDR GMIDI.MDR IMUSE.ENG INSTALL.EXE LANDRU.CFG MEDNO.CFG "
          "MEDYES.CFG MEGAEM.EXE MICRO.FNT OPTIONS.CFG PAS.WDR READ.ME ROLAND.MDR SB16.WDR SB2CLONE.WDR SBCLONE.WDR "
          "SBPRO.WDR SETMUSE.EXE SETMUSE.INI SETMUSE.NON SHIP1.XID SHIP2.XID SHIP3.XID SHIP4.XID SHIP5.XID SLOWNO.CFG "
          "SLOWYES.CFG SMALL.FNT SNDSCAPE.MDR SNDSCAPE.WDR TESTMIDI TESTWAVE TINY.FNT TOUR1.XID TOUR2.XID TOUR3.XID "
          "TOUR4.XID TOUR5.XID VGA.PAC XWINGCD.001 XWINGCD.BAT XWINGCD2.ADV").split()


def iso_geometry(f):
    for ss, hdr in ((2048, 0), (2352, 16), (2352, 24)):
        f.seek(16 * ss + hdr)
        if f.read(6) == b"\x01CD001":
            return ss, hdr
    return None


def iso_top(path):
    """The files at the top of an ISO 9660 image: {NAME: bytes}."""
    out = {}
    with open(path, "rb") as f:
        geo = iso_geometry(f)
        if not geo:
            sys.exit(f"{path}: not an ISO 9660 image")
        ss, hdr = geo
        def read(sector, size):
            buf = bytearray()
            for k in range((size + 2047) // 2048):
                f.seek((sector + k) * ss + hdr)
                buf += f.read(2048)
            return bytes(buf[:size])
        pvd = read(16, 2048)
        ext, size = struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0]
        root = read(ext, size)
        i = 0
        while i < len(root):
            if not root[i]:
                i = (i // 2048 + 1) * 2048
                continue
            name = root[i + 33:i + 33 + root[i + 32]].decode("latin1").split(";")[0].rstrip(".").upper()
            if not root[i + 25] & 2 and len(name) > 1:
                out[name] = read(struct.unpack_from("<I", root, i + 2)[0], struct.unpack_from("<I", root, i + 10)[0])
            i += root[i]
    return out


def raw_image(path, cooked):
    """The disc as game.gog: raw 2352-byte MODE1 sectors (sync, header; EDC/ECC zero), or the 2048-byte sectors."""
    with open(path, "rb") as f:
        ss, hdr = iso_geometry(f)
        f.seek(0)
        whole = f.read()
    sectors = [whole[i + hdr:i + hdr + 2048] for i in range(0, len(whole) - ss + 1, ss)]
    if cooked:
        return b"".join(sectors)
    bcd = lambda n: (n // 10) << 4 | n % 10
    out = bytearray()
    sync = b"\x00" + b"\xff" * 10 + b"\x00"
    for lba, data in enumerate(sectors):
        a = lba + 150
        out += sync + bytes([bcd(a // 4500), bcd(a // 75 % 60), bcd(a % 75), 1]) + data + bytes(288)
    return bytes(out)


def placeholder(what):
    return (f"Placeholder in a test build of the page's (dosbox3d) GOG-layout X-Wing installer: GOG's {what} is not "
            "part of it.\r\n").encode()


def conf(single):
    if single:
        return ("[autoexec]\r\n@echo off\r\nmount c \"..\"\r\nimgmount d \"..\\game.gog\" -t iso\r\nc:\r\n"
                "xwingcd.bat\r\nexit\r\n").encode()
    return ("# Placeholder for GOG's DOSBox settings (test build)\r\n[sdl]\r\nfullscreen=false\r\n[dosbox]\r\n"
            "machine=svga_s3\r\nmemsize=16\r\n[cpu]\r\ncore=auto\r\ncycles=auto\r\n[sblaster]\r\nsbtype=sb16\r\n").encode()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--hd", help="the installed game's folder (its hard-disk files)")
    ap.add_argument("--iso", required=True, help="the disc's image")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--image-name", default="game.gog")
    ap.add_argument("--subdir", default="", help="the game's files (and the image) one folder down")
    ap.add_argument("--image-at", default=None, help="the image's folder when not the game's (\"\" = the top)")
    ap.add_argument("--cooked", action="store_true", help="game.gog of 2048-byte sectors")
    ap.add_argument("--verify", action="store_true", help="list, test and unpack it with innoextract as the page does")
    a = ap.parse_args()

    top = iso_top(a.iso)
    hd = {}
    if a.hd:
        for n in os.listdir(a.hd):
            p = os.path.join(a.hd, n)
            if os.path.isfile(p):
                hd[n.upper()] = p
    if "BWING.EXE" not in top and "BWING.EXE" not in hd:
        sys.exit("no BWING.EXE on the disc's top or in --hd: not the X-Wing Collector's CD")

    inst = Installer(app_name="X-Wing Collector's CD (1994): GOG's layout, a test build from your own CD",
                     app_id="dosbox3d-xwing-gog-layout-test", app_version="test",
                     publisher="dosbox3d test build", default_dir="{autopf}\\X-Wing CD (1994) test")
    sub = a.subdir.strip("/").replace("/", "\\") + "\\" if a.subdir else ""
    at = a.subdir if a.image_at is None else a.image_at
    img = at.strip("/").replace("/", "\\") + "\\" if at else ""
    said = []
    # GOG's redistributables first, as its installers have them
    inst.dependency("__redist/ISI/scriptinterpreter.exe", placeholder("script interpreter"), "9dd97f16719932b0bdd6fc98a8dd9296")
    for n in ("dosbox-0.74-2.1.tar.gz", "DOSBox 0.74-2 Manual.txt", "GOGDOSConfig.exe", "DOSBox.exe", "SDL_net.dll", "SDL.dll",
              "Documentation/README.txt"):
        inst.dependency(f"DOSBOX/{n}", placeholder(f"DOSBox ({n})"), "f6e534c171c324e4ac7abb8e0f900770")
    # the disc: game.gog and its cue sheet
    t0 = time.time()
    image = raw_image(a.iso, a.cooked)
    mode = "MODE1/2048" if a.cooked else "MODE1/2352"
    cue = f'FILE "{a.image_name}" BINARY\r\n  TRACK 01 {mode}\r\n    INDEX 01 00:00:00\r\n'.encode()
    inst.game(img + "game.ins", cue, "en-US#de-DE#")
    for ext in ("info", "script", "hashdb"):
        inst.game(f"goggame-{GAME_ID}.{ext}", ('{\r\n    "gameId": "%s",\r\n    "file": "goggame-%s.%s",\r\n    "note": "placeholder in '
                                               'a test build: not GOG\'s file"\r\n}\r\n' % (GAME_ID, GAME_ID, ext)).encode())
    # the hard-disk files GOG has, in its order (yours where you have them, else the disc's)
    missing = []
    for n in GOG_HD:
        if n in hd:
            data, mtime = open(hd[n], "rb").read(), os.path.getmtime(hd[n])
        elif n in top:
            data, mtime = top[n], None
        else:
            missing.append(n)
            continue
        inst.game(sub + n, data, mtime=mtime)
    inst.game(img + a.image_name, image, mtime=os.path.getmtime(a.iso))
    said.append(f"{a.image_name}: {len(image):,} bytes ({mode}) from {a.iso} in {time.time() - t0:.1f} s")
    if missing:
        said.append(f"not on this CD nor in --hd, left out: {', '.join(missing)}")
    left = sorted(set(hd) - set(GOG_HD) - {"TOPACE5.PLT"})
    if left:
        said.append(f"of --hd, not in GOG's folder, left out: {', '.join(left)}")
    # GOG's support files
    inst.game("__support/app\\dosbox_xwingcd.conf", conf(False), "en-US#de-DE#")
    inst.game("__support/app\\dosbox_xwingcd_single.conf", conf(True), "en-US#de-DE#")
    pilot = open(hd["TOPACE5.PLT"], "rb").read() if "TOPACE5.PLT" in hd else top.get("TOPACE5.PLT")
    if pilot:
        inst.game("__support/save\\TOPACE5.PLT", pilot, "en-US#de-DE#")
    # the installer's furniture: temporary files, the launcher's, GOG's support DLL
    for n in ("background.jpg", "EULA_de-DE.txt", "EULA_fr-FR.txt", "botva2.dll", "InnoCallback.dll", "md5log.ini"):
        inst.temp(n, placeholder(f"installer file {n}"))
    inst.app(f"goggame-{GAME_ID}.ico", placeholder("icon"))
    inst.app("webcache.zip", b"PK\x05\x06" + bytes(18))
    inst.common("GOG.com\\supportInstaller\\uninstall.dll", placeholder("support installer"))

    t1 = time.time()
    size = inst.write(a.out)
    said.append(f"{a.out}: {size:,} bytes, {len(inst.files)} file entries, {len(inst.data)} chunks, written in {time.time() - t1:.1f} s")
    print("\n".join(said))
    if a.verify:
        verify(a.out, a.image_name, a.subdir, at, image, hd, top)


def verify(exe, image_name, subdir, image_at, image, hd, top):
    """innoextract (1.9, as the page's) lists it, tests every checksum, and unpacks it as the page does."""
    common = ["--gog", "--exclude-temp", "--language", "en-US"]
    listing = subprocess.run(["innoextract", "--list", *common, exe], capture_output=True, text=True)
    print(listing.stdout.strip()[-1500:])
    if listing.returncode or "Warning" in listing.stdout + listing.stderr:
        sys.exit(f"innoextract --list: status {listing.returncode}\n{listing.stderr}")
    test = subprocess.run(["innoextract", "--test", *common, exe], capture_output=True, text=True)
    if test.returncode or "Warning" in test.stdout + test.stderr:
        sys.exit(f"innoextract --test: status {test.returncode}\n{test.stderr}")
    with tempfile.TemporaryDirectory() as out:
        run = subprocess.run(["innoextract", "--extract", "--quiet", *common, "--output-dir", out, exe], capture_output=True, text=True)
        if run.returncode:
            sys.exit(f"innoextract --extract: status {run.returncode}\n{run.stderr}")
        base = os.path.join(out, subdir) if subdir else out
        got = open(os.path.join(out, image_at, image_name) if image_at else os.path.join(out, image_name), "rb").read()
        assert hashlib.md5(got).digest() == hashlib.md5(image).digest(), "the image unpacked differs"
        bw = open(os.path.join(base, "BWING.EXE"), "rb").read()
        want = open(hd["BWING.EXE"], "rb").read() if "BWING.EXE" in hd else top["BWING.EXE"]
        assert bw == want, "BWING.EXE unpacked differs"
        names = sorted(os.path.relpath(os.path.join(d, n), out) for d, _, ns in os.walk(out) for n in ns)
        assert not any(n.startswith("tmp/") for n in names), "temporary files were unpacked"
        print(f"innoextract: listed, tested and unpacked ({len(names)} files; {image_name} and BWING.EXE byte for byte)")


if __name__ == "__main__":
    main()
