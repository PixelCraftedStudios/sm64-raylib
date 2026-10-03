// Raylib backend for the SM64-style gfx engine.
//
// The old version drew with raylib's fixed-function path, which has no way to
// express the N64 color combiner. This version generates one GLSL program per
// shader_id (same logic as gfx_opengl.c) and feeds the engine's vertex buffer
// straight to the GPU via rlgl, so w, vertex colors, textures, fog and alpha
// all behave like the reference OpenGL backend.
//
// Requires raylib built for desktop OpenGL 3.3 (the default).

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

#include "raylib.h"
#include "rlgl.h"

#include "gfx_cc.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"

// ----------------------------------------------------------------------------
// State
// ----------------------------------------------------------------------------

#define MAX_ATTRIBS 7
// Fixed attribute locations used in the generated GLSL.
#define LOC_POS   0
#define LOC_UV    1
#define LOC_FOG   2
#define LOC_IN1   3   // inputs occupy 3..6

// Pushes decals toward the camera in NDC (stand-in for glPolygonOffset).
// Raise if shadows/decals z-fight, lower if they poke through geometry.
#define DECAL_NDC_OFFSET 2e-5f

struct ShaderProgram {
    uint32_t shader_id;
    unsigned int program;
    uint8_t num_inputs;
    bool used_textures[2];
    uint8_t num_floats;
    int attrib_locations[MAX_ATTRIBS];
    uint8_t attrib_sizes[MAX_ATTRIBS];
    uint8_t num_attribs;
    bool used_noise;
    int loc_decal;
    int loc_frame_count;
    int loc_window_height;
};

#define MAX_SHADERS 64
static struct ShaderProgram shader_program_pool[MAX_SHADERS];
static uint8_t shader_program_pool_size = 0;
static struct ShaderProgram *current_shader = NULL;

#define MAX_TEXTURES 4096
struct TexSlot {
    Texture2D tex;
    bool linear;
    uint32_t cms, cmt;
};
static struct TexSlot texture_pool[MAX_TEXTURES];
static uint32_t current_texture_id = 0;
static uint32_t tile_tex[2] = { 0, 0 };  // texture currently selected per tile
static uint32_t last_selected_texture = 0; // target of the next upload

static unsigned int vao = 0;
static unsigned int vbo = 0;
#define VBO_BYTES (1 << 20)

static bool decal_enabled = false;
static int frame_count = 0;
static int current_height = 480;

// ============================================================================
// 1. WINDOW MANAGER
// ============================================================================

static void gfx_raylib_wm_init(const char *game_name, bool start_in_fullscreen) {
    InitWindow(640, 480, game_name);
    if (start_in_fullscreen) {
        ToggleFullscreen();
    }
    SetTargetFPS(30);
}

// Input is handled elsewhere; the engine still requires this hook to exist.
static void gfx_raylib_wm_set_keyboard_callbacks(bool (*on_key_down)(int scancode), bool (*on_key_up)(int scancode), void (*on_all_keys_up)(void)) {
    (void)on_key_down;
    (void)on_key_up;
    (void)on_all_keys_up;
}

static void gfx_raylib_wm_set_fullscreen_changed_callback(void (*on_fullscreen_changed)(bool is_now_fullscreen)) {
    (void)on_fullscreen_changed;
}

static void gfx_raylib_wm_set_fullscreen(bool enable) {
    if (enable != IsWindowFullscreen()) {
        ToggleFullscreen();
    }
}

static void gfx_raylib_wm_main_loop(void (*run_one_game_iter)(void)) {
    while (!WindowShouldClose()) {
        run_one_game_iter();
    }

    for (uint32_t i = 0; i < current_texture_id; i++) {
        if (texture_pool[i].tex.id > 0) UnloadTexture(texture_pool[i].tex);
    }
    CloseWindow();
}

static void gfx_raylib_wm_get_dimensions(uint32_t *width, uint32_t *height) {
    *width = GetScreenWidth();
    *height = GetScreenHeight();
}

static void gfx_raylib_wm_handle_events(void) {
}

static bool gfx_raylib_wm_start_frame(void) {
    BeginDrawing();
    return true;
}

static void gfx_raylib_wm_swap_buffers_begin(void) {
}

static void gfx_raylib_wm_swap_buffers_end(void) {
    EndDrawing();
}

static double gfx_raylib_wm_get_time(void) {
    return GetTime();
}

// ============================================================================
// 2. RENDERER
// ============================================================================

// ---- GLSL generation (mirrors gfx_opengl.c) --------------------------------

static void append_str(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
}

static void append_line(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
    buf[(*len)++] = '\n';
}

static const char *shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha, bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:
                return with_alpha ? "vec4(0.0, 0.0, 0.0, 0.0)" : "vec3(0.0, 0.0, 0.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_TEXEL0:
                return with_alpha ? "texVal0" : "texVal0.rgb";
            case SHADER_TEXEL0A:
                return hint_single_element ? "texVal0.a" :
                    (with_alpha ? "vec4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)" : "vec3(texVal0.a, texVal0.a, texVal0.a)");
            case SHADER_TEXEL1:
                return with_alpha ? "texVal1" : "texVal1.rgb";
        }
    } else {
        switch (item) {
            case SHADER_0:        return "0.0";
            case SHADER_INPUT_1:  return "vInput1.a";
            case SHADER_INPUT_2:  return "vInput2.a";
            case SHADER_INPUT_3:  return "vInput3.a";
            case SHADER_INPUT_4:  return "vInput4.a";
            case SHADER_TEXEL0:   return "texVal0.a";
            case SHADER_TEXEL0A:  return "texVal0.a";
            case SHADER_TEXEL1:   return "texVal1.a";
        }
    }
    return "0.0";
}

static void append_formula(char *buf, size_t *len, uint8_t c[2][4], bool do_single, bool do_multiply, bool do_mix, bool with_alpha, bool only_alpha, bool opt_alpha) {
    if (do_single) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    } else if (do_multiply) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
    } else if (do_mix) {
        append_str(buf, len, "mix(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, ")");
    } else {
        append_str(buf, len, "(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " - ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ") * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, " + ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    }
}

// ---- Interface --------------------------------------------------------------

static bool gfx_raylib_renderer_z_is_from_0_to_1(void) {
    return false;
}

static void gfx_raylib_renderer_unload_shader(struct ShaderProgram *old_prg) {
    (void)old_prg;
}

static void gfx_raylib_renderer_load_shader(struct ShaderProgram *new_prg) {
    current_shader = new_prg;
}

static struct ShaderProgram *gfx_raylib_renderer_create_and_load_new_shader(uint32_t shader_id) {
    if (shader_program_pool_size >= MAX_SHADERS) {
        fprintf(stderr, "gfx_raylib: shader pool full\n");
        abort();
    }

    struct CCFeatures cc;
    gfx_cc_get_features(shader_id, &cc);

    char vs[4096];
    char fs[4096];
    size_t vs_len = 0;
    size_t fs_len = 0;
    size_t num_floats = 4;
    bool any_tex = cc.used_textures[0] || cc.used_textures[1];
    int in_size = cc.opt_alpha ? 4 : 3;

    // ---------------- Vertex shader ----------------
    append_line(vs, &vs_len, "#version 330");
    append_line(vs, &vs_len, "layout(location = 0) in vec4 aVtxPos;");
    if (any_tex) {
        append_line(vs, &vs_len, "layout(location = 1) in vec2 aTexCoord;");
        append_line(vs, &vs_len, "out vec2 vTexCoord;");
        num_floats += 2;
    }
    if (cc.opt_fog) {
        append_line(vs, &vs_len, "layout(location = 2) in vec4 aFog;");
        append_line(vs, &vs_len, "out vec4 vFog;");
        num_floats += 4;
    }
    for (int i = 0; i < cc.num_inputs; i++) {
        vs_len += sprintf(vs + vs_len, "layout(location = %d) in vec%d aInput%d;\n", LOC_IN1 + i, in_size, i + 1);
        vs_len += sprintf(vs + vs_len, "out vec%d vInput%d;\n", in_size, i + 1);
        num_floats += in_size;
    }
    append_line(vs, &vs_len, "uniform float uDecal;");
    append_line(vs, &vs_len, "void main() {");
    if (any_tex) append_line(vs, &vs_len, "vTexCoord = aTexCoord;");
    if (cc.opt_fog) append_line(vs, &vs_len, "vFog = aFog;");
    for (int i = 0; i < cc.num_inputs; i++) {
        vs_len += sprintf(vs + vs_len, "vInput%d = aInput%d;\n", i + 1, i + 1);
    }
    append_line(vs, &vs_len, "gl_Position = aVtxPos;");
    append_line(vs, &vs_len, "gl_Position.z -= uDecal * gl_Position.w;");
    append_line(vs, &vs_len, "}");

    // ---------------- Fragment shader ----------------
    append_line(fs, &fs_len, "#version 330");
    if (any_tex) append_line(fs, &fs_len, "in vec2 vTexCoord;");
    if (cc.opt_fog) append_line(fs, &fs_len, "in vec4 vFog;");
    for (int i = 0; i < cc.num_inputs; i++) {
        fs_len += sprintf(fs + fs_len, "in vec%d vInput%d;\n", in_size, i + 1);
    }
    if (cc.used_textures[0]) append_line(fs, &fs_len, "uniform sampler2D uTex0;");
    if (cc.used_textures[1]) append_line(fs, &fs_len, "uniform sampler2D uTex1;");
    append_line(fs, &fs_len, "out vec4 outColor;");

    bool use_noise = cc.opt_alpha && cc.opt_noise;
    if (use_noise) {
        append_line(fs, &fs_len, "uniform int frame_count;");
        append_line(fs, &fs_len, "uniform int window_height;");
        append_line(fs, &fs_len, "float random(in vec3 value) {");
        append_line(fs, &fs_len, "    float r = dot(sin(value), vec3(12.9898, 78.233, 37.719));");
        append_line(fs, &fs_len, "    return fract(sin(r) * 143758.5453);");
        append_line(fs, &fs_len, "}");
    }

    append_line(fs, &fs_len, "void main() {");
    if (cc.used_textures[0]) append_line(fs, &fs_len, "vec4 texVal0 = texture(uTex0, vTexCoord);");
    if (cc.used_textures[1]) append_line(fs, &fs_len, "vec4 texVal1 = texture(uTex1, vTexCoord);");

    append_str(fs, &fs_len, cc.opt_alpha ? "vec4 texel = " : "vec3 texel = ");
    if (!cc.color_alpha_same && cc.opt_alpha) {
        append_str(fs, &fs_len, "vec4(");
        append_formula(fs, &fs_len, cc.c, cc.do_single[0], cc.do_multiply[0], cc.do_mix[0], false, false, true);
        append_str(fs, &fs_len, ", ");
        append_formula(fs, &fs_len, cc.c, cc.do_single[1], cc.do_multiply[1], cc.do_mix[1], true, true, true);
        append_str(fs, &fs_len, ")");
    } else {
        append_formula(fs, &fs_len, cc.c, cc.do_single[0], cc.do_multiply[0], cc.do_mix[0], cc.opt_alpha, false, cc.opt_alpha);
    }
    append_line(fs, &fs_len, ";");

    if (cc.opt_texture_edge && cc.opt_alpha) {
        append_line(fs, &fs_len, "if (texel.a > 0.3) texel.a = 1.0; else discard;");
    }
    if (cc.opt_fog) {
        if (cc.opt_alpha) {
            append_line(fs, &fs_len, "texel = vec4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);");
        } else {
            append_line(fs, &fs_len, "texel = mix(texel, vFog.rgb, vFog.a);");
        }
    }
    if (use_noise) {
        append_line(fs, &fs_len, "texel.a *= floor(random(vec3(floor(gl_FragCoord.xy * (240.0 / float(window_height))), float(frame_count))) + 0.5);");
    }
    if (cc.opt_alpha) {
        append_line(fs, &fs_len, "outColor = texel;");
    } else {
        append_line(fs, &fs_len, "outColor = vec4(texel, 1.0);");
    }
    append_line(fs, &fs_len, "}");

    vs[vs_len] = '\0';
    fs[fs_len] = '\0';

    unsigned int program = rlLoadShaderCode(vs, fs);
    if (program == 0) {
        fprintf(stderr, "gfx_raylib: shader 0x%08x failed to build\n--- VS ---\n%s\n--- FS ---\n%s\n",
                (unsigned)shader_id, vs, fs);
        abort();
    }

    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_size++];
    prg->shader_id = shader_id;
    prg->program = program;
    prg->num_inputs = (uint8_t)cc.num_inputs;
    prg->used_textures[0] = cc.used_textures[0];
    prg->used_textures[1] = cc.used_textures[1];
    prg->num_floats = (uint8_t)num_floats;

    int cnt = 0;
    prg->attrib_locations[cnt] = LOC_POS; prg->attrib_sizes[cnt++] = 4;
    if (any_tex)     { prg->attrib_locations[cnt] = LOC_UV;  prg->attrib_sizes[cnt++] = 2; }
    if (cc.opt_fog)  { prg->attrib_locations[cnt] = LOC_FOG; prg->attrib_sizes[cnt++] = 4; }
    for (int i = 0; i < cc.num_inputs; i++) {
        prg->attrib_locations[cnt] = LOC_IN1 + i;
        prg->attrib_sizes[cnt++] = (uint8_t)in_size;
    }
    prg->num_attribs = (uint8_t)cnt;

    rlEnableShader(program);
    if (cc.used_textures[0]) {
        int slot = 0;
        rlSetUniform(rlGetLocationUniform(program, "uTex0"), &slot, RL_SHADER_UNIFORM_INT, 1);
    }
    if (cc.used_textures[1]) {
        int slot = 1;
        rlSetUniform(rlGetLocationUniform(program, "uTex1"), &slot, RL_SHADER_UNIFORM_INT, 1);
    }
    prg->loc_decal = rlGetLocationUniform(program, "uDecal");
    prg->used_noise = use_noise;
    if (use_noise) {
        prg->loc_frame_count = rlGetLocationUniform(program, "frame_count");
        prg->loc_window_height = rlGetLocationUniform(program, "window_height");
    }
    rlDisableShader();

    current_shader = prg;
    return prg;
}

static struct ShaderProgram *gfx_raylib_renderer_lookup_shader(uint32_t shader_id) {
    for (size_t i = 0; i < shader_program_pool_size; i++) {
        if (shader_program_pool[i].shader_id == shader_id) {
            return &shader_program_pool[i];
        }
    }
    return NULL;
}

static void gfx_raylib_renderer_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static uint32_t gfx_raylib_renderer_new_texture(void) {
    if (current_texture_id >= MAX_TEXTURES) return 0;
    uint32_t id = current_texture_id++;
    texture_pool[id].tex.id = 0;
    texture_pool[id].linear = false;
    texture_pool[id].cms = 0;
    texture_pool[id].cmt = 0;
    return id;
}

static void gfx_raylib_renderer_select_texture(int tile, uint32_t texture_id) {
    if (texture_id >= MAX_TEXTURES) return;
    tile_tex[tile & 1] = texture_id;
    last_selected_texture = texture_id;
}

static int cm_to_gl(uint32_t val) {
    if (val & 2) return RL_TEXTURE_WRAP_CLAMP;               // G_TX_CLAMP
    return (val & 1) ? RL_TEXTURE_WRAP_MIRROR_REPEAT : RL_TEXTURE_WRAP_REPEAT; // G_TX_MIRROR
}

static void apply_sampler(struct TexSlot *t) {
    if (t->tex.id == 0) return;
    int filter = t->linear ? RL_TEXTURE_FILTER_LINEAR : RL_TEXTURE_FILTER_NEAREST;
    rlTextureParameters(t->tex.id, RL_TEXTURE_MIN_FILTER, filter);
    rlTextureParameters(t->tex.id, RL_TEXTURE_MAG_FILTER, filter);
    rlTextureParameters(t->tex.id, RL_TEXTURE_WRAP_S, cm_to_gl(t->cms));
    rlTextureParameters(t->tex.id, RL_TEXTURE_WRAP_T, cm_to_gl(t->cmt));
}

static void gfx_raylib_renderer_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    // Engine flow is select_texture(tile, id) -> upload, so the target is the
    // most recently selected texture.
    if (last_selected_texture >= MAX_TEXTURES) return;
    struct TexSlot *slot = &texture_pool[last_selected_texture];

    if (slot->tex.id > 0) {
        UnloadTexture(slot->tex);
    }

    Image img = {
        .data = (void *)rgba32_buf,
        .width = width,
        .height = height,
        .mipmaps = 1,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
    };
    slot->tex = LoadTextureFromImage(img);
    apply_sampler(slot); // a re-upload creates a new GL texture, so re-apply params
}

static void gfx_raylib_renderer_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    uint32_t id = tile_tex[tile & 1];
    if (id >= MAX_TEXTURES) return;
    struct TexSlot *slot = &texture_pool[id];
    slot->linear = linear_filter;
    slot->cms = cms;
    slot->cmt = cmt;
    apply_sampler(slot);
}

static void gfx_raylib_renderer_set_depth_test(bool depth_test) {
    if (depth_test) rlEnableDepthTest(); else rlDisableDepthTest();
}

static void gfx_raylib_renderer_set_depth_mask(bool z_upd) {
    if (z_upd) rlEnableDepthMask(); else rlDisableDepthMask();
}

static void gfx_raylib_renderer_set_zmode_decal(bool zmode_decal) {
    decal_enabled = zmode_decal; // applied as a uniform at draw time
}

static void gfx_raylib_renderer_set_viewport(int x, int y, int width, int height) {
    rlViewport(x, y, width, height);
    current_height = height;
}

static void gfx_raylib_renderer_set_scissor(int x, int y, int width, int height) {
    rlScissor(x, y, width, height);
}

static void gfx_raylib_renderer_set_use_alpha(bool use_alpha) {
    if (use_alpha) {
        rlEnableColorBlend();
    } else {
        rlDisableColorBlend();
    }
}

static void gfx_raylib_renderer_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    struct ShaderProgram *prg = current_shader;
    if (!prg || buf_vbo_num_tris == 0) return;

    size_t bytes = sizeof(float) * buf_vbo_len;
    if (bytes > VBO_BYTES) {
        fprintf(stderr, "gfx_raylib: vertex buffer too large (%zu bytes)\n", bytes);
        return;
    }

    // Shader + uniforms
    rlEnableShader(prg->program);
    float decal = decal_enabled ? DECAL_NDC_OFFSET : 0.0f;
    rlSetUniform(prg->loc_decal, &decal, RL_SHADER_UNIFORM_FLOAT, 1);
    if (prg->used_noise) {
        rlSetUniform(prg->loc_frame_count, &frame_count, RL_SHADER_UNIFORM_INT, 1);
        rlSetUniform(prg->loc_window_height, &current_height, RL_SHADER_UNIFORM_INT, 1);
    }

    // Textures: tile 0 -> slot 0, tile 1 -> slot 1
    for (int t = 0; t < 2; t++) {
        if (!prg->used_textures[t]) continue;
        unsigned int id = texture_pool[tile_tex[t]].tex.id;
        if (id == 0) id = rlGetTextureIdDefault();
        rlActiveTextureSlot(t);
        rlEnableTexture(id);
    }

    // Vertex data
    rlEnableVertexArray(vao);
    rlUpdateVertexBuffer(vbo, buf_vbo, (int)bytes, 0);
    rlEnableVertexBuffer(vbo);

    bool enabled[MAX_ATTRIBS] = { false };
    size_t stride = prg->num_floats * sizeof(float);
    size_t pos = 0;
    for (int i = 0; i < prg->num_attribs; i++) {
        int loc = prg->attrib_locations[i];
        rlSetVertexAttribute(loc, prg->attrib_sizes[i], RL_FLOAT, false, (int)stride, (int)(pos * sizeof(float)));
        rlEnableVertexAttribute(loc);
        enabled[loc] = true;
        pos += prg->attrib_sizes[i];
    }
    for (int loc = 0; loc < MAX_ATTRIBS; loc++) {
        if (!enabled[loc]) rlDisableVertexAttribute(loc);
    }

    rlDrawVertexArray(0, (int)(3 * buf_vbo_num_tris));

    // Restore raylib's state
    rlDisableVertexArray();
    rlDisableVertexBuffer();
    for (int t = 1; t >= 0; t--) {
        if (!prg->used_textures[t]) continue;
        rlActiveTextureSlot(t);
        rlDisableTexture();
    }
    rlActiveTextureSlot(0);
    rlDisableShader();
}

static void gfx_raylib_renderer_init(void) {
    // InitWindow() already initialised rlgl; don't call rlglInit again.
    rlDisableBackfaceCulling();       // the engine culls on the CPU
    rlEnableDepthTest();
    rlSetBlendMode(RL_BLEND_ALPHA);   // SRC_ALPHA, ONE_MINUS_SRC_ALPHA
    rlDisableColorBlend();

    vao = rlLoadVertexArray();
    rlEnableVertexArray(vao);
    vbo = rlLoadVertexBuffer(NULL, VBO_BYTES, true);
    rlDisableVertexArray();
    rlDisableVertexBuffer();
}

static void gfx_raylib_renderer_on_resize(void) {}

static void gfx_raylib_renderer_start_frame(void) {
    frame_count++;
    rlDisableScissorTest();
    rlEnableDepthMask();              // must be on to clear the depth buffer
    ClearBackground(BLACK);           // clears color + depth
    rlEnableScissorTest();
}

static void gfx_raylib_renderer_end_frame(void) {}
static void gfx_raylib_renderer_finish_render(void) {}

// ============================================================================
// 3. ENGINE BINDINGS
// ============================================================================

struct GfxWindowManagerAPI gfx_dummy_wm_api = {
    gfx_raylib_wm_init,
    gfx_raylib_wm_set_keyboard_callbacks,
    gfx_raylib_wm_set_fullscreen_changed_callback,
    gfx_raylib_wm_set_fullscreen,
    gfx_raylib_wm_main_loop,
    gfx_raylib_wm_get_dimensions,
    gfx_raylib_wm_handle_events,
    gfx_raylib_wm_start_frame,
    gfx_raylib_wm_swap_buffers_begin,
    gfx_raylib_wm_swap_buffers_end,
    gfx_raylib_wm_get_time
};

struct GfxRenderingAPI gfx_dummy_renderer_api = {
    gfx_raylib_renderer_z_is_from_0_to_1,
    gfx_raylib_renderer_unload_shader,
    gfx_raylib_renderer_load_shader,
    gfx_raylib_renderer_create_and_load_new_shader,
    gfx_raylib_renderer_lookup_shader,
    gfx_raylib_renderer_shader_get_info,
    gfx_raylib_renderer_new_texture,
    gfx_raylib_renderer_select_texture,
    gfx_raylib_renderer_upload_texture,
    gfx_raylib_renderer_set_sampler_parameters,
    gfx_raylib_renderer_set_depth_test,
    gfx_raylib_renderer_set_depth_mask,
    gfx_raylib_renderer_set_zmode_decal,
    gfx_raylib_renderer_set_viewport,
    gfx_raylib_renderer_set_scissor,
    gfx_raylib_renderer_set_use_alpha,
    gfx_raylib_renderer_draw_triangles,
    gfx_raylib_renderer_init,
    gfx_raylib_renderer_on_resize,
    gfx_raylib_renderer_start_frame,
    gfx_raylib_renderer_end_frame,
    gfx_raylib_renderer_finish_render
};