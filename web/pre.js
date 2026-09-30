// Runs before main(): keeps the per-user data directory in the browser, and
// brings in what the page serves beside the program.
//
// SDL_GetPrefPath returns /libsdl/<organization>/<application>/ under
// Emscripten, so settings, saves and EBOOT.ELF all live below /libsdl. That
// tree is an IndexedDB file system: its contents are loaded before main()
// starts, and every change is written back on its own (autoPersist). Code runs
// only while the page is between frames, so a write back never sees a file
// half written.
Module.preRun = Module.preRun || [];

const kDataDirectory = "/libsdl/Yakumo/MHP3rd";

// Big files come from the browser's own storage after the first visit: the
// origin private file system (OPFS) keeps each one on disk, and asking for
// persistent storage keeps the browser from dropping them when space runs
// short. (The Cache API refuses a file the size of the disc image.) Bump the
// folder's name to drop what is stored, for example after the files on the
// server change.
const kStoreFolder = "yakumo-files-v1";
let persistAsked = false;

function setStatus(text) {
  if (Module.setStatus) Module.setStatus(text);
}

// The file name `url` is kept under.
function storedName(url) {
  return new URL(url, location.href).pathname.replace(/[^A-Za-z0-9._-]/g, "_");
}

// The response for `url`, from storage when it is there, otherwise
// downloaded with its progress shown and, when `cache`, stored.
async function fetchStored(url, cache) {
  let folder = null;
  if (cache && navigator.storage && navigator.storage.getDirectory) {
    try {
      const root = await navigator.storage.getDirectory();
      folder = await root.getDirectoryHandle(kStoreFolder, { create: true });
      const stored = await folder.getFileHandle(storedName(url));
      return new Response(await stored.getFile());
    } catch (error) {
      // Not stored yet, or no storage here.
    }
    if (!persistAsked && navigator.storage.persist) {
      persistAsked = true;
      navigator.storage.persist().catch(() => {});
    }
  }
  const response = await fetch(url);
  if (!response.ok) throw new Error(response.status + " " + response.statusText);
  const total = Number(response.headers.get("Content-Length")) || 0;
  if (!folder || !response.body) return response;
  // One branch of the body is written to disk, the other only counted for the
  // progress; then the file is read back from disk, so it is in memory once.
  // It is written under a .part name and renamed when complete, so a download
  // cut short is never taken for the file.
  const [stored_body, counted_body] = response.body.tee();
  const name = storedName(url);
  const part = await folder.getFileHandle(name + ".part", { create: true });
  const storing = stored_body.pipeTo(await part.createWritable());
  const reader = counted_body.getReader();
  let received = 0;
  let shown = -1;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    received += value.byteLength;
    const percent = total ? Math.floor((received * 100) / total) : -1;
    if (percent !== shown) {
      shown = percent;
      const name = url.slice(url.lastIndexOf("/") + 1);
      setStatus("Downloading " + name + (total ? ": " + percent + "%" : ": " + (received >> 20) + " MB") +
                " (once; kept by the browser)");
    }
  }
  try {
    await storing;
    await part.move(name);
    return new Response(await (await folder.getFileHandle(name)).getFile());
  } catch (error) {
    // No room, or a private window: download it again, not kept.
    console.warn("[preload] cannot keep " + url + " in the browser: " + error);
    folder.removeEntry(name + ".part").catch(() => {});
    return fetch(url);
  }
}

// Loads `url` into the file system at `path` before main() starts. A missing
// file is left out, with a note unless `optional`. With `cache`, the file is
// kept in the browser after the first download.
function preload(url, path, optional, cache) {
  const id = "yakumo-preload-" + path;
  addRunDependency(id);
  fetchStored(url, cache)
    .then((response) => response.arrayBuffer())
    .then((bytes) => {
      FS.mkdirTree(path.slice(0, path.lastIndexOf("/")));
      FS.writeFile(path, new Uint8Array(bytes));
      console.log("[preload] " + url + ": " + bytes.byteLength + " bytes");
    })
    .catch((error) => {
      if (!optional) console.error("[preload] cannot load " + url + ": " + error);
    })
    .finally(() => removeRunDependency(id));
}

// The game's text needs a font with Japanese in it, and a page has no system
// fonts to find. fonts/NotoSansCJK-Regular.ttc beside the page (Noto Sans CJK,
// SIL Open Font License) goes where host/fonts/game_font.cpp looks on Linux.
Module.preRun.push(() => {
  preload("fonts/NotoSansCJK-Regular.ttc", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", true, true);
});

// Development loader, until the game data is loaded in chunks:
//   Yakumo.html?game=<folder URL>   downloads <folder>/EBOOT.ELF and
//                                   <folder>/disc.iso into /game and points
//                                   MHP3RD_GAME_DIR there
//   Yakumo.html?env=A=1,B=2         sets environment variables
// The whole image is held in memory, about 1.3 GB, and is downloaded only the
// first time; later visits take it from the browser's storage. The memory stick of that
// game folder (/game/ms0, the saves) is the one in the data directory, so it
// is kept.
Module.preRun.push(() => {
  const params = new URLSearchParams(location.search);
  for (const pair of (params.get("env") || "").split(",")) {
    const at = pair.indexOf("=");
    if (at > 0) ENV[pair.slice(0, at)] = pair.slice(at + 1);
  }
  const game = params.get("game");
  if (!game) return;
  const base = game.endsWith("/") ? game : game + "/";
  FS.mkdir("/game");
  FS.symlink(kDataDirectory + "/ms0", "/game/ms0");
  ENV.MHP3RD_GAME_DIR = "/game";
  for (const name of ["EBOOT.ELF", "disc.iso"]) preload(base + name, "/game/" + name, false, true);
});

Module.preRun.push(() => {
  FS.mkdir("/libsdl");
  FS.mount(IDBFS, { autoPersist: true }, "/libsdl");
  addRunDependency("yakumo-data");
  FS.syncfs(true, (error) => {
    if (error) console.error("[data] cannot read the saved data:", error);
    FS.mkdirTree(kDataDirectory + "/ms0");
    removeRunDependency("yakumo-data");
  });
});
