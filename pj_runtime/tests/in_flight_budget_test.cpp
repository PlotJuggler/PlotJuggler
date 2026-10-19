// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The in-flight budget on its own (docs/derived_objects/ARCHITECTURE.md §11): one line of waiters, served first in,
// first out from a drain that never runs inside the releasing call. Order is proven by the grant log, never by time.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "event_pump_test_utils.h"
#include "pj_runtime/InFlightBudget.h"

namespace PJ {
namespace {

using Reservation = InFlightBudget::Reservation;

/// Records each grant by name and keeps the reservations it was given, so the test decides when they return.
struct Grants {
  std::vector<std::string> log;
  std::vector<std::optional<Reservation>> held;

  std::function<void(Reservation)> to(std::string name) {
    return [this, name = std::move(name)](Reservation room) {
      log.push_back(name);
      held.emplace_back(std::move(room));
    };
  }
  /// Returns the room granted to the `index`-th grant.
  void release(size_t index) {
    held.at(index).reset();
  }
};

/// Runs the drains the budget posted.
void pumpPostedDrains() {
  test::pumpFor(20);
}

// A release posts the drain instead of serving the line inside the destructor; the drain then grants the waiters in
// arrival order while the head fits, and stops at the first that does not.
TEST(InFlightBudgetTest, WaitersAreGrantedInOrderAndNeverInsideTheReleasingCall) {
  InFlightBudget budget(100);
  Grants grants;
  std::optional<Reservation> full = budget.tryReserve(100);
  ASSERT_TRUE(full.has_value());
  const InFlightBudget::Ticket first = budget.whenRoom(40, grants.to("a"));
  const InFlightBudget::Ticket second = budget.whenRoom(40, grants.to("b"));
  const InFlightBudget::Ticket third = budget.whenRoom(40, grants.to("c"));
  pumpPostedDrains();
  EXPECT_TRUE(grants.log.empty()) << "no room yet";

  full.reset();
  EXPECT_TRUE(grants.log.empty()) << "not inside the releasing call";
  EXPECT_EQ(budget.reserved(), 0u);
  pumpPostedDrains();
  EXPECT_EQ(grants.log, (std::vector<std::string>{"a", "b"})) << "in order, while the head fits";
  EXPECT_EQ(budget.reserved(), 80u);

  grants.release(0);
  EXPECT_EQ(grants.log.size(), 2u);
  budget.drain();  // a consumer serves the line at its own point
  EXPECT_EQ(grants.log, (std::vector<std::string>{"a", "b", "c"}));
  EXPECT_EQ(budget.reserved(), 80u);

  pumpPostedDrains();  // the drain the release posted finds nothing left to grant
  const InFlightBudget::Ticket late = budget.whenRoom(20, grants.to("d"));
  EXPECT_EQ(grants.log.size(), 3u) << "room is free, yet not inside the waiting call";
  pumpPostedDrains();
  EXPECT_EQ(grants.log.back(), "d");
}

// A small request that would fit by itself waits behind an earlier large waiter: it neither starts at once nor is
// granted ahead of it, so mixed sizes cannot starve the large one.
TEST(InFlightBudgetTest, ASmallNewcomerQueuesBehindALargeWaiter) {
  InFlightBudget budget(100);
  Grants grants;
  std::optional<Reservation> running = budget.tryReserve(60);
  ASSERT_TRUE(running.has_value());
  const InFlightBudget::Ticket large = budget.whenRoom(80, grants.to("large"));
  EXPECT_FALSE(budget.tryReserve(30).has_value()) << "30 fit beside 60, but the large one waits first";
  const InFlightBudget::Ticket small = budget.whenRoom(30, grants.to("small"));
  pumpPostedDrains();
  budget.drain();
  EXPECT_TRUE(grants.log.empty()) << "the small one does not pass the large one, even in a drain";

  running.reset();
  pumpPostedDrains();
  EXPECT_EQ(grants.log, std::vector<std::string>{"large"}) << "30 do not fit beside 80";
  grants.release(0);
  pumpPostedDrains();
  EXPECT_EQ(grants.log, (std::vector<std::string>{"large", "small"}));
  EXPECT_TRUE(budget.tryReserve(70).has_value()) << "nobody waits: room at once";
}

// BUD-06 (changed): a run larger than the whole budget runs alone once nothing else is reserved, rather than never;
// a reservation grows past the capacity only when it is the only one.
TEST(InFlightBudgetTest, ARunLargerThanTheBudgetRunsAlone) {
  InFlightBudget budget(100);
  Grants grants;
  {
    std::optional<Reservation> alone = budget.tryReserve(150);
    ASSERT_TRUE(alone.has_value()) << "nothing else is reserved";
    EXPECT_FALSE(budget.tryReserve(1).has_value());
    EXPECT_TRUE(alone->grow(50)) << "the only reservation";
    EXPECT_EQ(budget.reserved(), 200u);
  }
  {
    std::optional<Reservation> first = budget.tryReserve(60);
    const std::optional<Reservation> second = budget.tryReserve(30);
    ASSERT_TRUE(first.has_value() && second.has_value());
    EXPECT_FALSE(first->grow(20)) << "110 of 100, beside another reservation";
    EXPECT_TRUE(first->grow(10)) << "100 of 100 fit";
    EXPECT_EQ(budget.reserved(), 100u);
  }
  std::optional<Reservation> small = budget.tryReserve(50);
  ASSERT_TRUE(small.has_value());
  const InFlightBudget::Ticket large = budget.whenRoom(150, grants.to("large"));
  pumpPostedDrains();
  EXPECT_TRUE(grants.log.empty()) << "it waits for the budget to be empty";
  small.reset();
  pumpPostedDrains();
  EXPECT_EQ(grants.log, std::vector<std::string>{"large"}) << "alone, rather than never";
  EXPECT_EQ(budget.reserved(), 150u);
}

// A ticket that left the line is never granted, and leaving the head lets the next waiter in when it fits, without
// any release.
TEST(InFlightBudgetTest, ACancelledTicketIsNeverGranted) {
  InFlightBudget budget(100);
  Grants grants;
  std::optional<Reservation> running = budget.tryReserve(50);
  ASSERT_TRUE(running.has_value());
  std::optional<InFlightBudget::Ticket> large(budget.whenRoom(80, grants.to("large")));
  const InFlightBudget::Ticket small = budget.whenRoom(10, grants.to("small"));
  pumpPostedDrains();
  EXPECT_TRUE(grants.log.empty()) << "the large head blocks the line";

  large.reset();
  pumpPostedDrains();
  EXPECT_EQ(grants.log, std::vector<std::string>{"small"}) << "the next waiter fits once the head left";

  InFlightBudget::Ticket replaced = budget.whenRoom(90, grants.to("replaced"));
  replaced = budget.whenRoom(10, grants.to("kept"));  // assigning over a ticket leaves the line
  running.reset();
  pumpPostedDrains();
  EXPECT_EQ(grants.log, (std::vector<std::string>{"small", "kept"}));
}

// A waiter placed at the front is served before the earlier ones: the run that already staged takes its room first.
TEST(InFlightBudgetTest, AFrontWaiterIsServedFirst) {
  InFlightBudget budget(100);
  Grants grants;
  std::optional<Reservation> running = budget.tryReserve(100);
  const InFlightBudget::Ticket back = budget.whenRoom(10, grants.to("back"));
  const InFlightBudget::Ticket front = budget.whenRoom(95, grants.to("front"), InFlightBudget::Place::kFront);
  running.reset();
  pumpPostedDrains();
  EXPECT_EQ(grants.log, std::vector<std::string>{"front"});
}

// A ticket may outlive its budget (a queued job the caller keeps after the session went): the budget empties its line
// when destroyed, so the ticket's cleanup touches nothing (the ASan lane catches the read of the freed budget).
TEST(InFlightBudgetTest, ATicketMayOutliveItsBudget) {
  auto budget = std::make_unique<InFlightBudget>(100);
  Grants grants;
  std::optional<InFlightBudget::Ticket> queued(budget->whenRoom(40, grants.to("queued")));
  budget.reset();
  queued.reset();
  pumpPostedDrains();
  EXPECT_TRUE(grants.log.empty());
}

}  // namespace
}  // namespace PJ
