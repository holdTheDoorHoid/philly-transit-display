// Stats page in the main page's visual language (DESIGN.md SS8): the same header strip, then
// one stop-style panel per configured stop with the 30-day numbers - a big on-time percentage
// over an on-time meter (the share of arrivals inside SEPTA's window, filled in the same
// green/amber/red the percentage is coloured), then the mean lateness in the badge colours, the
// worst hour and the ghost count as captioned tiles, and a footer with the sample count and how
// many of those arrivals were inferred. The inferred count is not a footnote (DESIGN.md SS9.2):
// every `arrive` row is derived from a prediction that stopped being published, and the page
// says so next to the numbers built on it, in every layout.
#include "stats_screen.h"

#include <cstdio>
#include <string>
#include <vector>

#include "../net_poller.h"
#include "transit_stats/summary.h"
#include "ui_common.h"

namespace transit_app::ui {

namespace {

// How much of the tile layout each panel gets, decided once at build time from the height the
// panels will share (DESIGN.md SS8: "layout from the runtime resolution"). Two stops on a
// 320-tall board get Tiles; two on a 240-tall board, or four on a 320-tall one, get Compact;
// four on a 240-tall board get Line, the only thing that fits in ~49 px without clipping.
enum class Layout : uint8_t {
  Tiles,    // title row / big % with its caption over a 6 px meter / three two-line tiles / footer
  Compact,  // title row carrying the footer / 3 px meter / four two-line tiles, the % in the body font
  Line,     // title row carrying the footer / 3 px meter / "87% on time • +1.3 min late • 3 ghosts"
};

constexpr int32_t kMeterTiles = 6;  // the meter's height under the big number
constexpr int32_t kMeterThin = 3;   // and as the rule under the title row (Compact/Line)

struct StatsPanel {
  std::string stop_key;
  lv_obj_t *panel;
  lv_obj_t *score = nullptr;        // Tiles: the box holding the big % row and the meter
  lv_obj_t *pct = nullptr;          // "87%" (Tiles, big) or the "87% / on time" tile (Compact)
  lv_obj_t *meter = nullptr;        // the on-time meter's track; child 0 is the fill
  lv_obj_t *avg = nullptr;          // mean-late tile; Line: the whole summary; every layout: the loading/no-data caption
  lv_obj_t *worst = nullptr;
  lv_obj_t *ghosts = nullptr;
  lv_obj_t *footer = nullptr;       // "123 arrivals • 40 inferred"
  bool showing_caption = false;     // last refresh showed loading/no-data rather than numbers
  bool fresh = true;                // never refreshed: `shown` is a default view, which is also
                                    // what "nothing cached yet" looks like, so it must not match
  StopSummaryView shown;            // what the labels currently say, so a refresh with the same
                                    // numbers (the cache changes every ~10 min, the page ticks
                                    // at 1 Hz) does not rebuild ten strings for nothing
};

struct StatsScreenCtx {
  Layout layout = Layout::Tiles;
  bool short_captions = false;  // "avg"/"worst" instead of "mean late"/"worst hour" (240 wide)
  bool short_footer = false;    // "n=123 • 40 inferred" when the footer shares the title row on a narrow panel
  std::vector<StatsPanel> panels;
};

const char *const kBullet = " \xE2\x80\xA2 ";  // U+2022: the built-in font lacks the middle dot

// "5 PM", "12 AM"; "--" when the aggregator had no hour to name.
void hour12(int hour, char *out, size_t n) {
  if (hour < 0 || hour > 23) {
    snprintf(out, n, "--");
    return;
  }
  int h12 = hour % 12;
  if (h12 == 0) h12 = 12;
  snprintf(out, n, "%d %s", h12, hour < 12 ? "AM" : "PM");
}

// Green from 80 %, amber to 60 %, red below (the web Stats page's thresholds); grey when there
// is no percentage to colour.
lv_color_t pctColor(bool has_on_time, float pct) {
  if (!has_on_time) return colorScheduled();
  if (pct >= 80.0f) return colorOnTime();
  if (pct >= 60.0f) return colorSkipped();
  return colorLate();
}

// badgeFor()'s colours: green inside SEPTA's -1..+5 min on-time window, red late, blue early.
lv_color_t avgColor(float mean_late_min) {
  if (mean_late_min > 5.0f) return colorLate();
  if (mean_late_min < -1.0f) return colorEarly();
  return colorOnTime();
}

// A two-line tile: the value in its colour over the caption in the secondary colour. One label
// with inline recolor rather than two objects - the LVGL pool is 36 KB and four stops make
// twelve of these. Composed with snprintf, not std::string (ui_common.h colorHex).
void tileText(char *out, size_t n, uint32_t value_hex, const char *value, const char *caption) {
  snprintf(out, n, "#%06x %s#\n#%06x %s#", (unsigned)value_hex, value, (unsigned)colorHex(colorSubtext()), caption);
}

lv_obj_t *makeTile(lv_obj_t *row, int32_t h, lv_text_align_t align) {
  lv_obj_t *l = makeLabel(row, fontSmall(h), colorSubtext());
  lv_label_set_recolor(l, true);
  lv_obj_set_style_text_align(l, align, 0);
  lv_label_set_text(l, "");
  return l;
}

// The on-time meter: a full-width track in a quiet grey, with the on-time share filled in from
// the left in the percentage's colour. Two boxes; the fill's width is set as a percentage.
lv_obj_t *makeMeter(lv_obj_t *parent, int32_t height) {
  lv_obj_t *track = makeBox(parent);
  lv_obj_set_size(track, lv_pct(100), height);
  lv_obj_set_style_bg_color(track, lv_color_mix(colorSubtext(), colorPanelBg(), 56), 0);
  lv_obj_t *fill = makeBox(track);
  lv_obj_set_size(fill, 0, lv_pct(100));
  return track;
}

void setMeter(lv_obj_t *meter, bool has_on_time, float pct, lv_color_t color) {
  int v = has_on_time ? (int)(pct + 0.5f) : 0;
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  lv_obj_t *fill = lv_obj_get_child(meter, 0);
  lv_obj_set_width(fill, lv_pct(v));
  lv_obj_set_style_bg_color(fill, color, 0);
}

void setHidden(lv_obj_t *o, bool hidden) {
  if (o == nullptr) return;
  if (hidden) {
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
}

bool sameNumbers(const StopSummaryView &a, const StopSummaryView &b) {
  const transit_stats::StopSummary &x = a.summary, &y = b.summary;
  return a.has_value == b.has_value && a.inferred == b.inferred && x.samples == y.samples &&
         x.has_on_time == y.has_on_time && x.on_time_pct == y.on_time_pct && x.mean_late_min == y.mean_late_min &&
         x.worst_hour == y.worst_hour && x.ghosts == y.ghosts;
}

}  // namespace

lv_obj_t *createStatsScreen(const Config &cfg) {
  int32_t w, h;
  screenSize(w, h);

  lv_obj_t *screen = lv_obj_create(nullptr);
  lv_obj_set_size(screen, w, h);
  lv_obj_set_style_bg_color(screen, colorBg(), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(screen, 0, 0);
  lv_obj_set_style_border_width(screen, 0, 0);
  lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);  // DESIGN.md SS8: tap anywhere cycles pages

  auto *ctx = new StatsScreenCtx();

  // ---- Header: the main page's strip, with a title on the left and the next page on the right ----
  lv_obj_t *header = makeHeader(screen, h);
  lv_obj_t *title = makeLabel(header, fontBody(h), colorText());
  lv_label_set_recolor(title, true);
  lv_label_set_text_fmt(title, "Statistics  #%06x last 30 days#", (unsigned)colorHex(colorSubtext()));
  lv_obj_t *hint = makeLabel(header, fontSmall(h), colorSubtext());
  lv_label_set_text(hint, "tap for device info");
  if (w < 300) lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);  // 240 wide: the title alone fills the strip

  lv_obj_t *area = makePanelsArea(screen);
  const std::vector<transit::StopConfig> &stops = cfg.stops;
  if (stops.empty()) {
    lv_obj_t *empty = makeLabel(area, fontSmall(h), colorSubtext());
    lv_label_set_text(empty, "No stops configured yet.");
  }

  // ---- Layout: from the height each panel will get, not from the board name ----
  // Panels share the area below the header equally (flex_grow, like the main page), so the per-
  // panel height is the area minus its 4 px padding and gaps, over the stop count. Each layout
  // needs its lines plus the panel padding and 2 px row gaps.
  int n = (int)stops.size();
  int32_t lh_small = lv_font_get_line_height(fontSmall(h));
  int32_t lh_big = lv_font_get_line_height(fontBig(h));
  int32_t per_panel = n > 0 ? (h - headerHeight(h) - 8 - 4 * (n - 1)) / n : h;
  int32_t need_tiles = 12 + lh_small + 2 + (lh_big + 2 + kMeterTiles) + 2 + 2 * lh_small + 2 + lh_small;
  int32_t need_compact = 12 + lh_small + 2 + kMeterThin + 2 + 2 * lh_small;
  Layout layout = per_panel >= need_tiles ? Layout::Tiles : per_panel >= need_compact ? Layout::Compact : Layout::Line;
  ctx->layout = layout;
  ctx->short_captions = w < 300;
  ctx->short_footer = layout != Layout::Tiles && w < 400;

  for (const transit::StopConfig &s : stops) {
    StatsPanel sp;
    sp.stop_key = s.key;
    sp.panel = makePanel(area);
    lv_obj_set_style_pad_row(sp.panel, 2, 0);
    if (layout == Layout::Line) lv_obj_set_style_pad_all(sp.panel, 4, 0);  // two lines in ~49 px
    // Title at the top, the numbers spread through whatever height the panel got, the footer (or
    // the tiles) along the bottom - a card, not a list that stops a third of the way down. In
    // Compact/Line the title row and the meter under it are one block so the rule stays attached
    // to the title when the tiles drop to the bottom.
    lv_obj_set_flex_align(sp.panel, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *head = sp.panel;
    if (layout != Layout::Tiles) {
      head = makeBox(sp.panel);
      lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
      lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_set_style_pad_row(head, 2, 0);
      lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    }

    // Title row: the route badge and the same title the main page shows for this stop. When the
    // footer shares this row (Compact/Line) on a panel under 400 px wide, the stop's short label
    // ("17 Southbound") stands in for the full title rather than being squeezed to "17 Sou...".
    lv_obj_t *title_row = makeRow(head, 6);
    makeRouteBadge(title_row, fontSmall(h), s.route);
    lv_obj_t *t = makeLabel(title_row, fontBody(h), colorText());
    std::string title_text = ctx->short_footer ? (s.label.empty() ? s.route : s.label) : panelTitle(s);
    lv_label_set_text(t, title_text.c_str());
    lv_obj_set_flex_grow(t, 1);
    // LONG_DOT only ellipsizes when both sizes are fixed; with a content height the label wraps
    // instead and pushes the tiles out of the panel (seen at 240 wide with four stops).
    lv_obj_set_height(t, lh_small);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    if (layout != Layout::Tiles) {
      // No room for a footer line: the sample/inferred counts ride on the title row instead, and
      // the meter is the rule under it.
      sp.footer = makeLabel(title_row, fontSmall(h), colorSubtext());
      lv_label_set_text(sp.footer, "");
      sp.meter = makeMeter(head, kMeterThin);
    }

    if (layout == Layout::Line) {
      sp.avg = makeLabel(sp.panel, fontSmall(h), colorText());
      lv_label_set_recolor(sp.avg, true);
      lv_obj_set_size(sp.avg, lv_pct(100), lh_small);
      // CLIP, not DOT: LVGL 9.5's LONG_DOT counts the inline recolor commands as visible text
      // when deciding where the dots go, so a coloured line that fits was cut at "5 P...". The
      // text is composed to fit (refreshStatsScreen); a five-digit ghost count clips at the edge.
      lv_label_set_long_mode(sp.avg, LV_LABEL_LONG_CLIP);
      lv_label_set_text(sp.avg, "");
    } else {
      if (layout == Layout::Tiles) {
        // The score: "87%" in the big font with "on time" beside it at the baseline, and the
        // meter directly under both, as wide as the panel.
        sp.score = makeBox(sp.panel);
        lv_obj_set_style_bg_opa(sp.score, LV_OPA_TRANSP, 0);
        lv_obj_set_size(sp.score, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_row(sp.score, 2, 0);
        lv_obj_set_flex_flow(sp.score, LV_FLEX_FLOW_COLUMN);
        lv_obj_t *score_row = makeRow(sp.score, 8);
        lv_obj_set_flex_align(score_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        sp.pct = makeLabel(score_row, fontBig(h), colorText());
        lv_label_set_text(sp.pct, "--");
        lv_obj_t *cap = makeLabel(score_row, fontSmall(h), colorSubtext());
        lv_obj_set_style_pad_bottom(cap, (lh_big - lh_small) / 6, 0);  // the big font's descender space
        lv_label_set_text(cap, "on time");
        sp.meter = makeMeter(sp.score, kMeterTiles);
      }
      // The tiles: first one flush left, last one flush right, so the row lines up with the
      // meter and the title above it instead of floating.
      lv_obj_t *tiles = makeRow(sp.panel, ctx->short_captions ? 4 : 8);
      lv_obj_set_flex_align(tiles, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
      if (layout == Layout::Compact) sp.pct = makeTile(tiles, h, LV_TEXT_ALIGN_LEFT);
      sp.avg = makeTile(tiles, h, layout == Layout::Compact ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT);
      sp.worst = makeTile(tiles, h, LV_TEXT_ALIGN_CENTER);
      sp.ghosts = makeTile(tiles, h, LV_TEXT_ALIGN_RIGHT);
      if (layout == Layout::Tiles) {
        sp.footer = makeLabel(sp.panel, fontSmall(h), colorSubtext());
        lv_label_set_text(sp.footer, "");
      }
    }
    ctx->panels.push_back(sp);
  }

  lv_obj_set_user_data(screen, ctx);
  lv_obj_add_event_cb(screen, [](lv_event_t *e) {  // see main_screen.cpp: freed with the screen
    lv_obj_t *scr = static_cast<lv_obj_t *>(lv_event_get_target(e));
    delete static_cast<StatsScreenCtx *>(lv_obj_get_user_data(scr));
    lv_obj_set_user_data(scr, nullptr);
  }, LV_EVENT_DELETE, nullptr);
  return screen;
}

void refreshStatsScreen(lv_obj_t *screen) {
  auto *ctx = static_cast<StatsScreenCtx *>(lv_obj_get_user_data(screen));
  if (ctx == nullptr) {
    return;
  }
  const Layout layout = ctx->layout;
  char buf[112];
  // getStopSummary() never touches SD from here (F27) - it returns the cached value and asks the
  // poller task for a refresh - so this is safe to call at the screen's own cadence.
  for (StatsPanel &p : ctx->panels) {
    StopSummaryView v = getStopSummary(p.stop_key);
    const transit_stats::StopSummary &s = v.summary;
    if (!p.fresh && sameNumbers(v, p.shown)) continue;
    p.fresh = false;
    p.shown = v;

    // F27: the summary is computed on the poller task, so the first visit finds nothing cached.
    // Say so. Rendering StopSummary's default zeroes would read as "0% on time, n=0", which is a
    // claim, not a blank. Same for a stop with no samples.
    // Three ASCII dots: U+2026 is outside Montserrat 14's built-in range and drew as a box.
    const char *caption = !v.has_value ? "loading..." : (s.samples == 0 ? "no data yet" : nullptr);
    bool show_caption = caption != nullptr;
    if (show_caption != p.showing_caption) {
      p.showing_caption = show_caption;
      // Only the `avg` label stays up, carrying the caption in the secondary colour like the main
      // page's no-data line; the numbers and the meter are hidden rather than zeroed.
      setHidden(p.score ? p.score : p.pct, show_caption);
      setHidden(p.meter, show_caption);
      setHidden(p.worst, show_caption);
      setHidden(p.ghosts, show_caption);
      setHidden(p.footer, show_caption);
      // With nothing to spread out, keep the caption under the title instead of at the bottom.
      lv_obj_set_flex_align(p.panel, show_caption ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_SPACE_BETWEEN,
                            LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    }
    if (show_caption) {
      lv_label_set_text(p.avg, caption);
      continue;
    }

    // DESIGN.md SS9.2 (F21): with no arrival whose lateness we ever learned there is no
    // percentage. A dash is the answer; "0%" would be the worst possible one.
    lv_color_t pct_color = pctColor(s.has_on_time, s.on_time_pct);
    uint32_t pct_c = colorHex(pct_color);
    uint32_t avg_c = colorHex(s.has_on_time ? avgColor(s.mean_late_min) : colorScheduled());
    char pct[8] = "--", avg[16] = "--", worst[8];
    if (s.has_on_time) {
      snprintf(pct, sizeof pct, "%.0f%%", (double)s.on_time_pct);
      snprintf(avg, sizeof avg, "%+.1f min", (double)s.mean_late_min);
    }
    hour12(s.worst_hour, worst, sizeof worst);
    setMeter(p.meter, s.has_on_time, s.on_time_pct, pct_color);
    // "123 arrivals • 40 inferred"; the terser "n=123 • 40 inferred" where the footer shares a
    // 320-wide title row with the stop's label.
    snprintf(buf, sizeof buf, ctx->short_footer ? "n=%u%s%u inferred" : "%u arrivals%s%u inferred",
             (unsigned)s.samples, kBullet, (unsigned)v.inferred);
    lv_label_set_text(p.footer, buf);

    if (layout == Layout::Line) {
      // "87% on time • +1.3 min late • 3 ghosts": three facts in ~280 px, which is what a 320-wide
      // panel has. Four ("worst 5 PM" as well) measured ~330 px and clipped at "3 gho"; the worst
      // hour is the one to lose on a 2.4" board showing four stops.
      char late[20] = "--";
      if (s.has_on_time) {
        if (s.mean_late_min < 0.0f) {
          snprintf(late, sizeof late, "%.1f min early", (double)-s.mean_late_min);
        } else {
          snprintf(late, sizeof late, "+%.1f min late", (double)s.mean_late_min);
        }
      }
      snprintf(buf, sizeof buf, "#%06x %s# on time%s#%06x %s#%s%u ghosts", (unsigned)pct_c, pct, kBullet,
               (unsigned)avg_c, late, kBullet, (unsigned)s.ghosts);
      lv_label_set_text(p.avg, buf);
      continue;
    }
    if (layout == Layout::Tiles) {
      lv_label_set_text(p.pct, pct);
      lv_obj_set_style_text_color(p.pct, pct_color, 0);
    } else {
      tileText(buf, sizeof buf, pct_c, pct, "on time");
      lv_label_set_text(p.pct, buf);
    }
    uint32_t text_c = colorHex(colorText());
    tileText(buf, sizeof buf, avg_c, avg, ctx->short_captions ? "avg" : "mean late");
    lv_label_set_text(p.avg, buf);
    tileText(buf, sizeof buf, text_c, worst, ctx->short_captions ? "worst" : "worst hour");
    lv_label_set_text(p.worst, buf);
    snprintf(worst, sizeof worst, "%u", (unsigned)s.ghosts);  // reuse: the ghost count fits the same buffer
    tileText(buf, sizeof buf, text_c, worst, "ghosts");
    lv_label_set_text(p.ghosts, buf);
  }
}

}  // namespace transit_app::ui
