// Runs before main(): keeps the per-user data directory in the browser.
//
// SDL_GetPrefPath returns /libsdl/<organization>/<application>/ under
// Emscripten, so settings, saves and EBOOT.ELF all live below /libsdl. That
// tree is an IndexedDB file system: its contents are loaded before main()
// starts, and every change is written back on its own (autoPersist). Code runs
// only while the page is between frames, so a write back never sees a file
// half written.
Module.preRun = Module.preRun || [];

// Development loader, until the game data is loaded in chunks:
//   Yakumo.html?game=<folder URL>   downloads <folder>/EBOOT.ELF and
//                                   <folder>/disc.iso into /game and points
//                                   MHP3RD_GAME_DIR there
//   Yakumo.html?env=A=1,B=2         sets environment variables
// The whole image is held in memory, about 1.3 GB.
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
  ENV.MHP3RD_GAME_DIR = "/game";
  for (const name of ["EBOOT.ELF", "disc.iso"]) {
    const id = "yakumo-game-" + name;
    addRunDependency(id);
    fetch(base + name)
      .then((response) => {
        if (!response.ok) throw new Error(response.status + " " + response.statusText);
        return response.arrayBuffer();
      })
      .then((bytes) => {
        FS.writeFile("/game/" + name, new Uint8Array(bytes));
        console.log("[game] " + name + ": " + bytes.byteLength + " bytes");
      })
      .catch((error) => console.error("[game] cannot load " + base + name + ": " + error))
      .finally(() => removeRunDependency(id));
  }
});

Module.preRun.push(() => {
  FS.mkdir("/libsdl");
  FS.mount(IDBFS, { autoPersist: true }, "/libsdl");
  addRunDependency("yakumo-data");
  FS.syncfs(true, (error) => {
    if (error) console.error("[data] cannot read the saved data:", error);
    removeRunDependency("yakumo-data");
  });
});
