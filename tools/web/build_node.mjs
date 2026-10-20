// build_node.mjs - the web builder's build (app/build.mjs), run in Node: the
// same WebAssembly the browser runs, with no browser. ./build.sh --rom
// builds the ROM with it, from the prebuilt web builder; it is also how the
// web builder's ROMs are compared with ./build.sh --compile's.
//
//   node tools/web/build_node.mjs [--progress] <game folder> <ROM to write> [web builder folder]
//
// The web builder folder defaults to build/web, as ./build.sh --web makes it.
// --progress shows each step's progress on one line that keeps updating,
// for a terminal; ./build.sh asks for it when its output is one, as Node
// cannot tell that from Git Bash, where its output is a pipe either way.
import { readFile, readdir, writeFile, stat, mkdir, rm, rename } from "node:fs/promises";
import { join, resolve, dirname, basename } from "node:path";
import { pathToFileURL, fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const live = process.argv.includes("--progress");
const [dataDir, out, webDir] = process.argv.slice(2).filter(a => a !== "--progress");
if (!dataDir || !out) {
    console.error("usage: node tools/web/build_node.mjs [--progress] <game folder> <ROM to write> [web builder folder]");
    process.exit(1);
}
const web = webDir ? resolve(webDir) : resolve(here, "..", "..", "build", "web");

// Node 18 keeps Web Crypto, which build.mjs hashes with, out of the globals
if (!globalThis.crypto)
    globalThis.crypto = (await import("node:crypto")).webcrypto;

// Songs and sounds converted by earlier builds, kept beside the web builder -
// build/web-cache, a folder for each, named by its hash - so that the next
// build reuses them (build.mjs's kept). Emptied whenever the converters change.
const cache = join(web, "..", "web-cache");
const store = {
    async begin(tools) {
        const marker = join(cache, "tools");
        if (await readFile(marker, "utf8").catch(() => null) !== tools) {
            await rm(cache, { recursive: true, force: true });
            await mkdir(cache, { recursive: true });
            await writeFile(marker, tools);
        }
    },
    async get(key) {
        const dir = join(cache, key);
        const names = await readdir(dir).catch(() => null);
        if (!names)
            return undefined;
        const files = new Map();
        for (const name of names.sort())
            files.set(name, new Uint8Array(await readFile(join(dir, name))));
        return files;
    },
    // written to the side and then renamed, so that a build stopped part way
    // never leaves half of one
    async put(key, files) {
        const dir = join(cache, key), part = dir + ".part";
        await rm(part, { recursive: true, force: true });
        await mkdir(part, { recursive: true });
        for (const [name, bytes] of files)
            await writeFile(join(part, name), bytes);
        await rm(dir, { recursive: true, force: true });
        await rename(part, dir);
    },
};

const { buildRom, STEPS } = await import(pathToFileURL(join(web, "build.mjs")).href);
const { loadPyodide } = await import(pathToFileURL(join(web, "pyodide", "pyodide.mjs")).href);

const t0 = Date.now();
const pyodide = await loadPyodide({ indexURL: join(web, "pyodide") + "/" });
const stamp = () => `  ${((Date.now() - t0) / 1000).toFixed(1).padStart(5)}s  `;
pyodide.setStdout({ batched: () => {} });   // the extractor's own detail, quietly

// Live, a step's line is rewritten as it goes and finished when the next
// begins - the step names it logs as they start are that line already:
//
//      12.2s  Converting the music     [████████░░░░░░░░░░░░]  3 of 11 songs
//       2.1s  Reading the game files… done
//
// Otherwise each step is a line as it starts.
const BAR = 20;
let open = false, current = null;
const steps = new Set((STEPS || []).map(([label]) => label));
const width = Math.max(0, ...[...steps].map(label => label.length));
const finish = () => { if (open) process.stdout.write("\n"); open = false; };
const log = line => {
    if (live && steps.has(line))
        return;
    finish();
    console.log(stamp() + line);
};
const progress = live ? ({ label, fraction, done, of, unit }) => {
    let text;
    if (of) {
        const filled = Math.round(fraction * BAR);
        text = `${label.padEnd(width)}  [${"█".repeat(filled)}${"░".repeat(BAR - filled)}]  ${done} of ${of} ${unit}`;
    } else {
        text = `${label}…${fraction >= 1 ? " done" : ""}`;
    }
    if (current !== label)
        finish();
    process.stdout.write("\r" + stamp() + text + "\x1b[K");
    open = true;
    current = label;
} : undefined;

const files = new Map();
for (const name of await readdir(dataDir))
    if ((await stat(join(dataDir, name))).isFile())
        files.set(name, new Uint8Array(await readFile(join(dataDir, name))));

try {
    const rom = await buildRom({
        pyodide,
        fetchBytes: async path => new Uint8Array(await readFile(join(web, path))),
        importModule: path => import(pathToFileURL(join(web, path)).href),
        log,
        progress,
        store,
    }, files, basename(out));
    await writeFile(out, rom.bytes);
} catch (e) {
    finish();
    console.error(e && e.message ? e.message : e);
    process.exit(1);
}
