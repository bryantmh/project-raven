// The recomp settings menu. Opening it pauses the game: the game thread stays inside the hooked SDL_GL_SwapWindow
// running the menu's own loop (the last game frame is kept in a texture and drawn behind the menu) and FMOD's
// master channel group is paused. Settings marked "restart" are read once at startup; the rest apply at once.
#include "ui/menu.h"

#include <windows.h>

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "app/config.h"
#include "app/paths.h"
#include "gl/gl_bridge.h"
#include "log.h"
#include "sdl/sdl_bridge.h"

namespace raven::app { std::string pick_folder_dialog(); }
namespace raven::audio { void set_paused(bool paused); }

namespace raven::ui {
namespace {
bool g_requested = false, g_open = false, g_inited = false, g_quit = false;
int g_current_tab = 0, g_tab_step = 0;

template <typename F>
F gl_fn(const char* name) {
    return reinterpret_cast<F>(gl::real(name));
}

// The frame the game was about to present, kept while the menu is open.
struct Snapshot {
    unsigned tex = 0, fbo = 0;
    int w = 0, h = 0;
    void capture(int width, int height) {
        auto gen_tex = gl_fn<void(__stdcall*)(int, unsigned*)>("glGenTextures");
        auto bind_tex = gl_fn<void(__stdcall*)(unsigned, unsigned)>("glBindTexture");
        auto tex_image = gl_fn<void(__stdcall*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*)>("glTexImage2D");
        auto gen_fb = gl_fn<void(__stdcall*)(int, unsigned*)>("glGenFramebuffers");
        auto bind_fb = gl_fn<void(__stdcall*)(unsigned, unsigned)>("glBindFramebuffer");
        auto fb_tex = gl_fn<void(__stdcall*)(unsigned, unsigned, unsigned, unsigned, int)>("glFramebufferTexture2D");
        auto blit = gl_fn<void(__stdcall*)(int, int, int, int, int, int, int, int, unsigned, unsigned)>("glBlitFramebuffer");
        auto read_buffer = gl_fn<void(__stdcall*)(unsigned)>("glReadBuffer");
        if (!tex) {
            gen_tex(1, &tex);
            gen_fb(1, &fbo);
        }
        if (w != width || h != height) {
            bind_tex(0x0DE1, tex);
            tex_image(0x0DE1, 0, 0x8058, width, height, 0, 0x1908, 0x1401, nullptr);
            bind_tex(0x0DE1, 0);
            bind_fb(0x8CA9, fbo);
            fb_tex(0x8CA9, 0x8CE0, 0x0DE1, tex, 0);
            w = width;
            h = height;
        }
        bind_fb(0x8CA8, 0);
        read_buffer(0x0405 /*GL_BACK*/);
        bind_fb(0x8CA9, fbo);
        blit(0, 0, w, h, 0, 0, w, h, 0x4000, 0x2600 /*NEAREST*/);
        bind_fb(0x8CA9, 0);
    }
    void draw(int width, int height) const {
        auto bind_fb = gl_fn<void(__stdcall*)(unsigned, unsigned)>("glBindFramebuffer");
        auto blit = gl_fn<void(__stdcall*)(int, int, int, int, int, int, int, int, unsigned, unsigned)>("glBlitFramebuffer");
        bind_fb(0x8CA8, fbo);
        bind_fb(0x8CA9, 0);
        blit(0, 0, w, h, 0, 0, width, height, 0x4000, 0x2601 /*LINEAR*/);
        bind_fb(0x8CA8, 0);
    }
} g_snapshot;

void init() {
    if (g_inited) return;
    g_inited = true;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    // Segoe UI from Windows, sized for the window height (ImGui's built-in font is too small to read from a couch).
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(sdl::window(), &dw, &dh);
    float px = 34.0f * std::max(0.75f, float(dh) / 1080.0f);
    char windir[MAX_PATH] = "C:\\Windows";
    GetWindowsDirectoryA(windir, MAX_PATH);
    std::string font = std::string(windir) + "\\Fonts\\segoeui.ttf";
    if (!std::filesystem::exists(font) || !io.Fonts->AddFontFromFileTTF(font.c_str(), px)) io.Fonts->AddFontDefault();
    ImGui::GetStyle().ScaleAllSizes(px / 16.0f);
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6;
    st.FrameRounding = 4;
    st.GrabRounding = 4;
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.07f, 0.10f, 0.94f);
    ImGui_ImplSDL2_InitForOpenGL(sdl::window(), SDL_GL_GetCurrentContext());
    ImGui_ImplOpenGL3_Init(nullptr);
}

// State saved around the menu so the game's GL state is untouched afterwards.
struct GlState {
    int read_fb = 0, draw_fb = 0, viewport[4] = {}, program = 0, vao = 0, array_buffer = 0, texture = 0,
        active_texture = 0, unpack_buffer = 0;
    unsigned char scissor = 0;
    void save() {
        auto get_int = gl_fn<void(__stdcall*)(unsigned, int*)>("glGetIntegerv");
        auto is_enabled = gl_fn<unsigned char(__stdcall*)(unsigned)>("glIsEnabled");
        get_int(0x8CAA, &read_fb);
        get_int(0x8CA6, &draw_fb);
        get_int(0x0BA2, viewport);
        get_int(0x8B8D /*CURRENT_PROGRAM*/, &program);
        get_int(0x85B5 /*VERTEX_ARRAY_BINDING*/, &vao);
        get_int(0x8894 /*ARRAY_BUFFER_BINDING*/, &array_buffer);
        get_int(0x84E0 /*ACTIVE_TEXTURE*/, &active_texture);
        get_int(0x8069, &texture);
        get_int(0x88EF, &unpack_buffer);
        scissor = is_enabled(0x0C11);
    }
    void restore() const {
        auto bind_fb = gl_fn<void(__stdcall*)(unsigned, unsigned)>("glBindFramebuffer");
        auto viewport_fn = gl_fn<void(__stdcall*)(int, int, int, int)>("glViewport");
        auto use_program = gl_fn<void(__stdcall*)(unsigned)>("glUseProgram");
        auto bind_vao = gl_fn<void(__stdcall*)(unsigned)>("glBindVertexArray");
        auto bind_buf = gl_fn<void(__stdcall*)(unsigned, unsigned)>("glBindBuffer");
        auto active_tex = gl_fn<void(__stdcall*)(unsigned)>("glActiveTexture");
        auto bind_tex = gl_fn<void(__stdcall*)(unsigned, unsigned)>("glBindTexture");
        auto enable = gl_fn<void(__stdcall*)(unsigned)>("glEnable");
        auto disable = gl_fn<void(__stdcall*)(unsigned)>("glDisable");
        bind_fb(0x8CA8, unsigned(read_fb));
        bind_fb(0x8CA9, unsigned(draw_fb));
        viewport_fn(viewport[0], viewport[1], viewport[2], viewport[3]);
        use_program(unsigned(program));
        bind_vao(unsigned(vao));
        bind_buf(0x8892, unsigned(array_buffer));
        bind_buf(0x88EC, unsigned(unpack_buffer));
        active_tex(unsigned(active_texture));
        bind_tex(0x0DE1, unsigned(texture));
        if (scissor) enable(0x0C11);
        else disable(0x0C11);
    }
};

// ---- widgets
bool g_dirty = false;
void restart_note() { ImGui::SameLine(); ImGui::TextDisabled("(restart)"); }
bool checkbox(const char* label, bool& v, bool restart = false) {
    bool c = ImGui::Checkbox(label, &v);
    if (restart) restart_note();
    g_dirty |= c;
    return c;
}
bool slider(const char* label, int& v, int lo, int hi, const char* fmt = "%d%%") {
    bool c = ImGui::SliderInt(label, &v, lo, hi, fmt);
    g_dirty |= c;
    return c;
}
bool combo(const char* label, int& index, const std::vector<std::string>& items, bool restart = false) {
    bool c = false;
    if (ImGui::BeginCombo(label, items[size_t(std::clamp(index, 0, int(items.size()) - 1))].c_str())) {
        for (int i = 0; i < int(items.size()); i++)
            if (ImGui::Selectable(items[size_t(i)].c_str(), i == index)) index = i, c = true;
        ImGui::EndCombo();
    }
    if (restart) restart_note();
    g_dirty |= c;
    return c;
}

struct Res {
    int w, h;
};
std::vector<Res> resolutions() {
    std::vector<Res> out;
    SDL_DisplayMode m;
    int display = SDL_GetWindowDisplayIndex(sdl::window());
    for (int i = 0, n = SDL_GetNumDisplayModes(display); i < n; i++)
        if (SDL_GetDisplayMode(display, i, &m) == 0 &&
            std::none_of(out.begin(), out.end(), [&](const Res& r) { return r.w == m.w && r.h == m.h; }))
            out.push_back({m.w, m.h});
    for (Res r : {Res{1280, 720}, Res{1920, 1080}, Res{2560, 1080}, Res{2560, 1440}, Res{3440, 1440}, Res{3840, 2160}})
        if (std::none_of(out.begin(), out.end(), [&](const Res& x) { return x.w == r.w && x.h == r.h; })) out.push_back(r);
    std::sort(out.begin(), out.end(), [](const Res& a, const Res& b) { return a.w * a.h > b.w * b.h; });
    return out;
}

void tab_general(app::Config& c) {
    ImGui::TextWrapped("Game folder: %s", c.game_folder.c_str());
    if (ImGui::Button("Change game folder...")) {
        std::string dir = app::pick_folder_dialog();
        if (!dir.empty()) {
            if (std::filesystem::exists(app::to_wide(dir + "\\halo\\game"))) c.game_folder = dir, g_dirty = true;
            else ImGui::OpenPopup("bad_folder");
        }
    }
    restart_note();
    if (ImGui::BeginPopup("bad_folder")) {
        ImGui::Text("That folder doesn't contain halo\\game.");
        ImGui::EndPopup();
    }
    ImGui::TextDisabled("The game is read from that folder; nothing is copied.");
    ImGui::Separator();
    checkbox("Free play", c.free_play, true);
}

void tab_display(app::Config& c) {
    if (combo("Display mode", c.display_mode, {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"}))
        sdl::apply_display_mode();
    static std::vector<Res> res = resolutions();
    std::vector<std::string> names{"Match display (fullscreen) / window"};
    int index = 0;
    for (size_t i = 0; i < res.size(); i++) {
        names.push_back(std::to_string(res[i].w) + " x " + std::to_string(res[i].h));
        if (res[i].w == c.render_width && res[i].h == c.render_height) index = int(i) + 1;
    }
    if (combo("Resolution", index, names, true)) {
        c.render_width = index ? res[size_t(index - 1)].w : 0;
        c.render_height = index ? res[size_t(index - 1)].h : 0;
    }
    if (combo("Aspect ratio", c.aspect, {"Auto (match window)", "16:9", "21:9"})) sdl::reapply_view_aspect();
    combo("UI layout on wide screens", c.ui_layout, {"Stretched (original)", "16:9 centered", "Corners (HUD; menus 16:9)"});
    if (checkbox("VSync", c.vsync)) SDL_GL_SetSwapInterval(c.vsync ? 1 : 0);
    std::vector<std::string> limits{"Unlimited", "30", "60", "120", "144", "165", "240"};
    static const int values[] = {0, 30, 60, 120, 144, 165, 240};
    int li = 0;
    for (int i = 0; i < 7; i++)
        if (values[i] == c.fps_limit) li = i;
    if (combo("Frame rate limit", li, limits)) c.fps_limit = values[li];
}

void tab_controls(app::Config& c) {
    slider("Rumble", c.rumble, 0, 100);
    slider("Aim speed", c.aim_speed, 20, 300);
    slider("Stick deadzone", c.aim_deadzone, 0, 50);
    ImGui::Separator();
    ImGui::TextDisabled("Controller: right or left stick aim, RT / A fire, LT / B reload,");
    ImGui::TextDisabled("Y / LB reload (point off-screen), RB / X grenade, Start join, Select menu.");
    ImGui::TextDisabled("Mouse: aim, left fire, right reload, middle grenade. Keys: 1/2 start, R reload.");
}

void tab_presentation(app::Config& c) {
    checkbox("Hide \"Money In\" / \"FREEPLAY\" text", c.hide_credit_text, true);
    checkbox("No countdown on the game mode menu", c.no_menu_countdown, true);
    checkbox("Skip the Xbox Live \"Get Connected\" screen", c.skip_xbox_screen, true);
    checkbox("Hide QR scanner notices", c.hide_qr_notices, true);
    checkbox("Boot image instead of boot text", c.boot_image, true);
}

void draw_menu(int w, int h) {
    auto& c = app::config();
    // Layout in ImGui's display units (the window's size as SDL reports it), not framebuffer pixels.
    ImVec2 disp = ImGui::GetIO().DisplaySize;
    float scale = std::max(0.75f, disp.y / 1080.0f) * (float(h) / std::max(1.0f, disp.y)) / std::max(0.01f, ImGui::GetIO().DisplayFramebufferScale.y);
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(1250 * scale, disp.x - 20), std::min(760 * scale, disp.y - 20)), ImGuiCond_Always);
    ImGui::Begin("Project Raven", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    // RAVEN_MENU_TAB=<0-3>: start on that tab (automated screenshots).
    static int first_tab = [] { const char* t = std::getenv("RAVEN_MENU_TAB"); return t ? std::atoi(t) : -1; }();
    // L1/R1 (LB/RB), Q/E or Ctrl+Tab move between tabs (g_tab_step, from run()).
    constexpr int kTabs = 4;
    if (g_tab_step) {
        first_tab = ((g_current_tab + g_tab_step) % kTabs + kTabs) % kTabs;
        g_tab_step = 0;
    }
    auto flags = [&](int i) { return first_tab == i ? ImGuiTabItemFlags_SetSelected : 0; };
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("General", nullptr, flags(0))) g_current_tab = 0, tab_general(c), ImGui::EndTabItem();
        if (ImGui::BeginTabItem("Display", nullptr, flags(1))) g_current_tab = 1, tab_display(c), ImGui::EndTabItem();
        if (ImGui::BeginTabItem("Controls", nullptr, flags(2))) g_current_tab = 2, tab_controls(c), ImGui::EndTabItem();
        if (ImGui::BeginTabItem("Presentation", nullptr, flags(3))) g_current_tab = 3, tab_presentation(c), ImGui::EndTabItem();
        first_tab = -1;
        ImGui::EndTabBar();
    }
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeightWithSpacing() * 1.5f);
    ImGui::Separator();
    if (ImGui::Button("Resume")) g_open = false;
    ImGui::SameLine();
    if (ImGui::Button("Quit game")) g_quit = true;
    ImGui::SameLine();
    ImGui::TextDisabled("  L1/R1 or Q/E: tabs.  (restart): applies the next time the game starts");
    ImGui::End();
    if (g_dirty) {
        app::save_config();
        g_dirty = false;
    }
}

bool is_toggle(const SDL_Event& e) {
    return (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) ||
           (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.scancode == SDL_SCANCODE_F1);
}

void run() {
    init();
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(sdl::window(), &w, &h);
    GlState saved;
    saved.save();
    g_snapshot.capture(w, h);
    audio::set_paused(true);
    SDL_ShowCursor(SDL_ENABLE);
    RLOG("menu: open");
    g_open = true;
    auto clear = gl_fn<void(__stdcall*)(unsigned)>("glClear");
    auto disable = gl_fn<void(__stdcall*)(unsigned)>("glDisable");
    // RAVEN_MENU_SECONDS: close by itself after this long (automated screenshots of the menu).
    static const char* auto_close = std::getenv("RAVEN_MENU_SECONDS");
    uint64_t opened = SDL_GetTicks64();
    while (g_open && !g_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) g_quit = true;
            if (is_toggle(e) || (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE)) g_open = false;
            if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) g_tab_step = -1;
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) g_tab_step = 1;
                // ImGui's gamepad nav uses the shoulders for tab bars too; don't let it step twice.
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER ||
                    e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
                    continue;
            }
            if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                if (e.key.keysym.scancode == SDL_SCANCODE_Q) g_tab_step = -1;
                if (e.key.keysym.scancode == SDL_SCANCODE_E) g_tab_step = 1;
            }
            ImGui_ImplSDL2_ProcessEvent(&e);
        }
        if (auto_close && SDL_GetTicks64() - opened > uint64_t(std::atof(auto_close) * 1000)) g_open = false;
        SDL_GL_GetDrawableSize(sdl::window(), &w, &h);
        disable(0x0C11);
        g_snapshot.draw(w, h);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        draw_menu(w, h);
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        sdl::present_menu_frame();
        if (!app::config().vsync) SDL_Delay(8);
    }
    (void)clear;
    if (g_quit) {
        RLOG("menu: quit");
        app::save_config();
        ExitProcess(0);
    }
    SDL_ShowCursor(SDL_DISABLE);
    // Put the game's frame back for the swap that follows.
    g_snapshot.draw(w, h);
    saved.restore();
    audio::set_paused(false);
    RLOG("menu: closed");
}
}  // namespace

void request_open() { g_requested = true; }
bool is_open() { return g_open; }

bool on_event(const SDL_Event& e) {
    if (is_toggle(e)) {
        g_requested = true;
        return true;
    }
    return false;
}

void before_present() {
    static bool auto_open_done = false;
    // RAVEN_MENU_AT=<seconds>: open the menu by itself once (automated screenshots).
    static const char* auto_open = std::getenv("RAVEN_MENU_AT");
    if (auto_open && !auto_open_done && SDL_GetTicks64() > uint64_t(std::atof(auto_open) * 1000)) {
        auto_open_done = true;
        g_requested = true;
    }
    if (!g_requested) return;
    g_requested = false;
    run();
}
}  // namespace raven::ui
