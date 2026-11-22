#include "picker.hpp"

#include <rsdkair/paths.hpp>
#include <rsdkair/s3air.hpp>
#include <rsdkair/version.hpp>

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace rsdkair {

namespace {

constexpr float kPi = 3.14159265358979f;

// Layout at 1280x720; everything scales with the window (see Impl::ui).
constexpr float kMargin = 54.0f;
constexpr float kRounding = 18.0f;
constexpr int kGlowSize = 48;

ImTextureRef texture_ref(SDL_Texture *tex) { return ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(tex))); }

ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

ImU32 with_alpha(ImU32 col, float alpha)
{
    const int a = static_cast<int>(((col >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
    return (col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

float approach(float value, float target, float dt, float speed = 14.0f)
{
    return value + (target - value) * (1.0f - std::exp(-speed * dt));
}

// Where art/ and fonts/ live: next to the executable on desktop, the APK's assets on
// Android (SDL_RWFromFile reads relative paths from there).
std::string resource_dir()
{
#if defined(__ANDROID__)
    return {};
#else
    char *base = SDL_GetBasePath();
    if (!base)
        return {};
    std::string dir = base;
    SDL_free(base);
    return dir;
#endif
}

std::vector<unsigned char> read_resource(const std::string &path)
{
    std::vector<unsigned char> data;
    SDL_RWops *rw = SDL_RWFromFile(path.c_str(), "rb");
    if (!rw)
        return data;
    std::size_t size = 0;
    void *bytes = SDL_LoadFile_RW(rw, &size, 1);
    if (bytes) {
        data.assign(static_cast<unsigned char *>(bytes), static_cast<unsigned char *>(bytes) + size);
        SDL_free(bytes);
    }
    return data;
}

SDL_Texture *make_texture(SDL_Renderer *renderer, const unsigned char *rgba_pixels, int w, int h)
{
    SDL_Texture *tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!tex)
        return nullptr;
    SDL_UpdateTexture(tex, nullptr, rgba_pixels, w * 4);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
    return tex;
}

// A small, heavily blurred copy of a cover for the backdrop (scaled up with linear
// filtering, it reads as a soft glow of the cover's colours).
std::vector<unsigned char> blurred(const unsigned char *src, int w, int h, int size)
{
    std::vector<float> img(static_cast<std::size_t>(size * size * 3), 0.0f);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int x0 = x * w / size, x1 = std::max(x0 + 1, (x + 1) * w / size);
            const int y0 = y * h / size, y1 = std::max(y0 + 1, (y + 1) * h / size);
            float sum[3] = {0, 0, 0};
            for (int sy = y0; sy < y1; ++sy) {
                for (int sx = x0; sx < x1; ++sx) {
                    const unsigned char *p = src + (static_cast<std::size_t>(sy) * w + sx) * 4;
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                }
            }
            const float n = static_cast<float>((x1 - x0) * (y1 - y0));
            for (int c = 0; c < 3; ++c)
                img[(static_cast<std::size_t>(y) * size + x) * 3 + c] = sum[c] / n;
        }
    }
    // Three box-blur passes, each horizontal then vertical.
    const int r = std::max(1, size / 12);
    std::vector<float> tmp(img.size());
    for (int pass = 0; pass < 6; ++pass) {
        const bool horizontal = pass % 2 == 0;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                float sum[3] = {0, 0, 0};
                int n = 0;
                for (int k = -r; k <= r; ++k) {
                    const int sx = horizontal ? std::clamp(x + k, 0, size - 1) : x;
                    const int sy = horizontal ? y : std::clamp(y + k, 0, size - 1);
                    for (int c = 0; c < 3; ++c)
                        sum[c] += img[(static_cast<std::size_t>(sy) * size + sx) * 3 + c];
                    ++n;
                }
                for (int c = 0; c < 3; ++c)
                    tmp[(static_cast<std::size_t>(y) * size + x) * 3 + c] = sum[c] / static_cast<float>(n);
            }
        }
        img.swap(tmp);
    }
    std::vector<unsigned char> out(static_cast<std::size_t>(size * size * 4));
    for (int i = 0; i < size * size; ++i) {
        for (int c = 0; c < 3; ++c)
            out[static_cast<std::size_t>(i) * 4 + c] = static_cast<unsigned char>(img[static_cast<std::size_t>(i) * 3 + c]);
        out[static_cast<std::size_t>(i) * 4 + 3] = 255;
    }
    return out;
}

struct Cover {
    SDL_Texture *art = nullptr;   // <id>.jpg, square
    SDL_Texture *glow = nullptr;  // blurred art, for the backdrop
};

// Which part of a cover to keep when it is cropped to the tile, from 0 (keep the left
// edge) to 1 (keep the right edge). Centred unless the art needs otherwise.
float cover_anchor_x(std::string_view id)
{
    // At 4:5 the crop keeps 80% of a square cover's width.
    if (id == "soniccd")
        return 0.93f;  // the disc (x 367-500 of 512) ends 5 px from the right, as from the bottom
    if (id == "s3air")
        return 0.9f;   // keeps Robotnik's Egg Mobile (x 419-488 of 512) whole
    return 0.5f;
}

// Credits ------------------------------------------------------------------------------
//
// The RSDK and Sonic Mania decompilation licenses require crediting the original and the
// decompilation authors (names as their LICENSE.md files give them); Sonic 3 A.I.R. is
// GPL-3.0, so its source is pointed to. See ROADMAP/2026-11-20/upstreams-and-licensing.md.

// One line under a game's title: who made the engine it runs on.
std::string engine_credit(std::string_view module)
{
    if (module == "rsdkv3")
        return "RSDK v3 by Christian \"Taxman\" Whitehead. Decompilation by Rubberduckycooly and st\xC3\x97tic.";
    if (module == "rsdkv4")
        return "RSDK v4 by Christian \"Taxman\" Whitehead. Decompilation by Rubberduckycooly and st\xC3\x97tic.";
    if (module == "rsdkv5")
        return "RSDK v5 by Evening Star. Decompilations by Rubberduckycooly, chuliRMG and st\xC3\x97tic.";
    if (module == "sonic3air")
        return "Sonic 3 A.I.R. by Eukaryot (GPL-3.0).";
    return {};
}

struct Credit {
    const char *what;
    const char *who;
};

struct CreditSection {
    const char *title;
    std::vector<Credit> lines;
};

const std::vector<CreditSection> &credits()
{
    static const std::vector<CreditSection> sections = {
        {"GAME ENGINES",
         {
             {"RSDK v3 and v4 (Sonic CD, Sonic 1, Sonic 2)",
              "Original engine by Christian \"Taxman\" Whitehead. Decompilation by Rubberduckycooly and "
              "st\xC3\x97tic (RSDKModding). RSDKv3/v4 Decompilation License: non-commercial."},
             {"RSDK v5U (Sonic Mania)",
              "Original engine by Evening Star. Decompilation by Rubberduckycooly and chuliRMG (RSDKModding). "
              "RSDKv5(U) Decompilation License: non-commercial."},
             {"Sonic Mania game code",
              "Decompilation by Rubberduckycooly, with st\xC3\x97tic (RSDKModding). Sonic Mania Decompilation "
              "License: non-commercial."},
             {"Sonic 3 A.I.R.",
              "By Eukaryot. GPL-3.0, source code at github.com/Eukaryot/sonic3air. It runs as its own program "
              "(its own app on Android), never inside rsdk-air."},
         }},
        {"LIBRARIES",
         {
             {"SDL2", "Sam Lantinga and contributors. zlib License."},
             {"Dear ImGui", "Omar Cornut and contributors. MIT License."},
             {"libogg, libvorbis, libtheora", "Xiph.Org Foundation. BSD License."},
             {"stb_image, stb_vorbis", "Sean Barrett. MIT License or public domain."},
             {"tinyxml2, theoraplay", "Lee Thomason; Ryan C. Gordon. zlib License."},
             {"iniparser, miniz", "Nicolas Devillard; Rich Geldreich and others. MIT License."},
             {"GLEW (desktop)", "Modified BSD, Mesa (MIT) and Khronos licenses."},
             {"Inter (font)", "Rasmus Andersson. SIL Open Font License 1.1."},
         }},
        {"ART",
         {
             {"Covers", "Fan art made for play.germade by its owner, in the style of the games."},
             {"Save screen", "Characters and zone pictures from rsdkv4-wasm (wasm-gaming), after craft's game "
                             "launcher."},
         }},
        {"RSDK-AIR",
         {
             {"rsdk-air", "A free, non-commercial fan project. MIT License, source code at "
                          "github.com/jgermade/rsdk-air. The license texts of everything above are in the "
                          "licenses folder of each download."},
             {"SEGA", "Not affiliated with or endorsed by SEGA. Sonic the Hedgehog and its characters are "
                      "trademarks of SEGA. Play with your own copy of the games."},
         }},
    };
    return sections;
}

// The engine's name as players know it, in capitals for the panel's label.
std::string engine_label(std::string_view module)
{
    if (module == "rsdkv3")
        return "RSDKV3";
    if (module == "rsdkv4")
        return "RSDKV4";
    if (module == "rsdkv5")
        return "RSDKV5U";
    if (module == "sonic3air")
        return "OXYGEN ENGINE";
    std::string label(module);
    for (char &ch : label)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return label;
}

// A game that is played in another app, with where to get that app (Android only:
// Sonic 3 A.I.R.'s official app). Empty when the game has no such app.
std::string get_app_url(const GameStatus &game)
{
#if defined(__ANDROID__)
    if (game.spec->module == "sonic3air" && !game.engine_found)
        return "https://sonic3air.org/";
#else
    (void)game;
#endif
    return {};
}

// A game rsdk-air can install for the player: Sonic 3 A.I.R. on desktop, from its official
// release (src/core/s3air.cpp).
bool installable(const GameStatus &game)
{
#if defined(__ANDROID__)
    (void)game;
    return false;
#else
    return game.spec->module == "sonic3air" && !game.engine_found && s3air_package() != nullptr;
#endif
}

// A game whose data the player can import with the system file picker (Android only: the
// data folder is private to the app, so there is no other way to put files there).
bool importable(const GameStatus &game)
{
#if defined(__ANDROID__)
    return game.engine_found && !game.available;
#else
    (void)game;
    return false;
#endif
}

enum class Action { None, Left, Right, Up, Down, Accept, Back, More, Credits };

} // namespace

struct Picker::Impl {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    bool imgui_ready = false;
    std::vector<std::vector<unsigned char>> font_data;  // must outlive the font atlas
    ImFont *font_regular = nullptr;
    ImFont *font_bold = nullptr;
    std::string resources;
    std::vector<Cover> covers;  // one per catalog() game
    std::vector<SDL_GameController *> pads;

    // State between frames.
    int focus = -1;
    std::vector<float> lift;       // focus animation per tile, 0..1
    int backdrop = -1, backdrop_prev = -1;
    float backdrop_mix = 1.0f;
    bool confirm_quit = false;
    int confirm_focus = 0;         // 0 No, 1 Yes
    float confirm_anim = 0.0f;
    int stick_dir = 0;             // last left-stick direction, for repeats
    Uint32 stick_next = 0;
    bool pointer_used = false;     // hover moves the focus only after the mouse moved
    float inset_l = 0, inset_t = 0, inset_r = 0, inset_b = 0;  // unsafe screen edges (Android)

    // Credits screen (a Credits pill in the header, X / C / F1).
    bool credits_open = false;
    float credits_anim = 0.0f;
    float credits_scroll = 0.0f;      // pixels scrolled
    float credits_scroll_max = 0.0f;  // from the last frame's layout
    bool credits_dragged = false;  // the finger moved: releasing it is not a tap
    ImVec2 credits_pill_min, credits_pill_max;
    ImVec2 credits_close_min, credits_close_max;

    // Asks Android which edges of the screen are covered by system bars or a display
    // cutout (camera notch). Elsewhere the whole window is usable.
    void update_insets()
    {
#if defined(__ANDROID__)
        auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
        auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
        if (!env || !activity)
            return;
        jclass cls = env->GetObjectClass(activity);
        jmethodID id = env->GetMethodID(cls, "getSafeInsets", "()[I");
        if (id) {
            auto arr = static_cast<jintArray>(env->CallObjectMethod(activity, id));
            if (arr && !env->ExceptionCheck() && env->GetArrayLength(arr) == 4) {
                jint v[4] = {};
                env->GetIntArrayRegion(arr, 0, 4, v);
                // Window coordinates on Android are pixels, like the insets.
                inset_l = static_cast<float>(v[0]);
                inset_t = static_cast<float>(v[1]);
                inset_r = static_cast<float>(v[2]);
                inset_b = static_cast<float>(v[3]);
            }
            if (arr)
                env->DeleteLocalRef(arr);
        }
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(cls);
        env->DeleteLocalRef(activity);
#endif
    }

    // The part of the window nothing covers; all content is laid out inside it.
    ImVec2 safe_min() const { return ImVec2(inset_l, inset_t); }
    ImVec2 safe_max() const
    {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        return ImVec2(std::max(inset_l + 1.0f, size.x - inset_r), std::max(inset_t + 1.0f, size.y - inset_b));
    }
    ImVec2 safe_size() const
    {
        const ImVec2 a = safe_min(), b = safe_max();
        return ImVec2(b.x - a.x, b.y - a.y);
    }

    ~Impl()
    {
        for (Cover &c : covers) {
            if (c.art)
                SDL_DestroyTexture(c.art);
            if (c.glow)
                SDL_DestroyTexture(c.glow);
        }
        for (SDL_GameController *pad : pads)
            SDL_GameControllerClose(pad);
        if (imgui_ready) {
            ImGui_ImplSDLRenderer2_Shutdown();
            ImGui_ImplSDL2_Shutdown();
            ImGui::DestroyContext();
        }
        if (renderer)
            SDL_DestroyRenderer(renderer);
        if (window)
            SDL_DestroyWindow(window);
    }

    ImFont *load_font(const char *file)
    {
        std::vector<unsigned char> data = read_resource(resources + "fonts/" + file);
        if (data.empty())
            return nullptr;
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        font_data.push_back(std::move(data));
        std::vector<unsigned char> &kept = font_data.back();
        return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(kept.data(), static_cast<int>(kept.size()), 18.0f, &cfg);
    }

    SDL_Texture *load_texture(const std::string &path, std::vector<unsigned char> *glow)
    {
        const std::vector<unsigned char> file = read_resource(path);
        if (file.empty())
            return nullptr;
        int w = 0, h = 0, n = 0;
        unsigned char *pixels = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &w, &h, &n, 4);
        if (!pixels)
            return nullptr;
        SDL_Texture *tex = make_texture(renderer, pixels, w, h);
        if (glow)
            *glow = blurred(pixels, w, h, kGlowSize);
        stbi_image_free(pixels);
        return tex;
    }

    void load_covers()
    {
        covers.assign(catalog().size(), Cover{});
        for (std::size_t i = 0; i < catalog().size(); ++i) {
            const std::string base = resources + "art/covers/" + std::string(catalog()[i].id);
            std::vector<unsigned char> glow;
            covers[i].art = load_texture(base + ".jpg", &glow);
            if (!glow.empty())
                covers[i].glow = make_texture(renderer, glow.data(), kGlowSize, kGlowSize);
        }
    }

    // Layout ---------------------------------------------------------------------------
    //
    // All covers in one row, square, with the title and year under each one; the details
    // panel below the row. If the window is too narrow for one row (a phone held upright),
    // the covers wrap onto more rows.

    struct Layout {
        float u = 1.0f;
        float tile = 0.0f;    // cover width
        float tile_h = 0.0f;  // cover height (kCoverAspect taller)
        std::vector<ImVec2> min, max;  // cover rectangle per game
        ImVec2 panel_min, panel_max;   // area for the details panel
        bool portrait = false;         // covers closer, no year under them
    };

    static constexpr float kCaption = 52.0f;
    static constexpr float kCaptionPortrait = 0.0f;  // no title or year: the panel names the game
    static constexpr float kCoverAspect = 1.25f;  // height / width: 4:5, a little taller than wide, like a box  // title + year under a cover, at u = 1

    // Scale of the whole layout: 1.0 at 1280x720, or at 720x1280 when upright, so a phone
    // shows text and buttons at the same size in both orientations.
    float ui() const
    {
        const ImVec2 size = safe_size();
        // Upright: a 600x1150 reference, so a phone's header, panel and hints are as easy
        // to read as in landscape (720x1280 came out small on phones).
        const float scale = size.y > size.x ? std::min(size.x / 600.0f, size.y / 1150.0f)
                                            : std::min(size.x / 1280.0f, size.y / 720.0f);
        return std::clamp(scale, 0.5f, 2.5f);
    }

    Layout layout(int count) const
    {
        Layout l;
        const ImVec2 origin = safe_min();
        const ImVec2 size = safe_size();
        const float u = l.u = ui();
        const float gap = 40.0f * u;  // generous: the covers are busy pictures
        const float margin = kMargin * u;
        const float left = origin.x + margin;
        float top = 96.0f * u;
        const float width = size.x - 2 * margin;
        auto tile_for = [&](int c) { return (width - static_cast<float>(c - 1) * gap) / static_cast<float>(c); };
        if (size.y > size.x)
            return layout_portrait(l, count, origin, size, top, 20.0f * u, margin, tile_for(2));
        int cols = std::max(1, count);
        while (cols > 1 && tile_for(cols) < 130.0f * u)
            --cols;
        // Leave room below for the panel (about 190 px) and the hints.
        const int rows = (count + cols - 1) / cols;
        const float room = size.y - top - 64.0f * u - 200.0f * u;
        const float by_height =
            (room - static_cast<float>(rows) * (kCaption * u + gap)) / static_cast<float>(rows) / kCoverAspect;
        // The row spans the details panel's width, as in portrait: covers as wide as that
        // allows; if the height stops them first, the space between them grows instead.
        l.tile = std::max(64.0f * u, std::min(tile_for(cols), by_height));
        l.tile_h = l.tile * kCoverAspect;
        const float hgap = cols > 1 ? (width - static_cast<float>(cols) * l.tile) / static_cast<float>(cols - 1) : 0.0f;
        // Centre covers + panel (about 160 px) vertically in the space between header and hints.
        const float block = static_cast<float>(rows) * (l.tile_h + kCaption * u + gap) - gap + 22.0f * u + 160.0f * u;
        top = origin.y + std::max(top, top + (size.y - 64.0f * u - top - block) / 2);
        for (int i = 0; i < count; ++i) {
            const float x = left + static_cast<float>(i % cols) * (l.tile + hgap);
            const float y = top + static_cast<float>(i / cols) * (l.tile_h + kCaption * u + gap);
            l.min.emplace_back(x, y);
            l.max.emplace_back(x + l.tile, y + l.tile_h);
        }
        const float grid_bottom = top + static_cast<float>(rows) * (l.tile_h + kCaption * u + gap) - gap;
        l.panel_min = ImVec2(left, grid_bottom + 22.0f * u);
        l.panel_max = ImVec2(origin.x + size.x - margin, origin.y + size.y - 62.0f * u);
        return l;
    }

    // Portrait (a phone held upright): square covers two per row; with an odd count the
    // last one spans both columns at 16:9 (2 + 2 + 1 wide for five games).
    Layout layout_portrait(Layout &l, int count, ImVec2 origin, ImVec2 size, float top, float gap, float margin,
                           float max_tile) const
    {
        const float u = l.u;
        l.portrait = true;
        const float cap = kCaptionPortrait * u;  // the title only
        const float width = size.x - 2 * margin;
        const int full_rows = count / 2;
        const bool wide_last = count % 2 == 1;
        constexpr float kWide = 9.0f / 16.0f;  // height / width of the wide cover
        // Covers as wide as the width allows (the margin keeps air at the sides), closer
        // together than in landscape: gap is smaller here.
        max_tile = (size.x - 2 * margin - gap) / 2;
        // Height used: full_rows * (tile + cap + gap) + [ (2 tile + gap) * kWide + cap ].
        // Below them: the details panel (title on two lines, status, engine credit) and
        // the hints, about 300 at scale 1.
        const float room = size.y - top - 64.0f * u - 300.0f * u;
        const float per_tile = static_cast<float>(full_rows) + (wide_last ? 2 * kWide : 0.0f);
        const float fixed = static_cast<float>(full_rows) * (cap + gap) + (wide_last ? gap * kWide + cap : -gap);
        const float by_height = per_tile > 0 ? (room - fixed) / per_tile : max_tile;
        l.tile = std::max(64.0f * u, std::min(max_tile, by_height));
        l.tile_h = l.tile;
        const float grid_w = count > 1 ? 2 * l.tile + gap : l.tile;
        const float wide_h = grid_w * kWide;
        const float block = static_cast<float>(full_rows) * (l.tile + cap + gap) + (wide_last ? wide_h + cap : -gap) +
                            22.0f * u + 250.0f * u;
        top = origin.y + std::max(top, top + (size.y - 64.0f * u - top - block) / 2);
        const float x0 = origin.x + margin + (width - grid_w) / 2;
        for (int i = 0; i < count; ++i) {
            const float y = top + static_cast<float>(i / 2) * (l.tile + cap + gap);
            if (wide_last && i == count - 1) {
                l.min.emplace_back(x0, y);
                l.max.emplace_back(x0 + grid_w, y + wide_h);
            } else {
                const float x = x0 + static_cast<float>(i % 2) * (l.tile + gap);
                l.min.emplace_back(x, y);
                l.max.emplace_back(x + l.tile, y + l.tile);
            }
        }
        const float grid_bottom = l.max.empty() ? top : l.max.back().y + cap;  // caption under the last row
        l.panel_min = ImVec2(origin.x + margin, grid_bottom + 22.0f * u);
        l.panel_max = ImVec2(origin.x + size.x - margin, origin.y + size.y - 62.0f * u);
        return l;
    }

    // Grid helpers -------------------------------------------------------------------

    // The rectangle nearest to `from` in the direction of `action`, or -1.
    static int nearest(const ImVec2 *mins, const ImVec2 *maxs, int count, int from, Action action)
    {
        const ImVec2 c((mins[from].x + maxs[from].x) / 2, (mins[from].y + maxs[from].y) / 2);
        int best = -1;
        float best_score = FLT_MAX;
        for (int i = 0; i < count; ++i) {
            if (i == from)
                continue;
            const ImVec2 o((mins[i].x + maxs[i].x) / 2, (mins[i].y + maxs[i].y) / 2);
            const float dx = o.x - c.x, dy = o.y - c.y;
            float along = 0, across = 0;
            switch (action) {
            case Action::Left: along = -dx; across = std::abs(dy); break;
            case Action::Right: along = dx; across = std::abs(dy); break;
            case Action::Up: along = -dy; across = std::abs(dx); break;
            case Action::Down: along = dy; across = std::abs(dx); break;
            default: return -1;
            }
            if (along <= 1.0f)
                continue;
            const float score = along + 2.0f * across;
            if (score < best_score) {
                best_score = score;
                best = i;
            }
        }
        return best;
    }

    void move_focus(Action action, int count)
    {
        if (count <= 0)
            return;
        const Layout l = layout(count);
        const int from = focus < 0 ? 0 : focus;
        const int best = nearest(l.min.data(), l.max.data(), count, from, action);
        if (best >= 0)
            set_focus(best, count);
    }

    void set_focus(int index, int count)
    {
        (void)count;
        focus = index;
    }

    int tile_at(ImVec2 p, int count) const
    {
        const Layout l = layout(count);
        for (int i = 0; i < count; ++i) {  // the cover and its caption
            const float cap = (l.portrait ? kCaptionPortrait : kCaption) * l.u;
            if (p.x >= l.min[i].x && p.x < l.max[i].x && p.y >= l.min[i].y && p.y < l.max[i].y + cap)
                return i;
        }
        return -1;
    }

    // Drawing ------------------------------------------------------------------------

    void text(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col, const std::string &s,
              float wrap = 0.0f)
    {
        dl->AddText(font, size, pos, col, s.c_str(), nullptr, wrap);
    }

    ImVec2 measure(ImFont *font, float size, const std::string &s, float wrap = 0.0f)
    {
        return font->CalcTextSizeA(size, FLT_MAX, wrap, s.c_str());
    }

    void draw_backdrop(ImDrawList *dl, float dt)
    {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        dl->AddRectFilled(ImVec2(0, 0), size, rgba(14, 13, 20));
        backdrop_mix = approach(backdrop_mix, 1.0f, dt, 5.0f);
        // Cover-fit the square glow to the window.
        const float side = std::max(size.x, size.y) * 1.15f;
        const ImVec2 min((size.x - side) / 2, (size.y - side) / 2 - side * 0.12f);
        const ImVec2 max(min.x + side, min.y + side);
        auto layer = [&](int index, float alpha) {
            if (index < 0 || !covers[static_cast<std::size_t>(index)].glow || alpha <= 0.01f)
                return;
            dl->AddImage(texture_ref(covers[static_cast<std::size_t>(index)].glow),
                         min, max, ImVec2(0, 0), ImVec2(1, 1), with_alpha(IM_COL32_WHITE, alpha));
        };
        layer(backdrop_prev, 0.55f * (1.0f - backdrop_mix));
        layer(backdrop, 0.55f * backdrop_mix);
        // Darken towards the bottom and the left, where the text sits.
        dl->AddRectFilledMultiColor(ImVec2(0, 0), size, rgba(14, 13, 20, 120), rgba(14, 13, 20, 90),
                                    rgba(14, 13, 20, 250), rgba(14, 13, 20, 250));
    }

    void draw_header(ImDrawList *dl, float u)
    {
        const ImVec2 o = safe_min(), e = safe_max();
        // Logo: white rounded square with a play triangle, then the name.
        const ImVec2 lmin(o.x + kMargin * u, o.y + 22.0f * u);
        const float ls = 28.0f * u;
        dl->AddRectFilled(lmin, ImVec2(lmin.x + ls, lmin.y + ls), IM_COL32_WHITE, 7.0f * u);
        const ImVec2 c(lmin.x + ls * 0.53f, lmin.y + ls * 0.5f);
        dl->AddTriangleFilled(ImVec2(c.x - 5.0f * u, c.y - 7.0f * u), ImVec2(c.x - 5.0f * u, c.y + 7.0f * u),
                              ImVec2(c.x + 7.0f * u, c.y), rgba(14, 13, 20));
        text(dl, font_bold, 22.0f * u, ImVec2(lmin.x + ls + 10.0f * u, lmin.y + 1.0f * u), IM_COL32_WHITE, "rsdk-air");

        const std::string version = std::string("v") + kVersion;
        const ImVec2 vs = measure(font_regular, 14.0f * u, version);
        const ImVec2 pill_max(e.x - kMargin * u, lmin.y + ls + 2.0f * u);
        const ImVec2 pill_min(pill_max.x - vs.x - 28.0f * u, lmin.y - 2.0f * u);
        dl->AddRectFilled(pill_min, pill_max, rgba(255, 255, 255, 18), 999.0f);
        dl->AddRect(pill_min, pill_max, rgba(255, 255, 255, 40), 999.0f, 1.0f * u);
        text(dl, font_regular, 14.0f * u,
             ImVec2(pill_min.x + 14.0f * u, (pill_min.y + pill_max.y - vs.y) / 2), rgba(220, 220, 230), version);

        // Credits, left of the version: a button (tap, or X / C).
        const char *label = "Credits";
        const ImVec2 cs = measure(font_bold, 14.0f * u, label);
        credits_pill_max = ImVec2(pill_min.x - 10.0f * u, pill_max.y);
        credits_pill_min = ImVec2(credits_pill_max.x - cs.x - 28.0f * u, pill_min.y);
        dl->AddRectFilled(credits_pill_min, credits_pill_max, rgba(255, 255, 255, 30), 999.0f);
        dl->AddRect(credits_pill_min, credits_pill_max, rgba(255, 255, 255, 60), 999.0f, 1.0f * u);
        text(dl, font_bold, 14.0f * u,
             ImVec2(credits_pill_min.x + 14.0f * u, (credits_pill_min.y + credits_pill_max.y - cs.y) / 2),
             IM_COL32_WHITE, label);
    }

    // The Credits screen: a scrolling panel over everything, Close at the bottom.
    void draw_credits(ImDrawList *dl, float u, float dt)
    {
        credits_anim = approach(credits_anim, credits_open ? 1.0f : 0.0f, dt, 16.0f);
        if (credits_anim < 0.01f)
            return;
        const float a = credits_anim;
        auto col = [&](ImU32 c) { return with_alpha(c, a); };
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        dl->AddRectFilled(ImVec2(0, 0), size, rgba(5, 5, 10, static_cast<int>(200 * a)));
        const ImVec2 o = safe_min(), e = safe_max();
        const float w = std::min(760.0f * u, e.x - o.x - 32.0f * u);
        const ImVec2 min((o.x + e.x - w) / 2, o.y + 24.0f * u + (1.0f - a) * 20.0f * u);
        const ImVec2 max(min.x + w, e.y - 24.0f * u);
        dl->AddRectFilled(min, max, col(rgba(24, 23, 33, 250)), 22.0f * u);
        dl->AddRect(min, max, col(rgba(255, 255, 255, 30)), 22.0f * u, 1.0f * u);
        const float pad = 28.0f * u;
        text(dl, font_bold, 28.0f * u, ImVec2(min.x + pad, min.y + pad - 4.0f * u), col(IM_COL32_WHITE), "Credits");

        // Scrolling body, clipped between the title and the Close button.
        const ImVec2 bmin(min.x + pad, min.y + pad + 44.0f * u), bmax(max.x - pad, max.y - pad - 58.0f * u);
        const float text_w = bmax.x - bmin.x;
        dl->PushClipRect(bmin, bmax, true);
        float y = bmin.y - credits_scroll;
        for (const CreditSection &section : credits()) {
            text(dl, font_bold, 12.0f * u, ImVec2(bmin.x, y), col(rgba(0x8e, 0x97, 0xa9)), section.title);
            y += 24.0f * u;
            for (const Credit &c : section.lines) {
                text(dl, font_bold, 15.5f * u, ImVec2(bmin.x, y), col(IM_COL32_WHITE), c.what, text_w);
                y += measure(font_bold, 15.5f * u, c.what, text_w).y + 3.0f * u;
                text(dl, font_regular, 14.0f * u, ImVec2(bmin.x, y), col(rgba(196, 196, 210)), c.who, text_w);
                y += measure(font_regular, 14.0f * u, c.who, text_w).y + 14.0f * u;
            }
            y += 10.0f * u;
        }
        dl->PopClipRect();
        const float content_h = y + credits_scroll - bmin.y;
        credits_scroll_max = std::max(0.0f, content_h - (bmax.y - bmin.y));
        credits_scroll = std::clamp(credits_scroll, 0.0f, credits_scroll_max);
        if (credits_scroll_max > 0) {  // a thin scroll bar
            const float track = bmax.y - bmin.y;
            const float thumb = std::max(30.0f * u, track * track / content_h);
            const float ty = bmin.y + (track - thumb) * credits_scroll / credits_scroll_max;
            dl->AddRectFilled(ImVec2(max.x - 14.0f * u, ty), ImVec2(max.x - 10.0f * u, ty + thumb),
                              col(rgba(255, 255, 255, 70)), 999.0f);
        }

        credits_close_max = ImVec2((min.x + max.x) / 2 + 90.0f * u, max.y - pad);
        credits_close_min = ImVec2((min.x + max.x) / 2 - 90.0f * u, credits_close_max.y - 46.0f * u);
        dl->AddRectFilled(credits_close_min, credits_close_max, col(IM_COL32_WHITE), 999.0f);
        const ImVec2 ls = measure(font_bold, 18.0f * u, "Close");
        text(dl, font_bold, 18.0f * u,
             ImVec2((credits_close_min.x + credits_close_max.x - ls.x) / 2,
                    (credits_close_min.y + credits_close_max.y - ls.y) / 2),
             col(rgba(14, 13, 20)), "Close");
    }

    void draw_lock(ImDrawList *dl, ImVec2 max, float u)
    {
        const ImVec2 bmax(max.x - 9.0f * u, max.y + 9.0f * u + 26.0f * u);
        const ImVec2 bmin(bmax.x - 26.0f * u, bmax.y - 26.0f * u);
        dl->AddRectFilled(bmin, bmax, rgba(10, 10, 15, 170), 8.0f * u);
        const ImVec2 c((bmin.x + bmax.x) / 2, (bmin.y + bmax.y) / 2);
        dl->AddRectFilled(ImVec2(c.x - 5.5f * u, c.y - 1.0f * u), ImVec2(c.x + 5.5f * u, c.y + 6.5f * u),
                          rgba(235, 235, 240), 1.5f * u);
        dl->PathArcTo(ImVec2(c.x, c.y - 1.5f * u), 3.6f * u, kPi, 2 * kPi, 12);
        dl->PathStroke(rgba(235, 235, 240), 1.8f * u);
    }

    void draw_tile(ImDrawList *dl, const Layout &l, const GameStatus &game, int index, float dt)
    {
        const float u = l.u;
        const bool focused = index == focus;
        float &lf = lift[static_cast<std::size_t>(index)];
        lf = approach(lf, focused ? 1.0f : 0.0f, dt);

        const float grow = 7.0f * u * lf;
        const ImVec2 min(l.min[index].x - grow, l.min[index].y - grow);
        const ImVec2 max(l.max[index].x + grow, l.max[index].y + grow);
        const float round = kRounding * u;

        // Soft shadow.
        for (int i = 1; i <= 6; ++i) {
            const float sp = static_cast<float>(i) * 2.5f * u;
            dl->AddRectFilled(ImVec2(min.x - sp, min.y - sp + 6.0f * u), ImVec2(max.x + sp, max.y + sp + 6.0f * u),
                              rgba(0, 0, 0, static_cast<int>(12 + 10 * lf)), round + sp);
        }

        const Cover &cover = covers[static_cast<std::size_t>(index)];
        if (cover.art) {
            int tw = 1, th = 1;
            SDL_QueryTexture(cover.art, nullptr, nullptr, &tw, &th);
            // Cover-fit: trim whichever sides the tile's aspect does not show.
            const float tile_aspect = (max.x - min.x) / (max.y - min.y);
            const float art_aspect = static_cast<float>(tw) / static_cast<float>(std::max(1, th));
            ImVec2 uv0(0, 0), uv1(1, 1);
            if (art_aspect > tile_aspect) {
                const float k = tile_aspect / art_aspect;
                uv0.x = (1 - k) * cover_anchor_x(game.spec->id), uv1.x = uv0.x + k;
            } else {
                const float k = art_aspect / tile_aspect;
                uv0.y = (1 - k) / 2, uv1.y = (1 + k) / 2;
            }
            dl->AddImageRounded(texture_ref(cover.art), min, max, uv0, uv1, IM_COL32_WHITE, round);
        } else {
            // Neutral fallback: a plain card with the title.
            dl->AddRectFilled(min, max, rgba(44, 46, 70), round);
            const std::string title(game.spec->title);
            text(dl, font_bold, 18.0f * u, ImVec2(min.x + 14.0f * u, min.y + 14.0f * u), rgba(240, 240, 250), title,
                 max.x - min.x - 28.0f * u);
        }
        if (!game.available) {
            dl->AddRectFilled(min, max, rgba(8, 8, 14, focused ? 80 : 120), round);
            draw_lock(dl, ImVec2(max.x, min.y), u);
        }
        if (focused) {
            const float ring = 4.0f * u;
            dl->AddRect(ImVec2(min.x - ring, min.y - ring), ImVec2(max.x + ring, max.y + ring),
                        with_alpha(IM_COL32_WHITE, 0.4f + 0.6f * lf), round + ring, 3.0f * u);
        }

        // Title and year under the cover (landscape; upright the details panel names it).
        if (l.portrait)
            return;
        const float ty = l.max[index].y + 12.0f * u + grow;
        const std::string title(game.spec->title);
        const float title_w = measure(font_bold, 15.0f * u, title).x;
        const float tile_w = l.max[index].x - l.min[index].x;
        const float title_size = title_w > tile_w ? 15.0f * u * tile_w / title_w : 15.0f * u;  // narrow tiles
        text(dl, focused ? font_bold : font_regular, title_size, ImVec2(l.min[index].x + 2.0f * u, ty),
             focused ? IM_COL32_WHITE : rgba(196, 196, 208), title);
        text(dl, font_regular, 12.5f * u, ImVec2(l.min[index].x + 2.0f * u, ty + 20.0f * u), rgba(140, 140, 155),
             std::to_string(game.spec->year));
    }

    std::string status_line(const GameStatus &game, std::string &chip, ImU32 &chip_col) const
    {
        if (game.available) {
            chip = "Ready";
            chip_col = rgba(80, 220, 140);
            return "Your game data was found. Press Play to start.";
        }
        if (installable(game)) {
            chip = "Not installed";
            chip_col = rgba(240, 190, 80);
            const S3airPackage &package = *s3air_package();
            return "Sonic 3 A.I.R. runs as its own program. rsdk-air can install the official version " +
                   std::string(package.version) + " for you from its GitHub releases (" +
                   std::to_string(package.size / 1000000) + " MB). Your ROM stays where it is.";
        }
        if (!get_app_url(game).empty()) {
            chip = "App not installed";
            chip_col = rgba(240, 190, 80);
            return "Sonic 3 A.I.R. runs in its own app. Install it from sonic3air.org, then come back here.";
        }
        if (!game.engine_found) {
            chip = "Coming soon";
            chip_col = rgba(240, 190, 80);
            return "This version of rsdk-air does not include the engine for this game yet.";
        }
        const bool missing = game.reason.rfind("missing ", 0) == 0;
        chip = missing ? "Data missing" : "Data not valid";
        chip_col = rgba(240, 190, 80);
        const std::string file(game.spec->required.front());
        if (importable(game)) {
            if (missing)
                return "Press Import and choose your own " + file + ". rsdk-air keeps a copy for the game.";
            return game.reason + "\nPress Import to choose another file.";
        }
        if (missing)
            return "Copy your own " + file + " to:\n" + path_to_utf8(game.data_dir);
        return game.reason;
    }

    // Sets play_min/max to the Play (or "Get the app") button; left at zero when inactive.
    void draw_details(ImDrawList *dl, const Layout &l, const GameStatus &game, ImVec2 &play_min, ImVec2 &play_max)
    {
        const float u = l.u;
        const ImVec2 min = l.panel_min;
        const float pad = 28.0f * u;
        const float width = l.panel_max.x - min.x;
        const float right_w = std::min(300.0f * u, width * 0.36f);
        const float text_w = width - right_w - 3 * pad;
        if (text_w < 120.0f * u)
            return;

        const std::string engine = engine_label(game.spec->module);
        const std::string title(game.spec->title);
        std::string chip;
        ImU32 chip_col = 0;
        const std::string line = status_line(game, chip, chip_col);
        const float title_size = std::min(40.0f * u, l.panel_max.y - min.y > 150.0f * u ? 40.0f * u : 30.0f * u);
        const float title_h = measure(font_bold, title_size, title, text_w).y;
        const float line_h = measure(font_regular, 16.0f * u, line, text_w).y;
        const std::string credit = engine_credit(game.spec->module);
        const float credit_h = credit.empty() ? 0.0f : 10.0f * u + measure(font_regular, 13.0f * u, credit, text_w).y;
        const float height = std::max(pad + 28.0f * u + title_h + 8.0f * u + line_h + credit_h + pad,
                                      2 * pad + 30.0f * u + 18.0f * u + 52.0f * u);
        const ImVec2 max(l.panel_max.x, std::min(min.y + height, l.panel_max.y));
        const ImU32 bg = rgba(24, 23, 33, 235);

        // Caret pointing up at the focused cover.
        const float cx = (l.min[focus].x + l.max[focus].x) / 2;
        dl->AddTriangleFilled(ImVec2(cx - 12.0f * u, min.y + 1), ImVec2(cx + 12.0f * u, min.y + 1),
                              ImVec2(cx, min.y - 12.0f * u), bg);
        dl->AddRectFilled(min, max, bg, 22.0f * u);
        dl->AddRect(min, max, rgba(255, 255, 255, 22), 22.0f * u, 1.0f * u);

        // Left: engine label, title, status line.
        float y = min.y + pad;
        dl->AddCircleFilled(ImVec2(min.x + pad + 4.0f * u, y + 7.0f * u), 4.0f * u, rgba(236, 72, 153));
        text(dl, font_bold, 12.5f * u, ImVec2(min.x + pad + 16.0f * u, y), rgba(190, 190, 205), engine);
        y += 28.0f * u;
        text(dl, font_bold, title_size, ImVec2(min.x + pad, y), IM_COL32_WHITE, title, text_w);
        y += title_h + 8.0f * u;
        text(dl, font_regular, 16.0f * u, ImVec2(min.x + pad, y), rgba(200, 200, 212), line, text_w);
        if (!credit.empty()) {
            y += line_h + 10.0f * u;
            text(dl, font_regular, 13.0f * u, ImVec2(min.x + pad, y), rgba(140, 140, 156), credit, text_w);
        }

        // Right: chips (year, status) and the Play button.
        const float rx = max.x - pad - right_w;
        float x = rx;
        y = min.y + pad;
        auto pill = [&](const std::string &label, ImU32 fg, ImU32 border) {
            const ImVec2 ts = measure(font_regular, 13.0f * u, label);
            const ImVec2 pmin(x, y), pmax(x + ts.x + 24.0f * u, y + 28.0f * u);
            dl->AddRectFilled(pmin, pmax, rgba(255, 255, 255, 14), 999.0f);
            dl->AddRect(pmin, pmax, border, 999.0f, 1.0f * u);
            text(dl, font_regular, 13.0f * u, ImVec2(pmin.x + 12.0f * u, pmin.y + (28.0f * u - ts.y) / 2), fg, label);
            x = pmax.x + 8.0f * u;
        };
        pill(std::to_string(game.spec->year), rgba(220, 220, 230), rgba(255, 255, 255, 40));
        pill(chip, chip_col, with_alpha(chip_col, 0.45f));

        const ImVec2 bmin(rx, y + 28.0f * u + 18.0f * u), bmax(rx + std::min(right_w, 190.0f * u), bmin.y + 52.0f * u);
        // A game in an app that is not installed gets a "Get the app" button instead.
        const bool get_app = !game.available && !get_app_url(game).empty();
        const bool install = !game.available && installable(game);
        const bool import_data = importable(game);
        const bool active = game.available || get_app || install || import_data;
        const std::string label = game.available ? "Play"
                                  : install      ? "Install"
                                  : import_data  ? "Import"
                                  : get_app      ? "Get the app"
                                                 : chip;
        dl->AddRectFilled(bmin, bmax, active ? IM_COL32_WHITE : rgba(255, 255, 255, 26), 999.0f);
        const ImVec2 ls = measure(font_bold, 19.0f * u, label);
        float lx = (bmin.x + bmax.x - ls.x) / 2;
        if (game.available) {
            lx += 10.0f * u;
            const float ty = (bmin.y + bmax.y) / 2;
            dl->AddTriangleFilled(ImVec2(lx - 22.0f * u, ty - 7.5f * u), ImVec2(lx - 22.0f * u, ty + 7.5f * u),
                                  ImVec2(lx - 9.0f * u, ty), rgba(14, 13, 20));
        }
        if (active) {
            play_min = bmin;
            play_max = bmax;
        }
        text(dl, font_bold, 19.0f * u, ImVec2(lx, (bmin.y + bmax.y - ls.y) / 2),
             active ? rgba(14, 13, 20) : rgba(160, 160, 172), label);
    }

    void draw_hint(ImDrawList *dl, float &x, float y, float u, const char *glyph, ImU32 glyph_col, const char *key,
                   const char *label)
    {
        // Drawn right to left: label, key cap, pad glyph. With dl == nullptr only moves x
        // (to measure a row of hints first).
        const ImVec2 ls = measure(font_regular, 14.0f * u, label);
        x -= ls.x;
        if (dl)
            text(dl, font_regular, 14.0f * u, ImVec2(x, y - ls.y / 2), rgba(225, 225, 235), label);
        x -= 10.0f * u;
        if (key) {
            const ImVec2 ks = measure(font_regular, 12.0f * u, key);
            const ImVec2 kmin(x - ks.x - 14.0f * u, y - 11.0f * u);
            if (dl) {
                dl->AddRectFilled(kmin, ImVec2(x, y + 11.0f * u), rgba(255, 255, 255, 20), 5.0f * u);
                dl->AddRect(kmin, ImVec2(x, y + 11.0f * u), rgba(255, 255, 255, 60), 5.0f * u, 1.0f * u);
                text(dl, font_regular, 12.0f * u, ImVec2(kmin.x + 7.0f * u, y - ks.y / 2), rgba(225, 225, 235), key);
            }
            x = kmin.x - 7.0f * u;
        }
        if (glyph) {
            const float r = 11.0f * u;
            if (dl) {
                dl->AddCircleFilled(ImVec2(x - r, y), r, glyph_col);
                const ImVec2 gs = measure(font_bold, 13.0f * u, glyph);
                text(dl, font_bold, 13.0f * u, ImVec2(x - r - gs.x / 2, y - gs.y / 2), rgba(14, 13, 20), glyph);
            }
            x -= 2 * r;
        }
        x -= 26.0f * u;
    }

    void draw_hints(ImDrawList *dl, float u, bool can_play)
    {
        const ImVec2 e = safe_max();
        float x = e.x - kMargin * u;
        const float y = e.y - 28.0f * u;
        draw_hint(dl, x, y, u, nullptr, 0, "Arrows", "Navigate");
        draw_hint(dl, x, y, u, "X", rgba(110, 160, 250), "C", "Credits");
        draw_hint(dl, x, y, u, "B", rgba(240, 110, 110), "Esc", "Quit");
        if (can_play)
            draw_hint(dl, x, y, u, "A", rgba(80, 220, 140), "Enter", "Play");
    }

    // Returns the Yes/No button rectangles for mouse hits.
    void draw_confirm(ImDrawList *dl, float u, float dt, ImVec2 rects[4])
    {
        draw_dialog(dl, u, dt, confirm_quit, confirm_anim, confirm_focus, "Quit rsdk-air?", rects);
    }

    // A Yes/No question over everything, No first. `anim` eases in and out with `open`;
    // `yes_focused` says which button has the ring; rects gets the No and Yes buttons.
    void draw_dialog(ImDrawList *dl, float u, float dt, bool open, float &anim, int yes_focused,
                     const std::string &question, ImVec2 rects[4])
    {
        anim = approach(anim, open ? 1.0f : 0.0f, dt, 18.0f);
        if (anim < 0.01f)
            return;
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        dl->AddRectFilled(ImVec2(0, 0), size, rgba(5, 5, 10, static_cast<int>(170 * anim)));
        const ImVec2 half(220.0f * u, 95.0f * u);
        const ImVec2 o = safe_min(), e = safe_max();
        const ImVec2 c((o.x + e.x) / 2, (o.y + e.y) / 2 + (1.0f - anim) * 20.0f * u);
        const ImVec2 min(c.x - half.x, c.y - half.y), max(c.x + half.x, c.y + half.y);
        dl->AddRectFilled(min, max, with_alpha(rgba(28, 27, 38), anim), 22.0f * u);
        dl->AddRect(min, max, with_alpha(rgba(255, 255, 255, 30), anim), 22.0f * u, 1.0f * u);
        const ImVec2 qs = measure(font_bold, 26.0f * u, question);
        text(dl, font_bold, 26.0f * u, ImVec2(c.x - qs.x / 2, min.y + 28.0f * u), with_alpha(IM_COL32_WHITE, anim),
             question);
        const char *labels[2] = {"No", "Yes"};
        for (int i = 0; i < 2; ++i) {
            const float bx = c.x + (i == 0 ? -1.0f : 1.0f) * 95.0f * u;
            const ImVec2 bmin(bx - 80.0f * u, max.y - 72.0f * u), bmax(bx + 80.0f * u, max.y - 26.0f * u);
            const bool f = yes_focused == i;
            dl->AddRectFilled(bmin, bmax, with_alpha(f ? IM_COL32_WHITE : rgba(255, 255, 255, 26), anim), 999.0f);
            const ImVec2 ls = measure(font_bold, 18.0f * u, labels[i]);
            text(dl, font_bold, 18.0f * u, ImVec2(bx - ls.x / 2, (bmin.y + bmax.y - ls.y) / 2),
                 with_alpha(f ? rgba(14, 13, 20) : rgba(225, 225, 235), anim), labels[i]);
            rects[i * 2] = bmin;
            rects[i * 2 + 1] = bmax;
        }
    }

    // Input --------------------------------------------------------------------------

    static Action key_action(SDL_Keycode key)
    {
        switch (key) {
        case SDLK_LEFT: return Action::Left;
        case SDLK_RIGHT: return Action::Right;
        case SDLK_UP: return Action::Up;
        case SDLK_DOWN: return Action::Down;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE: return Action::Accept;
        case SDLK_ESCAPE:
        case SDLK_AC_BACK: return Action::Back;
        case SDLK_DELETE:
        case SDLK_BACKSPACE: return Action::More;
        case SDLK_c:
        case SDLK_F1: return Action::Credits;
        default: return Action::None;
        }
    }

    static Action pad_action(Uint8 button)
    {
        switch (button) {
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return Action::Left;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return Action::Right;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return Action::Up;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return Action::Down;
        case SDL_CONTROLLER_BUTTON_A:
        case SDL_CONTROLLER_BUTTON_START: return Action::Accept;
        case SDL_CONTROLLER_BUTTON_B:
        case SDL_CONTROLLER_BUTTON_BACK: return Action::Back;
        case SDL_CONTROLLER_BUTTON_Y: return Action::More;
        case SDL_CONTROLLER_BUTTON_X: return Action::Credits;
        default: return Action::None;
        }
    }

    // Left stick as a d-pad, with key-like repeats.
    Action stick_action()
    {
        int dir = 0;
        for (SDL_GameController *pad : pads) {
            const int x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
            const int y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
            constexpr int dead = 20000;
            if (std::abs(x) > dead || std::abs(y) > dead) {
                dir = std::abs(x) > std::abs(y) ? (x < 0 ? 1 : 2) : (y < 0 ? 3 : 4);
                break;
            }
        }
        const Uint32 now = SDL_GetTicks();
        if (dir == 0) {
            stick_dir = 0;
            return Action::None;
        }
        if (dir == stick_dir && now < stick_next)
            return Action::None;
        stick_next = now + (dir == stick_dir ? 120 : 380);
        stick_dir = dir;
        static constexpr Action kDirs[] = {Action::None, Action::Left, Action::Right, Action::Up, Action::Down};
        return kDirs[dir];
    }
};

Picker::Picker() : impl_(std::make_unique<Impl>()) {}

Picker::~Picker() = default;

bool Picker::open(std::string &error)
{
    Impl &s = *impl_;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
#if defined(__ANDROID__)
    // Fullscreen hides the status and navigation bars (immersive mode). Without it they
    // stay on screen over the picker: with targetSdk 35 the app draws edge to edge.
    // Resizable keeps every orientation: without it SDL locks the screen to the one the
    // device had when the window was created (SDLActivity.setOrientationBis).
    const Uint32 window_flags = SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#else
    const Uint32 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#endif
    s.window = SDL_CreateWindow("rsdk-air", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                window_flags);
    if (!s.window) {
        error = std::string("cannot create the window: ") + SDL_GetError();
        return false;
    }
    SDL_SetWindowMinimumSize(s.window, 640, 400);
    s.renderer = SDL_CreateRenderer(s.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s.renderer)
        s.renderer = SDL_CreateRenderer(s.window, -1, 0);
    if (!s.renderer) {
        error = std::string("cannot create a renderer: ") + SDL_GetError();
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui_ImplSDL2_InitForSDLRenderer(s.window, s.renderer);
    ImGui_ImplSDLRenderer2_Init(s.renderer);
    s.imgui_ready = true;

    s.resources = resource_dir();
    s.font_regular = s.load_font("Inter-Medium.ttf");
    s.font_bold = s.load_font("Inter-Bold.ttf");
    if (!s.font_regular)
        s.font_regular = io.Fonts->AddFontDefault();
    if (!s.font_bold)
        s.font_bold = s.font_regular;
    s.load_covers();
    s.update_insets();

    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            if (SDL_GameController *pad = SDL_GameControllerOpen(i))
                s.pads.push_back(pad);
        }
    }
    return true;
}

void Picker::suspend()
{
#if !defined(__ANDROID__)
    SDL_HideWindow(impl_->window);
#endif
}

void Picker::resume()
{
#if !defined(__ANDROID__)
    SDL_ShowWindow(impl_->window);
    SDL_RaiseWindow(impl_->window);
#endif
    // Input that piled up while the game ran must not leak into the picker.
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_KEYDOWN, SDL_MOUSEWHEEL);
    SDL_FlushEvents(SDL_JOYAXISMOTION, SDL_JOYBUTTONUP);
    SDL_FlushEvents(SDL_CONTROLLERAXISMOTION, SDL_CONTROLLERBUTTONUP);
    SDL_FlushEvents(SDL_FINGERDOWN, SDL_FINGERMOTION);
    impl_->confirm_quit = false;
}

int Picker::choose(const std::vector<GameStatus> &games)
{
    Impl &s = *impl_;
    const int count = static_cast<int>(games.size());
    if (count == 0)
        return -1;
    s.lift.resize(games.size(), 0.0f);
    if (s.focus < 0 || s.focus >= count) {
        s.focus = 0;
        for (int i = 0; i < count; ++i) {
            if (games[static_cast<std::size_t>(i)].available) {
                s.focus = i;
                break;
            }
        }
        s.lift[static_cast<std::size_t>(s.focus)] = 1.0f;
        s.set_focus(s.focus, count);
    }

    ImVec2 play_min, play_max;
    ImVec2 confirm_rects[4];
    Uint64 last = SDL_GetPerformanceCounter();
    for (;;) {
        int chosen = -2;  // -2 keep going, -1 quit, >= 0 play
        auto act = [&](Action a) {
            if (a == Action::None)
                return;
            if (s.credits_open) {
                const float step = 60.0f * s.ui();
                if (a == Action::Up)
                    s.credits_scroll = std::max(0.0f, s.credits_scroll - step);
                else if (a == Action::Down)
                    s.credits_scroll = std::min(s.credits_scroll_max, s.credits_scroll + step);
                else if (a == Action::Back || a == Action::Accept || a == Action::Credits)
                    s.credits_open = false;
                return;
            }
            if (a == Action::Credits) {
                if (!s.confirm_quit) {
                    s.credits_open = true;
                    s.credits_scroll = 0.0f;
                }
                return;
            }
            if (s.confirm_quit) {
                if (a == Action::Left || a == Action::Right)
                    s.confirm_focus = 1 - s.confirm_focus;
                else if (a == Action::Back)
                    s.confirm_quit = false;
                else if (a == Action::Accept) {
                    if (s.confirm_focus == 1)
                        chosen = -1;
                    s.confirm_quit = false;
                }
                return;
            }
            if (a == Action::Back) {
                s.confirm_quit = true;
                s.confirm_focus = 0;  // No
            } else if (a == Action::More) {
                // Nothing to delete on the covers.
            } else if (a == Action::Accept) {
                const GameStatus &game = games[static_cast<std::size_t>(s.focus)];
                if (game.available)
                    chosen = s.focus;
                else if (installable(game))
                    chosen = kInstallBase - s.focus;
                else if (importable(game))
                    chosen = kImportBase - s.focus;
                else if (const std::string url = get_app_url(game); !url.empty())
                    SDL_OpenURL(url.c_str());
            } else {
                s.move_focus(a, count);
            }
        };
        auto hit = [](ImVec2 p, ImVec2 min, ImVec2 max) {
            return max.x > min.x && p.x >= min.x && p.x < max.x && p.y >= min.y && p.y < max.y;
        };

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            switch (event.type) {
            case SDL_QUIT: return -1;
            case SDL_WINDOWEVENT: s.update_insets(); break;  // rotation, bars shown or hidden
            case SDL_KEYDOWN: act(Impl::key_action(event.key.keysym.sym)); break;
            case SDL_CONTROLLERBUTTONDOWN: act(Impl::pad_action(event.cbutton.button)); break;
            case SDL_CONTROLLERDEVICEADDED:
                if (SDL_GameController *pad = SDL_GameControllerOpen(event.cdevice.which))
                    s.pads.push_back(pad);
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                for (auto it = s.pads.begin(); it != s.pads.end(); ++it) {
                    if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(*it)) == event.cdevice.which) {
                        SDL_GameControllerClose(*it);
                        s.pads.erase(it);
                        break;
                    }
                }
                break;
            case SDL_MOUSEMOTION:
                s.pointer_used = true;
                if (s.credits_open && (event.motion.state & SDL_BUTTON_LMASK)) {  // drag to scroll (touch too)
                    s.credits_scroll = std::clamp(s.credits_scroll - static_cast<float>(event.motion.yrel), 0.0f,
                                                  s.credits_scroll_max);
                    if (std::abs(event.motion.yrel) > 2)
                        s.credits_dragged = true;
                }
                break;
            case SDL_MOUSEWHEEL:
                if (s.credits_open) {
                    s.credits_scroll = std::clamp(s.credits_scroll - event.wheel.preciseY * 50.0f * s.ui(), 0.0f,
                                                  s.credits_scroll_max);
                }
                break;
            case SDL_MOUSEBUTTONUP: {
                if (event.button.button != SDL_BUTTON_LEFT)
                    break;
                const ImVec2 p(static_cast<float>(event.button.x), static_cast<float>(event.button.y));
                if (s.credits_open) {
                    // A drag scrolls; only a tap on Close closes.
                    if (!s.credits_dragged && hit(p, s.credits_close_min, s.credits_close_max))
                        s.credits_open = false;
                    s.credits_dragged = false;
                    break;
                }
                if (!s.confirm_quit && hit(p, s.credits_pill_min, s.credits_pill_max)) {
                    act(Action::Credits);
                    break;
                }
                if (s.confirm_quit) {
                    if (hit(p, confirm_rects[0], confirm_rects[1]))
                        s.confirm_quit = false;
                    else if (hit(p, confirm_rects[2], confirm_rects[3]))
                        chosen = -1;
                    break;
                }
                if (hit(p, play_min, play_max)) {
                    act(Action::Accept);
                    break;
                }
                const int tile = s.tile_at(p, count);
                if (tile >= 0) {
#if !defined(__ANDROID__)
                    // Desktop: a click on the focused cover (or a double click) plays it.
                    // Android: a tap only selects; the game starts from the Play button.
                    if (tile == s.focus || event.button.clicks >= 2)
                        act(Action::Accept);
#endif
                    s.set_focus(tile, count);
                }
                break;
            }
            default: break;
            }
        }
        act(s.stick_action());
        if (chosen != -2)
            return chosen;

        // Hover follows the mouse once it has moved (touch taps go through clicks above).
        if (s.pointer_used && !s.confirm_quit && !s.credits_open) {
            int mx = 0, my = 0;
            SDL_GetMouseState(&mx, &my);
            const int tile = s.tile_at(ImVec2(static_cast<float>(mx), static_cast<float>(my)), count);
            if (tile >= 0 && tile != s.focus)
                s.set_focus(tile, count);
            s.pointer_used = false;
        }

        if (s.focus != s.backdrop) {
            s.backdrop_prev = s.backdrop;
            s.backdrop = s.focus;
            s.backdrop_mix = s.backdrop_prev < 0 ? 1.0f : 0.0f;
        }

        const Uint64 now = SDL_GetPerformanceCounter();
        const float dt = std::min(0.1f, static_cast<float>(now - last) / static_cast<float>(SDL_GetPerformanceFrequency()));
        last = now;

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        ImDrawList *dl = ImGui::GetBackgroundDrawList();
        const Impl::Layout l = s.layout(count);
        const float u = l.u;
        s.draw_backdrop(dl, dt);
        s.draw_header(dl, u);
        play_min = play_max = ImVec2(0, 0);
        for (int i = 0; i < count; ++i) {
            if (i != s.focus)
                s.draw_tile(dl, l, games[static_cast<std::size_t>(i)], i, dt);
        }
        s.draw_tile(dl, l, games[static_cast<std::size_t>(s.focus)], s.focus, dt);  // on top
        s.draw_details(dl, l, games[static_cast<std::size_t>(s.focus)], play_min, play_max);
        const GameStatus &focused = games[static_cast<std::size_t>(s.focus)];
        s.draw_hints(dl, u, focused.available || installable(focused) || importable(focused) ||
                                   !get_app_url(focused).empty());
        s.draw_confirm(ImGui::GetForegroundDrawList(), u, dt, confirm_rects);
        s.draw_credits(ImGui::GetForegroundDrawList(), u, dt);
        ImGui::Render();

        SDL_SetRenderDrawColor(s.renderer, 14, 13, 20, 255);
        SDL_RenderClear(s.renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), s.renderer);
        SDL_RenderPresent(s.renderer);

        // Nothing moving: sleep until the next event instead of redrawing at full rate.
        bool settled = s.backdrop_mix > 0.995f && std::abs(s.confirm_anim - (s.confirm_quit ? 1.0f : 0.0f)) < 0.005f &&
                       std::abs(s.credits_anim - (s.credits_open ? 1.0f : 0.0f)) < 0.005f;
        for (std::size_t i = 0; i < s.lift.size(); ++i)
            settled = settled && std::abs(s.lift[i] - (static_cast<int>(i) == s.focus ? 1.0f : 0.0f)) < 0.003f;
        if (settled && s.pads.empty())
            SDL_WaitEventTimeout(nullptr, 500);
    }
}

} // namespace rsdkair

namespace rsdkair {

void Picker::busy(const std::string &title, const std::function<std::string()> &status,
                  const std::function<float()> &progress, const std::function<bool()> &done)
{
    Impl &s = *impl_;
    Uint64 last = SDL_GetPerformanceCounter();
    float sweep = 0.0f;  // position of the indeterminate bar
    while (!done()) {
        SDL_Event event;
        while (SDL_PollEvent(&event))
            ImGui_ImplSDL2_ProcessEvent(&event);  // keeps the window responsive; input is ignored

        const Uint64 now = SDL_GetPerformanceCounter();
        const float dt = std::min(0.1f, static_cast<float>(now - last) / static_cast<float>(SDL_GetPerformanceFrequency()));
        last = now;
        sweep = std::fmod(sweep + dt * 0.6f, 1.0f);

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        ImDrawList *dl = ImGui::GetBackgroundDrawList();
        const float u = s.ui();
        s.draw_backdrop(dl, dt);
        s.draw_header(dl, u);

        const ImVec2 half(270.0f * u, 92.0f * u);
        const ImVec2 o = s.safe_min(), e = s.safe_max();
        const ImVec2 c((o.x + e.x) / 2, (o.y + e.y) / 2);
        const ImVec2 min(c.x - half.x, c.y - half.y), max(c.x + half.x, c.y + half.y);
        dl->AddRectFilled(min, max, rgba(28, 27, 38, 245), 22.0f * u);
        dl->AddRect(min, max, rgba(255, 255, 255, 30), 22.0f * u, 1.0f * u);
        const float pad = 28.0f * u;
        s.text(dl, s.font_bold, 22.0f * u, ImVec2(min.x + pad, min.y + pad), IM_COL32_WHITE, title);
        s.text(dl, s.font_regular, 15.0f * u, ImVec2(min.x + pad, min.y + pad + 38.0f * u), rgba(200, 200, 212),
               status(), max.x - pad);

        const ImVec2 bmin(min.x + pad, max.y - pad - 10.0f * u), bmax(max.x - pad, max.y - pad);
        dl->AddRectFilled(bmin, bmax, rgba(255, 255, 255, 26), 999.0f);
        const float fraction = progress();
        const float w = bmax.x - bmin.x;
        if (fraction >= 0.0f) {
            dl->AddRectFilled(bmin, ImVec2(bmin.x + w * std::clamp(fraction, 0.02f, 1.0f), bmax.y), IM_COL32_WHITE,
                              999.0f);
        } else {
            const float x0 = bmin.x + (w * 1.3f) * sweep - w * 0.3f;
            dl->AddRectFilled(ImVec2(std::max(bmin.x, x0), bmin.y), ImVec2(std::min(bmax.x, x0 + w * 0.3f), bmax.y),
                              IM_COL32_WHITE, 999.0f);
        }
        ImGui::Render();
        SDL_SetRenderDrawColor(s.renderer, 14, 13, 20, 255);
        SDL_RenderClear(s.renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), s.renderer);
        SDL_RenderPresent(s.renderer);
    }
    SDL_FlushEvents(SDL_KEYDOWN, SDL_MOUSEWHEEL);  // presses made while busy do not count
    SDL_FlushEvents(SDL_CONTROLLERAXISMOTION, SDL_CONTROLLERBUTTONUP);
}

} // namespace rsdkair
