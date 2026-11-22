#include "picker.hpp"

#include <rsdkair/paths.hpp>
#include <rsdkair/s3air.hpp>
#include <rsdkair/saves_rsdkv4.hpp>
#include <rsdkair/version.hpp>

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
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

enum class Action { None, Left, Right, Up, Down, Accept, Back, More };

// Games whose save slots the picker shows before starting (Sonic 1 and 2): the module
// takes --slot / --player (engines/rsdkv4/rsdkv4_module.cpp).
bool has_slots(const GameStatus &game) { return game.available && game.spec->module == "rsdkv4"; }

// The slot screen's look: rsdkv4-wasm's app (src/app/app.css, from craft's GameLauncher),
// redrawn with Dear ImGui. Art: art/players/<name>.png and art/stages/<game>/*.png.
constexpr const char *kPlayerArt[] = {"sonic", "tails", "knuckles", "sonictails"};
// Chaos Emerald colours, in order.
constexpr ImU32 kGems[] = {IM_COL32(0x58, 0xe0, 0x58, 255), IM_COL32(0xf0, 0xa0, 0x38, 255),
                           IM_COL32(0xf0, 0x78, 0xd8, 255), IM_COL32(0x98, 0x78, 0xf0, 255),
                           IM_COL32(0xf0, 0xf0, 0xf0, 255), IM_COL32(0xf0, 0x78, 0x78, 255),
                           IM_COL32(0x78, 0xc0, 0xf8, 255)};
// The coloured band behind each character on a new-game card: left and right insets (in
// fractions of the width) and colour, as .player-choose[data-player=…]::after.
struct PlayerBand {
    float left, right;
    int r, g, b;
};
constexpr PlayerBand kBands[] = {
    {0.30f, 0.20f, 13, 122, 233}, {0.40f, 0.10f, 241, 176, 0}, {0.20f, 0.30f, 255, 20, 0}, {0.20f, 0.70f, 160, 32, 240}};
// @keyframes rsdk-player-bg: the bands drift between these insets on a focused card.
constexpr float kBandFrames[][2] = {{0.3f, 0.2f}, {0.5f, 0.5f}, {0.1f, 0.2f}, {0.6f, 0.4f}, {0.7f, 0.1f}, {0.2f, 0.8f},
                                    {0.0f, 0.6f}, {0.4f, 0.1f}, {0.1f, 0.7f}, {0.8f, 0.0f}, {0.3f, 0.2f}};

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

    // Save-slot screen (Sonic 1/2). Cards: 0 "NO SAVE", 1-4 the save slots, 5 "GAME
    // MENUS" (the game's own title screen). Opened by Play on a cover; Back returns.
    static constexpr int kSlotCards = 6;
    static constexpr int kMenusCard = 5;
    bool slots_open = false;
    int slots_game = -1;              // index into the games list
    std::string slots_game_id;
    std::filesystem::path slots_dir;  // the game's save folder
    std::array<rsdkv4::Slot, rsdkv4::kSlotCount> slots{};
    int slot_focus = 1;
    std::array<int, kSlotCards> slot_char{};  // character for a new game, per card
    float slots_anim = 0.0f;
    float slots_time = 0.0f;   // drives the drifting bands on a focused new-game card
    bool delete_open = false;  // "Delete game n?" dialog
    int delete_focus = 0;      // 0 No, 1 Yes
    float delete_anim = 0.0f;
    std::string delete_error;
    std::vector<std::string> chosen_args;  // engine arguments for the last choice

    struct SlotHits {
        ImVec2 card_min[kSlotCards], card_max[kSlotCards];
        ImVec2 button_min[kSlotCards], button_max[kSlotCards];
        ImVec2 pick_min[kSlotCards], pick_max[kSlotCards];   // character square (new game)
        ImVec2 trash_min[kSlotCards], trash_max[kSlotCards];
    };
    SlotHits slot_hits{};

    // Images under art/, loaded on first use (nullptr cached for missing files).
    std::map<std::string, SDL_Texture *> art_cache;

    SDL_Texture *art(const std::string &relative, bool pixelated)
    {
        auto it = art_cache.find(relative);
        if (it != art_cache.end())
            return it->second;
        SDL_Texture *tex = load_texture(resources + "art/" + relative, nullptr);
        if (tex && pixelated)
            SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
        art_cache[relative] = tex;
        return tex;
    }

    void open_slots(int index, const GameStatus &game)
    {
        slots_open = true;
        slots_game = index;
        slots_game_id = std::string(game.spec->id);
        slots_dir = game.save_dir;
        delete_open = false;
        slots_anim = 0.0f;
        reload_slots();
        // The first slot with a game in it is where most people want to be (as in
        // rsdkv4-wasm), else the first empty one.
        slot_focus = 1;
        for (int i = 0; i < rsdkv4::kSlotCount; ++i) {
            if (!slots[static_cast<std::size_t>(i)].empty) {
                slot_focus = i + 1;
                break;
            }
        }
    }

    void reload_slots()
    {
        for (int i = 0; i < rsdkv4::kSlotCount; ++i) {
            slots[static_cast<std::size_t>(i)] = rsdkv4::Slot{};
            slots[static_cast<std::size_t>(i)].slot = i;
        }
        if (auto read = rsdkv4::read_slots(slots_dir))
            slots = *read;
    }

    const rsdkv4::Slot *saved_slot(int card) const
    {
        if (card < 1 || card > rsdkv4::kSlotCount)
            return nullptr;
        const rsdkv4::Slot &slot = slots[static_cast<std::size_t>(card - 1)];
        return slot.empty ? nullptr : &slot;
    }

    // A slot without a game: the character picker over PLAY.
    bool new_game_card(int card) const { return card >= 1 && card <= 4 && !saved_slot(card); }

    // The engine arguments for starting from a card (engines/rsdkv4/rsdkv4_module.cpp).
    std::vector<std::string> card_args(int card) const
    {
        if (card == kMenusCard)
            return {};  // the game's own title screen and menus
        const std::string slot = std::to_string(card - 1);  // card 0 -> -1, no save
        if (saved_slot(card))
            return {"--slot", slot};
        return {"--slot", slot, "--player", std::to_string(slot_char[static_cast<std::size_t>(card)])};
    }

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
        for (auto &[path, tex] : art_cache) {
            if (tex)
                SDL_DestroyTexture(tex);
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

    // Scale of the whole layout: 1.0 at 1280x720, or at 720x1280 when upright, so a phone
    // shows text and buttons at the same size in both orientations.
    float ui() const
    {
        const ImVec2 size = safe_size();
        const float scale = size.y > size.x ? std::min(size.x / 720.0f, size.y / 1280.0f)
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
            return layout_portrait(l, count, origin, size, top, gap, margin, tile_for(2));
        int cols = std::max(1, count);
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
        top = origin.y + std::max(top, top + (size.y - 64.0f * u - top - block) / 2);
        for (int i = 0; i < count; ++i) {
            const float x = left + static_cast<float>(i % cols) * (l.tile + gap);
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
        const float cap = kCaption * u;
        const float width = size.x - 2 * margin;
        const int full_rows = count / 2;
        const bool wide_last = count % 2 == 1;
        constexpr float kWide = 9.0f / 16.0f;  // height / width of the wide cover
        // Height used: full_rows * (tile + cap + gap) + [ (2 tile + gap) * kWide + cap ].
        const float room = size.y - top - 64.0f * u - 200.0f * u;
        const float per_tile = static_cast<float>(full_rows) + (wide_last ? 2 * kWide : 0.0f);
        const float fixed = static_cast<float>(full_rows) * (cap + gap) + (wide_last ? gap * kWide + cap : -gap);
        const float by_height = per_tile > 0 ? (room - fixed) / per_tile : max_tile;
        l.tile = std::max(64.0f * u, std::min(max_tile, by_height));
        l.tile_h = l.tile;
        const float grid_w = count > 1 ? 2 * l.tile + gap : l.tile;
        const float wide_h = grid_w * kWide;
        const float block = static_cast<float>(full_rows) * (l.tile + cap + gap) + (wide_last ? wide_h + cap : -gap) +
                            22.0f * u + 160.0f * u;
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

    // Slot cards, as rsdkv4-wasm: 170 x 220 at scale 1 (NO SAVE and GAME MENUS 170 x 130),
    // top-aligned in a row: six in one row in landscape, three per row in portrait or
    // when narrow. The heading sits above the row, left-aligned with it; hints below.
    struct SlotLayout {
        float u = 1.0f;
        float k = 1.0f;  // card scale: card width / 170
        ImVec2 min[kSlotCards], max[kSlotCards];
        ImVec2 heading;  // top-left of the kicker + "Load game"
        int order[kSlotCards] = {0, 1, 2, 3, 4, 5};  // cards in reading order (Left/Right)
        float hints_y = 0.0f;
        float centre_x = 0.0f;
    };

    SlotLayout slot_layout() const
    {
        SlotLayout l;
        const ImVec2 origin = safe_min();
        const ImVec2 size = safe_size();
        const float u = l.u = ui();
        const float margin = kMargin * u;
        const float width = size.x - 2 * margin;
        const float gap = 22.0f * u;
        const bool portrait = size.y > size.x;
        const float heading_h = 62.0f * u;
        auto width_for = [&](int c, float cap) {
            return std::min(cap, (width - static_cast<float>(c - 1) * gap) / static_cast<float>(c));
        };
        // Portrait: NO SAVE and GAME MENUS share the first row, the slots follow.
        if (portrait) {
            const int order[kSlotCards] = {0, kMenusCard, 1, 2, 3, 4};
            std::copy(std::begin(order), std::end(order), l.order);
        }
        auto card_h = [&](int card, float w) {
            return w * (card == 0 || card == kMenusCard ? 130.0f : 220.0f) / 170.0f;
        };
        // Each row is as tall as its tallest card.
        auto grid_height = [&](int c, float w) {
            float h = 0.0f;
            for (int start = 0; start < kSlotCards; start += c) {
                float row = 0.0f;
                for (int i = start; i < std::min(kSlotCards, start + c); ++i)
                    row = std::max(row, card_h(l.order[i], w));
                h += row + (start > 0 ? gap : 0.0f);
            }
            return h;
        };
        auto height_for = [&](int c, float w) {  // heading + cards + hints
            return heading_h + 30.0f * u + grid_height(c, w) + 56.0f * u;
        };
        const float room_h = size.y - 86.0f * u;
        int cols = kSlotCards;
        float w = width_for(cols, 190.0f * u);
        if (portrait) {
            // Two big cards per row for a finger, if three rows fit; else three per row.
            cols = 2;
            w = width_for(2, 260.0f * u);
            if (height_for(2, w) > room_h) {
                cols = 3;
                w = width_for(3, 220.0f * u);
            }
        } else if (w < 140.0f * u) {
            cols = 3;
            w = width_for(3, 220.0f * u);
        }
        l.k = w / 170.0f;
        const float grid_w = static_cast<float>(cols) * w + static_cast<float>(cols - 1) * gap;
        const float grid_h = grid_height(cols, w);
        const float block = heading_h + 30.0f * u + grid_h + 34.0f * u + 22.0f * u;
        const float top = origin.y + 70.0f * u;  // below the app's header
        const float room = origin.y + size.y - top - 16.0f * u;
        const float y0 = top + std::max(0.0f, (room - block) / 2);
        const float x0 = origin.x + margin + (width - grid_w) / 2;
        l.heading = ImVec2(x0, y0);
        const float cards_y = y0 + heading_h + 30.0f * u;
        float y = cards_y;
        for (int start = 0; start < kSlotCards; start += cols) {
            float row = 0.0f;
            for (int i = start; i < std::min(kSlotCards, start + cols); ++i) {
                const int card = l.order[i];
                const float x = x0 + static_cast<float>(i - start) * (w + gap);
                l.min[card] = ImVec2(x, y);
                l.max[card] = ImVec2(x + w, y + card_h(card, w));
                row = std::max(row, card_h(card, w));
            }
            y += row + gap;
        }
        l.hints_y = cards_y + grid_h + 34.0f * u + 11.0f * u;
        l.centre_x = origin.x + size.x / 2;
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
            if (has_slots(game))
                return "Your game data was found. Press Play to choose a save.";
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
        text(dl, font_regular, 16.0f * u, ImVec2(min.x + pad, y), rgba(200, 200, 212), line, text_w);

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

    // Save-slot screen -----------------------------------------------------------------

    // Draws `tex` into [min, max] keeping its aspect: fitted inside (contain) or filling
    // it with the overflow cropped (cover).
    void draw_image(ImDrawList *dl, SDL_Texture *tex, ImVec2 min, ImVec2 max, bool cover, ImU32 tint,
                    float rounding = 0.0f)
    {
        int tw = 1, th = 1;
        SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);
        const float box_w = max.x - min.x, box_h = max.y - min.y;
        const float ta = static_cast<float>(tw) / static_cast<float>(std::max(1, th));
        const float ba = box_w / std::max(1.0f, box_h);
        ImVec2 uv0(0, 0), uv1(1, 1);
        ImVec2 a = min, b = max;
        if (cover) {
            if (ta > ba) {
                const float k = ba / ta;
                uv0.x = (1 - k) / 2, uv1.x = (1 + k) / 2;
            } else {
                const float k = ta / ba;
                uv0.y = (1 - k) / 2, uv1.y = (1 + k) / 2;
            }
        } else if (ta > ba) {
            const float h = box_w / ta;
            a.y = min.y + (box_h - h) / 2, b.y = a.y + h;
        } else {
            const float w = box_h * ta;
            a.x = min.x + (box_w - w) / 2, b.x = a.x + w;
        }
        if (rounding > 0)
            dl->AddImageRounded(texture_ref(tex), a, b, uv0, uv1, tint, rounding);
        else
            dl->AddImage(texture_ref(tex), a, b, uv0, uv1, tint);
    }

    // The blue PLAY / CONTINUE button at the bottom of a card.
    void draw_slot_button(ImDrawList *dl, int card, ImVec2 min, ImVec2 max, const char *label, float k, bool focused)
    {
        dl->AddRectFilled(min, max, focused ? rgba(0, 0, 255) : rgba(65, 105, 225), 4.0f * k);
        const ImVec2 ls = measure(font_bold, 16.0f * k, label);
        const ImVec2 at((min.x + max.x - ls.x) / 2, (min.y + max.y - ls.y) / 2);
        text(dl, font_bold, 16.0f * k, ImVec2(at.x, at.y + 1.0f * k), rgba(0, 0, 0, 160), label);
        text(dl, font_bold, 16.0f * k, at, IM_COL32_WHITE, label);
        slot_hits.button_min[card] = min;
        slot_hits.button_max[card] = max;
    }

    void draw_slot_card(ImDrawList *dl, const SlotLayout &l, int card)
    {
        const float u = l.u, k = l.k;
        const bool focused = card == slot_focus;
        const float lift = focused ? 4.0f * k : 0.0f;  // translateY(-4px)
        const ImVec2 min(l.min[card].x, l.min[card].y - lift), max(l.max[card].x, l.max[card].y - lift);
        const float round = 8.0f * k;
        const float pad = 12.0f * k;

        slot_hits.card_min[card] = min;
        slot_hits.card_max[card] = max;
        slot_hits.button_min[card] = slot_hits.button_max[card] = ImVec2(0, 0);
        slot_hits.pick_min[card] = slot_hits.pick_max[card] = ImVec2(0, 0);
        slot_hits.trash_min[card] = slot_hits.trash_max[card] = ImVec2(0, 0);

        if (focused) {
            for (int i = 1; i <= 5; ++i) {  // 0 24px 50px -20px #000
                const float sp = static_cast<float>(i) * 4.0f * k;
                dl->AddRectFilled(ImVec2(min.x - sp + 6.0f * k, min.y - sp + 24.0f * k),
                                  ImVec2(max.x + sp - 6.0f * k, max.y + sp + 4.0f * k), rgba(0, 0, 0, 22), round + sp);
            }
        }
        // 2px black border, #333 card.
        dl->AddRectFilled(min, max, rgba(0, 0, 0), round);
        dl->AddRectFilled(ImVec2(min.x + 2.0f * k, min.y + 2.0f * k), ImVec2(max.x - 2.0f * k, max.y - 2.0f * k),
                          rgba(0x33, 0x33, 0x33), round - 2.0f * k);
        if (focused) {
            const float ring = 3.0f * u;
            dl->AddRect(ImVec2(min.x - ring / 2, min.y - ring / 2), ImVec2(max.x + ring / 2, max.y + ring / 2),
                        IM_COL32_WHITE, round + ring / 2, ring);
        }

        const ImVec2 in_min(min.x + pad, min.y + pad), in_max(max.x - pad, max.y - pad);
        const float in_w = in_max.x - in_min.x;

        // NO SAVE and GAME MENUS: a short card with its name.
        if (card == 0 || card == kMenusCard) {
            const char *name = card == 0 ? "NO SAVE" : "GAME MENUS";
            const ImVec2 ns = measure(font_bold, 16.0f * k, name);
            text(dl, font_bold, 16.0f * k, ImVec2((min.x + max.x - ns.x) / 2, (min.y + max.y - ns.y) / 2),
                 IM_COL32_WHITE, name);
            return;
        }

        const ImVec2 bmin(in_min.x, in_max.y - 34.0f * k), bmax(in_max.x, in_max.y);
        const rsdkv4::Slot *saved = saved_slot(card);

        if (!saved) {
            // Character picker: a white square, two colour bands, the character on top.
            const int ch = std::clamp(slot_char[static_cast<std::size_t>(card)], 0, 3);
            const ImVec2 pmin = in_min, pmax(in_max.x, in_min.y + in_w);
            dl->AddRectFilled(pmin, pmax, IM_COL32_WHITE);
            auto band = [&](float left, float right, ImU32 col) {
                dl->AddRectFilled(ImVec2(pmin.x + in_w * left, pmin.y), ImVec2(pmax.x - in_w * right, pmax.y), col);
            };
            auto drift = [&](float period, bool reverse, float &left, float &right) {
                float t = std::fmod(slots_time / period, 1.0f);
                if (reverse)
                    t = 1.0f - t;
                const float f = t * 10.0f;
                const int i = std::min(9, static_cast<int>(f));
                const float m = f - static_cast<float>(i);
                left = kBandFrames[i][0] + (kBandFrames[i + 1][0] - kBandFrames[i][0]) * m;
                right = kBandFrames[i][1] + (kBandFrames[i + 1][1] - kBandFrames[i][1]) * m;
            };
            float gl = 3.0f * 16.0f * k / in_w, gr = 16.0f * k / in_w;  // inset: 0 1rem 0 3rem
            const PlayerBand &pb = kBands[ch];
            float cl = pb.left, cr = pb.right;
            if (focused) {
                drift(20.0f, false, gl, gr);
                drift(30.0f, true, cl, cr);
            }
            band(gl, gr, rgba(180, 180, 180, 51));
            band(cl, cr, rgba(pb.r, pb.g, pb.b, 51));
            const float top_pad = focused ? 0.0f : 16.0f * k;  // padding 1rem .5rem 0, none when focused
            const float side_pad = focused ? 0.0f : 8.0f * k;
            if (SDL_Texture *tex = art(std::string("players/") + kPlayerArt[ch] + ".png", false)) {
                draw_image(dl, tex, ImVec2(pmin.x + side_pad, pmin.y + top_pad), ImVec2(pmax.x - side_pad, pmax.y),
                           false, IM_COL32_WHITE);
            } else {
                const std::string name(rsdkv4::character_name(ch));
                const ImVec2 ns = measure(font_bold, 15.0f * k, name);
                text(dl, font_bold, 15.0f * k, ImVec2((pmin.x + pmax.x - ns.x) / 2, (pmin.y + pmax.y - ns.y) / 2),
                     rgba(40, 40, 50), name);
            }
            slot_hits.pick_min[card] = pmin;
            slot_hits.pick_max[card] = pmax;
            draw_slot_button(dl, card, bmin, bmax, "PLAY", k, focused);
            return;
        }

        // A saved slot: zone portrait with its tag and the character, zone name,
        // emeralds, lives and score, CONTINUE.
        const std::optional<rsdkv4::SlotZone> zone = rsdkv4::describe_slot(slots_game_id, *saved);
        const ImVec2 smin = in_min, smax(in_max.x, in_min.y + in_w * 9.0f / 16.0f);
        const float sr = 4.0f * k;
        dl->AddRectFilledMultiColor(smin, smax, rgba(0x1b, 0x2a, 0x4a), rgba(0x14, 0x1f, 0x38), rgba(0x08, 0x0d, 0x1c),
                                    rgba(0x0c, 0x14, 0x28));
        if (zone && !zone->art.empty()) {
            if (SDL_Texture *tex = art(zone->art, true))
                draw_image(dl, tex, smin, smax, true, rgba(230, 230, 230), sr);
        }
        dl->AddRect(smin, smax, rgba(255, 255, 255, 51), sr, 1.0f * k);
        if (zone && !zone->tag.empty()) {
            const std::string tag = zone->act > 0 ? zone->tag + " - ACT " + std::to_string(zone->act) : zone->tag;
            const ImVec2 ts = measure(font_bold, 8.5f * k, tag);
            const ImVec2 tmin(smin.x + 4.0f * k, smin.y + 4.0f * k);
            dl->AddRectFilled(tmin, ImVec2(tmin.x + ts.x + 8.0f * k, tmin.y + ts.y + 2.0f * k), rgba(0, 0, 0, 184),
                              3.0f * k);
            text(dl, font_bold, 8.5f * k, ImVec2(tmin.x + 4.0f * k, tmin.y + 1.0f * k), rgba(0xfb, 0xbf, 0x24), tag);
        }
        // Delete, for touch (also Y / Delete): a small bin in the portrait's corner.
        {
            const float b = 20.0f * k;
            const ImVec2 tmax(smax.x - 4.0f * k, smin.y + 4.0f * k + b), tmin(tmax.x - b, tmax.y - b);
            dl->AddRectFilled(tmin, tmax, rgba(0, 0, 0, 184), 3.0f * k);
            const ImVec2 c((tmin.x + tmax.x) / 2, (tmin.y + tmax.y) / 2);
            const ImU32 ic = rgba(230, 230, 236);
            dl->AddRect(ImVec2(c.x - 3.5f * k, c.y - 2.0f * k), ImVec2(c.x + 3.5f * k, c.y + 5.5f * k), ic, 1.0f * k,
                        1.2f * k);
            dl->AddLine(ImVec2(c.x - 5.5f * k, c.y - 4.0f * k), ImVec2(c.x + 5.5f * k, c.y - 4.0f * k), ic, 1.2f * k);
            dl->AddLine(ImVec2(c.x - 2.0f * k, c.y - 6.0f * k), ImVec2(c.x + 2.0f * k, c.y - 6.0f * k), ic, 1.2f * k);
            // A bigger target than the icon: at least 40 px for a finger.
            const float grow = std::max(0.0f, (40.0f * u - b) / 2);
            slot_hits.trash_min[card] = ImVec2(tmin.x - grow, tmin.y - grow);
            slot_hits.trash_max[card] = ImVec2(tmax.x + grow, tmax.y + grow);
        }
        // The character, overlapping the portrait's bottom-right corner by 1rem.
        const float icon_h = 64.0f * k;
        const char *player = kPlayerArt[std::clamp(saved->character, 0, 3)];
        if (SDL_Texture *tex = art(std::string("players/") + player + ".png", false)) {
            const ImVec2 imax(smax.x, smax.y + 16.0f * k), imin(imax.x - icon_h, imax.y - icon_h);
            draw_image(dl, tex, ImVec2(imin.x + 2.0f * k, imin.y + 2.0f * k), ImVec2(imax.x + 2.0f * k, imax.y + 2.0f * k),
                       false, rgba(0, 0, 0, 150));  // drop shadow
            draw_image(dl, tex, imin, imax, false, IM_COL32_WHITE);
        }
        float y = smax.y + 4.0f * k;
        const std::string name = zone ? zone->name : std::string();
        const float title_w = in_w * 0.74f;  // padding-right: 26%, clear of the character
        text(dl, font_bold, 11.5f * k, ImVec2(in_min.x, y + 1.0f * k), rgba(0, 0, 0, 200), name, title_w);
        text(dl, font_bold, 11.5f * k, ImVec2(in_min.x, y), rgba(0xfb, 0xbf, 0x24), name, title_w);
        y += 11.5f * k * 2.3f;

        // Emeralds, centred: diamonds, dim when missing.
        const int total = rsdkv4::emerald_total(slots_game_id);
        const int have = rsdkv4::emerald_count(slots_game_id, *saved);
        const float gem = 11.0f * k, ggap = 3.0f * k;
        float gx = (in_min.x + in_max.x - (static_cast<float>(total) * gem + static_cast<float>(total - 1) * ggap)) / 2;
        const float gy = bmin.y - 8.0f * k - 20.0f * k - 6.0f * k - gem;
        for (int i = 0; i < total; ++i, gx += gem + ggap) {
            const ImU32 c = with_alpha(kGems[i], i < have ? 1.0f : 0.2f);
            const ImVec2 top(gx + gem / 2, gy), right(gx + gem, gy + gem * 0.38f), bottom(gx + gem / 2, gy + gem),
                left(gx, gy + gem * 0.38f);
            dl->AddQuadFilled(top, right, bottom, left, c);
        }

        // Lives and score chips.
        const std::string lives = std::to_string(saved->lives), score = std::to_string(saved->score);
        const float fs = 11.0f * k, chip_h = 18.0f * k, icon = 10.0f * k;
        const float lw = icon + 4.0f * k + measure(font_regular, fs, lives).x + 10.0f * k;
        const float sw = icon + 4.0f * k + measure(font_regular, fs, score).x + 10.0f * k;
        float cx = (in_min.x + in_max.x - (lw + 6.0f * k + sw)) / 2;
        const float cy = bmin.y - 8.0f * k - chip_h;
        auto chip = [&](float w, const std::string &value, bool heart) {
            const ImVec2 cmin(cx, cy), cmax(cx + w, cy + chip_h);
            dl->AddRectFilled(cmin, cmax, rgba(0, 0, 0, 102), 3.0f * k);
            dl->AddRect(cmin, cmax, rgba(255, 255, 255, 26), 3.0f * k, 1.0f);
            const ImVec2 ic(cmin.x + 5.0f * k + icon / 2, (cmin.y + cmax.y) / 2);
            if (heart) {
                const float r = icon * 0.27f;
                dl->AddCircleFilled(ImVec2(ic.x - r * 0.9f, ic.y - r * 0.6f), r, rgba(0xf4, 0x3f, 0x5e));
                dl->AddCircleFilled(ImVec2(ic.x + r * 0.9f, ic.y - r * 0.6f), r, rgba(0xf4, 0x3f, 0x5e));
                dl->AddTriangleFilled(ImVec2(ic.x - icon * 0.5f, ic.y - r * 0.3f), ImVec2(ic.x + icon * 0.5f, ic.y - r * 0.3f),
                                      ImVec2(ic.x, ic.y + icon * 0.48f), rgba(0xf4, 0x3f, 0x5e));
            } else {
                ImVec2 pts[10];
                for (int i = 0; i < 10; ++i) {
                    const float r = (i % 2 == 0 ? 0.5f : 0.22f) * icon;
                    const float a = -kPi / 2 + static_cast<float>(i) * kPi / 5;
                    pts[i] = ImVec2(ic.x + std::cos(a) * r, ic.y + std::sin(a) * r);
                }
                dl->AddConcavePolyFilled(pts, 10, rgba(0xfb, 0xbf, 0x24));
            }
            const ImVec2 vs = measure(font_regular, fs, value);
            text(dl, font_regular, fs, ImVec2(cmin.x + 5.0f * k + icon + 4.0f * k, (cmin.y + cmax.y - vs.y) / 2),
                 rgba(0xe2, 0xe8, 0xf0), value);
            cx += w + 6.0f * k;
        };
        chip(lw, lives, true);
        chip(sw, score, false);

        draw_slot_button(dl, card, bmin, bmax, "CONTINUE", k, focused);
    }

    void draw_slots(ImDrawList *dl, const GameStatus &game, float dt)
    {
        slots_anim = approach(slots_anim, 1.0f, dt, 12.0f);
        slots_time += dt;
        const SlotLayout l = slot_layout();
        const float u = l.u;
        // Darker backdrop with a blue glow from the top (.rsdk-overlay on the slot screen).
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        dl->AddRectFilled(ImVec2(0, 0), size, rgba(4, 6, 12, static_cast<int>(200 * slots_anim)));
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x, size.y * 0.6f), rgba(29, 78, 216, static_cast<int>(70 * slots_anim)),
                                    rgba(29, 78, 216, static_cast<int>(70 * slots_anim)), rgba(29, 78, 216, 0),
                                    rgba(29, 78, 216, 0));

        // Heading: the game's name as a kicker, then "Load game".
        std::string kicker(game.spec->title);
        for (char &c : kicker)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        text(dl, font_bold, 12.0f * u, l.heading, rgba(0x8e, 0x97, 0xa9), kicker);
        text(dl, font_bold, 32.0f * u, ImVec2(l.heading.x, l.heading.y + 20.0f * u), IM_COL32_WHITE, "Load game");

        for (int i = 0; i < kSlotCards; ++i) {
            if (i != slot_focus)
                draw_slot_card(dl, l, i);
        }
        draw_slot_card(dl, l, slot_focus);  // on top: its ring and shadow
        if (!delete_error.empty()) {
            const ImVec2 es = measure(font_regular, 14.0f * u, delete_error);
            text(dl, font_regular, 14.0f * u, ImVec2(l.centre_x - es.x / 2, l.hints_y + 18.0f * u), rgba(240, 120, 120),
                 delete_error);
        }
    }

    void draw_slot_hints(ImDrawList *dl, float u)
    {
        const SlotLayout l = slot_layout();
        const bool saved = saved_slot(slot_focus) != nullptr;
        const bool fresh = new_game_card(slot_focus);
        const char *accept = slot_focus == kMenusCard ? "Open" : saved ? "Continue" : "Play";
        // Measure, then draw centred under the cards (draw_hint goes right to left).
        auto hints = [&](ImDrawList *target, float &x) {
            if (fresh)
                draw_hint(target, x, l.hints_y, u, nullptr, 0, "Up/Down", "Character");
            if (saved)
                draw_hint(target, x, l.hints_y, u, "Y", rgba(240, 200, 80), "Del", "Delete");
            draw_hint(target, x, l.hints_y, u, "B", rgba(240, 110, 110), "Esc", "Back");
            draw_hint(target, x, l.hints_y, u, "A", rgba(80, 220, 140), "Enter", accept);
        };
        float x = 0.0f;
        hints(nullptr, x);
        const float width = -x - 26.0f * u;
        x = l.centre_x + width / 2;
        hints(dl, x);
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

std::vector<std::string> Picker::launch_args() const { return impl_->chosen_args; }
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

    // Back from a game: the slot screen shows again, with the slots as the game left them.
    if (s.slots_open) {
        const bool valid = s.slots_game >= 0 && s.slots_game < count &&
                           has_slots(games[static_cast<std::size_t>(s.slots_game)]);
        if (valid) {
            s.reload_slots();
            s.delete_open = false;
        } else {
            s.slots_open = false;
        }
    }
    s.chosen_args.clear();

    ImVec2 play_min, play_max;
    ImVec2 confirm_rects[4];
    ImVec2 delete_rects[4];
    Uint64 last = SDL_GetPerformanceCounter();
    for (;;) {
        int chosen = -2;  // -2 keep going, -1 quit, >= 0 play
        auto play_card = [&](int card) {
            s.slot_focus = card;
            s.chosen_args = s.card_args(card);
            chosen = s.slots_game;
        };
        auto delete_focused = [&]() {
            std::string error;
            if (rsdkv4::delete_slot(s.slots_dir, s.slot_focus - 1, error))
                s.delete_error.clear();
            else
                s.delete_error = "Could not delete the slot: " + error;
            s.reload_slots();
        };
        // As rsdkv4-wasm: Left/Right go through the cards in order, Up/Down change the
        // character on a new-game card (and move between rows elsewhere, for portrait).
        auto slots_act = [&](Action a) {
            const int card = s.slot_focus;
            switch (a) {
            case Action::Back: s.slots_open = false; break;
            case Action::Accept: play_card(card); break;
            case Action::More:
                if (s.saved_slot(card)) {
                    s.delete_open = true;
                    s.delete_focus = 0;  // No
                }
                break;
            case Action::Left:
            case Action::Right: {
                const Impl::SlotLayout l = s.slot_layout();
                const int *pos = std::find(l.order, l.order + Impl::kSlotCards, card);
                const int at = static_cast<int>(pos - l.order) + (a == Action::Left ? -1 : 1);
                if (at >= 0 && at < Impl::kSlotCards)
                    s.slot_focus = l.order[at];
                break;
            }
            case Action::Up:
            case Action::Down:
                if (s.new_game_card(card)) {
                    int &ch = s.slot_char[static_cast<std::size_t>(card)];
                    ch = (ch + (a == Action::Up ? 3 : 1)) % 4;
                } else {
                    const Impl::SlotLayout l = s.slot_layout();
                    const int next = Impl::nearest(l.min, l.max, Impl::kSlotCards, card, a);
                    if (next >= 0)
                        s.slot_focus = next;
                }
                break;
            default: break;
            }
        };
        auto act = [&](Action a) {
            if (a == Action::None)
                return;
            if (s.delete_open) {
                if (a == Action::Left || a == Action::Right)
                    s.delete_focus = 1 - s.delete_focus;
                else if (a == Action::Back)
                    s.delete_open = false;
                else if (a == Action::Accept) {
                    if (s.delete_focus == 1)
                        delete_focused();
                    s.delete_open = false;
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
            if (s.slots_open) {
                slots_act(a);
                return;
            }
            if (a == Action::Back) {
                s.confirm_quit = true;
                s.confirm_focus = 0;  // No
            } else if (a == Action::More) {
                // Nothing to delete on the covers.
            } else if (a == Action::Accept) {
                const GameStatus &game = games[static_cast<std::size_t>(s.focus)];
                if (has_slots(game))
                    s.open_slots(s.focus, game);
                else if (game.available)
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
                break;
            case SDL_MOUSEBUTTONUP: {
                if (event.button.button != SDL_BUTTON_LEFT)
                    break;
                const ImVec2 p(static_cast<float>(event.button.x), static_cast<float>(event.button.y));
                if (s.delete_open) {
                    if (hit(p, delete_rects[0], delete_rects[1]))
                        s.delete_open = false;
                    else if (hit(p, delete_rects[2], delete_rects[3])) {
                        delete_focused();
                        s.delete_open = false;
                    }
                    break;
                }
                if (s.slots_open && !s.confirm_quit) {
                    const Impl::SlotHits &h = s.slot_hits;
                    for (int c = 0; c < Impl::kSlotCards; ++c) {
                        int &ch = s.slot_char[static_cast<std::size_t>(c)];
                        if (hit(p, h.trash_min[c], h.trash_max[c])) {
                            s.slot_focus = c;
                            s.delete_open = true;
                            s.delete_focus = 0;
                        } else if (hit(p, h.pick_min[c], h.pick_max[c])) {
                            s.slot_focus = c;  // a tap on the character cycles it
                            ch = (ch + 1) % 4;
                        } else if (hit(p, h.button_min[c], h.button_max[c])) {
                            play_card(c);
                        } else if (hit(p, h.card_min[c], h.card_max[c])) {
                            if (c == 0 || c == Impl::kMenusCard)
                                play_card(c);  // the short cards are buttons themselves
                            else
                                s.slot_focus = c;
                        } else {
                            continue;
                        }
                        break;
                    }
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
        if (s.pointer_used && !s.confirm_quit && !s.slots_open) {
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
        if (s.slots_open) {
            s.draw_slots(dl, games[static_cast<std::size_t>(s.slots_game)], dt);
            s.draw_header(dl, u);  // again, over the slot screen's darker backdrop
            s.draw_slot_hints(dl, u);
        } else {
            for (int i = 0; i < count; ++i) {
                if (i != s.focus)
                    s.draw_tile(dl, l, games[static_cast<std::size_t>(i)], i, dt);
            }
            s.draw_tile(dl, l, games[static_cast<std::size_t>(s.focus)], s.focus, dt);  // on top
            s.draw_details(dl, l, games[static_cast<std::size_t>(s.focus)], play_min, play_max);
            const GameStatus &focused = games[static_cast<std::size_t>(s.focus)];
            s.draw_hints(dl, u, focused.available || installable(focused) || importable(focused) ||
                                       !get_app_url(focused).empty());
        }
        s.draw_dialog(ImGui::GetForegroundDrawList(), u, dt, s.delete_open, s.delete_anim, s.delete_focus,
                      "Delete game " + std::to_string(s.slot_focus) + "?", delete_rects);
        s.draw_confirm(ImGui::GetForegroundDrawList(), u, dt, confirm_rects);
        ImGui::Render();

        SDL_SetRenderDrawColor(s.renderer, 14, 13, 20, 255);
        SDL_RenderClear(s.renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), s.renderer);
        SDL_RenderPresent(s.renderer);

        // Nothing moving: sleep until the next event instead of redrawing at full rate.
        bool settled = s.backdrop_mix > 0.995f && std::abs(s.confirm_anim - (s.confirm_quit ? 1.0f : 0.0f)) < 0.005f &&
                       std::abs(s.delete_anim - (s.delete_open ? 1.0f : 0.0f)) < 0.005f &&
                       (!s.slots_open || (s.slots_anim > 0.995f && !s.new_game_card(s.slot_focus)));
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
