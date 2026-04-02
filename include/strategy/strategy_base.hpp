#pragma once

#include "core/order_executor.hpp"
#include <vector>
#include <string>
#include <memory>

namespace hft {

// ═══════════════════════════════════════════════════════════════════════════
// Strategy Base Class - All strategies inherit from this
// ═══════════════════════════════════════════════════════════════════════════

class StrategyBase {
protected:
    const uint32_t strategy_id_;
    const std::string name_;
    std::vector<std::string> tickers_;
    OrderExecutor* executor_;
    
    std::atomic<bool> active_{false};
    std::atomic<uint64_t> orders_placed_{0};
    std::atomic<uint64_t> orders_filled_{0};
    std::atomic<double> realized_pnl_{0.0};

public:
    StrategyBase(uint32_t id, const std::string& name, 
                 const std::vector<std::string>& tickers)
        : strategy_id_(id)
        , name_(name)
        , tickers_(tickers)
        , executor_(nullptr) {}

    virtual ~StrategyBase() = default;

    // ───────────────────────────────────────────────────────────────────
    // Lifecycle
    // ───────────────────────────────────────────────────────────────────
    void attach_executor(OrderExecutor* executor) {
        executor_ = executor;
    }

    virtual void start() {
        if (!executor_) {
            throw std::runtime_error("Executor not attached to strategy " + name_);
        }
        active_ = true;
        on_start();
    }

    virtual void stop() {
        active_ = false;
        on_stop();
    }

    // ───────────────────────────────────────────────────────────────────
    // Market data callbacks - override these in your strategy
    // ───────────────────────────────────────────────────────────────────
    virtual void on_quote(const std::string& ticker, 
                         double bid, double ask,
                         double bid_size, double ask_size) = 0;

    virtual void on_trade(const std::string& ticker,
                         double price, double size,
                         bool is_buy) = 0;

    virtual void on_book_update(const std::string& ticker,
                               const std::vector<std::pair<double, double>>& bids,
                               const std::vector<std::pair<double, double>>& asks) {
        // Optional override
    }

    // ───────────────────────────────────────────────────────────────────
    // Quote update using struct
    // ───────────────────────────────────────────────────────────────────
    void on_quote_update(const QuoteUpdate& quote) {
        on_quote(quote.ticker, quote.bid_price, quote.ask_price,
                 quote.bid_size, quote.ask_size);
    }

    // ───────────────────────────────────────────────────────────────────
    // Trade event using struct
    // ───────────────────────────────────────────────────────────────────
    void on_trade_event(const TradeEvent& trade) {
        on_trade(trade.ticker, trade.price, trade.quantity, !trade.is_buyer_maker);
    }

protected:
    virtual void on_start() {}
    virtual void on_stop() {}

    // ───────────────────────────────────────────────────────────────────
    // Order submission helpers
    // ───────────────────────────────────────────────────────────────────
    bool place_limit_order(const char* ticker, OrderSide side, 
                          double quantity, double price,
                          TimeInForce tif = TimeInForce::GTC,
                          ExecType exec = ExecType::DEFAULT) {
        if (!executor_ || !active_) return false;
        
        Trade trade = Trade::create_limit_order(
            strategy_id_, ticker, side, quantity, price, tif, exec
        );
        
        bool submitted = executor_->submit_trade(trade, 
            [this](const OrderResponse& resp) {
                on_order_response(resp);
            }
        );
        
        if (submitted) orders_placed_.fetch_add(1);
        return submitted;
    }

    bool place_market_order(const char* ticker, OrderSide side, double quantity) {
        if (!executor_ || !active_) return false;
        
        Trade trade = Trade::create_market_order(strategy_id_, ticker, side, quantity);
        
        bool submitted = executor_->submit_trade(trade,
            [this](const OrderResponse& resp) {
                on_order_response(resp);
            }
        );
        
        if (submitted) orders_placed_.fetch_add(1);
        return submitted;
    }

    bool place_stop_order(const char* ticker, OrderSide side,
                         double quantity, double trigger_price,
                         std::optional<double> limit_price = std::nullopt) {
        if (!executor_ || !active_) return false;
        
        Trade trade = Trade::create_stop_order(
            strategy_id_, ticker, side, quantity, trigger_price, limit_price
        );
        
        bool submitted = executor_->submit_trade(trade,
            [this](const OrderResponse& resp) {
                on_order_response(resp);
            }
        );
        
        if (submitted) orders_placed_.fetch_add(1);
        return submitted;
    }

    // ───────────────────────────────────────────────────────────────────
    // Cancel orders
    // ───────────────────────────────────────────────────────────────────
    void cancel_order(const std::string& instrument, uint64_t order_id,
                     uint64_t client_oid = 0) {
        if (executor_) {
            executor_->cancel_order(instrument, order_id, client_oid);
        }
    }

    void cancel_all_orders(const std::string& instrument = "") {
        if (executor_) {
            executor_->cancel_all_orders(instrument);
        }
    }

    // ───────────────────────────────────────────────────────────────────
    // Order response callback - override for custom handling
    // ───────────────────────────────────────────────────────────────────
    virtual void on_order_response(const OrderResponse& response) {
        if (response.status == OrderStatus::FILLED) {
            orders_filled_.fetch_add(1);
        }
    }

public:
    // ───────────────────────────────────────────────────────────────────
    // Accessors
    // ───────────────────────────────────────────────────────────────────
    uint32_t id() const { return strategy_id_; }
    const std::string& name() const { return name_; }
    const std::vector<std::string>& tickers() const { return tickers_; }
    bool is_active() const { return active_; }
    uint64_t orders_placed() const { return orders_placed_; }
    uint64_t orders_filled() const { return orders_filled_; }
    double realized_pnl() const { return realized_pnl_.load(); }
};

// ═══════════════════════════════════════════════════════════════════════════
// Strategy Manager - Manages multiple strategies
// ═══════════════════════════════════════════════════════════════════════════

class StrategyManager {
    std::vector<std::unique_ptr<StrategyBase>> strategies_;
    OrderExecutor* executor_;

public:
    explicit StrategyManager(OrderExecutor* executor) : executor_(executor) {}

    template<typename T, typename... Args>
    T* add_strategy(Args&&... args) {
        auto strategy = std::make_unique<T>(std::forward<Args>(args)...);
        strategy->attach_executor(executor_);
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

    // Dispatch market data to all strategies
    void dispatch_quote(const QuoteUpdate& quote) {
        for (auto& strategy : strategies_) {
            if (strategy->is_active()) {
                strategy->on_quote_update(quote);
            }
        }
    }

    void dispatch_trade(const TradeEvent& trade) {
        for (auto& strategy : strategies_) {
            if (strategy->is_active()) {
                strategy->on_trade_event(trade);
            }
        }
    }

    // Get strategy by ID
    StrategyBase* get_strategy(uint32_t id) {
        for (auto& strategy : strategies_) {
            if (strategy->id() == id) {
                return strategy.get();
            }
        }
        return nullptr;
    }

    size_t count() const { return strategies_.size(); }
};

} // namespace hft
