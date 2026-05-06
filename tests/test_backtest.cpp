#include "backtest/backtest_replay_driver.hpp"
#include "storage/in_memory_storage.hpp"
#include "strategy/strategy_base.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using hft::OrderResponse;
using hft::OrderSide;
using hft::OrderStatus;
using hft::QuoteUpdate;
using hft::StrategyBase;
using hft::StrategyManager;
using hft::TradeEvent;
using hft::backtest::BacktestReplayDriver;
using hft::backtest::SimulatedExecutionConfig;
using hft::backtest::SimulatedExecutionSink;
using hft::storage::InMemoryStorage;
using hft::storage::ReplayRequest;
using hft::storage::StorageEvent;
using hft::storage::StorageEventType;

template <size_t N>
void copy_string(const std::string& source, char (&destination)[N]) {
    std::strncpy(destination, source.c_str(), N - 1);
    destination[N - 1] = '\0';
}

QuoteUpdate make_quote(const std::string& ticker,
                       double bid,
                       double ask,
                       int64_t timestamp_ns) {
    QuoteUpdate quote{};
    copy_string(ticker, quote.ticker);
    quote.bid_price = bid;
    quote.bid_size = 2.0;
    quote.ask_price = ask;
    quote.ask_size = 3.0;
    quote.timestamp_ns = timestamp_ns;
    return quote;
}

TradeEvent make_trade_event(const std::string& ticker,
                            double price,
                            double quantity,
                            int64_t timestamp_ns) {
    TradeEvent trade{};
    copy_string(ticker, trade.ticker);
    trade.price = price;
    trade.quantity = quantity;
    trade.is_buyer_maker = false;
    trade.timestamp_ns = timestamp_ns;
    trade.trade_id = static_cast<uint64_t>(timestamp_ns);
    return trade;
}

class BuyMarketOnFirstQuote final : public StrategyBase {
public:
    BuyMarketOnFirstQuote(uint32_t id, std::string ticker, double quantity)
        : StrategyBase(id, "BuyMarketOnFirstQuote", {ticker}),
          ticker_(std::move(ticker)),
          quantity_(quantity) {}

    void on_quote(const std::string& ticker,
                  double bid,
                  double ask,
                  double bid_size,
                  double ask_size) override {
        (void)bid;
        (void)ask;
        (void)bid_size;
        (void)ask_size;
        if (!sent_ && ticker == ticker_) {
            sent_ = place_market_order(ticker.c_str(), OrderSide::BUY, quantity_);
        }
    }

    void on_trade(const std::string& ticker, double price, double size, bool is_buy) override {
        (void)ticker;
        (void)price;
        (void)size;
        (void)is_buy;
    }

protected:
    void on_order_response(const OrderResponse& response) override {
        responses.push_back(response);
    }

public:
    std::vector<OrderResponse> responses;

private:
    std::string ticker_;
    double quantity_;
    bool sent_{false};
};

class BuyLimitOnFirstQuote final : public StrategyBase {
public:
    BuyLimitOnFirstQuote(uint32_t id, std::string ticker, double quantity, double limit_price)
        : StrategyBase(id, "BuyLimitOnFirstQuote", {ticker}),
          ticker_(std::move(ticker)),
          quantity_(quantity),
          limit_price_(limit_price) {}

    void on_quote(const std::string& ticker,
                  double bid,
                  double ask,
                  double bid_size,
                  double ask_size) override {
        (void)bid;
        (void)ask;
        (void)bid_size;
        (void)ask_size;
        if (!sent_ && ticker == ticker_) {
            sent_ = place_limit_order(ticker.c_str(), OrderSide::BUY, quantity_, limit_price_);
        }
    }

    void on_trade(const std::string& ticker, double price, double size, bool is_buy) override {
        (void)ticker;
        (void)price;
        (void)size;
        (void)is_buy;
    }

protected:
    void on_order_response(const OrderResponse& response) override {
        responses.push_back(response);
    }

public:
    std::vector<OrderResponse> responses;

private:
    std::string ticker_;
    double quantity_;
    double limit_price_;
    bool sent_{false};
};

TEST(BacktestReplay, MarketOrderFillsAgainstCurrentQuoteAndPersistsExecutionEvents) {
    InMemoryStorage historical;
    ASSERT_TRUE(historical.append(StorageEvent{
        0, 100, StorageEventType::QuoteUpdate, make_quote("BTC_USDT", 99.0, 100.0, 100)}));

    auto execution_storage = std::make_shared<InMemoryStorage>();
    SimulatedExecutionConfig config;
    config.taker_fee_bps = 10.0;
    SimulatedExecutionSink sink(execution_storage, config);
    StrategyManager manager(&sink);
    auto* strategy = manager.add_strategy<BuyMarketOnFirstQuote>(1, "BTC_USDT", 0.5);
    manager.start_all();

    ReplayRequest request;
    request.max_events = 1;
    const auto replay_stats = BacktestReplayDriver(historical, manager, sink).run(request);

    EXPECT_EQ(replay_stats.events_processed, 1u);
    EXPECT_EQ(replay_stats.quote_events, 1u);
    ASSERT_EQ(strategy->responses.size(), 1u);
    EXPECT_EQ(strategy->responses[0].status, OrderStatus::FILLED);
    EXPECT_DOUBLE_EQ(strategy->responses[0].avg_price, 100.0);
    EXPECT_DOUBLE_EQ(strategy->responses[0].filled_quantity, 0.5);
    EXPECT_EQ(strategy->stats().orders_submitted, 1u);
    EXPECT_EQ(strategy->stats().orders_filled, 1u);

    const auto execution_stats = sink.stats();
    EXPECT_EQ(execution_stats.submitted, 1u);
    EXPECT_EQ(execution_stats.filled, 1u);
    EXPECT_DOUBLE_EQ(execution_stats.gross_notional, 50.0);
    EXPECT_DOUBLE_EQ(execution_stats.fees, 0.05);

    ReplayRequest execution_request;
    execution_request.max_events = 0;
    const auto execution_batch = execution_storage->replay(execution_request);
    ASSERT_EQ(execution_batch.events.size(), 2u);
    EXPECT_EQ(execution_batch.events[0].type, StorageEventType::TradeSubmitted);
    EXPECT_EQ(execution_batch.events[1].type, StorageEventType::OrderResponse);
}

TEST(BacktestReplay, RestingLimitOrderFillsOnLaterMarketTrade) {
    InMemoryStorage historical;
    ASSERT_TRUE(historical.append(StorageEvent{
        0, 100, StorageEventType::QuoteUpdate, make_quote("BTC_USDT", 99.0, 100.0, 100)}));
    ASSERT_TRUE(historical.append(StorageEvent{
        0, 110, StorageEventType::MarketTrade, make_trade_event("BTC_USDT", 99.25, 1.0, 110)}));

    SimulatedExecutionSink sink;
    StrategyManager manager(&sink);
    auto* strategy = manager.add_strategy<BuyLimitOnFirstQuote>(2, "BTC_USDT", 0.25, 99.5);
    manager.start_all();

    ReplayRequest request;
    request.max_events = 0;
    const auto replay_stats = BacktestReplayDriver(historical, manager, sink).run(request);

    EXPECT_EQ(replay_stats.events_processed, 2u);
    EXPECT_EQ(replay_stats.trade_events, 1u);
    ASSERT_EQ(strategy->responses.size(), 1u);
    EXPECT_EQ(strategy->responses[0].status, OrderStatus::FILLED);
    EXPECT_DOUBLE_EQ(strategy->responses[0].avg_price, 99.25);
    EXPECT_DOUBLE_EQ(strategy->responses[0].filled_quantity, 0.25);
    EXPECT_EQ(strategy->stats().orders_submitted, 1u);
    EXPECT_EQ(strategy->stats().orders_filled, 1u);

    const auto execution_stats = sink.stats();
    EXPECT_EQ(execution_stats.submitted, 1u);
    EXPECT_EQ(execution_stats.resting, 0u);
    EXPECT_EQ(execution_stats.filled, 1u);
}

}  // namespace
