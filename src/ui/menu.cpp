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
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "app/config.h"
#include "app/paths.h"
#include "gl/gl_bridge.h"
#include "input/bindings.h"
#include "game/gameplay.h"
#include "game/movie_skip.h"
#include "game/players.h"
#include "input/input.h"
#include "ui/glyphs.h"
#include "log.h"
#include "sdl/sdl_bridge.h"

namespace raven::app { std::string pick_folder_dialog(); }
namespace raven::audio { void set_paused(bool paused); }

namespace raven::ui {
namespace {
bool g_requested = false, g_open = false, g_inited = false, g_quit = false;
int g_current_tab = 0, g_tab_step = 0;

// "Press a button" capture for the Controls tab: armed once everything held when it started is released.
struct Capture {
    bool active = false, armed = false, pad = false;
    input::Action action{};
    int slot = 0;
    uint64_t started = 0;
    int player = 0;  // whose controller bindings
} g_capture;
int g_bind_player = 0;  // Controls tab: the player whose controller bindings the table shows
// After a capture ends, the button or key that ended it may still be held. ImGui reads controller buttons from their
// held state, so a slot would see X / Square still down on the next frame and clear the binding just made. Nothing
// is cleared until everything is released.
bool g_wait_release = false;
int g_focus_slot = -1;  // tests (RAVEN_MENU_CAPTURE): the slot id to give keyboard / controller focus

bool anything_held() {
    if (SDL_GetMouseState(nullptr, nullptr)) return true;
    int n = 0;
    const Uint8* keys = SDL_GetKeyboardState(&n);
    for (int i = 0; i < n; i++)
        if (keys[i]) return true;
    for (int j = 0; j < SDL_NumJoysticks(); j++) {
        SDL_GameController* c = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(j));
        if (!c) continue;
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++)
            if (SDL_GameControllerGetButton(c, SDL_GameControllerButton(b))) return true;
        if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000 ||
            SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000)
            return true;
    }
    return false;
}

// Called for every menu event while capturing; true if the capture took it.
bool capture_event(const SDL_Event& e) {
    auto& c = g_capture;
    bool input_event = e.type == SDL_KEYDOWN || e.type == SDL_KEYUP || e.type == SDL_MOUSEBUTTONDOWN ||
                       e.type == SDL_MOUSEBUTTONUP || e.type == SDL_CONTROLLERBUTTONDOWN ||
                       e.type == SDL_CONTROLLERBUTTONUP || e.type == SDL_CONTROLLERAXISMOTION;
    if (!input_event) return false;
    if (!c.armed) return true;
    if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
        c.active = false;
        g_wait_release = true;
        return true;
    }
    input::Bind b = input::bind_from_event(e);
    if (b.kind == input::Bind::None || b.is_pad() != c.pad) return true;
    input::set_bind(c.action, c.pad, c.slot, b, c.player);
    RLOG("menu: bound %s slot %d to %s (player %d)", input::action_name(c.action), c.slot + 1, input::bind_label(b).c_str(),
         c.player + 1);
    c.active = false;
    g_wait_release = true;
    app::save_config();
    return true;
}

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
ImFont* g_game_font = nullptr;

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
    // Bahnschrift (Windows 10+), close to the game's own bold sans, for prompts drawn over the game.
    std::string game_font = std::string(windir) + "\\Fonts\\bahnschrift.ttf";
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = true;
    if (std::filesystem::exists(game_font)) g_game_font = io.Fonts->AddFontFromFileTTF(game_font.c_str(), px * 2.0f, &fc);
    if (!g_game_font) g_game_font = io.Fonts->Fonts[0];
    ImGui::GetStyle().ScaleAllSizes(px / 16.0f);
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6;
    st.FrameRounding = 4;
    st.GrabRounding = 4;
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.07f, 0.10f, 0.94f);
    ImGui_ImplSDL2_InitForOpenGL(sdl::window(), SDL_GL_GetCurrentContext());
    // Every connected controller drives the menu (ImGui's default is only the first one).
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_AutoAll);
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
            if (std::filesystem::exists(app::to_wide(dir + "\\halo\\game")) && std::filesystem::is_directory(app::to_wide(dir + "\\dump")))
                c.game_folder = dir, g_dirty = true;
            else ImGui::OpenPopup("bad_folder");
        }
    }
    restart_note();
    if (ImGui::BeginPopup("bad_folder")) {
        ImGui::Text("That folder needs both the halo and dump folders.");
        ImGui::EndPopup();
    }
    ImGui::TextDisabled("The game is read from that folder; nothing is copied.");
    ImGui::Separator();
    checkbox("Free play", c.free_play, true);
    ImGui::Separator();
    ImGui::TextDisabled("Audio: FMOD Studio by Firelight Technologies Pty Ltd.");
}

void tab_gameplay(app::Config& c) {
    combo("Difficulty", c.difficulty, {"Arcade (gets harder the longer you play)", "Easy", "Normal", "Heroic", "Legendary"});
    static const char* const kNotes[] = {
        "The original: you can't die for the first couple of minutes, then damage climbs fast (up to 5x) to end the credit.",
        "You take half damage.", "You take normal damage the whole time.", "You take double damage.",
        "You take triple damage."};
    ImGui::TextDisabled("%s", kNotes[std::clamp(c.difficulty, 0, 4)]);
    ImGui::Separator();
    int players = c.four_players ? 1 : 0;
    if (combo("Players", players, {"Up to 2 (the original cabinet)", "Up to 4 (the 4-player cabinet)"}, true))
        c.four_players = players == 1;
    ImGui::TextDisabled("Up to 4 runs the game as its 4-player cabinet: all four ODSTs can join, with a HUD for each.");
    ImGui::Separator();
    if (checkbox("Subtitles in mission movies", c.subtitles)) game::apply_subtitles();
    if (slider("Game volume", c.game_volume, 0, 100, "%d")) game::apply_game_volume();
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

PadStyle pad_style(const app::Config& c) {
    // RAVEN_MENU_ICONS=<1|2>: force the icon style (automated screenshots).
    static const char* force = std::getenv("RAVEN_MENU_ICONS");
    if (force) return std::atoi(force) == 2 ? PadStyle::PlayStation : PadStyle::Xbox;
    if (c.button_icons == 1) return PadStyle::Xbox;
    if (c.button_icons == 2) return PadStyle::PlayStation;
    return input::controller_is_playstation(0) || (!*input::controller_name(0) && input::controller_is_playstation(1))
               ? PadStyle::PlayStation
               : PadStyle::Xbox;
}

// One binding slot: a button showing the icon; click to rebind, right-click (or the controller's X / Square) to
// clear.
void bind_slot(input::Action a, bool pad, int slot, PadStyle style, float w, int player) {
    using input::Bind;
    Bind b = pad ? input::pad_bind(a, slot, player) : input::key_bind(a, slot);
    float h = ImGui::GetFrameHeight();
    ImGui::PushID(int(a) * 8 + (pad ? 4 : 0) + slot);
    bool capturing = g_capture.active && g_capture.action == a && g_capture.pad == pad && g_capture.slot == slot &&
                     (!pad || g_capture.player == player);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    if (g_focus_slot == int(a) * 8 + (pad ? 4 : 0) + slot) ImGui::SetKeyboardFocusHere(), g_focus_slot = -1;
    if (ImGui::Button("##slot", ImVec2(w, h * 1.25f))) {
        g_capture = {true, false, pad, a, slot, SDL_GetTicks64(), player};
    }
    bool clear = ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
                 (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false));
    if (clear && !g_capture.active && !g_wait_release) {
        input::set_bind(a, pad, slot, Bind{}, player);
        RLOG("menu: cleared %s slot %d (player %d)", input::action_name(a), slot + 1, player + 1);
        app::save_config();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float gh = h * 1.0f, top = pos.y + h * 0.125f;
    if (capturing) {
        const char* t = pad ? "Press a button..." : "Press a key...";
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h * 1.25f - ts.y) * 0.5f), IM_COL32(255, 210, 90, 255), t);
    } else if (b.kind == Bind::None) {
        ImVec2 ts = ImGui::CalcTextSize("-");
        dl->AddText(ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h * 1.25f - ts.y) * 0.5f), IM_COL32(120, 125, 135, 255), "-");
    } else {
        // The icon says it all (no words beside it).
        float gw = glyph_width(b, gh, style);
        draw_glyph(dl, ImVec2(pos.x + (w - gw) * 0.5f, top), gh, b, style);
    }
    ImGui::PopID();
}

// Player p's ODST: the ODSTs this cabinet has. Picking one another player has swaps the two.
void odst_combo(app::Config& c, int p) {
    int mine = game::player_slot(p);
    // "Ethan Graves (blue)": the color of their JOIN NOW panel, HUD and crosshair, for players who don't know the names.
    auto label = [](int slot) { return std::string(game::odst_name(slot)) + " (" + game::odst_color(slot) + ")"; };
    ImGui::PushID(p);
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("Marcus Hudson (orange)").x + ImGui::GetFrameHeight() * 2);
    if (ImGui::BeginCombo("##odst", label(mine).c_str())) {
        for (int seat = 0; seat < game::slot_count(); seat++) {
            int slot = game::slot_at(seat);
            if (!ImGui::Selectable(label(slot).c_str(), slot == mine) || slot == mine) continue;
            // Write everyone's current ODST, then swap with whoever had this one.
            for (int i = 0; i < game::player_count(); i++) c.odst[i] = game::player_slot(i);
            for (int i = 0; i < game::player_count(); i++)
                if (c.odst[i] == slot) c.odst[i] = mine;
            c.odst[p] = slot;
            g_dirty = true;
            RLOG("menu: player %d plays %s", p + 1, game::odst_name(slot));
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

void tab_controls(app::Config& c) {
    using input::Action;
    PadStyle style = pad_style(c);
    // Who plays which ODST, with what.
    int players = game::player_count();
    if (ImGui::BeginTable("players", 3, ImGuiTableFlags_SizingFixedFit)) {
        for (int p = 0; p < players; p++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Player %d", p + 1);
            ImGui::TableNextColumn();
            odst_combo(c, p);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            std::string who = p == 0 ? "Keyboard & mouse" : "";
            const char* pad = input::controller_name(p);
            if (*pad) who += (who.empty() ? "" : " + ") + std::string(pad);
            if (who.empty()) ImGui::TextDisabled("connect a controller");
            else ImGui::TextUnformatted(who.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Each ODST has their own seat: their HUD, JOIN NOW panel and color.");
    if (checkbox("Keyboard & mouse is player 1, controllers start at player 2", c.kbm_own_player))
        input::reassign_controllers();
    combo("Button icons", c.button_icons, {"Match the controller", "Xbox", "PlayStation"});
    slider("Rumble", c.rumble, 0, 100);
    slider("Aim speed", c.aim_speed, 20, 300);
    slider("Stick deadzone", c.aim_deadzone, 0, 50);
    ImGui::Separator();
    ImGui::TextDisabled("Both sticks aim. Click a slot and press the new button (Esc cancels);");
    ImGui::TextDisabled("right-click or X / Square clears it. Each player has their own controller buttons.");
    // Whose controller buttons the table shows: one button per player (the keyboard and mouse are player 1's).
    g_bind_player = std::clamp(g_bind_player, 0, players - 1);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Buttons for");
    for (int p = 0; p < players; p++) {
        ImGui::SameLine();
        char name[16];
        std::snprintf(name, sizeof name, "Player %d", p + 1);
        bool selected = p == g_bind_player;
        if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(name)) g_bind_player = p, g_capture.active = false;
        if (selected) ImGui::PopStyleColor();
    }
    bool keys = g_bind_player == 0;
    float slot_w = std::max(ImGui::GetFrameHeight() * 4.6f, ImGui::CalcTextSize("Press a button...").x + 20);
    if (ImGui::BeginTable("binds", keys ? 5 : 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthFixed, slot_w);
        ImGui::TableSetupColumn("##pad2", ImGuiTableColumnFlags_WidthFixed, slot_w);
        if (keys) {
            ImGui::TableSetupColumn("Keyboard & mouse", ImGuiTableColumnFlags_WidthFixed, slot_w);
            ImGui::TableSetupColumn("##key2", ImGuiTableColumnFlags_WidthFixed, slot_w);
        }
        ImGui::TableHeadersRow();
        ImGui::PushID(g_bind_player);
        for (int i = 0; i < input::kActionCount; i++) {
            auto a = Action(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(input::action_name(a));
            for (int col = 0; col < (keys ? 4 : 2); col++) {
                ImGui::TableNextColumn();
                bind_slot(a, col < 2, col % 2, style, slot_w, g_bind_player);
            }
        }
        ImGui::PopID();
        ImGui::EndTable();
    }
    if (players > 1) {
        char copy[64];
        std::snprintf(copy, sizeof copy, "Give every player player %d's buttons", g_bind_player + 1);
        if (ImGui::Button(copy)) input::copy_pad_binds(g_bind_player), app::save_config();
        ImGui::SameLine();
    }
    if (ImGui::Button("Reset controls to defaults")) input::reset_binds(), app::save_config();
    ImGui::SameLine();
    ImGui::TextDisabled("Fixed: 5 coin, F2 operator test, F3 service.");
}

void tab_presentation(app::Config& c) {
    checkbox("Hide \"Money In\" / \"FREEPLAY\" text", c.hide_credit_text, true);
    checkbox("No countdown on the game mode menu", c.no_menu_countdown, true);
    checkbox("Skip the Xbox Live \"Get Connected\" screen", c.skip_xbox_screen, true);
    checkbox("Hide QR scanner notices", c.hide_qr_notices, true);
    checkbox("Boot image instead of boot text", c.boot_image, true);
    checkbox("Start skips mission movies (instead of Reload)", c.start_skips_movies, true);
    checkbox("Mission movies fill wide screens", c.movies_fill);
    checkbox("Hide the other ODSTs' \"JOIN NOW / Press START\" panels (all but player 1's)", c.hide_p2_join, true);
}

void draw_menu(int w, int h) {
    auto& c = app::config();
    // Layout in ImGui's display units (the window's size as SDL reports it), not framebuffer pixels.
    ImVec2 disp = ImGui::GetIO().DisplaySize;
    float scale = std::max(0.75f, disp.y / 1080.0f) * (float(h) / std::max(1.0f, disp.y)) / std::max(0.01f, ImGui::GetIO().DisplayFramebufferScale.y);
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(1250 * scale, disp.x - 20), std::min(1000 * scale, disp.y * 0.94f)), ImGuiCond_Always);
    ImGui::Begin("Project Raven", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    // RAVEN_MENU_TAB=<0-4>: start on that tab (automated screenshots).
    static int first_tab = [] { const char* t = std::getenv("RAVEN_MENU_TAB"); return t ? std::atoi(t) : -1; }();
    // L1/R1 (LB/RB), Q/E or Ctrl+Tab move between tabs (g_tab_step, from run()).
    constexpr int kTabs = 5;
    if (g_tab_step) {
        first_tab = ((g_current_tab + g_tab_step) % kTabs + kTabs) % kTabs;
        g_tab_step = 0;
    }
    auto flags = [&](int i) { return first_tab == i ? ImGuiTabItemFlags_SetSelected : 0; };
    if (ImGui::BeginTabBar("tabs")) {
        // Each tab scrolls in the space above the Resume / Quit row.
        float footer = ImGui::GetFrameHeightWithSpacing() * 1.6f;
        auto tab = [&](const char* name, int i, void (*body)(app::Config&)) {
            if (!ImGui::BeginTabItem(name, nullptr, flags(i))) return;
            g_current_tab = i;
            ImGui::BeginChild("content", ImVec2(0, -footer), ImGuiChildFlags_None, ImGuiWindowFlags_NavFlattened);
            body(c);
            // RAVEN_MENU_SCROLL=<0..1>: scroll the tab that far down (automated screenshots).
            static const char* scroll = std::getenv("RAVEN_MENU_SCROLL");
            if (scroll) ImGui::SetScrollY(ImGui::GetScrollMaxY() * float(std::atof(scroll)));
            ImGui::EndChild();
            ImGui::EndTabItem();
        };
        tab("General", 0, tab_general);
        tab("Gameplay", 1, tab_gameplay);
        tab("Display", 2, tab_display);
        tab("Controls", 3, tab_controls);
        tab("Presentation", 4, tab_presentation);
        first_tab = -1;
        ImGui::EndTabBar();
    }
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeightWithSpacing() * 1.5f);
    ImGui::Separator();
    if (ImGui::Button("Resume")) g_open = false;
    ImGui::SameLine();
    if (ImGui::Button("Quit game")) g_quit = true;
    ImGui::SameLine();
    ImGui::TextDisabled("  (restart): applies the next time the game starts");
    if (g_capture.active && !g_capture.armed && !anything_held()) g_capture.armed = true;
    if (g_capture.active && SDL_GetTicks64() - g_capture.started > 8000) g_capture.active = false, g_wait_release = true;
    if (g_wait_release && !g_capture.active && !anything_held()) g_wait_release = false;
    ImGui::End();
    if (g_dirty) {
        app::save_config();
        g_dirty = false;
    }
}

// The Settings menu binding (Select / F1 by default). With nothing bound on a device, its default still works there.
bool is_toggle(const SDL_Event& e) {
    using input::Action;
    using input::Bind;
    if (input::event_presses(e, Action::Menu)) return true;
    bool pad_empty = input::pad_bind(Action::Menu, 0).kind == Bind::None && input::pad_bind(Action::Menu, 1).kind == Bind::None;
    bool key_empty = input::key_bind(Action::Menu, 0).kind == Bind::None && input::key_bind(Action::Menu, 1).kind == Bind::None;
    return (pad_empty && e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) ||
           (key_empty && e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.scancode == SDL_SCANCODE_F1);
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
    // Automated test runs (RAVEN_HEADLESS) never read the user's real controllers: only the script's virtual ones
    // drive the menu.
    if (std::getenv("RAVEN_HEADLESS")) {
        std::vector<SDL_GameController*> pads;
        for (int i = 0; i < SDL_NumJoysticks(); i++)
            if (SDL_JoystickIsVirtual(i) && SDL_IsGameController(i))
                if (SDL_GameController* c = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i))) pads.push_back(c);
        ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual, pads.data(), int(pads.size()));
    }
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
            if (g_capture.active) {
                if (capture_event(e)) continue;
            } else if (is_toggle(e) || (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE)) {
                g_open = false;
            }
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
        // RAVEN_MENU_PRESS=<seconds>:<button>: hold that button on virtual controller 0 for 0.5 s, that long after
        // the menu opens (tests, e.g. clearing a focused slot with X).
        static const char* press = std::getenv("RAVEN_MENU_PRESS");
        static int press_step = 0;
        if (press && press_step < 2) {
            double at = 0;
            int button = 0;
            std::sscanf(press, "%lf:%d", &at, &button);
            uint64_t t = SDL_GetTicks64() - opened;
            if (press_step == 0 && t > uint64_t(at * 1000) && input::set_virtual_button(0, button, true)) press_step = 1;
            else if (press_step == 1 && t > uint64_t(at * 1000) + 500) input::set_virtual_button(0, button, false), press_step = 2;
        }
        if (auto_close && SDL_GetTicks64() - opened > uint64_t(std::atof(auto_close) * 1000)) g_open = false;
        // RAVEN_MENU_CAPTURE=<action>:<pad 0|1>:<slot>:<code>: 1 s after opening, focus that slot and start a capture
        // on it (as pressing A on it does); 1 s later press <code> (a scancode, or a controller button). With a
        // virtual controller attached (test script "pad:0") the button is held on it for 0.5 s, like a real press;
        // otherwise a press and release are sent as events.
        static const char* cap = std::getenv("RAVEN_MENU_CAPTURE");
        static int cap_step = 0;
        if (cap && cap_step < 3) {
            int a = 0, pad = 0, slot = 0, code = 0;
            std::sscanf(cap, "%d:%d:%d:%d", &a, &pad, &slot, &code);
            uint64_t t = SDL_GetTicks64() - opened;
            if (cap_step == 0 && t > 1000) {
                g_focus_slot = a * 8 + (pad ? 4 : 0) + slot;
                g_capture = {true, false, pad != 0, input::Action(a), slot, SDL_GetTicks64(), g_bind_player};
                cap_step = 1;
            } else if (cap_step == 1 && t > 2000 && pad && input::set_virtual_button(0, code, true)) {
                cap_step = 2;
            } else if (cap_step == 2 && t > 2500) {
                input::set_virtual_button(0, code, false);
                cap_step = 3;
            } else if (cap_step == 1 && t > 2000) {
                for (bool down : {true, false}) {
                    SDL_Event ev{};
                    if (pad) {
                        ev.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
                        ev.cbutton.button = uint8_t(code);
                        ev.cbutton.state = down ? SDL_PRESSED : SDL_RELEASED;
                    } else {
                        ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
                        ev.key.keysym.scancode = SDL_Scancode(code);
                        ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
                    }
                    SDL_PushEvent(&ev);
                }
                cap_step = 3;
            }
        }
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

// "Press <Start> to skip" over a mission movie (src/game/movie_skip.cpp), where the game's "Press RELOAD to skip"
// was: top center, light gray. The icon is the Start binding of the controller in use, else the keyboard's.
void draw_skip_prompt() {
    init();
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(sdl::window(), &w, &h);
    GlState saved;
    saved.save();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;  // the game hides the cursor; keep it that way
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    using input::Bind;
    int owner = -1;  // the first player with a controller: their Start button
    for (int p = game::player_count() - 1; p >= 0; p--)
        if (*input::controller_name(p)) owner = p;
    bool pad = owner >= 0;
    Bind b = pad ? input::pad_bind(input::Action::Start, 0, owner) : Bind{};
    if (pad && b.kind == Bind::None) b = input::pad_bind(input::Action::Start, 1, owner);
    if (b.kind == Bind::None) {
        pad = false;
        b = input::key_bind(input::Action::Start, 0);
        if (b.kind == Bind::None) b = input::key_bind(input::Action::Start, 1);
    }
    PadStyle style = pad && input::controller_is_playstation(owner) ? PadStyle::PlayStation : PadStyle::Xbox;
    // Sizes in ImGui display units (the window), from the game's prompt: text ~5% of the height, centered 3.6% down.
    ImVec2 disp = io.DisplaySize;
    float size = disp.y * 0.066f, glyph_h = size * 0.85f;
    ImFont* f = g_game_font;
    const char* left = "Press ";
    const char* right = " to skip";
    float lw = f->CalcTextSizeA(size, FLT_MAX, 0, left).x, rw = f->CalcTextSizeA(size, FLT_MAX, 0, right).x;
    float gw = b.kind == Bind::None ? 0 : glyph_width(b, glyph_h, style) + size * 0.1f;
    float x = disp.x * 0.5f - (lw + gw + rw) * 0.5f, cy = disp.y * 0.036f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImU32 text = IM_COL32(218, 220, 224, 255), shadow = IM_COL32(0, 0, 0, 160);
    auto put = [&](float at, const char* t) {
        ImVec2 p(at, cy - size * 0.5f);
        dl->AddText(f, size, ImVec2(p.x + size * 0.05f, p.y + size * 0.05f), shadow, t);
        for (float dx : {0.0f, size * 0.025f}) dl->AddText(f, size, ImVec2(p.x + dx, p.y), text, t);  // bolder
    };
    put(x, left);
    if (gw > 0) draw_glyph(dl, ImVec2(x + lw + size * 0.05f, cy - glyph_h * 0.5f), glyph_h, b, style);
    put(x + lw + gw, right);
    ImGui::Render();
    auto bind_fb = gl_fn<void(__stdcall*)(unsigned, unsigned)>("glBindFramebuffer");
    auto viewport = gl_fn<void(__stdcall*)(int, int, int, int)>("glViewport");
    bind_fb(0x8D40, 0);
    viewport(0, 0, w, h);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    saved.restore();
}

void before_present() {
    if (game::movie_skip_prompt() && !g_open) draw_skip_prompt();
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
