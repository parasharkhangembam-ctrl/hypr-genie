#pragma once
// Genie geometry: pure maths, no Hyprland types, so it can be unit-tested.
//
// Two overlapping phases, like macOS (and BCGenieEffect / Harshil Shah's
// "Recreating the Mac genie effect"):
//
//   Phase A (p 0.0 -> 0.5)  the bottom edge is pulled down into the dock icon
//                           while the top edge stays put; the sides bend into
//                           a funnel (S-curves) that ends in the icon.
//   Phase B (p 0.4 -> 1.0)  the whole window pours down the funnel; whatever
//                           passes the icon's top edge is squeezed into it.
//
//   p = 0.00     p = 0.25       p = 0.50        p = 0.75       p = 1.00
//   +------+     +------+       +------+
//   |      |     |      |        \    /          \    /
//   |      |      \    /          \  /            \  /
//   +------+       \  /            ||              ||
//                   ||             ||              ||
//     [  ]         [  ]           [  ]            [##]           [##]
//
// The window is cut into thin horizontal rows. For every row we compute
// where it is drawn (dst) and which band of the window it shows (v0..v1).

#include <algorithm>
#include <cmath>
#include <vector>

namespace genie {

    struct Rect {
        double x = 0, y = 0, w = 0, h = 0;
    };

    struct Slice {
        Rect   dst;    // where this row is drawn (same units as src/icon)
        double v0, v1; // which band of the window it shows (0 = top, 1 = bottom)
    };

    inline double clamp01(double v) {
        return std::clamp(v, 0.0, 1.0);
    }
    // smooth step: gentle start and end (also the funnel's S-curve)
    inline double smooth(double t) {
        t = clamp01(t);
        return t * t * (3.0 - 2.0 * t);
    }

    constexpr double PHASE_A_END   = 0.5; // pulling the bottom into the icon
    constexpr double PHASE_B_START = 0.4; // pouring down the funnel

    // Slices for a window whose icon is BELOW it (the dock at the bottom).
    inline std::vector<Slice> slicesDown(const Rect& src, const Rect& icon, double p, int rows) {
        std::vector<Slice> out;
        out.reserve(rows);

        const double a       = smooth(p / PHASE_A_END);
        const double s       = smooth((p - PHASE_B_START) / (1.0 - PHASE_B_START));
        const double winBot  = src.y + src.h;
        const double iconBot = icon.y + icon.h;

        // top of the "inside the icon" zone; starts at the window bottom if a
        // tall (e.g. maximised) window already overlaps the dock
        const double Z0 = std::max(icon.y, winBot);
        const double Z  = Z0 + (icon.y - Z0) * a;

        const double B = winBot + (iconBot - winBot) * a; // content bottom (phase A)
        const double T = src.y + (icon.y - src.y) * s;    // content top    (phase B)
        const double L = B - src.y;                       // content length (stretched)

        // content that went past the zone top is squeezed to fit the icon
        const double extent = (T + L) - Z;
        const double room   = std::max(0.0, iconBot - Z);
        const double k      = (extent > room && extent > 1e-9) ? room / extent : 1.0;

        auto         mapY = [&](double v) {
            const double yu = T + v * L;
            return yu <= Z ? yu : Z + (yu - Z) * k;
        };

        // funnel: how far a row at screen-y has bent toward the icon (0..1)
        const double funnelH = std::max(1e-6, Z - src.y);
        auto         bend    = [&](double y) { return a * smooth((y - src.y) / funnelH); };

        double       prevY = mapY(0.0);
        for (int i = 0; i < rows; ++i) {
            const double v0 = double(i) / rows;
            const double v1 = double(i + 1) / rows;
            const double y1 = mapY(v1);
            const double e  = bend((prevY + y1) * 0.5);
            const double xl = src.x + (icon.x - src.x) * e;
            const double xr = (src.x + src.w) + ((icon.x + icon.w) - (src.x + src.w)) * e;
            out.push_back(Slice{Rect{xl, prevY, xr - xl, y1 - prevY}, v0, v1});
            prevY = y1;
        }
        return out;
    }

    // p = 0: window at rest, p = 1: fully inside the icon.
    inline std::vector<Slice> slices(const Rect& src, const Rect& icon, double p, int rows) {
        if (rows < 1 || src.w <= 0 || src.h <= 0)
            return {};
        p = clamp01(p);

        const bool iconBelow = (icon.y + icon.h * 0.5) >= (src.y + src.h * 0.5);
        if (iconBelow)
            return slicesDown(src, icon, p, rows);

        // icon above (dock at the top): mirror vertically, solve, mirror back
        const Rect mSrc{src.x, -(src.y + src.h), src.w, src.h};
        const Rect mIcon{icon.x, -(icon.y + icon.h), icon.w, icon.h};
        auto       out = slicesDown(mSrc, mIcon, p, rows);
        for (auto& sl : out) {
            sl.dst.y            = -(sl.dst.y + sl.dst.h);
            const double v0     = 1.0 - sl.v1;
            const double v1     = 1.0 - sl.v0;
            sl.v0               = v0;
            sl.v1               = v1;
        }
        return out;
    }

    // overall opacity: fades only at the very end, so the window vanishes
    // cleanly even when the dock is auto-hidden
    inline double alphaAt(double p) {
        return 1.0 - smooth((p - 0.86) / 0.14);
    }
}
