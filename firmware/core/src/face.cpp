// See face.hpp for the zongzhu-bot licence and what was adapted.
#include "hg/face.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "hg/cooperate.hpp"

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

Rgb shade_rgb(int dx, int dy, int r, Rgb hi, Rgb mid, Rgb lo) {
  int t = 128;
  if (r > 0) {
    t = (dx + dy + 2 * r) * 255 / (4 * r);
    if (t < 0) t = 0;
    if (t > 255) t = 255;
  }
  Rgb o;
  if (t <= 128) {
    o.r = static_cast<uint8_t>(hi.r + (mid.r - hi.r) * t / 128);
    o.g = static_cast<uint8_t>(hi.g + (mid.g - hi.g) * t / 128);
    o.b = static_cast<uint8_t>(hi.b + (mid.b - hi.b) * t / 128);
  } else {
    const int u = t - 128;
    o.r = static_cast<uint8_t>(mid.r + (lo.r - mid.r) * u / 127);
    o.g = static_cast<uint8_t>(mid.g + (lo.g - mid.g) * u / 127);
    o.b = static_cast<uint8_t>(mid.b + (lo.b - mid.b) * u / 127);
  }
  return o;
}

// Floyd–Steinberg in 8-bit, with a 2-LSB ordered term so the first pixels of a
// band already cross an RGB565 bin. The error is the part rgb565() truncates
// (red/blue step 8, green step 4); spreading it is what breaks the radial
// bands a 4×4 Bayer tile draws as rings. Single-threaded: the rows live in
// static storage so the paint does not put 10 KB on the stack.
class BodyDither {
 public:
  void reset(int width) {
    width_ = width;
    if (width_ < 0) width_ = 0;
    if (width_ > kMax) width_ = kMax;
    std::memset(er_, 0, sizeof(er_));
    std::memset(eg_, 0, sizeof(eg_));
    std::memset(eb_, 0, sizeof(eb_));
  }
  void next_row() {
    if (width_ <= 0) return;
    std::memcpy(er_[0], er_[1], static_cast<size_t>(width_) * sizeof(int16_t));
    std::memcpy(eg_[0], eg_[1], static_cast<size_t>(width_) * sizeof(int16_t));
    std::memcpy(eb_[0], eb_[1], static_cast<size_t>(width_) * sizeof(int16_t));
    std::memset(er_[1], 0, static_cast<size_t>(width_) * sizeof(int16_t));
    std::memset(eg_[1], 0, static_cast<size_t>(width_) * sizeof(int16_t));
    std::memset(eb_[1], 0, static_cast<size_t>(width_) * sizeof(int16_t));
  }
  uint16_t apply(int x, int y, Rgb c) {
    const int bay = kBayer[(y & 3) * 4 + (x & 3)] - 8;  // -8..+7
    int r = c.r + bay * 2;
    int g = c.g + bay;
    int b = c.b + bay * 2;
    const bool track = x >= 0 && x < width_;
    if (track) {
      r += er_[0][x];
      g += eg_[0][x];
      b += eb_[0][x];
      er_[0][x] = eg_[0][x] = eb_[0][x] = 0;
    }
    r = clamp8(r);
    g = clamp8(g);
    b = clamp8(b);
    const uint16_t q = rgb(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b));
    if (!track) return q;
    const int rq = ((q >> 11) & 31) << 3;
    const int gq = ((q >> 5) & 63) << 2;
    const int bq = (q & 31) << 3;
    spill(x, r - rq, g - gq, b - bq);
    return q;
  }

 private:
  static constexpr int kMax = 800;
  int width_ = 0;
  int16_t er_[2][kMax] = {};
  int16_t eg_[2][kMax] = {};
  int16_t eb_[2][kMax] = {};

  static int clamp_err(int v) {
    if (v < -96) return -96;
    if (v > 96) return 96;
    return v;
  }
  void spill(int x, int er, int eg, int eb) {
    auto add = [&](int xx, int row, int wr, int wg, int wb) {
      if (xx < 0 || xx >= width_) return;
      er_[row][xx] = static_cast<int16_t>(clamp_err(er_[row][xx] + wr));
      eg_[row][xx] = static_cast<int16_t>(clamp_err(eg_[row][xx] + wg));
      eb_[row][xx] = static_cast<int16_t>(clamp_err(eb_[row][xx] + wb));
    };
    const int r7 = er * 7 / 16, g7 = eg * 7 / 16, b7 = eb * 7 / 16;
    const int r3 = er * 3 / 16, g3 = eg * 3 / 16, b3 = eb * 3 / 16;
    const int r5 = er * 5 / 16, g5 = eg * 5 / 16, b5 = eb * 5 / 16;
    add(x + 1, 0, r7, g7, b7);
    add(x - 1, 1, r3, g3, b3);
    add(x, 1, r5, g5, b5);
    add(x + 1, 1, er - r7 - r3 - r5, eg - g7 - g3 - g5, eb - b7 - b3 - b5);
  }
};

BodyDither g_dither;

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

// One-pixel edge. sd <= -1 is opaque, sd == 0 is half, sd >= 1 is empty.
// Indexed by sd+1 so the edge itself is a lookup, not a float ramp.
constexpr int kCoverageLut[3] = {256, 128, 0};

int coverage(int sd) {
  if (sd <= -1) return kCoverageLut[0];
  if (sd >= 1) return kCoverageLut[2];
  return kCoverageLut[1];
}

// Same result as coverage(isqrt(d2) - r), without a square root.
int coverage_d2(int d2, int r) {
  const int r2 = r * r;
  if (d2 < r2) return kCoverageLut[0];
  if (d2 < (r + 1) * (r + 1)) return kCoverageLut[1];
  return kCoverageLut[2];
}

// Largest dx with dx^2 <= lim, or -1 when lim < 0.
int chord(int lim) {
  if (lim < 0) return -1;
  return isqrt_u32(static_cast<uint32_t>(lim));
}

void paint_body(Canvas& c, int cx, int cy, int r, Rgb hi, Rgb mid, Rgb lo, const Style& style) {
  if (r < 2) return;
  const bool round = style.shape_k == 0 || style.shape == 0;
  const int ext = r + 3;
  const int y0 = std::max(cy - ext, c.clip_y0());
  const int y1 = std::min(cy + ext + 1, c.clip_y1());
  const int r2 = r * r;
  const int edge2 = (r + 1) * (r + 1);
  g_dither.reset(c.width());
  for (int y = y0; y < y1; ++y) {
    if (((y - y0) & 7) == 0) cooperate();
    const int dy = y - cy;
    const int dy2 = dy * dy;
    uint16_t* row = c.mutable_row(y);
    if (round) {
      // Full pixels are d2 < r^2, the antialiased ring is r^2 <= d2 < (r+1)^2.
      // One chord per row replaces a square root on every pixel.
      const int edge = chord(edge2 - dy2 - 1);
      const int full = chord(r2 - dy2 - 1);
      for (int dx = -edge; dx <= edge; ++dx) {
        const int x = cx + dx;
        const int adx = dx < 0 ? -dx : dx;
        const int cov = adx <= full ? 256 : 128;
        const uint16_t col = g_dither.apply(x, y, shade_rgb(dx, dy, r, hi, mid, lo));
        if (!row || x < 0 || x >= c.width()) continue;
        if (cov >= 256) row[x] = c.encoded(col);
        else c.blend(x, y, col, cov);
      }
    } else {
      for (int dx = -ext; dx <= ext; ++dx) {
        const int sd = blended_sd(dx, dy, r, style);
        const int cov = coverage(sd);
        if (cov <= 0) continue;
        const uint16_t col = g_dither.apply(cx + dx, y, shade_rgb(dx, dy, r, hi, mid, lo));
        const int x = cx + dx;
        if (!row || x < 0 || x >= c.width()) continue;
        if (cov >= 256) row[x] = c.encoded(col);
        else c.blend(x, y, col, cov);
      }
    }
    g_dither.next_row();
  }
}

void paint_aa_disc(Canvas& c, int cx, int cy, int r, uint16_t color) {
  if (r < 1) return;
  const int y0 = std::max(cy - r - 1, c.clip_y0());
  const int y1 = std::min(cy + r + 2, c.clip_y1());
  const int r2 = r * r;
  const int edge2 = (r + 1) * (r + 1);
  const uint16_t solid = c.encoded(color);
  for (int y = y0; y < y1; ++y) {
    const int dy = y - cy;
    const int dy2 = dy * dy;
    const int edge = chord(edge2 - dy2 - 1);
    const int full = chord(r2 - dy2 - 1);
    uint16_t* row = c.mutable_row(y);
    for (int dx = -edge; dx <= edge; ++dx) {
      const int x = cx + dx;
      if (!row || x < 0 || x >= c.width()) continue;
      const int adx = dx < 0 ? -dx : dx;
      if (adx <= full) row[x] = solid;
      else c.blend(x, y, color, 128);
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
      int cov;
      if (hh >= hw_y) {
        const int inner = hh - hw_y;
        if (ay <= inner) cov = coverage(ax - hw_y);
        else {
          const int ey = ay - inner;
          cov = coverage_d2(ax * ax + ey * ey, hw_y);
        }
      } else {
        const int inner = hw_y - hh;
        if (ax <= inner) cov = coverage(ay - hh);
        else {
          const int ex = ax - inner;
          cov = coverage_d2(ex * ex + ay * ay, hh);
        }
      }
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
    if (((y - y0) & 7) == 0) cooperate();
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

int body_radius(int diameter, const Style& style) {
  const int r = std::max(2, diameter / 2);
  const int wake = std::min(100, static_cast<int>(style.wake));
  const int morph = std::min(100, static_cast<int>(style.morph));
  return r * wake / 100 * (100 - morph) / 100;
}

struct BodyKey {
  int cx = 0, cy = 0, r = 0, width = 0;
  uint8_t shape = 0, shape_k = 0;
  uint8_t hr = 0, hg = 0, hb = 0, mr = 0, mg = 0, mb = 0, lr = 0, lg = 0, lb = 0;
  uint16_t bg = 0;
  bool swap = false;
  bool operator==(const BodyKey& o) const {
    return cx == o.cx && cy == o.cy && r == o.r && width == o.width && shape == o.shape && shape_k == o.shape_k &&
           hr == o.hr && hg == o.hg && hb == o.hb && mr == o.mr && mg == o.mg && mb == o.mb && lr == o.lr &&
           lg == o.lg && lb == o.lb && bg == o.bg && swap == o.swap;
  }
};

BodyKey make_key(int cx, int cy, int diameter, Screen screen, uint16_t backdrop, const Style& style, int canvas_w,
                 bool swap) {
  BodyKey k;
  k.cx = cx;
  k.cy = cy;
  k.r = body_radius(diameter, style);
  k.width = canvas_w;
  k.shape = style.shape;
  k.shape_k = style.shape_k;
  const Rgb hi = stops_hi(screen);
  const Rgb mid = stops_mid(screen);
  const Rgb lo = stops_lo(screen);
  k.hr = hi.r;
  k.hg = hi.g;
  k.hb = hi.b;
  k.mr = mid.r;
  k.mg = mid.g;
  k.mb = mid.b;
  k.lr = lo.r;
  k.lg = lo.g;
  k.lb = lo.b;
  k.bg = backdrop;
  k.swap = swap;
  return k;
}

// Post-dither body, including the antialiased edge, in framebuffer byte order.
// One face is on screen and the core is single-threaded, so this is global.
// A large malloc lands in PSRAM on the ESP32 (SPIRAM_USE_MALLOC).
struct BodyCache {
  BodyKey key{};
  bool valid = false;
  bool empty = false;
  int x0 = 0, y0 = 0, w = 0, h = 0;
  uint16_t* px = nullptr;
  size_t cap = 0;
  Box overlay{};
};

BodyCache g_cache;

void add_box(Box& b, int x0, int y0, int x1, int y1) {
  if (!b.any) {
    b.x0 = x0;
    b.y0 = y0;
    b.x1 = x1;
    b.y1 = y1;
    b.any = x0 < x1 && y0 < y1;
    return;
  }
  if (x0 < b.x0) b.x0 = x0;
  if (y0 < b.y0) b.y0 = y0;
  if (x1 > b.x1) b.x1 = x1;
  if (y1 > b.y1) b.y1 = y1;
}

void store_body(Canvas& c, const BodyKey& key) {
  g_cache.key = key;
  g_cache.valid = false;
  g_cache.empty = false;
  if (key.r < 2) {
    g_cache.empty = true;
    g_cache.w = g_cache.h = 0;
    g_cache.valid = true;
    return;
  }
  const int ext = key.r + 3;
  int x0 = key.cx - ext;
  int y0 = key.cy - ext;
  int x1 = key.cx + ext + 1;
  int y1 = key.cy + ext + 1;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > c.width()) x1 = c.width();
  if (y1 > c.height()) y1 = c.height();
  // Ink sits on the disc plus a one-pixel ring. Empty rows outside that can be
  // clipped off without changing the dither. A clip through the ink cannot.
  const int ink0 = std::max(0, key.cy - key.r - 1);
  const int ink1 = std::min(c.height(), key.cy + key.r + 2);
  if (ink0 < c.clip_y0() || ink1 > c.clip_y1() || x1 <= x0 || y1 <= y0) return;
  if (y0 < c.clip_y0()) y0 = c.clip_y0();
  if (y1 > c.clip_y1()) y1 = c.clip_y1();
  const int w = x1 - x0;
  const int h = y1 - y0;
  const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
  if (n > g_cache.cap) {
    std::free(g_cache.px);
    g_cache.px = static_cast<uint16_t*>(std::malloc(n * sizeof(uint16_t)));
    g_cache.cap = g_cache.px ? n : 0;
  }
  if (!g_cache.px) return;
  for (int row = 0; row < h; ++row) {
    const uint16_t* src = c.encoded_row(y0 + row);
    if (!src) return;
    std::memcpy(g_cache.px + static_cast<size_t>(row) * w, src + x0, static_cast<size_t>(w) * sizeof(uint16_t));
  }
  g_cache.x0 = x0;
  g_cache.y0 = y0;
  g_cache.w = w;
  g_cache.h = h;
  g_cache.empty = false;
  g_cache.valid = true;
}

void blit_cached(Canvas& c, const Box& box, uint16_t backdrop) {
  if (!box.any) return;
  if (!g_cache.valid || g_cache.empty || !g_cache.px) {
    c.fill_rect(box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0, backdrop);
    return;
  }
  const int sx1 = g_cache.x0 + g_cache.w;
  const int sy1 = g_cache.y0 + g_cache.h;
  for (int y = box.y0; y < box.y1; ++y) {
    if (y < g_cache.y0 || y >= sy1) {
      c.fill_rect(box.x0, y, box.x1 - box.x0, 1, backdrop);
      continue;
    }
    const int ix0 = std::max(box.x0, g_cache.x0);
    const int ix1 = std::min(box.x1, sx1);
    if (ix0 > box.x0) c.fill_rect(box.x0, y, ix0 - box.x0, 1, backdrop);
    if (ix1 < box.x1) c.fill_rect(ix1, y, box.x1 - ix1, 1, backdrop);
    if (ix0 < ix1) {
      const uint16_t* src = g_cache.px + static_cast<size_t>(y - g_cache.y0) * g_cache.w + (ix0 - g_cache.x0);
      c.blit_encoded(ix0, y, ix1 - ix0, 1, src, ix1 - ix0);
    }
  }
}

Sample placed(uint32_t frame, Mood mood, uint8_t level, uint8_t voice, const Style& style) {
  Sample s = sample(frame, mood, level, voice, false);
  // Blink and the thinking lift match the pixels draw_overlay paints.
  if (style.swirl) s.blink = 0;
  const int morph = std::min(100, static_cast<int>(style.morph));
  if (mood == Mood::Thinking && morph > 0 && morph < 40) {
    const int lift = (40 - morph) * 2;
    s.left.y = static_cast<int16_t>(s.left.y - lift);
    s.right.y = static_cast<int16_t>(s.right.y - lift);
  }
  return s;
}

void add_eye_box(Box& b, int cx, int cy, int diameter, const Eye& e, int blink) {
  if (e.w <= 0 || e.h <= 0) return;
  const int ex = cx + e.x * diameter / 1000;
  const int ey = cy + e.y * diameter / 1000;
  const int hw = std::max(1, e.w * diameter / 2000);
  const int hh = std::max(1, drawn_h(e.h, blink) * diameter / 2000);
  const int ext = hw + hh + 2;
  add_box(b, ex - ext, ey - ext, ex + ext + 1, ey + ext + 1);
}

void paint_eyes(Canvas& c, int cx, int cy, int diameter, const Style& style, const Palette& pal, const Sample& s,
                Mood mood) {
  const int morph = std::min(100, static_cast<int>(style.morph));
  const int wake = std::min(100, static_cast<int>(style.wake));
  const int alpha = (100 - morph) * wake / 100;
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
      .val(diameter)
      .val(static_cast<int>(level))
      .val(static_cast<int>(voice));
  // Swirl, the dot pulse and speaking move on frames the eye pose does not.
  if (style.swirl || style.morph >= 80 || mood == Mood::Speaking) k.val(static_cast<int>(frame));
  if (eyes_visible(mood, style)) {
    Sample s = sample(frame, mood, level, voice, dots);
    if (style.swirl) s.blink = 0;
    auto q = [&](int v) { return v * diameter / 1000; };
    k.val(q(s.left.x)).val(q(s.left.y)).val(q(s.left.w)).val(q(drawn_h(s.left.h, s.blink))).val(s.left.tilt);
    k.val(q(s.right.x)).val(q(s.right.y)).val(q(s.right.w)).val(q(drawn_h(s.right.h, s.blink))).val(s.right.tilt);
  }
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

Box unite(Box a, Box b) {
  if (!a.any) return b;
  if (!b.any) return a;
  add_box(a, b.x0, b.y0, b.x1, b.y1);
  return a;
}

bool body_cached(int cx, int cy, int diameter, Screen screen, uint16_t backdrop, const Style& style, int canvas_w,
                 bool swap) {
  if (!g_cache.valid) return false;
  return g_cache.key == make_key(cx, cy, diameter, screen, backdrop, style, canvas_w, swap);
}

Box overlay_box(int cx, int cy, int diameter, Screen screen, bool happy, uint32_t frame, uint8_t level, uint8_t voice,
                const Style& style) {
  Box b;
  if (diameter < 4) return b;
  const Mood mood = mood_for(screen, happy, style.reaction, style.busy);
  if (mood == Mood::Alert) {
    const int ext = std::max(8, diameter * 28 / 100);
    add_box(b, cx - ext, cy - ext, cx + ext + 1, cy + ext + 1);
    return b;
  }
  if (style.swirl) {
    const int ext = diameter * 74 / 100 + 2;
    add_box(b, cx - ext, cy - ext, cx + ext + 1, cy + ext + 1);
  }
  if (eyes_visible(mood, style)) {
    const Sample s = placed(frame, mood, level, voice, style);
    add_eye_box(b, cx, cy, diameter, s.left, s.blink);
    add_eye_box(b, cx, cy, diameter, s.right, s.blink);
  }
  const int morph = std::min(100, static_cast<int>(style.morph));
  if (morph > 0) {
    const int full = std::max(2, diameter * 9 / 100);
    const int gap = std::max(full * 2 + 2, diameter * 22 / 100);
    const int rad = full + full / 8 + 2;
    for (int i = 0; i < 3; ++i) {
      const int x = cx + (i - 1) * gap;
      add_box(b, x - rad, cy - rad, x + rad + 1, cy + rad + 1);
    }
  }
  return b;
}

Box last_overlay() { return g_cache.overlay; }

void blit_body(Canvas& c, const Box& box, uint16_t backdrop) { blit_cached(c, box, backdrop); }

void draw_overlay(Canvas& c, int cx, int cy, int diameter, Screen screen, bool happy, uint32_t frame, uint8_t level,
                  uint8_t voice, const Style& style) {
  if (diameter < 4) return;
  const Palette pal = palette_for(screen);
  const Mood mood = mood_for(screen, happy, style.reaction, style.busy);
  const int r = std::max(2, diameter / 2);
  const int morph = std::min(100, static_cast<int>(style.morph));
  if (mood == Mood::Alert) {
    paint_bang(c, cx, cy, diameter, pal.body);
  } else {
    if (eyes_visible(mood, style)) {
      const Sample s = placed(frame, mood, level, voice, style);
      paint_eyes(c, cx, cy, diameter, style, pal, s, mood);
    }
    if (style.swirl) paint_swirl(c, cx, cy, r, frame, true);
    if (morph > 0) {
      paint_dots(c, cx, cy, diameter, frame, static_cast<uint8_t>(morph), pal.body, pal.highlight, pal.shade);
    }
  }
  g_cache.overlay = overlay_box(cx, cy, diameter, screen, happy, frame, level, voice, style);
}

void discard_body_cache() { g_cache.valid = false; }

void draw(Canvas& c, int cx, int cy, int diameter, Screen screen, bool happy, uint32_t frame, uint8_t level,
          uint8_t voice, const Style& style, uint16_t backdrop) {
  if (diameter < 4) return;
  const Palette pal = palette_for(screen);
  const int r = std::max(2, diameter / 2);
  const Mood mood = mood_for(screen, happy, style.reaction, style.busy);
  if (mood == Mood::Alert) {
    draw_overlay(c, cx, cy, diameter, screen, happy, frame, level, voice, style);
    return;
  }
  if (style.swirl) paint_swirl(c, cx, cy, r, frame, false);

  const int morph = std::min(100, static_cast<int>(style.morph));
  const BodyKey key = make_key(cx, cy, diameter, screen, backdrop, style, c.width(), c.swapped());
  const bool hit = g_cache.valid && g_cache.key == key;
  if (key.r >= 2) {
    if (hit && !g_cache.empty) {
      Box all;
      all.x0 = g_cache.x0;
      all.y0 = g_cache.y0;
      all.x1 = g_cache.x0 + g_cache.w;
      all.y1 = g_cache.y0 + g_cache.h;
      all.any = true;
      blit_cached(c, all, backdrop);
    } else {
      paint_body(c, cx, cy, key.r, stops_hi(screen), stops_mid(screen), stops_lo(screen), style);
      store_body(c, key);
    }
  } else if (morph < 40) {
    paint_aa_disc(c, cx, cy, std::max(2, diameter * 6 / 100), pal.body);
    g_cache.valid = false;
  } else if (!hit) {
    store_body(c, key);
  }

  draw_overlay(c, cx, cy, diameter, screen, happy, frame, level, voice, style);
}

}  // namespace hg::face
