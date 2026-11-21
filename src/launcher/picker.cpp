#include "picker.hpp"

#include <rsdkair/paths.hpp>
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

enum class Action { None, Left, Right, Up, Down, Accept, Back };

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
    };

    static constexpr float kCaption = 52.0f;
    static constexpr float kCoverAspect = 1.25f;  // height / width: 4:5, a little taller than wide, like a box  // title + year under a cover, at u = 1

    // Scale of the whole layout: 1.0 at 1280x720.
    float ui() const
    {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        return std::clamp(std::min(size.x / 1280.0f, size.y / 720.0f), 0.5f, 2.5f);
    }

    Layout layout(int count) const
    {
        Layout l;
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        const float u = l.u = ui();
        const float gap = 40.0f * u;  // generous: the covers are busy pictures
        const float left = kMargin * u;
        float top = 96.0f * u;
        const float width = size.x - 2 * left;
        int cols = std::max(1, count);
        auto tile_for = [&](int c) { return (width - static_cast<float>(c - 1) * gap) / static_cast<float>(c); };
        while (cols > 1 && tile_for(cols) < 130.0f * u)
            --cols;
        // Leave room below for the panel (about 190 px) and the hints.
        const int rows = (count + cols - 1) / cols;
        const float room = size.y - top - 64.0f * u - 200.0f * u;
        const float by_height =
            (room - static_cast<float>(rows) * (kCaption * u + gap)) / static_cast<float>(rows) / kCoverAspect;
        l.tile = std::max(64.0f * u, std::min({tile_for(cols), by_height, 240.0f * u}));
        l.tile_h = l.tile * kCoverAspect;
        // Centre covers + panel (about 160 px) vertically in the space between header and hints.
        const float block = static_cast<float>(rows) * (l.tile_h + kCaption * u + gap) - gap + 22.0f * u + 160.0f * u;
        top = std::max(top, top + (size.y - 64.0f * u - top - block) / 2);
        for (int i = 0; i < count; ++i) {
            const float x = left + static_cast<float>(i % cols) * (l.tile + gap);
            const float y = top + static_cast<float>(i / cols) * (l.tile_h + kCaption * u + gap);
            l.min.emplace_back(x, y);
            l.max.emplace_back(x + l.tile, y + l.tile_h);
        }
        const float grid_bottom = top + static_cast<float>(rows) * (l.tile_h + kCaption * u + gap) - gap;
        l.panel_min = ImVec2(left, grid_bottom + 22.0f * u);
        l.panel_max = ImVec2(size.x - left, size.y - 62.0f * u);
        return l;
    }

    // Grid helpers -------------------------------------------------------------------

    void move_focus(Action action, int count)
    {
        if (count <= 0)
            return;
        const Layout l = layout(count);
        const int from = focus < 0 ? 0 : focus;
        const ImVec2 c((l.min[from].x + l.max[from].x) / 2, (l.min[from].y + l.max[from].y) / 2);
        int best = -1;
        float best_score = FLT_MAX;
        for (int i = 0; i < count; ++i) {
            if (i == from)
                continue;
            const ImVec2 o((l.min[i].x + l.max[i].x) / 2, (l.min[i].y + l.max[i].y) / 2);
            const float dx = o.x - c.x, dy = o.y - c.y;
            float along = 0, across = 0;
            switch (action) {
            case Action::Left: along = -dx; across = std::abs(dy); break;
            case Action::Right: along = dx; across = std::abs(dy); break;
            case Action::Up: along = -dy; across = std::abs(dx); break;
            case Action::Down: along = dy; across = std::abs(dx); break;
            default: return;
            }
            if (along <= 1.0f)
                continue;
            const float score = along + 2.0f * across;
            if (score < best_score) {
                best_score = score;
                best = i;
            }
        }
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
            if (p.x >= l.min[i].x && p.x < l.max[i].x && p.y >= l.min[i].y && p.y < l.max[i].y + kCaption * l.u)
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
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        // Logo: white rounded square with a play triangle, then the name.
        const ImVec2 lmin(kMargin * u, 22.0f * u);
        const float ls = 28.0f * u;
        dl->AddRectFilled(lmin, ImVec2(lmin.x + ls, lmin.y + ls), IM_COL32_WHITE, 7.0f * u);
        const ImVec2 c(lmin.x + ls * 0.53f, lmin.y + ls * 0.5f);
        dl->AddTriangleFilled(ImVec2(c.x - 5.0f * u, c.y - 7.0f * u), ImVec2(c.x - 5.0f * u, c.y + 7.0f * u),
                              ImVec2(c.x + 7.0f * u, c.y), rgba(14, 13, 20));
        text(dl, font_bold, 22.0f * u, ImVec2(lmin.x + ls + 10.0f * u, lmin.y + 1.0f * u), IM_COL32_WHITE, "rsdk-air");

        const std::string version = std::string("v") + kVersion;
        const ImVec2 vs = measure(font_regular, 14.0f * u, version);
        const ImVec2 pill_max(size.x - kMargin * u, lmin.y + ls + 2.0f * u);
        const ImVec2 pill_min(pill_max.x - vs.x - 28.0f * u, lmin.y - 2.0f * u);
        dl->AddRectFilled(pill_min, pill_max, rgba(255, 255, 255, 18), 999.0f);
        dl->AddRect(pill_min, pill_max, rgba(255, 255, 255, 40), 999.0f, 1.0f * u);
        text(dl, font_regular, 14.0f * u,
             ImVec2(pill_min.x + 14.0f * u, (pill_min.y + pill_max.y - vs.y) / 2), rgba(220, 220, 230), version);
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

        // Title and year under the cover.
        const float ty = l.max[index].y + 12.0f * u + grow;
        const std::string title(game.spec->title);
        const float title_w = measure(font_bold, 15.0f * u, title).x;
        const float title_size = title_w > l.tile ? 15.0f * u * l.tile / title_w : 15.0f * u;  // narrow tiles
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
        if (!game.engine_found) {
            chip = "Coming soon";
            chip_col = rgba(240, 190, 80);
            return "This version of rsdk-air does not include the engine for this game yet.";
        }
        chip = "Data missing";
        chip_col = rgba(240, 190, 80);
        const std::string file(game.spec->required.front());
        if (game.reason.rfind("missing ", 0) == 0)
            return "Copy your own " + file + " to:\n" + path_to_utf8(game.data_dir);
        return game.reason;
    }

    // Sets play_min/max to the Play button (left at zero when the game cannot be played).
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
        const float height = std::max(pad + 28.0f * u + title_h + 8.0f * u + line_h + pad, 2 * pad + 30.0f * u + 18.0f * u + 52.0f * u);
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
        text(dl, font_regular, 16.0f * u, ImVec2(min.x + pad, y), rgba(200, 200, 212), line, min.x + pad + text_w);

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
        const std::string label = game.available ? "Play" : chip;
        dl->AddRectFilled(bmin, bmax, game.available ? IM_COL32_WHITE : rgba(255, 255, 255, 26), 999.0f);
        const ImVec2 ls = measure(font_bold, 19.0f * u, label);
        float lx = (bmin.x + bmax.x - ls.x) / 2;
        if (game.available) {
            lx += 10.0f * u;
            const float ty = (bmin.y + bmax.y) / 2;
            dl->AddTriangleFilled(ImVec2(lx - 22.0f * u, ty - 7.5f * u), ImVec2(lx - 22.0f * u, ty + 7.5f * u),
                                  ImVec2(lx - 9.0f * u, ty), rgba(14, 13, 20));
            play_min = bmin;
            play_max = bmax;
        }
        text(dl, font_bold, 19.0f * u, ImVec2(lx, (bmin.y + bmax.y - ls.y) / 2),
             game.available ? rgba(14, 13, 20) : rgba(160, 160, 172), label);
    }

    void draw_hint(ImDrawList *dl, float &x, float y, float u, const char *glyph, ImU32 glyph_col, const char *key,
                   const char *label)
    {
        // Drawn right to left: label, key cap, pad glyph.
        const ImVec2 ls = measure(font_regular, 14.0f * u, label);
        x -= ls.x;
        text(dl, font_regular, 14.0f * u, ImVec2(x, y - ls.y / 2), rgba(225, 225, 235), label);
        x -= 10.0f * u;
        if (key) {
            const ImVec2 ks = measure(font_regular, 12.0f * u, key);
            const ImVec2 kmin(x - ks.x - 14.0f * u, y - 11.0f * u);
            dl->AddRectFilled(kmin, ImVec2(x, y + 11.0f * u), rgba(255, 255, 255, 20), 5.0f * u);
            dl->AddRect(kmin, ImVec2(x, y + 11.0f * u), rgba(255, 255, 255, 60), 5.0f * u, 1.0f * u);
            text(dl, font_regular, 12.0f * u, ImVec2(kmin.x + 7.0f * u, y - ks.y / 2), rgba(225, 225, 235), key);
            x = kmin.x - 7.0f * u;
        }
        if (glyph) {
            const float r = 11.0f * u;
            dl->AddCircleFilled(ImVec2(x - r, y), r, glyph_col);
            const ImVec2 gs = measure(font_bold, 13.0f * u, glyph);
            text(dl, font_bold, 13.0f * u, ImVec2(x - r - gs.x / 2, y - gs.y / 2), rgba(14, 13, 20), glyph);
            x -= 2 * r;
        }
        x -= 26.0f * u;
    }

    void draw_hints(ImDrawList *dl, float u, bool can_play)
    {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        float x = size.x - kMargin * u;
        const float y = size.y - 28.0f * u;
        draw_hint(dl, x, y, u, nullptr, 0, "Arrows", "Navigate");
        draw_hint(dl, x, y, u, "B", rgba(240, 110, 110), "Esc", "Quit");
        if (can_play)
            draw_hint(dl, x, y, u, "A", rgba(80, 220, 140), "Enter", "Play");
    }

    // Returns the Yes/No button rectangles for mouse hits.
    void draw_confirm(ImDrawList *dl, float u, float dt, ImVec2 rects[4])
    {
        confirm_anim = approach(confirm_anim, confirm_quit ? 1.0f : 0.0f, dt, 18.0f);
        if (confirm_anim < 0.01f)
            return;
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        dl->AddRectFilled(ImVec2(0, 0), size, rgba(5, 5, 10, static_cast<int>(170 * confirm_anim)));
        const ImVec2 half(220.0f * u, 95.0f * u);
        const ImVec2 c(size.x / 2, size.y / 2 + (1.0f - confirm_anim) * 20.0f * u);
        const ImVec2 min(c.x - half.x, c.y - half.y), max(c.x + half.x, c.y + half.y);
        dl->AddRectFilled(min, max, with_alpha(rgba(28, 27, 38), confirm_anim), 22.0f * u);
        dl->AddRect(min, max, with_alpha(rgba(255, 255, 255, 30), confirm_anim), 22.0f * u, 1.0f * u);
        const std::string q = "Quit rsdk-air?";
        const ImVec2 qs = measure(font_bold, 26.0f * u, q);
        text(dl, font_bold, 26.0f * u, ImVec2(c.x - qs.x / 2, min.y + 28.0f * u),
             with_alpha(IM_COL32_WHITE, confirm_anim), q);
        const char *labels[2] = {"No", "Yes"};
        for (int i = 0; i < 2; ++i) {
            const float bx = c.x + (i == 0 ? -1.0f : 1.0f) * 95.0f * u;
            const ImVec2 bmin(bx - 80.0f * u, max.y - 72.0f * u), bmax(bx + 80.0f * u, max.y - 26.0f * u);
            const bool f = confirm_focus == i;
            dl->AddRectFilled(bmin, bmax, with_alpha(f ? IM_COL32_WHITE : rgba(255, 255, 255, 26), confirm_anim),
                              999.0f);
            const ImVec2 ls = measure(font_bold, 18.0f * u, labels[i]);
            text(dl, font_bold, 18.0f * u, ImVec2(bx - ls.x / 2, (bmin.y + bmax.y - ls.y) / 2),
                 with_alpha(f ? rgba(14, 13, 20) : rgba(225, 225, 235), confirm_anim), labels[i]);
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
    s.window = SDL_CreateWindow("rsdk-air", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
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
            } else if (a == Action::Accept) {
                if (games[static_cast<std::size_t>(s.focus)].available)
                    chosen = s.focus;
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
                break;
            case SDL_MOUSEBUTTONUP: {
                if (event.button.button != SDL_BUTTON_LEFT)
                    break;
                const ImVec2 p(static_cast<float>(event.button.x), static_cast<float>(event.button.y));
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
                    if (tile == s.focus || event.button.clicks >= 2)
                        act(Action::Accept);
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
        if (s.pointer_used && !s.confirm_quit) {
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
        s.draw_hints(dl, u, games[static_cast<std::size_t>(s.focus)].available);
        s.draw_confirm(ImGui::GetForegroundDrawList(), u, dt, confirm_rects);
        ImGui::Render();

        SDL_SetRenderDrawColor(s.renderer, 14, 13, 20, 255);
        SDL_RenderClear(s.renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), s.renderer);
        SDL_RenderPresent(s.renderer);

        // Nothing moving: sleep until the next event instead of redrawing at full rate.
        bool settled = s.backdrop_mix > 0.995f && std::abs(s.confirm_anim - (s.confirm_quit ? 1.0f : 0.0f)) < 0.005f;
        for (std::size_t i = 0; i < s.lift.size(); ++i)
            settled = settled && std::abs(s.lift[i] - (static_cast<int>(i) == s.focus ? 1.0f : 0.0f)) < 0.003f;
        if (settled && s.pads.empty())
            SDL_WaitEventTimeout(nullptr, 500);
    }
}

} // namespace rsdkair
