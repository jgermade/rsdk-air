#include "game_menu.hpp"

#include <rsdkair/saves_rsdkv4.hpp>

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdlrenderer2.h>
#include <stb_image.h>

#if defined(__ANDROID__)
#include <GLES/gl.h>
#define RSDKAIR_GLES 1
#else
#include <SDL_opengl.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace rsdkair {

namespace {

constexpr float kPi = 3.14159265358979f;

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

// art/ and fonts/: next to the executable on desktop, the APK's assets on Android.
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

int next_pow2(int v)
{
    int p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

// ---- Renderers -----------------------------------------------------------------------
//
// The menu draws with Dear ImGui through whatever the engine draws with. Both renderers
// leave the engine's state as they found it.

class Renderer {
public:
    virtual ~Renderer() = default;
    virtual void init() = 0;      // with the ImGui context current
    virtual void shutdown() = 0;  // idem
    // A texture for art; nullptr-equivalent (0) on failure.
    virtual ImTextureID make_texture(const unsigned char *pixels, int w, int h, bool pixelated) = 0;
    // The frame the engine just drew (before it presents it), for the paused backdrop.
    virtual bool capture(ImTextureID &tex, ImVec2 &uv0, ImVec2 &uv1) = 0;
    virtual void sizes(ImVec2 &window, ImVec2 &framebuffer) = 0;
    virtual void render(ImDrawData *draw_data) = 0;
    virtual void present() = 0;
    virtual void end() = 0;  // the menu closed: give the engine its state back
};

// Fixed-function OpenGL 1.x / OpenGL ES 1.x (RSDKv3 and RSDKv4 ask for a 1.0 context).
// After imgui_impl_opengl2.cpp, with two ES 1 limits: textures are power-of-two sized
// (the texture matrix scales coordinates back) and are re-uploaded whole.
class Gl1Renderer final : public Renderer {
public:
    explicit Gl1Renderer(SDL_Window *window) : window_(window) {}

    void init() override
    {
        ImGuiIO &io = ImGui::GetIO();
        io.BackendRendererName = "rsdkair_gl1";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    }

    void shutdown() override
    {
        for (ImTextureData *tex : ImGui::GetPlatformIO().Textures) {
            if (tex->RefCount == 1) {
                tex->SetStatus(ImTextureStatus_WantDestroy);
                update_texture(tex);
            }
        }
        for (Tex &t : textures_) {
            if (t.id)
                glDeleteTextures(1, &t.id);
            t.id = 0;
        }
        ImGuiIO &io = ImGui::GetIO();
        io.BackendRendererName = nullptr;
        io.BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;
    }

    ImTextureID make_texture(const unsigned char *pixels, int w, int h, bool pixelated) override
    {
        Tex t;
        t.id = upload(pixels, w, h, pixelated, GL_RGBA, t.su, t.sv);
        textures_.push_back(t);
        return static_cast<ImTextureID>(textures_.size());
    }

    bool capture(ImTextureID &tex, ImVec2 &uv0, ImVec2 &uv1) override
    {
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(window_, &w, &h);
        if (w <= 0 || h <= 0)
            return false;
        const int pw = next_pow2(w), ph = next_pow2(h);
        GLint last = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &last);
        if (!frozen_ || frozen_w_ < pw || frozen_h_ < ph) {
            if (frozen_)
                glDeleteTextures(1, &textures_[frozen_ - 1].id);
            float su = 1, sv = 1;
            // RGB: ES 1 only copies into a format the framebuffer has.
            const GLuint id = upload(nullptr, pw, ph, false, GL_RGB, su, sv);
            if (!frozen_) {
                textures_.push_back(Tex{id, 1.0f, 1.0f});
                frozen_ = static_cast<int>(textures_.size());
            } else {
                textures_[frozen_ - 1] = Tex{id, 1.0f, 1.0f};
            }
            frozen_w_ = pw;
            frozen_h_ = ph;
        }
        glBindTexture(GL_TEXTURE_2D, textures_[frozen_ - 1].id);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(last));
        tex = static_cast<ImTextureID>(frozen_);
        // The framebuffer's first row is the bottom one.
        uv0 = ImVec2(0.0f, static_cast<float>(h) / static_cast<float>(frozen_h_));
        uv1 = ImVec2(static_cast<float>(w) / static_cast<float>(frozen_w_), 0.0f);
        return glGetError() == GL_NO_ERROR;
    }

    void sizes(ImVec2 &window, ImVec2 &framebuffer) override
    {
        int w = 0, h = 0, fw = 0, fh = 0;
        SDL_GetWindowSize(window_, &w, &h);
        SDL_GL_GetDrawableSize(window_, &fw, &fh);
        window = ImVec2(static_cast<float>(w), static_cast<float>(h));
        framebuffer = ImVec2(static_cast<float>(fw), static_cast<float>(fh));
    }

    void render(ImDrawData *dd) override
    {
        static_assert(sizeof(ImDrawIdx) == 2, "GL ES 1 draws 16-bit indices only");
        const int fb_w = static_cast<int>(dd->DisplaySize.x * dd->FramebufferScale.x);
        const int fb_h = static_cast<int>(dd->DisplaySize.y * dd->FramebufferScale.y);
        if (fb_w <= 0 || fb_h <= 0)
            return;
        if (dd->Textures) {
            for (ImTextureData *tex : *dd->Textures) {
                if (tex->Status != ImTextureStatus_OK)
                    update_texture(tex);
            }
        }

        // Save what we touch.
        GLint viewport[4], scissor[4], texture = 0, blend_src = 0, blend_dst = 0, matrix_mode = 0, env_mode = 0;
        GLfloat color[4];
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_SCISSOR_BOX, scissor);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        glGetIntegerv(GL_BLEND_SRC, &blend_src);
        glGetIntegerv(GL_BLEND_DST, &blend_dst);
        glGetIntegerv(GL_MATRIX_MODE, &matrix_mode);
        glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &env_mode);
        glGetFloatv(GL_CURRENT_COLOR, color);
        const GLenum caps[] = {GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_SCISSOR_TEST, GL_TEXTURE_2D,
                               GL_LIGHTING, GL_ALPHA_TEST, GL_FOG};
        const GLenum arrays[] = {GL_VERTEX_ARRAY, GL_TEXTURE_COORD_ARRAY, GL_COLOR_ARRAY, GL_NORMAL_ARRAY};
        GLboolean cap_on[8], array_on[4];
        for (int i = 0; i < 8; ++i)
            cap_on[i] = glIsEnabled(caps[i]);
        for (int i = 0; i < 4; ++i)
            array_on[i] = glIsEnabled(arrays[i]);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_LIGHTING);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_FOG);
        glEnable(GL_SCISSOR_TEST);
        glEnable(GL_TEXTURE_2D);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_NORMAL_ARRAY);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glColor4f(1, 1, 1, 1);
        glViewport(0, 0, fb_w, fb_h);

        const float l = dd->DisplayPos.x, r = dd->DisplayPos.x + dd->DisplaySize.x;
        const float t = dd->DisplayPos.y, b = dd->DisplayPos.y + dd->DisplaySize.y;
        glMatrixMode(GL_TEXTURE);
        glPushMatrix();
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
#if RSDKAIR_GLES
        glOrthof(l, r, b, t, -1.0f, 1.0f);
#else
        glOrtho(l, r, b, t, -1.0, 1.0);
#endif
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();

        const ImVec2 clip_off = dd->DisplayPos, clip_scale = dd->FramebufferScale;
        for (const ImDrawList *list : dd->CmdLists) {
            const ImDrawVert *vtx = list->VtxBuffer.Data;
            const ImDrawIdx *idx = list->IdxBuffer.Data;
            glVertexPointer(2, GL_FLOAT, sizeof(ImDrawVert), &vtx->pos);
            glTexCoordPointer(2, GL_FLOAT, sizeof(ImDrawVert), &vtx->uv);
            glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ImDrawVert), &vtx->col);
            for (const ImDrawCmd &cmd : list->CmdBuffer) {
                if (cmd.UserCallback)
                    continue;
                const float cx0 = (cmd.ClipRect.x - clip_off.x) * clip_scale.x;
                const float cy0 = (cmd.ClipRect.y - clip_off.y) * clip_scale.y;
                const float cx1 = (cmd.ClipRect.z - clip_off.x) * clip_scale.x;
                const float cy1 = (cmd.ClipRect.w - clip_off.y) * clip_scale.y;
                if (cx1 <= cx0 || cy1 <= cy0)
                    continue;
                glScissor(static_cast<GLint>(cx0), static_cast<GLint>(static_cast<float>(fb_h) - cy1),
                          static_cast<GLsizei>(cx1 - cx0), static_cast<GLsizei>(cy1 - cy0));
                const std::size_t id = static_cast<std::size_t>(cmd.GetTexID());
                const Tex *tex = id >= 1 && id <= textures_.size() ? &textures_[id - 1] : nullptr;
                glBindTexture(GL_TEXTURE_2D, tex ? tex->id : 0);
                glMatrixMode(GL_TEXTURE);
                glLoadIdentity();
                if (tex)
                    glScalef(tex->su, tex->sv, 1.0f);
                glMatrixMode(GL_MODELVIEW);
                glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd.ElemCount), GL_UNSIGNED_SHORT, idx + cmd.IdxOffset);
            }
        }

        // Restore.
        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_TEXTURE);
        glPopMatrix();
        glMatrixMode(static_cast<GLenum>(matrix_mode));
        for (int i = 0; i < 8; ++i) {
            if (cap_on[i])
                glEnable(caps[i]);
            else
                glDisable(caps[i]);
        }
        for (int i = 0; i < 4; ++i) {
            if (array_on[i])
                glEnableClientState(arrays[i]);
            else
                glDisableClientState(arrays[i]);
        }
        glBlendFunc(static_cast<GLenum>(blend_src), static_cast<GLenum>(blend_dst));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, env_mode);
        glColor4f(color[0], color[1], color[2], color[3]);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
    }

    void present() override { SDL_GL_SwapWindow(window_); }
    void end() override {}

private:
    struct Tex {
        GLuint id = 0;
        float su = 1.0f, sv = 1.0f;  // image size / texture size
    };

    // A power-of-two texture holding w x h pixels at its top-left (pixels may be null).
    GLuint upload(const unsigned char *pixels, int w, int h, bool pixelated, GLenum format, float &su, float &sv)
    {
        const int pw = next_pow2(w), ph = next_pow2(h);
        GLint last = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &last);
        GLuint id = 0;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        const GLint filter = pixelated ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        const int bpp = format == GL_RGB ? 3 : 4;
        const std::vector<unsigned char> zeros(static_cast<std::size_t>(pw) * static_cast<std::size_t>(ph) * bpp, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(format), pw, ph, 0, format, GL_UNSIGNED_BYTE, zeros.data());
        if (pixels)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, format, GL_UNSIGNED_BYTE, pixels);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(last));
        su = static_cast<float>(w) / static_cast<float>(pw);
        sv = static_cast<float>(h) / static_cast<float>(ph);
        return id;
    }

    // Dear ImGui's own textures (the font atlas): created, updated (whole), destroyed.
    void update_texture(ImTextureData *tex)
    {
        if (tex->Status == ImTextureStatus_WantCreate) {
            IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);
            Tex t;
            t.id = upload(static_cast<const unsigned char *>(tex->GetPixels()), tex->Width, tex->Height, false, GL_RGBA,
                          t.su, t.sv);
            textures_.push_back(t);
            tex->SetTexID(static_cast<ImTextureID>(textures_.size()));
            tex->SetStatus(ImTextureStatus_OK);
        } else if (tex->Status == ImTextureStatus_WantUpdates) {
            const std::size_t id = static_cast<std::size_t>(tex->GetTexID());
            if (id >= 1 && id <= textures_.size()) {
                GLint last = 0;
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &last);
                glBindTexture(GL_TEXTURE_2D, textures_[id - 1].id);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex->Width, tex->Height, GL_RGBA, GL_UNSIGNED_BYTE,
                                tex->GetPixels());
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(last));
            }
            tex->SetStatus(ImTextureStatus_OK);
        } else if (tex->Status == ImTextureStatus_WantDestroy) {
            const std::size_t id = static_cast<std::size_t>(tex->GetTexID());
            if (id >= 1 && id <= textures_.size() && textures_[id - 1].id) {
                glDeleteTextures(1, &textures_[id - 1].id);
                textures_[id - 1].id = 0;
            }
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    }

    SDL_Window *window_;
    std::vector<Tex> textures_;  // ImTextureID = index + 1
    int frozen_ = 0;             // ImTextureID of the captured frame
    int frozen_w_ = 0, frozen_h_ = 0;
};

// An SDL_Renderer (RSDKv5): Dear ImGui's own backend, plus saving the engine's logical
// size, viewport, scale and clip rect (RSDKv5 draws at a logical size).
class SdlRenderer final : public Renderer {
public:
    SdlRenderer(SDL_Window *window, SDL_Renderer *renderer) : window_(window), renderer_(renderer) {}

    void init() override { ImGui_ImplSDLRenderer2_Init(renderer_); }

    void shutdown() override
    {
        ImGui_ImplSDLRenderer2_Shutdown();
        for (SDL_Texture *t : textures_)
            SDL_DestroyTexture(t);
        textures_.clear();
    }

    ImTextureID make_texture(const unsigned char *pixels, int w, int h, bool pixelated) override
    {
        SDL_Texture *t = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
        if (!t)
            return ImTextureID_Invalid;
        SDL_UpdateTexture(t, nullptr, pixels, w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, pixelated ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
        textures_.push_back(t);
        return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(t));
    }

    bool capture(ImTextureID &tex, ImVec2 &uv0, ImVec2 &uv1) override
    {
        save();
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(renderer_, &w, &h);
        if (w <= 0 || h <= 0)
            return false;
        std::vector<unsigned char> pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
        if (SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_ABGR8888, pixels.data(), w * 4) != 0)
            return false;
        for (std::size_t i = 3; i < pixels.size(); i += 4)
            pixels[i] = 255;
        tex = make_texture(pixels.data(), w, h, false);
        uv0 = ImVec2(0, 0);
        uv1 = ImVec2(1, 1);
        return tex != ImTextureID_Invalid;
    }

    void sizes(ImVec2 &window, ImVec2 &framebuffer) override
    {
        save();
        int w = 0, h = 0, fw = 0, fh = 0;
        SDL_GetWindowSize(window_, &w, &h);
        SDL_GetRendererOutputSize(renderer_, &fw, &fh);
        window = ImVec2(static_cast<float>(w), static_cast<float>(h));
        framebuffer = ImVec2(static_cast<float>(fw), static_cast<float>(fh));
    }

    void render(ImDrawData *dd) override { ImGui_ImplSDLRenderer2_RenderDrawData(dd, renderer_); }
    void present() override { SDL_RenderPresent(renderer_); }

    void end() override
    {
        if (!saved_)
            return;
        SDL_RenderSetLogicalSize(renderer_, logical_w_, logical_h_);
        SDL_RenderSetScale(renderer_, scale_x_, scale_y_);
        SDL_RenderSetViewport(renderer_, &viewport_);
        SDL_RenderSetClipRect(renderer_, clip_on_ ? &clip_ : nullptr);
        saved_ = false;
    }

private:
    // Takes the engine's logical size off (once per menu), so the menu covers the window.
    void save()
    {
        if (saved_)
            return;
        SDL_RenderGetLogicalSize(renderer_, &logical_w_, &logical_h_);
        SDL_RenderGetScale(renderer_, &scale_x_, &scale_y_);
        SDL_RenderGetViewport(renderer_, &viewport_);
        clip_on_ = SDL_RenderIsClipEnabled(renderer_);
        SDL_RenderGetClipRect(renderer_, &clip_);
        SDL_RenderSetLogicalSize(renderer_, 0, 0);
        SDL_RenderSetScale(renderer_, 1.0f, 1.0f);
        SDL_RenderSetViewport(renderer_, nullptr);
        SDL_RenderSetClipRect(renderer_, nullptr);
        saved_ = true;
    }

    SDL_Window *window_;
    SDL_Renderer *renderer_;
    std::vector<SDL_Texture *> textures_;
    bool saved_ = false;
    int logical_w_ = 0, logical_h_ = 0;
    float scale_x_ = 1.0f, scale_y_ = 1.0f;
    SDL_Rect viewport_{}, clip_{};
    bool clip_on_ = false;
};

// ---- Look ----------------------------------------------------------------------------
// The slot cards are rsdkv4-wasm's (src/app/app.css, craft's GameLauncher); the rest
// follows its pause panel. Art: art/players/<name>.png, art/stages/<game>/*.png.

constexpr const char *kPlayerArt[] = {"sonic", "tails", "knuckles", "sonictails"};
constexpr ImU32 kGems[] = {IM_COL32(0x58, 0xe0, 0x58, 255), IM_COL32(0xf0, 0xa0, 0x38, 255),
                           IM_COL32(0xf0, 0x78, 0xd8, 255), IM_COL32(0x98, 0x78, 0xf0, 255),
                           IM_COL32(0xf0, 0xf0, 0xf0, 255), IM_COL32(0xf0, 0x78, 0x78, 255),
                           IM_COL32(0x78, 0xc0, 0xf8, 255)};
struct PlayerBand {
    float left, right;
    int r, g, b;
};
constexpr PlayerBand kBands[] = {
    {0.30f, 0.20f, 13, 122, 233}, {0.40f, 0.10f, 241, 176, 0}, {0.20f, 0.30f, 255, 20, 0}, {0.20f, 0.70f, 160, 32, 240}};
constexpr float kBandFrames[][2] = {{0.3f, 0.2f}, {0.5f, 0.5f}, {0.1f, 0.2f}, {0.6f, 0.4f}, {0.7f, 0.1f}, {0.2f, 0.8f},
                                    {0.0f, 0.6f}, {0.4f, 0.1f}, {0.1f, 0.7f}, {0.8f, 0.0f}, {0.3f, 0.2f}};

enum class Action { None, Left, Right, Up, Down, Accept, Back, More };

bool hit(ImVec2 p, ImVec2 min, ImVec2 max)
{
    return max.x > min.x && p.x >= min.x && p.x < max.x && p.y >= min.y && p.y < max.y;
}

} // namespace

// ---- The menu ---------------------------------------------------------------------------

struct GameMenu::Impl {
    std::string game_id, title;
    RsdkAirHost hooks{};
    const RsdkAirEngineControl *control = nullptr;
    std::unique_ptr<Renderer> renderer;
    ImGuiContext *imgui = nullptr;
    std::vector<std::vector<unsigned char>> font_data;
    ImFont *font_regular = nullptr, *font_bold = nullptr;
    std::string resources = resource_dir();
    std::map<std::string, ImTextureID> art_cache;
    bool open_requested = false;
    std::vector<SDL_GameController *> pads;

    // State of one opening of the menu.
    enum class Screen { Main, Settings, ConfirmExit, ConfirmDelete };
    bool starting = false;  // the game's start screen (no frame drawn yet)
    bool closing = false;
    Screen screen = Screen::Main;
    int focus = 0;              // index into `items` (Main) or rows (Settings)
    bool picking = false;       // choosing the character on a new-game card
    int dialog_focus = 0;       // 0 the safe answer, 1 the other
    int delete_slot = -1;
    float anim = 0.0f, time = 0.0f;
    ImTextureID frozen = ImTextureID_Invalid;
    ImVec2 frozen_uv0, frozen_uv1;
    std::array<rsdkv4::Slot, rsdkv4::kSlotCount> slots{};
    std::array<int, 5> slot_char{};  // character for a new game per card (0 NO SAVE, 1-4)
    int stick_dir = 0;

    // What can be focused on the main screen, in reading order.
    enum class Kind { NoSave, Slot, Continue, Play, Screen, Settings, Exit };
    struct Item {
        Kind kind;
        int arg = 0;  // slot 0-3, screen index
        std::string label;
        ImVec2 min, max;           // whole item (card or button)
        ImVec2 button_min, button_max;  // card's PLAY / CONTINUE
        ImVec2 pick_min, pick_max;      // card's character square
        ImVec2 trash_min, trash_max;    // card's bin
    };
    static Item item(Kind kind, int arg, std::string label)
    {
        Item it{};
        it.kind = kind;
        it.arg = arg;
        it.label = std::move(label);
        return it;
    }
    std::vector<Item> items;
    struct Row {
        ImVec2 min, max, less_min, less_max, more_min, more_max;
    };
    std::vector<Row> rows;  // settings rows; the last is Back
    ImVec2 dialog_rects[4];

    Impl(std::string id, std::string name) : game_id(std::move(id)), title(std::move(name))
    {
        hooks.ctx = this;
        hooks.attach = [](void *ctx, const RsdkAirEngineControl *c, int starting) {
            static_cast<Impl *>(ctx)->attach(c, starting != 0);
        };
        hooks.event = [](void *ctx, const void *e) {
            return static_cast<Impl *>(ctx)->event(*static_cast<const SDL_Event *>(e)) ? 1 : 0;
        };
        hooks.frame = [](void *ctx) { static_cast<Impl *>(ctx)->frame(); };
    }

    ~Impl() { teardown(); }

    void teardown()
    {
        if (!imgui)
            return;
        ImGui::SetCurrentContext(imgui);
        if (renderer)
            renderer->shutdown();
        ImGui::DestroyContext(imgui);
        imgui = nullptr;
        renderer.reset();
        art_cache.clear();
        for (SDL_GameController *pad : pads)
            SDL_GameControllerClose(pad);
        pads.clear();
    }

    // ---- hooks ----

    void attach(const RsdkAirEngineControl *c, bool start)
    {
        if (!c || !c->window)
            return;
        if (control && control->window == c->window && renderer) {
            control = c;
        } else {
            teardown();
            control = c;
            if (c->gfx == RSDKAIR_GFX_SDL_RENDERER && c->renderer)
                renderer = std::make_unique<SdlRenderer>(static_cast<SDL_Window *>(c->window),
                                                         static_cast<SDL_Renderer *>(c->renderer));
            else if (c->gfx == RSDKAIR_GFX_GL1)
                renderer = std::make_unique<Gl1Renderer>(static_cast<SDL_Window *>(c->window));
            else
                return;
            IMGUI_CHECKVERSION();
            imgui = ImGui::CreateContext();
            ImGui::SetCurrentContext(imgui);
            ImGuiIO &io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            font_regular = load_font("Inter-Medium.ttf");
            font_bold = load_font("Inter-Bold.ttf");
            if (!font_regular)
                font_regular = io.Fonts->AddFontDefault();
            if (!font_bold)
                font_bold = font_regular;
            renderer->init();
        }
        // The back button opens the menu instead of leaving the game.
        SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
        if (start)
            run(true);
    }

    bool event(const SDL_Event &e)
    {
        if (!renderer)
            return false;
        switch (e.type) {
        case SDL_KEYDOWN:
            if (!e.key.repeat && (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_AC_BACK)) {
                open_requested = true;
                return true;
            }
            break;
        case SDL_KEYUP:
            return e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_AC_BACK;
        case SDL_CONTROLLERBUTTONDOWN: {
            SDL_GameController *pad = SDL_GameControllerFromInstanceID(e.cbutton.which);
            const Uint8 b = e.cbutton.button;
            const bool other = pad && ((b == SDL_CONTROLLER_BUTTON_START &&
                                        SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK)) ||
                                       (b == SDL_CONTROLLER_BUTTON_BACK &&
                                        SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)));
            if (b == SDL_CONTROLLER_BUTTON_GUIDE || other) {
                open_requested = true;
                return true;
            }
            break;
        }
        default: break;
        }
        return false;
    }

    void frame()
    {
        if (open_requested && renderer) {
            open_requested = false;
            run(false);
        }
    }

    // ---- resources ----

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

    ImTextureID art(const std::string &relative, bool pixelated)
    {
        auto it = art_cache.find(relative);
        if (it != art_cache.end())
            return it->second;
        ImTextureID id = ImTextureID_Invalid;
        const std::vector<unsigned char> file = read_resource(resources + "art/" + relative);
        int w = 0, h = 0, n = 0;
        if (!file.empty()) {
            if (unsigned char *px = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &w, &h, &n, 4)) {
                id = renderer->make_texture(px, w, h, pixelated);
                stbi_image_free(px);
            }
        }
        art_cache[relative] = id;
        return id;
    }

    // ---- helpers ----

    void text(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col, const std::string &s, float wrap = 0)
    {
        dl->AddText(font, size, pos, col, s.c_str(), nullptr, wrap);
    }

    ImVec2 measure(ImFont *font, float size, const std::string &s, float wrap = 0)
    {
        return font->CalcTextSizeA(size, FLT_MAX, wrap, s.c_str());
    }

    float ui() const
    {
        const ImVec2 s = ImGui::GetIO().DisplaySize;
        const float scale = s.y > s.x ? std::min(s.x / 720.0f, s.y / 1280.0f) : std::min(s.x / 1280.0f, s.y / 720.0f);
        return std::clamp(scale, 0.5f, 2.5f);
    }

    bool has_slots() const { return control && control->slots_kind == RSDKAIR_SLOTS_RSDKV4 && control->read_save_ram; }

    void reload_slots()
    {
        for (int i = 0; i < rsdkv4::kSlotCount; ++i) {
            slots[static_cast<std::size_t>(i)] = rsdkv4::Slot{};
            slots[static_cast<std::size_t>(i)].slot = i;
        }
        if (!has_slots())
            return;
        std::vector<int> ram(rsdkv4::kSlotCount * 8, 0);
        if (control->read_save_ram(ram.data(), static_cast<int>(ram.size())) <= 0)
            return;
        std::vector<std::int32_t> ints(ram.begin(), ram.end());
        for (int i = 0; i < rsdkv4::kSlotCount; ++i)
            slots[static_cast<std::size_t>(i)] = rsdkv4::decode_slot(ints, i);
    }

    // The main screen's items, in reading order.
    void build_items()
    {
        items.clear();
        if (has_slots()) {
            items.push_back(item(Kind::NoSave, -1, "NO SAVE"));
            for (int i = 0; i < rsdkv4::kSlotCount; ++i)
                items.push_back(item(Kind::Slot, i, ""));
        }
        if (!starting)
            items.push_back(item(Kind::Continue, 0, "Continue"));
        else
            items.push_back(item(Kind::Play, 0, has_slots() ? "Title screen" : "Play"));
        const int screens = control->screen_count ? control->screen_count() : 0;
        for (int i = 0; i < screens; ++i) {
            const char *name = control->screen_name ? control->screen_name(i) : nullptr;
            if (name && *name)
                items.push_back(item(Kind::Screen, i, name));
        }
        if (control->setting_count && control->setting_count() > 0)
            items.push_back(item(Kind::Settings, 0, "Settings"));
        items.push_back(item(Kind::Exit, 0, "Back to the launcher"));
    }

    int first_focus() const
    {
        // Paused: Continue. Starting: the first saved slot, else the first empty one,
        // else Play.
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].kind == Kind::Continue)
                return static_cast<int>(i);
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].kind == Kind::Slot && !slots[static_cast<std::size_t>(items[i].arg)].empty)
                return static_cast<int>(i);
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].kind == Kind::Slot || items[i].kind == Kind::Play)
                return static_cast<int>(i);
        }
        return 0;
    }

    // ---- the loop ----

    void run(bool start)
    {
        ImGui::SetCurrentContext(imgui);
        starting = start;
        closing = false;
        screen = Screen::Main;
        picking = false;
        anim = 0.0f;
        frozen = ImTextureID_Invalid;
        reload_slots();
        build_items();
        focus = first_focus();
        open_pads();

        if (!starting && control->set_paused)
            control->set_paused(1);
        if (!starting && !renderer->capture(frozen, frozen_uv0, frozen_uv1))
            frozen = ImTextureID_Invalid;

        Uint64 last = SDL_GetPerformanceCounter();
        while (!closing) {
            SDL_Event e;
            while (SDL_PollEvent(&e))
                handle(e);
            const Uint64 now = SDL_GetPerformanceCounter();
            float dt = static_cast<float>(now - last) / static_cast<float>(SDL_GetPerformanceFrequency());
            last = now;
            dt = std::min(dt, 0.1f);
            time += dt;

            ImVec2 window, fb;
            renderer->sizes(window, fb);
            ImGuiIO &io = ImGui::GetIO();
            io.DisplaySize = window;
            io.DisplayFramebufferScale = ImVec2(window.x > 0 ? fb.x / window.x : 1.0f, window.y > 0 ? fb.y / window.y : 1.0f);
            io.DeltaTime = std::max(dt, 1.0f / 1000.0f);
            ImGui::NewFrame();
            draw(ImGui::GetBackgroundDrawList(), dt);
            ImGui::Render();
            renderer->render(ImGui::GetDrawData());
            renderer->present();
            if (dt < 1.0f / 60.0f)
                SDL_Delay(static_cast<Uint32>((1.0f / 60.0f - dt) * 1000.0f));
        }
        renderer->end();
        if (!starting && control->set_paused)
            control->set_paused(0);
        // Presses made in the menu do not reach the game.
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_KEYDOWN, SDL_KEYUP);
        SDL_FlushEvents(SDL_CONTROLLERAXISMOTION, SDL_CONTROLLERBUTTONUP);
        SDL_FlushEvents(SDL_FINGERDOWN, SDL_FINGERMOTION);
        SDL_FlushEvents(SDL_MOUSEMOTION, SDL_MOUSEWHEEL);
    }

    void open_pads()
    {
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            if (SDL_IsGameController(i) && !SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i))) {
                if (SDL_GameController *pad = SDL_GameControllerOpen(i))
                    pads.push_back(pad);
            }
        }
    }

    void close() { closing = true; }

    void leave()
    {
        if (control->request_exit)
            control->request_exit();
        close();
    }

    // ---- input ----

    void handle(const SDL_Event &e)
    {
        switch (e.type) {
        case SDL_QUIT: leave(); break;
        case SDL_KEYDOWN:
            switch (e.key.keysym.sym) {
            case SDLK_LEFT: act(Action::Left); break;
            case SDLK_RIGHT: act(Action::Right); break;
            case SDLK_UP: act(Action::Up); break;
            case SDLK_DOWN: act(Action::Down); break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_SPACE: act(Action::Accept); break;
            case SDLK_ESCAPE:
            case SDLK_AC_BACK: act(Action::Back); break;
            case SDLK_DELETE:
            case SDLK_BACKSPACE: act(Action::More); break;
            default: break;
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: act(Action::Left); break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: act(Action::Right); break;
            case SDL_CONTROLLER_BUTTON_DPAD_UP: act(Action::Up); break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: act(Action::Down); break;
            case SDL_CONTROLLER_BUTTON_A: act(Action::Accept); break;
            case SDL_CONTROLLER_BUTTON_B:
            case SDL_CONTROLLER_BUTTON_START:
            case SDL_CONTROLLER_BUTTON_GUIDE: act(Action::Back); break;
            case SDL_CONTROLLER_BUTTON_Y: act(Action::More); break;
            default: break;
            }
            break;
        case SDL_CONTROLLERAXISMOTION: {
            if (e.caxis.axis != SDL_CONTROLLER_AXIS_LEFTX && e.caxis.axis != SDL_CONTROLLER_AXIS_LEFTY)
                break;
            const bool x = e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX;
            const int v = e.caxis.value;
            int dir = 0;
            if (v < -20000)
                dir = x ? 1 : 3;
            else if (v > 20000)
                dir = x ? 2 : 4;
            if (dir == 0 && ((x && (stick_dir == 1 || stick_dir == 2)) || (!x && (stick_dir == 3 || stick_dir == 4))))
                stick_dir = 0;
            if (dir != 0 && dir != stick_dir) {
                stick_dir = dir;
                constexpr Action kDirs[] = {Action::None, Action::Left, Action::Right, Action::Up, Action::Down};
                act(kDirs[dir]);
            }
            break;
        }
        case SDL_CONTROLLERDEVICEADDED:
            if (!SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(e.cdevice.which))) {
                if (SDL_GameController *pad = SDL_GameControllerOpen(e.cdevice.which))
                    pads.push_back(pad);
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT && e.button.which != SDL_TOUCH_MOUSEID)
                tap(ImVec2(static_cast<float>(e.button.x), static_cast<float>(e.button.y)));
            break;
        case SDL_FINGERUP: {
            const ImVec2 s = ImGui::GetIO().DisplaySize;
            tap(ImVec2(e.tfinger.x * s.x, e.tfinger.y * s.y));
            break;
        }
        default: break;
        }
    }

    // The item nearest to the focused one in a direction (geometry, as the picker does).
    int nearest(int from, Action a) const
    {
        const Item &f = items[static_cast<std::size_t>(from)];
        const ImVec2 c((f.min.x + f.max.x) / 2, (f.min.y + f.max.y) / 2);
        int best = -1;
        float best_score = FLT_MAX;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (static_cast<int>(i) == from)
                continue;
            const ImVec2 o((items[i].min.x + items[i].max.x) / 2, (items[i].min.y + items[i].max.y) / 2);
            const float dx = o.x - c.x, dy = o.y - c.y;
            float along = 0, across = 0;
            switch (a) {
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
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    const rsdkv4::Slot *saved(const Item &it) const
    {
        if (it.kind != Kind::Slot)
            return nullptr;
        const rsdkv4::Slot &s = slots[static_cast<std::size_t>(it.arg)];
        return s.empty ? nullptr : &s;
    }

    int &character(const Item &it) { return slot_char[static_cast<std::size_t>(it.kind == Kind::NoSave ? 0 : it.arg + 1)]; }

    void start_slot(const Item &it)
    {
        const rsdkv4::Slot *s = saved(it);
        const int player = s ? s->character : character(it);
        if (control->start_slot)
            control->start_slot(it.kind == Kind::NoSave ? -1 : it.arg, player);
        close();
    }

    void activate(int index)
    {
        if (index < 0 || index >= static_cast<int>(items.size()))
            return;
        focus = index;
        const Item &it = items[static_cast<std::size_t>(index)];
        switch (it.kind) {
        case Kind::NoSave: start_slot(it); break;
        case Kind::Slot:
            if (saved(it) || picking)
                start_slot(it);
            else
                picking = true;  // choose the character, then Accept again
            break;
        case Kind::Continue:
        case Kind::Play: close(); break;
        case Kind::Screen:
            if (control->load_screen)
                control->load_screen(it.arg);
            close();
            break;
        case Kind::Settings:
            screen = Screen::Settings;
            focus = 0;
            break;
        case Kind::Exit:
            if (starting) {
                leave();
            } else {
                screen = Screen::ConfirmExit;
                dialog_focus = 0;  // Stay
            }
            break;
        }
    }

    void change_setting(int index, int dir)
    {
        RsdkAirSetting s{};
        if (!control->setting_get || !control->setting_set || !control->setting_get(index, &s))
            return;
        int value = s.value;
        if (s.kind == RSDKAIR_SETTING_RANGE) {
            value = std::clamp(value + dir * std::max(1, s.step), s.min, s.max);
        } else {
            int n = 0;
            while (s.choices && s.choices[n])
                ++n;
            if (n == 0)
                return;
            value = ((value + dir) % n + n) % n;
        }
        if (value != s.value)
            control->setting_set(index, value);
    }

    void act(Action a)
    {
        if (a == Action::None)
            return;
        switch (screen) {
        case Screen::ConfirmExit:
        case Screen::ConfirmDelete:
            if (a == Action::Left || a == Action::Right)
                dialog_focus = 1 - dialog_focus;
            else if (a == Action::Back)
                screen = Screen::Main;
            else if (a == Action::Accept)
                answer(dialog_focus == 1);
            return;
        case Screen::Settings: {
            const int count = static_cast<int>(rows.size());
            if (a == Action::Back) {
                back_from_settings();
            } else if (a == Action::Up) {
                focus = std::max(0, focus - 1);
            } else if (a == Action::Down) {
                focus = std::min(count - 1, focus + 1);
            } else if (focus == count - 1) {
                if (a == Action::Accept)
                    back_from_settings();
            } else if (a == Action::Left || a == Action::Right) {
                change_setting(focus, a == Action::Left ? -1 : 1);
            } else if (a == Action::Accept) {
                change_setting(focus, 1);
            }
            return;
        }
        case Screen::Main: break;
        }

        if (items.empty())
            return;
        Item &it = items[static_cast<std::size_t>(focus)];
        if (picking) {
            int &ch = character(it);
            if (a == Action::Left || a == Action::Up)
                ch = (ch + 3) % 4;
            else if (a == Action::Right || a == Action::Down)
                ch = (ch + 1) % 4;
            else if (a == Action::Accept)
                start_slot(it);
            else if (a == Action::Back)
                picking = false;
            return;
        }
        switch (a) {
        case Action::Accept: activate(focus); break;
        case Action::Back:
            if (starting)
                activate(static_cast<int>(items.size()) - 1);  // back to the launcher
            else
                close();  // Continue
            break;
        case Action::More:
            if (saved(it)) {
                delete_slot = it.arg;
                screen = Screen::ConfirmDelete;
                dialog_focus = 0;  // Cancel
            }
            break;
        default: {
            const int next = nearest(focus, a);
            if (next >= 0)
                focus = next;
            break;
        }
        }
    }

    void back_from_settings()
    {
        screen = Screen::Main;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].kind == Kind::Settings)
                focus = static_cast<int>(i);
        }
    }

    void answer(bool yes)
    {
        if (screen == Screen::ConfirmExit) {
            screen = Screen::Main;
            if (yes)
                leave();
        } else if (screen == Screen::ConfirmDelete) {
            screen = Screen::Main;
            if (yes && control->delete_slot && delete_slot >= 0) {
                control->delete_slot(delete_slot);
                reload_slots();
            }
        }
    }

    void tap(ImVec2 p)
    {
        if (screen == Screen::ConfirmExit || screen == Screen::ConfirmDelete) {
            if (hit(p, dialog_rects[0], dialog_rects[1]))
                answer(false);
            else if (hit(p, dialog_rects[2], dialog_rects[3]))
                answer(true);
            return;
        }
        if (screen == Screen::Settings) {
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const Row &r = rows[i];
                const int index = static_cast<int>(i);
                if (index == static_cast<int>(rows.size()) - 1) {
                    if (hit(p, r.min, r.max))
                        back_from_settings();
                } else if (hit(p, r.less_min, r.less_max)) {
                    focus = index;
                    change_setting(index, -1);
                } else if (hit(p, r.more_min, r.more_max) || hit(p, r.min, r.max)) {
                    focus = index;
                    change_setting(index, 1);
                }
            }
            return;
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            Item &it = items[i];
            const int index = static_cast<int>(i);
            if (hit(p, it.trash_min, it.trash_max)) {
                focus = index;
                picking = false;
                delete_slot = it.arg;
                screen = Screen::ConfirmDelete;
                dialog_focus = 0;
                return;
            }
            if (hit(p, it.pick_min, it.pick_max)) {
                focus = index;
                int &ch = character(it);
                ch = (ch + 1) % 4;
                return;
            }
            if (hit(p, it.button_min, it.button_max)) {
                focus = index;
                start_slot(it);  // with the character shown
                return;
            }
            if (hit(p, it.min, it.max)) {
                if (it.kind == Kind::Slot) {
                    if (focus != index)
                        picking = false;
                    focus = index;
                } else {
                    picking = false;
                    activate(index);
                }
                return;
            }
        }
    }

    // ---- drawing ----

    // Draws an image keeping its aspect: inside the box (contain) or filling it (cover).
    void draw_fit(ImDrawList *dl, ImTextureID tex, float aspect, ImVec2 min, ImVec2 max, bool cover,
                  ImU32 tint = IM_COL32_WHITE, float rounding = 0.0f)
    {
        const float bw = max.x - min.x, bh = max.y - min.y;
        const float ba = bw / std::max(1.0f, bh);
        ImVec2 uv0(0, 0), uv1(1, 1), a = min, b = max;
        if (cover) {
            if (aspect > ba) {
                const float k = ba / aspect;
                uv0.x = (1 - k) / 2, uv1.x = (1 + k) / 2;
            } else {
                const float k = aspect / ba;
                uv0.y = (1 - k) / 2, uv1.y = (1 + k) / 2;
            }
        } else if (aspect > ba) {
            const float h = bw / aspect;
            a.y = min.y + (bh - h) / 2, b.y = a.y + h;
        } else {
            const float w = bh * aspect;
            a.x = min.x + (bw - w) / 2, b.x = a.x + w;
        }
        if (rounding > 0)
            dl->AddImageRounded(ImTextureRef(tex), a, b, uv0, uv1, tint, rounding);
        else
            dl->AddImage(ImTextureRef(tex), a, b, uv0, uv1, tint);
    }

    void draw_hint(ImDrawList *dl, float &x, float y, float u, const char *glyph, ImU32 glyph_col, const char *key,
                   const char *label)
    {
        // Right to left: label, key cap, pad glyph. dl == nullptr only measures.
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

    struct Hint {
        const char *glyph;
        ImU32 col;
        const char *key;
        const char *label;
    };

    void draw_hints(ImDrawList *dl, float u, float y, const std::vector<Hint> &hints)
    {
        // draw_hint goes right to left: the last hint first, so they read in order.
        float x = 0.0f;
        for (auto h = hints.rbegin(); h != hints.rend(); ++h)
            draw_hint(nullptr, x, y, u, h->glyph, h->col, h->key, h->label);
        const float width = -x - 26.0f * u;
        x = ImGui::GetIO().DisplaySize.x / 2 + width / 2;
        for (auto h = hints.rbegin(); h != hints.rend(); ++h)
            draw_hint(dl, x, y, u, h->glyph, h->col, h->key, h->label);
    }

    void draw(ImDrawList *dl, float dt)
    {
        anim = approach(anim, 1.0f, dt, 12.0f);
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        const float u = ui();

        // Backdrop: the frozen game, or the cover before the game's first frame; dimmed,
        // with the blue glow from the top of rsdkv4-wasm's overlay.
        dl->AddRectFilled(ImVec2(0, 0), size, rgba(0, 0, 0));
        if (frozen != ImTextureID_Invalid) {
            dl->AddImage(ImTextureRef(frozen), ImVec2(0, 0), size, frozen_uv0, frozen_uv1);
        } else {
            const ImTextureID cover = art("covers/" + game_id + ".jpg", false);
            if (cover != ImTextureID_Invalid)
                draw_fit(dl, cover, 1.0f, ImVec2(0, 0), size, true);
        }
        dl->AddRectFilled(ImVec2(0, 0), size, rgba(4, 6, 12, frozen != ImTextureID_Invalid ? 190 : 225));
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x, size.y * 0.6f), rgba(29, 78, 216, 60),
                                    rgba(29, 78, 216, 60), rgba(29, 78, 216, 0), rgba(29, 78, 216, 0));

        draw_main(dl, u);
        if (screen == Screen::Settings)
            draw_settings(dl, u);
        else if (screen == Screen::ConfirmExit)
            draw_dialog(dl, u, "Back to the launcher?", "The game saves where the game itself saves.", "Stay", "Leave");
        else if (screen == Screen::ConfirmDelete)
            draw_dialog(dl, u, "Delete game " + std::to_string(delete_slot + 1) + "?", "This cannot be undone.",
                        "Cancel", "Delete");
    }

    // Main screen: heading, the slot cards (if any), a row of buttons, hints.
    void draw_main(ImDrawList *dl, float u)
    {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        const bool portrait = size.y > size.x;
        const float margin = (portrait ? 28.0f : 54.0f) * u;
        const float width = size.x - 2 * margin;
        const float gap = 22.0f * u;
        const bool cards = has_slots();

        // Card size: five in a row in landscape; in portrait NO SAVE across the top and
        // the four slots two by two.
        const int cols = portrait ? 2 : 5;
        const float w = cards ? std::min(portrait ? 250.0f * u : 190.0f * u,
                                         (width - static_cast<float>(cols - 1) * gap) / static_cast<float>(cols))
                              : 0.0f;
        const float k = w / 170.0f;
        const float tall = w * 220.0f / 170.0f, short_h = w * 130.0f / 170.0f;
        const float grid_w = cards ? static_cast<float>(cols) * w + static_cast<float>(cols - 1) * gap : 0.0f;
        const float grid_h = !cards ? 0.0f : portrait ? short_h * 0.6f + gap + 2 * tall + gap : tall;

        // Buttons: pills, wrapped into centred rows.
        struct Pill {
            std::size_t item;
            float w;
        };
        std::vector<Pill> pills;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].kind == Kind::NoSave || items[i].kind == Kind::Slot)
                continue;
            pills.push_back({i, measure(font_bold, 16.0f * u, items[i].label).x + 44.0f * u});
        }
        const float pill_h = 48.0f * u, pill_gap = 12.0f * u;
        std::vector<std::vector<Pill>> lines(1);
        float line_w = 0.0f;
        for (const Pill &p : pills) {
            if (!lines.back().empty() && line_w + pill_gap + p.w > width) {
                lines.emplace_back();
                line_w = 0.0f;
            }
            line_w += (lines.back().empty() ? 0.0f : pill_gap) + p.w;
            lines.back().push_back(p);
        }
        const float pills_h = static_cast<float>(lines.size()) * pill_h + static_cast<float>(lines.size() - 1) * pill_gap;

        const float heading_h = 62.0f * u;
        const float block = heading_h + 28.0f * u + (cards ? grid_h + 34.0f * u : 0.0f) + pills_h + 40.0f * u;
        const float y0 = std::max(24.0f * u, (size.y - block) / 2);
        const float x0 = cards ? (size.x - grid_w) / 2 : margin;

        // Heading.
        std::string kicker = title;
        for (char &c : kicker)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const std::string heading = !starting ? "Paused" : cards ? "Load game" : title;
        const float hx = cards ? x0 : (size.x - measure(font_bold, 32.0f * u, heading).x) / 2;
        const float kx = cards ? x0 : (size.x - measure(font_bold, 12.0f * u, kicker).x) / 2;
        text(dl, font_bold, 12.0f * u, ImVec2(kx, y0), rgba(0x8e, 0x97, 0xa9), kicker);
        text(dl, font_bold, 32.0f * u, ImVec2(hx, y0 + 20.0f * u), IM_COL32_WHITE, heading);
        float y = y0 + heading_h + 28.0f * u;

        // Cards.
        if (cards) {
            std::size_t slot_i = 0;
            for (std::size_t i = 0; i < items.size(); ++i) {
                Item &it = items[i];
                if (it.kind == Kind::NoSave) {
                    if (portrait) {
                        it.min = ImVec2(x0, y);
                        it.max = ImVec2(x0 + grid_w, y + short_h * 0.6f);
                    } else {
                        it.min = ImVec2(x0, y);
                        it.max = ImVec2(x0 + w, y + short_h);
                    }
                } else if (it.kind == Kind::Slot) {
                    const int pos = static_cast<int>(slot_i++);
                    float cx, cy;
                    if (portrait) {
                        cx = x0 + static_cast<float>(pos % 2) * (w + gap);
                        cy = y + short_h * 0.6f + gap + static_cast<float>(pos / 2) * (tall + gap);
                    } else {
                        cx = x0 + static_cast<float>(pos + 1) * (w + gap);
                        cy = y;
                    }
                    it.min = ImVec2(cx, cy);
                    it.max = ImVec2(cx + w, cy + tall);
                }
            }
            for (std::size_t i = 0; i < items.size(); ++i) {
                if ((items[i].kind == Kind::NoSave || items[i].kind == Kind::Slot) && static_cast<int>(i) != focus)
                    draw_card(dl, items[i], static_cast<int>(i), k, u);
            }
            if (focus >= 0 && focus < static_cast<int>(items.size()) &&
                (items[static_cast<std::size_t>(focus)].kind == Kind::NoSave ||
                 items[static_cast<std::size_t>(focus)].kind == Kind::Slot))
                draw_card(dl, items[static_cast<std::size_t>(focus)], focus, k, u);
            y += grid_h + 34.0f * u;
        }

        // Buttons.
        for (const std::vector<Pill> &line : lines) {
            float lw = 0.0f;
            for (const Pill &p : line)
                lw += p.w;
            lw += static_cast<float>(line.size() - 1) * pill_gap;
            float x = (size.x - lw) / 2;
            for (const Pill &p : line) {
                Item &it = items[p.item];
                it.min = ImVec2(x, y);
                it.max = ImVec2(x + p.w, y + pill_h);
                it.button_min = it.button_max = it.pick_min = it.pick_max = it.trash_min = it.trash_max = ImVec2(0, 0);
                const bool f = static_cast<int>(p.item) == focus && screen == Screen::Main;
                const bool danger = it.kind == Kind::Exit;
                const ImU32 bg = f ? (danger ? rgba(0xf8, 0x71, 0x71) : IM_COL32_WHITE) : rgba(255, 255, 255, 16);
                if (f) {
                    const float ring = 3.0f * u;
                    dl->AddRect(ImVec2(it.min.x - ring, it.min.y - ring), ImVec2(it.max.x + ring, it.max.y + ring),
                                danger ? rgba(0xf8, 0x71, 0x71) : IM_COL32_WHITE, 14.0f * u + ring, ring);
                }
                dl->AddRectFilled(it.min, it.max, bg, 14.0f * u);
                const ImVec2 ls = measure(font_bold, 16.0f * u, it.label);
                text(dl, font_bold, 16.0f * u, ImVec2(x + (p.w - ls.x) / 2, y + (pill_h - ls.y) / 2),
                     f ? (danger ? rgba(0x1a, 0x05, 0x05) : rgba(0x0b, 0x0d, 0x12)) : IM_COL32_WHITE, it.label);
                x += p.w + pill_gap;
            }
            y += pill_h + pill_gap;
        }

        // Hints.
        if (screen == Screen::Main) {
            std::vector<Hint> hints;
            const Item *it = focus >= 0 && focus < static_cast<int>(items.size()) ? &items[static_cast<std::size_t>(focus)] : nullptr;
            if (picking) {
                hints = {{"A", rgba(80, 220, 140), "Enter", "Play"},
                         {"B", rgba(240, 110, 110), "Esc", "Cancel"},
                         {nullptr, 0, "Arrows", "Character"}};
            } else {
                hints.push_back({"A", rgba(80, 220, 140), "Enter", "Choose"});
                hints.push_back({"B", rgba(240, 110, 110), "Esc", starting ? "Back" : "Continue"});
                if (it && saved(*it))
                    hints.push_back({"Y", rgba(240, 200, 80), "Del", "Delete"});
            }
            draw_hints(dl, u, std::min(size.y - 26.0f * u, y + 20.0f * u), hints);
        }
    }

    void draw_card(ImDrawList *dl, Item &it, int index, float k, float u)
    {
        const bool focused = index == focus && screen == Screen::Main;
        const float lift = focused ? 4.0f * k : 0.0f;
        const ImVec2 min(it.min.x, it.min.y - lift), max(it.max.x, it.max.y - lift);
        const float round = 8.0f * k, pad = 12.0f * k;
        it.button_min = it.button_max = it.pick_min = it.pick_max = it.trash_min = it.trash_max = ImVec2(0, 0);

        if (focused) {
            for (int i = 1; i <= 5; ++i) {
                const float sp = static_cast<float>(i) * 4.0f * k;
                dl->AddRectFilled(ImVec2(min.x - sp + 6.0f * k, min.y - sp + 24.0f * k),
                                  ImVec2(max.x + sp - 6.0f * k, max.y + sp + 4.0f * k), rgba(0, 0, 0, 22), round + sp);
            }
        }
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

        if (it.kind == Kind::NoSave) {
            const ImVec2 ns = measure(font_bold, 16.0f * k, "NO SAVE");
            text(dl, font_bold, 16.0f * k, ImVec2((min.x + max.x - ns.x) / 2, (min.y + max.y - ns.y) / 2),
                 IM_COL32_WHITE, "NO SAVE");
            return;
        }

        const ImVec2 bmin(in_min.x, in_max.y - 34.0f * k), bmax(in_max.x, in_max.y);
        const rsdkv4::Slot *s = saved(it);
        auto button = [&](const char *label) {
            dl->AddRectFilled(bmin, bmax, focused ? rgba(0, 0, 255) : rgba(65, 105, 225), 4.0f * k);
            const ImVec2 ls = measure(font_bold, 16.0f * k, label);
            const ImVec2 at((bmin.x + bmax.x - ls.x) / 2, (bmin.y + bmax.y - ls.y) / 2);
            text(dl, font_bold, 16.0f * k, ImVec2(at.x, at.y + 1.0f * k), rgba(0, 0, 0, 160), label);
            text(dl, font_bold, 16.0f * k, at, IM_COL32_WHITE, label);
            it.button_min = bmin;
            it.button_max = bmax;
        };

        if (!s) {
            // New game: the character large on white, two colour bands, PLAY.
            const int ch = std::clamp(character(it), 0, 3);
            const ImVec2 pmin = in_min, pmax(in_max.x, in_min.y + in_w);
            dl->AddRectFilled(pmin, pmax, IM_COL32_WHITE);
            auto band = [&](float left, float right, ImU32 col) {
                dl->AddRectFilled(ImVec2(pmin.x + in_w * left, pmin.y), ImVec2(pmax.x - in_w * right, pmax.y), col);
            };
            auto drift = [&](float period, bool reverse, float &left, float &right) {
                float t = std::fmod(time / period, 1.0f);
                if (reverse)
                    t = 1.0f - t;
                const float f = t * 10.0f;
                const int i = std::min(9, static_cast<int>(f));
                const float m = f - static_cast<float>(i);
                left = kBandFrames[i][0] + (kBandFrames[i + 1][0] - kBandFrames[i][0]) * m;
                right = kBandFrames[i][1] + (kBandFrames[i + 1][1] - kBandFrames[i][1]) * m;
            };
            float gl = 3.0f * 16.0f * k / in_w, gr = 16.0f * k / in_w;
            const PlayerBand &pb = kBands[ch];
            float cl = pb.left, cr = pb.right;
            if (focused) {
                drift(20.0f, false, gl, gr);
                drift(30.0f, true, cl, cr);
            }
            band(gl, gr, rgba(180, 180, 180, 51));
            band(cl, cr, rgba(pb.r, pb.g, pb.b, 51));
            const float top_pad = focused ? 0.0f : 16.0f * k, side_pad = focused ? 0.0f : 8.0f * k;
            const ImTextureID tex = art(std::string("players/") + kPlayerArt[ch] + ".png", false);
            if (tex != ImTextureID_Invalid) {
                draw_fit(dl, tex, 1.0f, ImVec2(pmin.x + side_pad, pmin.y + top_pad), ImVec2(pmax.x - side_pad, pmax.y),
                         false);
            } else {
                const std::string name(rsdkv4::character_name(ch));
                const ImVec2 ns = measure(font_bold, 15.0f * k, name);
                text(dl, font_bold, 15.0f * k, ImVec2((pmin.x + pmax.x - ns.x) / 2, (pmin.y + pmax.y - ns.y) / 2),
                     rgba(40, 40, 50), name);
            }
            if (focused && picking) {  // arrows while choosing
                const float cy = (pmin.y + pmax.y) / 2, a = 9.0f * k;
                dl->AddTriangleFilled(ImVec2(pmin.x + 4.0f * k, cy), ImVec2(pmin.x + 4.0f * k + a, cy - a),
                                      ImVec2(pmin.x + 4.0f * k + a, cy + a), rgba(20, 20, 30));
                dl->AddTriangleFilled(ImVec2(pmax.x - 4.0f * k, cy), ImVec2(pmax.x - 4.0f * k - a, cy - a),
                                      ImVec2(pmax.x - 4.0f * k - a, cy + a), rgba(20, 20, 30));
            }
            it.pick_min = pmin;
            it.pick_max = pmax;
            button("PLAY");
            return;
        }

        // Saved game: zone portrait with tag and act, the character on its corner, the
        // zone's name, emeralds, lives and score, CONTINUE.
        const std::optional<rsdkv4::SlotZone> zone = rsdkv4::describe_slot(game_id, *s);
        const ImVec2 smin = in_min, smax(in_max.x, in_min.y + in_w * 9.0f / 16.0f);
        const float sr = 4.0f * k;
        dl->AddRectFilledMultiColor(smin, smax, rgba(0x1b, 0x2a, 0x4a), rgba(0x14, 0x1f, 0x38), rgba(0x08, 0x0d, 0x1c),
                                    rgba(0x0c, 0x14, 0x28));
        if (zone && !zone->art.empty()) {
            const ImTextureID tex = art(zone->art, true);
            if (tex != ImTextureID_Invalid)
                draw_fit(dl, tex, 140.0f / 80.0f, smin, smax, true, rgba(230, 230, 230), sr);
        }
        dl->AddRect(smin, smax, rgba(255, 255, 255, 51), sr, 1.0f * k);
        if (zone && !zone->tag.empty()) {
            const std::string tag = zone->act > 0 ? zone->tag + " - ACT " + std::to_string(zone->act) : zone->tag;
            const ImVec2 ts = measure(font_bold, 8.5f * k, tag);
            const ImVec2 tmin(smin.x + 4.0f * k, smin.y + 4.0f * k);
            dl->AddRectFilled(tmin, ImVec2(tmin.x + ts.x + 8.0f * k, tmin.y + ts.y + 2.0f * k), rgba(0, 0, 0, 184), 3.0f * k);
            text(dl, font_bold, 8.5f * k, ImVec2(tmin.x + 4.0f * k, tmin.y + 1.0f * k), rgba(0xfb, 0xbf, 0x24), tag);
        }
        {  // delete, for touch (Y / Delete too)
            const float bsz = 20.0f * k;
            const ImVec2 tmax(smax.x - 4.0f * k, smin.y + 4.0f * k + bsz), tmin(tmax.x - bsz, tmax.y - bsz);
            dl->AddRectFilled(tmin, tmax, rgba(0, 0, 0, 184), 3.0f * k);
            const ImVec2 c((tmin.x + tmax.x) / 2, (tmin.y + tmax.y) / 2);
            const ImU32 ic = rgba(230, 230, 236);
            dl->AddRect(ImVec2(c.x - 3.5f * k, c.y - 2.0f * k), ImVec2(c.x + 3.5f * k, c.y + 5.5f * k), ic, 1.0f * k, 1.2f * k);
            dl->AddLine(ImVec2(c.x - 5.5f * k, c.y - 4.0f * k), ImVec2(c.x + 5.5f * k, c.y - 4.0f * k), ic, 1.2f * k);
            dl->AddLine(ImVec2(c.x - 2.0f * k, c.y - 6.0f * k), ImVec2(c.x + 2.0f * k, c.y - 6.0f * k), ic, 1.2f * k);
            const float grow = std::max(0.0f, (40.0f * u - bsz) / 2);
            it.trash_min = ImVec2(tmin.x - grow, tmin.y - grow);
            it.trash_max = ImVec2(tmax.x + grow, tmax.y + grow);
        }
        const float icon_h = 64.0f * k;
        const ImTextureID player = art(std::string("players/") + kPlayerArt[std::clamp(s->character, 0, 3)] + ".png", false);
        if (player != ImTextureID_Invalid) {
            const ImVec2 imax(smax.x, smax.y + 16.0f * k), imin(imax.x - icon_h, imax.y - icon_h);
            draw_fit(dl, player, 1.0f, ImVec2(imin.x + 2.0f * k, imin.y + 2.0f * k), ImVec2(imax.x + 2.0f * k, imax.y + 2.0f * k),
                     false, rgba(0, 0, 0, 150));
            draw_fit(dl, player, 1.0f, imin, imax, false);
        }
        float y = smax.y + 4.0f * k;
        const std::string name = zone ? zone->name : std::string();
        const float title_w = in_w * 0.74f;
        text(dl, font_bold, 11.5f * k, ImVec2(in_min.x, y + 1.0f * k), rgba(0, 0, 0, 200), name, title_w);
        text(dl, font_bold, 11.5f * k, ImVec2(in_min.x, y), rgba(0xfb, 0xbf, 0x24), name, title_w);
        y += 11.5f * k * 2.3f;

        const int total = rsdkv4::emerald_total(game_id);
        const int have = rsdkv4::emerald_count(game_id, *s);
        const float gem = 11.0f * k, ggap = 3.0f * k;
        float gx = (in_min.x + in_max.x - (static_cast<float>(total) * gem + static_cast<float>(total - 1) * ggap)) / 2;
        const float gy = bmin.y - 8.0f * k - 20.0f * k - 6.0f * k - gem;
        for (int i = 0; i < total; ++i, gx += gem + ggap) {
            const ImU32 c = with_alpha(kGems[i], i < have ? 1.0f : 0.2f);
            dl->AddQuadFilled(ImVec2(gx + gem / 2, gy), ImVec2(gx + gem, gy + gem * 0.38f), ImVec2(gx + gem / 2, gy + gem),
                              ImVec2(gx, gy + gem * 0.38f), c);
        }

        const std::string lives = std::to_string(s->lives), score = std::to_string(s->score);
        const float fs = 11.0f * k, chip_h = 18.0f * k, icon = 10.0f * k;
        const float lw = icon + 4.0f * k + measure(font_regular, fs, lives).x + 10.0f * k;
        const float sw = icon + 4.0f * k + measure(font_regular, fs, score).x + 10.0f * k;
        float cx = (in_min.x + in_max.x - (lw + 6.0f * k + sw)) / 2;
        const float cy = bmin.y - 8.0f * k - chip_h;
        auto chip = [&](float cw, const std::string &value, bool heart) {
            const ImVec2 cmin(cx, cy), cmax(cx + cw, cy + chip_h);
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
                    const float ang = -kPi / 2 + static_cast<float>(i) * kPi / 5;
                    pts[i] = ImVec2(ic.x + std::cos(ang) * r, ic.y + std::sin(ang) * r);
                }
                dl->AddConcavePolyFilled(pts, 10, rgba(0xfb, 0xbf, 0x24));
            }
            const ImVec2 vs = measure(font_regular, fs, value);
            text(dl, font_regular, fs, ImVec2(cmin.x + 5.0f * k + icon + 4.0f * k, (cmin.y + cmax.y - vs.y) / 2),
                 rgba(0xe2, 0xe8, 0xf0), value);
            cx += cw + 6.0f * k;
        };
        chip(lw, lives, true);
        chip(sw, score, false);
        button("CONTINUE");
    }

    // A panel in the middle, as rsdkv4-wasm's .rsdk-panel.
    void panel(ImDrawList *dl, float u, float w, float h, ImVec2 &min, ImVec2 &max)
    {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        dl->AddRectFilled(ImVec2(0, 0), size, rgba(4, 6, 12, 200));
        min = ImVec2((size.x - w) / 2, (size.y - h) / 2);
        max = ImVec2(min.x + w, min.y + h);
        dl->AddRectFilled(min, max, rgba(0x15, 0x18, 0x21), 28.0f * u);
        dl->AddRect(min, max, rgba(255, 255, 255, 36), 28.0f * u, 1.0f * u);
    }

    void draw_dialog(ImDrawList *dl, float u, const std::string &question, const std::string &detail, const char *no,
                     const char *yes)
    {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        const float w = std::min(500.0f * u, size.x * 0.92f), pad = 26.0f * u;
        const float qh = measure(font_bold, 24.0f * u, question, w - 2 * pad).y;
        const float dh = measure(font_regular, 15.0f * u, detail, w - 2 * pad).y;
        const float h = pad + qh + 10.0f * u + dh + 22.0f * u + 48.0f * u + pad;
        ImVec2 min, max;
        panel(dl, u, w, h, min, max);
        float y = min.y + pad;
        text(dl, font_bold, 24.0f * u, ImVec2(min.x + pad, y), IM_COL32_WHITE, question, w - 2 * pad);
        y += qh + 10.0f * u;
        text(dl, font_regular, 15.0f * u, ImVec2(min.x + pad, y), rgba(0x8e, 0x97, 0xa9), detail, w - 2 * pad);
        y += dh + 22.0f * u;
        const float bw = (w - 2 * pad - 10.0f * u) / 2;
        const char *labels[2] = {no, yes};
        for (int i = 0; i < 2; ++i) {
            const ImVec2 bmin(min.x + pad + static_cast<float>(i) * (bw + 10.0f * u), y), bmax(bmin.x + bw, y + 48.0f * u);
            const bool f = dialog_focus == i;
            const bool danger = i == 1;
            dl->AddRectFilled(bmin, bmax, f ? (danger ? rgba(0xf8, 0x71, 0x71) : IM_COL32_WHITE) : rgba(255, 255, 255, 13),
                              14.0f * u);
            const ImVec2 ls = measure(font_bold, 16.0f * u, labels[i]);
            text(dl, font_bold, 16.0f * u, ImVec2((bmin.x + bmax.x - ls.x) / 2, (bmin.y + bmax.y - ls.y) / 2),
                 f ? rgba(0x0b, 0x0d, 0x12) : IM_COL32_WHITE, labels[i]);
            dialog_rects[i * 2] = bmin;
            dialog_rects[i * 2 + 1] = bmax;
        }
        std::vector<Hint> hints = {{"A", rgba(80, 220, 140), "Enter", "Choose"}, {"B", rgba(240, 110, 110), "Esc", "Back"}};
        draw_hints(dl, u, std::min(size.y - 26.0f * u, max.y + 30.0f * u), hints);
    }

    void draw_settings(ImDrawList *dl, float u)
    {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        const int count = control->setting_count ? control->setting_count() : 0;
        const float w = std::min(560.0f * u, size.x * 0.92f), pad = 26.0f * u;
        const float row_h = 52.0f * u, row_gap = 8.0f * u;
        const float h = pad + 40.0f * u + static_cast<float>(count + 1) * (row_h + row_gap) + pad;
        ImVec2 min, max;
        panel(dl, u, w, std::min(h, size.y - 20.0f * u), min, max);
        text(dl, font_bold, 26.0f * u, ImVec2(min.x + pad, min.y + pad - 4.0f * u), IM_COL32_WHITE, "Settings");
        rows.assign(static_cast<std::size_t>(count + 1), Row{});
        focus = std::clamp(focus, 0, count);
        float y = min.y + pad + 40.0f * u;
        for (int i = 0; i <= count; ++i) {
            Row &r = rows[static_cast<std::size_t>(i)];
            r.min = ImVec2(min.x + pad, y);
            r.max = ImVec2(max.x - pad, y + row_h);
            const bool f = focus == i;
            dl->AddRectFilled(r.min, r.max, f ? IM_COL32_WHITE : rgba(255, 255, 255, 13), 14.0f * u);
            const ImU32 fg = f ? rgba(0x0b, 0x0d, 0x12) : IM_COL32_WHITE;
            if (i == count) {
                const ImVec2 ls = measure(font_bold, 16.0f * u, "Back");
                text(dl, font_bold, 16.0f * u, ImVec2((r.min.x + r.max.x - ls.x) / 2, y + (row_h - ls.y) / 2), fg, "Back");
                break;
            }
            RsdkAirSetting s{};
            if (control->setting_get)
                control->setting_get(i, &s);
            const std::string label = s.label ? s.label : "";
            const ImVec2 ls = measure(font_bold, 16.0f * u, label);
            text(dl, font_bold, 16.0f * u, ImVec2(r.min.x + 16.0f * u, y + (row_h - ls.y) / 2), fg, label);
            std::string value;
            if (s.kind == RSDKAIR_SETTING_RANGE) {
                value = std::to_string(s.value) + (s.max == 100 ? "%" : "");
            } else if (s.choices) {
                int n = 0;
                while (s.choices[n])
                    ++n;
                if (s.value >= 0 && s.value < n)
                    value = s.choices[s.value];
            }
            // < value >, arrows as tap targets.
            const float arrow = 36.0f * u;
            r.more_max = ImVec2(r.max.x - 6.0f * u, r.max.y - 6.0f * u);
            r.more_min = ImVec2(r.more_max.x - arrow, r.min.y + 6.0f * u);
            const ImVec2 vs = measure(font_regular, 15.0f * u, value);
            const float vx = r.more_min.x - 8.0f * u - vs.x;
            r.less_max = ImVec2(vx - 8.0f * u, r.more_max.y);
            r.less_min = ImVec2(r.less_max.x - arrow, r.more_min.y);
            text(dl, font_regular, 15.0f * u, ImVec2(vx, y + (row_h - vs.y) / 2), fg, value);
            auto tri = [&](ImVec2 bmin, ImVec2 bmax, bool left) {
                const ImVec2 c((bmin.x + bmax.x) / 2, (bmin.y + bmax.y) / 2);
                const float a = 6.0f * u;
                if (left)
                    dl->AddTriangleFilled(ImVec2(c.x - a, c.y), ImVec2(c.x + a * 0.6f, c.y - a), ImVec2(c.x + a * 0.6f, c.y + a), fg);
                else
                    dl->AddTriangleFilled(ImVec2(c.x + a, c.y), ImVec2(c.x - a * 0.6f, c.y - a), ImVec2(c.x - a * 0.6f, c.y + a), fg);
            };
            tri(r.less_min, r.less_max, true);
            tri(r.more_min, r.more_max, false);
            y += row_h + row_gap;
        }
        std::vector<Hint> hints = {{nullptr, 0, "Left/Right", "Change"}, {"B", rgba(240, 110, 110), "Esc", "Back"}};
        draw_hints(dl, u, std::min(size.y - 26.0f * u, max.y + 30.0f * u), hints);
    }
};

GameMenu::GameMenu(std::string game_id, std::string title)
    : impl_(std::make_unique<Impl>(std::move(game_id), std::move(title)))
{
}

GameMenu::~GameMenu() = default;

const RsdkAirHost *GameMenu::host() const { return &impl_->hooks; }

} // namespace rsdkair
