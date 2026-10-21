// Active-low push buttons, polled from the app task with a short debounce.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include "driver/gpio.h"

namespace hgp {
namespace {
constexpr uint8_t kDebounceTicks = 3;  // 3 polls at ~10 ms
}

void Buttons::begin(const ButtonConfig& cfg) {
  const int gpios[4] = {cfg.talk, cfg.cancel, cfg.up, cfg.down};
  const hg::Button ids[4] = {hg::Button::Talk, hg::Button::Cancel, hg::Button::Up, hg::Button::Down};
  for (int i = 0; i < 4; ++i) {
    buttons_[i].gpio = gpios[i];
    buttons_[i].id = ids[i];
    if (gpios[i] < 0) continue;
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << gpios[i];
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
  }
}

void Buttons::sample() {
  // One step per scheduler tick. A paint that cooperates many times in the
  // same 10 ms must not burn the 3-tick debounce in one frame.
  const TickType_t now = xTaskGetTickCount();
  if (sampled_at_ == now) return;
  sampled_at_ = now;
  for (auto& b : buttons_) {
    if (b.gpio < 0) continue;
    const bool down = gpio_get_level(static_cast<gpio_num_t>(b.gpio)) == 0;
    if (down == b.pressed) {
      b.stable = 0;
      continue;
    }
    if (++b.stable >= kDebounceTicks) {
      b.pressed = down;
      b.stable = 0;
      if (pending_n_ < 8) pending_[pending_n_++] = Edge{b.id, down};
    }
  }
}

void Buttons::poll(hg::App& app) {
  sample();
  const uint8_t n = pending_n_;
  pending_n_ = 0;
  for (uint8_t i = 0; i < n; ++i) app.on_button(pending_[i].id, pending_[i].down);
}

}  // namespace hgp
