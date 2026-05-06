#pragma once

#include "strategy/execution_sink.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace hft::risk {

struct RiskLimits {
    double max_order_quantity{0.0};
    double max_order_notional{0.0};
    double max_net_position_abs{0.0};
    bool allow_market_orders{true};
};

struct Position {
    double net_quantity{0.0};
    double avg_entry_price{0.0};
    double realized_pnl{0.0};
};

struct RiskDecision {
    bool accepted{true};
    std::string reason;
};

class RiskManager {
public:
    explicit RiskManager(RiskLimits limits = {}) : limits_(limits) {}

    void set_limits(RiskLimits limits) {
        std::lock_guard lock(mutex_);
        limits_ = limits;
    }

    void update_quote(const QuoteUpdate& quote) {
        std::lock_guard lock(mutex_);
        mid_prices_[quote.ticker] = (quote.bid_price + quote.ask_price) / 2.0;
    }

    RiskDecision check_order(const Trade& trade) const {
        std::lock_guard lock(mutex_);
        return check_order_locked(trade);
    }

    void on_fill(const Trade& trade, const OrderResponse& response) {
        if (response.status != OrderStatus::FILLED && response.status != OrderStatus::PARTIALLY_FILLED) {
            return;
        }

        const double fill_qty = response.filled_quantity;
        if (fill_qty <= 0.0) {
            return;
        }

        std::lock_guard lock(mutex_);
        Position& position = positions_[trade.ticker];
        const double signed_qty = trade.side == OrderSide::BUY ? fill_qty : -fill_qty;
        const double fill_price = response.avg_price > 0.0 ? response.avg_price : trade.price;

        if (position.net_quantity == 0.0 ||
            (position.net_quantity > 0.0 && signed_qty > 0.0) ||
            (position.net_quantity < 0.0 && signed_qty < 0.0)) {
            const double old_abs = std::abs(position.net_quantity);
            const double new_abs = old_abs + std::abs(signed_qty);
            position.avg_entry_price =
                new_abs > 0.0
                    ? ((position.avg_entry_price * old_abs) + (fill_price * std::abs(signed_qty))) / new_abs
                    : 0.0;
            position.net_quantity += signed_qty;
            return;
        }

        const double old_net = position.net_quantity;
        const double closing_qty = std::min(std::abs(old_net), std::abs(signed_qty));
        const double pnl_sign = old_net > 0.0 ? 1.0 : -1.0;
        position.realized_pnl += (fill_price - position.avg_entry_price) * closing_qty * pnl_sign;
        position.net_quantity += signed_qty;

        if (position.net_quantity == 0.0) {
            position.avg_entry_price = 0.0;
        } else if ((old_net > 0.0 && position.net_quantity < 0.0) ||
                   (old_net < 0.0 && position.net_quantity > 0.0)) {
            position.avg_entry_price = fill_price;
        }
    }

    Position position(const std::string& ticker) const {
        std::lock_guard lock(mutex_);
        const auto it = positions_.find(ticker);
        return it == positions_.end() ? Position{} : it->second;
    }

private:
    RiskDecision check_order_locked(const Trade& trade) const {
        if (trade.quantity <= 0.0) {
            return reject("quantity must be positive");
        }
        if (!limits_.allow_market_orders && trade.type == OrderType::MARKET) {
            return reject("market orders disabled");
        }
        if (limits_.max_order_quantity > 0.0 && trade.quantity > limits_.max_order_quantity) {
            return reject("order quantity exceeds limit");
        }

        const double risk_price = order_risk_price_locked(trade);
        if (limits_.max_order_notional > 0.0 && risk_price * trade.quantity > limits_.max_order_notional) {
            return reject("order notional exceeds limit");
        }

        if (limits_.max_net_position_abs > 0.0) {
            const auto it = positions_.find(trade.ticker);
            const double current = it == positions_.end() ? 0.0 : it->second.net_quantity;
            const double signed_qty = trade.side == OrderSide::BUY ? trade.quantity : -trade.quantity;
            if (std::abs(current + signed_qty) > limits_.max_net_position_abs) {
                return reject("net position exceeds limit");
            }
        }

        return {};
    }

    double order_risk_price_locked(const Trade& trade) const {
        if (trade.price > 0.0) {
            return trade.price;
        }

        const auto it = mid_prices_.find(trade.ticker);
        return it == mid_prices_.end() ? 0.0 : it->second;
    }

    static RiskDecision reject(std::string reason) {
        return RiskDecision{false, std::move(reason)};
    }

    mutable std::mutex mutex_;
    RiskLimits limits_;
    std::unordered_map<std::string, Position> positions_;
    std::unordered_map<std::string, double> mid_prices_;
};

class RiskCheckedExecutionSink final : public IExecutionSink {
public:
    RiskCheckedExecutionSink(IExecutionSink& downstream, RiskManager& risk_manager)
        : downstream_(downstream), risk_manager_(risk_manager) {}

    bool submit_trade(const Trade& trade,
                      OrderCallback response_callback,
                      StorageFailureCallback storage_failure_callback) override {
        const RiskDecision decision = risk_manager_.check_order(trade);
        if (!decision.accepted) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            OrderResponse response;
            response.client_order_id = trade.client_order_id;
            response.status = OrderStatus::REJECTED;
            response.error_code = -100;
            std::strncpy(response.error_message, decision.reason.c_str(),
                         sizeof(response.error_message) - 1);
            if (response_callback) {
                response_callback(response);
            }
            return false;
        }

        {
            std::lock_guard lock(mutex_);
            submitted_trades_[trade.client_order_id] = trade;
        }

        return downstream_.submit_trade(
            trade,
            [this, response_callback = std::move(response_callback)](const OrderResponse& response) mutable {
                Trade trade{};
                bool found = false;
                {
                    std::lock_guard lock(mutex_);
                    const auto it = submitted_trades_.find(response.client_order_id);
                    if (it != submitted_trades_.end()) {
                        trade = it->second;
                        found = true;
                        if (response.status == OrderStatus::FILLED ||
                            response.status == OrderStatus::CANCELED ||
                            response.status == OrderStatus::REJECTED ||
                            response.status == OrderStatus::EXPIRED) {
                            submitted_trades_.erase(it);
                        }
                    }
                }

                if (found) {
                    risk_manager_.on_fill(trade, response);
                }
                if (response_callback) {
                    response_callback(response);
                }
            },
            std::move(storage_failure_callback));
    }

    void cancel_order(const std::string& instrument,
                      uint64_t order_id,
                      uint64_t client_oid = 0) override {
        downstream_.cancel_order(instrument, order_id, client_oid);
    }

    void cancel_all_orders(const std::string& instrument = "") override {
        downstream_.cancel_all_orders(instrument);
    }

    void update_quote(const QuoteUpdate& quote) {
        risk_manager_.update_quote(quote);
    }

    uint64_t rejected_count() const {
        return rejected_.load(std::memory_order_relaxed);
    }

private:
    IExecutionSink& downstream_;
    RiskManager& risk_manager_;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, Trade> submitted_trades_;
    std::atomic<uint64_t> rejected_{0};
};

}  // namespace hft::risk
