// A port can run this between rows of a long paint. The core stays
// single-threaded: the hook must not call back into App or Ui.
#pragma once

namespace hg {

using CooperateFn = void (*)();

inline CooperateFn& cooperate_slot() {
  static CooperateFn fn = nullptr;
  return fn;
}

inline void set_cooperate(CooperateFn fn) { cooperate_slot() = fn; }

inline void cooperate() {
  if (CooperateFn fn = cooperate_slot()) fn();
}

}  // namespace hg
