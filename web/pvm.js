// The TIE Fighter demo's floppies (LucasArts, 1994: tiedem1.zip and tiedem2.zip, each a disk), unpacked in the browser
// the way their installer (INSTALIT, Hi Tech Expressions) unpacks them.  docs/tiedemo.md.
//
// TIEDEMO.001 and TIEDEMO.002 are one stream of files, each compressed with PKWARE's Data Compression Library
// ("implode"), end to end, split across the two disks.  The first disk ends with a directory (from "\x05[PVM]"): a
// record of 55 bytes for each file,
//   +0  0x1C            +1  the file's offset on its first disk (4 bytes)   +5  0xF0
//   +6  its name (13 bytes, zero padded)                                     +19 its DOS attributes
//   +20 its compressed size (4)   +24 its date and time (4)   +28 its CRC-32 (4)
//   +32 its disk, its disk again, 1 (a file running onto the next disk still says its first)   +35 its size (4)   +39 zeros   +51 its folder (2: 1 the game's,
//   2 RESOURCE, 3 CP: the folders after the records, each a length byte and "[INSTALLATIONDIRECTORY]\\CP" (left-over
//   bytes of a longer name after it))   +53 1
// and the disk's data stops where the directory begins (the rest of the disk's file is the directory and the disks'
// names).  A file that runs past the end of the first disk's data goes on at the start of the second's.
//
// explode() is zlib's contrib/blast.c (Mark Adler's decoder of the PKWARE format, zlib licence) in JavaScript.

const MAXBITS = 13;
const LITLEN = [11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8, 9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24,
  12, 11, 7, 9, 11, 12, 6, 7, 22, 5, 7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12, 8, 12, 5, 38, 5,
  38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24, 10, 27, 44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61, 12,
  45, 44, 173];
const LENLEN = [2, 35, 36, 53, 38, 23];
const DISTLEN = [2, 20, 53, 230, 247, 151, 248];
const BASE = [3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264];
const EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8];

// (a canonical code from blast's compact lengths: each byte's low nibble a length, its high nibble the repeats less 1)
function construct(rep) {
  const length = [];
  for (const r of rep) for (let k = (r >> 4) + 1; k > 0; k--) length.push(r & 15);
  const count = new Array(MAXBITS + 1).fill(0);
  for (const l of length) count[l]++;
  const offs = new Array(MAXBITS + 2).fill(0);
  for (let l = 1; l < MAXBITS; l++) offs[l + 1] = offs[l] + count[l];
  const symbol = new Array(length.length).fill(0);
  length.forEach((l, s) => { if (l) symbol[offs[l]++] = s; });
  return { count, symbol };
}
let tables = null;

// One compressed file from `data` at `at`: { out (its bytes), used (the compressed bytes read) }.  `size`, when known,
// is its length (the directory's); the stream's own end code ends it either way.
export function explode(data, at = 0, size = 0) {
  if (!tables) tables = { lit: construct(LITLEN), len: construct(LENLEN), dist: construct(DISTLEN) };
  let pos = at, bitbuf = 0, bitcnt = 0;
  const bits = (need) => {
    while (bitcnt < need) {
      if (pos >= data.length) throw new Error("the compressed file ends early");
      bitbuf |= data[pos++] << bitcnt;
      bitcnt += 8;
    }
    const v = bitbuf & ((1 << need) - 1);
    bitbuf >>>= need;
    bitcnt -= need;
    return v;
  };
  const decode = (h) => {            // (the code's bits come inverted, first bit the code's highest)
    let code = 0, first = 0, index = 0;
    for (let len = 1; len <= MAXBITS; len++) {
      code |= bits(1) ^ 1;
      const count = h.count[len];
      if (code - first < count) return h.symbol[index + code - first];
      index += count;
      first = (first + count) << 1;
      code <<= 1;
    }
    throw new Error("a bad code in the compressed file");
  };
  let out = new Uint8Array(size > 0 ? size : 65536), n = 0;
  const room = (k) => {
    if (n + k <= out.length) return;
    const bigger = new Uint8Array(Math.max(out.length * 2, n + k));
    bigger.set(out.subarray(0, n));
    out = bigger;
  };
  const lit = bits(8), dict = bits(8);
  if (lit > 1 || dict < 4 || dict > 6) throw new Error("not a PKWARE-compressed file");
  for (;;) {
    if (bits(1)) {
      const sym = decode(tables.len);
      let len = BASE[sym] + bits(EXTRA[sym]);
      if (len === 519) break;                       // (the end code)
      const db = len === 2 ? 2 : dict;
      const dist = (decode(tables.dist) << db) + bits(db) + 1;
      if (dist > n) throw new Error("a copy from before the file's start");
      room(len);
      for (; len > 0; len--, n++) out[n] = out[n - dist];
    } else {
      room(1);
      out[n++] = lit ? decode(tables.lit) : bits(8);
    }
  }
  return { out: out.subarray(0, n), used: pos - at };
}

const FOLDERS = { 1: "", 2: "RESOURCE/", 3: "CP/" };

// (the directory's check of each file: the CRC-32 of zip and zlib)
let crcTable = null;
export function crc32(bytes) {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
      crcTable[n] = c >>> 0;
    }
  }
  let c = 0xFFFFFFFF;
  for (let i = 0; i < bytes.length; i++) c = crcTable[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

// The directory at the end of a first disk's file, or null: { dataEnd, entries: [{ name, folder, disk, last, offset,
// csize, size }] }
export function pvmDirectory(vol1) {
  const tag = [0x05, 0x5B, 0x50, 0x56, 0x4D, 0x5D];            // "\x05[PVM]"
  let at = -1;
  for (let i = vol1.length - 6; i >= Math.max(0, vol1.length - 65536) && at < 0; i--) {
    if (tag.every((b, k) => vol1[i + k] === b)) at = i;
  }
  if (at < 0) return null;
  const u32 = (o) => (vol1[o] | (vol1[o + 1] << 8) | (vol1[o + 2] << 16)) + vol1[o + 3] * 0x1000000;
  const entries = [];
  for (let r = at + 6; r + 55 <= vol1.length && vol1[r] === 0x1C && vol1[r + 5] === 0xF0 && entries.length < 1024; r += 55) {
    let name = "";
    for (let k = 0; k < 13 && vol1[r + 6 + k]; k++) name += String.fromCharCode(vol1[r + 6 + k]);
    const folder = vol1[r + 51] | (vol1[r + 52] << 8);
    if (!/^[A-Z0-9_$~!#%&'(){}@^`-]{1,8}(\.[A-Z0-9_$~!#%&'(){}@^`-]{1,3})?$/i.test(name) || !(folder in FOLDERS)) break;
    entries.push({ name, folder, offset: u32(r + 1), csize: u32(r + 20), crc: u32(r + 28), disk: vol1[r + 32], size: u32(r + 35) });
  }
  return entries.length ? { dataEnd: at, entries } : null;
}

// The files of the disks' volumes (their data, in disk order: [TIEDEMO.001, TIEDEMO.002]) as { path, data }, under
// `root` ("TIEDEMO/").  Throws when a file does not come out at its directory's size and CRC.
export function unpackPvm(volumes, root = "") {
  const dir = pvmDirectory(volumes[0]);
  if (!dir) throw new Error("no installer directory at the end of the first disk");
  // (each disk's data: the first's stops at its directory; a file is read across the disks' data end to end)
  const data = [volumes[0].subarray(0, dir.dataEnd), ...volumes.slice(1)];
  const starts = [];
  let total = 0;
  for (const d of data) { starts.push(total); total += d.length; }
  const all = new Uint8Array(total);
  data.forEach((d, i) => all.set(d, starts[i]));
  const files = [];
  for (const e of dir.entries) {
    if (e.disk < 1 || e.disk > data.length) throw new Error(`${e.name} is on disk ${e.disk}, which is not here`);
    const { out } = explode(all, starts[e.disk - 1] + e.offset, e.size);
    if (out.length !== e.size) throw new Error(`${e.name} came out ${out.length} bytes, not ${e.size}`);
    if (crc32(out) !== e.crc) throw new Error(`${e.name} came out damaged (its CRC is not the directory's)`);
    files.push({ path: root + FOLDERS[e.folder] + e.name, data: out });
  }
  return files;
}
