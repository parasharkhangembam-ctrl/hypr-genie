// Genie — the macOS Genie animation for Hyprland (Lua config, 0.56+)
// Copyright (c) 2026 Parashar Khangembam. MIT licence, see LICENSE.
// Written with help from Claude (Anthropic).
//
//   hyprctl genie minimize [0xADDR] [x y w h] [slow]   window    -> dock icon
//   hyprctl genie restore   0xADDR  [x y w h] [slow]   dock icon -> window
//   hyprctl genie open     [0xADDR] [x y w h] [slow]   a new window pours out of its icon
//   hyprctl genie close    [0xADDR] [x y w h] [slow]   close the window into its icon
//   hyprctl genie duration [seconds]                   get / set the speed (0.15 - 3)
//   hyprctl genie ping | status
//
// x y w h = the dock icon rectangle in layout pixels (the same units
// hyprctl clients / layers print). Without it the icon is bottom-centre.
//
// How it works
//   1. take a snapshot of the window (the same kind Hyprland uses when a
//      window closes)
//   2. hide the real window (minimise: move it to special:minimized;
//      restore: keep it invisible until the end)
//   3. every frame, right after the windows and below the dock layer, draw
//      the snapshot as ~300 thin rows; each row gets its own position, width
//      and texture band from genie_math.hpp, which bends the window into the
//      macOS funnel

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>

#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include "genie_math.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <unistd.h>

inline HANDLE               PHANDLE = nullptr;
static constexpr const char* GENIE_VERSION = "1.4.4";

// ---------------------------------------------------------------- state ----
struct SGenieAnim {
    bool                                  minimize = true; // false = restore
    PHLWINDOWREF                          window;
    PHLMONITORREF                         monitor;
    SP<Render::IFramebuffer>              snapshot;
    genie::Rect                           src, icon; // monitor-local physical pixels
    std::chrono::steady_clock::time_point start;
    double                                seconds   = 0.55;
    bool                                  finishing = false;
    SP<CEventLoopTimer>                   safety; // finishes the anim even if no frames come
};

static std::vector<SP<SGenieAnim>>     g_anims;
static std::unordered_map<std::string, CBox> g_savedBoxes; // address -> layout box before minimise
static double                          g_seconds = 0.18;
static SP<SHyprCtlCommand>             g_cmd;
inline decltype(Event::bus()->m_events.render.stage.listen(std::function<void(eRenderStage)>{})) g_renderListener;

// -------------------------------------------------------------- helpers ----
static void notify(const std::string& text, bool error = false) {
    HyprlandAPI::addNotification(PHANDLE, "[genie] " + text, error ? CHyprColor{1.0, 0.35, 0.35, 1.0} : CHyprColor{0.56, 0.66, 0.78, 1.0}, error ? 6000 : 2500);
}

static std::string addrOf(PHLWINDOW w) {
    return std::format("0x{:x}", reinterpret_cast<uintptr_t>(w.get()));
}

// run a Lua dispatcher exactly like `hyprctl dispatch '<expr>'`
static std::string dispatch(const std::string& expr) {
    return HyprlandAPI::invokeHyprctlCommand("dispatch", expr);
}

static PHLWINDOW windowFromToken(std::string tok) {
    if (tok.starts_with("address:"))
        tok = tok.substr(8);
    std::ranges::transform(tok, tok.begin(), [](unsigned char c) { return std::tolower(c); });
    if (!tok.starts_with("0x"))
        tok = "0x" + tok;
    for (const auto& w : Desktop::windowState()->windows()) {
        if (w && addrOf(w) == tok)
            return w;
    }
    return nullptr;
}

// layout (logical, global) box -> monitor-local physical pixels
static genie::Rect toLocalPx(const CBox& b, PHLMONITOR m) {
    return {(b.x - m->m_position.x) * m->m_scale, (b.y - m->m_position.y) * m->m_scale, b.w * m->m_scale, b.h * m->m_scale};
}

// keep the snapshot area inside the monitor (no smeared edge pixels)
static genie::Rect clampToMonitor(genie::Rect r, PHLMONITOR m) {
    const double X2 = std::min(r.x + r.w, m->m_pixelSize.x), Y2 = std::min(r.y + r.h, m->m_pixelSize.y);
    r.x = std::max(0.0, r.x);
    r.y = std::max(0.0, r.y);
    r.w = std::max(1.0, X2 - r.x);
    r.h = std::max(1.0, Y2 - r.y);
    return r;
}

static CBox defaultIcon(PHLMONITOR m) {
    return CBox{m->m_position.x + m->m_size.x / 2.0 - 22.0, m->m_position.y + m->m_size.y - 59.0, 44.0, 44.0};
}

static double progressOf(const SGenieAnim& a) {
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - a.start).count();
    return std::clamp(elapsed / std::max(0.05, a.seconds), 0.0, 1.0);
}

// ------------------------------------------------------- render element ----
class CGenieElement : public IPassElement {
  public:
    CGenieElement(WP<SGenieAnim> anim, double t) : m_anim(anim), m_t(t) {
        ;
    }
    virtual ~CGenieElement() = default;

    virtual std::vector<UP<IPassElement>> draw() override;
    virtual bool                          needsLiveBlur() override {
        return false;
    }
    virtual bool needsPrecomputeBlur() override {
        return false;
    }
    virtual const char* passName() override {
        return "CGenieElement";
    }
    virtual ePassElementType type() override {
        return EK_CUSTOM;
    }
    virtual bool undiscardable() override {
        return true;
    }

  private:
    WP<SGenieAnim> m_anim;
    double         m_t = 0;
};

std::vector<UP<IPassElement>> CGenieElement::draw() {
    const auto A = m_anim.lock();
    if (!A || !A->snapshot || !Render::GL::g_pHyprOpenGL)
        return {};

    const auto MON = A->monitor.lock();
    const auto TEX = A->snapshot->getTexture();
    if (!MON || !TEX || !TEX->ok() || MON->m_pixelSize.x <= 0 || MON->m_pixelSize.y <= 0)
        return {};

    const double p     = A->minimize ? m_t : 1.0 - m_t; // 0 = window, 1 = icon
    const float  alpha = sc<float>(genie::alphaAt(p));
    if (alpha <= 0.002F)
        return {};

    const int  ROWS = std::clamp(sc<int>(std::ceil(A->src.h / 3.0)), 32, 320);
    const auto SL   = genie::slices(A->src, A->icon, p, ROWS);
    const auto FB   = MON->m_pixelSize;

    g_pHyprRenderer->pushMonitorTransformEnabled(true);
    for (const auto& s : SL) {
        if (s.dst.w < 0.05 || s.dst.h < 0.01)
            continue;

        const CBox     BOX{s.dst.x, s.dst.y, s.dst.w, s.dst.h};
        const Vector2D UV0{A->src.x / FB.x, (A->src.y + s.v0 * A->src.h) / FB.y};
        const Vector2D UV1{(A->src.x + A->src.w) / FB.x, (A->src.y + s.v1 * A->src.h) / FB.y};

        Render::GL::g_pHyprOpenGL->renderTexture(TEX, BOX,
                                                  {
                                                      .damage                      = &g_pHyprRenderer->m_renderData.damage,
                                                      .a                           = alpha,
                                                      .allowCustomUV               = true,
                                                      .allowDim                    = false,
                                                      .primarySurfaceUVTopLeft     = UV0,
                                                      .primarySurfaceUVBottomRight = UV1,
                                                  });
    }
    g_pHyprRenderer->popMonitorTransformEnabled();
    return {};
}

// ------------------------------------------------------------ lifecycle ----
static void finishAnim(SP<SGenieAnim> a) {
    if (!a || std::ranges::find(g_anims, a) == g_anims.end())
        return;

    if (a->safety) {
        g_pEventLoopManager->removeTimer(a->safety);
        a->safety.reset();
    }

    if (!a->minimize) {
        if (const auto W = a->window.lock(); W && W->m_isMapped) {
            W->alpha(Desktop::View::WINDOW_ALPHA_MOVE_FROM_WORKSPACE)->setValueAndWarp(1.F);
            dispatch(std::format("hl.dsp.focus({{ window = \"address:{}\" }})", addrOf(W)));
        }
    }

    std::erase(g_anims, a);
    if (const auto M = a->monitor.lock(); M)
        g_pHyprRenderer->damageMonitor(M);
}

static void cancelFor(PHLWINDOW w) {
    std::vector<SP<SGenieAnim>> mine;
    for (const auto& a : g_anims) {
        if (a->window.lock() == w)
            mine.emplace_back(a);
    }
    for (const auto& a : mine)
        finishAnim(a);
}

static void startAnim(const SP<SGenieAnim>& a, bool slow) {
    a->seconds = g_seconds * (slow ? 8.0 : 1.0);
    a->start   = std::chrono::steady_clock::now();

    const auto SAFETY_SEC = std::chrono::duration<double>(a->seconds + 0.75);
    a->safety             = makeShared<CEventLoopTimer>(std::chrono::duration_cast<Time::steady_dur>(SAFETY_SEC),
                                                        [w = WP<SGenieAnim>(a)](SP<CEventLoopTimer>, void*) {
                                                if (const auto A = w.lock(); A)
                                                    finishAnim(A);
                                            },
                                                        nullptr);
    g_pEventLoopManager->addTimer(a->safety);

    g_anims.emplace_back(a);
    if (const auto M = a->monitor.lock(); M)
        g_pHyprRenderer->damageMonitor(M);
}

// ------------------------------------------------ desktop slide (Spaces) ----
// macOS slides the whole Space - wallpaper and desktop widgets included. Hyprland
// keeps background/bottom layers (wallpaper, desktop clock) still and only slides
// windows. While a workspace slides we cover those layers and draw their own live
// textures as one seamless strip (current Space + its neighbour), moving with it.
// No snapshots, so nothing extra is rendered - it costs a few texture draws a frame.
// hyprctl genie desktopslide on|off
static bool        g_desktopSlide = true;
static long        g_dsCalls = 0, g_dsFrames = 0, g_dsAdded = 0, g_dsDraws = 0, g_dsSnaps = 0, g_dsWs = 0;
static std::string g_dsLast;

struct SDeskSnap { // unused since 1.4.4 (live textures); kept so older code paths still compile
    PHLMONITORREF                         monitor;
    std::vector<SP<Render::IFramebuffer>> fbs;
    bool                                  pending = false;
    bool                                  sliding = false;
};
static std::vector<SP<SDeskSnap>> g_deskSnaps;
static SP<CEventLoopTimer>        g_deskTimer;
static void                       refreshDeskSnap(WP<SDeskSnap>) {}

// offsets (layout px) to draw the desktop at; empty when nothing slides
static std::vector<Vector2D> slideOffsets(PHLMONITOR M) {
    bool     moving = false;
    Vector2D other;
    for (const auto& ref : State::workspaceState()->workspaces()) {
        const auto WS = ref.lock();
        if (!WS || WS->m_isSpecialWorkspace || WS->m_monitor.lock() != M)
            continue;
        const Vector2D O = WS->m_renderOffset->value();
        if (std::abs(O.x) >= M->m_size.x - 0.5 || std::abs(O.y) >= M->m_size.y - 0.5)
            continue; // completely off screen
        if (std::abs(O.x) > 0.5 || std::abs(O.y) > 0.5) {
            moving = true;
            if (WS != M->m_activeWorkspace)
                other = O;
        }
    }
    if (!moving)
        return {};
    const Vector2D A = M->m_activeWorkspace ? M->m_activeWorkspace->m_renderOffset->value() : Vector2D{};
    const Vector2D R = (std::abs(A.x) > 0.5 || std::abs(A.y) > 0.5) ? A : other;
    if (std::abs(R.x) <= 0.5 && std::abs(R.y) <= 0.5)
        return {};
    const bool     H = std::abs(R.x) >= std::abs(R.y);
    const Vector2D N = H ? Vector2D{R.x - std::copysign(M->m_size.x, R.x), R.y} : Vector2D{R.x, R.y - std::copysign(M->m_size.y, R.y)};
    return {R, N};
}

class CDeskSlideElement : public IPassElement {
  public:
    CDeskSlideElement(PHLMONITORREF mon, std::vector<Vector2D> offs) : m_mon(mon), m_offs(std::move(offs)) {
        ;
    }
    virtual ~CDeskSlideElement() = default;

    virtual std::vector<UP<IPassElement>> draw() override {
        const auto M = m_mon.lock();
        if (!M || !Render::GL::g_pHyprOpenGL)
            return {};
        struct SL {
            SP<Render::ITexture> tex;
            CBox                 box; // layout px, monitor-local
        };
        std::vector<SL> layers;
        for (size_t lvl : {0, 1}) { // 0 = background (wallpaper), 1 = bottom (desktop widgets)
            for (const auto& ref : M->m_layerSurfaceLayers[lvl]) {
                const auto LS = ref.lock();
                if (!LS || !LS->m_mapped || !LS->wlSurface() || !LS->wlSurface()->resource())
                    continue;
                const auto TEX = LS->wlSurface()->resource()->m_current.texture;
                if (!TEX || !TEX->ok())
                    continue;
                const CBox G = LS->m_geometry;
                layers.push_back({TEX, CBox{G.x - M->m_position.x, G.y - M->m_position.y, G.w, G.h}});
            }
        }
        if (layers.empty())
            return {}; // nothing to draw: leave Hyprland's still desktop visible
        g_dsDraws++;
        const double S  = M->m_scale;
        const auto   PX = M->m_pixelSize;
        g_pHyprRenderer->pushMonitorTransformEnabled(true);
        Render::GL::g_pHyprOpenGL->renderRect(CBox{0, 0, PX.x, PX.y}, CHyprColor(0, 0, 0, 1), {.damage = &g_pHyprRenderer->m_renderData.damage});
        for (const auto& O : m_offs) {
            for (const auto& l : layers) {
                const CBox BOX{(l.box.x + O.x) * S, (l.box.y + O.y) * S, l.box.w * S, l.box.h * S};
                if (BOX.x >= PX.x || BOX.y >= PX.y || BOX.x + BOX.w <= 0 || BOX.y + BOX.h <= 0)
                    continue;
                Render::GL::g_pHyprOpenGL->renderTexture(l.tex, BOX, {.damage = &g_pHyprRenderer->m_renderData.damage, .a = 1.F, .allowDim = false});
            }
        }
        g_pHyprRenderer->popMonitorTransformEnabled();
        return {};
    }
    virtual bool needsLiveBlur() override {
        return false;
    }
    virtual bool needsPrecomputeBlur() override {
        return false;
    }
    virtual const char* passName() override {
        return "CDeskSlideElement";
    }
    virtual ePassElementType type() override {
        return EK_CUSTOM;
    }
    virtual bool undiscardable() override {
        return true;
    }

  private:
    PHLMONITORREF         m_mon;
    std::vector<Vector2D> m_offs;
};

static void desktopSlide() {
    g_dsCalls++;
    if (!g_desktopSlide || g_pHyprRenderer->m_bRenderingSnapshot)
        return;
    const auto MON = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!MON)
        return;
    auto offs = slideOffsets(MON);
    if (offs.empty())
        return;
    g_dsFrames++;
    g_dsLast = std::format("strip at {:.0f},{:.0f} + {:.0f},{:.0f}", offs[0].x, offs[0].y, offs[1].x, offs[1].y);
    g_dsAdded++;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CDeskSlideElement>(PHLMONITORREF(MON), std::move(offs)));
}

static void onRenderStage(eRenderStage stage) {
    if (stage == RENDER_PRE_WINDOWS) {
        desktopSlide();
        return;
    }
    if (stage != RENDER_POST_WINDOWS || g_anims.empty())
        return;

    const auto MON = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!MON)
        return;

    bool any = false;
    for (const auto& a : g_anims) {
        if (a->monitor.lock() != MON)
            continue;

        const double t = progressOf(*a);
        g_pHyprRenderer->m_renderPass.add(makeUnique<CGenieElement>(WP<SGenieAnim>(a), t));
        any = true;

        if (t >= 1.0 && !a->finishing) {
            a->finishing = true;
            g_pEventLoopManager->doLater([w = WP<SGenieAnim>(a)] { finishAnim(w.lock()); });
        }
    }

    if (any)
        g_pHyprRenderer->damageMonitor(MON); // keep frames coming while we animate
}

// -------------------------------------------------------------- actions ----
static std::string doMinimize(PHLWINDOW W, std::optional<CBox> iconBox, bool slow) {
    if (!W || !W->m_isMapped)
        return "error: window not found";
    if (!W->m_workspace || W->m_workspace->m_isSpecialWorkspace)
        return "error: window is not on a normal workspace (already minimised?)";
    const auto MON = W->m_monitor.lock();
    if (!MON)
        return "error: window has no monitor";

    cancelFor(W);

    const auto ADDR = addrOf(W);
    const CBox WIN  = W->getFullWindowBoundingBox(); // includes border + shadow
    const auto SNAP = g_pHyprRenderer->makeSnapshotFB(W);
    g_savedBoxes[ADDR] = W->geometricBox(Desktop::View::IGeometric::GEOMETRIC_GOAL);

    dispatch(std::format("hl.dsp.window.move({{ workspace = \"special:minimized\", follow = false, window = \"address:{}\" }})", ADDR));
    if (!W->m_workspace || !W->m_workspace->m_isSpecialWorkspace)
        return "error: could not move the window to special:minimized";

    // Hyprland fades a window out in place when it leaves a visible workspace;
    // finish that fade instantly so no ghost stays behind the genie
    W->alpha(Desktop::View::WINDOW_ALPHA_MOVE_TO_WORKSPACE)->warp();

    if (!SNAP)
        return "ok (minimised without animation: no snapshot)";

    auto A      = makeShared<SGenieAnim>();
    A->minimize = true;
    A->window   = W;
    A->monitor  = MON;
    A->snapshot = SNAP;
    A->src      = clampToMonitor(toLocalPx(WIN, MON), MON);
    A->icon     = toLocalPx(iconBox.value_or(defaultIcon(MON)), MON);
    startAnim(A, slow);
    return "ok";
}

static std::string doRestore(PHLWINDOW W, std::optional<CBox> iconBox, bool slow) {
    if (!W || !W->m_isMapped)
        return "error: window not found";

    cancelFor(W);

    PHLMONITOR MON = Desktop::focusState()->monitor();
    if (!MON)
        MON = W->m_monitor.lock();
    if (!MON || !MON->m_activeWorkspace)
        return "error: no active workspace to restore onto";

    const auto ADDR = addrOf(W);
    const auto WS   = MON->m_activeWorkspace;
    if (W->m_workspace != WS)
        dispatch(std::format("hl.dsp.window.move({{ workspace = {}, follow = false, window = \"address:{}\" }})", WS->m_id, ADDR));
    if (W->m_workspace != WS)
        return "error: could not move the window back";

    // put it back exactly where it was before minimising
    if (const auto IT = g_savedBoxes.find(ADDR); IT != g_savedBoxes.end()) {
        const CBox B = IT->second;
        if (W->m_isFloating) {
            const auto SIZE = W->size(Desktop::View::IGeometric::GEOMETRIC_GOAL);
            if (std::abs(SIZE.x - B.w) > 1 || std::abs(SIZE.y - B.h) > 1)
                dispatch(std::format("hl.dsp.window.resize({{ x = {}, y = {}, window = \"address:{}\" }})", sc<int>(B.w), sc<int>(B.h), ADDR));
            const auto POS = W->position(Desktop::View::IGeometric::GEOMETRIC_GOAL);
            if (std::abs(POS.x - B.x) > 1 || std::abs(POS.y - B.y) > 1)
                dispatch(std::format("hl.dsp.window.move({{ x = {}, y = {}, window = \"address:{}\" }})", sc<int>(B.x), sc<int>(B.y), ADDR));
        }
        g_savedBoxes.erase(IT);
    }
    W->finishAnimation(); // jump straight to the final position/size

    // snapshot it fully visible, then keep it invisible while the genie plays
    auto& FROM = W->alpha(Desktop::View::WINDOW_ALPHA_MOVE_FROM_WORKSPACE);
    FROM->setValueAndWarp(1.F);
    const CBox WIN  = W->getFullWindowBoundingBox();
    const auto SNAP = g_pHyprRenderer->makeSnapshotFB(W);
    if (!SNAP) {
        dispatch(std::format("hl.dsp.focus({{ window = \"address:{}\" }})", ADDR));
        return "ok (restored without animation: no snapshot)";
    }
    FROM->setValueAndWarp(0.F);

    auto A      = makeShared<SGenieAnim>();
    A->minimize = false;
    A->window   = W;
    A->monitor  = MON;
    A->snapshot = SNAP;
    A->src      = clampToMonitor(toLocalPx(WIN, MON), MON);
    A->icon     = toLocalPx(iconBox.value_or(defaultIcon(MON)), MON);
    startAnim(A, slow);
    return "ok";
}

static std::string doClose(PHLWINDOW W, std::optional<CBox> iconBox, bool slow) {
    if (!W || !W->m_isMapped) return "error: window not found";
    const auto MON = W->m_monitor.lock();
    if (!MON) return "error: window has no monitor";
    cancelFor(W);
    const auto ADDR = addrOf(W);
    const CBox WIN  = W->getFullWindowBoundingBox();
    const auto SNAP = g_pHyprRenderer->makeSnapshotFB(W);
    dispatch(std::format(R"(hl.dsp.window.close({{ window = "address:{}" }}))", ADDR));
    if (!SNAP) return "ok (closed without animation: no snapshot)";
    auto A = makeShared<SGenieAnim>();
    A->minimize = true; A->window = W; A->monitor = MON; A->snapshot = SNAP;
    A->src = clampToMonitor(toLocalPx(WIN, MON), MON);
    A->icon = toLocalPx(iconBox.value_or(defaultIcon(MON)), MON);
    startAnim(A, slow);
    return "ok";
}

static std::string doOpen(PHLWINDOW W, std::optional<CBox> iconBox, bool slow) {
    if (!W || !W->m_isMapped) return "error: window not found";
    const auto MON = W->m_monitor.lock();
    if (!MON) return "error: window has no monitor";
    cancelFor(W);
    W->finishAnimation();
    W->alpha(Desktop::View::WINDOW_ALPHA_FADE)->warp();
    auto& FROM = W->alpha(Desktop::View::WINDOW_ALPHA_MOVE_FROM_WORKSPACE);
    FROM->setValueAndWarp(1.F);
    const CBox WIN  = W->getFullWindowBoundingBox();
    const auto SNAP = g_pHyprRenderer->makeSnapshotFB(W);
    if (!SNAP) return "error: no snapshot";
    FROM->setValueAndWarp(0.F);
    auto A = makeShared<SGenieAnim>();
    A->minimize = false; A->window = W; A->monitor = MON; A->snapshot = SNAP;
    A->src = clampToMonitor(toLocalPx(WIN, MON), MON);
    A->icon = toLocalPx(iconBox.value_or(defaultIcon(MON)), MON);
    startAnim(A, slow);
    return "ok";
}

// ---------------------------------------------------------- hyprctl cmd ----
static const char* USAGE = "usage: hyprctl genie minimize [0xADDR] [x y w h] [slow]\n"
                           "       hyprctl genie restore 0xADDR [x y w h] [slow]\n"
                           "       hyprctl genie open|close [0xADDR] [x y w h] [slow]\n"
                           "       hyprctl genie duration <seconds> | desktopslide on|off | ping | status";

static std::string onCommand(eHyprCtlOutputFormat, std::string request) {
    std::istringstream       in(request);
    std::string              word, verb;
    std::vector<std::string> rest;
    in >> word >> verb;
    for (std::string t; in >> t;)
        rest.emplace_back(t);

    if (verb.empty() || verb == "help")
        return USAGE;
    if (verb == "ping")
        return std::format("ok genie {}", GENIE_VERSION);
    if (verb == "status")
        return std::format("ok genie {}: {} running, duration {:.2f}s", GENIE_VERSION, g_anims.size(), g_seconds);

    if (verb == "desktopslide") {
        if (!rest.empty())
            g_desktopSlide = rest[0] == "on" || rest[0] == "1" || rest[0] == "true";
        return std::format("ok desktopslide {} | sliding frames {} | drawn {} | last: {}", g_desktopSlide ? "on" : "off", g_dsFrames, g_dsDraws, g_dsLast);
    }
    if (verb == "duration") {
        if (rest.empty())
            return std::format("ok {:.2f}", g_seconds);
        try {
            g_seconds = std::clamp(std::stod(rest[0]), 0.15, 3.0);
        } catch (...) { return "error: duration needs a number of seconds"; }
        return std::format("ok duration {:.2f}s", g_seconds);
    }

    if (verb != "minimize" && verb != "minimise" && verb != "restore" && verb != "open" && verb != "close")
        return std::string("error: unknown command\n") + USAGE;

    PHLWINDOW           W;
    std::vector<double> nums;
    bool                slow = false;
    for (const auto& t : rest) {
        if (t == "slow") {
            slow = true;
            continue;
        }
        if (!W && (t.starts_with("0x") || t.starts_with("address:"))) {
            W = windowFromToken(t);
            if (!W)
                return "error: no window with address " + t;
            continue;
        }
        try {
            nums.emplace_back(std::stod(t));
        } catch (...) { return "error: unexpected argument '" + t + "'"; }
    }

    std::optional<CBox> icon;
    if (nums.size() == 4)
        icon = CBox{nums[0], nums[1], std::max(1.0, nums[2]), std::max(1.0, nums[3])};
    else if (!nums.empty())
        return "error: the icon rectangle needs 4 numbers: x y w h";

    if (verb == "close") { if (!W) W = Desktop::focusState()->window(); return doClose(W, icon, slow); }
    if (verb == "open") { if (!W) W = Desktop::focusState()->window(); return doOpen(W, icon, slow); }
    if (verb == "restore") {
        if (!W)
            return "error: restore needs a window address";
        return doRestore(W, icon, slow);
    }

    if (!W)
        W = Desktop::focusState()->window();
    return doMinimize(W, icon, slow);
}

// ---------------------------------------------------------------- entry ----
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}


static std::vector<CHyprSignalListener> g_windowListeners;
static CHyprSignalListener              g_openListener;

// ~/.local/bin/genie-minimize (from install.sh) knows where the dock icon is
static std::string minimizeHelper() {
    const char* home = std::getenv("HOME");
    if (!home)
        return "";
    const std::string P = std::string(home) + "/.local/bin/genie-minimize";
    return access(P.c_str(), X_OK) == 0 ? P : "";
}

// Hyprland keeps a toplevel's protocol object private; this standard C++ idiom (explicit
// template instantiation may name private members) lets the plugin send it one event.
struct SToplevelResourceTag {
    using type = SP<CXdgToplevel> CXDGToplevelResource::*;
    friend type toplevelResource(SToplevelResourceTag);
};
template <typename Tag, typename Tag::type M>
struct SGenieRob {
    friend typename Tag::type toplevelResource(Tag) { return M; }
};
template struct SGenieRob<SToplevelResourceTag, &CXDGToplevelResource::m_resource>;

// Hyprland tells apps it can only fullscreen and maximise; with Genie loaded it can minimise
// too. Saying so makes GTK 4 / libadwaita apps show a real minimise (yellow) button.
static void advertiseMinimize(const SP<CXDGToplevelResource>& tl) {
    if (!tl)
        return;
    const auto& RES = tl.get()->*toplevelResource(SToplevelResourceTag{});
    if (!RES || RES->version() < 5)
        return;
    wl_array arr;
    wl_array_init(&arr);
    for (const auto CAP : {XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN, XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE, XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE})
        *sc<uint32_t*>(wl_array_add(&arr, sizeof(uint32_t))) = sc<uint32_t>(CAP);
    RES->sendWmCapabilities(&arr);
    wl_array_release(&arr);
    if (const auto OWNER = tl->m_owner.lock(); OWNER)
        OWNER->scheduleConfigure(); // the protocol wants a configure after new capabilities
}

// an app asked to be minimised / maximised (its own title-bar buttons)
static void hookWindowXDG(PHLWINDOW W) {
    if (!W || !W->m_xdgSurface || !W->m_xdgSurface->m_toplevel) return;
    auto tl = W->m_xdgSurface->m_toplevel.lock();
    if (!tl) return;
    advertiseMinimize(tl);
    g_windowListeners.emplace_back(tl->m_events.stateChanged.registerListener([w = PHLWINDOWREF(W), tlWeak = WP<CXDGToplevelResource>(tl)](std::any) {
        auto win = w.lock();
        auto toplevel = tlWeak.lock();
        if (!win || !toplevel) return;
        if (toplevel->m_state.requestsMinimize.value_or(false)) {
            toplevel->m_state.requestsMinimize.reset();
            if (const auto HELPER = minimizeHelper(); !HELPER.empty())
                dispatch(std::format("hl.dsp.exec_cmd([==[{} {}]==])", HELPER, addrOf(win)));
            else // no helper installed: genie into the bottom centre
                g_pEventLoopManager->doLater([w = PHLWINDOWREF(win)] {
                    if (const auto X = w.lock(); X)
                        doMinimize(X, std::nullopt, false);
                });
        } else if (toplevel->m_state.requestsMaximize.has_value()) {
            toplevel->m_state.requestsMaximize.reset();
            dispatch(std::format(R"(hl.dsp.focus({{ window = "address:{}" }}))", addrOf(win)));
            dispatch("hl.dsp.window.fullscreen(0)");
        }
    }));
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // refuse to run if built against a different Hyprland (a mismatch can crash it)
    const std::string HASH = __hyprland_api_get_hash();
    if (HASH != __hyprland_api_get_client_hash()) {
        notify("built for a different Hyprland version - rebuild it: ./install.sh", true);
        throw std::runtime_error("[genie] version mismatch");
    }

    g_deskTimer = makeShared<CEventLoopTimer>(std::chrono::seconds(20), [](SP<CEventLoopTimer> self, void*) {
        for (const auto& s : g_deskSnaps) {
            if (!s->sliding && !s->pending)
                refreshDeskSnap(s);
        }
        self->updateTimeout(std::chrono::seconds(20));
    }, nullptr);
    g_pEventLoopManager->addTimer(g_deskTimer);
    g_renderListener = Event::bus()->m_events.render.stage.listen(std::function<void(eRenderStage)>{[](eRenderStage s) { onRenderStage(s); }});
    g_cmd            = HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{.name = "genie", .exact = false, .fn = onCommand});

    for (const auto& w : Desktop::windowState()->windows())
        hookWindowXDG(w);
    g_openListener = Event::bus()->m_events.window.open.listen([](PHLWINDOW w) { hookWindowXDG(w); });
    notify(std::format("v{} loaded - try: hyprctl genie minimize", GENIE_VERSION));
    return {"genie", "macOS-style Genie minimise, restore, open and close", "Parashar Khangembam", GENIE_VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (g_deskTimer) {
        g_pEventLoopManager->removeTimer(g_deskTimer);
        g_deskTimer.reset();
    }
    g_deskSnaps.clear();
    // never leave a restoring window invisible
    for (const auto& a : g_anims) {
        if (a->safety)
            g_pEventLoopManager->removeTimer(a->safety);
        if (!a->minimize) {
            if (const auto W = a->window.lock(); W)
                W->alpha(Desktop::View::WINDOW_ALPHA_MOVE_FROM_WORKSPACE)->setValueAndWarp(1.F);
        }
        if (const auto M = a->monitor.lock(); M)
            g_pHyprRenderer->damageMonitor(M);
    }
    g_anims.clear();
    g_savedBoxes.clear();
    g_renderListener = {};
    g_windowListeners.clear();
    g_openListener = {};
    if (g_cmd)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_cmd);
    g_cmd.reset();
}
