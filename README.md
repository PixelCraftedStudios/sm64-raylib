# Super Mario 64 - Raylib Port Template

This repository contains a modular, high-performance standalone port of **Super Mario 64** driven entirely by the **Raylib ecosystem**. 

By decoupling the engine from legacy DirectX 11 window frames and WASAPI audio mixers, this fork implements custom GLSL-generated hardware shader pipelines via `rlgl`, an atomic lock-free power-of-two audio ring buffer streaming natively at 32kHz, and unified 360-degree analog gamepad and keyboard layout polling handlers. 

Because it communicates exclusively using universal Raylib abstractions, this codebase is free of platform-locked dependencies and is ready to be compiled across multiple target device architectures.

This repository does not include copyrighted game assets necessary for compiling the executable. A prior copy of an original game ROM is required to extract assets during the initial asset build phase.

---

## Workspace Additions & Changes
* **`build.sh`**: Local automation script containing multi-threaded compiler macro shortcut parameters.
* **`src/pc/gfx/gfx_raylib.c`**: Custom desktop-level GLSL program generator and vertex array buffer loader using the `rlgl` layer.
* **`src/pc/audio/audio_raylib.c`**: Ultra-smooth, lock-free consumer-threaded ring buffer audio stream running at 32kHz.
* **`src/pc/controller/controller_raylib.c`**: Parallel input driver mapping keyboard matrices and 360-degree analog thumsticks to N64 inputs.

---

## Building the Raylib Port Natively

### Linux / Steam Deck
1. Install compiler tools and hardware libraries (Ubuntu/Debian):
   ```bash
   sudo apt install -y git build-essential pkg-config libusb-1.0-0-dev libglfw3-dev libraylib-dev
   ```
2. Place a valid Super Mario 64 ROM called `baserom.<VERSION>.z64` into the repository root directory (where `VERSION` can be `us`, `jp`, or `eu`).
3. Execute compilation using compiler injections to bind your Raylib backend targets:
   ```bash
   make VERSION=us CC="gcc -Dgfx_dxgi_api=gfx_dummy_wm_api -Dgfx_direct3d11_api=gfx_dummy_renderer_api -Daudio_wasapi=audio_raylib_api -Dcontroller_xinput=controller_raylib_api" LDFLAGS="-lm -lraylib -lglfw -lGL" OBJS="build/us_pc/src/pc/gfx/gfx_raylib.o build/us_pc/src/pc/audio/audio_raylib.o build/us_pc/src/pc/controller/controller_raylib.o" -j$(nproc)
   ```

### Windows (MSYS2 MinGW-w64)
1. Download and install the standard Windows distribution installer from [msys2.org](https://www.msys2.org/).
2. From your Windows Start Menu, launch the **MSYS2 MinGW 64-bit** terminal (do NOT use the base MSYS terminal shell).
3. Install the compilation environment and Raylib packages:
   ```bash
   pacman -S git make python3 mingw-w64-x86_64-gcc mingw-w64-x86_64-raylib mingw-w64-x86_64-glfw3
   ```
4. Change directory to your local repository workspace clone path (e.g., `cd /c/Users/<username>/Documents/ray64`).
5. Place a valid Super Mario 64 ROM called `baserom.<VERSION>.z64` into the repository root directory.
6. Run the local automation script to refresh dependencies and compile using 12 concurrent hardware build threads:
   ```bash
   ./build.sh
   ```
   *Alternatively, if running the raw compilation manually, use:*
   ```bash
   touch src/pc/controller/controller_entry_point.c
   make VERSION=us CC="gcc -Dgfx_dxgi_api=gfx_dummy_wm_api -Dgfx_direct3d11_api=gfx_dummy_renderer_api -Daudio_wasapi=audio_raylib_api -Dcontroller_xinput=controller_raylib_api -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_33" LDFLAGS="-lm -no-pie -mwindows -lraylib -lglfw3 -lopengl32 -lgdi32 -lwinmm -lole32 -lshell32" OBJS="build/us_pc/src/pc/gfx/gfx_raylib.o build/us_pc/src/pc/audio/audio_raylib.o build/us_pc/src/pc/controller/controller_raylib.o" -j12
   ```
7. Your fully standalone executable file will be generated at `./build/us_pc/sm64.us.exe`.

---

## Default Controls Mapping Layout

The engine continuously processes both inputs concurrently, allowing you to swap between keyboard and USB controllers mid-game on the fly.

| N64 Action | ⌨️ Keyboard Mapping | 🎮 Universal Gamepad Input |
| :--- | :--- | :--- |
| **3D Movement** | **W / A / S / D** (Digital) | **Left Analog Stick** (True 360° Velocity) |
| **A Button** (Jump) | **Space** | **Face Button Down** (A / Cross) |
| **B Button** (Attack) | **X** | **Face Button Left** (X / Square) |
| **Z Trigger** (Crouch)| **Z** | **Left Shoulder Bumper** (L1 / LB) |
| **R Button** (Camera) | *None* | **Right Shoulder Bumper** (R1 / RB) |
| **START Button** | **Enter** | **Center Right Menu Button** (Options) |
| **C-Buttons** (Camera) | **Arrow Keys** (Up/Down/Left/Right) | **Right Analog Stick** (Tilt Vector) |

---

## Project Structure
	
	sm64
	├── actors: object behaviors, geo layout, and display lists
	├── asm: handwritten assembly code, rom header
	├── assets: animation and demo data
	├── bin: C files for ordering display lists and textures
	├── build: output binary cache compilation directory
	├── data: behavior scripts, misc. data
	├── include: core engine header files
	├── levels: level scripts, geo layout, and display lists
	├── lib: SDK library code
	├── rsp: audio and Fast3D RSP assembly code
	├── sound: sequences, sound samples, and sound banks
	├── src: C source code for game
	│   ├── audio: native audio engines
	│   │   └── audio_raylib.c  <-- [Added] Custom 32kHz ring-buffered driver
	│   ├── game: behaviors and rest of game source
	│   ├── goddard: Mario intro face engine screen
	│   ├── menu: title screen and file select asset layout managers
	│   └── pc: port code and peripheral renderer structures
	│       ├── gfx
	│       │   └── gfx_raylib.c <-- [Added] Custom shader & vertex backend
	│       └── controller
	│           └── controller_raylib.c <-- [Added] Unified gamepad/key API
	├── text: dialog strings, level names, act names
	├── textures: skybox and level texture blocks
	└── tools: system build tools and assemblers
