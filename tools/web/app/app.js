// app.js - the page: the game folder chosen or dropped, the build started
// in worker.mjs, its progress shown, and the ROM offered for download.
import { GAMES, checkGame, namesList, checkRomName, describe } from "./build.mjs";

const $ = id => document.getElementById(id);
const pick = $("pick"), folder = $("folder"), found = $("found");
const romname = $("romname"), buildButton = $("build"), download = $("download");
const progress = $("progress"), bar = $("bar"), percent = $("percent");

function setBar(fraction) {
    bar.style.width = `${fraction * 100}%`;
    percent.textContent = `${Math.floor(fraction * 100)}%`;
}
const status = $("status"), log = $("log");

let files = null;                           // [{name, file}] - the folder's own
let worker = null;
let romUrl = null;

// --- the folder ---

// Only the folder's own files: the game's are all at its top, and a
// subfolder is something else's - though if a subfolder holds the game,
// the folder around it was picked by mistake, and that is worth saying.
// nested: a Map of each subfolder's name to the names of its files.
function useFiles(list, nested = new Map()) {
    files = list;
    const { games, missing } = checkGame(list.map(f => f.name));
    const ok = games.length === 1 && !missing.length;
    let text;
    if (ok) {
        text = `Found ${GAMES[games[0]]}.`;
    } else if (games.length === 1) {
        text = `Found ${GAMES[games[0]]}, but ${namesList(missing)} ${missing.length === 1 ? "is" : "are"} ` +
               "missing: the folder needs all of the game's files.";
    } else if (games.length > 1) {
        text = `This folder holds more than one game - ${namesList(games.map(g => GAMES[g]))}: ` +
               "give each its own folder.";
    } else {
        const inside = [...nested].find(([, names]) => checkGame(names).games.length);
        text = inside ? `The game is not in this folder itself, but in its folder ${inside[0]}: choose that one.`
            : list.length ? "No game here: the folder needs VSWAP.WL1 (the shareware), VSWAP.WL6 " +
                            "(the full game) or VSWAP.SOD (Spear of Destiny), and the files that go with it."
            : "That folder is empty.";
    }
    found.className = "found " + (ok ? "ok" : "bad");
    found.textContent = text;
    romname.placeholder = games.length === 1 && games[0] === "SOD" ? "Spear of Destiny.z64" : "Wolfenstein 3D.z64";
    buildButton.disabled = !ok;
}

folder.addEventListener("change", () => {
    const list = [], nested = new Map();
    for (const file of folder.files) {
        const parts = (file.webkitRelativePath || file.name).split("/");
        if (parts.length <= 2)
            list.push({ name: parts[parts.length - 1], file });
        else if (parts.length === 3)
            nested.set(parts[1], [...(nested.get(parts[1]) || []), parts[2]]);
    }
    useFiles(list, nested);
});

// Dropped: a folder, read through the entries API
function readEntries(reader) {
    return new Promise((resolve, reject) => reader.readEntries(resolve, reject));
}
async function namesIn(dirEntry) {
    const names = [], reader = dirEntry.createReader();
    for (let batch; (batch = await readEntries(reader)).length; )
        names.push(...batch.map(entry => entry.name));
    return names;
}

// The folder's own files, and its subfolders' file names (see useFiles)
async function filesOf(dirEntry) {
    const list = [], nested = new Map(), reader = dirEntry.createReader();
    for (let batch; (batch = await readEntries(reader)).length; )
        for (const entry of batch) {
            if (entry.isFile)
                list.push({ name: entry.name,
                            file: await new Promise((res, rej) => entry.file(res, rej)) });
            else if (entry.isDirectory)
                nested.set(entry.name, await namesIn(entry));
        }
    return { list, nested };
}

pick.addEventListener("dragover", e => { e.preventDefault(); pick.classList.add("over"); });
pick.addEventListener("dragleave", () => pick.classList.remove("over"));
pick.addEventListener("drop", async e => {
    e.preventDefault();
    pick.classList.remove("over");
    const item = [...e.dataTransfer.items].find(i => i.kind === "file");
    const entry = item && item.webkitGetAsEntry && item.webkitGetAsEntry();
    if (entry && entry.isDirectory) {
        const { list, nested } = await filesOf(entry);
        useFiles(list, nested);
    } else {
        found.className = "found bad";
        found.textContent = item && /\.zip$/i.test(item.getAsFile()?.name || "")
            ? "That is a zip file: unzip it first, then drop the folder it makes."
            : "Drop the folder itself, not the files in it.";
        buildButton.disabled = true;
    }
});

// --- the build ---

function setStatus(text, bad) {
    status.textContent = text;
    status.className = "status" + (bad ? " bad" : "");
}

function startWorker() {
    worker = new Worker(new URL("./worker.mjs", import.meta.url), { type: "module" });
    worker.onmessage = ({ data }) => {
        if (data.type === "log") {
            log.textContent += data.line + "\n";
            log.scrollTop = log.scrollHeight;
            if (!data.line.startsWith(" ") && !data.line.includes(":"))
                setStatus(data.line);
        } else if (data.type === "progress") {
            // what this step is at, and the bar for the whole build
            setStatus(describe(data));
            setBar(data.overall);
        } else if (data.type === "done") {
            const blob = new Blob([data.bytes], { type: "application/octet-stream" });
            if (romUrl)
                URL.revokeObjectURL(romUrl);
            romUrl = URL.createObjectURL(blob);
            download.href = romUrl;
            download.download = data.name;
            download.textContent = `Download ${data.name}`;
            download.hidden = false;
            setBar(1);
            setStatus(`Built ${data.name} - ${data.title}, ${(data.bytes.byteLength / 1e6).toFixed(1)} MB.`);
            buildButton.disabled = false;
        } else if (data.type === "failed") {
            progress.hidden = true;
            setStatus("The ROM was not built:\n" + data.message, true);
            log.textContent += "FAILED: " + data.message + "\n";
            buildButton.disabled = false;
        }
    };
    worker.onerror = e => {
        progress.hidden = true;
        setStatus("The builder stopped: " + (e.message || "an error in the page"), true);
        buildButton.disabled = false;
        worker = null;
    };
}

buildButton.addEventListener("click", async () => {
    let name = romname.value.trim();
    try {
        if (name)
            name = checkRomName(name);
    } catch (e) {
        setStatus(e.message, true);
        return;
    }
    buildButton.disabled = true;
    download.hidden = true;
    log.textContent = "";
    setBar(0);
    progress.hidden = false;
    setStatus("Reading your files…");
    const payload = [];
    for (const { name: n, file } of files)
        payload.push({ name: n, buffer: await file.arrayBuffer() });
    if (!worker)
        startWorker();
    worker.postMessage({ files: payload, romName: name || null },
                       payload.map(f => f.buffer));
});
