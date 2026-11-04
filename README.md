# JJFFE x64 / Vulkan

Port of **JJFFE 2.8** (Frontier: First Encounters, decompiled by John Jordan)
from 32-bit Windows (DirectDraw / DirectInput / DirectSound / DirectMusic)
to **64-bit Windows with a Vulkan renderer**. The same code also builds on
Linux (SDL2 + Vulkan).

> JJFFE is only a replacement executable – it still needs the game data from
> FFE (shareware or full version). See `jjffe28.txt`.

## How the port works

The game core is ~133,000 lines of hand-decompiled 32-bit x86 assembly
(`ffe.asm`, `ffedat.asm`, `ffebmp.asm`). A 64-bit process cannot run 32-bit
code, and `push`/`pop` with 32-bit registers, `pushad` and the 32-bit calling
convention don't exist in x64. So:

1. **`tools/asm2c.py`** – a static recompiler (x86-32 NASM → C). Each
   instruction becomes C code operating on emulated registers, lazily
   evaluated flags and an x87 FPU. The result is in `gen/` (committed, so
   building doesn't need Python).
2. **32-bit guest memory** – all game data lives in one memory block
   (`g_mem`), and guest pointers are 32-bit offsets. That way pointer tables
   (`dd DATA_xxx`), structures and the original pointer arithmetic work
   unchanged on a 64-bit host.
3. **`runtime/`** – guest memory and heap, calls/jumps (including function
   pointer tables), `SetJmp`/`LongJmp`, and *thunks* that pass calls from the
   game to the C library (`fopen`, `sprintf`, …) and to the platform API
   (`ffeapi.h`, now with fixed-size `int32_t` types instead of `long`).
4. **`platform/`** – new platform layer:
   * `vulkan/vkpresent.c` – Vulkan renderer: uploads the 320×200 frame to a
     texture and scales it into the swapchain (`vkCmdBlitImage`,
     letterboxing, nearest/bilinear filter, vsync). The Vulkan loader is
     loaded at runtime through **volk**, so you don't need `vulkan-1.lib`,
     just a Vulkan driver.
   * `win64/` – Windows x64: Win32 window, keyboard (scancodes from
     `WM_KEYDOWN`), mouse (Raw Input), timer (QPC), sound (own mixer +
     `waveOut`), music (HMP→MIDI + MCI), joystick (winmm).
   * `sdl2/` – Linux and other systems (SDL2 + Vulkan).
   * `common/` – shared framebuffer/palette, keyboard with key repeat,
     sample mixer.
5. **`third_party/`** – Khronos **Vulkan-Headers 1.4.309** and the **volk**
   loader (vendored, Apache-2.0 / MIT licences).

There is no OpenGL anywhere: the old SDL 1.2 + OpenGL backend (`sdl/`, `ffelnxsdl.mak`, `jjffesdl.dsp`) has been removed and Vulkan is the only renderer. The old 32-bit DirectX backend (`win32/`, `jjffew32.dsp`) has been removed as well; `platform/win64/` replaces it.

## Building

### Windows x64 (Visual Studio 2019/2022)

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

Output: `build\Release\ffewin64.exe` + `ffewin64.cfg`. Copy both into the
FFE game directory (the one with `mission.dat`).

### Windows x64 – cross-compiling with MinGW-w64 (from Linux)

```sh
cmake -S . -B build-win64 -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
cmake --build build-win64 -j
```

### Linux (SDL2 + Vulkan)

```sh
sudo apt install libsdl2-dev
cmake -S . -B build && cmake --build build -j
```

### Regenerating C code from the assembly

After changing `ffe.asm`/`ffedat.asm`/`ffebmp.asm`:

```sh
python3 tools/asm2c.py . gen
# or: cmake -DFFE_REGENERATE=ON ...
```

## Configuration (`ffewin64.cfg`)

| Section | Key | Meaning |
|---|---|---|
| VIDEO | `fullscreen` | 1 = borderless fullscreen (Alt+Enter / Ctrl+F12 toggles) |
| VIDEO | `winwidth`, `winheight` | window size |
| VIDEO | `vsync` | 1 = FIFO, 0 = mailbox/immediate |
| VIDEO | `bilinear` | bilinear filtering when scaling |
| VIDEO | `aspect43` | 1 = 4:3 image (like a CRT), 0 = 16:10 |
| VIDEO | `validation` | Vulkan validation layer (debugging) |
| SOUND | `nosound`, `nomusic` | turns sound/music off |

## Tests

```sh
cmake -S . -B build -DFFE_HEADLESS_TEST=ON && cmake --build build -j
tests/run_headless.sh build/ffetest
```

The headless test runs the game without a window, simulates keypresses
(`FFE_KEYS="ms:scancode,..."`) and saves screenshots. The same variables
(`FFE_KEYS`, `FFE_FRAMES`, `FFE_SCREENSHOT`) also work in `ffewin64.exe` and
`ffesdl2`.

Tested: the core runs on x86-64 (20,000 frames in a stress test, intro, game,
maps, Imperial Herald); `ffewin64.exe` (PE32+ x86-64) runs with Vulkan under
Wine + lavapipe; the Linux version under Xvfb + lavapipe with no errors from
the Khronos validation layer.
