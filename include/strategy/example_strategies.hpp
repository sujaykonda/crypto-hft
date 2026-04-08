#pragma once

#include "strategy/strategy_base.hpp"

#include <chrono>
#include <iostream>
#include <string>

namespace hft {

class SimpleMarketMaker : public StrategyBase {
    double spread_bps_;
    double order_size_;
    double mid_price_{0.0};

    std::chrono::steady_clock::time_point last_update_;
    static constexpr int kMinUpdateIntervalMs = 100;

public:
    SimpleMarketMaker(uint32_t id,
                      const std::string& ticker,
                      double spread_bps,
                      double order_size)
        : StrategyBase(id, "MarketMaker_" + ticker, {ticker}),
          spread_bps_(spread_bps),
          order_size_(order_size),
          last_update_(std::chrono::steady_clock::now()) {}

    void on_quote(const std::string& ticker,
                  double bid,
                  double ask,
                  double bid_size,
                  double ask_size) override {
        (void)bid_size;
        (void)ask_size;

        if (!is_active()) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update_).count();
        if (elapsed < kMinUpdateIntervalMs) {
            return;
        }

        last_update_ = now;
        mid_price_ = (bid + ask) / 2.0;
        const double half_spread = mid_price_ * (spread_bps_ / 10000.0) / 2.0;

        const double my_bid = mid_price_ - half_spread;
        const double my_ask = mid_price_ + half_spread;

        place_limit_order(
            ticker.c_str(), OrderSide::BUY, order_size_, my_bid, TimeInForce::GTC, ExecType::POST_ONLY);
        place_limit_order(
            ticker.c_str(), OrderSide::SELL, order_size_, my_ask, TimeInForce::GTC, ExecType::POST_ONLY);
    }

    void on_trade(const std::string& ticker, double price, double size, bool is_buy) override {
        (void)ticker;
        (void)price;
        (void)size;
        (void)is_buy;
    }

protected:
    void on_start() override {
        std::cout << "[" << name_ << "] Started with spread=" << spread_bps_
                  << "bps, size=" << order_size_ << std::endl;
    }

    void on_stop() override {
        if (!tickers_.empty()) {
            cancel_all_orders(tickers_[0]);
        }

        std::cout << "[" << name_ << "] Stopped" << std::endl;
    }

    void on_order_response(const OrderResponse& response) override {
        if (response.status == OrderStatus::REJECTED) {
            std::cerr << "[" << name_ << "] Order rejected: " << response.error_message
                      << std::endl;
        } else if (response.status == OrderStatus::FILLED) {
            std::cout << "[" << name_ << "] Order filled: qty=" << response.filled_quantity
                      << " @ " << response.avg_price << std::endl;
        }
    }
};

class SimpleMomentum : public StrategyBase {
    double threshold_pct_;
    double order_size_;
    double last_price_{0.0};
    double reference_price_{0.0};
    int trade_count_{0};

public:
    SimpleMomentum(uint32_t id,
                   const std::string& ticker,
                   double threshold_pct,
                   double order_size)
        : StrategyBase(id, "Momentum_" + ticker, {ticker}),
          threshold_pct_(threshold_pct),
          order_size_(order_size) {}

    void on_quote(const std::string& ticker,
                  double bid,
                  double ask,
                  double bid_size,
                  double ask_size) override {
        (void)ticker;
        (void)bid;
        (void)ask;
        (void)bid_size;
        (void)ask_size;
    }

    void on_trade(const std::string& ticker, double price, double size, bool is_buy) override {
        (void)size;
        (void)is_buy;

        if (!is_active()) {
            return;
        }

        if (reference_price_ == 0.0) {
            reference_price_ = price;
            last_price_ = price;
            return;
        }

        const double pct_change = (price - reference_price_) / reference_price_ * 100.0;
        if (pct_change > threshold_pct_) {
            place_market_order(ticker.c_str(), OrderSide::BUY, order_size_);
            reference_price_ = price;
            ++trade_count_;
        } else if (pct_change < -threshold_pct_) {
            place_market_order(ticker.c_str(), OrderSide::SELL, order_size_);
            reference_price_ = price;
            ++trade_count_;
        }

        last_price_ = price;
    }

protected:
    void on_start() override {
        std::cout << "[" << name_ << "] Started with threshold=" << threshold_pct_ << "%"
                  << std::endl;
    }

    void on_stop() override {
        std::cout << "[" << name_ << "] Stopped. Total trades: " << trade_count_ << std::endl;
    }
};

}  // namespace hft
