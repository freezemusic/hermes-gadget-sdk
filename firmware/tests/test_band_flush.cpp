#include <deque>
#include <string>
#include <vector>

#include "band_flush.hpp"
#include "check.hpp"

namespace {

// A fake panel: records each prepare, kick and wait, and answers waits from a
// script (true when nothing is scripted).
struct Panel {
  std::vector<std::string> ops;
  std::deque<bool> completions;
  bool kick_ok = true;
  int slots = 2;
  hg::BandFlush bands;
  Panel() : bands(make(2)) {}
  explicit Panel(int slot_count) : slots(slot_count), bands(make(slot_count)) {}

  hg::BandFlush make(int slot_count) {
    return hg::BandFlush(
        60, 20, 50, slot_count,
        [this](int slot, int y, int rows) {
          ops.push_back("prep " + std::to_string(slot) + " " + std::to_string(y) + "+" + std::to_string(rows));
        },
        [this](int slot, int y, int rows) {
          ops.push_back("kick " + std::to_string(slot) + " " + std::to_string(y) + "+" + std::to_string(rows));
          return kick_ok;
        },
        [this](uint32_t ms) {
          ops.push_back("wait " + std::to_string(ms));
          if (completions.empty()) return true;
          bool done = completions.front();
          completions.pop_front();
          return done;
        });
  }
};

using Ops = std::vector<std::string>;
using Event = hg::BandFlush::Event;

}  // namespace

TEST("BandFlush: two slots copy the next band while the previous transfer runs") {
  Panel p;
  CHECK(p.bands.flush(10, 55) == Event::None);
  CHECK(p.ops == Ops({"prep 0 10+20", "kick 0 10+20", "prep 1 30+20", "wait 50", "kick 1 30+20", "prep 0 50+5",
                      "wait 50", "kick 0 50+5", "wait 50"}));
}

TEST("BandFlush: one slot refills only after the transfer completes") {
  Panel p(1);
  CHECK(p.bands.flush(10, 55) == Event::None);
  CHECK(p.ops == Ops({"prep 0 10+20", "kick 0 10+20", "wait 50", "prep 0 30+20", "kick 0 30+20", "wait 50",
                      "prep 0 50+5", "kick 0 50+5", "wait 50"}));
}

TEST("BandFlush: a late transfer pauses drawing without blocking, then redraws everything") {
  Panel p;
  p.completions = {false};
  CHECK(p.bands.flush(0, 60) == Event::TimedOut);
  // The second band is copied into the idle slot, then the wait for the first fails.
  CHECK(p.ops == Ops({"prep 0 0+20", "kick 0 0+20", "prep 1 20+20", "wait 50"}));

  p.ops.clear();
  p.completions = {false};
  CHECK(p.bands.flush(40, 60) == Event::None);
  CHECK(p.ops == Ops({"wait 0"}));

  p.ops.clear();
  CHECK(p.bands.flush(20, 40) == Event::Resumed);
  CHECK(p.ops == Ops({"wait 0", "prep 0 0+20", "kick 0 0+20", "prep 1 20+20", "wait 50", "kick 1 20+20", "prep 0 40+20",
                      "wait 50", "kick 0 40+20", "wait 50"}));

  p.ops.clear();
  CHECK(p.bands.flush(0, 20) == Event::None);
  CHECK(p.ops == Ops({"prep 0 0+20", "kick 0 0+20", "wait 50"}));
}

TEST("BandFlush: a kick error stops drawing for good") {
  Panel p;
  p.kick_ok = false;
  CHECK(p.bands.flush(0, 60) == Event::Failed);
  CHECK(p.ops == Ops({"prep 0 0+20", "kick 0 0+20"}));

  p.ops.clear();
  p.kick_ok = true;
  CHECK(p.bands.flush(0, 60) == Event::None);
  CHECK(p.ops.empty());
}
