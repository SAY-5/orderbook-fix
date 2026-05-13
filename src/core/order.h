// SPDX-License-Identifier: MIT
#pragma once

#include "types.h"

namespace obfix {

// Intrusive doubly-linked list node so each price level is a stable
// time-ordered queue and orders can be cancelled in O(1).
struct Order {
    OrderId id{0};
    SessionId session{0};
    ClOrdID clord_id{};
    Side side{Side::Buy};
    OrdType type{OrdType::Limit};
    Price price{0};
    Quantity orig_qty{0};
    Quantity leaves_qty{0};
    Timestamp ts{0};

    Order* prev{nullptr};
    Order* next{nullptr};

    Quantity cum_filled() const { return orig_qty - leaves_qty; }
};

}  // namespace obfix
