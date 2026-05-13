// SPDX-License-Identifier: MIT
#pragma once

#include "types.h"
#include <string>
#include <variant>
#include <vector>

namespace obfix {

struct Trade {
    OrderId aggressor_id;
    OrderId resting_id;
    ClOrdID aggressor_clord;
    ClOrdID resting_clord;
    Price price;
    Quantity qty;
    Side aggressor_side;
};

struct Ack {
    OrderId id;
    ClOrdID clord_id;
};

struct OrdReject {
    ClOrdID clord_id;
    std::string reason;
};

struct CancelAck {
    OrderId id;
    ClOrdID clord_id;
    ClOrdID orig_clord_id;
};

struct ReplaceAck {
    OrderId id;
    ClOrdID clord_id;
    ClOrdID orig_clord_id;
    Quantity new_qty;
    Price new_price;
};

using Event = std::variant<Trade, Ack, OrdReject, CancelAck, ReplaceAck>;
using EventList = std::vector<Event>;

}  // namespace obfix
