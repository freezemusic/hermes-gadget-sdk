// How the device retries the Hermes WebSocket, apart from the socket itself
// (firmware/esp32/main/port_ws.cpp tears that down). A connect that never opens
// is a different failure from a server that hung up: the station can still look
// joined, on the channel it used before the router moved, while every new
// handshake dies at the same timeout. The waits grow from 1 s to 30 s with a
// little jitter so a fleet doesn't retry as one. After a few of those timeouts
// the station is dropped and joined again; after another few the device restarts.
#pragma once
#include <cstddef>
#include <cstdint>

namespace hg {

class LinkRecover {
 public:
  static constexpr uint32_t kCapMs = 30000;
  static constexpr uint32_t kBaseMs[] = {1000, 2000, 4000, 8000, 15000, 30000};
  static constexpr size_t kSteps = sizeof(kBaseMs) / sizeof(kBaseMs[0]);
  // Consecutive connect timeouts before the station is dropped and joined again,
  // and before a restart. The rejoin is the one in between: a restart that
  // didn't happen (the port has no restart) is asked again at the next multiple.
  static constexpr int kTimeoutsBeforeRejoin = 3;
  static constexpr int kTimeoutsBeforeRestart = 6;

  enum class Action : uint8_t { Retry, RejoinWifi, Restart };

  struct Decision {
    Action action = Action::Retry;
    uint32_t wait_ms = 0;
  };

  // `jitter` is 0..255. It adds up to a quarter of the current step, and the
  // sum never exceeds kCapMs. The step itself is the floor, so a retry is never
  // sooner than the table.
  Decision on_connect_timeout(uint8_t jitter) {
    ++timeouts_;
    Decision d;
    d.wait_ms = wait(jitter);
    if (timeouts_ >= kTimeoutsBeforeRestart && timeouts_ % kTimeoutsBeforeRestart == 0)
      d.action = Action::Restart;
    else if (timeouts_ % kTimeoutsBeforeRejoin == 0)
      d.action = Action::RejoinWifi;
    return d;
  }

  // The server closed, the handshake stalled, or the network dropped. Back off,
  // and don't treat it as a dead association.
  uint32_t on_other_drop(uint8_t jitter) { return wait(jitter); }

  // The socket opened: packets are getting through, so the association is live.
  void on_opened() { timeouts_ = 0; }

  // The session stayed up, a new server was saved, or the operator asked to
  // start the waits over. The timeout count is kept by on_manual(); reset()
  // clears that too.
  void on_settled() { step_ = 0; }
  void on_manual() { step_ = 0; }
  void reset() {
    step_ = 0;
    timeouts_ = 0;
  }

  // An authentication or protocol error: the next wait is the cap.
  void on_hard_failure() { step_ = kSteps - 1; }

  int timeouts() const { return timeouts_; }

 private:
  uint32_t wait(uint8_t jitter) {
    const size_t step = step_ < kSteps ? step_ : kSteps - 1;
    if (step_ < kSteps - 1) ++step_;
    const uint32_t base = kBaseMs[step];
    const uint32_t extra = (base / 4) * jitter / 255;
    const uint32_t sum = base + extra;
    return sum > kCapMs ? kCapMs : sum;
  }

  size_t step_ = 0;
  int timeouts_ = 0;
};

}  // namespace hg
