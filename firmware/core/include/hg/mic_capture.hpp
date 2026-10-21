// Turns a microphone block into the PCM that is uploaded.
//
// Saved clips from the AMOLED-2.06 peaked around 100–400 counts (−40 to −50 dBFS)
// and opened with a button or touch click of several thousand counts. A speech
// detector treats that as silence, or locks onto the click. The PGA cannot make
// up the difference (the ES7210 stops at 37.5 dB), so each recording:
//   1. drops the first 200 ms, which is the click — nothing in the firmware
//      plays a tone when talk starts;
//   2. high-passes near 100 Hz, so the thump and a DC offset do not set the gain;
//   3. raises the peak toward −3 dBFS, and never by more than the configured
//      ceiling, so a quiet room is not amplified to full scale.
// Speech with a crest of about 17 dB then sits near −20 dBFS RMS. The same gain
// is kept for the rest of the recording and only reduced when a louder block
// would pass −3 dBFS, so a pause does not pump the noise up and a late peak
// does not wrap.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hg {

class MicCapture {
 public:
  static constexpr uint32_t kTrimMs = 200;
  static constexpr int kDefaultBoostDb = 30;  // software ceiling, not the codec PGA
  static constexpr int kBoostDbMax = 36;
  static constexpr int kPgaDbMax = 42;  // ES8311's top step; an ES7210 ignores past 37
  // 32767 * 10^(−3/20). The upload's target peak.
  static constexpr int kTargetPeak = 23197;
  // Blocks quieter than this (about −57 dBFS) are not used to choose the gain.
  static constexpr int kMinPeak = 48;
  static constexpr int32_t kUnityQ10 = 1024;

  MicCapture() { reset(16000, kDefaultBoostDb); }

  // `boost_db` is the software ceiling (0 keeps the level, still trims and filters).
  // `trim_ms` is overridable so a test can exercise one stage.
  void reset(uint32_t sample_rate, int boost_db, uint32_t trim_ms = kTrimMs) {
    rate_ = sample_rate == 0 ? 16000u : sample_rate;
    drop_left_ = static_cast<size_t>(rate_) * static_cast<size_t>(trim_ms) / 1000u;
    x1_ = 0;
    y1_ = 0;
    gained_ = false;
    gain_q10_ = kUnityQ10;
    a_q15_ = highpass_q15(rate_);
    set_boost_db(boost_db);
  }

  // Lowers the ceiling immediately. A recording that already chose a higher
  // gain drops to the new ceiling on the next block.
  void set_boost_db(int db) {
    if (db < 0) db = 0;
    if (db > kBoostDbMax) db = kBoostDbMax;
    boost_q10_ = boost_q10(db);
    if (gained_ && gain_q10_ > boost_q10_) gain_q10_ = boost_q10_;
  }

  // One-pole high-pass coefficient, Q15. 16 kHz and 24 kHz are the rates the
  // firmware actually captures at; the values are exp(−2π·100/rate)·32768.
  static int32_t highpass_q15(uint32_t sample_rate) {
    if (sample_rate == 16000) return 31506;
    if (sample_rate == 24000) return 31921;
    if (sample_rate == 0) return 0;
    const double a = std::exp(-2.0 * 3.14159265358979323846 * 100.0 / static_cast<double>(sample_rate));
    int q = static_cast<int>(std::lround(a * 32768.0));
    if (q < 0) q = 0;
    if (q > 32767) q = 32767;
    return q;
  }

  static int32_t boost_q10(int db) {
    if (db <= 0) return kUnityQ10;
    if (db > kBoostDbMax) db = kBoostDbMax;
    const double lin = std::pow(10.0, static_cast<double>(db) / 20.0);
    int q = static_cast<int>(std::lround(lin * 1024.0));
    if (q < kUnityQ10) q = kUnityQ10;
    return q;
  }

  // Drops the opening click and high-passes what remains, in place.
  // Returns how many samples at the front are kept (0 while the trim lasts).
  size_t shape(int16_t* samples, size_t count) {
    if (samples == nullptr || count == 0) return 0;
    size_t in = 0;
    if (drop_left_ > 0) {
      const size_t skip = drop_left_ < count ? drop_left_ : count;
      drop_left_ -= skip;
      in = skip;
    }
    size_t out = 0;
    for (; in < count; ++in) samples[out++] = highpass(samples[in]);
    return out;
  }

  // Peak normaliser. A block under kMinPeak is left as it is, so silence keeps
  // the board's own noise floor. The first louder block sets the gain; a later
  // louder block can only turn it down.
  void normalise(int16_t* samples, size_t count) {
    if (samples == nullptr || count == 0) return;
    int32_t peak = 0;
    for (size_t i = 0; i < count; ++i) {
      const int32_t a = samples[i] < 0 ? -static_cast<int32_t>(samples[i]) : static_cast<int32_t>(samples[i]);
      if (a > peak) peak = a;
    }
    int32_t apply = kUnityQ10;
    if (peak >= kMinPeak) {
      int32_t want = static_cast<int32_t>((static_cast<int64_t>(kTargetPeak) << 10) / peak);
      if (want > boost_q10_) want = boost_q10_;
      if (want < 1) want = 1;
      if (!gained_ || want < gain_q10_) gain_q10_ = want;
      gained_ = true;
      apply = gain_q10_;
    }
    if (apply == kUnityQ10) return;
    for (size_t i = 0; i < count; ++i) {
      int32_t y = static_cast<int32_t>((static_cast<int64_t>(samples[i]) * apply) >> 10);
      if (y > 32767) y = 32767;
      if (y < -32768) y = -32768;
      samples[i] = static_cast<int16_t>(y);
    }
  }

 private:
  int16_t highpass(int16_t x) {
    // One-pole high-pass, fc ≈ 100 Hz: y[n] = x[n] − x[n−1] + a·y[n−1].
    const int32_t y = (static_cast<int32_t>(x) - x1_) +
                      static_cast<int32_t>((static_cast<int64_t>(a_q15_) * y1_) >> 15);
    x1_ = x;
    int32_t clamped = y;
    if (clamped > 32767) clamped = 32767;
    if (clamped < -32768) clamped = -32768;
    y1_ = clamped;
    return static_cast<int16_t>(clamped);
  }

  uint32_t rate_ = 16000;
  size_t drop_left_ = 0;
  int32_t x1_ = 0;
  int32_t y1_ = 0;
  int32_t a_q15_ = 31506;
  int32_t boost_q10_ = kUnityQ10;
  int32_t gain_q10_ = kUnityQ10;
  bool gained_ = false;
};

}  // namespace hg
