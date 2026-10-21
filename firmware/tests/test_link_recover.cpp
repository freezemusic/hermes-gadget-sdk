#include <vector>

#include "check.hpp"
#include "hg/link_recover.hpp"

TEST("link: waits grow from 1 s to 30 s, and jitter never makes one sooner or past the cap") {
  hg::LinkRecover plain, jittered;
  const uint32_t floor[] = {1000, 2000, 4000, 8000, 15000, 30000, 30000};
  for (uint32_t base : floor) {
    const uint32_t low = plain.on_other_drop(0);
    const uint32_t high = jittered.on_other_drop(255);
    CHECK_EQ(low, base);
    CHECK(high >= base);
    CHECK(high <= hg::LinkRecover::kCapMs);
    if (base < hg::LinkRecover::kCapMs) CHECK(high > base);
    else CHECK_EQ(high, hg::LinkRecover::kCapMs);
  }
}

TEST("link: connect timeouts rejoin Wi-Fi on the third and restart on the sixth") {
  hg::LinkRecover link;
  std::vector<hg::LinkRecover::Action> actions;
  for (int i = 0; i < 6; ++i) actions.push_back(link.on_connect_timeout(0).action);
  CHECK(actions[0] == hg::LinkRecover::Action::Retry);
  CHECK(actions[1] == hg::LinkRecover::Action::Retry);
  CHECK(actions[2] == hg::LinkRecover::Action::RejoinWifi);
  CHECK(actions[3] == hg::LinkRecover::Action::Retry);
  CHECK(actions[4] == hg::LinkRecover::Action::Retry);
  CHECK(actions[5] == hg::LinkRecover::Action::Restart);
}

TEST("link: a server drop does not count toward the Wi-Fi rejoin") {
  hg::LinkRecover link;
  link.on_other_drop(0);
  link.on_other_drop(0);
  link.on_other_drop(0);
  CHECK_EQ(link.timeouts(), 0);
  CHECK(link.on_connect_timeout(0).action == hg::LinkRecover::Action::Retry);
  CHECK_EQ(link.timeouts(), 1);
}

TEST("link: an open socket clears the timeout count, and a settled session the wait") {
  hg::LinkRecover link;
  link.on_connect_timeout(0);
  link.on_connect_timeout(0);
  link.on_opened();
  CHECK_EQ(link.timeouts(), 0);
  CHECK(link.on_connect_timeout(0).action == hg::LinkRecover::Action::Retry);
  // The wait had climbed. A session that stayed up starts it over.
  link.on_connect_timeout(0);
  link.on_connect_timeout(0);
  link.on_settled();
  CHECK_EQ(link.on_other_drop(0), 1000u);
}

TEST("link: a hard failure waits the cap, and a manual retry starts the waits over") {
  hg::LinkRecover link;
  link.on_hard_failure();
  CHECK_EQ(link.on_other_drop(0), hg::LinkRecover::kCapMs);
  link.on_manual();
  CHECK_EQ(link.on_other_drop(0), 1000u);
  // The timeout count is the operator's to keep: a manual retry is not a cure.
  link.on_connect_timeout(0);
  link.on_connect_timeout(0);
  link.on_manual();
  CHECK_EQ(link.timeouts(), 2);
  CHECK(link.on_connect_timeout(0).action == hg::LinkRecover::Action::RejoinWifi);
}
