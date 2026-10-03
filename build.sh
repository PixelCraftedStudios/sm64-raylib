#!/bin/bash
set -e

# ---------------------------------------------------------------------------
# One-time setup (safe to run every time): make sure the original Windows
# backends can't clash with the raylib ones. The raylib files define the
# engine's ORIGINAL symbol names (audio_wasapi, controller_xinput), so the
# originals must not also define them.
# ---------------------------------------------------------------------------

# Controller: move the original xinput backend out of the way (.c or .cpp).
for ext in c cpp; do
    f="src/pc/controller/controller_xinput.$ext"
    if [ -f "$f" ]; then
        mv "$f" "$f.bak"
    fi
done

# Audio: the Makefile lists audio_wasapi.o explicitly, so keep the file but
# empty it. Works whether the original is .c or .cpp.
for ext in cpp c; do
    f="src/pc/audio/audio_wasapi.$ext"
    if [ -f "$f" ]; then
        # Only back up the real original, never an already-stubbed file.
        if [ ! -f "$f.bak" ]; then
            cp "$f" "$f.bak"
        fi
        printf '/* Disabled: replaced by audio_raylib.c */\ntypedef int audio_wasapi_disabled;\n' > "$f"
    fi
done

echo "==== Step 1: Building tools ===="
make VERSION=us tools CXX="g++" CC="gcc" -j4

# ---------------------------------------------------------------------------
# Force every object that depends on the renames or on the replaced files to
# rebuild. make can't see that CC flags changed, so stale objects are what
# caused the "multiple definition" / "undefined reference" errors.
# ---------------------------------------------------------------------------
rm -f build/us_pc/src/pc/pc_main.o \
      build/us_pc/src/pc/gfx/gfx_raylib.o \
      build/us_pc/src/pc/audio/audio_raylib.o \
      build/us_pc/src/pc/audio/audio_wasapi.o \
      build/us_pc/src/pc/controller/controller_raylib.o \
      build/us_pc/src/pc/controller/controller_entry_point.o

echo "==== Step 2: Building the game ===="
# Only the graphics backend needs a -D redirect (pc_main.c picks DX11 by default).
# Audio and controller use the original symbol names directly.
make VERSION=us \
     CC="gcc -Dgfx_dxgi_api=gfx_dummy_wm_api -Dgfx_direct3d11_api=gfx_dummy_renderer_api -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_33" \
     LDFLAGS="-lm -no-pie -mwindows -lraylib -lglfw3 -lopengl32 -lgdi32 -lwinmm -lole32 -lshell32" \
     OBJS="build/us_pc/src/pc/gfx/gfx_raylib.o build/us_pc/src/pc/audio/audio_raylib.o build/us_pc/src/pc/controller/controller_raylib.o" \
     -j4

echo "==== Done! ===="