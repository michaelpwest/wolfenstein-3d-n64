// web_tools.mjs - what ./build.sh does with the web builder in Node, so that
// building a ROM needs nothing but Node.js:
//
//   node tools/web/web_tools.mjs fetch <url> <folder>   download a zip, unpack it into <folder>
//   node tools/web/web_tools.mjs serve <folder> <port> [open]
//                                                        serve <folder> to this computer,
//                                                        and open it in the browser
//
// serve runs until Ctrl+C. It opens the browser itself once it is listening,
// so the page never asks before the server can answer.
//
// fetch replaces <folder> only once the zip has unpacked whole. Its zips are
// the ones ./build.sh --web makes with Python's zipfile: stored or deflated
// entries, no zip64.
import { mkdir, rm, rename, writeFile, readFile, stat } from "node:fs/promises";
import { fstatSync } from "node:fs";
import { createServer } from "node:http";
import { spawn } from "node:child_process";
import { join, resolve, dirname, extname, sep } from "node:path";
import { inflateRawSync } from "node:zlib";

const [command, ...args] = process.argv.slice(2);
// A failure ends the command, and Node with it once it is idle: exiting
// with process.exit() while fetch still holds its connection trips an
// assertion in Node on Windows.
class Failure extends Error {}
const fail = message => { throw new Failure(message); };

async function fetchZip(url, folder) {
    let res;
    try {
        res = await fetch(url);
    } catch (e) {
        fail(`could not reach ${url}: ${e.cause?.message || e.message}`);
    }
    if (!res.ok)
        fail(`${url}: ${res.status} ${res.statusText}`);
    const zip = Buffer.from(await res.arrayBuffer());
    console.log(`  ${(zip.length / 1e6).toFixed(1)} MB`);

    const out = resolve(folder), part = out + ".part";
    await rm(part, { recursive: true, force: true });
    for (const entry of entries(zip)) {
        const path = resolve(part, entry.name);
        if (!path.startsWith(part + sep))
            fail(`the zip has a file outside its folder: ${entry.name}`);
        if (entry.name.endsWith("/")) {
            await mkdir(path, { recursive: true });
            continue;
        }
        await mkdir(dirname(path), { recursive: true });
        await writeFile(path, entry.data());
    }
    await rm(out, { recursive: true, force: true });
    await rename(part, out);
}

// The zip's files, from its central directory
function* entries(zip) {
    let end = zip.length - 22;
    while (end >= 0 && zip.readUInt32LE(end) !== 0x06054b50)
        end--;
    if (end < 0)
        fail("what was fetched is not a zip");
    const count = zip.readUInt16LE(end + 10);
    let at = zip.readUInt32LE(end + 16);
    for (let i = 0; i < count; i++) {
        if (zip.readUInt32LE(at) !== 0x02014b50)
            fail("the zip's directory is damaged");
        const method = zip.readUInt16LE(at + 10);
        const packed = zip.readUInt32LE(at + 20);
        const size = zip.readUInt32LE(at + 24);
        const nameLen = zip.readUInt16LE(at + 28);
        const skip = zip.readUInt16LE(at + 30) + zip.readUInt16LE(at + 32);
        const local = zip.readUInt32LE(at + 42);
        const name = zip.toString("utf8", at + 46, at + 46 + nameLen);
        at += 46 + nameLen + skip;
        if (method !== 0 && method !== 8)
            fail(`${name}: a kind of compression this does not unpack (${method})`);
        yield {
            name,
            data() {
                const start = local + 30 + zip.readUInt16LE(local + 26) + zip.readUInt16LE(local + 28);
                const raw = zip.subarray(start, start + packed);
                const bytes = method === 8 ? inflateRawSync(raw) : raw;
                if (bytes.length !== size)
                    fail(`${name} did not unpack whole`);
                return bytes;
            },
        };
    }
}

const TYPES = {
    ".html": "text/html; charset=utf-8", ".css": "text/css", ".js": "text/javascript",
    ".mjs": "text/javascript", ".wasm": "application/wasm", ".json": "application/json",
    ".txt": "text/plain; charset=utf-8", ".py": "text/plain; charset=utf-8",
    ".h": "text/plain; charset=utf-8", ".zip": "application/zip", ".svg": "image/svg+xml",
};

// The page in the system's browser, where there is a way to open it
function openBrowser(url) {
    const [cmd, args] = process.platform === "win32" ? ["cmd", ["/c", "start", '""', url]]
                      : process.platform === "darwin" ? ["open", [url]]
                      : ["xdg-open", [url]];
    try {
        const child = spawn(cmd, args, { detached: true, stdio: "ignore", windowsHide: true,
                                         windowsVerbatimArguments: process.platform === "win32" });
        child.on("error", () => console.log("Open it in your browser yourself."));
        child.unref();
    } catch {
        console.log("Open it in your browser yourself.");
    }
}

function serve(folder, port, open) {
    const root = resolve(folder);
    const server = createServer(async (req, res) => {
        let path = decodeURIComponent(new URL(req.url, "http://localhost").pathname);
        if (path.endsWith("/"))
            path += "index.html";
        const file = resolve(join(root, path));
        try {
            if (!file.startsWith(root + sep) || !(await stat(file)).isFile())
                throw new Error();
            const body = await readFile(file);
            res.writeHead(200, { "Content-Type": TYPES[extname(file)] || "application/octet-stream",
                                 "Cache-Control": "no-cache" });
            res.end(body);
        } catch {
            res.writeHead(404, { "Content-Type": "text/plain" });
            res.end("not found");
        }
    });
    // Ctrl+C is how it is meant to end, not a failure
    process.on("SIGINT", () => {
        console.log("Stopped.");
        process.exit(0);
    });
    // In Git Bash a Windows program's input is a pipe from the terminal
    // window, which closes with it; without this, closing the window would
    // leave the server running out of sight. (A terminal of the system's
    // own closes it with a signal instead.) On Windows isFIFO() is never
    // true, so the type is read from the mode: S_IFIFO, 0o010000.
    try {
        if ((fstatSync(0).mode & 0o170000) === 0o010000) {
            process.stdin.on("end", () => process.exit(0));
            process.stdin.on("error", () => process.exit(0));
            process.stdin.resume();
        }
    } catch {
        // no input at all: nothing to watch
    }
    server.on("error", e => {
        console.error(e.code === "EADDRINUSE" ? `port ${port} is in use` : e.message);
        process.exitCode = 1;
    });
    server.listen(port, "127.0.0.1", () => {
        const url = `http://localhost:${port}`;
        console.log(`\nThe web builder is at ${url} - open it in your browser. Ctrl+C stops it.\n`);
        if (open)
            openBrowser(url);
    });
}

try {
    if (command === "fetch" && args.length === 2)
        await fetchZip(args[0], args[1]);
    else if (command === "serve" && (args.length === 2 || (args.length === 3 && args[2] === "open")))
        serve(args[0], Number(args[1]), args[2] === "open");
    else
        fail("usage: node tools/web/web_tools.mjs fetch <url> <folder> | serve <folder> <port> [open]");
} catch (e) {
    if (!(e instanceof Failure))
        throw e;
    console.error(e.message);
    process.exitCode = 1;
}
