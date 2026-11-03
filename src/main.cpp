// hyprdropper: niri-like interactive move for Hyprland's scrolling layout.
//
// While a tiled window in a scrolling workspace is dragged with the mouse (bindm / hl.dsp.window.drag()):
//   - the window is NOT floated; it stays in the layout, so nothing else moves. Its slot is the "gap".
//   - it is only *drawn* under the cursor (Hyprland's per-window floating render offset).
//   - an insert bar shows where it will land.
//   - the view edge-scrolls at outer monitor edges, the mouse wheel scrolls the view.
//   - Esc cancels.
// On release the window is moved once, keeping its column width.
//
// Everything that is not a tiled scrolling-layout window falls through to Hyprland unchanged.

#include <algorithm>
#include <any>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <linux/input-event-codes.h>
#include <wayland-server-core.h>

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/desktop/view/window/WindowGroupMembership.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/ipc/s1/S1.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/transformer/Transformer.hpp>
#include <hyprland/src/render/transformer/TransformerList.hpp>
#include <hyprland/src/desktop/view/window/WindowEffectsController.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>

// The scrolling algorithm keeps its column data private; we only read it and use the same column
// primitives Hyprland itself uses for drops.
#define private public
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#undef private

#include "DropLogic.hpp"

using namespace Layout;
using namespace Layout::Tiled;

static HANDLE PHANDLE = nullptr;

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

// ---------------------------------------------------------------- config

static struct {
    SP<Config::Values::CBoolValue>  enabled;
    SP<Config::Values::CColorValue> color;         // own bar colour
    SP<Config::Values::CBoolValue>  useThemeColor; // follow decoration:glow:color (theme accent)
    SP<Config::Values::CIntValue>   barWidth;
    SP<Config::Values::CIntValue>   edgeSize;
    SP<Config::Values::CFloatValue> edgeSpeed;
    SP<Config::Values::CFloatValue> wheelStep;
    SP<Config::Values::CFloatValue> dragOpacity;
    SP<Config::Values::CFloatValue> dragScale;
    SP<Config::Values::CBoolValue>  showGap;
    SP<Config::Values::CBoolValue>  barAnimation;
    SP<Config::Values::CIntValue>   barGrowMs;
    SP<Config::Values::CBoolValue>  neighborGlow;
    SP<Config::Values::CFloatValue> glowStrength;
} g_cfg;

// ---------------------------------------------------------------- state

struct SDropTarget {
    DropLogic::eKind kind = DropLogic::KIND_RESTORE;
    PHLWORKSPACEREF  workspace;
    bool             scrolling = false; // destination uses the scrolling layout
    WP<SColumnData>  afterColumn;       // NEW_COLUMN: insert after this one; empty = at front
    WP<ITarget>      refTarget;         // INTO_COLUMN: relative to this window ...
    bool             above = false;     // ... above (true) or below it
};

static struct {
    bool                     active  = false; // a drag we manage is in progress
    bool                     lifted  = false; // pointer moved past the threshold
    bool                     ending  = false; // inside our dragEnd handling
    bool                     cancel  = false; // Esc pressed
    bool                     swallowEscRelease = false;
    bool                     faded = false; // we lowered the window's layout alpha
    std::optional<CBox>      gapShown;      // the dragged window's slot as last drawn (global logical)
    PHLWINDOWREF             window;
    WP<ITarget>              target;
    PHLWORKSPACEREF          srcWorkspace;
    float                    srcWidth = 0.5F;
    Vector2D                 startMouse;
    Vector2D                 grabOffset; // pointer - window position at drag start

    SDropTarget              drop;
    std::optional<CBox>      barGoal; // global logical
    std::optional<CBox>      barShown;
    std::string              barKey;     // identity of the drop target the goal belongs to
    std::string              barShownKey;
    std::chrono::steady_clock::time_point barAppear; // when the bar jumped to its current target (grow-in)
    std::chrono::steady_clock::time_point liftTime;  // phase origin for the breathing glow

    // windows next to the insertion point and the edge that faces the bar: they get lit up
    enum eSide : uint8_t { SIDE_LEFT = 0, SIDE_RIGHT, SIDE_TOP, SIDE_BOTTOM };
    std::vector<std::pair<PHLWINDOWREF, eSide>> neighbors;
    std::vector<std::pair<PHLWINDOWREF, eSide>> neighborsShown;
    PHLMONITORREF            barMonitor;

    wl_event_source*         timer = nullptr;
    std::chrono::steady_clock::time_point lastTick;
} g_drag;

// ---------------------------------------------------------------- self-measurement (cheap, always on)

struct SPerfStat {
    uint64_t calls = 0;
    double   total = 0, max = 0; // microseconds
};
enum ePerf : uint8_t { PERF_MOTION = 0, PERF_TICK, PERF_DROP, PERF_RENDER, PERF_SHOULDRENDER, PERF_ANIM, PERF_LAST };
static const char* PERF_NAMES[PERF_LAST] = {"motion", "tick", "computeDrop", "render", "shouldRender", "scaleAnim"};
static SPerfStat   g_perf[PERF_LAST];

struct CPerfScope {
    ePerf                                 which;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~CPerfScope() {
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        auto&        p  = g_perf[which];
        p.calls++;
        p.total += us;
        p.max = std::max(p.max, us);
    }
};

static bool        g_hooksActive = false;
static std::string g_lastReject  = "none"; // why the last drag was not managed (diagnostics)
static uint64_t    g_beginCalls  = 0;

// ---------------------------------------------------------------- hooks

static CFunctionHook* g_hkBeginDrag      = nullptr;
static CFunctionHook* g_hkMoveMouse      = nullptr;
static CFunctionHook* g_hkUpdateDragWin  = nullptr;
static CFunctionHook* g_hkDragEnd        = nullptr;
static CFunctionHook* g_hkShouldRenderWm = nullptr;

typedef void (*origBeginDrag)(CLayoutManager*, SP<ITarget>, eMouseBindMode, std::optional<eRectCorner>, bool);
typedef void (*origMoveMouse)(CLayoutManager*, const Vector2D&);
typedef bool (*origUpdateDragWindow)(Supplementary::CDragStateController*);
typedef bool (*origDragEnd)(Supplementary::CDragStateController*);
typedef bool (*origShouldRenderWindow)(Render::IHyprRenderer*, PHLWINDOW, PHLMONITOR);

// ---------------------------------------------------------------- helpers

static CScrollingAlgorithm* scrollingOf(PHLWORKSPACE ws) {
    if (!ws || !ws->space() || !ws->space()->algorithm())
        return nullptr;
    const auto& TILED = ws->space()->algorithm()->tiledAlgo();
    if (!TILED)
        return nullptr;
    return dynamic_cast<CScrollingAlgorithm*>(TILED.get());
}

static PHLMONITOR monitorAt(const Vector2D& p) {
    for (const auto& m : State::monitorState()->monitors()) {
        if (!m || !m->m_enabled)
            continue;
        if (CBox{m->m_position, m->m_size}.containsPoint(p))
            return m;
    }
    return nullptr;
}

static PHLWORKSPACE activeWorkspaceOf(PHLMONITOR m) {
    if (!m)
        return nullptr;
    return m->m_activeSpecialWorkspace ? m->m_activeSpecialWorkspace : m->m_activeWorkspace;
}

// Theme colour unless the user chose otherwise:
//   use_theme_color set explicitly -> it decides
//   otherwise                      -> col.bar if the user set it, else the theme colour
static bool useThemeColor() {
    static auto PUSETHEME = CConfigValue<Config::INTEGER>("plugin:hyprdropper:use_theme_color");
    static auto PCOLBAR   = CConfigValue<Config::INTEGER>("plugin:hyprdropper:col.bar");
    if (PUSETHEME.setByUser())
        return *PUSETHEME;
    // compatibility: col.bar = 0 used to mean "theme colour"
    return !PCOLBAR.setByUser() || g_cfg.color->value() == 0;
}

static CHyprColor barColor() {
    if (!useThemeColor())
        return CHyprColor{sc<uint64_t>(g_cfg.color->value())};

    static auto PGLOW = CConfigValue<Config::IComplexConfigValue>("decoration:glow:color");
    auto* const GRAD  = sc<Config::CGradientValueData*>(PGLOW.ptr());
    if (GRAD && !GRAD->m_colors.empty()) {
        auto c = GRAD->m_colors.front();
        c.a    = 1.F;
        return c;
    }
    return CHyprColor{0xFF42A5F5};
}

static DropLogic::SRect toRect(const CBox& b) {
    return {b.x, b.y, b.w, b.h};
}

static CBox toBox(const DropLogic::SRect& r) {
    return {r.x, r.y, r.w, r.h};
}

static constexpr double HALO_RANGE_DAMAGE = 48.0; // >= halo range + bar glow

static void damageBar(const std::optional<CBox>& b) {
    if (!b)
        return;
    // include the (breathing) glow and the soft halo around the bar
    g_pHyprRenderer->damageBox(b->copy().expand(HALO_RANGE_DAMAGE));
}

// ---------------------------------------------------------------- scale transformer
//
// Draws the dragged window scaled around the grab point. Hyprland renders the window into a buffer and
// composites it into the box we return, so shrinking the box is all that is needed (the GPU scales).
// Instances live in the window's transformer list; we keep raw pointers only while they are alive and
// remove all of ours before the plugin is unloaded (their code lives in this .so).

class CDragScaleTransformer;
static std::vector<CDragScaleTransformer*> g_scalers;
static wl_event_source*                    g_animTimer = nullptr;
static std::chrono::steady_clock::time_point g_animLast;
static void                                ensureAnimTimer();

class CDragScaleTransformer : public Render::IWindowTransformer {
  public:
    CDragScaleTransformer(PHLWINDOWREF window, Vector2D frac) : m_window(window), m_frac(frac) {
        g_scalers.push_back(this);
    }
    virtual ~CDragScaleTransformer() {
        std::erase(g_scalers, this);
    }

    virtual Render::SWindowTransformBuffer transform(Render::CRenderContext&, const Render::SWindowTransformBuffer& in, const Render::SWindowTransformContext& ctx) {
        if (!m_alive || ctx.standalone || ctx.renderingSnapshot || !ctx.monitor || !in.framebuffer || isIdentity())
            return in;
        const double   MS     = ctx.monitor->m_scale;
        const Vector2D ANCHOR = (ctx.currentBox.pos() + ctx.currentBox.size() * m_frac) * MS;
        return {.framebuffer = in.framebuffer, .box = scaleAround(in.box, ANCHOR), .success = true};
    }
    virtual int priority() const {
        return 100; // after Hyprland's own transformers
    }
    virtual bool active() const {
        return m_alive;
    }
    virtual bool blocksDirectScanout() const {
        return m_alive;
    }
    virtual bool allocatesOutputBuffer() const {
        return false;
    }
    virtual CBox transformedExtents(const CBox& box) const {
        if (!m_alive || isIdentity())
            return box;
        return scaleAround(box, box.pos() + box.size() * m_frac);
    }
    virtual CBox sourceBoxForOutput(const CBox&, const CBox& inputBox) const {
        return inputBox;
    }

    bool isIdentity() const {
        return std::abs(m_scale - 1.0) < 0.0005;
    }
    CBox scaleAround(const CBox& b, const Vector2D& a) const {
        return {a.x + (b.x - a.x) * m_scale, a.y + (b.y - a.y) * m_scale, b.w * m_scale, b.h * m_scale};
    }

    // returns true while still animating
    bool step(double dt) {
        const auto W = m_window.lock();
        if (!W || !m_alive)
            return false;
        if (m_scale == m_target) {
            if (m_target >= 1.0)
                retire(W);
            return false;
        }
        g_pHyprRenderer->damageWindow(W, true);
        constexpr double TAU = 0.06; // ~ 200 ms to settle
        m_scale = m_target + (m_scale - m_target) * std::exp(-dt / TAU);
        if (std::abs(m_scale - m_target) < 0.002)
            m_scale = m_target;
        g_pHyprRenderer->damageWindow(W, true);
        return true;
    }

    void setTarget(double t) {
        m_target = std::clamp(t, 0.1, 1.0);
        ensureAnimTimer();
    }

    // deactivate and let Hyprland drop us; never touch `this` afterwards
    void retire(const PHLWINDOW& w) {
        m_alive = false;
        std::erase(g_scalers, this);
        g_pHyprRenderer->damageWindow(w, true);
        w->effects().transformers()->removeInactive(); // deletes this
    }

    PHLWINDOWREF m_window;
    Vector2D     m_frac;
    double       m_scale  = 1.0;
    double       m_target = 1.0;
    bool         m_alive  = true;
};

static CDragScaleTransformer* scalerFor(const PHLWINDOW& w) {
    for (auto* s : g_scalers) {
        if (s->m_window.lock() == w)
            return s;
    }
    return nullptr;
}

static int onAnimTick(void*) {
    CPerfScope perf{PERF_ANIM};
    const auto NOW = std::chrono::steady_clock::now();
    const auto DT  = std::min(std::chrono::duration<double>(NOW - g_animLast).count(), 0.05);
    g_animLast     = NOW;

    bool       more = false;
    // step() may retire (and delete) a scaler, so iterate over a copy
    for (auto* s : std::vector<CDragScaleTransformer*>(g_scalers)) {
        if (std::ranges::find(g_scalers, s) == g_scalers.end())
            continue;
        if (s->step(DT))
            more = true;
    }

    if (more)
        wl_event_source_timer_update(g_animTimer, 8);
    else {
        wl_event_source_remove(g_animTimer);
        g_animTimer = nullptr;
    }
    return 0;
}

static void ensureAnimTimer() {
    if (g_animTimer)
        return;
    g_animLast  = std::chrono::steady_clock::now();
    g_animTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, onAnimTick, nullptr);
    if (g_animTimer)
        wl_event_source_timer_update(g_animTimer, 1);
}

static void removeAllScalers() {
    if (g_animTimer) {
        wl_event_source_remove(g_animTimer);
        g_animTimer = nullptr;
    }
    for (auto* s : std::vector<CDragScaleTransformer*>(g_scalers)) {
        const auto W = s->m_window.lock();
        if (W)
            s->retire(W);
        else
            std::erase(g_scalers, s);
    }
    g_scalers.clear();
}

// only the border ring, not the whole (large) neighbour window
static void damageNeighbors(const decltype(g_drag.neighbors)& list) {
    for (const auto& [wref, side] : list) {
        (void)side;
        const auto W = wref.lock();
        if (!W)
            continue;
        const CBox   B  = CBox{W->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT), W->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)};
        const double BS = W->presentation().borderSize() + 2.0;
        // border ring
        g_pHyprRenderer->damageBox(CBox{B.x - BS, B.y - BS, B.w + 2 * BS, 2 * BS});
        g_pHyprRenderer->damageBox(CBox{B.x - BS, B.y + B.h - BS, B.w + 2 * BS, 2 * BS});
        g_pHyprRenderer->damageBox(CBox{B.x - BS, B.y - BS, 2 * BS, B.h + 2 * BS});
        g_pHyprRenderer->damageBox(CBox{B.x + B.w - BS, B.y - BS, 2 * BS, B.h + 2 * BS});
    }
}

static void damageGap(const std::optional<CBox>& b) {
    if (b)
        g_pHyprRenderer->damageBox(b->copy().expand(4));
}

static void updateGap() {
    std::optional<CBox> now;
    if (g_drag.active && g_drag.lifted && g_cfg.showGap->value()) {
        // the window's own visual box (surface + border), not the layout slot which also contains the gaps
        if (const auto W = g_drag.window.lock(); W)
            now = CBox{W->position(Desktop::View::IGeometric::GEOMETRIC_GOAL), W->size(Desktop::View::IGeometric::GEOMETRIC_GOAL)}.expand(W->presentation().borderSize());
    }
    if (now != g_drag.gapShown) {
        damageGap(g_drag.gapShown);
    damageNeighbors(g_drag.neighborsShown);
        damageGap(now);
        g_drag.gapShown = now;
    }
}

// ---------------------------------------------------------------- drop computation

static void computeDrop(const Vector2D& mouse) {
    CPerfScope perf{PERF_DROP};
    SDropTarget          drop;
    std::optional<CBox>  bar;
    PHLMONITORREF        barMon;

    const auto           MON = monitorAt(mouse);
    const auto           WS  = activeWorkspaceOf(MON);
    const auto           WIN = g_drag.window.lock();

    drop.workspace = WS;

    if (!MON || !WS || !WIN) {
        g_drag.drop    = drop;
        g_drag.barGoal = std::nullopt;
        return;
    }

    auto* ALGO = scrollingOf(WS);
    drop.scrolling = ALGO != nullptr;

    // non-scrolling destinations and fullscreen workspaces: no preview. On another workspace
    // Hyprland's own placement is used, on the source workspace nothing changes.
    if (!ALGO || Fullscreen::controller()->hasFullscreen(WS)) {
        drop.kind      = DropLogic::KIND_RESTORE;
        drop.scrolling = false;
        g_drag.drop    = drop;
        g_drag.barGoal = std::nullopt;
        return;
    }

    const auto&        SD = ALGO->m_scrollingData;

    DropLogic::SInput  in;
    in.area          = toRect(WS->space()->workArea());
    in.px            = mouse.x;
    in.py            = mouse.y;
    in.sameWorkspace = WS == WIN->m_workspace;
    in.barThickness  = std::max<Config::INTEGER>(1, g_cfg.barWidth->value());

    std::vector<std::vector<SP<ITarget>>> targets;

    for (const auto& col : SD->columns) {
        DropLogic::SColumnInfo ci;
        std::vector<SP<ITarget>> colTargets;
        bool                     first = true;
        for (const auto& td : col->targetDatas) {
            const auto T = td->target.lock();
            if (!T)
                continue;
            const auto BOX = T->position();
            ci.windows.push_back({toRect(BOX), T == g_drag.target.lock()});
            colTargets.push_back(T);
            if (first) {
                ci.box = toRect(BOX);
                first  = false;
            } else {
                const double L = std::min(ci.box.x, BOX.x), Tp = std::min(ci.box.y, BOX.y);
                const double R = std::max(ci.box.right(), BOX.x + BOX.w), B = std::max(ci.box.bottom(), BOX.y + BOX.h);
                ci.box         = {L, Tp, R - L, B - Tp};
            }
        }
        if (colTargets.empty())
            continue; // should not happen; keep indices aligned with `targets`
        in.columns.push_back(ci);
        targets.push_back(colTargets);
    }

    // `in.columns` may skip empty columns: map back to SD->columns by pointer
    std::vector<SP<SColumnData>> colRefs;
    for (const auto& col : SD->columns) {
        if (!col->targetDatas.empty())
            colRefs.push_back(col);
    }

    const auto RES = DropLogic::compute(in);
    drop.kind      = RES.kind;

    if (RES.kind == DropLogic::KIND_NEW_COLUMN) {
        if (RES.insertAfterColumn >= 0 && RES.insertAfterColumn < sc<int>(colRefs.size()))
            drop.afterColumn = colRefs[RES.insertAfterColumn];
    } else if (RES.kind == DropLogic::KIND_INTO_COLUMN) {
        const auto& COLT = targets[RES.intoColumn];
        // insertAfterWindow = -1 -> above the first window, else below window[insertAfterWindow]
        if (RES.insertAfterWindow < 0) {
            drop.refTarget = COLT.front();
            drop.above     = true;
        } else {
            drop.refTarget = COLT[RES.insertAfterWindow];
            drop.above     = false;
        }
    }

    if (RES.bar) {
        bar    = toBox(*RES.bar);
        barMon = MON;
    }

    // neighbours of the insertion point (never the dragged window itself)
    decltype(g_drag.neighbors) neighbors;
    const auto                 DRAGGED   = g_drag.window.lock();
    const auto                 ADD_NEIGH = [&](const SP<ITarget>& t, uint8_t side) {
        const auto W = t ? t->window() : nullptr;
        if (W && W != DRAGGED)
            neighbors.emplace_back(PHLWINDOWREF{W}, sc<decltype(g_drag)::eSide>(side));
    };
    if (RES.bar && RES.kind == DropLogic::KIND_NEW_COLUMN) {
        const int L = RES.insertAfterColumn, R = RES.insertAfterColumn + 1;
        if (L >= 0 && L < sc<int>(targets.size()))
            for (const auto& t : targets[L])
                ADD_NEIGH(t, 1 /* right edge faces the bar */);
        if (R >= 0 && R < sc<int>(targets.size()))
            for (const auto& t : targets[R])
                ADD_NEIGH(t, 0 /* left edge */);
    } else if (RES.bar && RES.kind == DropLogic::KIND_INTO_COLUMN) {
        const auto& COLT = targets[RES.intoColumn];
        const int   A = RES.insertAfterWindow, B = RES.insertAfterWindow + 1;
        if (A >= 0 && A < sc<int>(COLT.size()))
            ADD_NEIGH(COLT[A], 3 /* bottom edge */);
        if (B >= 0 && B < sc<int>(COLT.size()))
            ADD_NEIGH(COLT[B], 2 /* top edge */);
    }

    g_drag.drop       = drop;
    g_drag.barGoal    = bar;
    g_drag.barMonitor = barMon;
    g_drag.neighbors  = std::move(neighbors);
    // same key = same insertion point; the bar may then glide (e.g. while the tape scrolls).
    // a different key is a different target and the bar jumps there.
    g_drag.barKey = std::format("{}:{:x}:{:x}:{:x}:{}", sc<int>(drop.kind), rc<uintptr_t>(drop.workspace.get()), rc<uintptr_t>(drop.afterColumn.get()),
                                rc<uintptr_t>(drop.refTarget.get()), drop.above);
}

// ---------------------------------------------------------------- per-frame update

static void updateWindowOffset() {
    const auto WIN = g_drag.window.lock();
    if (!WIN)
        return;

    const auto MOUSE   = g_pInputManager->getMouseCoordsInternal();
    const auto DESIRED = MOUSE - g_drag.grabOffset;
    const auto OFFSET  = DESIRED - WIN->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);

    if (OFFSET == WIN->presentation().floatingOffset())
        return;

    g_pHyprRenderer->damageWindow(WIN, true);
    WIN->presentation().setFloatingOffset(OFFSET);
    g_pHyprRenderer->damageWindow(WIN, true);
}

static void updateBar() {
    const auto OLD = g_drag.barShown;

    if (!g_drag.barGoal) {
        g_drag.barShown.reset();
    } else if (!g_drag.barShown || g_drag.barShownKey != g_drag.barKey) {
        g_drag.barShown    = g_drag.barGoal; // appear or new target: jump, never travel across windows
        g_drag.barShownKey = g_drag.barKey;
        g_drag.barAppear   = std::chrono::steady_clock::now();
    } else {
        // glide towards the goal
        auto&           s = *g_drag.barShown;
        const auto&     g = *g_drag.barGoal;
        constexpr float K = 0.35F;
        s.x += (g.x - s.x) * K;
        s.y += (g.y - s.y) * K;
        s.w += (g.w - s.w) * K;
        s.h += (g.h - s.h) * K;
        if (std::abs(s.x - g.x) < 0.5 && std::abs(s.y - g.y) < 0.5 && std::abs(s.w - g.w) < 0.5 && std::abs(s.h - g.h) < 0.5)
            s = g;
    }

    if (OLD != g_drag.barShown) {
        damageBar(OLD);
        damageBar(g_drag.barShown);
    }

    if (g_drag.neighborsShown != g_drag.neighbors) {
        damageNeighbors(g_drag.neighborsShown);
        damageNeighbors(g_drag.neighbors);
        g_drag.neighborsShown = g_drag.neighbors;
    }
}

static bool hasMonitorBeyond(const PHLMONITOR& mon, const Vector2D& probe) {
    const auto OTHER = monitorAt(probe);
    return OTHER && OTHER != mon;
}

// Scroll the tape of `ws` by `px` logical pixels (positive = towards later columns).
static void scrollTape(PHLWORKSPACE ws, double px, bool instant) {
    auto* ALGO = scrollingOf(ws);
    if (!ALGO || px == 0)
        return;

    const auto&  SD      = ALGO->m_scrollingData;
    const auto   USABLE  = ALGO->usableArea();
    const bool   HORIZ   = SD->controller->isPrimaryHorizontal();
    const double PRIMARY = HORIZ ? USABLE.w : USABLE.h;
    const double MAXOFF  = std::max(SD->maxWidth() - PRIMARY, 0.0);
    const double CUR     = SD->controller->getOffset();

    double       next = CUR + px;
    // never scroll further out than Hyprland itself would leave the view; don't snap if we are already outside
    if (px > 0)
        next = std::min(next, std::max(CUR, MAXOFF));
    else
        next = std::max(next, std::min(CUR, 0.0));

    if (next == CUR)
        return;

    SD->controller->setOffset(next);
    SD->recalculate(instant);
}

static void edgeScroll(double dtSeconds) {
    const auto MOUSE = g_pInputManager->getMouseCoordsInternal();
    const auto MON   = monitorAt(MOUSE);
    if (!MON)
        return;

    const double ZONE  = std::max<Config::INTEGER>(1, g_cfg.edgeSize->value());
    const double SPEED = g_cfg.edgeSpeed->value();

    const double distL = MOUSE.x - MON->m_position.x;
    const double distR = MON->m_position.x + MON->m_size.x - 1 - MOUSE.x;

    double       dir = 0, dist = 0;
    if (distL < ZONE && !hasMonitorBeyond(MON, {MON->m_position.x - 2, MOUSE.y})) {
        dir  = -1;
        dist = distL;
    } else if (distR < ZONE && !hasMonitorBeyond(MON, {MON->m_position.x + MON->m_size.x + 1, MOUSE.y})) {
        dir  = 1;
        dist = distR;
    }

    if (dir == 0)
        return;

    const double factor = std::clamp(1.0 - dist / ZONE, 0.1, 1.0);
    scrollTape(activeWorkspaceOf(MON), dir * SPEED * factor * dtSeconds, true);
}

static void endOurDrag(bool cancel);

// tick at the fastest monitor's refresh rate (edge scrolling and layout-following stay in step with frames)
static int tickIntervalMs() {
    const auto   MON = g_pHyprRenderer->m_mostHzMonitor.lock();
    const double HZ  = MON && MON->m_refreshRate > 1 ? MON->m_refreshRate : 120.0;
    return std::clamp(sc<int>(std::floor(1000.0 / HZ)), 3, 16);
}

static int onTick(void*) {
    if (!g_drag.active)
        return 0;
    CPerfScope perf{PERF_TICK};

    const auto WIN = g_drag.window.lock();
    if (!WIN || !validMapped(WIN)) {
        endOurDrag(true);
        return 0;
    }

    const auto NOW = std::chrono::steady_clock::now();
    const auto DT  = std::chrono::duration<double>(NOW - g_drag.lastTick).count();
    g_drag.lastTick = NOW;

    if (g_drag.lifted) {
        edgeScroll(std::min(DT, 0.05));
        updateWindowOffset();
        computeDrop(g_pInputManager->getMouseCoordsInternal());
        updateBar();
        updateGap();
        // grow-in and breathing are drawn from the clock: keep the bar region repainting
        if (g_drag.barShown && g_cfg.barAnimation->value())
            damageBar(g_drag.barShown);
        if (g_cfg.neighborGlow->value() && !g_drag.neighborsShown.empty())
            damageNeighbors(g_drag.neighborsShown);
    }

    wl_event_source_timer_update(g_drag.timer, tickIntervalMs());
    return 0;
}

// ---------------------------------------------------------------- drop execution

static void placeInScrolling(const SP<ITarget>& target, const SDropTarget& drop) {
    auto* ALGO = scrollingOf(target->workspace());
    if (!ALGO)
        return;

    const auto DATA = ALGO->dataFor(target);
    if (!DATA)
        return;

    const auto& SD     = ALGO->m_scrollingData;
    const auto  SRCCOL = DATA->column.lock();
    if (!SRCCOL)
        return;

    if (drop.kind == DropLogic::KIND_NEW_COLUMN) {
        const auto AFTER = drop.afterColumn.lock();
        if (drop.afterColumn.expired() == false && !AFTER)
            return;

        // a solo column that only needs a width update (moved into an empty workspace)
        if (SRCCOL->targetDatas.size() == 1 && SD->columns.size() == 1 && !AFTER) {
            SRCCOL->setColumnWidth(g_drag.srcWidth);
            SD->fitCol(SRCCOL);
            SD->recalculate();
            return;
        }

        SRCCOL->remove(target); // may delete SRCCOL if it becomes empty

        int afterIdx = -1;
        if (AFTER) {
            afterIdx = sc<int>(SD->idx(AFTER));
            if (afterIdx < 0)
                afterIdx = sc<int>(SD->columns.size()) - 1; // reference vanished: append
        }

        auto NEWCOL = SD->add(afterIdx, g_drag.srcWidth);
        NEWCOL->add(target);
        SD->fitCol(NEWCOL);
        SD->recalculate();
        return;
    }

    if (drop.kind == DropLogic::KIND_INTO_COLUMN) {
        const auto REF = drop.refTarget.lock();
        if (!REF || REF == target)
            return;
        const auto REFDATA = ALGO->dataFor(REF);
        if (!REFDATA)
            return;
        const auto DSTCOL = REFDATA->column.lock();
        if (!DSTCOL)
            return;

        SRCCOL->remove(target);

        const int REFIDX = sc<int>(DSTCOL->idx(REF));
        DSTCOL->add(target, drop.above ? REFIDX - 1 : REFIDX);
        SD->fitCol(DSTCOL);
        SD->recalculate();
    }
}

static void performDrop(const SDropTarget& drop) {
    const auto WIN    = g_drag.window.lock();
    const auto TARGET = g_drag.target.lock();
    const auto DSTWS  = drop.workspace.lock();

    if (!WIN || !TARGET || !validMapped(WIN) || !DSTWS || TARGET->floating())
        return;

    if (DSTWS != WIN->m_workspace) {
        // other workspace: Hyprland moves it (its own placement), then we refine the position
        Desktop::globalWindowController()->moveWindowToWorkspace(WIN, DSTWS);

        if (WIN->m_workspace != DSTWS || TARGET->floating())
            return;

        if (drop.scrolling && drop.kind != DropLogic::KIND_RESTORE)
            placeInScrolling(TARGET, drop);

        if (DSTWS->m_monitor)
            Desktop::focusState()->rawMonitorFocus(DSTWS->m_monitor.lock());
        Desktop::focusState()->fullWindowFocus(WIN, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
        return;
    }

    if (drop.kind == DropLogic::KIND_RESTORE)
        return;

    placeInScrolling(TARGET, drop);
}

// ---------------------------------------------------------------- drag lifecycle

static void stopTimer() {
    if (g_drag.timer) {
        wl_event_source_remove(g_drag.timer);
        g_drag.timer = nullptr;
    }
}

static void resetState() {
    stopTimer();
    damageBar(g_drag.barShown);
    damageGap(g_drag.gapShown);
    if (const auto W = g_drag.window.lock(); W) {
        g_pHyprRenderer->damageWindow(W, true);
        W->presentation().clearFloatingOffset();
        if (g_drag.faded)
            *W->presentation().alpha(Desktop::View::WINDOW_ALPHA_LAYOUT) = 1.F;
        g_pHyprRenderer->damageWindow(W, true);
    }
    const bool SWALLOW = g_drag.swallowEscRelease;
    g_drag            = {};
    g_drag.swallowEscRelease = SWALLOW;
}

// Called for any end of a drag we manage (release, Esc, window gone).
static void endOurDrag(bool cancel) {
    if (!g_drag.active)
        return;
    g_drag.cancel = g_drag.cancel || cancel;
    // goes through our dragEnd hook, which does the actual work
    g_layoutManager->endDragTarget();
    // if Hyprland had nothing to end, make sure we are clean anyway
    if (g_drag.active)
        resetState();
}

static bool reject(const char* why) {
    g_lastReject = why;
    return false;
}

static bool eligible(const SP<ITarget>& target, eMouseBindMode mode) {
    if (!g_hooksActive)
        return reject("hooks inactive");
    if (!g_cfg.enabled->value())
        return reject("disabled");
    if (mode != MBIND_MOVE)
        return reject("not a move drag");
    if (!target)
        return reject("no target");
    if (target->type() != TARGET_TYPE_WINDOW)
        return reject("group target");
    if (target->floating())
        return reject("floating");
    const auto WIN = target->window();
    if (!WIN || !validMapped(WIN))
        return reject("window not mapped");
    if (WIN->grouping().group())
        return reject("window is grouped");
    if (Fullscreen::controller()->isFullscreen(WIN))
        return reject("fullscreen");
    auto* ALGO = scrollingOf(WIN->m_workspace);
    if (!ALGO)
        return reject("workspace not scrolling");
    if (!ALGO->dataFor(target))
        return reject("target not in scrolling data");
    g_lastReject = "none";
    return true;
}

static void hkBeginDrag(CLayoutManager* thisptr, SP<ITarget> target, eMouseBindMode mode, std::optional<eRectCorner> edge, bool exclusive) {
    g_beginCalls++;
    if (g_drag.active)
        resetState();

    if (eligible(target, mode)) {
        const auto WIN  = target->window();
        auto*      ALGO = scrollingOf(WIN->m_workspace);
        const auto DATA = ALGO->dataFor(target);

        g_drag.active       = true;
        g_drag.window       = WIN;
        g_drag.target       = target;
        g_drag.srcWorkspace = WIN->m_workspace;
        g_drag.srcWidth     = DATA && DATA->column ? DATA->column->getColumnWidth() : 0.5F;
        g_drag.startMouse   = g_pInputManager->getMouseCoordsInternal();
        g_drag.grabOffset   = g_drag.startMouse - WIN->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    }

    (*(origBeginDrag)g_hkBeginDrag->m_original)(thisptr, target, mode, edge, exclusive);

    // Hyprland may have rejected the drag (invalid window etc.) and already ended it
    if (g_drag.active && !g_layoutManager->dragController()->target())
        resetState();
}

static bool hkUpdateDragWindow(Supplementary::CDragStateController* thisptr) {
    // never float the window we manage
    if (g_drag.active && thisptr->target() == g_drag.target.lock())
        return false;
    return (*(origUpdateDragWindow)g_hkUpdateDragWin->m_original)(thisptr);
}

static void hkMoveMouse(CLayoutManager* thisptr, const Vector2D& pos) {
    if (!g_drag.active) {
        (*(origMoveMouse)g_hkMoveMouse->m_original)(thisptr, pos);
        return;
    }

    CPerfScope perf{PERF_MOTION};
    const auto WIN = g_drag.window.lock();
    if (!WIN || !validMapped(WIN)) {
        endOurDrag(true);
        return;
    }

    if (!g_drag.lifted) {
        static auto PTHRESH = CConfigValue<Config::INTEGER>("binds:drag_threshold");
        const double TH      = std::max<double>(*PTHRESH, 1.0);
        if (pos.distanceSq(g_drag.startMouse) < TH * TH)
            return;

        g_drag.lifted   = true;
        g_drag.lastTick = std::chrono::steady_clock::now();
        g_drag.liftTime = g_drag.lastTick;

        const float OPACITY = std::clamp<float>(g_cfg.dragOpacity->value(), 0.1F, 1.F);
        if (OPACITY < 1.F) {
            *WIN->presentation().alpha(Desktop::View::WINDOW_ALPHA_LAYOUT) = OPACITY;
            g_drag.faded                                                    = true;
        }
        const float SCALE = std::clamp<float>(g_cfg.dragScale->value(), 0.2F, 1.F);
        if (SCALE < 1.F) {
            const CBox FULL = WIN->getFullWindowBoundingBox();
            Vector2D   frac = {0.5, 0.5};
            if (FULL.w > 0 && FULL.h > 0)
                frac = ((g_drag.startMouse - FULL.pos()) / FULL.size()).clamp({0, 0}, {1, 1});
            auto* scaler = scalerFor(WIN);
            if (!scaler)
                scaler = WIN->effects().transformers()->emplace<CDragScaleTransformer>(PHLWINDOWREF{WIN}, frac);
            scaler->m_frac = frac;
            scaler->setTarget(SCALE);
        }

        g_drag.timer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, onTick, nullptr);
        if (g_drag.timer)
            wl_event_source_timer_update(g_drag.timer, tickIntervalMs());
    }

    updateWindowOffset();
    computeDrop(pos);
    updateBar();
    updateGap();
}

static bool hkDragEnd(Supplementary::CDragStateController* thisptr) {
    if (!g_drag.active || g_drag.ending)
        return (*(origDragEnd)g_hkDragEnd->m_original)(thisptr);

    g_drag.ending = true;

    const bool  DO_DROP = g_drag.lifted && !g_drag.cancel;
    if (DO_DROP)
        computeDrop(g_pInputManager->getMouseCoordsInternal());
    const auto  DROP   = g_drag.drop;
    const auto  WINDOW = g_drag.window;
    const auto  TARGET = g_drag.target;
    const float WIDTH  = g_drag.srcWidth;
    const bool  LIFTED = g_drag.lifted;

    // where the window is drawn right now: the landing animation starts here
    std::optional<Vector2D> renderedPos;
    if (const auto W = WINDOW.lock(); W && LIFTED)
        renderedPos = W->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) + W->presentation().floatingOffset();

    // clear our visuals before Hyprland finishes the drag (it also clears the offset and focuses)
    resetState();

    const bool RET = (*(origDragEnd)g_hkDragEnd->m_original)(thisptr);

    if (DO_DROP) {
        // performDrop reads these
        g_drag.window   = WINDOW;
        g_drag.target   = TARGET;
        g_drag.srcWidth = WIDTH;
        performDrop(DROP);
        g_drag.window.reset();
        g_drag.target.reset();
    }

    // land: fly from the cursor to wherever the layout put it (also back home on cancel/restore), and grow back
    if (const auto W = WINDOW.lock(); W && validMapped(W) && renderedPos) {
        const auto GOAL = W->position(Desktop::View::IGeometric::GEOMETRIC_GOAL);
        W->positionAnimation()->setValueAndWarp(*renderedPos);
        W->move(GOAL);
        g_pHyprRenderer->damageWindow(W, true);
    }
    if (const auto W = WINDOW.lock(); W) {
        if (auto* S = scalerFor(W); S)
            S->setTarget(1.0);
    }

    return RET;
}

static bool hkShouldRenderWindow(Render::IHyprRenderer* thisptr, PHLWINDOW window, PHLMONITOR monitor) {
    // hot path (every window x monitor x frame): no extra work at all unless we are dragging
    if (!g_drag.lifted)
        return (*(origShouldRenderWindow)g_hkShouldRenderWm->m_original)(thisptr, window, monitor);

    CPerfScope perf{PERF_SHOULDRENDER};
    const bool ORIG = (*(origShouldRenderWindow)g_hkShouldRenderWm->m_original)(thisptr, window, monitor);
    if (ORIG || !g_drag.active || !window || !monitor || window != g_drag.window.lock())
        return ORIG;

    // the dragged (tiled) window may be drawn over another monitor
    CBox box = window->getFullWindowBoundingBox();
    box.translate(window->presentation().floatingOffset());
    return !box.intersection(CBox{monitor->m_position, monitor->m_size}).empty();
}

// ---------------------------------------------------------------- render

static void renderGap(const Event::SRenderStageEvent& ev) {
    const auto MON = ev.monitor;
    if (!MON || !g_drag.gapShown)
        return;
    const CBox SLOT = *g_drag.gapShown;
    if (SLOT.intersection(CBox{MON->m_position, MON->m_size}).empty())
        return;

    static auto PROUND = CConfigValue<Config::INTEGER>("decoration:rounding");
    const float SCALE  = MON->m_scale;
    const auto  COLOR  = barColor();
    const auto  local  = [&](const CBox& b) { return b.copy().translate(-MON->m_position).scale(SCALE).round(); };
    const auto  add    = [&](const CBox& b, const CHyprColor& c, int round) {
        CRectPassElement::SRectData d;
        d.box   = local(b);
        d.color = c;
        d.round = round;
        Render::IHyprRenderer::addPassElement(ev.context->get(), makeUnique<CRectPassElement>(d));
    };

    const int ROUND = std::round(*PROUND * SCALE);
    add(SLOT, CHyprColor{sc<float>(COLOR.r), sc<float>(COLOR.g), sc<float>(COLOR.b), 0.07F}, ROUND);

    // dashed outline (straight edges only, the corners stay open)
    const double     T    = 2.0, DASH = 14.0, GAP = 9.0;
    const double     R    = *PROUND;
    const CHyprColor DCOL = {sc<float>(COLOR.r), sc<float>(COLOR.g), sc<float>(COLOR.b), 0.55F};
    for (double x = SLOT.x + R; x < SLOT.x + SLOT.w - R; x += DASH + GAP) {
        const double w = std::min(DASH, SLOT.x + SLOT.w - R - x);
        add({x, SLOT.y, w, T}, DCOL, 0);
        add({x, SLOT.y + SLOT.h - T, w, T}, DCOL, 0);
    }
    for (double y = SLOT.y + R; y < SLOT.y + SLOT.h - R; y += DASH + GAP) {
        const double h = std::min(DASH, SLOT.y + SLOT.h - R - y);
        add({SLOT.x, y, T, h}, DCOL, 0);
        add({SLOT.x + SLOT.w - T, y, T, h}, DCOL, 0);
    }
}

// breathing phase 0..1 shared by the bar glow and the neighbour glow, plus the grow-in alpha
static double pulsePhase() {
    if (!g_cfg.barAnimation->value())
        return 0.5;
    const double T = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_drag.liftTime).count();
    return 0.5 - 0.5 * std::cos(2.0 * M_PI * T / 1.6);
}

static double appearAlpha() {
    if (!g_cfg.barAnimation->value())
        return 1.0;
    const double APP = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_drag.barAppear).count();
    const double DUR = std::max<Config::INTEGER>(g_cfg.barGrowMs->value(), 1) / 1000.0;
    return std::clamp(APP / DUR * 3.0, 0.0, 1.0);
}

// Soft light around the insert bar, drawn with Hyprland's own (gaussian-like) shadow shader in the accent
// colour. It is emitted right before the dragged window renders, so it falls onto the neighbour windows
// but stays below the dragged window.
class CHaloPassElement : public IPassElement {
  public:
    CHaloPassElement(const CBox& pixelBox, const CBox& logicalBox, int round, int range, const CHyprColor& color, float a) :
        m_box(pixelBox), m_logical(logicalBox), m_round(round), m_range(range), m_color(color), m_a(a) {
        ;
    }
    virtual std::vector<UP<IPassElement>> draw(Render::CRenderContext& ctx) {
        g_pHyprRenderer->blend(true);
        g_pHyprRenderer->drawShadow(ctx, m_box, m_round, 2.F, m_range, Config::CGradientValueData{m_color}, m_a, Render::SWindowRenderPresentation{});
        return {};
    }
    virtual bool needsLiveBlur(Render::CRenderContext&) {
        return false;
    }
    virtual bool needsPrecomputeBlur(Render::CRenderContext&) {
        return false;
    }
    virtual const char* passName() {
        return "CHyprdropperHalo";
    }
    virtual ePassElementType type() {
        return EK_CUSTOM;
    }
    virtual std::optional<CBox> boundingBox(Render::CRenderContext&) {
        return m_logical;
    }

  private:
    CBox       m_box, m_logical;
    int        m_round = 0, m_range = 0;
    CHyprColor m_color;
    float      m_a = 1.F;
};

static constexpr double HALO_RANGE = 42.0; // logical px

// per monitor: halo already emitted in the current frame (the dragged window renders twice: main + popup pass)
static std::vector<PHLMONITORREF> g_haloThisFrame;

// the bar as drawn right now: grows from its centre along its length on a new target (easeOutCubic, no overshoot)
static CBox barVisualBox() {
    CBox box = *g_drag.barShown;
    if (!g_cfg.barAnimation->value())
        return box;
    const double APP = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_drag.barAppear).count();
    const double DUR = std::max<Config::INTEGER>(g_cfg.barGrowMs->value(), 1) / 1000.0;
    const double P   = std::clamp(APP / DUR, 0.0, 1.0);
    const double K   = 1.0 - std::pow(1.0 - P, 3);
    if (box.h >= box.w) {
        const double H = box.h * K;
        box.y += (box.h - H) / 2.0;
        box.h = H;
    } else {
        const double W = box.w * K;
        box.x += (box.w - W) / 2.0;
        box.w = W;
    }
    return box;
}

static void emitHalo(const Event::SRenderStageEvent& ev) {
    const auto MON = ev.monitor;
    if (!MON || !g_drag.barShown || MON != g_drag.barMonitor.lock())
        return;
    if (std::ranges::find(g_haloThisFrame, PHLMONITORREF{MON}) != g_haloThisFrame.end())
        return;
    g_haloThisFrame.emplace_back(MON);

    const double PHASE = pulsePhase();
    const double ALPHA = appearAlpha();
    const auto   COLOR = barColor();
    const float  SCALE = MON->m_scale;

    // Hyprland's shadow shader takes the object box grown by the range and fades from its edge inwards
    const CBox   OBJECT  = barVisualBox().translate(-MON->m_position);
    if (OBJECT.w < 1 || OBJECT.h < 1)
        return;
    const CBox   LOGICAL = OBJECT.copy().expand(HALO_RANGE);
    const CBox   PIXEL   = LOGICAL.copy().scale(SCALE).round();
    const int    ROUND   = std::round(std::min(OBJECT.w, OBJECT.h) / 2.0 * SCALE);
    const float  A       = sc<float>(std::clamp((0.27 + 0.24 * PHASE) * g_cfg.glowStrength->value(), 0.0, 1.0) * ALPHA);

    // grows in together with the bar
    Render::IHyprRenderer::addPassElement(ev.context->get(),
                                          makeUnique<CHaloPassElement>(PIXEL, LOGICAL.copy().expand(2), ROUND, sc<int>(std::round(HALO_RANGE * SCALE)),
                                                                       CHyprColor{sc<float>(COLOR.r), sc<float>(COLOR.g), sc<float>(COLOR.b), 1.F}, A));
}

// preferred spot: right before the dragged window renders (on the neighbours, below the dragged window)
static void renderHaloBeforeDragged(const Event::SRenderStageEvent& ev) {
    const auto W = ev.context->get().m_data.currentWindow.lock();
    if (W && W == g_drag.window.lock())
        emitHalo(ev);
}

// per monitor: neighbours already lit in the current frame (renderWindow runs once per pass)
static std::vector<std::pair<PHLMONITORREF, std::vector<PHLWINDOWREF>>> g_litThisFrame;

static void renderNeighborGlow(const Event::SRenderStageEvent& ev) {
    const auto MON = ev.monitor;
    const auto W   = ev.context->get().m_data.currentWindow.lock();
    if (!MON || !W)
        return;

    const auto IT = std::ranges::find_if(g_drag.neighborsShown, [&](const auto& n) { return n.first.lock() == W; });
    if (IT == g_drag.neighborsShown.end())
        return;

    auto FRAME = std::ranges::find_if(g_litThisFrame, [&](const auto& e) { return e.first.lock() == MON; });
    if (FRAME == g_litThisFrame.end()) {
        g_litThisFrame.emplace_back(PHLMONITORREF{MON}, std::vector<PHLWINDOWREF>{});
        FRAME = g_litThisFrame.end() - 1;
    }
    if (std::ranges::find(FRAME->second, PHLWINDOWREF{W}) != FRAME->second.end())
        return;
    FRAME->second.emplace_back(W);

    const double PHASE = pulsePhase();
    const double ALPHA = appearAlpha();
    const auto   COLOR = barColor();
    const float  SCALE = MON->m_scale;

    const CBox   GLOBAL = CBox{W->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT), W->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)};
    const CBox   LOCAL  = GLOBAL.copy().translate(-MON->m_position).scale(SCALE).round();
    if (LOCAL.w < 1 || LOCAL.h < 1)
        return;

    const double ROUNDING = W->presentation().rounding();

    // accent border, same shape as the window's own border (the soft light comes from the bar's halo)
    const int    BORDER     = W->presentation().borderSize();
    if (BORDER > 0) {
        const auto   ROUNDINGPOWER    = W->presentation().roundingPower();
        const auto   CORRECTIONOFFSET = (BORDER * (M_SQRT2 - 1) * std::max(2.0 - ROUNDINGPOWER, 0.0));
        CBorderPassElement::SBorderData data;
        data.box           = LOCAL;
        data.grad1         = Config::CGradientValueData{CHyprColor{sc<float>(COLOR.r), sc<float>(COLOR.g), sc<float>(COLOR.b), 1.F}};
        data.round         = ROUNDING * SCALE;
        data.outerRound    = ((ROUNDING + BORDER) - CORRECTIONOFFSET) * SCALE;
        data.roundingPower = ROUNDINGPOWER;
        data.a             = sc<float>((0.55 + 0.45 * PHASE) * ALPHA);
        data.borderSize    = BORDER;
        data.window        = W;
        Render::IHyprRenderer::addPassElement(ev.context->get(), makeUnique<CBorderPassElement>(data));
    }
}

static void onRenderStage(const Event::SRenderStageEvent& ev) {
    if (!g_drag.active)
        return;
    CPerfScope perf{PERF_RENDER};
    if (ev.stage == RENDER_BEGIN) {
        std::erase_if(g_litThisFrame, [&](const auto& e) { return e.first.expired() || e.first.lock() == ev.monitor; });
        std::erase_if(g_haloThisFrame, [&](const auto& m) { return m.expired() || m.lock() == ev.monitor; });
        return;
    }
    if (ev.stage == RENDER_PRE_WINDOW && ev.context && g_drag.lifted) {
        renderHaloBeforeDragged(ev);
        return;
    }
    if (ev.stage == RENDER_POST_WINDOW && ev.context && g_drag.lifted && g_cfg.neighborGlow->value()) {
        renderNeighborGlow(ev);
        return;
    }
    if (ev.stage == RENDER_POST_WALLPAPER && ev.context && g_drag.active && g_drag.lifted) {
        renderGap(ev);
        return;
    }
    if (ev.stage != RENDER_POST_WINDOWS || !ev.context || !g_drag.active || !g_drag.barShown)
        return;
    const auto MON = g_drag.barMonitor.lock();
    if (!MON || MON != ev.monitor)
        return;

    const float SCALE = MON->m_scale;
    auto        color = barColor();

    const double alpha   = appearAlpha();
    CBox         logical = barVisualBox();
    auto local = logical.translate(-MON->m_position).scale(SCALE).round();
    if (local.w < 1 || local.h < 1)
        return;

    // soft light around the bar; normally already emitted below the dragged window, this is the fallback
    emitHalo(ev);

    {
        CRectPassElement::SRectData bar;
        bar.box   = local;
        bar.color = CHyprColor{sc<float>(color.r), sc<float>(color.g), sc<float>(color.b), sc<float>(color.a * alpha)};
        bar.round = std::round(std::min(local.w, local.h) / 2.0);
        Render::IHyprRenderer::addPassElement(ev.context->get(), makeUnique<CRectPassElement>(bar));
    }
}

// ---------------------------------------------------------------- input

static void onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) {
    if (e.keycode != KEY_ESC)
        return;

    if (e.state == WL_KEYBOARD_KEY_STATE_RELEASED && g_drag.swallowEscRelease) {
        g_drag.swallowEscRelease = false;
        info.cancelled           = true;
        return;
    }

    if (!g_drag.active || e.state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return;

    info.cancelled           = true;
    g_drag.swallowEscRelease = true;
    endOurDrag(true);
}

static void onAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
    if (!g_drag.active || e.delta == 0)
        return;

    info.cancelled = true; // never let the wheel switch workspaces mid-drag

    const auto MON = monitorAt(g_pInputManager->getMouseCoordsInternal());
    const auto WS  = activeWorkspaceOf(MON);
    auto*      ALGO = scrollingOf(WS);
    if (!ALGO)
        return;

    const auto   USABLE  = ALGO->usableArea();
    const double PRIMARY = ALGO->m_scrollingData->controller->isPrimaryHorizontal() ? USABLE.w : USABLE.h;
    const double STEP    = PRIMARY * std::clamp<double>(g_cfg.wheelStep->value(), 0.01, 1.0);
    scrollTape(WS, (e.delta > 0 ? 1.0 : -1.0) * STEP, false);

    if (g_drag.lifted) {
        updateWindowOffset();
        computeDrop(g_pInputManager->getMouseCoordsInternal());
        updateBar();
    }
}

// ---------------------------------------------------------------- version gate

static std::string minorOf(std::string tag) {
    if (!tag.empty() && tag[0] == 'v')
        tag.erase(0, 1);
    const auto D1 = tag.find('.');
    if (D1 == std::string::npos)
        return tag;
    const auto D2 = tag.find('.', D1 + 1);
    return D2 == std::string::npos ? tag : tag.substr(0, D2);
}

static std::string approvedFile() {
    const char* XDG  = getenv("XDG_CONFIG_HOME");
    const char* HOME = getenv("HOME");
    std::string base = XDG && *XDG ? XDG : std::string(HOME ? HOME : "") + "/.config";
    return base + "/hyprdropper/approved-versions";
}

// Hyprland minor versions this build has been tested against (integration suite in tests/headless).
// Anything else stays inactive until approved in approvedFile() - plugin internals change between minors.
static const std::vector<std::string> TESTED_MINORS = {"0.57"};

static bool versionApproved(const std::string& minor) {
    if (getenv("HYPRDROPPER_SKIP_VERSION_GATE"))
        return true;
    if (!getenv("HYPRDROPPER_IGNORE_BUILTIN_VERSIONS") && std::ranges::find(TESTED_MINORS, minor) != TESTED_MINORS.end())
        return true;
    std::ifstream f(approvedFile());
    std::string   line;
    while (std::getline(f, line)) {
        line.erase(std::remove_if(line.begin(), line.end(), ::isspace), line.end());
        if (!line.empty() && line[0] != '#' && line == minor)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------- hyprctl status

static std::string statusString() {
    std::string s = std::format("hooks: {}\nenabled: {}\nactive: {}\nlifted: {}\nlast_reject: {}\nbegin_calls: {}\n", g_hooksActive, g_cfg.enabled ? g_cfg.enabled->value() : false, g_drag.active,
                                g_drag.lifted, g_lastReject, g_beginCalls);
    s += std::format("drop: {}\n", g_drag.drop.kind == DropLogic::KIND_RESTORE ? "restore" : (g_drag.drop.kind == DropLogic::KIND_NEW_COLUMN ? "new_column" : "into_column"));
    if (g_drag.barShown)
        s += std::format("bar_shown: {:.0f} {:.0f} {:.0f} {:.0f}\n", g_drag.barShown->x, g_drag.barShown->y, g_drag.barShown->w, g_drag.barShown->h);
    if (g_drag.barGoal)
        s += std::format("bar: {} {} {} {}\n", g_drag.barGoal->x, g_drag.barGoal->y, g_drag.barGoal->w, g_drag.barGoal->h);
    else
        s += "bar: none\n";
    if (const auto W = g_drag.window.lock(); W) {
        s += std::format("offset: {} {}\n", W->presentation().floatingOffset().x, W->presentation().floatingOffset().y);
        if (auto* S = scalerFor(W); S)
            s += std::format("scale: {:.3f} -> {:.3f}\n", S->m_scale, S->m_target);
    }
    s += std::format("scalers: {}\n", g_scalers.size());
    s += std::format("neighbors: {}\n", g_drag.neighborsShown.size());
    if (g_hooksActive) {
        const auto C = barColor();
        s += std::format("bar_color: {} {:02x}{:02x}{:02x}\n", useThemeColor() ? "theme" : "own", sc<int>(C.r * 255), sc<int>(C.g * 255), sc<int>(C.b * 255));
    }
    for (int i = 0; i < PERF_LAST; ++i) {
        const auto& p = g_perf[i];
        s += std::format("perf {}: calls {} avg {:.2f}us max {:.1f}us total {:.0f}us\n", PERF_NAMES[i], p.calls, p.calls ? p.total / p.calls : 0.0, p.max, p.total);
    }
    s += std::format("gap: {}\n", g_drag.gapShown ? std::format("{} {} {} {}", g_drag.gapShown->x, g_drag.gapShown->y, g_drag.gapShown->w, g_drag.gapShown->h) : "none");
    return s;
}

// ---------------------------------------------------------------- init / exit

static CFunctionHook* hookByName(const std::string& name, const std::string& mustContain, void* dst) {
    for (const auto& fn : HyprlandAPI::findFunctionsByName(PHANDLE, name)) {
        if (!fn.demangled.contains(mustContain))
            continue;
        auto* hk = HyprlandAPI::createFunctionHook(PHANDLE, fn.address, dst);
        return hk;
    }
    return nullptr;
}

static SP<HOOK_CALLBACK_FN>                 g_dummy;
static std::vector<CHyprSignalListener>     g_listeners;
static SP<IPC::Socket1::SCommand>           g_ctlCommand;
static SP<IPC::Socket1::SCommand>           g_ctlPerfReset;

static void notify(const std::string& text, const CHyprColor& col, float ms) {
    HyprlandAPI::addNotification(PHANDLE, "[hyprdropper] " + text, col, ms);
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const PLUGIN_DESCRIPTION_INFO INFO = {"hyprdropper", "niri-like drag & drop with insert preview for the scrolling layout", "oblivion8282-1337", "1.0"};

    if (std::string{__hyprland_api_get_hash()} != std::string{__hyprland_api_get_client_hash()}) {
        notify("Version mismatch: built for different Hyprland headers. Rebuild needed. Plugin inactive.", CHyprColor{1.0, 0.3, 0.3, 1.0}, 10000);
        throw std::runtime_error("[hyprdropper] version mismatch");
    }

    g_cfg.enabled   = makeShared<Config::Values::CBoolValue>("plugin:hyprdropper:enabled", "enable niri-like drag preview in the scrolling layout", true);
    g_cfg.color         = makeShared<Config::Values::CColorValue>("plugin:hyprdropper:col.bar", "insert bar colour (used when not following the theme)", 0xFF42A5F5);
    g_cfg.useThemeColor = makeShared<Config::Values::CBoolValue>("plugin:hyprdropper:use_theme_color",
                                                                 "use the theme accent (decoration:glow:color) instead of col.bar; default: yes unless col.bar is set", true);
    g_cfg.barWidth  = makeShared<Config::Values::CIntValue>("plugin:hyprdropper:bar_width", "insert bar thickness (logical px)", 6, Config::Values::SIntValueOptions{.min = 1, .max = 64});
    g_cfg.edgeSize  = makeShared<Config::Values::CIntValue>("plugin:hyprdropper:edge_size", "edge-scroll zone width (logical px)", 48, Config::Values::SIntValueOptions{.min = 1, .max = 500});
    g_cfg.edgeSpeed = makeShared<Config::Values::CFloatValue>("plugin:hyprdropper:edge_speed", "max edge-scroll speed (logical px/s)", 1800.F, Config::Values::SFloatValueOptions{.min = 0.F, .max = 20000.F});
    g_cfg.wheelStep = makeShared<Config::Values::CFloatValue>("plugin:hyprdropper:wheel_step", "scroll per wheel step while dragging, fraction of the view", 0.25F,
                                                              Config::Values::SFloatValueOptions{.min = 0.01F, .max = 1.F});
    g_cfg.dragOpacity = makeShared<Config::Values::CFloatValue>("plugin:hyprdropper:drag_opacity", "opacity of the dragged window (1 = opaque)", 0.8F,
                                                                Config::Values::SFloatValueOptions{.min = 0.1F, .max = 1.F});
    g_cfg.dragScale = makeShared<Config::Values::CFloatValue>("plugin:hyprdropper:drag_scale", "size of the dragged window while dragging (1 = unchanged)", 0.6F,
                                                              Config::Values::SFloatValueOptions{.min = 0.2F, .max = 1.F});
    g_cfg.showGap   = makeShared<Config::Values::CBoolValue>("plugin:hyprdropper:show_gap", "outline the dragged window's old slot", true);
    g_cfg.barAnimation = makeShared<Config::Values::CBoolValue>("plugin:hyprdropper:bar_animation", "grow-in and breathing glow for the insert bar", true);
    g_cfg.glowStrength = makeShared<Config::Values::CFloatValue>("plugin:hyprdropper:glow_strength", "strength of the soft light around the insert bar (1 = default)", 1.F,
                                                                 Config::Values::SFloatValueOptions{.min = 0.F, .max = 3.F});
    g_cfg.neighborGlow = makeShared<Config::Values::CBoolValue>("plugin:hyprdropper:neighbor_glow", "light up the windows next to the insert bar", true);
    g_cfg.barGrowMs    = makeShared<Config::Values::CIntValue>("plugin:hyprdropper:bar_grow_ms", "duration of the bar's grow-in (ms)", 500,
                                                               Config::Values::SIntValueOptions{.min = 1, .max = 3000});
    for (const auto& v : std::vector<SP<Config::Values::IValue>>{g_cfg.enabled, g_cfg.color, g_cfg.useThemeColor, g_cfg.barWidth, g_cfg.edgeSize, g_cfg.edgeSpeed, g_cfg.wheelStep, g_cfg.dragOpacity,
                                                                 g_cfg.dragScale, g_cfg.showGap, g_cfg.barAnimation, g_cfg.barGrowMs, g_cfg.neighborGlow, g_cfg.glowStrength})
        HyprlandAPI::addConfigValueV2(PHANDLE, v);

    g_ctlCommand = HyprlandAPI::registerHyprCtlCommand(PHANDLE, IPC::Socket1::SCommand{.name = "hyprdropper", .match = IPC::Socket1::COMMAND_MATCH_EXACT,
                                                                                         .handler = [](const IPC::Socket1::SRequest&) { return IPC::Socket1::SResponse{statusString()}; }});

    g_ctlPerfReset = HyprlandAPI::registerHyprCtlCommand(PHANDLE, IPC::Socket1::SCommand{.name = "hyprdropper-perfreset", .match = IPC::Socket1::COMMAND_MATCH_EXACT,
                                                                                           .handler = [](const IPC::Socket1::SRequest&) {
                                                                                               for (auto& p : g_perf)
                                                                                                   p = {};
                                                                                               return IPC::Socket1::SResponse{"ok"};
                                                                                           }});

    const auto VERSION = HyprlandAPI::getHyprlandVersion(PHANDLE);
    const auto MINOR   = minorOf(VERSION.tag);
    if (!versionApproved(MINOR)) {
        notify(std::format("Hyprland {} has not been tested with this plugin yet, so it stays inactive (Hyprland works normally). "
                           "Update the plugin, or approve the version yourself by adding \"{}\" to {}.",
                           MINOR, MINOR, approvedFile()),
               CHyprColor{1.0, 0.75, 0.2, 1.0}, 20000);
        return INFO;
    }

    g_hkBeginDrag      = hookByName("beginDragTarget", "CLayoutManager::beginDragTarget", (void*)&hkBeginDrag);
    g_hkMoveMouse      = hookByName("moveMouse", "CLayoutManager::moveMouse", (void*)&hkMoveMouse);
    g_hkUpdateDragWin  = hookByName("updateDragWindow", "CDragStateController::updateDragWindow", (void*)&hkUpdateDragWindow);
    g_hkDragEnd        = hookByName("dragEnd", "CDragStateController::dragEnd", (void*)&hkDragEnd);
    g_hkShouldRenderWm = hookByName("shouldRenderWindow", "CMonitor", (void*)&hkShouldRenderWindow);

    const bool FOUND = g_hkBeginDrag && g_hkMoveMouse && g_hkUpdateDragWin && g_hkDragEnd && g_hkShouldRenderWm;
    if (!FOUND) {
        notify("Could not find Hyprland functions to hook. Plugin inactive.", CHyprColor{1.0, 0.3, 0.3, 1.0}, 10000);
        return INFO;
    }

    const bool HOOKED = g_hkBeginDrag->hook() && g_hkMoveMouse->hook() && g_hkUpdateDragWin->hook() && g_hkDragEnd->hook() && g_hkShouldRenderWm->hook();
    if (!HOOKED) {
        for (auto* h : {g_hkBeginDrag, g_hkMoveMouse, g_hkUpdateDragWin, g_hkDragEnd, g_hkShouldRenderWm})
            h->unhook();
        notify("Hooking failed. Plugin inactive.", CHyprColor{1.0, 0.3, 0.3, 1.0}, 10000);
        return INFO;
    }

    g_listeners.push_back(Event::bus()->m_events.render.stage.listen([](const Event::SRenderStageEvent& ev) { onRenderStage(ev); }));
    g_listeners.push_back(Event::bus()->m_events.input.keyboard.key.listen([](const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) { onKey(e, info); }));
    g_listeners.push_back(Event::bus()->m_events.input.mouse.axis.listen([](const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) { onAxis(e, info); }));
    g_listeners.push_back(Event::bus()->m_events.window.destroy.listen([](PHLWINDOWREF w) {
        if (g_drag.active && w == g_drag.window)
            endOurDrag(true);
    }));
    g_listeners.push_back(Event::bus()->m_events.monitor.removed.listen([](PHLMONITOR) {
        if (g_drag.active)
            endOurDrag(true);
    }));

    g_hooksActive = true;
    return INFO;
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (g_drag.active)
        endOurDrag(true);
    resetState();
    removeAllScalers();
    g_hooksActive = false;
    g_listeners.clear();
    if (g_ctlCommand)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_ctlCommand);
    if (g_ctlPerfReset)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_ctlPerfReset);
}
