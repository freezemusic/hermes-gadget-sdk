// See face.hpp for the zongzhu-bot licence and what was adapted.
#include "hg/face.hpp"

#include <algorithm>

namespace hg::face {
namespace {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return rgb565(r, g, b); }

// Blink starts tile the timeline so the gap is O(1) in the frame number.
// start(i) = i*129 + 30 + (mix(i) % 52). The gap between starts is
// 129 + off(i+1) - off(i), which lands in 78..180 frames: 2.6 s .. 6.0 s at
// 30 fps. A single blink is 7 frames (close 2, hold 1, open 4: about 70, 30
// and 140 ms). Every 5th blink (index % 5 == 4) is a double: the same pulse,
// a 2-frame gap, and the pulse again.
constexpr uint32_t kBlinkWindow = 129;
constexpr uint32_t kBlinkBase = 30;
constexpr uint32_t kBlinkJitter = 52;

// Glances: start(i) = i*70 + (mix % 35). Gap is 36..104 frames, 1.2 s .. 3.5 s.
constexpr uint32_t kGlanceWindow = 70;
constexpr uint32_t kGlanceJitter = 35;

// Exponential ease 1-exp(-n*0.033*11), in thousandths, for n = 0..15.
// Upstream uses 1-exp(-dt*k) with k ≈ 11/s; one face frame is the dt.
constexpr int kEase[16] = {0, 304, 516, 663, 765, 837, 886, 921, 945, 962, 973, 981, 987, 991, 994, 996};

// Unit sine, 64 steps, thousandths.
constexpr int kSin[64] = {0,    98,   195,  290,  383,  471,  556,  634,  707,  773,  831,  882,  924,  957,  981,  995,
                          1000, 995,  981,  957,  924,  882,  831,  773,  707,  634,  556,  471,  383,  290,  195,  98,
                          0,    -98,  -195, -290, -383, -471, -556, -634, -707, -773, -831, -882, -924, -957, -981, -995,
                          -1000, -995, -981, -957, -924, -882, -831, -773, -707, -634, -556, -471, -383, -290, -195, -98};

// Idle breath, thousandths of diameter added to eye height. ~0.3 Hz.
constexpr int kBreathAmp = 8;
// Speaking bounce on top of the voice term. ~5.2 Hz (11 steps of the 64-entry
// sine per frame: 11/64*30).
constexpr int kSpeakAmp = 14;
constexpr int kSpeakVoice = 32;  // extra thousandths at voice == 100

// Eye-band limits, thousandths of the diameter from the face centre. Poses,
// glances and the thinking dots stay inside this, so a partial flush cannot
// leave a trail.
constexpr int kBandAbove = 250;
constexpr int kBandBelow = 160;

uint32_t mix32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

int sine(uint32_t index) { return kSin[index & 63u]; }

uint32_t blink_start(uint32_t i) { return i * kBlinkWindow + kBlinkBase + mix32(i + 1) % kBlinkJitter; }

uint32_t glance_start(uint32_t i) { return i * kGlanceWindow + mix32(i + 17) % kGlanceJitter; }

int pulse(int age) {
  // Close over 2 frames, hold 1, open over 4. The last sample is fully open.
  static const int k[7] = {520, 1000, 1000, 750, 500, 250, 0};
  if (age < 0 || age > 6) return 0;
  return k[age];
}

int blink_amount(uint32_t frame) {
  if (frame < kBlinkBase) return 0;
  const uint32_t i = (frame - kBlinkBase) / kBlinkWindow;
  const uint32_t start = blink_start(i);
  if (frame < start) return 0;
  const uint32_t age = frame - start;
  const bool dbl = (i % 5u) == 4u;
  if (!dbl) return age < 7u ? pulse(static_cast<int>(age)) : 0;
  if (age < 7u) return pulse(static_cast<int>(age));
  if (age < 9u) return 0;
  if (age < 16u) return pulse(static_cast<int>(age - 9u));
  return 0;
}

void gaze(uint32_t frame, Mood mood, int& gx, int& gy) {
  gx = gy = 0;
  if (mood == Mood::Sleepy) return;
  uint32_t i = frame / kGlanceWindow;
  uint32_t start = glance_start(i);
  if (frame < start) {
    if (i == 0) return;
    --i;
    start = glance_start(i);
  }
  const uint32_t next = glance_start(i + 1);
  const uint32_t dur = next - start;
  if (dur == 0) return;
  const uint32_t age = frame - start;
  const uint32_t h = mix32(i + 3);
  if ((h % 4u) == 0) return;  // some glances stay put, so the face returns to centre
  const int tx = static_cast<int>(h % 101u) - 50;          // about ±5% of the disc
  const int ty = static_cast<int>((h >> 8) % 81u) - 40;    // a little less vertically
  uint32_t edge = dur / 4;
  if (edge < 4) edge = 4;
  if (edge > 12) edge = 12;
  int k;
  if (age < edge) {
    k = kEase[std::min<uint32_t>(15, age * 15u / (edge - 1))];
  } else if (age + edge >= dur) {
    const uint32_t back = dur - age;
    k = back == 0 ? 0 : kEase[std::min<uint32_t>(15, back * 15u / edge)];
  } else {
    k = 1000;
  }
  gx = tx * k / 1000;
  gy = ty * k / 1000;
}

Eye mirror(Eye e) {
  e.x = static_cast<int16_t>(-e.x);
  return e;
}

// Base pose before gaze, breath, speaking bounce and blink. Sizes are
// thousandths of the disc. Eyes sit a little above centre, the way the round
// avatar reads: short dark pills, a small gap, a few degrees of tilt.
Eye pose_left(Mood mood, uint8_t level) {
  Eye e;
  e.x = -82;
  e.y = -46;
  e.w = 76;
  e.h = 158;
  e.tilt = 8;
  switch (mood) {
    case Mood::Attentive: {
      const int loud = std::min(100, static_cast<int>(level));
      e.y = -60;
      e.w = static_cast<int16_t>(84 + loud / 8);
      e.h = static_cast<int16_t>(178 + loud * 36 / 100);
      e.tilt = 5;
      break;
    }
    case Mood::Curious:
      e.x = -98;
      e.y = -52;
      e.w = 80;
      e.h = 168;
      e.tilt = -16;
      break;
    case Mood::Thinking:
      e.x = -108;
      e.y = -96;
      e.w = 72;
      e.h = 136;
      e.tilt = -8;
      break;
    case Mood::Speaking:
      break;
    case Mood::Happy:
      e.x = -90;
      e.y = -24;
      e.w = 136;
      e.h = 42;
      e.tilt = 22;
      break;
    case Mood::Confused:
      e.y = -28;
      e.h = 148;
      e.tilt = 12;
      break;
    case Mood::Sleepy:
      e.x = -72;
      e.y = 30;
      e.w = 30;
      e.h = 30;
      e.tilt = 0;
      break;
    case Mood::Neutral:
      break;
  }
  return e;
}

Eye pose_right(Mood mood, const Eye& left, uint8_t level) {
  (void)level;
  Eye e = mirror(left);
  switch (mood) {
    case Mood::Curious:
      e.x = 72;
      e.y = -34;
      e.w = 62;
      e.h = 108;
      e.tilt = 14;
      break;
    case Mood::Thinking:
      // Both eyes up and to the left, still a pair rather than stacked.
      e.x = -12;
      e.y = -100;
      e.w = 66;
      e.h = 92;
      e.tilt = 12;
      break;
    case Mood::Happy:
      e.tilt = -22;
      break;
    case Mood::Confused:
      e.x = 80;
      e.y = -36;
      e.w = 122;
      e.h = 38;
      e.tilt = -24;
      break;
    default:
      break;
  }
  return e;
}

int chord(int r, int dy) {
  if (dy < -r || dy > r) return -1;
  const int rest = r * r - dy * dy;
  int lo = 0, hi = r;
  while (lo < hi) {
    const int mid = (lo + hi + 1) / 2;
    if (mid * mid <= rest) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

void shade_disc(Canvas& c, int cx, int cy, int r, const Palette& pal) {
  if (r < 1) return;
  // Gloss stays in the upper-left and ends above the eyes, so the body colour
  // is what the pills sit on. The shade is only the lower-right crescent.
  // There is no antialiasing, so each step is a hard disc; keeping the steps
  // small and stacked reads as one highlight rather than a second body.
  const int mx = cx - r * 36 / 100, my = cy - r * 52 / 100, mr = std::max(1, r * 40 / 100);
  const int hx = cx - r * 48 / 100, hy = cy - r * 62 / 100, hr = std::max(1, r * 26 / 100);
  const int kx = cx - r * 56 / 100, ky = cy - r * 70 / 100, kr = std::max(1, r * 13 / 100);
  const int sx = cx + r * 50 / 100, sy = cy + r * 56 / 100, sr = std::max(1, r * 46 / 100);
  for (int dy = -r; dy <= r; ++dy) {
    const int dx = chord(r, dy);
    if (dx < 0) continue;
    const int y = cy + dy;
    const int x0 = cx - dx, x1 = cx + dx + 1;
    c.fill_rect(x0, y, x1 - x0, 1, pal.body);
    auto blob = [&](int bx, int by, int br, uint16_t color) {
      const int bdx = chord(br, y - by);
      if (bdx < 0) return;
      const int a0 = std::max(x0, bx - bdx);
      const int a1 = std::min(x1, bx + bdx + 1);
      if (a1 > a0) c.fill_rect(a0, y, a1 - a0, 1, color);
    };
    blob(sx, sy, sr, pal.shade);
    blob(mx, my, mr, pal.mid);
    blob(hx, hy, hr, pal.highlight);
    blob(kx, ky, kr, pal.core);
  }
}

int drawn_h(int h, int blink) {
  // A shut blink keeps a thin line (about 8% of the open height).
  int hh = h * (1000 - blink * 92 / 100) / 1000;
  if (hh < 8) hh = 8;
  return hh;
}

class Key {
 public:
  Key& val(int v) {
    h_ ^= static_cast<uint32_t>(v);
    h_ *= 16777619u;
    return *this;
  }
  uint32_t get() const { return h_; }

 private:
  uint32_t h_ = 2166136261u;
};

}  // namespace

Mood mood_for(Screen screen, bool happy) {
  if (happy && screen == Screen::Ready) return Mood::Happy;
  switch (screen) {
    case Screen::Listening:
    case Screen::Prompt:
      return Mood::Attentive;
    case Screen::Thinking:
      return Mood::Thinking;
    case Screen::Responding:
      return Mood::Speaking;
    case Screen::Error:
      return Mood::Confused;
    case Screen::Pairing:
    case Screen::Connecting:
    case Screen::Updating:
      return Mood::Curious;
    case Screen::Offline:
      return Mood::Sleepy;
    default:
      return Mood::Neutral;
  }
}

bool dots_for(Screen screen) {
  return screen == Screen::Thinking || screen == Screen::Connecting || screen == Screen::Updating;
}

static uint16_t mix_rgb(uint8_t ar, uint8_t ag, uint8_t ab, uint8_t br, uint8_t bg, uint8_t bb, int toward_b) {
  auto m = [&](int a, int b) { return static_cast<uint8_t>((a * (100 - toward_b) + b * toward_b) / 100); };
  return rgb(m(ar, br), m(ag, bg), m(ab, bb));
}

Palette palette_for(Screen screen) {
  Palette p;
  p.eye = rgb(kEyeR, kEyeG, kEyeB);
  uint8_t br, bg, bb, hr, hg, hb, cr, cg, cb, sr, sg, sb;
  if (screen == Screen::Error) {
    br = kRedR; bg = kRedG; bb = kRedB;
    hr = kRedHiR; hg = kRedHiG; hb = kRedHiB;
    cr = kRedCoreR; cg = kRedCoreG; cb = kRedCoreB;
    sr = kRedShadeR; sg = kRedShadeG; sb = kRedShadeB;
  } else if (screen == Screen::Offline) {
    br = kDimR; bg = kDimG; bb = kDimB;
    hr = kDimHiR; hg = kDimHiG; hb = kDimHiB;
    cr = kDimCoreR; cg = kDimCoreG; cb = kDimCoreB;
    sr = kDimShadeR; sg = kDimShadeG; sb = kDimShadeB;
  } else {
    br = kBlueR; bg = kBlueG; bb = kBlueB;
    hr = kBlueHiR; hg = kBlueHiG; hb = kBlueHiB;
    cr = kBlueCoreR; cg = kBlueCoreG; cb = kBlueCoreB;
    sr = kBlueShadeR; sg = kBlueShadeG; sb = kBlueShadeB;
  }
  p.body = rgb(br, bg, bb);
  p.mid = mix_rgb(br, bg, bb, hr, hg, hb, 45);
  p.highlight = rgb(hr, hg, hb);
  p.core = rgb(cr, cg, cb);
  p.shade = rgb(sr, sg, sb);
  return p;
}

Sample sample(uint32_t frame, Mood mood, uint8_t level, uint8_t voice, bool dots) {
  Sample s;
  s.left = pose_left(mood, level);
  s.right = pose_right(mood, s.left, level);
  int gx = 0, gy = 0;
  gaze(frame, mood, gx, gy);
  s.left.x = static_cast<int16_t>(s.left.x + gx);
  s.left.y = static_cast<int16_t>(s.left.y + gy);
  s.right.x = static_cast<int16_t>(s.right.x + gx);
  s.right.y = static_cast<int16_t>(s.right.y + gy);
  if (mood == Mood::Neutral || mood == Mood::Attentive || mood == Mood::Curious) {
    const int breath = sine((frame * 21u) >> 5) * kBreathAmp / 1000;
    s.left.h = static_cast<int16_t>(s.left.h + breath);
    s.right.h = static_cast<int16_t>(s.right.h + breath);
  }
  if (mood == Mood::Speaking) {
    const int amp = kSpeakAmp + static_cast<int>(std::min<uint8_t>(voice, 100)) * kSpeakVoice / 100;
    const int wobble = sine(frame * 11u) * amp / 1000;
    s.left.h = static_cast<int16_t>(s.left.h + wobble);
    s.right.h = static_cast<int16_t>(s.right.h + wobble);
    // A little width opposite the height, so the pill squashes rather than grows.
    s.left.w = static_cast<int16_t>(s.left.w - wobble / 3);
    s.right.w = static_cast<int16_t>(s.right.w - wobble / 3);
  }
  s.blink = static_cast<uint16_t>(blink_amount(frame));
  s.dots = dots ? static_cast<uint8_t>((frame / 5u) % 3u + 1u) : 0;
  return s;
}

uint32_t picture_key(uint32_t frame, Mood mood, uint8_t level, uint8_t voice, bool dots, int diameter) {
  const Sample s = sample(frame, mood, level, voice, dots);
  if (diameter < 1) diameter = 1;
  auto q = [&](int v) { return v * diameter / 1000; };
  Key k;
  k.val(q(s.left.x)).val(q(s.left.y)).val(q(s.left.w)).val(q(drawn_h(s.left.h, s.blink))).val(s.left.tilt);
  k.val(q(s.right.x)).val(q(s.right.y)).val(q(s.right.w)).val(q(drawn_h(s.right.h, s.blink))).val(s.right.tilt);
  k.val(s.dots);
  // The speaking bounce is meant to be sent every face frame; quantising the
  // sine to pixels would skip frames on a small disc and stall the bench.
  if (mood == Mood::Speaking) k.val(static_cast<int>(frame));
  return k.get();
}

void anim_rows(int cy, int diameter, int& y0, int& y1) {
  y0 = cy - diameter * kBandAbove / 1000 - 2;
  y1 = cy + diameter * kBandBelow / 1000 + 3;
}

void draw(Canvas& c, int cx, int cy, int diameter, Screen screen, bool happy, uint32_t frame, uint8_t level,
          uint8_t voice) {
  if (diameter < 4) return;
  const Palette pal = palette_for(screen);
  const int r = diameter / 2;
  shade_disc(c, cx, cy, r, pal);
  const Mood mood = mood_for(screen, happy);
  const Sample s = sample(frame, mood, level, voice, dots_for(screen));
  auto eye = [&](const Eye& e) {
    const int ex = cx + e.x * diameter / 1000;
    const int ey = cy + e.y * diameter / 1000;
    const int hw = std::max(1, e.w * diameter / 2000);
    const int hh = std::max(1, drawn_h(e.h, s.blink) * diameter / 2000);
    c.fill_tilted_capsule(ex, ey, hw, hh, e.tilt, pal.eye);
  };
  eye(s.left);
  eye(s.right);
  if (s.dots) {
    const int y = cy + diameter * 112 / 1000;
    const int gap = std::max(4, diameter * 72 / 1000);
    const int rad = std::max(2, diameter * 26 / 1000);
    const int x = cx - gap;
    for (int i = 0; i < 3; ++i) {
      const int rr = (i + 1 == s.dots) ? rad + std::max(1, rad / 3) : std::max(1, rad * 2 / 3);
      c.fill_circle(x + i * gap, y, rr, pal.eye);
    }
  }
}

}  // namespace hg::face
