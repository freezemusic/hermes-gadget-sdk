// The recording trim and the upload normaliser (hg/mic_capture.hpp).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "check.hpp"
#include "hg/mic_capture.hpp"

namespace {

int peak_of(const std::vector<int16_t>& s) {
  int peak = 0;
  for (int16_t v : s) {
    const int a = v < 0 ? -static_cast<int>(v) : static_cast<int>(v);
    if (a > peak) peak = a;
  }
  return peak;
}

void fill_square(std::vector<int16_t>& b, int amp, size_t half = 16) {
  for (size_t i = 0; i < b.size(); ++i) b[i] = ((i / half) % 2) ? -amp : amp;
}

// Runs the capture the way the app does: shape, then normalise, across chunks
// shorter than the trim, because the codec posts about 13 ms at a time.
std::vector<int16_t> run(hg::MicCapture& cap, const std::vector<int16_t>& in, size_t chunk = 640) {
  std::vector<int16_t> out;
  for (size_t i = 0; i < in.size();) {
    const size_t n = std::min(chunk, in.size() - i);
    std::vector<int16_t> buf(in.begin() + static_cast<std::ptrdiff_t>(i),
                             in.begin() + static_cast<std::ptrdiff_t>(i + n));
    const size_t kept = cap.shape(buf.data(), buf.size());
    if (kept) {
      cap.normalise(buf.data(), kept);
      out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(kept));
    }
    i += n;
  }
  return out;
}

}  // namespace

TEST("mic: the high-pass sits near 100 Hz at both capture rates") {
  CHECK_EQ(hg::MicCapture::highpass_q15(16000), 31506);
  CHECK_EQ(hg::MicCapture::highpass_q15(24000), 31921);
  CHECK(hg::MicCapture::highpass_q15(24000) > hg::MicCapture::highpass_q15(16000));
  CHECK_EQ(hg::MicCapture::boost_q10(0), hg::MicCapture::kUnityQ10);
  CHECK_EQ(hg::MicCapture::boost_q10(30), 32382);  // 10^(30/20) in Q10
}

TEST("mic: the opening click is dropped and a quiet voice is raised toward -3 dBFS") {
  // 200 ms of full scale, then 200 ms of a ±400 square. That is the watch clip:
  // a clipped press, then speech peaking around a few hundred counts.
  const size_t trim = 16000u * hg::MicCapture::kTrimMs / 1000u;
  CHECK_EQ(trim, size_t(3200));
  std::vector<int16_t> in(trim, 32767);
  std::vector<int16_t> voice(trim);
  fill_square(voice, 400);
  in.insert(in.end(), voice.begin(), voice.end());

  hg::MicCapture cap;
  cap.reset(16000, hg::MicCapture::kDefaultBoostDb);
  const std::vector<int16_t> out = run(cap, in, 320);  // shorter than the trim
  CHECK_EQ(out.size(), trim);  // the full-scale press is gone
  const int peak = peak_of(out);
  // ±400 cannot reach −3 dBFS under the +30 dB ceiling, but it must come up
  // well clear of the raw recording. The high-pass rings on a square edge, so
  // the peak sits above 400·31.6 and still under the −3 dBFS target.
  CHECK(peak > 10000);
  CHECK(peak <= hg::MicCapture::kTargetPeak);
}

TEST("mic: a loud block later in the take is limited to about -3 dBFS") {
  hg::MicCapture cap;
  cap.reset(16000, 30, 0);
  std::vector<int16_t> quiet(640), loud(640);
  fill_square(quiet, 400);
  fill_square(loud, 8000);
  CHECK_EQ(cap.shape(quiet.data(), quiet.size()), quiet.size());
  cap.normalise(quiet.data(), quiet.size());
  CHECK(peak_of(quiet) > 10000);  // the ceiling, not yet the −3 dBFS target
  CHECK_EQ(cap.shape(loud.data(), loud.size()), loud.size());
  cap.normalise(loud.data(), loud.size());
  const int peak = peak_of(loud);
  CHECK(peak > 22000);
  CHECK(peak <= hg::MicCapture::kTargetPeak);
}

TEST("mic: pure noise under the floor is not amplified, and a hot block is turned down") {
  hg::MicCapture cap;
  cap.reset(16000, 30, 0);
  std::vector<int16_t> noise(1000);
  fill_square(noise, 10, 2);  // ±10, about −70 dBFS, under the floor
  CHECK_EQ(cap.shape(noise.data(), noise.size()), noise.size());
  cap.normalise(noise.data(), noise.size());
  // A step of 20 rings a little in the high-pass. +30 dB would take it to
  // several hundred, which is what this bound rules out.
  CHECK(peak_of(noise) <= 32);

  std::vector<int16_t> hot(640);
  fill_square(hot, 30000);
  hg::MicCapture limit;
  limit.reset(16000, 30, 0);
  limit.shape(hot.data(), hot.size());
  limit.normalise(hot.data(), hot.size());
  const int peak = peak_of(hot);
  CHECK(peak > 22000);
  CHECK(peak <= hg::MicCapture::kTargetPeak);
  CHECK(peak < 30000);
}

TEST("mic: a DC offset is gone within a few tens of milliseconds") {
  hg::MicCapture cap;
  cap.reset(16000, 0, 0);  // unity ceiling: this is the filter, not the gain
  std::vector<int16_t> dc(1600, 8000);
  CHECK_EQ(cap.shape(dc.data(), dc.size()), dc.size());
  cap.normalise(dc.data(), dc.size());
  int tail = 0;
  for (size_t i = dc.size() - 200; i < dc.size(); ++i) {
    const int a = dc[i] < 0 ? -dc[i] : dc[i];
    if (a > tail) tail = a;
  }
  CHECK(tail < 20);
  // Speech-band energy still passes: a 2 kHz square keeps most of its level.
  std::vector<int16_t> tone(800);
  fill_square(tone, 1000, 4);
  hg::MicCapture speech;
  speech.reset(16000, 0, 0);
  speech.shape(tone.data(), tone.size());
  CHECK(peak_of(tone) > 700);
}

TEST("mic: a zero boost leaves a quiet voice quiet, and the trim still applies at 24 kHz") {
  const size_t trim = 24000u * hg::MicCapture::kTrimMs / 1000u;
  CHECK_EQ(trim, size_t(4800));
  std::vector<int16_t> in(trim, 20000);
  std::vector<int16_t> voice(2400);
  fill_square(voice, 400);
  in.insert(in.end(), voice.begin(), voice.end());
  hg::MicCapture cap;
  cap.reset(24000, 0);
  const std::vector<int16_t> out = run(cap, in, 320);
  CHECK_EQ(out.size(), voice.size());
  const int peak = peak_of(out);
  CHECK(peak > 250);
  CHECK(peak < 600);
}
