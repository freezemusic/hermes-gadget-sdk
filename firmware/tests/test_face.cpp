// The procedural face: blink and glance schedules, and that the disc and the
// eyes actually land in the framebuffer.
#include <vector>

#include "check.hpp"
#include "hg/face.hpp"
#include "hg/ui.hpp"

namespace {

int count_color(const std::vector<uint16_t>& fb, uint16_t color) {
  int n = 0;
  for (uint16_t p : fb) n += p == color;
  return n;
}

}  // namespace

TEST("face: blinks are 2.6-6s apart and every fifth blink is a double") {
  std::vector<uint32_t> ons;
  bool prev = false;
  for (uint32_t f = 0; f < 5000; ++f) {
    const bool on = hg::face::sample(f, hg::face::Mood::Neutral, 0, 0, false).blink > 0;
    if (on && !prev) ons.push_back(f);
    prev = on;
  }
  CHECK(ons.size() > 20);
  int doubles = 0;
  for (size_t i = 1; i < ons.size(); ++i) {
    const uint32_t gap = ons[i] - ons[i - 1];
    if (gap < 20) {
      ++doubles;
      CHECK(gap >= 7);  // the second pulse of a double, after the 2-frame gap
    } else {
      CHECK(gap >= 78);
      CHECK(gap <= 180);
    }
  }
  // One double (two pulses, so one short gap) per five blinks.
  // Each double is two pulses, so it adds one short gap and one extra entry.
  CHECK(doubles > 0);
  const int events = static_cast<int>(ons.size()) - doubles;
  CHECK(doubles * 5 >= events - 5);
  CHECK(doubles * 5 <= events + 5);
}

TEST("face: glances move the eyes and come back") {
  const int home = hg::face::sample(0, hg::face::Mood::Neutral, 0, 0, false).left.x;
  bool moved = false, back = false;
  for (uint32_t f = 0; f < 800; ++f) {
    const int x = hg::face::sample(f, hg::face::Mood::Neutral, 0, 0, false).left.x;
    if (x != home) moved = true;
    if (moved && x == home) back = true;
    CHECK(x > home - 80);
    CHECK(x < home + 80);
  }
  CHECK(moved);
  CHECK(back);
}

TEST("face: a shut blink is a thinner eye than an open one, on a blue disc") {
  struct Panel : hg::Display {
    std::vector<uint16_t> fb = std::vector<uint16_t>(120 * 120, 0);
    hg::DisplayInfo info() const override {
      hg::DisplayInfo d;
      d.width = d.height = 120;
      return d;
    }
    uint16_t* framebuffer() override { return fb.data(); }
    void flush(uint16_t, uint16_t) override {}
  } panel;
  hg::Canvas canvas(panel.framebuffer(), 120, 120, false);
  const uint16_t blue = hg::rgb565(hg::face::kBlueR, hg::face::kBlueG, hg::face::kBlueB);
  const uint16_t eye = hg::rgb565(hg::face::kEyeR, hg::face::kEyeG, hg::face::kEyeB);

  uint32_t open_f = 0, shut_f = 0;
  bool found_shut = false;
  for (uint32_t f = 0; f < 2000; ++f) {
    const auto s = hg::face::sample(f, hg::face::Mood::Neutral, 0, 0, false);
    if (s.blink == 0) open_f = f;
    if (s.blink >= 1000) {
      shut_f = f;
      found_shut = true;
      break;
    }
  }
  CHECK(found_shut);

  auto paint = [&](uint32_t frame) {
    std::fill(panel.fb.begin(), panel.fb.end(), 0);
    hg::face::draw(canvas, 60, 60, 100, hg::Screen::Ready, false, frame, 0, 0);
  };
  paint(open_f);
  const int open_eyes = count_color(panel.fb, eye);
  const int blue_px = count_color(panel.fb, blue);
  CHECK(blue_px > 500);
  CHECK(open_eyes > 40);
  paint(shut_f);
  const int shut_eyes = count_color(panel.fb, eye);
  CHECK(shut_eyes > 0);
  CHECK(shut_eyes < open_eyes / 2);

  // Same model, same pixels.
  const uint32_t key = hg::face::picture_key(open_f, hg::face::Mood::Neutral, 0, 0, false, 100);
  CHECK(key == hg::face::picture_key(open_f, hg::face::Mood::Neutral, 0, 0, false, 100));
  CHECK(key != hg::face::picture_key(shut_f, hg::face::Mood::Neutral, 0, 0, false, 100));
}

TEST("face: moods pick a pose and a body colour") {
  CHECK(hg::face::mood_for(hg::Screen::Ready, false) == hg::face::Mood::Neutral);
  CHECK(hg::face::mood_for(hg::Screen::Ready, true) == hg::face::Mood::Happy);
  CHECK(hg::face::mood_for(hg::Screen::Listening, false) == hg::face::Mood::Attentive);
  CHECK(hg::face::mood_for(hg::Screen::Thinking, false) == hg::face::Mood::Thinking);
  CHECK(hg::face::mood_for(hg::Screen::Responding, false) == hg::face::Mood::Speaking);
  CHECK(hg::face::mood_for(hg::Screen::Error, false) == hg::face::Mood::Confused);
  CHECK(hg::face::mood_for(hg::Screen::Pairing, false) == hg::face::Mood::Curious);
  CHECK(hg::face::mood_for(hg::Screen::Connecting, false) == hg::face::Mood::Curious);
  CHECK(hg::face::mood_for(hg::Screen::Offline, false) == hg::face::Mood::Sleepy);
  CHECK(hg::face::dots_for(hg::Screen::Thinking));
  CHECK(!hg::face::dots_for(hg::Screen::Ready));

  const auto loud = hg::face::sample(10, hg::face::Mood::Attentive, 100, 0, false);
  const auto quiet = hg::face::sample(10, hg::face::Mood::Attentive, 0, 0, false);
  CHECK(loud.left.h > quiet.left.h);

  const auto red = hg::face::palette_for(hg::Screen::Error);
  const auto blue = hg::face::palette_for(hg::Screen::Ready);
  CHECK(red.body != blue.body);
  CHECK(hg::face::palette_for(hg::Screen::Offline).body != blue.body);
}

TEST("face: speaking changes every frame; a settled idle face does not") {
  const uint32_t a = hg::face::picture_key(1000, hg::face::Mood::Speaking, 0, 50, false, 200);
  const uint32_t b = hg::face::picture_key(1001, hg::face::Mood::Speaking, 0, 50, false, 200);
  CHECK(a != b);
  // Far from a blink or a glance edge, two consecutive idle frames can share
  // pixels. The key must not change just because the counter did.
  bool stable = false;
  for (uint32_t f = 200; f < 600; ++f) {
    const auto s = hg::face::sample(f, hg::face::Mood::Neutral, 0, 0, false);
    if (s.blink != 0) continue;
    const uint32_t k0 = hg::face::picture_key(f, hg::face::Mood::Neutral, 0, 0, false, 200);
    const uint32_t k1 = hg::face::picture_key(f + 1, hg::face::Mood::Neutral, 0, 0, false, 200);
    if (k0 == k1) {
      stable = true;
      break;
    }
  }
  CHECK(stable);
}
