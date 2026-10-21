// Procedural face for hero screens: a shaded body and two dark eyes, no mouth.
//
// Blink spacing, the 1-in-5 double blink, glance spacing and the exponential
// glance ease are adapted from zongzhu-bot (MIT):
//   https://github.com/Liu-zhongxian/zongzhu-bot
//   xiaozhi-esp32/main/boards/common/zongzhu/eye_model.h
//   xiaozhi-esp32/main/boards/common/zongzhu/eye_display.h
//   commit b47140208d65ef46e53bf109d498dc6adf3116bd
// Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd.
// Copyright (c) 2025 Project Contributors
// The full license is LICENSES/zongzhu-MIT.txt.
//
// Eye shapes and which expression a state holds are informed by the Grok Bot
// iOS app v0.18.0 activity table (unofficial extract; see docs/faces.md). The
// drawing and the timing code here are original. Nothing reads a clock or an
// RNG; every pose is a function of the frame counter and Style the app passes.
//
// Colours and timings to edit:
//   kBlue / kRed / kDim / kEye     gradient stops and the pupil (RGB888)
//   kBlinkWindow / kGlanceWindow   how often the face blinks and looks aside
//   kBreathAmp / kSpeakAmp         idle breathing and speaking bounce
#pragma once

#include <cstdint>

#include "hg/canvas.hpp"
#include "hg/ui.hpp"

namespace hg::face {

// Body gradient stops, RGB888. Light is the top-left, the named colour is the
// middle, and shade is a deeper version of the same hue at the bottom-right.
// RGB565 cannot hold these exactly; rgb565() quantises them. The disc dithers
// with a 2-step ordered term plus Floyd–Steinberg, so the truncated remainder
// is spread instead of drawing as rings.
constexpr uint8_t kBlueR = 0x49, kBlueG = 0x9B, kBlueB = 0xF1;
constexpr uint8_t kBlueHiR = 0xA9, kBlueHiG = 0xD4, kBlueHiB = 0xFC;
constexpr uint8_t kBlueCoreR = 0xC5, kBlueCoreG = 0xE4, kBlueCoreB = 0xFE;
constexpr uint8_t kBlueShadeR = 0x3F, kBlueShadeG = 0x86, kBlueShadeB = 0xE6;

constexpr uint8_t kRedR = 0xEC, kRedG = 0x66, kRedB = 0x5E;
constexpr uint8_t kRedHiR = 0xF8, kRedHiG = 0xC4, kRedHiB = 0xBC;
constexpr uint8_t kRedCoreR = 0xFE, kRedCoreG = 0xD4, kRedCoreB = 0xCE;
constexpr uint8_t kRedShadeR = 0xE2, kRedShadeG = 0x5A, kRedShadeB = 0x52;

constexpr uint8_t kDimR = 0x3A, kDimG = 0x6C, kDimB = 0xA8;
constexpr uint8_t kDimHiR = 0x8F, kDimHiG = 0xB6, kDimHiB = 0xDE;
constexpr uint8_t kDimCoreR = 0xB4, kDimCoreG = 0xD0, kDimCoreB = 0xE8;
constexpr uint8_t kDimShadeR = 0x2F, kDimShadeG = 0x5E, kDimShadeB = 0x96;

constexpr uint8_t kEyeR = 0x0D, kEyeG = 0x0D, kEyeB = 0x0D;

enum class Mood : uint8_t {
  Neutral,    // ready, boot, cards
  Attentive,  // listening, questions
  Curious,    // pairing, and working when a status phrase is showing
  Thinking,   // waiting on the server: the body becomes three dots
  Speaking,
  Happy,
  Confused,  // a failed turn, and an idle reaction
  Sleepy,    // offline, before the body collapses
  Wink,
  Wide,  // searching, and an idle reaction
  Surprised,
  Alert,  // error: a red mark, no body
  Sleep,  // one dot
  Playful,
  Proud,
  Shy,
};

// What the app is doing to the body, on top of the mood.
// morph 0 = the body, 100 = three dots. shape 0 circle, 1 pebble, 2 squircle,
// 3 triangle; shape_k is the blend, 0..100. wake 0 = one dot, 100 = full body.
// reaction is documented in docs/faces.md (0 = the screen's own pose).
struct Style {
  uint8_t morph = 0;
  uint8_t shape = 0;
  uint8_t shape_k = 0;
  uint8_t wake = 100;
  uint8_t reaction = 0;
  bool asleep = false;
  bool swirl = false;
  bool busy = false;
};

// True while the silhouette outside the eye band is moving (rings, a shape
// blend, the thinking morph, or the sleep collapse). A settled pebble, a
// finished three-dot pose and a finished sleep dot are not wide: one wide
// flush has already cleared the old pixels.
bool anim_wide(const Style& style);

Mood mood_for(Screen screen, bool happy, uint8_t reaction = 0, bool busy = false);
// Thinking dots. A working/searching phrase (busy) keeps the body instead.
bool dots_for(Screen screen, bool busy = false);

struct Palette {
  uint16_t body, mid, highlight, core, shade, eye;
};

Palette palette_for(Screen screen);

// One eye, thousandths of the face diameter, origin at the face centre.
// w/h are the full pill size; tilt is degrees clockwise (screen y grows down).
struct Eye {
  int16_t x = 0, y = 0, w = 0, h = 0, tilt = 0;
};

struct Sample {
  Eye left, right;
  uint16_t blink = 0;  // 0 open .. 1000 fully shut
  uint8_t dots = 0;    // 0 hidden, otherwise 1..3 (which dot is lit)
};

// level and voice are 0..100 (microphone RMS, speaker RMS).
Sample sample(uint32_t frame, Mood mood, uint8_t level, uint8_t voice, bool dots);

// Hash of the pixels that will be drawn at this diameter. Equal keys mean the
// pose did not move. Level and voice are part of it. Speaking, the loading
// swirl and the three-dot pulse also mix in the raw frame. The UI still
// flushes a live face every tick: a key that misses a blink is how the panel
// stayed on one paint.
uint32_t picture_key(uint32_t frame, Mood mood, uint8_t level, uint8_t voice, bool dots, int diameter,
                     const Style& style = {});

// Rows the face can touch, in canvas coordinates. wide covers the body and the
// rings; morph >= 80 and not wide is the three-dot band. A settled blink uses
// the eye box from overlay_box instead of this band.
void anim_rows(int cy, int diameter, int& y0, int& y1, bool wide = false, uint8_t morph = 0);

// Axis-aligned box, x1/y1 exclusive. any is false when nothing was added.
struct Box {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool any = false;
};

Box unite(Box a, Box b);

// True when the cached body matches this pose, so a settled frame can blit it.
// backdrop is the logical RGB565 the hero area was cleared to.
bool body_cached(int cx, int cy, int diameter, Screen screen, uint16_t backdrop, const Style& style, int canvas_w,
                 bool swap);

// Eyes, and the dot pulse once the body is gone. Empty when nothing moves on
// top of the body.
Box overlay_box(int cx, int cy, int diameter, Screen screen, bool happy, uint32_t frame, uint8_t level, uint8_t voice,
                const Style& style);

// The overlay box remembered by the last draw or draw_overlay.
Box last_overlay();

// Copies cached body pixels (already in framebuffer byte order) over `box`.
// Gaps the cache does not cover are filled with backdrop.
void blit_body(Canvas& c, const Box& box, uint16_t backdrop);

// Eyes, rings in front, dots, or the error mark. Does not paint the body.
void draw_overlay(Canvas& c, int cx, int cy, int diameter, Screen screen, bool happy, uint32_t frame, uint8_t level,
                  uint8_t voice, const Style& style = {});

// Drops the cached body. The next draw rasterises it again.
void discard_body_cache();

// Body, eyes, and any overlay. The caller has already filled the background
// with `backdrop` (logical RGB565). A repeat of the same body blits the cache.
void draw(Canvas& c, int cx, int cy, int diameter, Screen screen, bool happy, uint32_t frame, uint8_t level,
          uint8_t voice, const Style& style = {}, uint16_t backdrop = 0);

}  // namespace hg::face
