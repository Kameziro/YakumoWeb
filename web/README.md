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
    -DMHP3RD_GENERATED_CODE=OFF
cmake --build out/web --target Yakumo
mkdir -p build/web && cp out/web/bin/Yakumo.* build/web/
```

Then serve `build/web` over HTTP (for example `python -m http.server 8765 --directory build/web`) and open `Yakumo.html` in a current Chrome or Edge. The page needs WebGPU and WebAssembly JSPI.

How the port fits in:

- `host/gpu/webgpu_renderer.cpp` implements `gpu::VulkanRenderer` with WebGPU. `vulkan_renderer.hpp` exposes no Vulkan types, so the kernel and the interface use it unchanged. So far it draws the interface only.
- Each presented frame waits for the browser's next animation frame through JSPI, which suspends the wasm stack. The port's blocking loops keep their shape.
- `host/platform/web_dialogs.cpp` stands in for SDL's file dialogs, which Emscripten's SDL3 lacks.

## Milestones

| # | Milestone | Status |
| --- | --- | --- |
| M0 | Docker image; native bootstrap, `--install` and `generate.sh` in the container | Done |
| M1 | Bootstrap compiled to WebAssembly opens an SDL3 canvas | Done: the setup screens run in Chrome with WebGPU and JSPI |
| M2 | Browser main loop, stack, persistent config and saves | |
| M3 | Generated code linked, overlays interpreted, game data loaded in chunks; frames run without rendering | |
| M4 | WebGPU renderer; menus visible | |
| M5 | Deployed behind authentication | |
| M6+ | FFmpeg, compiled overlays, threads, CI | |
