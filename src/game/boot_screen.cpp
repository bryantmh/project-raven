// Boot image. While it starts up, the game draws progress text (game name and version, resource names) on black
// through its message screen (0x4fc460: clear, draw text, swap, pump events). With the setting on we draw
// assets/boot.jpg instead, fitted to the window, and keep swapping and pumping like the original.
#include "game/boot_screen.h"

#include <windows.h>
#include <wincodec.h>

#include <SDL.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

#include "app/config.h"
#include "app/paths.h"
#include "game/hook.h"
#include "gl/gl_bridge.h"
#include "linux/shim.h"
#include "log.h"
#include "sdl/sdl_bridge.h"

namespace raven::game {
namespace {
constexpr uint64_t kMessageScreen = 0x4fc460;  // void MessageScreen(const char* text)

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> bgra;
};

// Decode with WIC (any format Windows reads: JPEG/JFIF, PNG, ...) to top-down BGRA.
Image load_image(const std::wstring& path) {
    Image img;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom))) {
        UINT w, h;
        conv->GetSize(&w, &h);
        img.bgra.resize(size_t(w) * h * 4);
        if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(img.bgra.size()), img.bgra.data()))) img.w = w, img.h = h;
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    if (!img.w) RLOG("boot image: couldn't load %s", app::to_utf8(path).c_str());
    return img;
}

template <typename F>
F gl_fn(const char* name) {
    return reinterpret_cast<F>(gl::real(name));
}

// GL enums
constexpr unsigned kTexture2D = 0x0DE1, kReadFb = 0x8CA8, kDrawFb = 0x8CA9, kReadFbBinding = 0x8CAA,
                   kDrawFbBinding = 0x8CA6, kColorBit = 0x4000, kLinear = 0x2601, kScissor = 0x0C11,
                   kUnpackBuffer = 0x88EC, kUnpackBufferBinding = 0x88EF, kUnpackAlignment = 0x0CF5;

unsigned g_fbo = 0;
int g_img_w = 0, g_img_h = 0;

bool ensure_texture() {
    static bool tried = false;
    if (tried) return g_fbo != 0;
    tried = true;
    auto path = std::filesystem::path(app::to_wide(app::paths().runtime_dir)) / L"assets" / L"boot.jpg";
    Image img = load_image(path.wstring());
    if (!img.w) return false;
    std::vector<uint8_t> flipped(img.bgra.size());  // GL wants bottom-up rows
    for (int y = 0; y < img.h; y++)
        std::memcpy(&flipped[size_t(y) * img.w * 4], &img.bgra[size_t(img.h - 1 - y) * img.w * 4], size_t(img.w) * 4);

    auto gen_tex = gl_fn<void(__stdcall*)(int, unsigned*)>("glGenTextures");
    auto bind_tex = gl_fn<void(__stdcall*)(unsigned, unsigned)>("glBindTexture");
    auto tex_image = gl_fn<void(__stdcall*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*)>("glTexImage2D");
    auto tex_param = gl_fn<void(__stdcall*)(unsigned, unsigned, int)>("glTexParameteri");
    auto get_int = gl_fn<void(__stdcall*)(unsigned, int*)>("glGetIntegerv");
    auto gen_fb = gl_fn<void(__stdcall*)(int, unsigned*)>("glGenFramebuffers");
    auto bind_fb = gl_fn<void(__stdcall*)(unsigned, unsigned)>("glBindFramebuffer");
    auto fb_tex = gl_fn<void(__stdcall*)(unsigned, unsigned, unsigned, unsigned, int)>("glFramebufferTexture2D");
    auto pixel_store = gl_fn<void(__stdcall*)(unsigned, int)>("glPixelStorei");
    auto bind_buf = gl_fn<void(__stdcall*)(unsigned, unsigned)>("glBindBuffer");
    if (!gen_fb || !fb_tex || !bind_buf) return false;

    int old_tex = 0, old_read_fb = 0, old_unpack = 0, old_align = 4;
    get_int(0x8069 /*GL_TEXTURE_BINDING_2D*/, &old_tex);
    get_int(kReadFbBinding, &old_read_fb);
    get_int(kUnpackBufferBinding, &old_unpack);
    get_int(kUnpackAlignment, &old_align);
    if (old_unpack) bind_buf(kUnpackBuffer, 0);
    pixel_store(kUnpackAlignment, 4);
    unsigned tex = 0;
    gen_tex(1, &tex);
    bind_tex(kTexture2D, tex);
    tex_param(kTexture2D, 0x2801 /*MIN_FILTER*/, int(kLinear));
    tex_param(kTexture2D, 0x2800 /*MAG_FILTER*/, int(kLinear));
    tex_image(kTexture2D, 0, 0x8058 /*GL_RGBA8*/, img.w, img.h, 0, 0x80E1 /*GL_BGRA*/, 0x1401, flipped.data());
    gen_fb(1, &g_fbo);
    bind_fb(kReadFb, g_fbo);
    fb_tex(kReadFb, 0x8CE0 /*COLOR_ATTACHMENT0*/, kTexture2D, tex, 0);
    bind_fb(kReadFb, unsigned(old_read_fb));
    bind_tex(kTexture2D, unsigned(old_tex));
    pixel_store(kUnpackAlignment, old_align);
    if (old_unpack) bind_buf(kUnpackBuffer, unsigned(old_unpack));
    g_img_w = img.w;
    g_img_h = img.h;
    return true;
}

// Fit the image in the drawable (black bars on other aspect ratios) with a framebuffer blit.
void draw_image() {
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(sdl::window(), &w, &h);
    auto get_int = gl_fn<void(__stdcall*)(unsigned, int*)>("glGetIntegerv");
    auto bind_fb = gl_fn<void(__stdcall*)(unsigned, unsigned)>("glBindFramebuffer");
    auto blit = gl_fn<void(__stdcall*)(int, int, int, int, int, int, int, int, unsigned, unsigned)>("glBlitFramebuffer");
    auto is_enabled = gl_fn<unsigned char(__stdcall*)(unsigned)>("glIsEnabled");
    auto disable = gl_fn<void(__stdcall*)(unsigned)>("glDisable");
    auto enable = gl_fn<void(__stdcall*)(unsigned)>("glEnable");
    auto clear_color = gl_fn<void(__stdcall*)(float, float, float, float)>("glClearColor");
    auto clear = gl_fn<void(__stdcall*)(unsigned)>("glClear");
    auto viewport = gl_fn<void(__stdcall*)(int, int, int, int)>("glViewport");
    int old_read = 0, old_draw = 0, old_vp[4] = {};
    float old_clear[4] = {};
    get_int(kReadFbBinding, &old_read);
    get_int(kDrawFbBinding, &old_draw);
    get_int(0x0BA2 /*GL_VIEWPORT*/, old_vp);
    gl_fn<void(__stdcall*)(unsigned, float*)>("glGetFloatv")(0x0C22 /*GL_COLOR_CLEAR_VALUE*/, old_clear);
    bool scissor = is_enabled(kScissor);
    if (scissor) disable(kScissor);
    bind_fb(kDrawFb, 0);
    viewport(0, 0, w, h);
    clear_color(0, 0, 0, 1);
    clear(kColorBit);
    float s = std::min(float(w) / float(g_img_w), float(h) / float(g_img_h));
    int dw = int(float(g_img_w) * s), dh = int(float(g_img_h) * s), dx = (w - dw) / 2, dy = (h - dh) / 2;
    bind_fb(kReadFb, g_fbo);
    blit(0, 0, g_img_w, g_img_h, dx, dy, dx + dw, dy + dh, kColorBit, kLinear);
    bind_fb(kReadFb, unsigned(old_read));
    bind_fb(kDrawFb, unsigned(old_draw));
    viewport(old_vp[0], old_vp[1], old_vp[2], old_vp[3]);
    clear_color(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
    if (scissor) enable(kScissor);
}

SYSV void h_message_screen(const char* text) {
    RLOG("boot: %s", text ? text : "");
    if (!ensure_texture()) return;
    draw_image();
    sdl::present();
}
}  // namespace

void install_boot_screen() {
    if (!app::config().boot_image) return;
    hook_jump(kMessageScreen, reinterpret_cast<const void*>(&h_message_screen));
}
}  // namespace raven::game
