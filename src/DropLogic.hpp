#pragma once

// Pure drop-target logic, independent of Hyprland types so it can be unit-tested.
// All coordinates are global logical pixels.

#include <optional>
#include <vector>

namespace DropLogic {

    struct SRect {
        double x = 0, y = 0, w = 0, h = 0;

        double right() const {
            return x + w;
        }
        double bottom() const {
            return y + h;
        }
        double centerY() const {
            return y + h / 2.0;
        }
        bool containsX(double px) const {
            return px >= x && px < right();
        }
        bool contains(double px, double py) const {
            return containsX(px) && py >= y && py < bottom();
        }
        bool operator==(const SRect&) const = default;
    };

    struct SWindowInfo {
        SRect box;
        bool  dragged = false;
    };

    struct SColumnInfo {
        SRect                    box; // union of its windows' boxes
        std::vector<SWindowInfo> windows;
    };

    struct SInput {
        std::vector<SColumnInfo> columns;       // in tape order
        SRect                    area;          // usable area of the workspace (global)
        double                   px = 0, py = 0; // pointer
        bool                     sameWorkspace = true; // dragged window lives in this workspace
        double                   barThickness  = 6;
    };

    enum eKind {
        KIND_RESTORE = 0, // put it back where it was (no bar)
        KIND_NEW_COLUMN,  // new column inserted after column `insertAfterColumn` (-1 = at front)
        KIND_INTO_COLUMN, // into column `intoColumn`, after window `insertAfterWindow` (-1 = at top)
    };

    struct SResult {
        eKind                kind              = KIND_RESTORE;
        int                  insertAfterColumn = -1;
        int                  intoColumn        = -1;
        int                  insertAfterWindow = -1;
        std::optional<SRect> bar;
    };

    // Hyprland's own threshold (ScrollingAlgorithm::newTarget, RANGE_THRESHOLD)
    inline constexpr double EDGE_FRACTION = 0.3;

    SResult compute(const SInput& in);
}
