// Unit tests for genie_math.hpp (no Hyprland needed):  make test
#include "genie_math.hpp"
#include <cstdio>
#include <cmath>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

int main() {
    const genie::Rect win{400, 150, 900, 600}, icon{940, 1010, 48, 48};
    const int ROWS = 200;

    // p = 0: the window is drawn exactly where it is, row by row
    auto s = genie::slices(win, icon, 0.0, ROWS);
    CHECK((int)s.size() == ROWS, "rows %zu", s.size());
    CHECK(near(s.front().dst.y, win.y) && near(s.back().dst.y + s.back().dst.h, win.y + win.h), "p=0 not at rest");
    for (const auto& r : s)
        CHECK(near(r.dst.x, win.x) && near(r.dst.w, win.w), "p=0 row bent: x=%f w=%f", r.dst.x, r.dst.w);

    // p = 1: everything is inside the icon
    s = genie::slices(win, icon, 1.0, ROWS);
    for (const auto& r : s) {
        CHECK(r.dst.x >= icon.x - 1e-6 && r.dst.x + r.dst.w <= icon.x + icon.w + 1e-6, "p=1 row outside icon (x)");
        CHECK(r.dst.y >= icon.y - 1e-6 && r.dst.y + r.dst.h <= icon.y + icon.h + 1e-6, "p=1 row outside icon (y)");
    }

    // every frame: rows stay in order, never overlap, cover the whole window texture
    for (int f = 0; f <= 100; ++f) {
        const double p = f / 100.0;
        s = genie::slices(win, icon, p, ROWS);
        CHECK(near(s.front().v0, 0) && near(s.back().v1, 1), "p=%.2f texture not covered", p);
        for (size_t i = 1; i < s.size(); ++i) {
            CHECK(s[i].dst.y >= s[i - 1].dst.y + s[i - 1].dst.h - 1e-6, "p=%.2f rows overlap at %zu", p, i);
            CHECK(s[i].dst.w >= -1e-9 && s[i].dst.h >= -1e-9, "p=%.2f negative size at %zu", p, i);
        }
    }

    // dock at the top: the result is the mirror image
    const genie::Rect iconTop{940, 10, 48, 48};
    s = genie::slices(win, iconTop, 1.0, ROWS);
    for (const auto& r : s)
        CHECK(r.dst.y >= iconTop.y - 1e-6 && r.dst.y + r.dst.h <= iconTop.y + iconTop.h + 1e-6, "top dock: row outside icon");

    // fade only at the very end
    CHECK(near(genie::alphaAt(0.0), 1) && near(genie::alphaAt(0.8), 1) && near(genie::alphaAt(1.0), 0), "alpha curve");
    CHECK(genie::slices(win, icon, 0.5, 0).empty() && genie::slices({0, 0, 0, 0}, icon, 0.5, 10).empty(), "bad input");

    std::printf(fails ? "%d test(s) failed\n" : "all genie_math tests passed\n", fails);
    return fails ? 1 : 0;
}
