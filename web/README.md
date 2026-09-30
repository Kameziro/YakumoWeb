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
profiles/mhp3rd/scripts/generate.sh
```

## Milestones

| # | Milestone | Status |
| --- | --- | --- |
| M0 | Docker image; native bootstrap, `--install` and `generate.sh` in the container | Bootstrap builds; install and generate not yet run |
| M1 | Bootstrap compiled to WebAssembly opens an SDL3 canvas | |
| M2 | Browser main loop, stack, persistent config and saves | |
| M3 | Generated code linked, overlays interpreted, game data loaded in chunks; frames run without rendering | |
| M4 | WebGPU renderer; menus visible | |
| M5 | Deployed behind authentication | |
| M6+ | FFmpeg, compiled overlays, threads, CI | |
