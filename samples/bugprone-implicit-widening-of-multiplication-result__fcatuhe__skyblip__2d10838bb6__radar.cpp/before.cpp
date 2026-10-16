#include "products/skyblip_go/pages/radar.h"

#include "core/flight/arc.h"
#include "core/units/units.h"
#include "core/util/format.h"
#include "core/util/intmath.h"
#include "ui/widgets/skyship.h"
#include "ui/widgets/stone.h"

namespace skyblip::go {

namespace {
// The screen is 200x200, an EVEN grid: there is no middle pixel. The centre is
// the POINT where four pixels meet, so each axis has a near-side and a far-side
// middle pixel - 99 and 100. Everything on this screen is built around that
// point rather than around a pixel:
//
//   kNear = 99   the pixel just before the centre (left, and above)
//   kFar  = 100  the pixel just after it (right, and below)
//
// A feature at distance d from the centre therefore occupies kNear-(d-1) on one
// side and kFar+(d-1) on the other. A feature ON the centre is a PAIR of
// pixels, never one. That makes the own ship exactly centred (its fuselage
// straddles 99|100), the rings exactly concentric with it, and every target
// offset measured from the same point in both directions.
constexpr int kNear = kGlassW / 2 - 1;
constexpr int kFar = kGlassW / 2;
constexpr int kCx = kFar;  // only for centring text, which has no such nicety
constexpr int kMargin = 4;
constexpr int kOuterR = 92;
constexpr int kRingW = 2;
constexpr int kGlyphH = 7;
constexpr int kCellW = 6;
constexpr int kClockScale = 2;
constexpr int kSecondsScale = 1;
constexpr int kSecondsGap = 2;
constexpr int kRangeScale = 2;
constexpr int kStateScale = 1;
constexpr int kTrafficScale = 3;
constexpr int kRangePad = 6;
constexpr int kKeepOutPad = 3;
constexpr int kReadingCorner = 3;
constexpr int kLabelPad = 2;
constexpr int kStackGap = 2;
constexpr int kFooterBottom = kGlassH - kMargin;
constexpr int kFooterY = kFooterBottom - kGlyphH;
constexpr int kClockY = kFooterBottom - kGlyphH * kClockScale;
constexpr int kStateY = kClockY - kStackGap - kGlyphH * kStateScale;
constexpr int kRangeY = kFooterBottom - kGlyphH * kRangeScale;
constexpr int kUnitGap = 3;
constexpr int32_t kQ14One = 16384;
constexpr int32_t kTurn16 = 65536;
constexpr int16_t kCaretClimbE8 = 20;
constexpr int32_t kLevelM = 60;
constexpr int32_t kLeaderSeconds = 60;
constexpr uint32_t kLeaderStepMs = 5000;
constexpr int kStepsPerMinute = static_cast<int>((kLeaderSeconds * 1000) / kLeaderStepMs);
constexpr int kMinLeaderPx = 3;
constexpr int kOwnNoseAhead = ui::kSkyshipRowsToNose + 1;
constexpr int kMinuteClearPx = kOwnNoseAhead + kMinLeaderPx;
constexpr int kFooterTop = kStateY - kLabelPad;
constexpr int kMinuteBallR = 3;
constexpr int kBallClearR = kOuterR - kRingW - kMinuteBallR;
constexpr int kDashPx = 3;
constexpr int kDashGapPx = 3;
constexpr int kWedgeInnerR = 15;
constexpr int kWedgeOuterR = kOuterR - kRingW;
constexpr int64_t kTanScale = 10000;
constexpr int64_t kWedgeEdgeTanE4 = 10000;
constexpr int kFormationD = 13;
constexpr int kFormationCorner = 4;
constexpr int kFormationInset = 3;
constexpr int kDigitW = 5;
constexpr int kBannerScale = 2;
constexpr int kBannerPad = 4;
constexpr int kOwnShipTail = kNear - ui::kSkyshipRowsToNose + ui::kSkyshipRows;
constexpr int kBannerY = (kOwnShipTail + kFooterTop - kGlyphH * kBannerScale) / 2;
constexpr int kNoteScale = 1;
constexpr int kNotePad = 2;
constexpr int kNoteGap = 5;
constexpr int kNoteY = kBannerY + kGlyphH * kBannerScale + kBannerPad + kNoteGap;
constexpr int kMinutesMarked = 2;

int half_chord_in_half_pixels(int r, int b) {
    const int32_t v = 4 * r * r - (2 * b + 1) * (2 * b + 1);
    if (v < 0) return -1;
    int q = isqrt<int32_t>(v);
    while (q * q > v) q--;
    return (q - 1) / 2;
}

void ring(ui::Canvas& fb, int r) {
    for (int b = 0; b < r; b++) {
        const int outer = half_chord_in_half_pixels(r, b);
        if (outer < 0) continue;
        const int inner = half_chord_in_half_pixels(r - kRingW, b) + 1;
        const int w = outer - inner + 1;
        fb.hline(kFar + inner, kFar + b, w, true);
        fb.hline(kFar + inner, kNear - b, w, true);
        fb.hline(kNear - outer, kFar + b, w, true);
        fb.hline(kNear - outer, kNear - b, w, true);
    }
}

int16_t c16(int32_t deg) {
    int32_t d = ((deg % 360) + 360) % 360;
    if (d >= 180) d -= 360;  // keep the cordic value inside int16_t
    return static_cast<int16_t>((d * kTurn16) / 360);
}

struct HeadingUp {
    int32_t ahead;
    int32_t right;
};

HeadingUp heading_up(int32_t north, int32_t east, int16_t track) {
    const int64_t c = icos(track), s = isin(track);
    return {static_cast<int32_t>((north * c + east * s) / kQ14One),
            static_cast<int32_t>((east * c - north * s) / kQ14One)};
}

int px_of(int32_t right) { return right >= 0 ? kFar + right : kNear + right + 1; }
int py_of(int32_t ahead) { return ahead <= 0 ? kFar - ahead : kNear - ahead + 1; }

int text_width(const char* s, int scale) {
    int n = 0;
    while (s[n]) n++;
    return n * kCellW * scale - scale;
}

struct Box {
    int x;
    int y;
    int w;
    int h;
};

Box padded(int x, int y, int w, int h, int pad) {
    return {x - pad, y - pad, w + 2 * pad, h + 2 * pad};
}

void clear_behind(ui::Canvas& fb, int x, int y, int w, int h, int pad) {
    const Box b = padded(x, y, w, h, pad);
    fb.rect(b.x, b.y, b.w, b.h, false, true);
}

bool flight_over(const RadarSnapshot& snap) { return snap.flight_time_valid && !snap.airborne; }

Box clock_box(const RadarSnapshot& snap, int pad) {
    char buf[8];
    fmt_flight_clock(buf, snap.flight_seconds, snap.flight_time_valid);
    int w = text_width(buf, kClockScale);
    if (flight_over(snap)) w += kSecondsGap + text_width("00", kSecondsScale);
    return padded(kMargin, kClockY, w, kGlyphH * kClockScale, pad);
}

void flight_clock(ui::Canvas& fb, const RadarSnapshot& snap) {
    char buf[8];
    fmt_flight_clock(buf, snap.flight_seconds, snap.flight_time_valid);
    const int minutes_w = text_width(buf, kClockScale);
    clear_behind(fb, kMargin, kClockY, minutes_w, kGlyphH * kClockScale, kLabelPad);
    fb.draw_text(kMargin, kClockY, buf, true, kClockScale);
    if (!flight_over(snap)) return;

    char seconds[4];
    seconds[fmt_uint(seconds, snap.flight_seconds % 60, 2)] = 0;
    const int x = kMargin + minutes_w + kSecondsGap;
    const int y = kClockY + kGlyphH * (kClockScale - kSecondsScale);
    clear_behind(fb, x, y, text_width(seconds, kSecondsScale), kGlyphH * kSecondsScale, kLabelPad);
    fb.draw_text(x, y, seconds, true, kSecondsScale);
}

struct RangeText {
    char number[8];
    const char* unit;
    int number_w;
    int w;
};

RangeText range_text(const RadarSnapshot& snap) {
    RangeText t;
    const bool metric = snap.units == go::Units::Metric;
    const int32_t km_e1 = div_round(snap.range_nm * kMetresPerNm, 100);
    const int n = metric ? fmt_uint(t.number, static_cast<uint32_t>(km_e1), 2, 1)
                         : fmt_uint(t.number, static_cast<uint32_t>(snap.range_nm));
    t.number[n] = 0;
    t.unit = metric ? "KM" : "NM";
    t.number_w = text_width(t.number, kRangeScale);
    t.w = t.number_w + kUnitGap + text_width(t.unit, 1);
    return t;
}

Box range_box(const RadarSnapshot& snap, int pad) {
    const int w = range_text(snap).w;
    return padded(kCx - w / 2, kRangeY, w, kGlyphH * kRangeScale, pad);
}

void range_label(ui::Canvas& fb, const RadarSnapshot& snap) {
    const RangeText t = range_text(snap);
    const Box b = range_box(snap, kRangePad);
    const int x = b.x + kRangePad;
    fb.rect(b.x, b.y, b.w, b.h, false, true);
    fb.draw_text(x, kRangeY, t.number, true, kRangeScale);
    fb.draw_text(x + t.number_w + kUnitGap, kRangeY + kGlyphH * (kRangeScale - 1), t.unit, true, 1);
}

void flight_word(ui::Canvas& fb, const RadarSnapshot& snap) {
    if (!snap.fix_valid || !snap.airborne) return;
    const char* state = "FLIGHT";
    clear_behind(fb, kMargin, kStateY, text_width(state, kStateScale), kGlyphH * kStateScale,
                 kLabelPad);
    fb.draw_text(kMargin, kStateY, state, true, kStateScale);
}

const char* ring_word(const RadarSnapshot& snap) {
    if (!snap.fix_valid) return "NO FIX";
    if (snap.airborne) return nullptr;
    return snap.taxiing ? "TAXI" : "GROUND";
}

// INFO: fc 18sep26 how far the receiver has got, under the word that says it has not got there
const char* ring_note(const RadarSnapshot& snap) {
    return snap.fix_valid ? nullptr : gnss::stage_name(snap.stage);
}

Box note_box(const char* note) {
    const int w = text_width(note, kNoteScale);
    return {kCx - w / 2 - kNotePad, kNoteY - kNotePad, w + 2 * kNotePad,
            kGlyphH * kNoteScale + 2 * kNotePad};
}

void state_note(ui::Canvas& fb, const char* note) {
    const Box b = note_box(note);
    fb.rect(b.x, b.y, b.w, b.h, false, true);
    fb.draw_text(b.x + kNotePad, b.y + kNotePad, note, true, kNoteScale);
}

void aircraft(ui::Canvas& fb, int in_view, bool counting) {
    char buf[4];
    if (counting)
        buf[fmt_uint(buf, static_cast<uint32_t>(in_view))] = 0;
    else
        buf[fmt_string(buf, "-")] = 0;
    const int x = kGlassW - kMargin - text_width(buf, kTrafficScale);
    fb.draw_text(x, kFooterBottom - kGlyphH * kTrafficScale, buf, true, kTrafficScale);
    fb.draw_text(x - kUnitGap - text_width("ACT", 1), kFooterY, "ACT", true, 1);
}

Box banner_box(const char* word) {
    const int w = text_width(word, kBannerScale);
    return {kCx - w / 2 - kBannerPad, kBannerY - kBannerPad, w + 2 * kBannerPad,
            kGlyphH * kBannerScale + 2 * kBannerPad};
}

void state_banner(ui::Canvas& fb, const char* word) {
    const Box b = banner_box(word);
    fb.rect(b.x, b.y, b.w, b.h, false, true);
    fb.draw_text(b.x + kBannerPad, b.y + kBannerPad, word, true, kBannerScale);
}

struct Plotted {
    int32_t right;
    int32_t ahead;
    int x;
    int y;
    bool in_ring;
    ui::Cut cut;
    ui::Band band;
    ui::Trend trend;
};

ui::Cut cut_of(int32_t up_m) {
    if (up_m > kLevelM) return ui::Cut::Crown;
    if (up_m < -kLevelM) return ui::Cut::Pavilion;
    return ui::Cut::Diamond;
}

ui::Band band_of(int32_t up_m) {
    const int32_t apart = up_m < 0 ? -up_m : up_m;
    return apart <= traffic::kAdvisoryAltM ? ui::Band::Near : ui::Band::Far;
}

ui::Trend trend_of(const RadarTarget& t) {
    if (!t.climb_valid) return ui::Trend::Level;
    if (t.climb_e8 >= kCaretClimbE8) return ui::Trend::Climbing;
    if (t.climb_e8 <= -kCaretClimbE8) return ui::Trend::Sinking;
    return ui::Trend::Level;
}

int64_t range_metres(const RadarSnapshot& snap) {
    return static_cast<int64_t>(snap.range_nm > 0 ? snap.range_nm : 1) * kMetresPerNm;
}

int32_t to_px(int32_t metres, int64_t range) {
    return static_cast<int32_t>((static_cast<int64_t>(metres) * kOuterR) / range);
}

bool inside_ring(int32_t right, int32_t ahead) {
    return right * right + ahead * ahead <= kOuterR * kOuterR;
}

bool on_glass(int x, int y) { return x >= 0 && x < kGlassW && y >= 0 && y < kGlassH; }

bool plot_point(const RadarSnapshot& snap, const RadarTarget& t, int16_t track, Plotted& out) {
    const int64_t range = range_metres(snap);
    const HeadingUp at = heading_up(t.north_m, t.east_m, track);
    const int32_t dx = to_px(at.right, range), dy = to_px(at.ahead, range);
    const int x = px_of(dx), y = py_of(dy);
    if (!on_glass(x, y)) return false;
    const ui::Cut cut = cut_of(t.up_m);
    const ui::Band band = band_of(t.up_m);
    const ui::Trend trend = trend_of(t);
    if (!inside_ring(dx, dy) && y + ui::stone_below(cut, band, trend) >= kFooterTop) return false;
    out = {dx, dy, x, y, inside_ring(dx, dy), cut, band, trend};
    return true;
}

flight::Motion motion_of(int32_t speed_mm_s, int32_t track_cdeg, int16_t turn_cdps, bool turning) {
    flight::Motion m{};
    m.speed_mm_s = speed_mm_s;
    m.track_cdeg = track_cdeg;
    m.turn_cdps = turn_cdps;
    m.turning = turning;
    return m;
}

HeadingUp on_glass_at(const flight::Position& p, const RadarSnapshot& snap, int16_t track) {
    const HeadingUp at = heading_up(p.north_m, p.east_m, track);
    const int64_t range = range_metres(snap);
    return {to_px(at.ahead, range), to_px(at.right, range)};
}

Box formation_box() {
    return {kNear - kFormationD, kNear - kFormationD, 2 * kFormationD + 2, 2 * kFormationD + 2};
}

void formation_square(ui::Canvas& fb) {
    const int d = kFormationD, r = kFormationCorner;
    const int x0 = kNear - d, x1 = kFar + d, y0 = kNear - d, y1 = kFar + d;
    fb.hline(x0 + r + 1, y0, x1 - x0 - 2 * r - 1, true);
    fb.hline(x0 + r + 1, y1, x1 - x0 - 2 * r - 1, true);
    fb.vline(x0, y0 + r + 1, y1 - y0 - 2 * r - 1, true);
    fb.vline(x1, y0 + r + 1, y1 - y0 - 2 * r - 1, true);
    for (int step = 0; step <= 90; step++) {
        const int16_t a = c16(step);
        const int dx = r - (r * icos(a)) / kQ14One, dy = r - (r * isin(a)) / kQ14One;
        fb.set_pixel(x0 + dx, y0 + dy, true);
        fb.set_pixel(x1 - dx, y0 + dy, true);
        fb.set_pixel(x0 + dx, y1 - dy, true);
        fb.set_pixel(x1 - dx, y1 - dy, true);
    }
}

// One digit per quadrant of own-ship's nose: how many of the formation are over there.
void formation_counts(ui::Canvas& fb, const RadarSnapshot& snap, int16_t track) {
    int count[4] = {0, 0, 0, 0};
    for (int i = 0; i < snap.n_targets; i++) {
        const RadarTarget& t = snap.targets[i];
        if (!t.in_formation) continue;
        const HeadingUp at = heading_up(t.north_m, t.east_m, track);
        const int quadrant = (at.ahead < 0 ? 2 : 0) + (at.right >= 0 ? 1 : 0);
        count[quadrant]++;
    }

    const int left = kNear - kFormationD + kFormationInset;
    const int right = kFar + kFormationD - kFormationInset - kDigitW + 1;
    const int fore = kNear - kFormationD + kFormationInset;
    const int aft = kFar + kFormationD - kFormationInset - kGlyphH + 1;
    const int x[4] = {left, right, left, right};
    const int y[4] = {fore, fore, aft, aft};
    for (int q = 0; q < 4; q++) {
        if (count[q] <= 0) continue;
        char buf[4];
        buf[fmt_uint(buf, static_cast<uint32_t>(count[q] > 9 ? 9 : count[q]))] = 0;
        fb.draw_text(x[q], y[q], buf, true, 1);
    }
}

void minute_ball(ui::Canvas& fb, int x, int y) {
    const int r = kMinuteBallR;
    for (int dy = -r; dy < r; dy++)
        for (int dx = -r; dx < r; dx++) {
            const int px = 2 * dx + 1, py = 2 * dy + 1;
            if (px * px + py * py <= 4 * r * r) fb.set_pixel(x + dx, y + dy, true);
        }
}

struct DashPen {
    ui::Canvas& fb;
    int phase{0};
    int x{0};
    int y{0};
    bool down{false};
    bool fresh{false};

    void lift() { down = false; }

    void to(int x1, int y1) {
        if (down)
            stroke(x1, y1);
        else
            fresh = down = true;
        x = x1;
        y = y1;
    }

   private:
    void dash(int at_x, int at_y, bool steep) {
        if (phase++ % (kDashPx + kDashGapPx) >= kDashPx) return;
        fb.set_pixel(at_x, at_y, true);
        fb.set_pixel(steep ? at_x - 1 : at_x, steep ? at_y : at_y - 1, true);
    }

    void stroke(int x1, int y1) {
        int at_x = x, at_y = y;
        const int dx = x1 > at_x ? x1 - at_x : at_x - x1;
        const int dy = y1 > at_y ? y1 - at_y : at_y - y1;
        const int sx = at_x < x1 ? 1 : -1, sy = at_y < y1 ? 1 : -1;
        const bool steep = dy >= dx;
        int err = dx - dy;
        if (fresh) dash(at_x, at_y, steep);
        fresh = false;
        while (at_x != x1 || at_y != y1) {
            const int e2 = 2 * err;
            if (e2 > -dy) {
                err -= dy;
                at_x += sx;
            }
            if (e2 < dx) {
                err += dx;
                at_y += sy;
            }
            dash(at_x, at_y, steep);
        }
    }
};

void own_vector(ui::Canvas& fb, const RadarSnapshot& snap, int16_t track) {
    if (snap.speed_mm_s <= 0) return;
    flight::Arc arc(motion_of(snap.speed_mm_s, snap.track_cdeg, snap.turn_cdps, true),
                    kLeaderStepMs);
    DashPen pen{fb};
    for (int step = 1; step <= kMinutesMarked * kStepsPerMinute; step++) {
        const HeadingUp at = on_glass_at(arc.advance(), snap, track);
        if (!inside_ring(at.right, at.ahead)) return;
        const int64_t out = at.right * at.right + at.ahead * at.ahead;
        if (out < kMinuteClearPx * kMinuteClearPx) {
            pen.lift();
            continue;
        }
        const int x = px_of(at.right), y = py_of(at.ahead);
        pen.to(x, y);
        if (step % kStepsPerMinute != 0) continue;
        if (out <= kBallClearR * kBallClearR) minute_ball(fb, x, y);
    }
}

struct Leader {
    int32_t right;
    int32_t ahead;
    bool valid;
};

Leader leader_of(const RadarSnapshot& snap, const RadarTarget& t, int16_t track) {
    if (t.speed_mm_s <= 0) return {0, 0, false};
    flight::Arc arc(motion_of(t.speed_mm_s, t.track_cdeg, t.turn_cdps, t.turn_valid),
                    kLeaderStepMs);
    HeadingUp end{0, 0};
    for (int step = 0; step < kStepsPerMinute; step++)
        end = on_glass_at(arc.advance(), snap, track);
    if (end.right * end.right + end.ahead * end.ahead < kMinLeaderPx * kMinLeaderPx)
        return {0, 0, false};
    return {end.right, end.ahead, true};
}

void draw_leader(ui::Canvas& fb, const RadarSnapshot& snap, const RadarTarget& t, int16_t track,
                 const Plotted& p, const Leader& v) {
    if (!v.valid) return;
    flight::Arc arc(motion_of(t.speed_mm_s, t.track_cdeg, t.turn_cdps, t.turn_valid),
                    kLeaderStepMs);
    int from_x = p.x, from_y = p.y;
    for (int step = 0; step < kStepsPerMinute; step++) {
        const HeadingUp at = on_glass_at(arc.advance(), snap, track);
        const int to_x = px_of(p.right + at.right), to_y = py_of(p.ahead + at.ahead);
        fb.line(from_x, from_y, to_x, to_y, true);
        from_x = to_x;
        from_y = to_y;
    }
}

void traffic_symbol(ui::Canvas& fb, const Plotted& p, traffic::Level alarm_level) {
    ui::draw_stone(fb, p.x, p.y, p.cut, p.band, p.trend, alarm_level >= traffic::Level::Advisory);
}

int plot(ui::Canvas& fb, const RadarSnapshot& snap, int16_t track) {
    Plotted shown[kMaxRadarTargets];
    const RadarTarget* in_view[kMaxRadarTargets];
    int n = 0;
    int in_ring = 0;
    for (int i = 0; i < snap.n_targets && n < kMaxRadarTargets; i++) {
        Plotted p;
        if (!plot_point(snap, snap.targets[i], track, p)) continue;
        // A member of the formation is drawn once, as the square around own ship
        // and the count in its quadrant. Twice is two aircraft.
        if (snap.targets[i].in_formation) {
            if (p.in_ring) in_ring++;
            continue;
        }
        shown[n] = p;
        in_view[n] = &snap.targets[i];
        n++;
    }

    for (int i = 0; i < n; i++)
        if (shown[i].in_ring) in_ring++;

    for (int i = 0; i < n; i++)
        draw_leader(fb, snap, *in_view[i], track, shown[i], leader_of(snap, *in_view[i], track));
    if (in_ring > 0) own_vector(fb, snap, track);

    for (int i = 0; i < n; i++) traffic_symbol(fb, shown[i], in_view[i]->alarm_level);
    return in_ring;
}

struct Wedge {
    int32_t x{0};
    int32_t y{0};
};

int alarm_wedges(const RadarSnapshot& snap, int16_t track, Wedge* out) {
    if (!snap.alarm_flash) return 0;
    int n = 0;
    for (int i = 0; i < snap.n_targets && n < kMaxRadarTargets; i++) {
        const RadarTarget& t = snap.targets[i];
        if (t.alarm_level < traffic::Level::Advisory || t.alarm_dismissed) continue;
        const HeadingUp at = heading_up(t.north_m, t.east_m, track);
        if (at.ahead == 0 && at.right == 0) continue;
        out[n++] = {at.right, -at.ahead};
    }
    return n;
}

bool in_wedge(const Wedge& w, int64_t px, int64_t py) {
    const int64_t along = px * w.x + py * w.y;
    if (along <= 0) return false;
    const int64_t across = px * w.y - py * w.x;
    return (across < 0 ? -across : across) * kTanScale <= kWedgeEdgeTanE4 * along;
}

bool in_any_wedge(const Wedge* wedges, int n, int64_t px, int64_t py) {
    for (int i = 0; i < n; i++)
        if (in_wedge(wedges[i], px, py)) return true;
    return false;
}

bool inside_rounded(const Box& b, int corner, int x, int y) {
    const int past_left = b.x + corner - x;
    const int past_right = x - (b.x + b.w - 1 - corner);
    const int past_top = b.y + corner - y;
    const int past_bottom = y - (b.y + b.h - 1 - corner);
    if (x < b.x || y < b.y || past_right > corner || past_bottom > corner) return false;
    const int dx = past_left > 0 ? past_left : (past_right > 0 ? past_right : 0);
    const int dy = past_top > 0 ? past_top : (past_bottom > 0 ? past_bottom : 0);
    return dx * dx + dy * dy <= corner * corner;
}

bool spared(const Box* keep_out, int n, int x, int y) {
    for (int i = 0; i < n; i++)
        if (inside_rounded(keep_out[i], kReadingCorner, x, y)) return true;
    return false;
}

bool at_the_apex(const Box* formation, int64_t r2, int x, int y) {
    if (formation) return inside_rounded(*formation, kFormationCorner, x, y);
    return r2 < 4 * static_cast<int64_t>(kWedgeInnerR) * kWedgeInnerR;
}

void invert_wedges(ui::Canvas& fb, const Wedge* wedges, int n_wedges, const Box* keep_out,
                   int n_keep_out, const Box* formation) {
    const int64_t outer2 = 4 * static_cast<int64_t>(kWedgeOuterR) * kWedgeOuterR;
    const int bottom = kFar + kOuterR < kGlassH ? kFar + kOuterR : kGlassH;
    for (int y = kFar - kOuterR; y < bottom; y++) {
        const int64_t py = 2 * (y - kFar) + 1;
        for (int x = kFar - kOuterR; x < kFar + kOuterR; x++) {
            const int64_t px = 2 * (x - kFar) + 1;
            const int64_t r2 = px * px + py * py;
            if (r2 > outer2 || at_the_apex(formation, r2, x, y)) continue;
            if (!in_any_wedge(wedges, n_wedges, px, py)) continue;
            if (spared(keep_out, n_keep_out, x, y)) continue;
            fb.set_pixel(x, y, !fb.get_pixel(x, y));
        }
    }
}

}  // namespace

void draw_radar(ui::Canvas& fb, const RadarSnapshot& snap) {
    fb.clear(true);

    ring(fb, kOuterR);

    const int16_t track = c16(to_degrees(CentiDegrees(snap.track_cdeg)).v);

    ui::draw_skyship(fb, kFar, kNear);

    const int in_ring = snap.fix_valid ? plot(fb, snap, track) : 0;

    flight_clock(fb, snap);
    flight_word(fb, snap);
    range_label(fb, snap);
    aircraft(fb, in_ring, snap.fix_valid && snap.receiver_listening);

    Wedge wedges[kMaxRadarTargets];
    const int n_wedges = alarm_wedges(snap, track, wedges);
    if (n_wedges > 0) {
        const Box readings[] = {range_box(snap, kKeepOutPad), clock_box(snap, kKeepOutPad)};
        const Box square = formation_box();
        invert_wedges(fb, wedges, n_wedges, readings, 2,
                      snap.formation_members > 0 ? &square : nullptr);
    }

    if (snap.formation_members > 0) {
        formation_square(fb);
        formation_counts(fb, snap, track);
    }

    if (const char* word = ring_word(snap)) state_banner(fb, word);
    if (const char* note = ring_note(snap)) state_note(fb, note);
}

}  // namespace skyblip::go
