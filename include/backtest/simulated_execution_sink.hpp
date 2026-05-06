#pragma once

#include "storage/storage_interface.hpp"
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
#include <vector>

namespace hft::backtest {

struct SimulatedExecutionConfig {
    double maker_fee_bps{0.0};
    double taker_fee_bps{0.0};
    bool reject_without_market{true};
};

struct SimulatedExecutionStats {
    uint64_t submitted{0};
    uint64_t resting{0};
    uint64_t filled{0};
    uint64_t canceled{0};
    uint64_t rejected{0};
    uint64_t storage_append_failures{0};
    double gross_notional{0.0};
    double fees{0.0};
};

class SimulatedExecutionSink final : public IExecutionSink {
public:
    explicit SimulatedExecutionSink(std::shared_ptr<storage::IStorage> storage = nullptr,
                                    SimulatedExecutionConfig config = {})
        : storage_(std::move(storage)), config_(config) {}

    bool submit_trade(const Trade& trade,
                      OrderCallback response_callback,
                      StorageFailureCallback storage_failure_callback) override {
        submitted_.fetch_add(1, std::memory_order_relaxed);
        append_storage_event(storage::StorageEvent{
                                 0,
                                 storage::now_ns(),
                                 storage::StorageEventType::TradeSubmitted,
                                 trade},
                             storage_failure_callback);

        std::optional<OrderResponse> response;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            const auto rejection = validate_order_locked(trade);
            if (rejection.has_value()) {
                response = make_response_locked(trade, OrderStatus::REJECTED, 0.0, 0, rejection->c_str());
            } else if (const auto fill = immediate_fill_locked(trade)) {
                response = make_response_locked(trade, OrderStatus::FILLED, fill->price, fill->timestamp_ns, "");
            } else {
                PendingOrder pending;
                pending.trade = trade;
                pending.exchange_order_id = next_exchange_order_id_++;
                pending.response_callback = std::move(response_callback);
                pending.storage_failure_callback = std::move(storage_failure_callback);
                resting_orders_.push_back(std::move(pending));
                return true;
            }
        }

        publish_response(*response, response_callback, storage_failure_callback);
        return true;
    }

    void cancel_order(const std::string& instrument,
                      uint64_t order_id,
                      uint64_t client_oid = 0) override {
        std::vector<ResponseDelivery> deliveries;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = resting_orders_.begin();
            while (it != resting_orders_.end()) {
                const bool instrument_matches = instrument.empty() || instrument == it->trade.ticker;
                const bool id_matches =
                    (client_oid != 0 && it->trade.client_order_id == client_oid) ||
                    (order_id != 0 && it->exchange_order_id == order_id);
                if (!instrument_matches || !id_matches) {
                    ++it;
                    continue;
                }

                OrderResponse response =
                    make_response_locked(it->trade, OrderStatus::CANCELED, 0.0, storage::now_ns(), "");
                response.exchange_order_id = it->exchange_order_id;
                deliveries.push_back(ResponseDelivery{std::move(*it), response});
                it = resting_orders_.erase(it);
                break;
            }
        }

        deliver_all(deliveries);
    }

    void cancel_all_orders(const std::string& instrument = "") override {
        std::vector<ResponseDelivery> deliveries;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = resting_orders_.begin();
            while (it != resting_orders_.end()) {
                if (!instrument.empty() && instrument != it->trade.ticker) {
                    ++it;
                    continue;
                }

                OrderResponse response =
                    make_response_locked(it->trade, OrderStatus::CANCELED, 0.0, storage::now_ns(), "");
                response.exchange_order_id = it->exchange_order_id;
                deliveries.push_back(ResponseDelivery{std::move(*it), response});
                it = resting_orders_.erase(it);
            }
        }

        deliver_all(deliveries);
    }

    void on_quote(const QuoteUpdate& quote) {
        std::vector<ResponseDelivery> deliveries;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quotes_[quote.ticker] = quote;
            fill_resting_orders_locked(quote, deliveries);
        }

        deliver_all(deliveries);
    }

    void on_trade(const TradeEvent& trade_event) {
        std::vector<ResponseDelivery> deliveries;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = resting_orders_.begin();
            while (it != resting_orders_.end()) {
                if (trade_event.ticker != std::string(it->trade.ticker) ||
                    it->trade.type != OrderType::LIMIT ||
                    !trade_crosses_limit(trade_event, it->trade)) {
                    ++it;
                    continue;
                }

                OrderResponse response = make_response_locked(
                    it->trade, OrderStatus::FILLED, trade_event.price, trade_event.timestamp_ns, "");
                response.exchange_order_id = it->exchange_order_id;
                deliveries.push_back(ResponseDelivery{std::move(*it), response});
                it = resting_orders_.erase(it);
            }
        }

        deliver_all(deliveries);
    }

    SimulatedExecutionStats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return SimulatedExecutionStats{
            submitted_.load(std::memory_order_relaxed),
            static_cast<uint64_t>(resting_orders_.size()),
            filled_.load(std::memory_order_relaxed),
            canceled_.load(std::memory_order_relaxed),
            rejected_.load(std::memory_order_relaxed),
            storage_append_failures_.load(std::memory_order_relaxed),
            gross_notional_,
            fees_,
        };
    }

private:
    struct Fill {
        double price{0.0};
        int64_t timestamp_ns{0};
    };

    struct PendingOrder {
        Trade trade{};
        uint64_t exchange_order_id{0};
        OrderCallback response_callback;
        StorageFailureCallback storage_failure_callback;
    };

    struct ResponseDelivery {
        PendingOrder pending;
        OrderResponse response;
    };

    std::optional<std::string> validate_order_locked(const Trade& trade) const {
        if (trade.ticker[0] == '\0') {
            return "missing ticker";
        }
        if (trade.quantity <= 0.0) {
            return "quantity must be positive";
        }
        if (trade.type == OrderType::LIMIT && trade.price <= 0.0) {
            return "limit price must be positive";
        }
        if (trade.type != OrderType::MARKET && trade.type != OrderType::LIMIT) {
            return "simulator supports market and limit orders";
        }
        if (trade.type == OrderType::MARKET && config_.reject_without_market &&
            quotes_.find(trade.ticker) == quotes_.end()) {
            return "no quote available for market order";
        }
        return std::nullopt;
    }

    std::optional<Fill> immediate_fill_locked(const Trade& trade) const {
        const auto quote_it = quotes_.find(trade.ticker);
        if (quote_it == quotes_.end()) {
            return std::nullopt;
        }

        const QuoteUpdate& quote = quote_it->second;
        if (trade.side == OrderSide::BUY) {
            if (trade.type == OrderType::MARKET || trade.price >= quote.ask_price) {
                return Fill{quote.ask_price, quote.timestamp_ns};
            }
        } else if (trade.type == OrderType::MARKET || trade.price <= quote.bid_price) {
            return Fill{quote.bid_price, quote.timestamp_ns};
        }

        return std::nullopt;
    }

    void fill_resting_orders_locked(const QuoteUpdate& quote, std::vector<ResponseDelivery>& deliveries) {
        auto it = resting_orders_.begin();
        while (it != resting_orders_.end()) {
            if (quote.ticker != std::string(it->trade.ticker) || it->trade.type != OrderType::LIMIT) {
                ++it;
                continue;
            }

            const bool crossed = it->trade.side == OrderSide::BUY ? it->trade.price >= quote.ask_price
                                                                  : it->trade.price <= quote.bid_price;
            if (!crossed) {
                ++it;
                continue;
            }

            const double fill_price = it->trade.side == OrderSide::BUY ? quote.ask_price : quote.bid_price;
            OrderResponse response =
                make_response_locked(it->trade, OrderStatus::FILLED, fill_price, quote.timestamp_ns, "");
            response.exchange_order_id = it->exchange_order_id;
            deliveries.push_back(ResponseDelivery{std::move(*it), response});
            it = resting_orders_.erase(it);
        }
    }

    static bool trade_crosses_limit(const TradeEvent& trade_event, const Trade& order) {
        return order.side == OrderSide::BUY ? trade_event.price <= order.price
                                            : trade_event.price >= order.price;
    }

    OrderResponse make_response_locked(const Trade& trade,
                                       OrderStatus status,
                                       double fill_price,
                                       int64_t timestamp_ns,
                                       const char* error_message) {
        OrderResponse response{};
        response.client_order_id = trade.client_order_id;
        response.exchange_order_id = next_exchange_order_id_++;
        response.status = status;
        response.filled_quantity = status == OrderStatus::FILLED ? trade.quantity : 0.0;
        response.remaining_quantity = status == OrderStatus::FILLED ? 0.0 : trade.quantity;
        response.avg_price = fill_price;
        response.exchange_timestamp_ns = timestamp_ns > 0 ? timestamp_ns : storage::now_ns();

        if (status == OrderStatus::FILLED) {
            const double notional = fill_price * trade.quantity;
            const double fee_rate = trade.type == OrderType::MARKET ? config_.taker_fee_bps : config_.maker_fee_bps;
            response.fee = notional * fee_rate / 10000.0;
            std::strncpy(response.fee_currency, "QUOTE", sizeof(response.fee_currency) - 1);
            gross_notional_ += notional;
            fees_ += response.fee;
        } else if (status == OrderStatus::REJECTED) {
            response.error_code = -1;
            std::strncpy(response.error_message, error_message, sizeof(response.error_message) - 1);
        }

        return response;
    }

    void deliver_all(const std::vector<ResponseDelivery>& deliveries) {
        for (const auto& delivery : deliveries) {
            publish_response(delivery.response,
                             delivery.pending.response_callback,
                             delivery.pending.storage_failure_callback);
        }
    }

    void publish_response(const OrderResponse& response,
                          const OrderCallback& response_callback,
                          const StorageFailureCallback& storage_failure_callback) {
        if (response.status == OrderStatus::FILLED) {
            filled_.fetch_add(1, std::memory_order_relaxed);
        } else if (response.status == OrderStatus::CANCELED) {
            canceled_.fetch_add(1, std::memory_order_relaxed);
        } else if (response.status == OrderStatus::REJECTED) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
        }

        append_storage_event(storage::StorageEvent{
                                 0,
                                 storage::now_ns(),
                                 storage::StorageEventType::OrderResponse,
                                 response},
                             storage_failure_callback);

        if (response_callback) {
            response_callback(response);
        }
    }

    bool append_storage_event(storage::StorageEvent event,
                              const StorageFailureCallback& storage_failure_callback) {
        if (!storage_) {
            return true;
        }

        const bool appended = storage_->append(std::move(event));
        if (!appended) {
            storage_append_failures_.fetch_add(1, std::memory_order_relaxed);
            if (storage_failure_callback) {
                storage_failure_callback();
            }
        }
        return appended;
    }

    std::shared_ptr<storage::IStorage> storage_;
    SimulatedExecutionConfig config_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, QuoteUpdate> quotes_;
    std::vector<PendingOrder> resting_orders_;
    uint64_t next_exchange_order_id_{1};
    double gross_notional_{0.0};
    double fees_{0.0};

    std::atomic<uint64_t> submitted_{0};
    std::atomic<uint64_t> filled_{0};
    std::atomic<uint64_t> canceled_{0};
    std::atomic<uint64_t> rejected_{0};
    std::atomic<uint64_t> storage_append_failures_{0};
};

}  // namespace hft::backtest
