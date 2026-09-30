# Web port (work in progress)

This branch builds Yakumo for the browser with Emscripten and WebGPU. Nothing web-specific works yet; the milestones below track progress.

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
```

## Web build in the container

```bash
emcmake cmake -S . -B out/web -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSPRECOMP_PROFILE=mhp3rd \
    -DMHP3RD_FFMPEG=OFF -DPSPRECOMP_BUILD_TESTS=OFF -DPSPRECOMP_BUILD_PROFILE_TESTS=OFF \
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

Until the game data is loaded in chunks, a development loader in `web/pre.js` downloads a game folder whole (about 1.3 GB, held in memory). Put `EBOOT.ELF` (from the data directory after `--install`) and `disc.iso` in `build/web/game/`, which Git ignores, and open:

```
http://127.0.0.1:8765/Yakumo.html?game=game&env=MHP3RD_NO_AUDIO=1,MHP3RD_PERF=log
```

`env=` sets environment variables, comma-separated. The saves of that game folder (`/game/ms0`) are the data directory's, so they persist.

The game's text needs a font with Japanese in it, and a page has no system fonts. Put Noto Sans CJK (SIL Open Font License) at `build/web/fonts/NotoSansCJK-Regular.ttc`, for example from Debian's `fonts-noto-cjk` package, and the page loads it where the game looks on Linux. Without it the game's text is blank.

How the port fits in:

- `host/gpu/webgpu_renderer.cpp` implements `gpu::VulkanRenderer` with WebGPU. `vulkan_renderer.hpp` exposes no Vulkan types, so the kernel and the interface use it unchanged. So far it draws the interface only.
- Each presented frame waits for the browser's next animation frame through JSPI, which suspends the wasm stack. The port's blocking loops keep their shape.
- `host/platform/web_dialogs.cpp` stands in for SDL's file dialogs, which Emscripten's SDL3 lacks.
- The whole build uses WebAssembly exceptions (`-fwasm-exceptions`), since the port reports errors as C++ exceptions, and threads (`-pthread`), since the host starts `std::thread`s; eight workers start with the page.
- The kernel's idle hook (`present_until`) gives the browser the time the kernel is about to sleep to keep PSP speed, since a sleep on the page's thread would spin.
- `web/pre.js` mounts `/libsdl`, where `SDL_GetPrefPath` puts the data directory, on IndexedDB before `main()` starts, and writes every change back on its own.

## Milestones

| # | Milestone | Status |
| --- | --- | --- |
| M0 | Docker image; native bootstrap, `--install` and `generate.sh` in the container | Done |
| M1 | Bootstrap compiled to WebAssembly opens an SDL3 canvas | Done: the setup screens run in Chrome with WebGPU and JSPI |
| M2 | Browser main loop, stack, persistent config and saves | Done: JSPI keeps the tab responsive and the data directory persists in IndexedDB. Open risk for M3: the JSPI stack holds about 1 MB (about 15,700 small frames in Chrome), and each guest call nests one large generated function |
| M3 | Generated code linked, overlays interpreted, game data loaded in chunks; frames run without rendering | Frames run: 30 fps at 100% speed in Chrome, 60 display lists a second, nothing drawn yet. The JSPI stack is enough so far. Game data still comes whole, not in chunks |
| M4 | WebGPU renderer; menus visible | |
| M5 | Deployed behind authentication | |
| M6+ | FFmpeg, compiled overlays, threads, CI | |
