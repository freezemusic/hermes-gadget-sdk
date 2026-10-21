#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>

namespace hg {

// Sends framebuffer rows to an LCD a band at a time. With one staging slot the
// next band is copied only after that transfer completes. With two slots the
// idle buffer is filled while the other transfer is on the bus, so the copy
// overlaps DMA. The SPI window commands still run only when the bus is free,
// and a slot is never kicked again until its own transfer has completed.
// A late completion pauses drawing until it arrives; a kick error stops
// drawing until reboot. Neither waits longer than one timeout.
class BandFlush {
 public:
  // Copies rows [y, y + rows) into staging slot (0 or 1). Does not touch the bus.
  using Prepare = std::function<void(int slot, int y, int rows)>;
  // Programs the window and starts DMA from that slot. The slot stays owned by
  // DMA until Wait succeeds.
  using Kick = std::function<bool(int slot, int y, int rows)>;
  // Waits up to timeout_ms for the in-flight transfer to complete; true if it did.
  using Wait = std::function<bool(uint32_t timeout_ms)>;
  enum class Event { None, TimedOut, Resumed, Failed };

  // `slots` is 1 or 2. Anything else is treated as 1.
  BandFlush(int height, int band_rows, uint32_t timeout_ms, int slots, Prepare prepare, Kick kick, Wait wait)
      : height_(height), band_rows_(band_rows), timeout_ms_(timeout_ms), slots_(slots >= 2 ? 2 : 1),
        prepare_(std::move(prepare)), kick_(std::move(kick)), wait_(std::move(wait)) {}

  Event flush(int y0, int y1) {
    Event event = Event::None;
    if (state_ == State::Failed) return event;
    if (state_ == State::Pending) {
      if (!wait_(0)) return event;
      // Flushes skipped while paused never reached the panel; the framebuffer
      // still holds them, so draw all of it.
      state_ = State::Ready;
      y0 = 0;
      y1 = height_;
      event = Event::Resumed;
    }
    if (y1 < y0) y1 = y0;
    int slot = 0;
    bool busy = false;
    for (int y = y0; y < y1;) {
      const int rows = std::min(band_rows_, y1 - y);
      // The single slot is free only after its transfer completes. The second
      // slot can be filled first, while that transfer is still running.
      if (slots_ == 1 && busy) {
        if (!wait_(timeout_ms_)) {
          state_ = State::Pending;
          return Event::TimedOut;
        }
        busy = false;
      }
      prepare_(slot, y, rows);
      if (busy) {
        if (!wait_(timeout_ms_)) {
          state_ = State::Pending;
          return Event::TimedOut;
        }
        busy = false;
      }
      if (!kick_(slot, y, rows)) {
        state_ = State::Failed;
        return Event::Failed;
      }
      busy = true;
      y += rows;
      if (slots_ == 2) slot ^= 1;
    }
    if (busy && !wait_(timeout_ms_)) {
      state_ = State::Pending;
      return Event::TimedOut;
    }
    return event;
  }

 private:
  enum class State { Ready, Pending, Failed };
  int height_;
  int band_rows_;
  uint32_t timeout_ms_;
  int slots_;
  Prepare prepare_;
  Kick kick_;
  Wait wait_;
  State state_ = State::Ready;
};

}  // namespace hg
