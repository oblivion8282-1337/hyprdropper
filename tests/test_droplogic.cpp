// Unit tests for the pure drop logic. Build & run: make test
#include "../src/DropLogic.hpp"

#include <cstdio>
#include <string>

using namespace DropLogic;

static int g_failed = 0, g_passed = 0;

#define CHECK(cond)                                                                                                                                                                \
    do {                                                                                                                                                                           \
        if (!(cond)) {                                                                                                                                                             \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                                                                         \
            g_failed++;                                                                                                                                                            \
        } else                                                                                                                                                                     \
            g_passed++;                                                                                                                                                            \
    } while (0)

// Area: 0..1920 x 0..1080, gaps of 10 between columns, columns 600 wide.
// col0: 10..610, col1: 620..1220, col2: 1230..1830
static SColumnInfo col(double x, double w, std::vector<SWindowInfo> wins = {}) {
    SColumnInfo c;
    c.box = {x, 10, w, 1060};
    if (wins.empty())
        wins.push_back({c.box, false});
    c.windows = wins;
    return c;
}

static SInput threeCols(int draggedCol = -1) {
    SInput in;
    in.area    = {0, 0, 1920, 1080};
    in.columns = {col(10, 600), col(620, 600), col(1230, 600)};
    if (draggedCol >= 0)
        in.columns[draggedCol].windows[0].dragged = true;
    return in;
}

static void test_right_third_makes_new_column_after() {
    std::puts("right third of a window -> new column after it");
    auto in = threeCols(0);
    in.px   = 620 + 600 * 0.9; // col1, right 10%
    in.py   = 500;
    auto r  = compute(in);
    CHECK(r.kind == KIND_NEW_COLUMN);
    CHECK(r.insertAfterColumn == 1);
    CHECK(r.bar.has_value());
    // vertical bar centred in the gap between col1 (ends 1220) and col2 (starts 1230)
    CHECK(r.bar->w == 6);
    CHECK(r.bar->x + r.bar->w / 2 == 1225);
}

static void test_left_third_makes_new_column_before() {
    std::puts("left third of a window -> new column before it");
    auto in = threeCols(0);
    in.px   = 1230 + 600 * 0.1; // col2 left 10%
    in.py   = 500;
    auto r  = compute(in);
    CHECK(r.kind == KIND_NEW_COLUMN);
    CHECK(r.insertAfterColumn == 1);
}

static void test_middle_goes_into_column() {
    std::puts("middle of a window -> into its column, above/below by half");
    auto in = threeCols(0);
    in.px   = 620 + 300;
    in.py   = 200; // upper half
    auto r  = compute(in);
    CHECK(r.kind == KIND_INTO_COLUMN);
    CHECK(r.intoColumn == 1);
    CHECK(r.insertAfterWindow == -1); // at top
    CHECK(r.bar.has_value());
    CHECK(r.bar->h == 6);
    CHECK(r.bar->w == 600);

    in.py = 900; // lower half
    r     = compute(in);
    CHECK(r.kind == KIND_INTO_COLUMN);
    CHECK(r.insertAfterWindow == 0);
}

static void test_gap_between_columns_is_seam() {
    std::puts("pointer in the gap between two columns -> new column at that seam (not at the end)");
    auto in = threeCols(2);
    in.px   = 615; // between col0 (ends 610) and col1 (starts 620)
    in.py   = 500;
    auto r  = compute(in);
    CHECK(r.kind == KIND_NEW_COLUMN);
    CHECK(r.insertAfterColumn == 0);
}

static void test_right_of_last_column_appends() {
    std::puts("right of the last column -> new column at the end");
    auto in = threeCols(0);
    in.px   = 1900;
    in.py   = 500;
    auto r  = compute(in);
    CHECK(r.kind == KIND_NEW_COLUMN);
    CHECK(r.insertAfterColumn == 2);
    CHECK(r.bar.has_value());
    CHECK(r.bar->right() <= 1920);
}

static void test_left_of_first_column_prepends() {
    std::puts("left of the first column -> new column at the front");
    auto in = threeCols(2);
    in.px   = 4;
    in.py   = 500;
    auto r  = compute(in);
    CHECK(r.kind == KIND_NEW_COLUMN);
    CHECK(r.insertAfterColumn == -1);
    CHECK(r.bar.has_value());
    CHECK(r.bar->x >= 0);
}

static void test_own_slot_restores() {
    std::puts("pointer over the dragged window's own slot -> restore, no bar");
    auto in = threeCols(1);
    in.px   = 620 + 300;
    in.py   = 500;
    auto r  = compute(in);
    CHECK(r.kind == KIND_RESTORE);
    CHECK(!r.bar.has_value());
}

static void test_seams_next_to_own_solo_column_restore() {
    std::puts("seams directly next to the dragged window's solo column -> restore");
    auto in = threeCols(1);
    // right third of col0 == seam between col0 and col1 (own) -> same place
    in.px  = 10 + 600 * 0.9;
    in.py  = 500;
    auto r = compute(in);
    CHECK(r.kind == KIND_RESTORE);
    // left third of col2 == seam between col1 (own) and col2
    in.px = 1230 + 600 * 0.1;
    r     = compute(in);
    CHECK(r.kind == KIND_RESTORE);
    // gap between col1 and col2
    in.px = 1225;
    r     = compute(in);
    CHECK(r.kind == KIND_RESTORE);
    // but one further is a real move
    in.px = 1230 + 600 * 0.9;
    r     = compute(in);
    CHECK(r.kind == KIND_NEW_COLUMN);
    CHECK(r.insertAfterColumn == 2);
}

static void test_other_workspace_never_restores() {
    std::puts("on another workspace there is no restore rule");
    auto in          = threeCols(-1);
    in.sameWorkspace = false;
    in.px            = 10 + 600 * 0.9;
    in.py            = 500;
    auto r           = compute(in);
    CHECK(r.kind == KIND_NEW_COLUMN);
    CHECK(r.insertAfterColumn == 0);
}

static void test_empty_workspace() {
    std::puts("empty workspace -> first column, bar at the left edge");
    SInput in;
    in.area          = {1920, 0, 1920, 1080};
    in.sameWorkspace = false;
    in.px            = 2500;
    in.py            = 500;
    auto r           = compute(in);
    CHECK(r.kind == KIND_NEW_COLUMN);
    CHECK(r.insertAfterColumn == -1);
    CHECK(r.bar.has_value());
    CHECK(r.bar->x == 1920);
    CHECK(r.bar->h == 1080);
}

static void test_multi_window_column_restore_neighbours() {
    std::puts("dragged window inside a multi-window column: neighbours' adjacent halves restore");
    SInput in;
    in.area = {0, 0, 1920, 1080};
    // col0 has 3 windows stacked: A (10..350), D dragged (360..700), B (710..1070)
    SColumnInfo c;
    c.box     = {10, 10, 600, 1060};
    c.windows = {{{10, 10, 600, 340}, false}, {{10, 360, 600, 340}, true}, {{10, 710, 600, 360}, false}};
    in.columns = {c, col(620, 600)};
    in.px      = 300;

    in.py  = 300; // A lower half -> after A == D's own position
    auto r = compute(in);
    CHECK(r.kind == KIND_RESTORE);

    in.py = 750; // B upper half -> before B == D's own position
    r     = compute(in);
    CHECK(r.kind == KIND_RESTORE);

    in.py = 100; // A upper half -> top, a real move
    r     = compute(in);
    CHECK(r.kind == KIND_INTO_COLUMN);
    CHECK(r.intoColumn == 0);
    CHECK(r.insertAfterWindow == -1);

    in.py = 1000; // B lower half -> bottom
    r     = compute(in);
    CHECK(r.kind == KIND_INTO_COLUMN);
    CHECK(r.insertAfterWindow == 2);

    // dragging out of a multi-window column into its own side seam is a real move (new column)
    in.px = 10 + 600 * 0.9;
    in.py = 100;
    r     = compute(in);
    CHECK(r.kind == KIND_NEW_COLUMN);
    CHECK(r.insertAfterColumn == 0);
}

static void test_vertical_gap_inside_column_picks_nearest() {
    std::puts("pointer in the vertical gap between two windows of a column -> between them");
    SInput in;
    in.area = {0, 0, 1920, 1080};
    SColumnInfo c;
    c.box      = {10, 10, 600, 1060};
    c.windows  = {{{10, 10, 600, 520}, false}, {{10, 540, 600, 530}, false}};
    in.columns = {c, col(620, 600, {{{620, 10, 600, 1060}, true}})};
    in.px      = 300;
    in.py      = 535;
    auto r     = compute(in);
    CHECK(r.kind == KIND_INTO_COLUMN);
    CHECK(r.insertAfterWindow == 0);
    CHECK(r.bar.has_value());
    CHECK(r.bar->y + r.bar->h / 2 == 535);
}

int main() {
    test_right_third_makes_new_column_after();
    test_left_third_makes_new_column_before();
    test_middle_goes_into_column();
    test_gap_between_columns_is_seam();
    test_right_of_last_column_appends();
    test_left_of_first_column_prepends();
    test_own_slot_restores();
    test_seams_next_to_own_solo_column_restore();
    test_other_workspace_never_restores();
    test_empty_workspace();
    test_multi_window_column_restore_neighbours();
    test_vertical_gap_inside_column_picks_nearest();

    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
