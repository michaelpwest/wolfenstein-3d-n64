// worker.mjs - runs the build (build.mjs) away from the page, so that the
// page stays responsive. Python is loaded the first time and kept for the
// next build. Messages to the page: {type: "log", line}, {type: "progress",
// label, fraction, overall}, then {type: "done", name, title, bytes} or
// {type: "failed", message}.
import { loadPyodide } from "./pyodide/pyodide.mjs";
import { buildRom } from "./build.mjs";

const base = new URL("./", import.meta.url);
let pyodide = null;

// Songs and sounds converted by earlier builds, kept in this browser's
// IndexedDB so that a build after a reload reuses them too (build.mjs's
// kept). One database, emptied whenever the converters change.
function browserStore() {
    let db = null;
    const open = () => db ??= new Promise((resolve, reject) => {
        const request = indexedDB.open("wolf3d-n64-builder", 1);
        request.onupgradeneeded = () => {
            request.result.createObjectStore("converted");
            request.result.createObjectStore("meta");
        };
        request.onsuccess = () => resolve(request.result);
        request.onerror = () => reject(request.error);
    });
    // one request in a transaction of its own; its result once that is done
    const run = (name, mode, ask) => open().then(d => new Promise((resolve, reject) => {
        const tx = d.transaction(name, mode);
        const request = ask(tx.objectStore(name));
        tx.oncomplete = () => resolve(request.result);
        tx.onerror = tx.onabort = () => reject(tx.error);
    }));
    return {
        async begin(tools) {
            if (await run("meta", "readonly", s => s.get("tools")) !== tools) {
                await run("converted", "readwrite", s => s.clear());
                await run("meta", "readwrite", s => s.put(tools, "tools"));
            }
        },
        get: key => run("converted", "readonly", s => s.get(key)),
        put: (key, files) => run("converted", "readwrite", s => s.put(files, key)),
    };
}

const env = {
    store: typeof indexedDB === "undefined" ? undefined : browserStore(),
    async fetchBytes(path) {
        const response = await fetch(new URL(path, base));
        if (!response.ok)
            throw new Error(`${path} is missing from the page (${response.status})`);
        return new Uint8Array(await response.arrayBuffer());
    },
    importModule: path => import(new URL(path, base).href),
    log: line => postMessage({ type: "log", line }),
    progress: p => postMessage({ type: "progress", ...p }),
};

self.onmessage = async ({ data }) => {
    try {
        if (!pyodide) {
            env.log("Loading Python…");
            pyodide = await loadPyodide({ indexURL: new URL("pyodide/", base).href });
            pyodide.setStdout({ batched: env.log });
            pyodide.setStderr({ batched: env.log });
        }
        env.pyodide = pyodide;
        const files = new Map(data.files.map(f => [f.name, new Uint8Array(f.buffer)]));
        const rom = await buildRom(env, files, data.romName);
        postMessage({ type: "done", name: rom.name, title: rom.title, bytes: rom.bytes },
                    [rom.bytes.buffer]);
    } catch (e) {
        postMessage({ type: "failed", message: (e && e.message) || String(e) });
    }
};
