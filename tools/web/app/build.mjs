// build.mjs - the ROM build, in the browser (worker.mjs) or in Node
// (tools/web/build_node.mjs): the steps the Makefile takes after compiling,
// with nothing leaving the machine it runs on.
//
//   extract_assets.py   the game files    -> wolf3d.dat, wolfbig.dat, audio/
//                       (in Pyodide, Python compiled to WebAssembly)
//   imf2xm              the songs         -> XM modules
//   audioconv64         the XM modules    -> xm64, the digitized sounds -> wav64
//   mkdfs               all of that       -> the ROM's filesystem
//   n64tool             program + symbols + filesystem -> the ROM
//   ed64romconfig       the ROM header's save type: SRAM, for the ten slots
//
// The tools are libdragon's (and imf2xm this project's), compiled with
// Emscripten (wasm_tools.sh). Each runs in a fresh instance with its own
// in-memory file system: the files it needs are put in, its main is called
// with the arguments the Makefile gives it, and what it wrote is read out.

// The files the build reads beside this one, relative to it
const EXTRACTOR = ["extract_assets.py", "lzexe.py", "wolf_tables.py"];

// The build's steps, and about how much of its time each takes, for an
// overall bar: converting the music is nearly all of it
export const STEPS = [
    ["Reading the game files", 3],
    ["Converting the music", 88],
    ["Converting the sounds", 5],
    ["Putting the ROM together", 4],
];

// Reports a step's progress to env.progress, as {label, fraction, overall,
// done, of, unit}: how far into this step and into the whole build, and for
// a step that goes through things one at a time, how many of them ("3 of 11
// songs"); `of` is 0 for the others. Logs each step as it starts.
function tracker(env) {
    const total = STEPS.reduce((sum, [, share]) => sum + share, 0);
    let step = 0, before = 0, of = 0, unit = "";
    const send = (fraction, done) => env.progress && env.progress({
        label: STEPS[step][0], fraction, done, of, unit,
        overall: (before + STEPS[step][1] * fraction) / total,
    });
    return {
        // start(n), or start(n, of, unit) for a step that counts
        start(n, count = 0, what = "") {
            step = n;
            of = count;
            unit = what;
            before = STEPS.slice(0, n).reduce((sum, [, share]) => sum + share, 0);
            env.log(STEPS[n][0]);
            send(0, 0);
        },
        count: done => send(of ? done / of : 1, done),  // so many done
        part: fraction => send(fraction, 0),             // or, without a count, so far
    };
}

// Progress, as the page words it: "Converting the music… 3 of 11 songs"
export function describe({ label, done, of, unit }) {
    return of ? `${label}… ${done} of ${of} ${unit}` : `${label}…`;
}

export const GAMES = { WL1: "the shareware episode", WL6: "the full game",
                       SOD: "Spear of Destiny" };

// Which game a set of file names is, by its VSWAP file, as the extractor
// tells them apart; null for none, or more than one
export function findGame(names) {
    const { games } = checkGame(names);
    return games.length === 1 ? games[0] : null;
}

const DATA_FILES = ["VSWAP", "GAMEMAPS", "MAPHEAD", "VGAGRAPH", "VGADICT", "VGAHEAD",
                    "AUDIOT", "AUDIOHED"];

// A folder's file names, checked as build.sh's check_folder does: {games}
// - the kinds of game there, by their VSWAP files - and for one game,
// missing, the files of it that are not there: its eight data files and its
// executable (Spear of Destiny's is SPEAR.EXE, or WOLF3D.EXE in some releases)
export function checkGame(names) {
    const upper = new Set(names.map(n => n.toUpperCase()));
    const games = Object.keys(GAMES).filter(ext => upper.has("VSWAP." + ext));
    if (games.length !== 1)
        return { games, missing: [] };
    const ext = games[0];
    const missing = DATA_FILES.map(base => `${base}.${ext}`).filter(f => !upper.has(f));
    const exes = ext === "SOD" ? ["SPEAR.EXE", "WOLF3D.EXE"] : ["WOLF3D.EXE"];
    if (!exes.some(exe => upper.has(exe)))
        missing.push(exes[0]);
    return { games, missing };
}

// A, B and C
export function namesList(names) {
    return names.length < 2 ? names.join("") : `${names.slice(0, -1).join(", ")} and ${names.at(-1)}`;
}

// build.sh's --name rules: a file name, .z64 added if it is not there
export function checkRomName(name) {
    let n = name.trim();
    if (!n.toLowerCase().endsWith(".z64"))
        n += ".z64";
    if (/[\\/:*?"<>|\x00-\x1f]/.test(n) || n === ".z64")
        throw new Error('The ROM name must be a file name, without a folder or any of \\ / : * ? " < > |');
    return n;
}

// env: { pyodide, fetchBytes(path), importModule(path), log(line),
//        progress({label, fraction, overall, ...}) and store - both
//        optional; see tracker above and kept below }
// files: a Map of file name to Uint8Array - the game folder's
export async function buildRom(env, files, romName) {
    const { pyodide, log } = env;
    const steps = tracker(env);

    const { games, missing } = checkGame([...files.keys()]);
    if (games.length !== 1)
        throw new Error("These files are not one of the games: the folder needs " +
                        "VSWAP.WL1, VSWAP.WL6 or VSWAP.SOD and the files that go with it");
    if (missing.length)
        throw new Error(`${GAMES[games[0]][0].toUpperCase()}${GAMES[games[0]].slice(1)} is missing ` +
                        `${namesList(missing)}: the folder needs all of the game's files`);
    if (romName)
        romName = checkRomName(romName);

    // --- the extractor, in Pyodide ---
    steps.start(0);
    const PY = pyodide.FS;
    pyodide.runPython("import shutil\nfor d in ('/data', '/out'): shutil.rmtree(d, ignore_errors=True)");
    PY.mkdirTree("/data");
    for (const [name, bytes] of files)
        PY.writeFile("/data/" + name, bytes);
    PY.mkdirTree("/app/tools");
    PY.mkdirTree("/app/src");
    for (const f of EXTRACTOR)
        PY.writeFile("/app/tools/" + f, await env.fetchBytes("py/" + f));
    PY.writeFile("/app/src/sprites.h", await env.fetchBytes("py/sprites.h"));
    const failed = await pyodide.runPythonAsync(`
import importlib, sys
if "/app/tools" not in sys.path:
    sys.path.insert(0, "/app/tools")
import extract_assets
extract_assets = importlib.reload(extract_assets)
sys.argv = ["extract_assets.py", "--data", "/data", "--out", "/out"]
failed = None
try:
    extract_assets.main()
except SystemExit as e:
    if e.code not in (None, 0):
        failed = str(e.code)
failed`);
    if (failed)
        throw new Error(failed);
    const blob = PY.readFile("/out/wolf3d.dat");
    const big = PY.readFile("/out/wolfbig.dat");
    const audio = new Map();
    for (const f of PY.readdir("/out/audio").filter(f => !f.startsWith(".")).sort())
        audio.set(f, PY.readFile("/out/audio/" + f));

    const spear = blob[11] === 2;               // the blob's version word
    const title = spear ? "Spear of Destiny" : "Wolfenstein 3D";
    const program = spear ? "spear" : "wolf3d"; // as the Makefile names it
    romName = romName || (spear ? "Spear of Destiny.z64" : "Wolfenstein 3D.z64");

    steps.part(1);

    // --- the Makefile's audio rule, for the default kinds: the songs as XM
    // modules, the AdLib effects as their own chunks (synthesized on the
    // N64), the digitized sounds as they are. The Makefile hands each tool
    // the whole folder; here each song and sound goes through on its own,
    // which the tools do anyway, so that the build can say how far it is. ---
    // Each one's result is kept, and taken from there by the next build that
    // has the same song or sound (see kept, below).
    const tools = await toolsId(env);
    if (env.store)
        await env.store.begin(tools).catch(() => {});
    let reused = 0;

    const songs = [...audio.keys()].filter(f => f.endsWith(".imf"));
    steps.start(1, songs.length, "songs");
    const music = new Map();
    for (const [i, song] of songs.entries()) {
        const { files, again } = await kept(env, await sha256(tools, "song", song, audio.get(song)), async () => {
            const xm = (await runTool(env, "imf2xm", ["/in/audio", "/xm"],
                                      { "/in/audio": new Map([[song, audio.get(song)]]) },
                                      ["/xm"])).get("/xm");
            return (await runTool(env, "audioconv64",
                                  ["-o", "/fs/music", ...[...xm.keys()].map(f => "/xm/" + f)],
                                  { "/xm": xm }, ["/fs/music"])).get("/fs/music");
        });
        for (const [f, bytes] of files)
            music.set(f, bytes);
        reused += again;
        steps.count(i + 1);
    }

    const wavs = [...audio.keys()].filter(f => f.endsWith(".wav"));
    steps.start(2, wavs.length, "sounds");
    const sfxFiles = new Map();
    for (const [i, wav] of wavs.entries()) {
        const { files, again } = await kept(env, await sha256(tools, "sound", wav, audio.get(wav)), async () =>
            (await runTool(env, "audioconv64",
                           ["--wav-compress", "0", "-o", "/fs/sfx", "/wav/sfx/" + wav],
                           { "/wav/sfx": new Map([[wav, audio.get(wav)]]) },
                           ["/fs/sfx"])).get("/fs/sfx"));
        for (const [f, bytes] of files)
            sfxFiles.set(f, bytes);
        reused += again;
        steps.count(i + 1);
    }
    if (reused)
        log(`  ${reused} of ${songs.length + wavs.length} songs and sounds were converted by an earlier build, and reused`);

    // --- the ROM's filesystem, and the ROM ---
    steps.start(3);
    for (const [f, bytes] of audio)
        if ((f.startsWith("al") && f.endsWith(".adl"))     // the AdLib effects, and
            || (f.startsWith("pc") && f.endsWith(".pcs")))  // the PC Speaker's
            sfxFiles.set(f, bytes);
    const dfs = await runTool(env, "mkdfs", ["/work/wolf3d.dfs", "/fs"], {
        "/fs": new Map([["wolf3d.dat", blob], ["wolfbig.dat", big]]),
        "/fs/music": music,
        "/fs/sfx": sfxFiles,
    }, ["/work"]);

    steps.part(1 / 3);
    const work = new Map([
        [program + ".elf.stripped", await env.fetchBytes("program/wolf3d.elf.stripped")],
        [program + ".elf.sym", await env.fetchBytes("program/wolf3d.elf.sym")],
        ["wolf3d.dfs", dfs.get("/work").get("wolf3d.dfs")],
    ]);
    // names relative to the work folder: the ROM's table of contents
    // records them, as it does the Makefile's
    const rom = await runTool(env, "n64tool",
        ["--title", title, "--toc", "--output", "/work/rom.z64",
         "--align", "256", program + ".elf.stripped",
         "--align", "8", program + ".elf.sym",
         "--align", "16", "wolf3d.dfs"],
        { "/work": work }, ["/work"], "/work");
    steps.part(2 / 3);
    const final = await runTool(env, "ed64romconfig", ["--savetype", "sram256k", "/work/rom.z64"],
        { "/work": new Map([["rom.z64", rom.get("/work").get("rom.z64")]]) }, ["/work"]);
    const bytes = final.get("/work").get("rom.z64");
    steps.part(1);
    log(`Built ${romName} (${title}, ${(bytes.length / 1e6).toFixed(1)} MB)`);
    return { name: romName, title, bytes };
}

// --- conversions kept ---
//
// Converting a song or a sound depends on nothing but its file - its name and
// its bytes - and the converters' own code. So the result is kept under a
// SHA-256 of all of those: in memory while this page, or this Node process,
// runs, and in env.store if there is one - IndexedDB in the browser
// (worker.mjs), a folder in build/ for Node (build_node.mjs) - so that a
// later build that has the same song, of this game or another, takes it from
// there. With the converters' code in the key, a new builder never takes an
// old one's results; env.store.begin(tools) lets a store drop those.
//
// env.store: { begin(tools), get(key) -> Map of file name to bytes, or
//              nothing, put(key, files) } - all async, all allowed to fail

const KEPT_FORMAT = "1";                    // to change, should what is kept
const remembered = new Map();

async function kept(env, key, convert) {
    let files = remembered.get(key);
    if (!files && env.store)
        files = await env.store.get(key).catch(() => undefined);
    if (files && files.size) {
        remembered.set(key, files);
        return { files, again: 1 };
    }
    files = await convert();
    remembered.set(key, files);
    if (env.store)
        await env.store.put(key, files).catch(() => {});
    return { files, again: 0 };
}

// The hex SHA-256 of strings and byte arrays, each preceded by its length
async function sha256(...parts) {
    const chunks = parts.map(p => typeof p === "string" ? new TextEncoder().encode(p) : p);
    const all = new Uint8Array(chunks.reduce((n, c) => n + 4 + c.length, 0));
    const view = new DataView(all.buffer);
    let at = 0;
    for (const c of chunks) {
        view.setUint32(at, c.length);
        all.set(c, at + 4);
        at += 4 + c.length;
    }
    const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", all));
    return [...digest].map(b => b.toString(16).padStart(2, "0")).join("");
}

// The converters' code, as one hash
let toolsHash = null;
function toolsId(env) {
    toolsHash ??= Promise.all(["imf2xm", "audioconv64"].map(name => wasmBytes(env, name)))
        .then(code => sha256(KEPT_FORMAT, ...code));
    return toolsHash;
}

// One tool, in an instance of its own: `inputs` maps folders to the files
// to put in them, `outputs` lists the folders to read back. Returns a Map
// of each output folder to a Map of its files. Each tool's WebAssembly is
// compiled once and kept, as a tool may run once for every song.
const wasm = new Map(), compiled = new Map();

function wasmBytes(env, name) {
    if (!wasm.has(name))
        wasm.set(name, env.fetchBytes(`tools/${name}.wasm`));
    return wasm.get(name);
}

function compile(env, name) {
    if (!compiled.has(name))
        compiled.set(name, wasmBytes(env, name).then(bytes => WebAssembly.compile(bytes)));
    return compiled.get(name);
}

async function runTool(env, name, args, inputs, outputs, cwd) {
    const factory = (await env.importModule(`tools/${name}.mjs`)).default;
    const module = await compile(env, name);
    let output = "";
    const mod = await factory({
        // a new instance of the module compiled already
        instantiateWasm(imports, receive) {
            WebAssembly.instantiate(module, imports).then(instance => receive(instance, module));
            return {};
        },
        noInitialRun: true,
        print: s => { output += s + "\n"; },
        printErr: s => { output += s + "\n"; },
        // exit() ends the run, not the page - or Node
        quit: (status, toThrow) => { throw toThrow; },
    });
    const FS = mod.FS;
    for (const [dir, files] of Object.entries(inputs)) {
        FS.mkdirTree(dir);
        for (const [f, bytes] of [...files].sort(([a], [b]) => (a < b ? -1 : a > b)))
            FS.writeFile(`${dir}/${f}`, bytes);
    }
    for (const dir of outputs)
        FS.mkdirTree(dir);
    if (cwd)
        FS.chdir(cwd);
    let status = 0;
    try {
        status = mod.callMain(args) || 0;
    } catch (e) {
        if (e && e.name === "ExitStatus")
            status = e.status;
        else
            throw new Error(`${name} failed: ${e && e.message ? e.message : e}\n${output}`);
    }
    if (status !== 0)
        throw new Error(`${name} failed:\n${output.slice(-2000)}`);
    const result = new Map();
    for (const dir of outputs) {
        const files = new Map();
        for (const f of FS.readdir(dir).filter(f => f !== "." && f !== "..").sort())
            if (FS.isFile(FS.stat(`${dir}/${f}`).mode))
                files.set(f, FS.readFile(`${dir}/${f}`));
        result.set(dir, files);
    }
    return result;
}
