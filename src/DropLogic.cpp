#include "DropLogic.hpp"

#include <algorithm>
#include <limits>

namespace DropLogic {

    // half of the distance we keep from a window edge when there is no neighbour to centre between
    static constexpr double EDGE_INSET = 4.0;

    static int sc(size_t v) {
        return static_cast<int>(v);
    }

    static int draggedWindowIndex(const SColumnInfo& c) {
        for (size_t i = 0; i < c.windows.size(); ++i) {
            if (c.windows[i].dragged)
                return sc(i);
        }
        return -1;
    }

    static int draggedColumnIndex(const SInput& in) {
        for (size_t i = 0; i < in.columns.size(); ++i) {
            if (draggedWindowIndex(in.columns[i]) >= 0)
                return sc(i);
        }
        return -1;
    }

    static SRect verticalBar(const SInput& in, int insertAfter) {
        const double T = in.barThickness;
        const auto*  L = insertAfter >= 0 && insertAfter < sc(in.columns.size()) ? &in.columns[insertAfter] : nullptr;
        const auto*  R = insertAfter + 1 >= 0 && insertAfter + 1 < sc(in.columns.size()) ? &in.columns[insertAfter + 1] : nullptr;

        double       cx = in.area.x + T / 2.0;
        double       top = in.area.y, bottom = in.area.bottom();

        if (L && R) {
            cx     = (L->box.right() + R->box.x) / 2.0;
            top    = std::min(L->box.y, R->box.y);
            bottom = std::max(L->box.bottom(), R->box.bottom());
        } else if (L) {
            cx     = L->box.right() + EDGE_INSET;
            top    = L->box.y;
            bottom = L->box.bottom();
        } else if (R) {
            cx     = R->box.x - EDGE_INSET;
            top    = R->box.y;
            bottom = R->box.bottom();
        }

        double x = cx - T / 2.0;
        // keep the bar on the workspace's usable area so it is always visible
        x = std::clamp(x, in.area.x, std::max(in.area.x, in.area.right() - T));
        if (!L && !R)
            x = in.area.x;

        return {x, top, T, bottom - top};
    }

    static SRect horizontalBar(const SInput& in, const SColumnInfo& c, int insertAfterWindow) {
        const double T     = in.barThickness;
        const auto*  ABOVE = insertAfterWindow >= 0 && insertAfterWindow < sc(c.windows.size()) ? &c.windows[insertAfterWindow] : nullptr;
        const auto*  BELOW = insertAfterWindow + 1 < sc(c.windows.size()) ? &c.windows[insertAfterWindow + 1] : nullptr;

        double       cy = c.box.y;
        if (ABOVE && BELOW)
            cy = (ABOVE->box.bottom() + BELOW->box.y) / 2.0;
        else if (ABOVE)
            cy = ABOVE->box.bottom() + EDGE_INSET;
        else if (BELOW)
            cy = BELOW->box.y - EDGE_INSET;

        double y = std::clamp(cy - T / 2.0, in.area.y, std::max(in.area.y, in.area.bottom() - T));
        return {c.box.x, y, c.box.w, T};
    }

    static SResult newColumn(int insertAfter) {
        SResult r;
        r.kind              = KIND_NEW_COLUMN;
        r.insertAfterColumn = insertAfter;
        return r;
    }

    // Candidate result before the "would it land where it already is" check.
    static SResult rawResult(const SInput& in) {
        const auto& cols = in.columns;

        if (cols.empty())
            return newColumn(-1);

        // 1. pointer inside a column's horizontal extent
        for (size_t ci = 0; ci < cols.size(); ++ci) {
            const auto& C = cols[ci];
            if (!C.box.containsX(in.px))
                continue;

            for (const auto& w : C.windows) {
                if (w.dragged && w.box.contains(in.px, in.py))
                    return {}; // own slot
            }

            const double rel = C.box.w > 0 ? (in.px - C.box.x) / C.box.w : 0.5;
            if (rel <= EDGE_FRACTION)
                return newColumn(sc(ci) - 1);
            if (rel >= 1.0 - EDGE_FRACTION)
                return newColumn(sc(ci));

            // middle: into this column, relative to the nearest non-dragged window
            int    best     = -1;
            double bestDist = std::numeric_limits<double>::max();
            for (size_t wi = 0; wi < C.windows.size(); ++wi) {
                const auto& w = C.windows[wi];
                if (w.dragged)
                    continue;
                const double d = in.py < w.box.y ? w.box.y - in.py : (in.py >= w.box.bottom() ? in.py - w.box.bottom() : 0.0);
                if (d < bestDist) {
                    bestDist = d;
                    best     = sc(wi);
                }
            }

            if (best < 0)
                return {}; // column only holds the dragged window

            const auto& W     = C.windows[best];
            const bool  ABOVE = in.py < W.box.centerY();

            SResult     r;
            r.kind              = KIND_INTO_COLUMN;
            r.intoColumn        = sc(ci);
            r.insertAfterWindow = ABOVE ? best - 1 : best;
            return r;
        }

        // 2. pointer in a seam or beyond the ends: nearest column to the left decides
        int left = -1;
        for (size_t ci = 0; ci < cols.size(); ++ci) {
            if (cols[ci].box.right() <= in.px)
                left = sc(ci);
        }

        return newColumn(left);
    }

    SResult compute(const SInput& in) {
        SResult r = rawResult(in);

        if (in.sameWorkspace && r.kind != KIND_RESTORE) {
            const int DCOL = draggedColumnIndex(in);

            if (DCOL >= 0) {
                const auto& D    = in.columns[DCOL];
                const int   DWIN = draggedWindowIndex(D);
                const bool  SOLO = D.windows.size() == 1;

                // a new column right next to its own (solo) column is the same place
                if (r.kind == KIND_NEW_COLUMN && SOLO && (r.insertAfterColumn == DCOL - 1 || r.insertAfterColumn == DCOL))
                    r = {};

                // inserting directly above or below itself in its own column is the same place
                if (r.kind == KIND_INTO_COLUMN && r.intoColumn == DCOL && (r.insertAfterWindow == DWIN - 1 || r.insertAfterWindow == DWIN))
                    r = {};
            }
        }

        if (r.kind == KIND_NEW_COLUMN)
            r.bar = verticalBar(in, r.insertAfterColumn);
        else if (r.kind == KIND_INTO_COLUMN)
            r.bar = horizontalBar(in, in.columns[r.intoColumn], r.insertAfterWindow);

        return r;
    }
}
