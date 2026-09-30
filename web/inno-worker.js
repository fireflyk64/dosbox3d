// Runs innoextract (compiled to WebAssembly, see scripts/build-web.sh) on an
// installer held in memory and posts the extracted files back.  Everything
// happens inside this worker's memory file system; nothing leaves the
// browser.
import createInnoextract from "./innoextract.js";

self.onmessage = async (ev) => {
  const { type, bytes } = ev.data;
  if (type !== "extract") return;
  const log = (line) => self.postMessage({ type: "log", line });
  try {
    const input = new Uint8Array(bytes);
    const Module = await createInnoextract({ print: log, printErr: log, locateFile: (p) => new URL(p, import.meta.url).href });
    const FS = Module.FS;
    FS.mkdir("/in"); FS.mkdir("/out");
    FS.writeFile("/in/setup.exe", input);
    log(`running innoextract on ${(input.length / 1048576).toFixed(1)} MB`);
    // --gog also picks up GOG Galaxy style split files; -m skips the
    // installer's own temp files; the language filter keeps one copy of
    // files that exist per language.
    const status = Module.callMain(["--extract", "--quiet", "--gog", "--exclude-temp", "--language", "en-US", "--output-dir", "/out", "/in/setup.exe"]);
    if (status) throw new Error(`innoextract exited with status ${status}`);
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
