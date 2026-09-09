# Luna

Luna is a small native runtime for Lua-driven 2D applications. It combines a coroutine-based Lua host with a build-time-selectable rendering backend, SDL3 windowing/input, and SDL3_mixer audio playback.

The executable treats the selected root directory as its runtime root, loads a Lua entry file from the file VFS at startup (default `main.lua`), and exposes engine, rendering, content-loading, audio, and save-file helpers through `require("luna")`.

## What It Includes

- LuaJIT embedded directly in the project
- SDL3 windowing, input, and event pumping
- GPU-accelerated Skia Graphite backend
- CPU-based Blend2D backend
- Canvas-style drawing APIs for rectangles, images, text, and paragraphs
- Asynchronous file-backed loading for images, font faces, and audio assets
- A small event/promise model for coordinating background work from Lua
- Sandboxed save-data helpers with JSON read/write support

## Project Layout

- `src/`: engine, renderer, canvas, audio mixer, logging, and VFS implementation
- `vendor/`: vendored LuaJIT build integration and renderer submodules
- `vcpkg-ports/`: overlay ports for Skia and SDL3-related packages
- `vcpkg-triplets/`: custom triplets used by the provided CMake presets
- `docs/`: runtime API, rendering, storage, packaging, and testing guides
- `test/`: native regression suites and Lua demos

## Requirements

- CMake 3.20 or newer
- Ninja
- A working `vcpkg` installation exposed through `VCPKG_ROOT`
- A C++20-capable toolchain
- A Vulkan-capable system and graphics driver when the Skia backend is enabled

The repository is set up around `vcpkg` manifests and CMake presets. Dependencies such as `fmt`, `SDL3`, `SDL3_mixer`, `Skia`, and Vulkan Memory Allocator are resolved through `vcpkg`. LuaJIT is vendored as `vendor/luajit` and must be initialized for every build. The Blend2D backend additionally expects `vendor/asmjit` and `vendor/blend2d` to be initialized as git submodules.

## Building

### Linux

The repo includes a preset for the custom `x64-linux-clang-lto` triplet:

```bash
git submodule update --init --recursive
cmake --preset vcpkg-x64-linux
cmake --build build
```

### Windows

The repo also includes a preset for the custom `x64-windows-clangcl-lto` triplet:

```powershell
git submodule update --init --recursive
cmake --preset vcpkg-x64-windows
cmake --build build
```

If `VCPKG_ROOT` is not set, CMake will not be able to find the toolchain file used by the presets.

Backend selection is controlled at configure time:

```bash
cmake --preset vcpkg-x64-linux -DLUNA_BACKEND_SKIA=ON -DLUNA_BACKEND_BLEND2D=ON
cmake --build build
```

If you want Blend2D in the build, initialize the renderer submodules first:

```bash
git submodule update --init --recursive
cmake --preset vcpkg-x64-linux -DLUNA_BACKEND_BLEND2D=ON
cmake --build build
```

To build Skia only and skip the Blend2D submodules:

```bash
git submodule update --init vendor/luajit
cmake --preset vcpkg-x64-linux -DLUNA_BACKEND_BLEND2D=OFF
cmake --build build
```

To build Blend2D only and skip Vulkan at runtime:

```bash
git submodule update --init --recursive
cmake --preset vcpkg-x64-linux -DLUNA_BACKEND_SKIA=OFF -DLUNA_BACKEND_BLEND2D=ON
cmake --build build
```

## Running

By default, Luna uses the current working directory as the root. It reads
`game.json` when present and uses its entry, otherwise `main.lua`:

```bash
./build/luna
```

You can also provide an explicit Lua entry path in the file VFS:

```bash
./build/luna --script path/to/main.lua
```

Named flags are available when the root and script path need to be set
independently:

```bash
./build/luna --root path/to/game --script scripts/main.lua
```

If the build includes more than one renderer backend, you can select one at
startup with `LUNA_BACKEND`:

```bash
LUNA_BACKEND=skia ./build/luna
LUNA_BACKEND=blend2d ./build/luna --script path/to/main.lua
```

Accepted values are `skia` and `blend2d` (`blend2d_cpu` is also accepted as an
alias). If `LUNA_BACKEND` is unset, Luna keeps its default backend selection.
If you request a backend that was not compiled into the current build, startup
fails with a clear error.

Blend2D also accepts an optional worker-count parameter through
`LUNA_BLEND2D_THREAD_COUNT`:

```bash
LUNA_BACKEND=blend2d LUNA_BLEND2D_THREAD_COUNT=4 ./build/luna
```

Set `LUNA_BLEND2D_THREAD_COUNT=0` to keep Blend2D in synchronous mode.

On Windows, run the generated `luna.exe` from the build output directory.

## Minimal Lua Example

```lua
local luna = require("luna")

local function main()
  while true do
    local events = luna.poll_events()
    for _, event in ipairs(events) do
      if event.type == "quit" then
        return
      end
    end

    luna.window:clear(0xFF1D2026)
    luna.window:draw_rect(100, 100, 320, 180, {
      shader = 0xFFFFCC66,
      style = "fill",
    })

    luna.next_frame()
  end
end

luna.start(main)
```

## Documentation

- [Lua runtime and tasks](docs/lua-api.md)
- [Canvas, paths, shaders, and text](docs/canvas.md)
- [Filesystem and asset API](docs/fs-assets.md)
- [Game archives and portable distribution](docs/game-archive.md)
- [Testing and contributing tests](docs/testing.md)

Engine documentation belongs here. Scenario logic, native-game recovery,
reference observations, and port-specific validation belong in the companion
game project.

## Packaging

Package a stable content directory (including `game.json`) and run it directly:

```sh
./build/luna_pack create path/to/content game.luna
./build/luna_pack verify game.luna
./build/luna --root game.luna
```

Archives use independently checked 256 KiB LZ4/stored chunks for seekable game
access. A directory manifest can mount multiple archives with `"type": "package"`.
See [the archive format and distribution guide](docs/game-archive.md).
