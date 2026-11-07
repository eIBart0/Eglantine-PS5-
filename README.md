<p align="center">
  <img src="sce_sys/icon0.png" width="128" alt="EglantinePS5 icon">
</p>

<h1 align="center">EglantinePS5 · NUP build</h1>

<p align="center">
  Run Nintendo Switch 1 and <strong>Switch 2</strong> games on a jailbroken PlayStation 5, from folders or single <code>.nup</code> files.<br>
  Ejecuta juegos de Nintendo Switch 1 y <strong>Switch 2</strong> en una PS5 con jailbreak, desde carpetas o ficheros <code>.nup</code>.
</p>

<p align="center">
  <a href="#english">English</a> · <a href="#español">Español</a> · <a href="../../releases">Releases / Descargas</a>
</p>

> [!WARNING]
> Unofficial, experimental software. It is not affiliated with Sony, Nintendo, the Eglantine team or the Eden team. It contains **no** games, keys or firmware: you must bring your own legally dumped files.
> Software no oficial y experimental. Sin relación con Sony, Nintendo ni los equipos de Eglantine o Eden. **No** incluye juegos, claves ni firmware: tienes que aportar los tuyos, volcados legalmente.

---

# English

## What this is

A PS5 homebrew build that stacks three layers, always updated in this order:

| # | Layer | Source | What we do with it |
|---|---|---|---|
| 1 | **Emulator core** | [Djihads80/Eglantine](https://github.com/Djihads80/Eglantine), branch `nx2-experimental` since v1.219.021 (`nx2` before; a Switch 2 capable fork of Eden) | Pinned as the `Eglantine/` submodule at its **latest** commit. Never forked: our changes are small patches. |
| 2 | **PS5 frontend** | [heydemoura/EglantinePS5](https://github.com/heydemoura/EglantinePS5) (launcher, Vulkan/RADV port, packaging), built on BlackBearReloaded's ProsperoEden | This repository's full history is that project's; we rebase on its `main` and keep our changes on top. |
| 3 | **NUP support** | `.nup` is Eglantine's single-file package (base game + update + DLC) | The library lists `.nup` files, runs them, and can **pack a game folder into a NUP** on the console. |

Current core pin: **Eglantine `ac7882a`** (`nx2-experimental`, `build-219` + 171 commits, 2026-11-07: the branch of Eglantine's test build `branch-nx2-experimental-efb9fb18` plus 12 later commits).

## Status (honest version)

| Item | State |
|---|---|
| Console tested | PS5 firmware **13.60**, jailbroken, launched with ShadowMountPlus; other users run it on **11.40** and **11.60**. |
| Donkey Kong Bananza (Switch 2, `.nup` with update 3.2.0 + DLC) | Runs on the current build (core `3081c8e`) with its update and DLC: **Docked, 1x resolution, 10-25 fps depending on the zone, about 15 in large open areas, 30 fps in cutscenes**. A new zone can show purple the first time it is entered; in one test the game's own "reset terrain" option cleared it. |
| Handheld mode, 0.5x resolution | Not yet confirmed. Earlier failures were caused by a stale shader cache (see below), so they have to be retested. |
| Update and DLC inside a NUP | Both load, and the Library shows them. Checked with Bananza: the game mounts its DLC (log: `NUP add-on 040074A01BF13001 mounted`) and its DLC menu shows 3.2.0 and DLC 1.0.0. The DLC's levels have not been played through yet. |
| Super Mario Bros. Wonder NUP ("Switch 2 Edition + DLC") | The tested file runs as the Switch 1 game with update 1.2.0, and its DLC stays unused: the executables in that file are the Switch 1 build, and the DLC holds Switch 2 data. The Library still lists it. |
| Mario Kart World (Switch 2) | With **Faster GPU emulation** on for the game, v1.219.018 reaches the race at 25-43 fps (0.5x resolution, handheld); with it off, gameplay runs at about 25 of its 30 fps (v1.219.016). Task and mesh draws are off because they hang the PS5's GPU. Every run so far ends after one to three minutes: out of memory (in a race the game itself holds about 5.9 GB), or once a freeze with the GPU thread stopped. The race crash was a single 352 MB request for four 85 MB terrain layers; v1.219.019 uploads those in groups of up to 128 MB, which worked, and then the JIT found no 16 MB block for its code cache while the game's 512 MB memory reserve sat unused. v1.219.020 takes the JIT's memory in 2 MB blocks, lends part of that reserve to the JIT and to a refused graphics allocation (half of it always stays for the game), and gives idle upload buffers back while memory is short (not run yet). Restart the app before each try. |
| v1.219.022 (memory and CPU) | Built 2026-11-07, **not run on a console yet** (v1.219.021, the same with a mimalloc start bug, crashed every game as it started). New core, and every change of the memory and CPU plan at once: thread stacks of 512 KB / 1 MB in the CPU's own memory, presenting at the output resolution, a descriptor queue that grows as needed, mimalloc for the session's allocations, an SSD swap for cold guest memory, the game in a clean process, BC1 textures in Mario Kart World, async shaders in Mario Kart World and Bananza, fewer repeated GPU register writes, thread placement. Each risky piece has a testing switch (see below). |
| The Legend of Zelda: Ocarina of Time (Switch 2) | Plays at about 18 fps on v1.219.015 (one tester's console): its movies and the game show. Its upscaling networks take much of the GPU's time. |
| Donkey Kong Bananza on v1.219.014 | Crashed when many objects broke at once; fixed since v1.219.015. A run on v1.219.016 played at 12-15 fps without that crash. |
| Other Switch 2 titles | Some fail early. Nothing is promised beyond what is listed here. |
| Frame generation (Lossless Scaling) | Compiled in, but pointless at a 20 fps base. |

### Known issue that is fixed here: "stuck at 98% on every launch after the first"

On the console, the Vulkan driver hung a Switch 2 game when it started from the pipeline cache saved by an earlier run. This build neither loads nor saves that global cache (`tools/eglantine-patches/0010-…`); per-game caches still work. If you ever see a hang at 98%, delete `/data/eglantineps5/user/cache/shader/` and tell us (attach `eden_log.txt`).

## Changes in v1.219.022

Every change of the memory and CPU plan in one build, on a new core. v1.219.021 had them too and crashed every game as it started: mimalloc took every thread for the same one (on the PS5 the pointer it reads at `%fs:0` is 0) and skipped its own start; it now takes `pthread_self()`, and a mimalloc that did not start leaves the allocations to the heap. Not run on a console since.

- **Core:** Eglantine `nx2-experimental` at `ac7882a` (its test build `branch-nx2-experimental-efb9fb18` plus 12 commits of 2026-11-07: shader stutter and AMD fixes). It brings Star Fox support and the Mario Kart World and Ocarina of Time work, and it already has 31 of our patches, which are gone. Four were ported by hand (0012, 0015, 0017, 0038), 0051 became the "networks off" switch alone, and 0082-0084 are new.
- **Guest thread stacks:** since core commit `9c83502ed8` every guest thread's 2 MB stack came from the app's heap, in the pool the GPU shares. They are back in the CPU's own memory while 96 MB of it stay free, at 512 KB for the game's threads and 1 MB for the system's.
- **Screen buffers:** the picture goes to the screen at the output resolution, and the console scales it: at 1080p the five framebuffers take 40 MB instead of 160.
- **Descriptor queues:** grow as frames need them (0082) instead of 120 MB taken at start.
- **mimalloc** (v3.6.1, pinned) takes the app's allocations once a game's session starts and gives free memory back to the pool in 2 MB blocks; Sony's allocator kept what it freed.
- **SSD swap:** while memory is short, slots of the game's memory the GPU already has a copy of (textures it uploaded) go to a file on the SSD (`/data/eglantineps5/user/swap/guest.swap`, up to 4 GB) and come back when the game touches them.
- **Game in a clean process:** a game chosen in the library starts in a new process (a few seconds of black screen), without what the launcher left in memory.
- **Mario Kart World:** ASTC textures with transparency as BC1 (opaque ones already were); async shaders by default, also in Donkey Kong Bananza (saved per-game settings still win).
- **CPU:** repeated writes of dynamic-state GPU registers are left out (0083), latched semaphore acquires resume without draining the split's back (0084), and the thread placement works again (its core probe saw one core).
- **Logs:** `EDEN_MEMORY_AT` (pool, heap, guest and flexible memory at app start, launcher and session start), `EDEN_THREAD_CPU` every 20 s, `EDEN_MIMALLOC`, `EDEN_SWAP`, `EDEN_FIBER_STACK` and the descriptor queue's peak.
- **Testing switches** for each of them (Settings > Diagnostics > Testing switches), to turn one off if a game breaks.

## Install

1. Download the release ZIP from **[Releases](../../releases)** and extract it. It contains one folder, `PPSA98515`.
2. Copy that folder to `/data/homebrew/PPSA98515` on the PS5 (or another folder ShadowMountPlus scans, e.g. `/mnt/usb0/homebrew/PPSA98515`). Use [ShadowMountPlus 1.7beta4](https://github.com/drakmor/ShadowMountPlus/releases/tag/1.7beta4) or newer.
3. Put your own **keys, firmware and games** in a *game files folder* (layout and options: [docs/UPSTREAM_README.md](docs/UPSTREAM_README.md#install)). Games go in `roms/`: `.nsp`, `.xci`, **`.nup`**, or an extracted folder.
4. Start **EglantinePS5** from the PS5 home screen.

> [!IMPORTANT]
> This build uses the same title ID as the upstream app (`PPSA98515`), so it **replaces** it. From `v1.206.001` on, the in-app updater checks this repository: when a new release is out it asks first, then downloads it, checks its signature and hash, and replaces the app's files in the folder it is installed in. Earlier builds still ask prosperostore.net, so install `v1.206.001` by hand.

## Version numbers

`v1.206.001` is `1`, Eglantine's `build-206`, and this project's own count, which starts again at `001` with each new core build. The console's content version is the same thing as `01.206.001`.

## Using NUP files

- **Play:** drop a `.nup` in `roms/`. It shows in the Library like any game. A NUP holds the base game, its updates and DLC in one file.
- **Make one from a folder:** for a game kept as an extracted folder, open its settings in the Library (Triangle) and use the **NUP** row. Press twice to confirm; packing runs in the background and the `.nup` appears next to the folder when done. Updates and DLC are included.
- The format and the packer are part of the core (`src/core/file_sys/nup_container.*`, `nup_writer.*`).

## What this repository changes

All changes to the emulator core are plain patches in [`tools/eglantine-patches/`](tools/eglantine-patches), applied automatically at build time; if one stops applying, the build says so.

| Patch | Purpose |
|---|---|
| 0001 | Extracted dumps with `base/` + `updates/<version>/` folders |
| 0004 | NUP packer lists folders through `ListDirectory` (native directory reader on the console) |
| 0005 | Log why an NSO or NUP fails to load |
| 0008 | `nx2:vi2` command 201 stub (Switch 2) |
| 0009 | Guest-thread watchdog written to `eden_log.txt` (diagnosis of hangs) |
| 0010 | Do not load/save the global Vulkan static pipeline cache |
| 0011 | The DLC inside a NUP reaches the game through a content provider (`aoc:u`, data storage by ID) |
| 0012 | Donkey Kong Bananza: command lists that are empty when submitted are read when parsed (ported onto the core's own copying of lists behind a queued GPU map, PR 543) |
| 0014 | Fence downloads: their staging buffers are reused as soon as the fence passes (Bananza ran out of graphics memory) |
| 0015 | Donkey Kong Bananza's upscaler slots, tried when the native program's own image handles form no pair (from [Orchid](https://github.com/MythosOfs/Orchid) `48d369310`, MythosOfs) |
| 0016 | Draws that sample their own colour target read a copy (Orchid `a49c81063`, MythosOfs) |
| 0017 | Page checks skipped for buffer ranges with no new CPU writes (Orchid `a3be6e11b`, `5ffe9eb53`, `4476c8671`, `2e0b577bb`; from monteger8/Eglantine-DKB, Claude for adamd); calls the core's `QueueGpuWrite` |
| 0018 | GPU split: wakes a back thread that stayed parked, also for the idle-hook request the core added in PR 639 (Orchid `1e4e0e82b`, from godpow/Eglantine, godpow) |
| 0019 | Log a guest thread that waits over 5 s for the GPU thread |
| 0020 | Log a kernel call that refuses an enum value (games abort on it with 2001-0120) |
| 0021 | Donkey Kong Bananza's terrain counters (heydemoura/EglantinePS5 patch `0008`) |
| 0022 | Direct guest reads of pages the GPU never wrote (from heydemoura/EglantinePS5 patch `0006`) |
| 0023 | Diagnostic headers the frontend's reports use (heydemoura/EglantinePS5 patch `0004`) |
| 0024 | Room in the GPU command queue for a burst of sparse tile maps, which froze Mario Kart World; each queue entry keeps fewer list entries in place, so the queue stays small |
| 0025 | Oversized native vertex streams are clamped (MythosOfs/Orchid `d7267d7f13`, godpow) |
| 0026 | Syncpoint increments with an id past the table are dropped (Orchid `bb9bdb1ed3`) |
| 0027 | Switch 2 scale, chat and camera commands are answered (Orchid `96dd790493`) |
| 0028 | Module names of Switch 2 programs (Orchid `0c92e5dd0e`) |
| 0029 | Scalar guest reads and writes split at 4 KiB device pages (Orchid `b841a347f2`) |
| 0030 | Syncpoint actions run outside the guard (Orchid `aa266e4577`, from andrewleeog/Eglantine `9ef2dde1a`) |
| 0033 | Texture cache counts what host images really take (Orchid `86587ae279`, godpow) |
| 0034 | Sparse images are backed whole when their tiles or layers cannot be set up (Orchid `64a5e4df86`) |
| 0035 | Released video images return as end-of-stream frames after a movie ends (Orchid `73cbde2a3a`) |
| 0036 | Native lowering of the HMMA matrix instruction, far less shader code (heydemoura/EglantinePS5 patch `0013`, adapted to this core) |
| 0037 | Controller callbacks are registered once the applet resource exists: a motion sample during boot crashed the app |
| 0038 | Task and mesh stages get the push constant range (Orchid `faa12fdbb2`, from Zortos and Eglantine PR 472) |
| 0039 | Reused GPU memory ranges are mapped again when their pages changed (Aubépine `92fe737e62`, from Purebuilt/AMDEGL `f6fba4f`) |
| 0040 | The syncpoint page acknowledges requests against values read after them (jpasqui `04f849c9bb`, without its trace) |
| 0041 | The GPU thread wakes for new submissions while channels wait on semaphores (Orchid `b15e4257b5`) |
| 0042 | Each memory manager's reference counts are sized to its own region (Orchid `d47bb55f34`, from exploitizm) |
| 0043 | Syncpoint actions are identified by id, so a late cancel cannot remove a newer one (Orchid `8837a265cc`, from Zortos and Eglantine PR 472; tests left out) |
| 0045 | Ocarina of Time's GPU-only regions are backed whole (Aubépine `24b7b61718`, adapted) |
| 0046 | Mario Kart World runs without task and mesh draws, which hang the PS5's GPU |
| 0047 | Mario Kart World's present pacing follows the presenting process (Eglantine PR 609; its buffer sizing part is in the core since PR 571) |
| 0051 | `EDEN_NX2_TENSOR=0` (the testing switch) rejects every upscaling network again: the core now reads any value as an emulation mode, and runs Ocarina of Time's networks through its own compact HMMA |
| 0053 | The GPU thread profile (`EDEN_NX2_GPU_PROFILE`) is read again when the GPU thread starts, so the testing switches can turn it on |
| 0070 | With the GPU thread profile on, the guest thread watchdog writes which GPU thread stopped going round its loop and the zone it stopped in |
| 0072 | Layers of a large layered texture are uploaded in groups of up to 128 MB, each finished before the next reuses its staging buffer: four of Mario Kart World's 85 MB terrain layers asked for 352 MB at once and ran the race out of memory |
| 0073 | The GPU thread profile splits "push" into command lists, puller methods, channel binds and list ends, and a GPU thread that comes back with a new game takes its old watchdog place (the old one gave a false alarm) |
| 0082 | The descriptor queues keep their entries in 384 KB blocks a frame takes as it needs them, freed when the frame comes round again: the fixed queues were 96 MB and 24 MB whatever was used; the log says the most a frame used |
| 0083 | A 3D register write that repeats the value it holds is left out when the register only feeds dynamic state (viewport, scissor, blend, depth, stencil...); memory-backed state is still marked on every write, as Eden needs (eden-emu PR 3712). `EDEN_NX2_SKIP_SAME_STATE=0`: as before |
| 0084 | A semaphore acquire the split's back already latched resumes without draining the back (after zortos293/Lichii PR 231 `475c498c1a`, PantACRO4life); `EDEN_NX2_SEM_FAST=0`: as before |

Since v1.219.021 the core is `nx2-experimental`, which has Eglantine's branch `oot-optimization-fixes` merged and Lichii's fixes we carried as 0074-0081: those patches (0055-0069, 0071, 0074-0081) and 0002, 0032, 0044, 0048, 0049, 0050 and 0052 are gone, because the core has them. Number 0006 is intentionally missing: 32 storage buffers per stage is now in the core itself. Frontend changes (NUP in the library, "pack to NUP", `.gitmodules` pointing at Djihads80's core, Vulkan-port anchors for the newest core, per-line log flushing) are ordinary commits on top of the upstream frontend.

Logs on the console: `/data/eglantineps5/user/log/eden_log.txt` (and `.old.txt`), `/data/eglantineps5/logs/`. Settings: `/data/eglantineps5/config/eglantineps5.json`.

**Testing switches:** Settings > Diagnostics > Testing switches changes the emulator's switches (`EDEN_NX2_...`) from the menu: upscaling networks, task and mesh draws, the split GPU thread, the texture memory limit, the texture cache log, the GPU thread profile, CPU writes on GPU pages and others, and since v1.219.021 the memory and CPU ones (guest thread stacks, screen buffers, smaller textures, thread placement, game in a clean process, app memory allocator, SSD swap, repeated register writes); each shows which of its choices is the default. They are saved to `/data/eglantineps5/config/env.txt` (one `NAME=VALUE` per line, which can also be edited by hand) and apply from the next start, so the app asks to confirm and closes.

## Updating and building

Update order, every time: **(1)** move the `Eglantine/` submodule to the newest Djihads80 `nx2` commit and re-apply the patches (`tools/update-eglantine.sh`; needs bash ≥ 4 because `apply-eglantine-patches.sh` uses `mapfile`, so on macOS use Homebrew bash or a Linux container); **(2)** merge the newest heydemoura/EglantinePS5 `main`; **(3)** check NUP loading and packing; **(4)** build, test on the console, publish.

Building follows [docs/BUILDING.md](docs/BUILDING.md) (`make package`, Linux x86-64 toolchain). On Apple Silicon we build in an `linux/amd64` Docker container (about 45 minutes for a clean build). Releases are the folder `PPSA98515/` zipped; the release notes list the core commit and the `eboot.bin` SHA-256.

## Credits and license

GPL-3.0-or-later (see [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). The complete source of every release is this repository plus the pinned `Eglantine/` submodule commit.

Thanks to: **Djihads80 and the Eglantine contributors**; the **Eden** and yuzu projects; **heydemoura** (EglantinePS5 frontend and PS5 port work); **BlackBearReloaded** (ProsperoEden, ps5-opengl, PS5 native app boilerplate); **Mihawk** (PS5 Mesa/RADV and PS5 Vulkan); **mpereiraesaa** (ps5-vulkan); **drakmor** (ShadowMountPlus). The detailed credit list is in [docs/UPSTREAM_README.md](docs/UPSTREAM_README.md).

---

# Español

## Qué es

Una build homebrew para PS5 que apila tres capas, siempre actualizadas en este orden:

| # | Capa | Origen | Qué hacemos |
|---|---|---|---|
| 1 | **Núcleo del emulador** | [Djihads80/Eglantine](https://github.com/Djihads80/Eglantine), rama `nx2-experimental` desde la v1.219.021 (antes `nx2`; fork de Eden con soporte Switch 2) | Submódulo `Eglantine/` fijado a su **último** commit. No lo forkeamos: nuestros cambios son parches pequeños. |
| 2 | **Frontend de PS5** | [heydemoura/EglantinePS5](https://github.com/heydemoura/EglantinePS5) (lanzador, port de Vulkan/RADV, empaquetado), basado en ProsperoEden de BlackBearReloaded | El historial de este repo es el de ese proyecto; rebaseamos sobre su `main` y mantenemos nuestros cambios encima. |
| 3 | **Soporte NUP** | `.nup` es el paquete de un solo fichero de Eglantine (juego base + update + DLC) | La biblioteca lista y ejecuta `.nup` y puede **empaquetar una carpeta de juego en un NUP** en la propia consola. |

Core actual: **Eglantine `ac7882a`** (`nx2-experimental`, `build-219` + 171 commits, 7-nov-2026: la rama de la build de prueba `branch-nx2-experimental-efb9fb18` más 12 commits posteriores).

A Switch 2 game (title IDs that start with `0400`) starts from the emulator core's own defaults for its games instead of the general values: **Faster GPU emulation** on and **Reactive flushing** off. Without them the core's second GPU thread stays off and every GPU fence waits for the console's GPU; Mario Kart World's title screen ran at about 11 FPS with the general defaults and about 24 FPS with these (see [docs/MKW-TITLE-FPS.md](docs/MKW-TITLE-FPS.md)). The game's own values, set in the Library, still win.

A game can have its own values: in its settings, **Performance** (see [Settings per game](#settings-per-game)). In the settings file, `"performance"` at the top level is what the switches set, and one inside a game's entry under `"games"` is that game's, whose values go first:

## Estado (versión honesta)

| Punto | Estado |
|---|---|
| Probado en consola | PS5 con firmware **13.60**, con jailbreak y ShadowMountPlus; otros usuarios la usan en **11.40** y **11.60**. |
| Donkey Kong Bananza (Switch 2, `.nup` con update 3.2.0 + DLC) | Funciona con la build actual (core `3081c8e`) con su update y su DLC: **modo TV, resolución 1x, 10-25 fps según la zona, unos 15 en zonas abiertas grandes, 30 fps en cinemáticas**. Una zona nueva puede verse morada la primera vez que se entra; en una prueba, la opción del juego «resetear terreno» lo arregló. |
| Modo portátil, resolución 0,5x | Sin confirmar. Los fallos anteriores venían de una caché de shaders antigua (ver abajo), así que hay que repetir la prueba. |
| Update y DLC dentro de un NUP | Se cargan los dos y la biblioteca los muestra. Comprobado con Bananza: el juego monta su DLC (log: `NUP add-on 040074A01BF13001 mounted`) y su menú de DLC muestra 3.2.0 y DLC 1.0.0. Los niveles del DLC aún no se han jugado a fondo. |
| NUP de Super Mario Bros. Wonder («Switch 2 Edition + DLC») | El archivo probado se ejecuta como el juego de Switch 1 con la update 1.2.0, y su DLC queda sin usar: los ejecutables de ese archivo son la versión de Switch 1 y el DLC contiene datos de Switch 2. La biblioteca lo sigue listando. |
| Mario Kart World (Switch 2) | Con **Emulación de GPU más rápida** activada para el juego, la v1.219.018 llega a la carrera a 25-43 fps (resolución 0,5x, modo portátil); sin ella, la partida va a unos 25 de sus 30 fps (v1.219.016). Los dibujos task y mesh están desactivados porque cuelgan la GPU de la PS5. Hasta ahora todas las partidas acaban a los uno a tres minutos: sin memoria (en carrera el juego ocupa unos 5,9 GB) o, una vez, colgado con el hilo de la GPU parado. El cierre en carrera fue una petición de 352 MB para cuatro capas de terreno de 85 MB; la v1.219.019 las sube en grupos de 128 MB como máximo, cosa que funcionó, y después el JIT no encontró un bloque de 16 MB para su caché de código con la reserva de 512 MB del juego sin usar. La v1.219.020 da al JIT su memoria en bloques de 2 MB, presta parte de esa reserva al JIT y a una petición gráfica rechazada (siempre queda la mitad para el juego) y devuelve los búferes de subida sin uso cuando falta memoria (sin probar aún). Reinicia la app antes de cada intento. |
| The Legend of Zelda: Ocarina of Time (Switch 2) | Se juega a unos 18 fps en la v1.219.015 (en la consola de un probador): se ven sus vídeos y el juego. Sus redes de escalado ocupan buena parte del tiempo de la GPU. |
| Donkey Kong Bananza en la v1.219.014 | Se cerraba al romperse muchos objetos a la vez; arreglado desde la v1.219.015. Una partida en la v1.219.016 fue a 12-15 fps sin ese cierre. |
| Otros juegos de Switch 2 | Algunos fallan pronto. No prometemos nada más allá de lo que aparece aquí. |
| Generación de fotogramas (Lossless Scaling) | Incluida, pero sin sentido con una base de 20 fps. |

### Problema conocido y arreglado: «se queda al 98% en cada arranque después del primero»

En la consola, el driver de Vulkan colgaba un juego de Switch 2 cuando arrancaba desde la caché de pipelines guardada por una ejecución anterior. Esta build ni carga ni guarda esa caché global (`tools/eglantine-patches/0010-…`); las cachés por juego siguen funcionando. Si alguna vez ves un cuelgue al 98%, borra `/data/eglantineps5/user/cache/shader/` y avísanos (adjunta `eden_log.txt`).

## Instalación

1. Descarga el ZIP desde **[Releases](../../releases)** y descomprímelo. Contiene una carpeta, `PPSA98515`.
2. Copia esa carpeta a `/data/homebrew/PPSA98515` en la PS5 (o a otra carpeta que escanee ShadowMountPlus, p. ej. `/mnt/usb0/homebrew/PPSA98515`). Usa [ShadowMountPlus 1.7beta4](https://github.com/drakmor/ShadowMountPlus/releases/tag/1.7beta4) o posterior.
3. Pon tus **claves, firmware y juegos** en una *carpeta de archivos de juego* (estructura y opciones: [docs/UPSTREAM_README.md](docs/UPSTREAM_README.md#install), en inglés). Los juegos van en `roms/`: `.nsp`, `.xci`, **`.nup`** o una carpeta extraída.
4. Abre **EglantinePS5** desde la pantalla de inicio de la PS5.

> [!IMPORTANT]
> Esta build usa el mismo ID de título que la app original (`PPSA98515`), así que la **sustituye**. Desde `v1.206.001`, el actualizador integrado consulta este repositorio: cuando sale una release nueva pregunta primero, la descarga, comprueba su firma y su hash, y sustituye los archivos de la app en la carpeta donde esté instalada. Las builds anteriores siguen consultando prosperostore.net, así que instala `v1.206.001` a mano.

## Números de versión

`v1.206.001` es `1`, la `build-206` de Eglantine y un contador propio de este proyecto, que vuelve a `001` con cada build nueva del core. En la consola, la versión de contenido es lo mismo: `01.206.001`.

## Usar ficheros NUP

- **Jugar:** deja un `.nup` en `roms/`. Aparece en la biblioteca como cualquier juego. Un NUP contiene el juego base, sus updates y DLC en un fichero.
- **Crear uno desde una carpeta:** para un juego guardado como carpeta extraída, abre sus ajustes en la biblioteca (Triángulo) y usa la fila **NUP**. Pulsa dos veces para confirmar; el empaquetado se hace en segundo plano y el `.nup` aparece junto a la carpeta al terminar. Incluye updates y DLC.
- El formato y el empaquetador forman parte del core (`src/core/file_sys/nup_container.*`, `nup_writer.*`).

## Qué cambia este repositorio

Todos los cambios al core del emulador son parches simples en [`tools/eglantine-patches/`](tools/eglantine-patches), que se aplican solos al compilar; si uno deja de aplicar, la build lo dice. La tabla de parches está en la sección en inglés. Falta el 0006 a propósito: las 32 storage buffers por etapa ya vienen en el core. Los cambios del frontend (NUP en la biblioteca, «empaquetar a NUP», `.gitmodules` apuntando al core de Djihads80, anclajes del port de Vulkan para el core más nuevo, volcado del log línea a línea) son commits normales encima del frontend original.

Logs en la consola: `/data/eglantineps5/user/log/eden_log.txt` (y `.old.txt`), `/data/eglantineps5/logs/`. Ajustes: `/data/eglantineps5/config/eglantineps5.json`.

**Interruptores de prueba:** Ajustes > Diagnóstico > Interruptores de prueba cambia desde el menú los interruptores del emulador (`EDEN_NX2_...`): redes de escalado, dibujos task y mesh, el hilo de GPU dividido, el límite de memoria de texturas, el registro de texturas, el perfil del hilo de GPU, las escrituras de CPU en páginas de GPU y otros; cada uno indica cuál de sus opciones es la de por defecto. Se guardan en `/data/eglantineps5/config/env.txt` (un `NOMBRE=VALOR` por línea, que también se puede editar a mano) y se aplican desde el siguiente arranque, así que la app pide confirmación y se cierra.

## Actualizar y compilar

Orden de actualización, siempre: **(1)** subir el submódulo `Eglantine/` al último commit de `nx2` de Djihads80 y reaplicar los parches (`tools/update-eglantine.sh`; necesita bash ≥ 4 porque `apply-eglantine-patches.sh` usa `mapfile`, así que en macOS usa bash de Homebrew o un contenedor Linux); **(2)** fusionar el último `main` de heydemoura/EglantinePS5; **(3)** comprobar la carga y el empaquetado de NUP; **(4)** compilar, probar en consola y publicar.

La compilación sigue [docs/BUILDING.md](docs/BUILDING.md) (`make package`, toolchain Linux x86-64). En Apple Silicon compilamos dentro de un contenedor Docker `linux/amd64` (unos 45 minutos una build limpia). Las releases son la carpeta `PPSA98515/` comprimida; las notas indican el commit del core y el SHA-256 del `eboot.bin`.

## Créditos y licencia

GPL-3.0-or-later (ver [LICENSE](LICENSE) y [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). El código fuente completo de cada release es este repositorio más el commit fijado del submódulo `Eglantine/`.

Gracias a: **Djihads80 y los colaboradores de Eglantine**; los proyectos **Eden** y yuzu; **heydemoura** (frontend EglantinePS5 y port a PS5); **BlackBearReloaded** (ProsperoEden, ps5-opengl, boilerplate nativo de PS5); **Mihawk** (PS5 Mesa/RADV y PS5 Vulkan); **mpereiraesaa** (ps5-vulkan); **drakmor** (ShadowMountPlus). La lista de créditos detallada está en [docs/UPSTREAM_README.md](docs/UPSTREAM_README.md).
