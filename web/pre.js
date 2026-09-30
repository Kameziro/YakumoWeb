// Runs before main(): keeps the per-user data directory in the browser.
//
// SDL_GetPrefPath returns /libsdl/<organization>/<application>/ under
// Emscripten, so settings, saves and EBOOT.ELF all live below /libsdl. That
// tree is an IndexedDB file system: its contents are loaded before main()
// starts, and every change is written back on its own (autoPersist). Code runs
// only while the page is between frames, so a write back never sees a file
// half written.
Module.preRun = Module.preRun || [];
Module.preRun.push(() => {
  FS.mkdir("/libsdl");
  FS.mount(IDBFS, { autoPersist: true }, "/libsdl");
  addRunDependency("yakumo-data");
  FS.syncfs(true, (error) => {
    if (error) console.error("[data] cannot read the saved data:", error);
    removeRunDependency("yakumo-data");
  });
});
