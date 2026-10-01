# Web port

This branch builds Yakumo for the browser with Emscripten and WebGPU. The game is playable in current Chrome and Edge: the village, its cutscenes and quests run at 30 fps and full speed, with sound, music, movies, the keyboard, the mouse and saves. The milestones at the end record what was done and what is missing.

The steps, in order:

1. Build the container ([Build environment](#build-environment)).
2. Prepare the game and generate its code natively in it ([Native stages](#native-stages-in-the-container)), then its overlays (`web/generate_overlays.sh`).
3. Build for the web ([Web build](#web-build-in-the-container)) and serve the page with `web/serve.py`, or from your own server behind a password (`web/nginx.conf.example`).

The web build still needs your own disc image. The native build steps in [docs/BUILDING.md](../docs/BUILDING.md) prepare the executable and generate the recompiled code, and the web build compiles that code to WebAssembly. Everything derived from the game stays out of Git, as in the native build.

## Build environment

One Docker image holds both toolchains: Debian 13 with the native build tools, and the Emscripten SDK. From Git Bash on Windows, or any shell on Linux and macOS:

```bash
web/docker/run.sh                                # shell in the container, checkout at /src
YAKUMO_ISO=/path/to/your.iso web/docker/run.sh   # the same, with the image at /iso/game.iso
web/docker/run.sh "<command>"                    # run one command and exit
```

Build trees (`/src/out`), the compiler cache and the per-user data directory (`EBOOT.ELF`, settings, saves) live on the Docker volumes `yakumo-out`, `yakumo-ccache` and `yakumo-data`, not in the checkout.

On Windows, check out with LF line endings (`git config core.autocrlf input`), or the shell scripts fail inside the container.

## Native stages in the container

```bash
cmake -S . -B out/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSPRECOMP_PROFILE=mhp3rd
cmake --build out/linux --target Yakumo
out/linux/bin/Yakumo --install /iso/game.iso --in-place
profiles/mhp3rd/scripts/prepare_game.sh /iso/game.iso ~/.local/share/Yakumo/MHP3rd/EBOOT.ELF
profiles/mhp3rd/scripts/generate.sh out/linux
web/generate_overlays.sh out/linux
```

`generate_overlays.sh` recompiles the 355 overlays to C++ (about 1.1 GB, in under a minute) without building the native libraries. The web build links the ones `MHP3RD_WEB_OVERLAYS` names: by default the cutscenes, the map parts and the small screens, 183 of them. The rest, mostly the monsters' and the weapons', would make the program too large for a page and run interpreted.

## Web build in the container

```bash
emcmake cmake -S . -B out/web -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSPRECOMP_PROFILE=mhp3rd \
    -DMHP3RD_FFMPEG=bundled -DPSPRECOMP_BUILD_TESTS=OFF -DPSPRECOMP_BUILD_PROFILE_TESTS=OFF \
    -DMHP3RD_WEB_LINK_OPT=-O1
cmake --build out/web --target Yakumo
mkdir -p build/web && cp out/web/bin/Yakumo.* build/web/
```

With the generated code (after `generate.sh`), the first build compiles it for about ten minutes; `-DMHP3RD_GENERATED_CODE=OFF` builds the setup screens alone in a minute. `MHP3RD_WEB_LINK_OPT=-O1` keeps the link at about half a minute, where the build type's `-O3` runs `wasm-opt` for more than ten.

Then serve the page and open `Yakumo.html` in a current Chrome or Edge, which have WebGPU and WebAssembly JSPI:

```bash
python web/serve.py build/web 8765
```

`serve.py` listens on 127.0.0.1 only and sends the cross-origin isolation headers that threads need (`Cross-Origin-Opener-Policy: same-origin`, `Cross-Origin-Embedder-Policy: require-corp`). A real server must send them too.

Until the game data is loaded in chunks, a development loader in `web/pre.js` downloads a game folder whole (about 1.3 GB, held in memory). It downloads each file once, with its progress shown, and keeps it in the browser's origin private file system (OPFS); later visits read it from there, in seconds. After the files on the server change, bump `kStoreFolder` in `web/pre.js`, or clear the site's data in the browser. Put `EBOOT.ELF` (from the data directory after `--install`) and `disc.iso` in `build/web/game/`, which Git ignores, and open:

```
http://127.0.0.1:8765/Yakumo.html?game=game&env=MHP3RD_NO_AUDIO=1,MHP3RD_PERF=log
```

`env=` sets environment variables, comma-separated. To serve it from a machine of your own, see `web/nginx.conf.example`. The saves of that game folder (`/game/ms0`) are the data directory's, so they persist.

The game's text needs a font with Japanese in it, and a page has no system fonts. Put Noto Sans CJK (SIL Open Font License) at `build/web/fonts/NotoSansCJK-Regular.ttc`, for example from Debian's `fonts-noto-cjk` package, and the page loads it where the game looks on Linux. Without it the game's text is blank.

How the port fits in:

- `host/gpu/webgpu_renderer.cpp` implements `gpu::VulkanRenderer` with WebGPU. `vulkan_renderer.hpp` exposes no Vulkan types, so the kernel and the interface use it unchanged. So far it draws the interface only.
- Each presented frame waits for the browser's next animation frame through JSPI, which suspends the wasm stack. The port's blocking loops keep their shape.
- `host/platform/web_dialogs.cpp` stands in for SDL's file dialogs, which Emscripten's SDL3 lacks.
- The whole build uses WebAssembly exceptions (`-fwasm-exceptions`), since the port reports errors as C++ exceptions, and threads (`-pthread`), since the host starts `std::thread`s; eight workers start with the page.
- The kernel's idle hook (`present_until`) gives the browser the time the kernel is about to sleep to keep PSP speed, since a sleep on the page's thread would spin.
- `web/pre.js` mounts `/libsdl`, where `SDL_GetPrefPath` puts the data directory, on IndexedDB before `main()` starts, and writes every change back on its own.

## Ad hoc play

A page has no TCP and no UDP, so the web port goes on line through a WebSocket gateway, `web/adhoc-gateway`, written for Node. For every TCP connection the native client would open to a PSP ad hoc server, the page opens a WebSocket to the gateway, which opens that connection and carries the byte stream unchanged:

```
page --wss /adhoc/ctl---> gateway --TCP 27312--> ad hoc server <--TCP-- desktop players
     --wss /adhoc/relay->         --TCP 27313-->
```

The server is any PSP ad hoc server: `Yakumo --adhoc-server`, a desktop player's Network > Host a session, or another one speaking the same protocols. Web and desktop players on the same server share its halls.

```bash
Yakumo --adhoc-server                  # on the machine that serves the page, or elsewhere
cd web/adhoc-gateway && npm install
ADHOC_SERVER=127.0.0.1:27312 node gateway.mjs
```

The gateway listens on `127.0.0.1:27380` and connects only to `ADHOC_SERVER` and the port after it; any other path is refused. `ADHOC_ALLOWED_ORIGIN` restricts the page origins it accepts, and `ADHOC_MAX_PER_ADDRESS` the connections per player address (64). `web/nginx.conf.example` passes `/adhoc/` to it, behind the page's password. `npm test` runs its tests.

`web/deploy` puts it all on an Ubuntu or Debian server with Nginx, Docker and Node: `bundle.sh` packs the page, the gateway and the ad hoc server (a Docker image, since a Yakumo build needs SDL3, which Ubuntu 24.04 lacks), and `install.sh`, run there as root with `DOMAIN` set, installs them, with HTTPS and a password when Nginx does not serve that domain yet.

In the game, Network > Server empty means this site's `/adhoc`; it also takes another path on the site, or a `ws://` or `wss://` address (`ws://127.0.0.1:27380/adhoc` with `serve.py`). The page cannot host a session or find one on the local network, so those parts of the Network page are left out.

In the page, the ad hoc client's network thread owns the WebSockets: they live in its worker, and instead of waiting in `poll()` it runs one pass of its loop every few milliseconds from a timer, so the worker's event loop can deliver their events. Two tabs of one site share the data directory, and with it the player's address and name: a second player on one computer needs another browser profile.

## Milestones

| # | Milestone | Status |
| --- | --- | --- |
| M0 | Docker image; native bootstrap, `--install` and `generate.sh` in the container | Done |
| M1 | Bootstrap compiled to WebAssembly opens an SDL3 canvas | Done: the setup screens run in Chrome with WebGPU and JSPI |
| M2 | Browser main loop, stack, persistent config and saves | Done: JSPI keeps the tab responsive and the data directory persists in IndexedDB. Open risk for M3: the JSPI stack holds about 1 MB (about 15,700 small frames in Chrome), and each guest call nests one large generated function |
| M3 | Generated code linked, overlays interpreted, game data loaded in chunks; frames run without rendering | Frames run: 30 fps at 100% speed in Chrome, 60 display lists a second, nothing drawn yet. The JSPI stack is enough so far. Game data still comes whole, not in chunks |
| M4 | WebGPU renderer; menus visible | Done: the game draws, in town and on a quest; keyboard, mouse buttons and a gamepad reach it; text uses Noto Sans CJK; saves persist. Missing: render targets as textures, points and lines, movies, pointer capture |
| M5 | Deployed behind authentication | Done: Nginx with a password, HTTPS and the isolation headers (`web/nginx.conf.example`); the files go up with `tar` over SSH |
| M6+ | FFmpeg, compiled overlays, threads, CI | FFmpeg: built for the web and linked statically (LGPL-2.1-or-later; the build is reproducible from this tree, so it can be relinked); music and movies play. Audio: an AudioWorklet on the browser's audio thread. Threads: done in M3. Overlays: 183 of 355 linked (`MHP3RD_WEB_OVERLAYS`); the rest, mostly monsters and weapons, run interpreted. The village, its cutscenes and a quest run at 30 fps and 100% speed in Chrome on an Intel laptop GPU. Next: render targets as textures, points and lines, mouse camera, CI |
