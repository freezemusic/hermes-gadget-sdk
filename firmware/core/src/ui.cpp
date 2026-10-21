#include "hg/ui.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "hg/face.hpp"
#include "hg/font.hpp"

namespace hg {
namespace {

// The background on backlit panels; emissive panels use true black (Ui::bg_).
constexpr uint16_t kBg = rgb565(10, 14, 20);
constexpr uint16_t kBar = rgb565(24, 31, 42);
constexpr uint16_t kText = rgb565(232, 238, 242);
constexpr uint16_t kDim = rgb565(132, 146, 160);
constexpr uint16_t kFaint = rgb565(52, 62, 76);
constexpr uint16_t kAccent = rgb565(242, 179, 61);
constexpr uint16_t kGreen = rgb565(61, 214, 140);
constexpr uint16_t kBlue = rgb565(90, 169, 242);
constexpr uint16_t kRed = rgb565(242, 95, 92);
constexpr uint16_t kYellow = rgb565(242, 201, 76);
constexpr uint16_t kGreenDim = rgb565(28, 92, 62);
constexpr uint16_t kRedDim = rgb565(122, 44, 44);

class Hash {
 public:
  Hash& add(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
      h_ ^= p[i];
      h_ *= 16777619u;
    }
    return *this;
  }
  Hash& add(const std::string& s) {
    add(s.data(), s.size());
    uint8_t sep = 0xFF;
    return add(&sep, 1);
  }
  template <typename T>
  Hash& val(T v) {
    return add(&v, sizeof(v));
  }
  uint32_t get() const { return h_; }

 private:
  uint32_t h_ = 2166136261u;
};

std::string fit(std::string_view s, int max_chars) {
  if (max_chars <= 0) return {};
  if (static_cast<int>(s.size()) <= max_chars) return std::string(s);
  if (max_chars <= 2) return std::string(s.substr(0, static_cast<size_t>(max_chars)));
  return std::string(s.substr(0, static_cast<size_t>(max_chars - 2))) + "..";
}

int cols_for(int width_px, int scale) { return width_px / (font::kCellWidth * scale); }

bool animated(Screen s) {
  switch (s) {
    case Screen::Boot:
    case Screen::Connecting:
    case Screen::Updating:
    case Screen::Ready:
    case Screen::Listening:
    case Screen::Thinking:
    case Screen::Responding:
      return true;
    default:
      return false;
  }
}

// Triangle wave in [0, period/2] for gentle pulsing.
int tri(uint32_t frame, int period) {
  int p = static_cast<int>(frame % static_cast<uint32_t>(period));
  return p < period / 2 ? p : period - p;
}

// Everything that changes hero pixels between frames, and nothing else, so a
// settled face does not flush. The disc colour follows the screen and is part
// of the static key, not this one.
face::Style face_style(const UiModel& m) {
  face::Style s;
  s.morph = m.face_morph;
  s.shape = m.face_shape;
  s.shape_k = m.face_shape_k;
  s.wake = m.face_wake;
  s.reaction = m.face_reaction;
  s.asleep = m.face_asleep;
  s.swirl = m.face_swirl;
  s.busy = m.face_busy;
  return s;
}

uint32_t hero_anim_key(const UiModel& m, int diameter) {
  const face::Style style = face_style(m);
  return face::picture_key(m.face_frame, face::mood_for(m.screen, m.face_happy, m.face_reaction, m.face_busy), m.level,
                           m.voice, face::dots_for(m.screen, m.face_busy), diameter, style);
}

// True while the face can change pixels from one face tick to the next.
// A finished sleep dot and the error mark stay still; everything else blinks,
// glances, breathes, pulses or is mid-morph, and has to be flushed.
bool face_live(const UiModel& m) {
  const face::Style s = face_style(m);
  if (s.swirl || s.wake < 100 || s.morph > 0) return true;
  if (s.shape_k > 0 && s.shape_k < 100) return true;
  const face::Mood mood = face::mood_for(m.screen, m.face_happy, m.face_reaction, m.face_busy);
  if (mood == face::Mood::Alert || mood == face::Mood::Sleep || mood == face::Mood::Sleepy) return false;
  return true;
}

uint16_t tone_color(DashRow::Tone t) {
  switch (t) {
    case DashRow::Tone::Busy: return kBlue;
    case DashRow::Tone::Waiting: return kAccent;
    case DashRow::Tone::Good: return kGreen;
    case DashRow::Tone::Warn: return kYellow;
    case DashRow::Tone::Bad: return kRed;
    case DashRow::Tone::Dim: break;
  }
  return kDim;
}

uint16_t caption_color(Screen s) {
  switch (s) {
    case Screen::Listening: return kGreen;
    case Screen::Thinking: return kBlue;
    case Screen::Responding:
    case Screen::Connecting:
    case Screen::Updating:
    case Screen::Pairing:
    case Screen::Prompt: return kAccent;
    case Screen::Offline:
    case Screen::Error: return kRed;
    default: return kText;
  }
}

}  // namespace

const char* screen_name(Screen s) {
  switch (s) {
    case Screen::Boot: return "boot";
    case Screen::Offline: return "offline";
    case Screen::Connecting: return "connecting";
    case Screen::Pairing: return "pairing";
    case Screen::Ready: return "ready";
    case Screen::Listening: return "listening";
    case Screen::Thinking: return "thinking";
    case Screen::Responding: return "responding";
    case Screen::Card: return "card";
    case Screen::Image: return "image";
    case Screen::Error: return "error";
    case Screen::Prompt: return "prompt";
    case Screen::Updating: return "updating";
    case Screen::Settings: return "settings";
    case Screen::Setup: return "setup";
    case Screen::Dashboard: return "dashboard";
  }
  return "unknown";
}

Ui::Ui(Display& display)
    : display_(display), panel_(display.info()), info_(panel_), bg_(panel_.emissive ? 0 : kBg) {
  if (panel_.round) {
    // The largest square inside the circle; the corners of the panel do not exist.
    const int side = std::min(panel_.width, panel_.height) * 707 / 1000;
    ox_ = (panel_.width - side) / 2;
    oy_ = (panel_.height - side) / 2;
    info_.width = info_.height = static_cast<uint16_t>(side);
  }
  int w = info_.width, h = info_.height;
  int s = std::max(1, std::min(4, std::min(w / 160, h / 120)));
  // A board can ask for larger text (font_scale multiplies the automatic pick);
  // clamp to the same 1..4 range every layout below assumes.
  s = std::min(4, s * std::max<uint8_t>(1, panel_.font_scale));
  layout_.scale = s;
  const int bar_h = Canvas::line_height(s) + 2 * s;
  // A round panel's top band holds the link dot and one line of the settings
  // label: 2r + 14s, and never under 36 px so the hold target stays finger-sized.
  const int dot_r = std::max(2, 3 * s / 2 + 1);
  layout_.top_h = panel_.round ? std::max(36, 2 * dot_r + 14 * s) : bar_h;
  layout_.bottom_h = bar_h;
  layout_.title_w = panel_.round ? std::min(w, 120 * s) : w;
  layout_.header_h = Canvas::line_height(s + 1) + 4 * s;
  layout_.main_y = layout_.top_h;
  layout_.main_h = h - layout_.top_h - layout_.bottom_h;
  int margin = 4 * s;
  int content_h = h - layout_.top_h - layout_.header_h - layout_.bottom_h;
  layout_.body_cols = cols_for(w - 2 * margin, s);
  layout_.body_rows = std::max(1, (content_h - margin) / Canvas::line_height(s));

  // Hero layout: the face is sized per screen (see hero_geom).
  layout_.hero_cols = cols_for(w - 4 * s, s);
}

void Ui::invalidate() { valid_ = false; }

Canvas Ui::canvas() {
  return Canvas(display_.framebuffer() + oy_ * panel_.width + ox_, info_.width, info_.height, info_.swap_bytes,
                panel_.width);
}

void Ui::flush(int y0, int y1) { push(y0 + oy_, y1 + oy_); }

void Ui::push(int y0, int y1) {
  if (!stats_) {
    display_.flush(static_cast<uint16_t>(y0), static_cast<uint16_t>(y1));
    return;
  }
  const uint64_t t0 = clock_->now_us();
  display_.flush(static_cast<uint16_t>(y0), static_cast<uint16_t>(y1));
  flush_us_ += clock_->now_us() - t0;
  ++bands_;
}

void Ui::measure(FrameStats* stats, System* clock) {
  stats_ = clock ? stats : nullptr;
  clock_ = clock;
  input_pending_ = false;
}

void Ui::mark_input() {
  if (!stats_ || input_pending_) return;  // the first input waiting is the one timed
  input_at_ = clock_->now_us();
  input_pending_ = true;
}

void Ui::render(const UiModel& m) {
  if (!stats_) {
    draw_frame(m);
    return;
  }
  flush_us_ = 0;
  bands_ = 0;
  const uint64_t t0 = clock_->now_us();
  draw_frame(m);
  const uint64_t t1 = clock_->now_us();
  auto us = [](uint64_t d) { return static_cast<uint32_t>(std::min<uint64_t>(d, UINT32_MAX)); };
  stats_->add_frame(us(t1 - t0), us(flush_us_), bands_);
  if (input_pending_ && bands_) {
    stats_->add_input(us(t1 - input_at_));
    input_pending_ = false;
  }
}

void Ui::draw_frame(const UiModel& m) {
  const int h = info_.height;
  if (!valid_ && (ox_ || oy_)) {
    // Round panel: everything outside the UI area stays the background colour.
    Canvas panel(display_.framebuffer(), panel_.width, panel_.height, panel_.swap_bytes);
    panel.fill_rect(0, 0, panel_.width, panel_.height, bg_);
    push(0, panel_.height);
  }
  if (panel_.round) draw_round_battery(m);
  Canvas c = canvas();
  const int y_header = layout_.top_h;
  const int y_content = y_header + layout_.header_h;
  const int y_bottom = h - layout_.bottom_h;

  uint32_t hashes[4];
  hashes[0] = Hash().add(m.title).val(m.link).val(panel_.round && m.screen == Screen::Settings).val(panel_.round && m.settings_hold)
                  .val(panel_.round ? int8_t(-1) : m.battery).val(!panel_.round && m.battery_charging)
                  .val(!panel_.round && m.battery_icon).val(!panel_.round && m.battery_text)
                  .val(!panel_.round && m.battery_low).get();
  hashes[1] = Hash()
                  .val(m.screen)
                  .add(m.headline)
                  .val(animated(m.screen) ? m.frame : 0u)
                  .val(m.screen == Screen::Listening ? m.level : uint8_t(0))
                  .val(m.speaking)
                  .get();
  Hash content;
  content.val(m.screen).add(m.detail).add(m.body).add(m.code).val(m.scroll).val(m.color_test)
      .val(m.qr ? m.qr->size : 0);
  for (const DashRow& row : m.dash) content.add(row.label).add(row.value).val(row.tone).val(row.heading);
  hashes[2] = content.get();
  hashes[3] = Hash().add(m.hint).get();

  if (m.hero) {
    // Hero mode: top bar, one face band, bottom bar.
    for (int i : {0, 3}) {
      if (valid_ && hash_[i] == hashes[i]) continue;
      int y0 = i == 0 ? 0 : y_bottom, y1 = i == 0 ? y_header : h;
      c.set_clip_rows(y0, y1);
      if (i == 0) draw_top(c, m);
      else draw_bottom(c, m);
      flush(y0, y1);
      hash_[i] = hashes[i];
    }
    uint32_t stat = Hash().val(m.screen).add(m.headline).add(m.detail).add(m.yes).add(m.no).val(m.caption_lines).get();
    const HeroGeom geom = hero_geom(m);
    const int face_d = geom.size;
    uint32_t anim = hero_anim_key(m, face_d);
    int y0 = y_header, y1 = y_bottom;
    // A caption or screen change clears the whole hero area. A settled face
    // (blink, glance, breath, microphone, speaker) blits the cached body over
    // the union of the previous and current eye box and flushes only those
    // rows. Morphing, a shape blend, the sleep collapse and the swirl still
    // redraw the whole silhouette. Gating the flush on a picture key left the
    // watch on the last full paint (frames stayed at 0).
    const bool full = !valid_ || !hero_valid_ || stat != hero_static_;
    const bool moved = anim != hero_anim_ || face_live(m);
    const face::Style style = face_style(m);
    const int cx = geom.x + geom.size / 2;
    const int cy = geom.y + geom.size / 2;
    const bool settled = !full && !face::anim_wide(style) &&
                         face::body_cached(cx, cy, geom.size, m.screen, bg_, style, c.width(), c.swapped());
    if (full || moved) {
      if (settled) {
        face::Box cur = face::overlay_box(cx, cy, geom.size, m.screen, m.face_happy, m.face_frame, m.level, m.voice, style);
        face::Box dirty = face::unite(face::last_overlay(), cur);
        if (dirty.any) {
          dirty.x0 -= 2;
          dirty.y0 -= 2;
          dirty.x1 += 2;
          dirty.y1 += 2;
        }
        y0 = std::max(dirty.y0, y_header);
        y1 = std::min(dirty.y1, y_bottom);
        if (y1 < y0) y1 = y0;
        c.set_clip_rows(y0, y1);
        face::blit_body(c, dirty, bg_);
        face::draw_overlay(c, cx, cy, geom.size, m.screen, m.face_happy, m.face_frame, m.level, m.voice, style);
        flush(y0, y1);
      } else {
        if (!full) hero_anim_rows(m, y0, y1);
        c.set_clip_rows(y0, y1);
        draw_hero(c, m);
        flush(y0, y1);
      }
    }
    hero_static_ = stat;
    hero_anim_ = anim;
    hero_valid_ = true;
    hash_[1] = hash_[2] = 0;  // header and content must repaint when hero mode ends
    valid_ = true;
    return;
  }
  hero_valid_ = false;

  struct Band {
    int y0, y1;
    void (Ui::*draw)(Canvas&, const UiModel&);
  };
  const Band bands[4] = {
      {0, y_header, &Ui::draw_top},
      {y_header, y_content, &Ui::draw_header},
      {y_content, y_bottom, &Ui::draw_content},
      {y_bottom, h, &Ui::draw_bottom},
  };
  for (int i = 0; i < 4; ++i) {
    // An image owns the area between the bars; the app draws and flushes it.
    if (m.screen == Screen::Image && (i == 1 || i == 2)) {
      hash_[i] = 0;
      continue;
    }
    if (valid_ && hash_[i] == hashes[i]) continue;
    c.set_clip_rows(bands[i].y0, bands[i].y1);
    (this->*bands[i].draw)(c, m);
    flush(bands[i].y0, bands[i].y1);
    hash_[i] = hashes[i];
  }
  valid_ = true;
}

void Ui::draw_top(Canvas& c, const UiModel& m) {
  const int s = layout_.scale;
  const int w = info_.width;
  c.fill_rect(0, 0, w, layout_.top_h, panel_.round ? bg_ : kBar);
  uint16_t dot = kRed;
  const char* label = "OFFLINE";
  switch (m.link) {
    case Link::Offline: dot = kRed; label = "NO NET"; break;
    case Link::Network: dot = kYellow; label = "NET"; break;
    case Link::Connecting: dot = kYellow; label = "LINK"; break;
    case Link::Online: dot = kGreen; label = "ONLINE"; break;
  }
  int ty = s;
  if (panel_.round) {
    // The settings hold target: a dim label under the link dot, with no bar
    // behind it. The label line (2r + 4s .. 2r + 13s) fits the band.
    int r = std::max(2, 3 * s / 2 + 1);
    if (m.settings_hold) {
      const char* control = m.screen == Screen::Settings ? "BACK TO HERMES" : "SETTINGS";
      c.text((w - Canvas::text_width(control, s)) / 2, 2 * r + 4 * s, control, s, kDim);
    }
    c.fill_circle(w / 2, s + r, r, dot);
    return;
  }
  const int pad = 3 * s + panel_.corner_inset;
  // The link cluster is the rightmost ink. On a ~100 px corner the arc at the
  // top of the bar still covers a plain `corner_inset`, so the cluster steps
  // further left than the title. Square panels (inset 0) are unchanged.
  const int right_pad = pad + panel_.corner_inset / 2;
  int label_w = Canvas::text_width(label, s);
  int label_x = w - right_pad - label_w;
  ty = std::max(s, (layout_.top_h - font::kGlyphHeight * s) / 2);
  c.text(label_x, ty, label, s, kDim);
  int r = std::max(2, 3 * s / 2 + 1);
  const int dot_x = label_x - 3 * s - r;
  c.fill_circle(dot_x, layout_.top_h / 2, r, dot);
  int title_end = dot_x - r;
  if (m.battery >= 0) {
    title_end -= 4 * s + battery_width(m);
    draw_battery(c, m, title_end, layout_.top_h / 2);
  }
  int title_cols = cols_for(title_end - 3 * s - pad, s);
  c.text(pad, ty, fit(m.title, title_cols), s, kText);
}

// Round panels: the battery is centred at the very top of the panel, above the UI
// square, as high as the circle leaves it room plus a small margin.
void Ui::draw_round_battery(const UiModel& m) {
  const int s = layout_.scale;
  const int bh = 7 * s;  // the taller of the icon (5s + 1) and the text (7s)
  const int hw = (m.battery >= 0 ? battery_width(m) : 0) / 2 + 2 * s;
  const int radius = panel_.width / 2;
  int y0 = 0;
  // The first row where the strip's top corners lie inside the circle.
  while (y0 < oy_ && (radius - y0) * (radius - y0) + hw * hw > radius * radius) ++y0;
  y0 = std::min(y0 + 3 * s, std::max(0, oy_ - bh));
  const int y1 = std::min<int>(oy_, y0 + bh);
  const uint32_t key = Hash().val(m.battery).val(m.battery_charging).val(m.battery_low).val(m.battery_icon)
                           .val(m.battery_text).val(y0).get();
  if (valid_ && key == battery_hash_) return;
  battery_hash_ = key;
  Canvas panel(display_.framebuffer(), panel_.width, panel_.height, panel_.swap_bytes);
  // Clear the band the strip can occupy at any level, so a shorter or hidden strip leaves nothing behind.
  panel.fill_rect(0, 0, panel_.width, oy_, bg_);
  if (m.battery >= 0) draw_battery(panel, m, (panel_.width - battery_width(m)) / 2, (y0 + y1) / 2);
  push(0, oy_);
}

int Ui::battery_width(const UiModel& m) const {
  const int s = layout_.scale;
  const int icon = m.battery_icon ? 10 * s : 0;
  const int text = m.battery_text ? Canvas::text_width(std::to_string(m.battery) + "%", s) : 0;
  return icon + text + (icon && text ? 2 * s : 0);
}

// A battery outline with a terminal nub, filled to the level, then the percent.
void Ui::draw_battery(Canvas& c, const UiModel& m, int x, int cy) {
  const int s = layout_.scale;
  if (m.battery_icon) {
    const int bw = 9 * s, bh = 5 * s + 1, t = std::max(1, s / 2);
    const int y = cy - bh / 2;
    const uint16_t fill = m.battery_charging ? kGreen : m.battery_low ? kRed : kText;
    c.fill_rect(x, y, bw, bh, kDim);
    c.fill_rect(x + t, y + t, bw - 2 * t, bh - 2 * t, panel_.round ? bg_ : kBar);
    c.fill_rect(x + bw, cy - s, s, 2 * s, kDim);
    const int inner = bw - 4 * t;
    const int level = std::max(m.battery > 0 ? 1 : 0, inner * std::min<int>(m.battery, 100) / 100);
    c.fill_rect(x + 2 * t, y + 2 * t, level, bh - 4 * t, fill);
    x += 12 * s;
  }
  if (m.battery_text) {
    // Without the icon, the text itself turns green while charging.
    const uint16_t col = m.battery_charging ? (m.battery_icon ? kDim : kGreen) : m.battery_low ? kRed : kDim;
    c.text(x, cy - (font::kGlyphHeight * s) / 2, std::to_string(m.battery) + "%", s, col);
  }
}

void Ui::draw_indicator(Canvas& c, const UiModel& m, int cx, int cy, int r) {
  const int s = layout_.scale;
  auto spinner = [&](uint16_t color) {
    int dot_r = std::max(1, r / 5);
    // Eight positions around the circle; precomputed unit vectors * 100.
    static const int kDx[8] = {0, 71, 100, 71, 0, -71, -100, -71};
    static const int kDy[8] = {-100, -71, 0, 71, 100, 71, 0, -71};
    int ring_r = r - dot_r;
    int head = static_cast<int>(m.frame % 8);
    for (int i = 0; i < 8; ++i) {
      int age = (head - i + 8) % 8;
      uint16_t col = age == 0 ? color : (age < 3 ? kDim : kFaint);
      c.fill_circle(cx + kDx[i] * ring_r / 100, cy + kDy[i] * ring_r / 100, dot_r, col);
    }
  };
  auto bars = [&](uint16_t color, int level_pct) {
    const int n = 5;
    int bw = std::max(3, (2 * r) / (2 * n - 1));
    int x0 = cx - (n * bw + (n - 1) * bw) / 2;
    for (int i = 0; i < n; ++i) {
      int jitter = static_cast<int>((m.frame * 3 + static_cast<uint32_t>(i) * 5) % 7);
      int pct = std::max(10, level_pct * (4 + jitter) / 10);
      int bh = std::max(2 * s, std::min(2 * r, 2 * r * pct / 100));
      c.fill_rect(x0 + i * 2 * bw, cy - bh / 2, bw, bh, color);
    }
  };

  switch (m.screen) {
    case Screen::Boot:
    case Screen::Connecting:
    case Screen::Updating:
      spinner(kAccent);
      break;
    case Screen::Thinking:
      spinner(kBlue);
      break;
    case Screen::Offline:
    case Screen::Error: {
      c.fill_circle(cx, cy, r, kRed);
      int tw = Canvas::text_width("!", s);
      c.text(cx - tw / 2, cy - (7 * s) / 2, "!", s, bg_);
      break;
    }
    case Screen::Pairing:
      c.ring(cx, cy, r, s + 1, kAccent);
      c.fill_circle(cx, cy, r / 3, kAccent);
      break;
    case Screen::Ready: {
      int pulse = tri(m.frame, 24);  // 0..12
      c.fill_circle(cx, cy, r * (8 + pulse / 3) / 12, kAccent);
      break;
    }
    case Screen::Listening:
      bars(kGreen, std::max<int>(m.level, 8));
      break;
    case Screen::Responding:
      if (m.speaking) {
        bars(kAccent, 40 + tri(m.frame, 8) * 10);
      } else {
        c.fill_circle(cx, cy, r * 2 / 3, kAccent);
      }
      break;
    case Screen::Card:
      c.fill_round_rect(cx - r, cy - r, 2 * r, 2 * r, r / 3, kAccent);
      break;
    case Screen::Prompt: {
      c.fill_circle(cx, cy, r, kAccent);
      int tw = Canvas::text_width("?", s);
      c.text(cx - tw / 2, cy - (7 * s) / 2, "?", s, bg_);
      break;
    }
    case Screen::Dashboard: {
      // A 2x2 grid of tiles.
      const int g = std::max(1, r / 6), t = r - g / 2;
      c.fill_round_rect(cx - r, cy - r, t, t, s, kAccent);
      c.fill_round_rect(cx + g / 2 + (g & 1), cy - r, t, t, s, kAccent);
      c.fill_round_rect(cx - r, cy + g / 2 + (g & 1), t, t, s, kAccent);
      c.fill_round_rect(cx + g / 2 + (g & 1), cy + g / 2 + (g & 1), t, t, s, kAccent);
      break;
    }
    case Screen::Image:
    case Screen::Settings:
    case Screen::Setup:
      break;
  }
}

void Ui::draw_header(Canvas& c, const UiModel& m) {
  const int s = layout_.scale;
  const int y0 = layout_.top_h;
  const int hh = layout_.header_h;
  c.fill_rect(0, y0, info_.width, hh, bg_);
  int r = hh / 2 - 2 * s;
  int cx = 4 * s + r;
  int cy = y0 + hh / 2;
  draw_indicator(c, m, cx, cy, r);
  int tx = cx + r + 4 * s;
  int hs = s + 1;
  int cols = cols_for(info_.width - tx - 2 * s, hs);
  c.text(tx, cy - (font::kGlyphHeight * hs) / 2, fit(m.headline, cols), hs, kText);
  c.fill_rect(4 * s, y0 + hh - 1, info_.width - 8 * s, 1, kFaint);
}

int Ui::body_rows(const UiModel& m) const {
  int rows = layout_.body_rows;
  if (m.detail.empty()) return rows;
  int detail = static_cast<int>(wrap_text(m.detail, layout_.body_cols).size());
  int max_detail = m.body.empty() ? rows : std::min(2, rows - 1);
  return rows - std::max(0, std::min(detail, max_detail));
}

void Ui::draw_content(Canvas& c, const UiModel& m) {
  const int s = layout_.scale;
  const int w = info_.width;
  const int y0 = layout_.top_h + layout_.header_h;
  const int y1 = info_.height - layout_.bottom_h;
  const int margin = 4 * s;
  const int lh = Canvas::line_height(s);
  c.fill_rect(0, y0, w, y1 - y0, bg_);
  int y = y0 + margin;

  if (m.color_test) {
    const uint16_t colors[] = {rgb565(255, 0, 0), rgb565(0, 255, 0), rgb565(0, 0, 255),
                               rgb565(255, 255, 255), rgb565(0, 0, 0)};
    for (int i = 0; i < 5; ++i) {
      const int left = w * i / 5, right = w * (i + 1) / 5;
      c.fill_rect(left, y0, right - left, y1 - y0, colors[i]);
    }
    return;
  }

  if (m.qr && m.qr->ok() && draw_qr(c, m, y0, y1)) return;
  if (m.screen == Screen::Dashboard) {
    draw_dashboard(c, m, y0, y1);
    return;
  }

  if (m.screen == Screen::Boot) {
    int big = s + 2;
    const char* name = "HERMES";
    int cy = (y0 + y1) / 2 - Canvas::line_height(big) / 2;
    c.text((w - Canvas::text_width(name, big)) / 2, cy, name, big, kAccent);
    int sy = cy + Canvas::line_height(big);
    c.text((w - Canvas::text_width("gadget", s)) / 2, sy, "gadget", s, kDim);
    if (!m.detail.empty()) {
      std::string d = fit(m.detail, layout_.body_cols);
      c.text((w - Canvas::text_width(d, s)) / 2, y1 - lh, d, s, kFaint);
    }
    return;
  }

  int rows = body_rows(m);
  if (!m.detail.empty()) {
    uint16_t dcol = m.screen == Screen::Error || m.screen == Screen::Offline ? kRed : kDim;
    if (m.screen == Screen::Thinking) dcol = kBlue;
    auto detail_lines = wrap_text(m.detail, layout_.body_cols);
    int shown = layout_.body_rows - rows;
    for (int i = 0; i < shown; ++i) {
      c.text(margin, y, detail_lines[static_cast<size_t>(i)], s, dcol);
      y += lh;
    }
  }
  if (m.body.empty() || rows <= 0) return;

  auto lines = wrap_text(m.body, layout_.body_cols);
  int total = static_cast<int>(lines.size());
  int max_first = std::max(0, total - rows);
  int first = m.scroll < 0 ? max_first : std::min(m.scroll, max_first);
  uint16_t col = m.screen == Screen::Ready ? kDim : kText;
  for (int i = 0; i < rows && first + i < total; ++i) {
    c.text(margin, y + i * lh, lines[static_cast<size_t>(first + i)], s, col);
  }
  if (total > rows) {
    // Scroll indicator along the right edge.
    int track_y = y, track_h = rows * lh;
    int thumb_h = std::max(2 * s, track_h * rows / total);
    int thumb_y = track_y + (track_h - thumb_h) * first / std::max(1, max_first);
    c.fill_rect(w - 2 * s, track_y, s, track_h, kFaint);
    c.fill_rect(w - 2 * s, thumb_y, s, thumb_h, kDim);
  }
}

int Ui::dash_rows() const {
  const int s = layout_.scale;
  const int content_h = info_.height - layout_.top_h - layout_.header_h - layout_.bottom_h;
  return std::max(1, (content_h - 4 * s) / (Canvas::line_height(s) + 2 * s));
}

// The dashboard: one row per session or tile, a dot in its state's colour, the
// label on the left and the value on the right. Headings start a section.
void Ui::draw_dashboard(Canvas& c, const UiModel& m, int y0, int y1) {
  const int s = layout_.scale;
  const int w = info_.width;
  const int margin = 4 * s;
  const int row_h = Canvas::line_height(s) + 2 * s;
  const int glyph_h = font::kGlyphHeight * s;
  const int rows = dash_rows();
  const int total = static_cast<int>(m.dash.size());
  const int max_first = std::max(0, total - rows);
  const int first = std::max(0, std::min(m.scroll, max_first));
  const bool scrolls = total > rows;
  const int right = w - margin - (scrolls ? 2 * s : 0);
  const int r = std::max(2, glyph_h / 3);
  int y = y0 + margin;
  for (int i = 0; i < rows && first + i < total; ++i, y += row_h) {
    const DashRow& row = m.dash[static_cast<size_t>(first + i)];
    const int ty = y + (row_h - glyph_h) / 2;
    if (row.heading) {
      c.text(margin, ty, fit(row.label, cols_for(right - margin, s)), s, kDim);
      c.fill_rect(margin, y + row_h - 1, right - margin, 1, kFaint);
      continue;
    }
    c.fill_circle(margin + r, ty + glyph_h / 2, r, tone_color(row.tone));
    const int lx = margin + 2 * r + 3 * s;
    const int cols = cols_for(right - lx, s);
    // The value keeps up to half the row; the label gets the rest, less a space.
    const std::string value = fit(row.value, std::min<int>(static_cast<int>(row.value.size()), cols / 2));
    const int value_cols = static_cast<int>(value.size());
    c.text(lx, ty, fit(row.label, cols - value_cols - (value_cols ? 1 : 0)), s, kText);
    if (value_cols) c.text(right - Canvas::text_width(value, s), ty, value, s, tone_color(row.tone));
  }
  if (scrolls) {
    const int track_y = y0 + margin, track_h = std::min(rows * row_h, y1 - track_y);
    const int thumb_h = std::max(2 * s, track_h * rows / total);
    const int thumb_y = track_y + (track_h - thumb_h) * first / std::max(1, max_first);
    c.fill_rect(w - 2 * s, track_y, s, track_h, kFaint);
    c.fill_rect(w - 2 * s, thumb_y, s, thumb_h, kDim);
  }
}

// A Wi-Fi setup code: a white panel with a four-module quiet zone, one square
// per module, at the largest integer scale that fits the content band. A phone
// camera needs the quiet zone and dark-on-light contrast, so neither is optional.
// Returns false, drawing nothing, when the code does not fit beside the text.
bool Ui::draw_qr(Canvas& c, const UiModel& m, int y0, int y1) {
  const int n = m.qr->size;
  const int w = info_.width;
  const int s = layout_.scale;
  const int lh = Canvas::line_height(s);
  const int margin = 4 * s;
  const int pad = 4;                     // quiet zone, in modules
  const int cells = n + 2 * pad;         // the code plus its quiet zone
  const int avail_h = y1 - y0 - 2 * margin;

  // The code only joins the network: the phone still needs the address, and a
  // phone that cannot scan needs the name and password. So every line of the
  // instructions stays on screen, and the code is shown only where it fits
  // beside them at kMinScale or larger, below which it is not reliably
  // scannable. Otherwise the caller draws the text-only screen.
  constexpr int kMinScale = 3;
  const std::vector<std::string> lines = wrap_text(m.body, layout_.body_cols);
  const int text_rows = static_cast<int>(lines.size());
  const int reserve = text_rows ? text_rows * lh + 2 * s : 0;
  const int scale = std::min((w - 2 * margin) / cells, (avail_h - reserve) / cells);
  if (scale < kMinScale) return false;

  const int side = cells * scale;
  const int x0 = (w - side) / 2;
  const int block = side + reserve;
  const int py0 = y0 + margin + std::max(0, (avail_h - block) / 2);
  c.fill_rect(x0, py0, side, side, rgb565(255, 255, 255));
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      if (m.qr->at(x, y)) {
        c.fill_rect(x0 + (x + pad) * scale, py0 + (y + pad) * scale, scale, scale, rgb565(0, 0, 0));
      }
    }
  }
  int ty = py0 + side + 2 * s;
  for (int i = 0; i < text_rows; ++i) {
    c.text((w - Canvas::text_width(lines[static_cast<size_t>(i)], s)) / 2, ty, lines[static_cast<size_t>(i)], s, kDim);
    ty += lh;
  }
  return true;
}

Ui::HeroGeom Ui::hero_geom(const UiModel& m) const {
  HeroGeom g;
  const int s = layout_.scale, w = info_.width, lh = Canvas::line_height(s);
  const int cols = layout_.hero_cols;
  if (!m.detail.empty()) g.detail = wrap_text(m.detail, cols);
  const int buttons_h = m.yes.empty() && m.no.empty() ? 0 : lh + 6 * s;
  auto caption_h = [&](int lines) { return (1 + lines) * lh + s; };
  // Drop caption lines before the face gets too small to read.
  constexpr int kSmallestFace = 48;
  int max_lines = std::max(1, static_cast<int>(m.caption_lines));
  while (max_lines > 1 && layout_.main_h - caption_h(max_lines) - buttons_h - 4 * s < kSmallestFace) --max_lines;
  if (static_cast<int>(g.detail.size()) > max_lines) {
    g.detail.resize(static_cast<size_t>(max_lines));
    std::string& last = g.detail.back();
    if (static_cast<int>(last.size()) + 2 > cols) last.resize(static_cast<size_t>(std::max(0, cols - 2)));
    last += "..";
  }
  // Reserve one detail line even when there is none, so the face keeps its size.
  const int ch = caption_h(std::max<int>(1, static_cast<int>(g.detail.size())));
  const int gap = lh;  // one clear text line under the silhouette, above the caption
  // Rings reach 74% of the diameter from the centre, which is 24% past the
  // disc. The caption starts below that, or it is drawn across the body.
  constexpr int kOverhangPct = 24;
  const int room = layout_.main_h - ch - buttons_h - gap - 4 * s;
  const int max_by_h = std::max(16, room > 0 ? room * 100 / (100 + kOverhangPct) : 16);
  const int max_by_w = std::max(16, (w - 8 * s) * 66 / 100);
  g.size = std::min(max_by_h, max_by_w);
  const int overhang = std::max(s, g.size * kOverhangPct / 100);
  const int block = g.size + overhang + gap + ch + buttons_h;
  g.x = (w - g.size) / 2;
  g.y = layout_.main_y + std::max(s, (layout_.main_h - block) / 2);
  g.caption_y = g.y + g.size + overhang + gap;
  g.buttons_y = g.caption_y + ch + 2 * s;
  return g;
}

void Ui::hero_anim_rows(const UiModel& m, int& y0, int& y1) const {
  const HeroGeom g = hero_geom(m);
  const int s = layout_.scale;
  const int lh = Canvas::line_height(s);
  int top = 0, bottom = 0;
  // Disc, rings, dots, the bang and the morph all sit inside the wide rect.
  // The caption is in the same flush so a later tick cannot leave the line
  // painted across the body.
  face::anim_rows(g.y + g.size / 2, g.size, top, bottom, true, 0);
  const int lines = 1 + static_cast<int>(g.detail.size());
  bottom = std::max(bottom, g.caption_y + lines * lh + s);
  if (!m.yes.empty() || !m.no.empty()) bottom = std::max(bottom, g.buttons_y + lh + 4 * s);
  if (g.y < top) top = g.y;
  y0 = std::max(top - 2, layout_.top_h);
  y1 = std::min(bottom + 2, static_cast<int>(info_.height) - layout_.bottom_h);
  if (y1 < y0) y1 = y0;
}

void Ui::draw_hero(Canvas& c, const UiModel& m) {
  const int s = layout_.scale;
  const int w = info_.width;
  const int y0 = layout_.top_h;
  const int y1 = info_.height - layout_.bottom_h;
  const int lh = Canvas::line_height(s);
  c.fill_rect(0, y0, w, y1 - y0, bg_);

  const HeroGeom g = hero_geom(m);
  face::draw(c, g.x + g.size / 2, g.y + g.size / 2, g.size, m.screen, m.face_happy, m.face_frame, m.level, m.voice,
             face_style(m), bg_);

  // Caption: headline, then the detail lines, centred.
  const int cols = layout_.hero_cols;
  std::string head = fit(m.headline, cols);
  int cy = g.caption_y;
  c.text((w - Canvas::text_width(head, s)) / 2, cy, head, s, caption_color(m.screen));
  for (const auto& line : g.detail) {
    cy += lh;
    c.text((w - Canvas::text_width(line, s)) / 2, cy, line, s, kDim);
  }

  // Answer buttons (Prompt screen): TALK on the left, CANCEL on the right.
  if (!m.yes.empty() || !m.no.empty()) {
    const int bh = lh + 4 * s, gap = 4 * s, margin = 4 * s;
    const int bw = (w - 2 * margin - gap) / 2;
    auto button = [&](int x, const std::string& label, uint16_t fill) {
      c.fill_round_rect(x, g.buttons_y, bw, bh, 3 * s, fill);
      std::string t = fit(label, cols_for(bw - 2 * s, s));
      c.text(x + (bw - Canvas::text_width(t, s)) / 2, g.buttons_y + 2 * s + s / 2, t, s, kText);
    };
    if (!m.yes.empty()) button(margin, m.yes, kGreenDim);
    if (!m.no.empty()) button(margin + bw + gap, m.no, kRedDim);
  }
}

void Ui::draw_bottom(Canvas& c, const UiModel& m) {
  const int s = layout_.scale;
  const int y0 = info_.height - layout_.bottom_h;
  c.fill_rect(0, y0, info_.width, layout_.bottom_h, panel_.round ? bg_ : kBar);
  std::string hint = fit(m.hint, cols_for(info_.width - 4 * s, s));
  c.text((info_.width - Canvas::text_width(hint, s)) / 2, y0 + s, hint, s, kDim);
}

}  // namespace hg
