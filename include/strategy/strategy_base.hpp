#pragma once

#include "storage/storage_interface.hpp"
#include "strategy/execution_sink.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hft {

struct StrategyStats {
    uint64_t quote_events{0};
    uint64_t trade_events{0};
    uint64_t orders_submitted{0};
    uint64_t order_responses{0};
    uint64_t orders_filled{0};
    uint64_t orders_rejected{0};
    uint64_t storage_append_failures{0};
    double realized_pnl{0.0};
    bool active{false};
};

class StrategyBase {
protected:
    const uint32_t strategy_id_;
    const std::string name_;
    std::vector<std::string> tickers_;
    IExecutionSink* execution_sink_{nullptr};

    std::atomic<bool> active_{false};
    std::atomic<uint64_t> quote_events_{0};
    std::atomic<uint64_t> trade_events_{0};
    std::atomic<uint64_t> orders_submitted_{0};
    std::atomic<uint64_t> order_responses_{0};
    std::atomic<uint64_t> orders_filled_{0};
    std::atomic<uint64_t> orders_rejected_{0};
    std::atomic<uint64_t> storage_append_failures_{0};
    std::atomic<double> realized_pnl_{0.0};

public:
    StrategyBase(uint32_t id, std::string name, std::vector<std::string> tickers)
        : strategy_id_(id), name_(std::move(name)), tickers_(std::move(tickers)) {}

    virtual ~StrategyBase() = default;

    void attach_execution_sink(IExecutionSink* execution_sink) {
        execution_sink_ = execution_sink;
    }

    virtual void start() {
        if (!execution_sink_) {
            throw std::runtime_error("Execution sink not attached to strategy " + name_);
        }

        active_ = true;
        on_start();
    }

    virtual void stop() {
        active_ = false;
        on_stop();
    }

    virtual void on_quote(const std::string& ticker,
                          double bid,
                          double ask,
                          double bid_size,
                          double ask_size) = 0;

    virtual void on_trade(const std::string& ticker, double price, double size, bool is_buy) = 0;

    virtual void on_book_update(
        const std::string& ticker,
        const std::vector<std::pair<double, double>>& bids,
        const std::vector<std::pair<double, double>>& asks) {
        (void)ticker;
        (void)bids;
        (void)asks;
    }

    void on_quote_update(const QuoteUpdate& quote) {
        quote_events_.fetch_add(1, std::memory_order_relaxed);
        on_quote(quote.ticker, quote.bid_price, quote.ask_price, quote.bid_size, quote.ask_size);
    }

    void on_trade_event(const TradeEvent& trade) {
        trade_events_.fetch_add(1, std::memory_order_relaxed);
        on_trade(trade.ticker, trade.price, trade.quantity, !trade.is_buyer_maker);
    }

    void on_order_update(const OrderResponse& response) {
        handle_order_response(response);
    }

    bool tracks_ticker(std::string_view ticker) const {
        if (tickers_.empty()) {
            return true;
        }

        return std::any_of(tickers_.begin(), tickers_.end(), [ticker](const std::string& candidate) {
            return candidate == ticker;
        });
    }

    void record_storage_append_failure() {
        storage_append_failures_.fetch_add(1, std::memory_order_relaxed);
    }

protected:
    virtual void on_start() {}
    virtual void on_stop() {}

    bool place_limit_order(const char* ticker,
                           OrderSide side,
                           double quantity,
                           double price,
                           TimeInForce tif = TimeInForce::GTC,
                           ExecType exec = ExecType::DEFAULT) {
        if (!execution_sink_ || !active_.load(std::memory_order_relaxed)) {
            return false;
        }

        const Trade trade =
            Trade::create_limit_order(strategy_id_, ticker, side, quantity, price, tif, exec);
        return submit_trade(trade);
    }

    bool place_market_order(const char* ticker, OrderSide side, double quantity) {
        if (!execution_sink_ || !active_.load(std::memory_order_relaxed)) {
            return false;
        }

        const Trade trade = Trade::create_market_order(strategy_id_, ticker, side, quantity);
        return submit_trade(trade);
    }

    bool place_stop_order(const char* ticker,
                          OrderSide side,
                          double quantity,
                          double trigger_price,
                          std::optional<double> limit_price = std::nullopt) {
        if (!execution_sink_ || !active_.load(std::memory_order_relaxed)) {
            return false;
        }

        const Trade trade =
            Trade::create_stop_order(strategy_id_, ticker, side, quantity, trigger_price, limit_price);
        return submit_trade(trade);
    }

    void cancel_order(const std::string& instrument, uint64_t order_id, uint64_t client_oid = 0) {
        if (execution_sink_) {
            execution_sink_->cancel_order(instrument, order_id, client_oid);
        }
    }

    void cancel_all_orders(const std::string& instrument = "") {
        if (execution_sink_) {
            execution_sink_->cancel_all_orders(instrument);
        }
    }

    virtual void on_order_response(const OrderResponse& response) {
        (void)response;
    }

private:
    bool submit_trade(const Trade& trade) {
        const bool submitted = execution_sink_->submit_trade(
            trade,
            [this](const OrderResponse& response) { handle_order_response(response); },
            [this]() { record_storage_append_failure(); });

        if (submitted) {
            orders_submitted_.fetch_add(1, std::memory_order_relaxed);
        }

        return submitted;
    }

    void handle_order_response(const OrderResponse& response) {
        order_responses_.fetch_add(1, std::memory_order_relaxed);

        if (response.status == OrderStatus::FILLED) {
            orders_filled_.fetch_add(1, std::memory_order_relaxed);
        } else if (response.status == OrderStatus::REJECTED) {
            orders_rejected_.fetch_add(1, std::memory_order_relaxed);
        }

        on_order_response(response);
    }

public:
    StrategyStats stats() const {
        return StrategyStats{
            quote_events_.load(std::memory_order_relaxed),
            trade_events_.load(std::memory_order_relaxed),
            orders_submitted_.load(std::memory_order_relaxed),
            order_responses_.load(std::memory_order_relaxed),
            orders_filled_.load(std::memory_order_relaxed),
            orders_rejected_.load(std::memory_order_relaxed),
            storage_append_failures_.load(std::memory_order_relaxed),
            realized_pnl_.load(std::memory_order_relaxed),
            active_.load(std::memory_order_relaxed),
        };
    }

    uint32_t id() const { return strategy_id_; }
    const std::string& name() const { return name_; }
    const std::vector<std::string>& tickers() const { return tickers_; }
    bool is_active() const { return active_.load(std::memory_order_relaxed); }
    uint64_t quote_events() const { return quote_events_.load(std::memory_order_relaxed); }
    uint64_t trade_events() const { return trade_events_.load(std::memory_order_relaxed); }
    uint64_t orders_placed() const { return orders_submitted_.load(std::memory_order_relaxed); }
    uint64_t order_responses() const { return order_responses_.load(std::memory_order_relaxed); }
    uint64_t orders_filled() const { return orders_filled_.load(std::memory_order_relaxed); }
    uint64_t orders_rejected() const { return orders_rejected_.load(std::memory_order_relaxed); }
    uint64_t storage_append_failures() const {
        return storage_append_failures_.load(std::memory_order_relaxed);
    }
    double realized_pnl() const { return realized_pnl_.load(std::memory_order_relaxed); }
};

class StrategyManager {
    std::vector<std::unique_ptr<StrategyBase>> strategies_;
    IExecutionSink* execution_sink_{nullptr};
    std::shared_ptr<storage::IStorage> storage_;

public:
    explicit StrategyManager(IExecutionSink* execution_sink,
                             std::shared_ptr<storage::IStorage> storage = nullptr)
        : execution_sink_(execution_sink), storage_(std::move(storage)) {}

    template <typename T, typename... Args>
    T* add_strategy(Args&&... args) {
        auto strategy = std::make_unique<T>(std::forward<Args>(args)...);
        strategy->attach_execution_sink(execution_sink_);
        T* ptr = strategy.get();
        strategies_.push_back(std::move(strategy));
        return ptr;
    }

    void start_all() {
        for (auto& strategy : strategies_) {
            strategy->start();
        }
    }

    void stop_all() {
        for (auto& strategy : strategies_) {
            strategy->stop();
        }
    }

    void dispatch_quote(const QuoteUpdate& quote) { dispatch_quote_impl(quote, true); }
    void replay_quote(const QuoteUpdate& quote) { dispatch_quote_impl(quote, false); }

    void dispatch_trade(const TradeEvent& trade) { dispatch_trade_impl(trade, true); }
    void replay_trade(const TradeEvent& trade) { dispatch_trade_impl(trade, false); }

    void dispatch_order_response(uint32_t strategy_id, const OrderResponse& response) {
        if (StrategyBase* strategy = get_strategy(strategy_id)) {
            strategy->on_order_update(response);
        }
    }

    StrategyBase* get_strategy(uint32_t id) {
        for (auto& strategy : strategies_) {
            if (strategy->id() == id) {
                return strategy.get();
            }
        }

        return nullptr;
    }

    const StrategyBase* get_strategy(uint32_t id) const {
        for (const auto& strategy : strategies_) {
            if (strategy->id() == id) {
                return strategy.get();
            }
        }

        return nullptr;
    }

    size_t count() const { return strategies_.size(); }

private:
    template <typename Fn>
    void dispatch_to_matching_strategies(std::string_view ticker, bool persisted, Fn&& fn) {
        for (auto& strategy : strategies_) {
            if (!strategy->is_active() || !strategy->tracks_ticker(ticker)) {
                continue;
            }

            if (!persisted) {
                strategy->record_storage_append_failure();
            }

            fn(*strategy);
        }
    }

    bool maybe_append_storage_event(storage::StorageEvent event, bool persist) {
        if (!persist || !storage_) {
            return true;
        }

        return storage_->append(std::move(event));
    }

    void dispatch_quote_impl(const QuoteUpdate& quote, bool persist) {
        const bool persisted = maybe_append_storage_event(
            storage::StorageEvent{
                0, storage::now_ns(), storage::StorageEventType::QuoteUpdate, quote},
            persist);

        dispatch_to_matching_strategies(quote.ticker, persisted, [&quote](StrategyBase& strategy) {
            strategy.on_quote_update(quote);
        });
    }

    void dispatch_trade_impl(const TradeEvent& trade, bool persist) {
        const bool persisted = maybe_append_storage_event(
            storage::StorageEvent{
                0, storage::now_ns(), storage::StorageEventType::MarketTrade, trade},
            persist);

        dispatch_to_matching_strategies(trade.ticker, persisted, [&trade](StrategyBase& strategy) {
            strategy.on_trade_event(trade);
        });
    }
};

}  // namespace hft
