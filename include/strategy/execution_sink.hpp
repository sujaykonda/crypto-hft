#pragma once

#include "types/trade.hpp"

#include <functional>
#include <string>

namespace hft {

using OrderCallback = std::function<void(const OrderResponse&)>;
using StorageFailureCallback = std::function<void()>;

class IExecutionSink {
public:
    virtual ~IExecutionSink() = default;

    virtual bool submit_trade(const Trade& trade,
                              OrderCallback response_callback,
                              StorageFailureCallback storage_failure_callback) = 0;

    virtual void cancel_order(const std::string& instrument,
                              uint64_t order_id,
                              uint64_t client_oid = 0) = 0;

    virtual void cancel_all_orders(const std::string& instrument = "") = 0;
};

}  // namespace hft
