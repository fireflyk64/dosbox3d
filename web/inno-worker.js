// Runs innoextract (compiled to WebAssembly, see scripts/build-web.sh) on an
// installer and posts the extracted files back.  Everything happens inside
// this worker; nothing leaves the browser.
//
// The installer is read where it is (a File: pieces of it as innoextract asks
// for them), never held whole: GOG's 35th Anniversary Editions of Wing
// Commander are 1.1 GB and 850 MB.  And of such an installer only the game is
// unpacked: its files are listed first, and when a program the page knows
// (`detect`, the registry's executables) sits in a directory of its own, only
// that directory is extracted (the Anniversary Editions' DOS/, 34 MB: the
// release sold before them, beside a Windows version and a patched one).
import createInnoextract from "./innoextract.js";

// The installer as a file of innoextract's file system, read from the Blob in windows of 4 MB.
function mountBlob(FS, path, blob) {
  const node = FS.lookupPath(path, { parent: true }).node;
  const file = FS.createFile(node, path.slice(path.lastIndexOf("/") + 1), {}, true, false);
  const reader = new FileReaderSync();
  const WINDOW = 4 * 1048576;
  let at = -1, win = null;
  file.usedBytes = blob.size;   // (its size, as the memory file system reports and seeks by it)
  file.stream_ops = {
    llseek: file.stream_ops.llseek,
    read(stream, buffer, offset, length, position) {
      let done = 0;
      while (done < length && position < blob.size) {
        if (!win || position < at || position >= at + win.length) {
          at = position - (position % WINDOW);
          win = new Uint8Array(reader.readAsArrayBuffer(blob.slice(at, Math.min(blob.size, at + WINDOW))));
        }
        const n = Math.min(length - done, at + win.length - position);
        buffer.set(win.subarray(position - at, position - at + n), offset + done);
        done += n; position += n;
      }
      return done;
    },
  };
}

// The directories to extract, of a listing's paths: [] for everything.  The known program nearest the top says
// where the game is; of several as near, the one with GOG's DOSBox configuration beside it (the DOS release), else
// all of them.  Installer furniture is no place for a game.
const FURNITURE = new Set(["dosbox", "__redist", "__support", "commonappdata", "tmp", "app"]);
function gameDirs(paths, detect) {
  const want = new Set(detect.map((d) => d.toUpperCase()));
  const dirOf = (p) => (p.includes("/") ? p.slice(0, p.lastIndexOf("/")) : "");
  let dirs = [...new Set(paths.filter((p) => want.has(p.slice(p.lastIndexOf("/") + 1).toUpperCase())).map(dirOf))]
    .filter((d) => !d.split("/").some((part) => FURNITURE.has(part.toLowerCase())));
  if (!dirs.length) return [];
  const depth = (d) => (d ? d.split("/").length : 0);
  const top = Math.min(...dirs.map(depth));
  if (top === 0) return [];
  dirs = dirs.filter((d) => depth(d) === top);
  const dos = dirs.filter((d) => paths.some((p) => dirOf(p) === d && /^dosbox[^/]*\.conf$/i.test(p.slice(d.length + 1))));
  return dos.length ? dos : dirs;
}

self.onmessage = async (ev) => {
  const { type, blob, detect = [] } = ev.data;
  if (type !== "extract") return;
  const log = (line) => self.postMessage({ type: "log", line });
  try {
    // (a run of innoextract: a module of its own each time, as a program is run once)
    const run = async (args, print) => {
      const Module = await createInnoextract({ print, printErr: log, locateFile: (p) => new URL(p, import.meta.url).href });
      Module.FS.mkdir("/in"); Module.FS.mkdir("/out");
      mountBlob(Module.FS, "/in/setup.exe", blob);
      const status = Module.callMain([...args, "/in/setup.exe"]);
      if (status) throw new Error(`innoextract exited with status ${status}`);
      return Module.FS;
    };
    // --gog also picks up GOG Galaxy style split files; --exclude-temp skips the
    // installer's own temp files; the language filter keeps one copy of
    // files that exist per language.
    const common = ["--quiet", "--gog", "--exclude-temp", "--language", "en-US"];
    log(`running innoextract on ${(blob.size / 1048576).toFixed(1)} MB`);
    const paths = [];
    await run(["--list", ...common], (line) => { for (const m of line.matchAll(/"([^"]+)"/g)) paths.push(m[1].replace(/\\/g, "/")); });
    const dirs = gameDirs(paths, detect);
    if (dirs.length) log(`${paths.length} files in the installer: unpacking only ${dirs.join(", ")}`);
    const FS = await run(["--extract", ...common, ...dirs.flatMap((d) => ["--include", "/" + d]), "--output-dir", "/out"], log);
    const files = [];
    const walk = (dirPath, rel) => {
      for (const name of FS.readdir(dirPath)) {
        if (name === "." || name === "..") continue;
        const full = dirPath + "/" + name;
        const st = FS.stat(full);
        if (FS.isDir(st.mode)) walk(full, rel ? rel + "/" + name : name);
        else files.push({ path: rel ? rel + "/" + name : name, data: FS.readFile(full).buffer });
      }
    };
    walk("/out", "");
    log(`extracted ${files.length} files`);
    self.postMessage({ type: "done", files }, files.map((f) => f.data));
  } catch (e) {
    self.postMessage({ type: "error", message: e && e.message ? e.message : String(e) });
  }
};
