// See face.hpp for the zongzhu-bot licence and what was adapted.
#include "hg/face.hpp"

#include <algorithm>
#include <cstdint>

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
constexpr int kEase[16] = {0, 304, 516, 663, 765, 837, 886, 921, 945, 962, 973, 981, 987, 991, 994, 996};

// Unit sine, 64 steps, thousandths.
constexpr int kSin[64] = {0,    98,   195,  290,  383,  471,  556,  634,  707,  773,  831,  882,  924,  957,  981,  995,
                          1000, 995,  981,  957,  924,  882,  831,  773,  707,  634,  556,  471,  383,  290,  195,  98,
                          0,    -98,  -195, -290, -383, -471, -556, -634, -707, -773, -831, -882, -924, -957, -981, -995,
                          -1000, -995, -981, -957, -924, -882, -831, -773, -707, -634, -556, -471, -383, -290, -195, -98};

constexpr int kBreathAmp = 8;
constexpr int kSpeakAmp = 14;
constexpr int kSpeakVoice = 32;

// Eye-band limits, thousandths of the diameter from the face centre.
constexpr int kBandAbove = 320;
constexpr int kBandBelow = 240;

// 4x4 Bayer, centred on 0 so a flat colour stays near its 565 value.
constexpr int8_t kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

constexpr uint16_t kRingG = rgb(61, 220, 151);
constexpr uint16_t kRingT = rgb(46, 196, 182);
constexpr uint16_t kRingP = rgb(167, 139, 250);
constexpr uint16_t kRingY = rgb(245, 215, 110);

struct Rgb {
  uint8_t r, g, b;
};

uint32_t mix32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

int sine(uint32_t index) { return kSin[index & 63u]; }

int iabs(int v) { return v < 0 ? -v : v; }

int isqrt_u32(uint32_t n) {
  uint32_t x = 0;
  uint32_t b = 1u << 15;
  while (b) {
    const uint32_t t = x + b;
    if (t != 0 && t <= n / t) x = t;
    b >>= 1;
  }
  return static_cast<int>(x);
}

uint32_t isqrt64(uint64_t n) {
  uint64_t x = 0;
  uint64_t b = 1ull << 31;
  while (b) {
    const uint64_t t = x + b;
    if (t != 0 && t <= n / t) x = t;
    b >>= 1;
  }
  return static_cast<uint32_t>(x);
}

uint32_t blink_start(uint32_t i) { return i * kBlinkWindow + kBlinkBase + mix32(i + 1) % kBlinkJitter; }

uint32_t glance_start(uint32_t i) { return i * kGlanceWindow + mix32(i + 17) % kGlanceJitter; }

int pulse(int age) {
  static const int k[7] = {520, 1000, 1000, 750, 500, 250, 0};
  if (age < 0 || age > 6) return 0;
  return k[age];
}

bool blinks(Mood mood) {
  switch (mood) {
    case Mood::Alert:
    case Mood::Sleep:
    case Mood::Sleepy:
      return false;
    default:
      return true;
  }
}

bool glances(Mood mood) {
  switch (mood) {
    case Mood::Sleepy:
    case Mood::Sleep:
    case Mood::Alert:
    case Mood::Happy:
    case Mood::Wink:
    case Mood::Playful:
    case Mood::Proud:
      return false;
    default:
      return true;
  }
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
  if (!glances(mood)) return;
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
  if ((h % 4u) == 0) return;
  const int tx = static_cast<int>(h % 101u) - 50;
  const int ty = static_cast<int>((h >> 8) % 81u) - 40;
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

// cos(0..90 degrees) in 256ths. Positive angles are clockwise, y down.
constexpr int16_t kCos256[91] = {
    256, 256, 256, 256, 255, 255, 255, 254, 254, 253, 252, 251, 250, 249, 248, 247, 246, 245, 243, 242, 241,
    239, 237, 236, 234, 232, 230, 228, 226, 224, 222, 219, 217, 215, 212, 210, 207, 204, 202, 199, 196, 193,
    190, 187, 184, 181, 178, 175, 171, 168, 165, 161, 158, 154, 150, 147, 143, 139, 136, 132, 128, 124, 120,
    116, 112, 108, 104, 100, 96,  92,  88,  83,  79,  75,  71,  66,  62,  58,  53,  49,  44,  40,  36,  31,
    27,  22,  18,  13,  9,   4,   0};

void sincos256(int deg, int& s, int& c) {
  while (deg < 0) deg += 360;
  deg %= 360;
  const int q = deg / 90;
  const int d = deg % 90;
  const int sinv = q & 1 ? kCos256[d] : kCos256[90 - d];
  const int cosv = q & 1 ? kCos256[90 - d] : kCos256[d];
  s = q == 0 || q == 1 ? sinv : -sinv;
  c = q == 0 || q == 3 ? cosv : -cosv;
}

Eye mirror_x(Eye e) {
  e.x = static_cast<int16_t>(-e.x);
  return e;
}

Eye pose_left(Mood mood, uint8_t level) {
  Eye e;
  e.x = -80;
  e.y = -42;
  e.w = 72;
  e.h = 156;
  e.tilt = 7;
  switch (mood) {
    case Mood::Attentive: {
      const int loud = std::min(100, static_cast<int>(level));
      e.y = -50;
      e.w = static_cast<int16_t>(78 + loud / 10);
      e.h = static_cast<int16_t>(172 + loud * 32 / 100);
      e.tilt = 5;
      break;
    }
    case Mood::Curious:
      e.x = -96;
      e.y = -48;
      e.w = 76;
      e.h = 168;
      e.tilt = -14;
      break;
    case Mood::Thinking:
      e.x = -88;
      e.y = -78;
      e.w = 68;
      e.h = 132;
      e.tilt = -6;
      break;
    case Mood::Speaking:
      break;
    case Mood::Happy:
      e.x = -76;
      e.y = -16;
      e.w = 116;
      e.h = 30;
      e.tilt = 3;
      break;
    case Mood::Confused:
      e.y = -30;
      e.h = 150;
      e.tilt = 10;
      break;
    case Mood::Sleepy:
      e.x = -62;
      e.y = 18;
      e.w = 26;
      e.h = 26;
      e.tilt = 0;
      break;
    case Mood::Wink:
      e.y = -36;
      e.h = 150;
      e.tilt = 6;
      break;
    case Mood::Wide:
      e.x = -88;
      e.y = -28;
      e.w = 92;
      e.h = 196;
      e.tilt = 5;
      break;
    case Mood::Surprised:
      e.x = -80;
      e.y = -16;
      e.w = 104;
      e.h = 116;
      e.tilt = 0;
      break;
    case Mood::Playful:
      e.x = -84;
      e.y = -48;
      e.w = 70;
      e.h = 150;
      e.tilt = 12;
      break;
    case Mood::Proud:
      e.x = -78;
      e.y = -50;
      e.w = 108;
      e.h = 52;
      e.tilt = 4;
      break;
    case Mood::Shy:
      e.x = -58;
      e.y = 28;
      e.w = 48;
      e.h = 86;
      e.tilt = 6;
      break;
    case Mood::Alert:
    case Mood::Sleep:
      e.w = 0;
      e.h = 0;
      break;
    case Mood::Neutral:
      break;
  }
  return e;
}

Eye pose_right(Mood mood, const Eye& left) {
  Eye e = mirror_x(left);
  switch (mood) {
    case Mood::Neutral:
      e.x = 84;
      e.y = -36;
      e.w = 66;
      e.h = 148;
      e.tilt = -5;
      break;
    case Mood::Attentive:
      e.x = 78;
      e.h = static_cast<int16_t>(left.h - 8);
      e.w = static_cast<int16_t>(left.w - 6);
      e.tilt = -8;
      break;
    case Mood::Curious:
      e.x = 70;
      e.y = -30;
      e.w = 58;
      e.h = 104;
      e.tilt = 11;
      break;
    case Mood::Thinking:
      e.x = 72;
      e.y = -84;
      e.w = 60;
      e.h = 118;
      e.tilt = 5;
      break;
    case Mood::Happy:
      e.x = 74;
      e.y = -12;
      e.w = 104;
      e.h = 28;
      e.tilt = -2;
      break;
    case Mood::Confused:
      e.x = 78;
      e.y = -22;
      e.w = 120;
      e.h = 30;
      e.tilt = -6;
      break;
    case Mood::Wink:
      e.x = 80;
      e.y = -20;
      e.w = 108;
      e.h = 28;
      e.tilt = -2;
      break;
    case Mood::Wide:
      e.tilt = -3;
      e.h = static_cast<int16_t>(left.h - 10);
      break;
    case Mood::Surprised:
      e.w = 96;
      e.h = 124;
      e.tilt = 2;
      break;
    case Mood::Playful:
      e.x = 78;
      e.y = -10;
      e.w = 100;
      e.h = 34;
      e.tilt = -6;
      break;
    case Mood::Proud:
      e.y = -46;
      e.h = 46;
      e.tilt = -3;
      break;
    case Mood::Shy:
      e.x = 64;
      e.y = 36;
      e.w = 42;
      e.h = 74;
      e.tilt = -4;
      break;
    default:
      e.tilt = static_cast<int16_t>(-left.tilt + 2);
      break;
  }
  return e;
}

int drawn_h(int h, int blink) {
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

Mood mood_of(uint8_t reaction) {
  switch (reaction) {
    case 1: return Mood::Curious;
    case 2: return Mood::Playful;
    case 3: return Mood::Proud;
    case 4: return Mood::Shy;
    case 5: return Mood::Wink;
    case 6: return Mood::Surprised;
    case 7: return Mood::Wide;
    case 8: return Mood::Confused;
    default: return Mood::Neutral;
  }
}

int clamp8(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return v;
}

uint16_t shade_pixel(int dx, int dy, int r, Rgb hi, Rgb mid, Rgb lo, int x, int y) {
  int t = 128;
  if (r > 0) {
    t = (dx + dy + 2 * r) * 255 / (4 * r);
    if (t < 0) t = 0;
    if (t > 255) t = 255;
  }
  int r8, g8, b8;
  if (t <= 128) {
    r8 = hi.r + (mid.r - hi.r) * t / 128;
    g8 = hi.g + (mid.g - hi.g) * t / 128;
    b8 = hi.b + (mid.b - hi.b) * t / 128;
  } else {
    const int u = t - 128;
    r8 = mid.r + (lo.r - mid.r) * u / 127;
    g8 = mid.g + (lo.g - mid.g) * u / 127;
    b8 = mid.b + (lo.b - mid.b) * u / 127;
  }
  const int d = kBayer[(y & 3) * 4 + (x & 3)] - 8;
  return rgb(static_cast<uint8_t>(clamp8(r8 + d)), static_cast<uint8_t>(clamp8(g8 + d)),
             static_cast<uint8_t>(clamp8(b8 + d)));
}

int circle_sd(int dx, int dy, int r) {
  const int dist = isqrt_u32(static_cast<uint32_t>(dx * dx + dy * dy));
  return dist - r;
}

int squircle_sd(int dx, int dy, int r) {
  const int64_t x2 = static_cast<int64_t>(dx) * dx;
  const int64_t y2 = static_cast<int64_t>(dy) * dy;
  const uint32_t l4 = isqrt64(isqrt64(static_cast<uint64_t>(x2 * x2 + y2 * y2)));
  return static_cast<int>(l4) - r;
}

int pebble_sd(int dx, int dy, int r) {
  const int dist = isqrt_u32(static_cast<uint32_t>(dx * dx + dy * dy));
  if (r < 1) return dist;
  // Wider than it is tall, with a bulge on one diagonal, so it is not a disc.
  int rr = r + (dx * dy) / (r * 3) + (dx * dx - 2 * dy * dy) / (r * 8);
  const int lo = r * 78 / 100;
  const int hi = r * 118 / 100;
  if (rr < lo) rr = lo;
  if (rr > hi) rr = hi;
  return dist - rr;
}

int triangle_sd(int dx, int dy, int r) {
  if (r < 1) return 0;
  // Point up. Inside is negative. The right edge is walked top to bottom-right;
  // in this y-down space the outward side is the negation of that cross product.
  const int top = -r * 90 / 100;
  const int bot = r * 68 / 100;
  const int half = r * 84 / 100;
  const int sd_bottom = dy - bot;
  const int ey = bot - top;
  auto cross = [&](int ex, int px, int py) {
    const int c = ex * py - ey * px;
    const int len = isqrt_u32(static_cast<uint32_t>(ex * ex + ey * ey));
    return len ? c / len : 0;
  };
  const int sd_r = -cross(half, dx, dy - top);
  const int sd_l = cross(-half, dx, dy - top);
  int sd = sd_bottom;
  if (sd_r > sd) sd = sd_r;
  if (sd_l > sd) sd = sd_l;
  return sd - r / 16;
}

int shape_sd(int dx, int dy, int r, uint8_t shape) {
  switch (shape) {
    case 1: return pebble_sd(dx, dy, r);
    case 2: return squircle_sd(dx, dy, r);
    case 3: return triangle_sd(dx, dy, r);
    default: return circle_sd(dx, dy, r);
  }
}

int blended_sd(int dx, int dy, int r, const Style& style) {
  const int circle = circle_sd(dx, dy, r);
  if (style.shape_k == 0 || style.shape == 0) return circle;
  const int target = shape_sd(dx, dy, r, style.shape);
  return circle + (target - circle) * static_cast<int>(style.shape_k) / 100;
}

int coverage(int sd) {
  if (sd <= -1) return 256;
  if (sd >= 1) return 0;
  return 128;
}

void paint_body(Canvas& c, int cx, int cy, int r, Rgb hi, Rgb mid, Rgb lo, const Style& style) {
  if (r < 2) return;
  const bool round = style.shape_k == 0 || style.shape == 0;
  const int ext = r + 3;
  const int y0 = std::max(cy - ext, c.clip_y0());
  const int y1 = std::min(cy + ext + 1, c.clip_y1());
  for (int y = y0; y < y1; ++y) {
    const int dy = y - cy;
    if (round) {
      const int rest = r * r - dy * dy;
      int half = 0;
      if (rest > 0) half = isqrt_u32(static_cast<uint32_t>(rest));
      const int x_lo = cx - half - 1;
      const int x_hi = cx + half + 1;
      for (int x = x_lo; x <= x_hi; ++x) {
        const int dx = x - cx;
        const int sd = isqrt_u32(static_cast<uint32_t>(dx * dx + dy * dy)) - r;
        const int cov = coverage(sd);
        if (cov <= 0) continue;
        const uint16_t col = shade_pixel(dx, dy, r, hi, mid, lo, x, y);
        if (cov >= 256) c.pixel(x, y, col);
        else c.blend(x, y, col, cov);
      }
    } else {
      for (int dx = -ext; dx <= ext; ++dx) {
        const int sd = blended_sd(dx, dy, r, style);
        const int cov = coverage(sd);
        if (cov <= 0) continue;
        const uint16_t col = shade_pixel(dx, dy, r, hi, mid, lo, cx + dx, y);
        if (cov >= 256) c.pixel(cx + dx, y, col);
        else c.blend(cx + dx, y, col, cov);
      }
    }
  }
}

void paint_aa_disc(Canvas& c, int cx, int cy, int r, uint16_t color) {
  if (r < 1) return;
  const int y0 = std::max(cy - r - 1, c.clip_y0());
  const int y1 = std::min(cy + r + 2, c.clip_y1());
  for (int y = y0; y < y1; ++y) {
    const int dy = y - cy;
    for (int dx = -r - 1; dx <= r + 1; ++dx) {
      const int sd = isqrt_u32(static_cast<uint32_t>(dx * dx + dy * dy)) - r;
      const int cov = coverage(sd);
      if (cov >= 256) c.pixel(cx + dx, y, color);
      else if (cov > 0) c.blend(cx + dx, y, color, cov);
    }
  }
}

// Taper is the top and bottom half-width, in thousandths of the nominal half-width.
void paint_eye(Canvas& c, int cx, int cy, int hw, int hh, int tilt, int taper_top, int taper_bot, uint16_t color,
               int alpha) {
  if (hw < 1 || hh < 1 || alpha <= 0) return;
  int s = 0, co = 256;
  sincos256(tilt, s, co);
  const int ext = hw + hh + 2;
  const int y0 = std::max(cy - ext, c.clip_y0());
  const int y1 = std::min(cy + ext + 1, c.clip_y1());
  for (int y = y0; y < y1; ++y) {
    const int dy = y - cy;
    for (int dx = -ext; dx <= ext; ++dx) {
      const int lx = (dx * co + dy * s) >> 8;
      const int ly = (-dx * s + dy * co) >> 8;
      const int span = hh * 2;
      int t = span > 0 ? (ly + hh) * 1000 / span : 500;
      if (t < 0) t = 0;
      if (t > 1000) t = 1000;
      int hw_y = (hw * ((1000 - t) * taper_top + t * taper_bot)) / 1000000;
      if (hw_y < 1) hw_y = 1;
      const int ax = iabs(lx);
      const int ay = iabs(ly);
      int sd;
      if (hh >= hw_y) {
        const int inner = hh - hw_y;
        if (ay <= inner) sd = ax - hw_y;
        else {
          const int ey = ay - inner;
          sd = isqrt_u32(static_cast<uint32_t>(ax * ax + ey * ey)) - hw_y;
        }
      } else {
        const int inner = hw_y - hh;
        if (ax <= inner) sd = ay - hh;
        else {
          const int ex = ax - inner;
          sd = isqrt_u32(static_cast<uint32_t>(ex * ex + ay * ay)) - hh;
        }
      }
      int cov = coverage(sd);
      if (cov <= 0) continue;
      cov = cov * alpha / 100;
      if (cov >= 256) c.pixel(cx + dx, y, color);
      else if (cov > 0) c.blend(cx + dx, y, color, cov);
    }
  }
}

void paint_ring(Canvas& c, int cx, int cy, int rx, int ry, int thick, int deg, uint16_t color, bool front,
                int body_r) {
  if (rx < 2 || ry < 2) return;
  int s = 0, co = 256;
  sincos256(deg, s, co);
  const int in_rx = std::max(1, rx - thick);
  const int in_ry = std::max(1, ry - thick);
  const int64_t outer = static_cast<int64_t>(rx) * rx * ry * ry;
  const int64_t inner = static_cast<int64_t>(in_rx) * in_rx * in_ry * in_ry;
  const int ext = std::max(rx, ry) + 2;
  const int y0 = std::max(cy - ext, c.clip_y0());
  const int y1 = std::min(cy + ext + 1, c.clip_y1());
  for (int y = y0; y < y1; ++y) {
    const int dy = y - cy;
    for (int dx = -ext; dx <= ext; ++dx) {
      const int lx = (dx * co + dy * s) >> 8;
      const int ly = (-dx * s + dy * co) >> 8;
      const int64_t e = static_cast<int64_t>(lx) * lx * ry * ry + static_cast<int64_t>(ly) * ly * rx * rx;
      if (e > outer || e < inner) continue;
      if (front && dy <= 0) continue;
      if (!front) {
        const int d2 = dx * dx + dy * dy;
        if (d2 < body_r * body_r && dy > 0) continue;
      }
      c.blend(cx + dx, y, color, front ? 210 : 230);
    }
  }
}

void paint_swirl(Canvas& c, int cx, int cy, int r, uint32_t frame, bool front) {
  struct Ring {
    int rxp, ryp, thick, phase;
    uint16_t color;
  };
  const Ring rings[4] = {
      {124, 86, 2, 20, kRingG}, {112, 100, 2, 100, kRingT}, {136, 74, 2, 180, kRingP}, {104, 112, 2, 250, kRingY},
  };
  const int spin = static_cast<int>((frame * 4u) % 360u);
  for (const Ring& ring : rings) {
    paint_ring(c, cx, cy, std::max(2, r * ring.rxp / 100), std::max(2, r * ring.ryp / 100), std::max(1, r * ring.thick / 100),
               spin + ring.phase, ring.color, front, r);
  }
}

void paint_bang(Canvas& c, int cx, int cy, int diameter, uint16_t color) {
  const int stem_h = std::max(8, diameter * 34 / 100);
  const int stem_w = std::max(3, diameter * 7 / 100);
  const int dot_r = std::max(2, diameter * 5 / 100);
  const int gap = std::max(3, diameter * 5 / 100);
  const int total = stem_h + gap + dot_r * 2;
  const int top = cy - total / 2;
  c.fill_tilted_capsule(cx, top + stem_h / 2, std::max(1, stem_w / 2), std::max(1, stem_h / 2), 0, color);
  paint_aa_disc(c, cx, top + stem_h + gap + dot_r, dot_r, color);
}

void paint_dots(Canvas& c, int cx, int cy, int diameter, uint32_t frame, uint8_t morph, uint16_t mid, uint16_t hi,
                uint16_t lo) {
  if (morph == 0) return;
  const int full = std::max(2, diameter * 9 / 100);
  const int gap = std::max(full * 2 + 2, diameter * 22 / 100);
  const uint16_t colors[3] = {mid, hi, lo};
  for (int i = 0; i < 3; ++i) {
    int rad = full * morph / 100;
    if (morph >= 90) {
      const int wob = sine(frame * 3u + static_cast<uint32_t>(i) * 21u);
      rad += wob * full / 8000;
    }
    if (rad < 1) continue;
    const int spread = gap * morph / 100;
    paint_aa_disc(c, cx + (i - 1) * spread, cy, rad, colors[i]);
  }
}

void taper_for(Mood mood, bool right, int& top, int& bot) {
  top = 1000;
  bot = 1000;
  if (mood == Mood::Happy || mood == Mood::Wink || mood == Mood::Confused || mood == Mood::Proud || mood == Mood::Sleepy ||
      mood == Mood::Playful)
    return;
  if (!right) {
    top = 880;
    bot = 1140;
  } else {
    top = 1160;
    bot = 900;
  }
  if (mood == Mood::Surprised || mood == Mood::Wide) {
    top = right ? 1040 : 960;
    bot = right ? 960 : 1040;
  }
}

bool eyes_visible(Mood mood, const Style& style) {
  if (mood == Mood::Alert || mood == Mood::Sleep) return false;
  if (style.morph >= 88) return false;
  if (style.wake < 45) return false;
  if (style.asleep && style.wake < 70) return false;
  return true;
}

Rgb stops_hi(Screen screen) {
  if (screen == Screen::Error) return {kRedHiR, kRedHiG, kRedHiB};
  if (screen == Screen::Offline) return {kDimHiR, kDimHiG, kDimHiB};
  return {kBlueHiR, kBlueHiG, kBlueHiB};
}
Rgb stops_mid(Screen screen) {
  if (screen == Screen::Error) return {kRedR, kRedG, kRedB};
  if (screen == Screen::Offline) return {kDimR, kDimG, kDimB};
  return {kBlueR, kBlueG, kBlueB};
}
Rgb stops_lo(Screen screen) {
  if (screen == Screen::Error) return {kRedShadeR, kRedShadeG, kRedShadeB};
  if (screen == Screen::Offline) return {kDimShadeR, kDimShadeG, kDimShadeB};
  return {kBlueShadeR, kBlueShadeG, kBlueShadeB};
}

}  // namespace

bool anim_wide(const Style& style) {
  if (style.swirl) return true;
  if (style.asleep || style.wake < 100) return true;
  if (style.shape_k > 0 && style.shape_k < 100) return true;
  if (style.morph > 0 && style.morph < 100) return true;
  return false;
}

Mood mood_for(Screen screen, bool happy, uint8_t reaction, bool busy) {
  if (screen == Screen::Error) return Mood::Alert;
  if (happy && screen == Screen::Ready) return Mood::Happy;
  if (screen == Screen::Thinking && busy) return reaction == 7 ? Mood::Wide : Mood::Curious;
  if (screen == Screen::Thinking) return Mood::Thinking;
  if (screen == Screen::Responding) return reaction == 6 ? Mood::Surprised : Mood::Speaking;
  if (screen == Screen::Ready && reaction) return mood_of(reaction);
  switch (screen) {
    case Screen::Listening:
    case Screen::Prompt:
      return Mood::Attentive;
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

bool dots_for(Screen screen, bool busy) { return screen == Screen::Thinking && !busy; }

uint16_t mix_rgb(uint8_t ar, uint8_t ag, uint8_t ab, uint8_t br, uint8_t bg, uint8_t bb, int toward_b) {
  auto m = [&](int a, int b) { return static_cast<uint8_t>((a * (100 - toward_b) + b * toward_b) / 100); };
  return rgb(m(ar, br), m(ag, bg), m(ab, bb));
}

Palette palette_for(Screen screen) {
  Palette p;
  p.eye = rgb(kEyeR, kEyeG, kEyeB);
  const Rgb mid = stops_mid(screen);
  const Rgb hi = stops_hi(screen);
  const Rgb lo = stops_lo(screen);
  Rgb core = {kBlueCoreR, kBlueCoreG, kBlueCoreB};
  if (screen == Screen::Error) core = {kRedCoreR, kRedCoreG, kRedCoreB};
  else if (screen == Screen::Offline) core = {kDimCoreR, kDimCoreG, kDimCoreB};
  p.body = rgb(mid.r, mid.g, mid.b);
  p.mid = mix_rgb(mid.r, mid.g, mid.b, hi.r, hi.g, hi.b, 45);
  p.highlight = rgb(hi.r, hi.g, hi.b);
  p.core = rgb(core.r, core.g, core.b);
  p.shade = rgb(lo.r, lo.g, lo.b);
  return p;
}

Sample sample(uint32_t frame, Mood mood, uint8_t level, uint8_t voice, bool dots) {
  Sample s;
  s.left = pose_left(mood, level);
  s.right = pose_right(mood, s.left);
  int gx = 0, gy = 0;
  gaze(frame, mood, gx, gy);
  s.left.x = static_cast<int16_t>(s.left.x + gx);
  s.left.y = static_cast<int16_t>(s.left.y + gy);
  s.right.x = static_cast<int16_t>(s.right.x + gx);
  s.right.y = static_cast<int16_t>(s.right.y + gy);
  if (mood == Mood::Neutral || mood == Mood::Attentive || mood == Mood::Curious || mood == Mood::Wide ||
      mood == Mood::Shy) {
    const int breath = sine((frame * 21u) >> 5) * kBreathAmp / 1000;
    s.left.h = static_cast<int16_t>(s.left.h + breath);
    s.right.h = static_cast<int16_t>(s.right.h + breath);
  }
  if (mood == Mood::Speaking) {
    const int amp = kSpeakAmp + static_cast<int>(std::min<uint8_t>(voice, 100)) * kSpeakVoice / 100;
    const int wobble = sine(frame * 11u) * amp / 1000;
    s.left.h = static_cast<int16_t>(s.left.h + wobble);
    s.right.h = static_cast<int16_t>(s.right.h + wobble);
    s.left.w = static_cast<int16_t>(s.left.w - wobble / 3);
    s.right.w = static_cast<int16_t>(s.right.w - wobble / 3);
  }
  s.blink = blinks(mood) ? static_cast<uint16_t>(blink_amount(frame)) : 0;
  s.dots = dots ? static_cast<uint8_t>((frame / 5u) % 3u + 1u) : 0;
  return s;
}

uint32_t picture_key(uint32_t frame, Mood mood, uint8_t level, uint8_t voice, bool dots, int diameter,
                     const Style& style) {
  if (diameter < 1) diameter = 1;
  Key k;
  k.val(static_cast<int>(mood))
      .val(style.morph)
      .val(style.shape)
      .val(style.shape_k)
      .val(style.wake)
      .val(style.reaction)
      .val(style.asleep)
      .val(style.swirl)
      .val(style.busy)
      .val(diameter);
  if (style.swirl || style.morph >= 80) k.val(static_cast<int>(frame));
  if (eyes_visible(mood, style)) {
    Sample s = sample(frame, mood, level, voice, dots);
    if (style.swirl) s.blink = 0;
    auto q = [&](int v) { return v * diameter / 1000; };
    k.val(q(s.left.x)).val(q(s.left.y)).val(q(s.left.w)).val(q(drawn_h(s.left.h, s.blink))).val(s.left.tilt);
    k.val(q(s.right.x)).val(q(s.right.y)).val(q(s.right.w)).val(q(drawn_h(s.right.h, s.blink))).val(s.right.tilt);
  }
  if (mood == Mood::Speaking) k.val(static_cast<int>(frame));
  return k.get();
}

void anim_rows(int cy, int diameter, int& y0, int& y1, bool wide, uint8_t morph) {
  if (wide) {
    y0 = cy - diameter * 74 / 100 - 2;
    y1 = cy + diameter * 74 / 100 + 3;
    return;
  }
  if (morph >= 80) {
    y0 = cy - diameter * 24 / 100 - 2;
    y1 = cy + diameter * 24 / 100 + 3;
    return;
  }
  y0 = cy - diameter * kBandAbove / 1000 - 2;
  y1 = cy + diameter * kBandBelow / 1000 + 3;
}

void draw(Canvas& c, int cx, int cy, int diameter, Screen screen, bool happy, uint32_t frame, uint8_t level,
          uint8_t voice, const Style& style) {
  if (diameter < 4) return;
  const Palette pal = palette_for(screen);
  const int r = std::max(2, diameter / 2);
  const Mood mood = mood_for(screen, happy, style.reaction, style.busy);
  if (mood == Mood::Alert) {
    paint_bang(c, cx, cy, diameter, pal.body);
    return;
  }
  if (style.swirl) paint_swirl(c, cx, cy, r, frame, false);

  const int wake = std::min(100, static_cast<int>(style.wake));
  const int morph = std::min(100, static_cast<int>(style.morph));
  int body_r = r * wake / 100 * (100 - morph) / 100;
  if (body_r >= 2) {
    paint_body(c, cx, cy, body_r, stops_hi(screen), stops_mid(screen), stops_lo(screen), style);
  } else if (morph < 40) {
    paint_aa_disc(c, cx, cy, std::max(2, diameter * 6 / 100), pal.body);
  }

  if (eyes_visible(mood, style)) {
    Sample s = sample(frame, mood, level, voice, dots_for(screen, style.busy));
    if (style.swirl) s.blink = 0;
    if (mood == Mood::Thinking && morph > 0 && morph < 40) {
      const int lift = (40 - morph) * 2;
      s.left.y = static_cast<int16_t>(s.left.y - lift);
      s.right.y = static_cast<int16_t>(s.right.y - lift);
    }
    const int alpha = (100 - morph) * std::min(wake, 100) / 100;
    auto eye = [&](const Eye& e, bool right) {
      if (e.w <= 0 || e.h <= 0) return;
      int top = 1000, bot = 1000;
      taper_for(mood, right, top, bot);
      const int ex = cx + e.x * diameter / 1000;
      const int ey = cy + e.y * diameter / 1000;
      const int hw = std::max(1, e.w * diameter / 2000);
      const int hh = std::max(1, drawn_h(e.h, s.blink) * diameter / 2000);
      paint_eye(c, ex, ey, hw, hh, e.tilt, top, bot, pal.eye, alpha);
    };
    eye(s.left, false);
    eye(s.right, true);
  }

  if (style.swirl) paint_swirl(c, cx, cy, r, frame, true);
  if (morph > 0) paint_dots(c, cx, cy, diameter, frame, static_cast<uint8_t>(morph), pal.body, pal.highlight, pal.shade);
}

}  // namespace hg::face
