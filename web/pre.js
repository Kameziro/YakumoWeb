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

// Loads `url` into the file system at `path` before main() starts. A missing
// file is left out, with a note unless `optional`.
function preload(url, path, optional) {
  const id = "yakumo-preload-" + path;
  addRunDependency(id);
  fetch(url)
    .then((response) => {
      if (!response.ok) throw new Error(response.status + " " + response.statusText);
      return response.arrayBuffer();
    })
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
  preload("fonts/NotoSansCJK-Regular.ttc", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", true);
});

// Development loader, until the game data is loaded in chunks:
//   Yakumo.html?game=<folder URL>   downloads <folder>/EBOOT.ELF and
//                                   <folder>/disc.iso into /game and points
//                                   MHP3RD_GAME_DIR there
//   Yakumo.html?env=A=1,B=2         sets environment variables
// The whole image is held in memory, about 1.3 GB. The memory stick of that
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
  for (const name of ["EBOOT.ELF", "disc.iso"]) preload(base + name, "/game/" + name, false);
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
