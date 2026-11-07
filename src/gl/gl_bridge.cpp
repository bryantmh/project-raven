// OpenGL for the game: SysV thunks over opengl32/WGL (tools/gen_gl_thunks.py), GLX entry points and GLU.
// docs/architecture.md#linux-libraries
#include "gl/gl_bridge.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

#include "linux/shim.h"
#include "log.h"

#include "gl/gl_thunks.inc"

namespace raven::gl {
namespace {
std::unordered_map<std::string, int>& index() {
    static std::unordered_map<std::string, int> m = [] {
        std::unordered_map<std::string, int> r;
        for (int i = 0; i < GL_THUNK_COUNT; i++) r[g_gl_thunks[i].name] = i;
        return r;
    }();
    return m;
}

// Every GL command is registered as a shim so the game's direct GL imports bind to the thunks.
struct RegisterAll {
    RegisterAll() {
        for (auto& t : g_gl_thunks) shim::register_symbol(t.name, t.thunk);
    }
} g_register_all;

HMODULE g_opengl32 = nullptr;
bool g_loaded = false;
void* g_driver[GL_THUNK_COUNT];  // the driver's entry points (g_gl_real, which the game calls, may hold wrappers)

// ---- The game's window framebuffer, redirected. The game draws to framebuffer 0 at its render size; we hand it a
// framebuffer of that size instead and scale it into the real window on present, so the window can be any size or
// mode (resized, fullscreen toggled) while the game keeps rendering at its resolution.
using BindFb = void(__stdcall*)(unsigned, unsigned);
using GetIntv = void(__stdcall*)(unsigned, int*);
using Buffer = void(__stdcall*)(unsigned);
using Buffers = void(__stdcall*)(int, const unsigned*);
BindFb d_bind_fb, d_bind_fb_ext;
GetIntv d_get_intv;
Buffer d_draw_buffer, d_read_buffer;
Buffers d_draw_buffers;
unsigned g_fbo = 0, g_rb[2] = {0, 0};
float g_view_aspect = 0;  // the frame's shape in the window: 0 = fill the window, else letterbox to this aspect
int g_fbo_w = 0, g_fbo_h = 0;
bool g_draw_default = true, g_read_default = true;

constexpr unsigned kFramebuffer = 0x8D40, kReadFb = 0x8CA8, kDrawFb = 0x8CA9, kColor0 = 0x8CE0;
bool is_window_buffer(unsigned b) { return b >= 0x0400 && b <= 0x0408; }  // GL_FRONT_LEFT .. GL_FRONT_AND_BACK

void track_bind(unsigned target, unsigned fb) {
    bool def = fb == 0 || fb == g_fbo;
    if (target == kFramebuffer || target == kDrawFb) g_draw_default = def;
    if (target == kFramebuffer || target == kReadFb) g_read_default = def;
}
void __stdcall w_bind_fb(unsigned target, unsigned fb) {
    if (fb == 0) fb = g_fbo;
    track_bind(target, fb);
    d_bind_fb(target, fb);
}
void __stdcall w_bind_fb_ext(unsigned target, unsigned fb) {
    if (fb == 0) fb = g_fbo;
    track_bind(target, fb);
    d_bind_fb_ext(target, fb);
}
void __stdcall w_get_intv(unsigned pname, int* v) {
    d_get_intv(pname, v);
    if ((pname == 0x8CA6 /*DRAW_FRAMEBUFFER_BINDING*/ || pname == 0x8CAA /*READ_FRAMEBUFFER_BINDING*/) && g_fbo &&
        unsigned(*v) == g_fbo)
        *v = 0;
    else if ((pname == 0x0C01 /*DRAW_BUFFER*/ && g_draw_default) || (pname == 0x0C02 /*READ_BUFFER*/ && g_read_default))
        if (unsigned(*v) == kColor0) *v = 0x0405;  // GL_BACK
}
void __stdcall w_draw_buffer(unsigned b) { d_draw_buffer(g_draw_default && is_window_buffer(b) ? kColor0 : b); }
void __stdcall w_read_buffer(unsigned b) { d_read_buffer(g_read_default && is_window_buffer(b) ? kColor0 : b); }
void __stdcall w_draw_buffers(int n, const unsigned* bufs) {
    if (!g_draw_default || n != 1 || !is_window_buffer(bufs[0])) return d_draw_buffers(n, bufs);
    d_draw_buffers(1, &kColor0);
}

// Texture bindings (unit 0), for telling UI elements apart by texture (ui_aspect.cpp).
using BindTex = void(__stdcall*)(unsigned, unsigned);
using ActiveTex = void(__stdcall*)(unsigned);
BindTex d_bind_tex;
ActiveTex d_active_tex;
unsigned g_active_unit = 0, g_tex0 = 0;
void __stdcall w_bind_tex(unsigned target, unsigned tex) {
    if (g_active_unit == 0 && target == 0x0DE1) g_tex0 = tex;
    d_bind_tex(target, tex);
}
void __stdcall w_active_tex(unsigned unit) {
    g_active_unit = unit - 0x84C0;
    d_active_tex(unit);
}

void patch(const char* name, void* wrapper, void** original) {
    auto it = index().find(name);
    if (it == index().end() || !g_gl_real[it->second]) return;
    *original = g_gl_real[it->second];
    g_gl_real[it->second] = wrapper;
}

void* real_proc(const char* name) {
    void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
    auto v = reinterpret_cast<intptr_t>(p);
    if (v == 0 || v == 1 || v == 2 || v == 3 || v == -1) p = reinterpret_cast<void*>(GetProcAddress(g_opengl32, name));
    return p;
}

// The driver calls debug callbacks with the Microsoft ABI; log them ourselves instead of calling the game's.
void __stdcall debug_callback(unsigned src, unsigned type, unsigned id, unsigned sev, int len, const char* msg, const void*) {
    if (sev == 0x9146 || sev == 0x9147) RLOG("[gl] %s", msg);  // HIGH / MEDIUM
}
}  // namespace

void load_entry_points() {
    if (!g_opengl32) g_opengl32 = LoadLibraryW(L"opengl32.dll");
    int found = 0;
    for (int i = 0; i < GL_THUNK_COUNT; i++) {
        g_gl_real[i] = g_driver[i] = real_proc(g_gl_thunks[i].name);
        found += g_gl_real[i] != nullptr;
    }
    g_loaded = true;
    RLOG("GL: %d of %d entry points available (%s / %s)", found, GL_THUNK_COUNT,
         reinterpret_cast<const char*>(glGetString(GL_VENDOR)), reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
}

void* real(const char* name) {
    auto it = index().find(name);
    return it == index().end() ? nullptr : g_driver[it->second];
}

void create_game_framebuffer(int w, int h) {
    auto gen_fb = reinterpret_cast<void(__stdcall*)(int, unsigned*)>(real("glGenFramebuffers"));
    auto gen_rb = reinterpret_cast<void(__stdcall*)(int, unsigned*)>(real("glGenRenderbuffers"));
    auto bind_rb = reinterpret_cast<void(__stdcall*)(unsigned, unsigned)>(real("glBindRenderbuffer"));
    auto storage = reinterpret_cast<void(__stdcall*)(unsigned, unsigned, int, int)>(real("glRenderbufferStorage"));
    auto attach = reinterpret_cast<void(__stdcall*)(unsigned, unsigned, unsigned, unsigned)>(real("glFramebufferRenderbuffer"));
    auto status = reinterpret_cast<unsigned(__stdcall*)(unsigned)>(real("glCheckFramebufferStatus"));
    d_bind_fb = reinterpret_cast<BindFb>(real("glBindFramebuffer"));
    if (!gen_fb || !d_bind_fb) {
        RLOG("GL: no framebuffer objects; the game draws to the window directly");
        return;
    }
    unsigned* rb = g_rb;
    gen_fb(1, &g_fbo);
    gen_rb(2, rb);
    bind_rb(0x8D41, rb[0]);
    storage(0x8D41, 0x8058 /*RGBA8*/, w, h);
    bind_rb(0x8D41, rb[1]);
    storage(0x8D41, 0x88F0 /*DEPTH24_STENCIL8*/, w, h);
    bind_rb(0x8D41, 0);
    d_bind_fb(kFramebuffer, g_fbo);
    attach(kFramebuffer, kColor0, 0x8D41, rb[0]);
    attach(kFramebuffer, 0x821A /*DEPTH_STENCIL_ATTACHMENT*/, 0x8D41, rb[1]);
    unsigned st = status(kFramebuffer);
    if (st != 0x8CD5) {
        RLOG("GL: game framebuffer incomplete (%#x); drawing to the window directly", st);
        d_bind_fb(kFramebuffer, 0);
        g_fbo = 0;
        return;
    }
    g_fbo_w = w;
    g_fbo_h = h;
    patch("glBindFramebuffer", reinterpret_cast<void*>(&w_bind_fb), reinterpret_cast<void**>(&d_bind_fb));
    patch("glBindFramebufferEXT", reinterpret_cast<void*>(&w_bind_fb_ext), reinterpret_cast<void**>(&d_bind_fb_ext));
    patch("glGetIntegerv", reinterpret_cast<void*>(&w_get_intv), reinterpret_cast<void**>(&d_get_intv));
    patch("glDrawBuffer", reinterpret_cast<void*>(&w_draw_buffer), reinterpret_cast<void**>(&d_draw_buffer));
    patch("glReadBuffer", reinterpret_cast<void*>(&w_read_buffer), reinterpret_cast<void**>(&d_read_buffer));
    patch("glDrawBuffers", reinterpret_cast<void*>(&w_draw_buffers), reinterpret_cast<void**>(&d_draw_buffers));
    patch("glBindTexture", reinterpret_cast<void*>(&w_bind_tex), reinterpret_cast<void**>(&d_bind_tex));
    patch("glActiveTexture", reinterpret_cast<void*>(&w_active_tex), reinterpret_cast<void**>(&d_active_tex));
    d_draw_buffer(kColor0);
    d_read_buffer(kColor0);
    RLOG("GL: game renders to a %dx%d framebuffer, scaled into the window", w, h);
}

unsigned bound_texture0() { return g_tex0; }

void set_view_aspect(float aspect) { g_view_aspect = aspect; }

void resize_game_framebuffer(int w, int h) {
    if (!g_fbo || (w == g_fbo_w && h == g_fbo_h)) return;
    auto bind_rb = reinterpret_cast<void(__stdcall*)(unsigned, unsigned)>(real("glBindRenderbuffer"));
    auto storage = reinterpret_cast<void(__stdcall*)(unsigned, unsigned, int, int)>(real("glRenderbufferStorage"));
    bind_rb(0x8D41, g_rb[0]);
    storage(0x8D41, 0x8058, w, h);
    bind_rb(0x8D41, g_rb[1]);
    storage(0x8D41, 0x88F0, w, h);
    bind_rb(0x8D41, 0);
    g_fbo_w = w;
    g_fbo_h = h;
}

void game_rect_in(int win_w, int win_h, int* x, int* y, int* w, int* h) {
    if (g_view_aspect <= 0 || win_h <= 0) {
        *x = *y = 0;
        *w = win_w;
        *h = win_h;
        return;
    }
    if (float(win_w) / float(win_h) > g_view_aspect) {
        *h = win_h;
        *w = int(float(win_h) * g_view_aspect + 0.5f);
    } else {
        *w = win_w;
        *h = int(float(win_w) / g_view_aspect + 0.5f);
    }
    *x = (win_w - *w) / 2;
    *y = (win_h - *h) / 2;
}

void present_game_framebuffer(int win_w, int win_h) {
    if (!g_fbo) return;
    auto is_enabled = reinterpret_cast<unsigned char(__stdcall*)(unsigned)>(real("glIsEnabled"));
    auto enable = reinterpret_cast<void(__stdcall*)(unsigned)>(real("glEnable"));
    auto disable = reinterpret_cast<void(__stdcall*)(unsigned)>(real("glDisable"));
    auto clear_color = reinterpret_cast<void(__stdcall*)(float, float, float, float)>(real("glClearColor"));
    auto get_floatv = reinterpret_cast<void(__stdcall*)(unsigned, float*)>(real("glGetFloatv"));
    auto clear = reinterpret_cast<void(__stdcall*)(unsigned)>(real("glClear"));
    auto blit = reinterpret_cast<void(__stdcall*)(int, int, int, int, int, int, int, int, unsigned, unsigned)>(real("glBlitFramebuffer"));
    auto color_mask = reinterpret_cast<void(__stdcall*)(unsigned char, unsigned char, unsigned char, unsigned char)>(real("glColorMask"));
    auto get_booleanv = reinterpret_cast<void(__stdcall*)(unsigned, unsigned char*)>(real("glGetBooleanv"));
    auto draw_buffer = reinterpret_cast<void(__stdcall*)(unsigned)>(real("glDrawBuffer"));
    auto read_buffer = reinterpret_cast<void(__stdcall*)(unsigned)>(real("glReadBuffer"));
    bool scissor = is_enabled(0x0C11), srgb = is_enabled(0x8DB9);
    float cc[4];
    unsigned char mask[4];
    get_floatv(0x0C22, cc);
    get_booleanv(0x0C23 /*COLOR_WRITEMASK*/, mask);
    if (scissor) disable(0x0C11);
    if (srgb) disable(0x8DB9);
    color_mask(1, 1, 1, 1);
    d_bind_fb(kDrawFb, 0);
    draw_buffer(0x0405);
    int x, y, w, h;
    game_rect_in(win_w, win_h, &x, &y, &w, &h);
    if (w != win_w || h != win_h) {
        clear_color(0, 0, 0, 1);
        clear(0x4000);
        clear_color(cc[0], cc[1], cc[2], cc[3]);
    }
    d_bind_fb(kReadFb, g_fbo);
    read_buffer(kColor0);
    blit(0, 0, g_fbo_w, g_fbo_h, x, y, x + w, y + h, 0x4000, w == g_fbo_w && h == g_fbo_h ? 0x2600 : 0x2601);
    color_mask(mask[0], mask[1], mask[2], mask[3]);
    if (scissor) enable(0x0C11);
    if (srgb) enable(0x8DB9);
    // The window's back buffer stays bound until the swap (the settings menu and screenshots use it).
    d_bind_fb(kReadFb, 0);
    read_buffer(0x0405);
}

void rebind_game_framebuffer() {
    if (!g_fbo) return;
    d_bind_fb(kDrawFb, g_draw_default ? g_fbo : 0);
    d_bind_fb(kReadFb, g_read_default ? g_fbo : 0);
    auto draw_buffer = reinterpret_cast<void(__stdcall*)(unsigned)>(real("glDrawBuffer"));
    auto read_buffer = reinterpret_cast<void(__stdcall*)(unsigned)>(real("glReadBuffer"));
    if (g_draw_default) draw_buffer(kColor0);
    if (g_read_default) read_buffer(kColor0);
}

void* thunk_for(const char* name) {
    if (!std::strncmp(name, "glDebugMessageCallback", 22)) return shim::lookup("glDebugMessageCallback");
    if (!std::strcmp(name, "glUniformMatrix4fv") || !std::strcmp(name, "glUniformMatrix4fvARB"))
        return shim::lookup("raven_glUniformMatrix4fv");
    auto it = index().find(name);
    if (it == index().end()) return nullptr;
    if (g_loaded && !g_gl_real[it->second]) return nullptr;  // tells GLEW the function isn't there
    return g_gl_thunks[it->second].thunk;
}

}  // namespace raven::gl

using namespace raven;

// glDebugMessageCallback{,ARB,KHR}: install our own logger (see debug_callback).
SHIM(void, glDebugMessageCallback, (void* cb, const void* user)) {
    if (auto f = reinterpret_cast<void(__stdcall*)(void*, const void*)>(gl::real("glDebugMessageCallback")))
        f(reinterpret_cast<void*>(&gl::debug_callback), nullptr);
}
SHIM_ALIAS(glDebugMessageCallbackARB, glDebugMessageCallback);
SHIM_ALIAS(glDebugMessageCallbackKHR, glDebugMessageCallback);

// glUniformMatrix4fv: the 2D UI layout fix (src/game/ui_aspect.cpp) adjusts 2D elements' matrices here.
#include "game/ui_aspect.h"
SHIM(void, raven_glUniformMatrix4fv, (int location, int count, unsigned char transpose, const float* m)) {
    static auto f = reinterpret_cast<void(__stdcall*)(int, int, unsigned char, const float*)>(gl::real("glUniformMatrix4fv"));
    float adjusted[16];
    if (count == 1 && game::ui_adjust_matrix(location, count, transpose, m, adjusted)) m = adjusted;
    f(location, count, transpose, m);
}

// ---- GLX (only what GLEW and the game ask for; the window and context come from the SDL bridge)
SHIM(void*, glXGetProcAddressARB, (const char* name)) {
    void* p = gl::thunk_for(name);
    if (!p && !std::strncmp(name, "glX", 3)) p = shim::lookup(name);
    RTRACE("glXGetProcAddress(%s) = %p", name, p);
    return p;
}
SHIM_ALIAS(glXGetProcAddress, glXGetProcAddressARB);
SHIM(int, glXQueryVersion, (void*, int* maj, int* min)) {
    if (maj) *maj = 1;
    if (min) *min = 4;
    return 1;
}
SHIM(const char*, glXGetClientString, (void*, int name)) {
    switch (name) {
        case 1: return "Project Raven";               // GLX_VENDOR
        case 2: return "1.4";                         // GLX_VERSION
        default: return "GLX_ARB_get_proc_address";   // GLX_EXTENSIONS
    }
}
static uint64_t g_fake_display[32];
SHIM(void*, glXGetCurrentDisplay, ()) { return g_fake_display; }
SHIM(const char*, glXQueryExtensionsString, (void*, int)) { return "GLX_ARB_get_proc_address"; }

// ---- GLU (glu32.dll, Microsoft ABI)
SHIM(const uint8_t*, gluErrorString, (unsigned err)) { return gluErrorString(err); }
SHIM(int, gluBuild2DMipmaps, (unsigned target, int comps, int w, int h, unsigned fmt, unsigned type, const void* data)) {
    return gluBuild2DMipmaps(target, comps, w, h, fmt, type, data);
}
