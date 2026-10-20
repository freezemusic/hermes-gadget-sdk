// Procedural face for hero screens: a flat coloured disc and two dark pill eyes.
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
// Adapted for Hermes Gadget: integer thousandths of the face diameter (the
// upstream table is pixel-sized white eyes), moods mapped onto Screen, and an
// original coloured-disc drawing. Timing assumes a face frame is 33 ms (about
// 30 fps; see kFaceFrameMs in app.cpp). Nothing here reads a clock or an RNG;
// every pose is a function of the frame counter the app passes in.
//
// Colours and timings to edit:
//   kBlue / kRed / kDim / kEye     body and pupil colours (RGB888)
//   kBlinkWindow / kGlanceWindow   how often the face blinks and looks aside
//   kBreathAmp / kSpeakAmp         idle breathing and speaking bounce
#pragma once

#include <cstdint>

#include "hg/canvas.hpp"
#include "hg/ui.hpp"

namespace hg::face {

// Body colours, RGB888. RGB565 cannot hold #499BF1 exactly; the constants are
// the source of truth and rgb565() quantises them at compile time.
constexpr uint8_t kBlueR = 0x49, kBlueG = 0x9B, kBlueB = 0xF1;
constexpr uint8_t kBlueHiR = 0x8E, kBlueHiG = 0xC6, kBlueHiB = 0xFB;
constexpr uint8_t kBlueCoreR = 0xD6, kBlueCoreG = 0xEA, kBlueCoreB = 0xFE;
constexpr uint8_t kBlueShadeR = 0x2E, kBlueShadeG = 0x78, kBlueShadeB = 0xD4;

constexpr uint8_t kRedR = 0xEC, kRedG = 0x66, kRedB = 0x5E;
constexpr uint8_t kRedHiR = 0xF8, kRedHiG = 0xA4, kRedHiB = 0x9C;
constexpr uint8_t kRedCoreR = 0xFE, kRedCoreG = 0xD4, kRedCoreB = 0xCE;
constexpr uint8_t kRedShadeR = 0xC4, kRedShadeG = 0x40, kRedShadeB = 0x3C;

constexpr uint8_t kDimR = 0x3A, kDimG = 0x6C, kDimB = 0xA8;
constexpr uint8_t kDimHiR = 0x74, kDimHiG = 0xA4, kDimHiB = 0xD0;
constexpr uint8_t kDimCoreR = 0xB4, kDimCoreG = 0xD0, kDimCoreB = 0xE8;
constexpr uint8_t kDimShadeR = 0x24, kDimShadeG = 0x50, kDimShadeB = 0x80;

constexpr uint8_t kEyeR = 0x0D, kEyeG = 0x0D, kEyeB = 0x0D;

enum class Mood : uint8_t {
  Neutral,    // ready, boot, cards
  Attentive,  // listening, questions
  Curious,    // pairing, connecting, updating
  Thinking,
  Speaking,
  Happy,
  Confused,  // error
  Sleepy,    // offline
};

Mood mood_for(Screen screen, bool happy);
bool dots_for(Screen screen);

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

// Hash of the pixels sample() will draw at this diameter. Equal keys mean the
// eye band does not need another flush. Speaking mixes in the raw frame so the
// bounce is sent every face frame.
uint32_t picture_key(uint32_t frame, Mood mood, uint8_t level, uint8_t voice, bool dots, int diameter);

// Rows the eyes, glances and thinking dots can touch, in canvas coordinates.
void anim_rows(int cy, int diameter, int& y0, int& y1);

// Disc plus eyes. The caller has already filled the background.
void draw(Canvas& c, int cx, int cy, int diameter, Screen screen, bool happy, uint32_t frame, uint8_t level,
          uint8_t voice);

}  // namespace hg::face
