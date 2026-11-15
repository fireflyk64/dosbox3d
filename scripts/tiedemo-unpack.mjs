// The TIE Fighter demo's two disks (TIEDEMO.001, TIEDEMO.002 from tiedem1.zip and tiedem2.zip) unpacked into a folder
// as its installer lays them out (web/pvm.js), plus the disk's own loose files (READ.ME, FLIGHT.HLP, DETECT.EXE,
// INSTALL.EXE).  docs/tiedemo.md.
//
//   node scripts/tiedemo-unpack.mjs DISK1_DIR DISK2_DIR OUT_DIR
import fs from "node:fs";
import path from "node:path";
import { unpackPvm, pvmDirectory } from "../web/pvm.js";
const [d1, d2, out] = process.argv.slice(2);
const v1 = new Uint8Array(fs.readFileSync(path.join(d1, "TIEDEMO.001")));
const v2 = new Uint8Array(fs.readFileSync(path.join(d2, "TIEDEMO.002")));
const t0 = Date.now();
const dir = pvmDirectory(v1);
console.log(`directory: ${dir.entries.length} files, the first disk's data ${dir.dataEnd} bytes`);
const files = unpackPvm([v1, v2]);
for (const f of files) {
  const p = path.join(out, f.path);
  fs.mkdirSync(path.dirname(p), { recursive: true });
  fs.writeFileSync(p, f.data);
}
for (const loose of ["READ.ME", "FLIGHT.HLP", "DETECT.EXE", "INSTALL.EXE"]) {
  const src = path.join(d1, loose);
  if (fs.existsSync(src)) fs.copyFileSync(src, path.join(out, loose));
}
console.log(`${files.length} files unpacked in ${Date.now() - t0} ms into ${out}`);
