#include "storage/in_memory_storage.hpp"
#include "strategy/live_order_executor_sink.hpp"
#include "strategy/storage_replay_driver.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using hft::ExecType;
using hft::IExecutionSink;
using hft::LiveOrderExecutorSink;
using hft::OrderCallback;
using hft::OrderResponse;
using hft::OrderSide;
using hft::OrderStatus;
using hft::OrderType;
using hft::QuoteUpdate;
using hft::ReplayStats;
using hft::StorageFailureCallback;
using hft::StorageReplayDriver;
using hft::StrategyBase;
using hft::StrategyManager;
using hft::StrategyStats;
using hft::TimeInForce;
using hft::Trade;
using hft::TradeEvent;
using hft::storage::IStorage;
using hft::storage::InMemoryStorage;
using hft::storage::ReplayRequest;
using hft::storage::StorageEvent;
using hft::storage::StorageEventType;

template <size_t N>
void copy_string(const std::string& source, char (&destination)[N]) {
    std::strncpy(destination, source.c_str(), N - 1);
    destination[N - 1] = '\0';
}

Trade make_trade(uint64_t client_order_id,
                 uint32_t strategy_id,
                 const std::string& ticker,
                 int64_t timestamp_ns) {
    Trade trade{};
    trade.client_order_id = client_order_id;
    trade.strategy_id = strategy_id;
    copy_string(ticker, trade.ticker);
    trade.side = OrderSide::BUY;
    trade.type = OrderType::LIMIT;
    trade.time_in_force = TimeInForce::GTC;
    trade.exec_type = ExecType::DEFAULT;
    trade.quantity = 2.0;
    trade.price = 101.5;
    trade.trigger_price = 0.0;
    trade.timestamp_ns = timestamp_ns;
    trade.priority = 64;
    return trade;
}

OrderResponse make_response(uint64_t client_order_id, OrderStatus status, int64_t exchange_ts_ns) {
    OrderResponse response{};
    response.client_order_id = client_order_id;
    response.exchange_order_id = client_order_id + 500;
    response.status = status;
    response.filled_quantity = 0.5;
    response.remaining_quantity = 0.0;
    response.avg_price = 99.75;
    response.fee = 0.01;
    copy_string("USDT", response.fee_currency);
    response.exchange_timestamp_ns = exchange_ts_ns;
    return response;
}

TradeEvent make_trade_event(const std::string& ticker, int64_t timestamp_ns, uint64_t trade_id) {
    TradeEvent trade{};
    copy_string(ticker, trade.ticker);
    trade.price = 100.25;
    trade.quantity = 1.0;
    trade.is_buyer_maker = false;
    trade.timestamp_ns = timestamp_ns;
    trade.trade_id = trade_id;
    return trade;
}

QuoteUpdate make_quote(const std::string& ticker, int64_t timestamp_ns) {
    QuoteUpdate quote{};
    copy_string(ticker, quote.ticker);
    quote.bid_price = 100.0;
    quote.bid_size = 3.0;
    quote.ask_price = 100.5;
    quote.ask_size = 4.0;
    quote.timestamp_ns = timestamp_ns;
    return quote;
}

class FakeExecutionSink final : public IExecutionSink {
public:
    bool accept_submissions{true};
    std::vector<Trade> submitted_trades;
    std::vector<OrderCallback> response_callbacks;
    std::vector<StorageFailureCallback> storage_failure_callbacks;
    std::vector<std::string> cancel_all_instruments;

    bool submit_trade(const Trade& trade,
                      OrderCallback response_callback,
                      StorageFailureCallback storage_failure_callback) override {
        if (!accept_submissions) {
            return false;
        }

        submitted_trades.push_back(trade);
        response_callbacks.push_back(std::move(response_callback));
        storage_failure_callbacks.push_back(std::move(storage_failure_callback));
        return true;
    }

    void cancel_order(const std::string& instrument,
                      uint64_t order_id,
                      uint64_t client_oid = 0) override {
        (void)instrument;
        (void)order_id;
        (void)client_oid;
    }

    void cancel_all_orders(const std::string& instrument = "") override {
        cancel_all_instruments.push_back(instrument);
    }

    void deliver_response(size_t index, const OrderResponse& response) {
        ASSERT_LT(index, response_callbacks.size());
        ASSERT_TRUE(static_cast<bool>(response_callbacks[index]));
        response_callbacks[index](response);
    }

    void fail_storage(size_t index) {
        ASSERT_LT(index, storage_failure_callbacks.size());
        ASSERT_TRUE(static_cast<bool>(storage_failure_callbacks[index]));
        storage_failure_callbacks[index]();
    }
};

class FailingStorage final : public IStorage {
public:
    bool append(StorageEvent event) override {
        (void)event;
        ++append_calls_;
        return false;
    }

    hft::storage::ReplayBatch replay(const ReplayRequest& request) const override {
        (void)request;
        return {};
    }

    bool flush(std::chrono::milliseconds timeout) override {
        (void)timeout;
        return true;
    }

    void close() override {}

    hft::storage::StorageMetrics metrics() const override {
        hft::storage::StorageMetrics metrics;
        metrics.rejected_backpressure = append_calls_;
        return metrics;
    }

    uint64_t append_calls() const { return append_calls_; }

private:
    uint64_t append_calls_{0};
};

class RecordingStrategy final : public StrategyBase {
public:
    RecordingStrategy(uint32_t id, std::string name, std::vector<std::string> tickers)
        : StrategyBase(id, std::move(name), std::move(tickers)) {}

    void on_quote(const std::string& ticker,
                  double bid,
                  double ask,
                  double bid_size,
                  double ask_size) override {
        last_quote_ticker = ticker;
        last_bid = bid;
        last_ask = ask;
        last_bid_size = bid_size;
        last_ask_size = ask_size;
    }

    void on_trade(const std::string& ticker, double price, double size, bool is_buy) override {
        last_trade_ticker = ticker;
        last_trade_price = price;
        last_trade_size = size;
        last_trade_is_buy = is_buy;
    }

    bool submit_limit(const std::string& ticker, double quantity, double price) {
        return place_limit_order(ticker.c_str(), OrderSide::BUY, quantity, price);
    }

    bool submit_market(const std::string& ticker, double quantity) {
        return place_market_order(ticker.c_str(), OrderSide::BUY, quantity);
    }

protected:
    void on_order_response(const OrderResponse& response) override {
        responses.push_back(response.status);
        last_response_order_id = response.client_order_id;
    }

public:
    std::string last_quote_ticker;
    std::string last_trade_ticker;
    double last_bid{0.0};
    double last_ask{0.0};
    double last_bid_size{0.0};
    double last_ask_size{0.0};
    double last_trade_price{0.0};
    double last_trade_size{0.0};
    bool last_trade_is_buy{false};
    uint64_t last_response_order_id{0};
    std::vector<OrderStatus> responses;
};

TEST(StrategyRuntime, StrategyBaseOrderHelpersUseExecutionSink) {
    FakeExecutionSink sink;
    RecordingStrategy strategy(7, "Test", {"BTC_USDT"});
    strategy.attach_execution_sink(&sink);
    strategy.start();

    ASSERT_TRUE(strategy.submit_limit("BTC_USDT", 1.25, 101.25));
    ASSERT_EQ(sink.submitted_trades.size(), 1u);
    EXPECT_EQ(sink.submitted_trades[0].strategy_id, 7u);
    EXPECT_STREQ(sink.submitted_trades[0].ticker, "BTC_USDT");
    EXPECT_DOUBLE_EQ(sink.submitted_trades[0].quantity, 1.25);
    EXPECT_DOUBLE_EQ(sink.submitted_trades[0].price, 101.25);

    sink.deliver_response(0, make_response(sink.submitted_trades[0].client_order_id, OrderStatus::FILLED, 1000));
    StrategyStats stats = strategy.stats();
    EXPECT_EQ(stats.orders_submitted, 1u);
    EXPECT_EQ(stats.order_responses, 1u);
    EXPECT_EQ(stats.orders_filled, 1u);
    EXPECT_EQ(stats.orders_rejected, 0u);

    sink.accept_submissions = false;
    EXPECT_FALSE(strategy.submit_market("BTC_USDT", 0.25));
    EXPECT_EQ(strategy.stats().orders_submitted, 1u);
}

TEST(StrategyRuntime, StrategyManagerFiltersByTickerAndActivity) {
    FakeExecutionSink sink;
    StrategyManager manager(&sink);
    auto* btc = manager.add_strategy<RecordingStrategy>(1, "BTC", std::vector<std::string>{"BTC_USDT"});
    auto* eth = manager.add_strategy<RecordingStrategy>(2, "ETH", std::vector<std::string>{"ETH_USDT"});

    manager.start_all();
    eth->stop();

    manager.dispatch_quote(make_quote("BTC_USDT", 100));
    manager.dispatch_trade(make_trade_event("ETH_USDT", 110, 1));

    EXPECT_EQ(btc->stats().quote_events, 1u);
    EXPECT_EQ(btc->stats().trade_events, 0u);
    EXPECT_EQ(eth->stats().quote_events, 0u);
    EXPECT_EQ(eth->stats().trade_events, 0u);
}

TEST(StrategyRuntime, LiveDispatchPersistsMarketEventsExactlyOnce) {
    FakeExecutionSink sink;
    auto storage = std::make_shared<InMemoryStorage>();
    StrategyManager manager(&sink, storage);
    auto* strategy =
        manager.add_strategy<RecordingStrategy>(11, "Recorder", std::vector<std::string>{"BTC_USDT"});

    manager.start_all();
    manager.dispatch_quote(make_quote("BTC_USDT", 100));
    manager.dispatch_trade(make_trade_event("BTC_USDT", 110, 8));

    ReplayRequest request;
    request.max_events = 0;
    const auto batch = storage->replay(request);

    ASSERT_EQ(batch.events.size(), 2u);
    EXPECT_EQ(batch.events[0].offset, 0u);
    EXPECT_EQ(batch.events[0].type, StorageEventType::QuoteUpdate);
    EXPECT_EQ(batch.events[1].offset, 1u);
    EXPECT_EQ(batch.events[1].type, StorageEventType::MarketTrade);
    EXPECT_EQ(strategy->stats().quote_events, 1u);
    EXPECT_EQ(strategy->stats().trade_events, 1u);
}

TEST(StrategyRuntime, LiveExecutionSinkRecordsSubmissionsAndResponses) {
    auto storage = std::make_shared<InMemoryStorage>();
    Trade captured_trade{};
    OrderCallback pending_callback;

    LiveOrderExecutorSink sink(
        [&captured_trade, &pending_callback](const Trade& trade, OrderCallback callback) {
            captured_trade = trade;
            pending_callback = std::move(callback);
            return true;
        },
        [](const std::string& instrument, uint64_t order_id, uint64_t client_oid) {
            (void)instrument;
            (void)order_id;
            (void)client_oid;
        },
        [](const std::string& instrument) { (void)instrument; },
        storage);

    RecordingStrategy strategy(9, "LiveAdapter", {"BTC_USDT"});
    strategy.attach_execution_sink(&sink);
    strategy.start();

    ASSERT_TRUE(strategy.submit_market("BTC_USDT", 0.75));
    ASSERT_TRUE(static_cast<bool>(pending_callback));

    pending_callback(make_response(captured_trade.client_order_id, OrderStatus::FILLED, 200));

    ReplayRequest request;
    request.max_events = 0;
    const auto batch = storage->replay(request);

    ASSERT_EQ(batch.events.size(), 2u);
    EXPECT_EQ(batch.events[0].type, StorageEventType::TradeSubmitted);
    EXPECT_EQ(batch.events[1].type, StorageEventType::OrderResponse);
    EXPECT_EQ(strategy.stats().orders_submitted, 1u);
    EXPECT_EQ(strategy.stats().orders_filled, 1u);

    const auto sink_stats = sink.stats();
    EXPECT_EQ(sink_stats.submitted_events_seen, 1u);
    EXPECT_EQ(sink_stats.response_events_seen, 1u);
}

TEST(StrategyRuntime, StorageAppendFailuresAreSurfacedWithoutBlockingDispatch) {
    auto storage = std::make_shared<FailingStorage>();
    FakeExecutionSink sink;
    StrategyManager manager(&sink, storage);
    auto* strategy =
        manager.add_strategy<RecordingStrategy>(15, "FailureAware", std::vector<std::string>{"BTC_USDT"});

    manager.start_all();
    manager.dispatch_quote(make_quote("BTC_USDT", 123));

    EXPECT_EQ(storage->append_calls(), 1u);
    EXPECT_EQ(strategy->stats().quote_events, 1u);
    EXPECT_EQ(strategy->stats().storage_append_failures, 1u);
}

TEST(StrategyRuntime, ReplayDriverRoutesResponsesAndDoesNotRewriteStorage) {
    InMemoryStorage storage;
    ASSERT_TRUE(storage.append(StorageEvent{
        0, 100, StorageEventType::QuoteUpdate, make_quote("BTC_USDT", 90)}));
    ASSERT_TRUE(storage.append(StorageEvent{
        0, 110, StorageEventType::TradeSubmitted, make_trade(41, 21, "BTC_USDT", 95)}));
    ASSERT_TRUE(storage.append(StorageEvent{
        0, 120, StorageEventType::OrderResponse, make_response(41, OrderStatus::FILLED, 96)}));
    ASSERT_TRUE(storage.append(StorageEvent{
        0, 130, StorageEventType::MarketTrade, make_trade_event("BTC_USDT", 97, 88)}));

    ReplayRequest full_request;
    full_request.max_events = 0;
    const size_t event_count_before = storage.replay(full_request).events.size();

    FakeExecutionSink sink_a;
    StrategyManager manager_a(&sink_a);
    auto* strategy_a =
        manager_a.add_strategy<RecordingStrategy>(21, "ReplayA", std::vector<std::string>{"BTC_USDT"});
    manager_a.start_all();

    StorageReplayDriver driver(storage, manager_a);
    ReplayRequest paged_request;
    paged_request.max_events = 1;
    const ReplayStats replay_stats_a = driver.run(paged_request);

    EXPECT_EQ(replay_stats_a.events_processed, 4u);
    EXPECT_EQ(replay_stats_a.responses_routed, 1u);
    EXPECT_EQ(replay_stats_a.unmatched_responses, 0u);
    EXPECT_EQ(strategy_a->stats().quote_events, 1u);
    EXPECT_EQ(strategy_a->stats().trade_events, 1u);
    EXPECT_EQ(strategy_a->stats().order_responses, 1u);

    const size_t event_count_after = storage.replay(full_request).events.size();
    EXPECT_EQ(event_count_before, event_count_after);

    FakeExecutionSink sink_b;
    StrategyManager manager_b(&sink_b);
    auto* strategy_b =
        manager_b.add_strategy<RecordingStrategy>(21, "ReplayB", std::vector<std::string>{"BTC_USDT"});
    manager_b.start_all();

    const ReplayStats replay_stats_b = StorageReplayDriver(storage, manager_b).run(paged_request);

    EXPECT_EQ(replay_stats_b.events_processed, replay_stats_a.events_processed);
    EXPECT_EQ(replay_stats_b.responses_routed, replay_stats_a.responses_routed);
    EXPECT_EQ(strategy_b->stats().quote_events, strategy_a->stats().quote_events);
    EXPECT_EQ(strategy_b->stats().trade_events, strategy_a->stats().trade_events);
    EXPECT_EQ(strategy_b->stats().order_responses, strategy_a->stats().order_responses);
}

}  // namespace
