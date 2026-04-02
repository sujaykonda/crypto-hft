#include "storage/in_memory_storage.hpp"
#include "storage/jsonl_async_storage.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using hft::ExecType;
using hft::OrderResponse;
using hft::OrderSide;
using hft::OrderStatus;
using hft::OrderType;
using hft::QuoteUpdate;
using hft::TimeInForce;
using hft::Trade;
using hft::TradeEvent;
using hft::storage::InMemoryStorage;
using hft::storage::JsonlAsyncStorage;
using hft::storage::JsonlAsyncStorageConfig;
using hft::storage::ReplayRequest;
using hft::storage::StorageEvent;
using hft::storage::StorageEventType;

std::filesystem::path make_temp_file_path() {
    static std::atomic<uint64_t> counter{0};
    const uint64_t id = counter.fetch_add(1, std::memory_order_relaxed);
    return std::filesystem::temp_directory_path() /
           ("crypto_hft_storage_test_" + std::to_string(hft::storage::now_ns()) + "_" +
            std::to_string(id) + ".jsonl");
}

template <size_t N>
void copy_string(const std::string& source, char (&destination)[N]) {
    std::strncpy(destination, source.c_str(), N - 1);
    destination[N - 1] = '\0';
}

Trade make_trade(uint64_t client_order_id, const std::string& ticker, int64_t timestamp_ns) {
    Trade trade{};
    trade.client_order_id = client_order_id;
    trade.strategy_id = 7;
    copy_string(ticker, trade.ticker);
    trade.side = OrderSide::BUY;
    trade.type = OrderType::LIMIT;
    trade.time_in_force = TimeInForce::GTC;
    trade.exec_type = ExecType::DEFAULT;
    trade.quantity = 2.5;
    trade.price = 101.25;
    trade.trigger_price = 0.0;
    trade.timestamp_ns = timestamp_ns;
    trade.priority = 64;
    return trade;
}

OrderResponse make_response(uint64_t client_order_id, OrderStatus status, int64_t exchange_ts_ns) {
    OrderResponse response{};
    response.client_order_id = client_order_id;
    response.exchange_order_id = client_order_id + 1000;
    response.status = status;
    response.filled_quantity = 1.25;
    response.remaining_quantity = 1.25;
    response.avg_price = 101.0;
    response.fee = 0.01;
    copy_string("USDT", response.fee_currency);
    response.exchange_timestamp_ns = exchange_ts_ns;
    response.error_code = 0;
    copy_string("", response.error_message);
    return response;
}

TradeEvent make_trade_event(uint64_t trade_id, const std::string& ticker, int64_t timestamp_ns) {
    TradeEvent trade_event{};
    copy_string(ticker, trade_event.ticker);
    trade_event.price = 99.5;
    trade_event.quantity = 0.5;
    trade_event.is_buyer_maker = false;
    trade_event.timestamp_ns = timestamp_ns;
    trade_event.trade_id = trade_id;
    return trade_event;
}

QuoteUpdate make_quote(const std::string& ticker, int64_t timestamp_ns) {
    QuoteUpdate quote{};
    copy_string(ticker, quote.ticker);
    quote.bid_price = 99.0;
    quote.bid_size = 2.0;
    quote.ask_price = 100.0;
    quote.ask_size = 3.0;
    quote.timestamp_ns = timestamp_ns;
    return quote;
}

StorageEvent submitted_event(uint64_t order_id, const std::string& ticker, int64_t ingest_ns) {
    return StorageEvent{0, ingest_ns, StorageEventType::TradeSubmitted, make_trade(order_id, ticker, ingest_ns - 5)};
}

StorageEvent response_event(uint64_t order_id, int64_t ingest_ns) {
    return StorageEvent{0, ingest_ns, StorageEventType::OrderResponse,
                        make_response(order_id, OrderStatus::FILLED, ingest_ns - 2)};
}

StorageEvent market_trade_event(uint64_t trade_id, const std::string& ticker, int64_t ingest_ns) {
    return StorageEvent{0, ingest_ns, StorageEventType::MarketTrade,
                        make_trade_event(trade_id, ticker, ingest_ns - 1)};
}

StorageEvent quote_event(const std::string& ticker, int64_t ingest_ns) {
    return StorageEvent{0, ingest_ns, StorageEventType::QuoteUpdate, make_quote(ticker, ingest_ns)};
}

void remove_file_if_exists(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}  // namespace

TEST(StorageInterface, InMemoryRoundTrip) {
    InMemoryStorage storage;

    ASSERT_TRUE(storage.append(submitted_event(10, "BTC_USDT", 100)));
    ASSERT_TRUE(storage.append(response_event(10, 110)));
    ASSERT_TRUE(storage.append(market_trade_event(77, "ETH_USDT", 120)));
    ASSERT_TRUE(storage.append(quote_event("BTC_USDT", 130)));

    ReplayRequest request;
    request.max_events = 0;
    const auto batch = storage.replay(request);

    ASSERT_EQ(batch.events.size(), 4u);
    EXPECT_TRUE(batch.end_of_stream);

    for (size_t i = 0; i < batch.events.size(); ++i) {
        EXPECT_EQ(batch.events[i].offset, i);
    }

    const Trade& trade = std::get<Trade>(batch.events[0].payload);
    EXPECT_STREQ(trade.ticker, "BTC_USDT");
    EXPECT_EQ(trade.client_order_id, 10u);

    const OrderResponse& response = std::get<OrderResponse>(batch.events[1].payload);
    EXPECT_EQ(response.status, OrderStatus::FILLED);
    EXPECT_EQ(response.client_order_id, 10u);

    const TradeEvent& trade_event = std::get<TradeEvent>(batch.events[2].payload);
    EXPECT_STREQ(trade_event.ticker, "ETH_USDT");
    EXPECT_EQ(trade_event.trade_id, 77u);

    const QuoteUpdate& quote = std::get<QuoteUpdate>(batch.events[3].payload);
    EXPECT_STREQ(quote.ticker, "BTC_USDT");
}

TEST(StorageInterface, ReplayFilters) {
    InMemoryStorage storage;

    ASSERT_TRUE(storage.append(submitted_event(1, "BTC_USDT", 100)));
    ASSERT_TRUE(storage.append(quote_event("ETH_USDT", 150)));
    ASSERT_TRUE(storage.append(market_trade_event(2, "ETH_USDT", 200)));
    ASSERT_TRUE(storage.append(quote_event("BTC_USDT", 250)));
    ASSERT_TRUE(storage.append(response_event(1, 300)));

    ReplayRequest offset_request;
    offset_request.start_offset = 2;
    offset_request.max_events = 0;
    const auto offset_batch = storage.replay(offset_request);
    ASSERT_EQ(offset_batch.events.size(), 3u);
    EXPECT_EQ(offset_batch.events.front().offset, 2u);

    ReplayRequest time_request;
    time_request.start_ts_ns = 140;
    time_request.end_ts_ns = 220;
    time_request.max_events = 0;
    const auto time_batch = storage.replay(time_request);
    ASSERT_EQ(time_batch.events.size(), 2u);
    EXPECT_EQ(time_batch.events[0].offset, 1u);
    EXPECT_EQ(time_batch.events[1].offset, 2u);

    ReplayRequest ticker_request;
    ticker_request.ticker = "ETH_USDT";
    ticker_request.max_events = 0;
    const auto ticker_batch = storage.replay(ticker_request);
    ASSERT_EQ(ticker_batch.events.size(), 2u);
    EXPECT_EQ(ticker_batch.events[0].offset, 1u);
    EXPECT_EQ(ticker_batch.events[1].offset, 2u);

    ReplayRequest page_request;
    page_request.start_offset = 1;
    page_request.max_events = 1;
    const auto page_batch = storage.replay(page_request);
    ASSERT_EQ(page_batch.events.size(), 1u);
    EXPECT_FALSE(page_batch.end_of_stream);
    ASSERT_TRUE(page_batch.next_offset.has_value());
    EXPECT_EQ(page_batch.next_offset.value(), 2u);
}

TEST(StorageInterface, MonotonicOffsetsMultiProducer) {
    InMemoryStorage storage;

    constexpr int thread_count = 4;
    constexpr int events_per_thread = 250;
    std::atomic<bool> start{false};
    std::atomic<bool> all_ok{true};

    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([t, &storage, &start, &all_ok] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < events_per_thread; ++i) {
                const uint64_t id = static_cast<uint64_t>(t) * 100000 + static_cast<uint64_t>(i);
                const bool ok = storage.append(submitted_event(id, "BTC_USDT", 1000 + i));
                if (!ok) {
                    all_ok.store(false, std::memory_order_release);
                    return;
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }
    ASSERT_TRUE(all_ok.load(std::memory_order_acquire));

    ReplayRequest request;
    request.max_events = 0;
    const auto batch = storage.replay(request);

    const size_t total = static_cast<size_t>(thread_count * events_per_thread);
    ASSERT_EQ(batch.events.size(), total);
    EXPECT_TRUE(batch.end_of_stream);

    for (size_t i = 0; i < total; ++i) {
        EXPECT_EQ(batch.events[i].offset, i);
    }
}

TEST(JsonlAsync, Backpressure) {
    const std::filesystem::path path = make_temp_file_path();
    remove_file_if_exists(path);

    JsonlAsyncStorageConfig config;
    config.file_path = path;
    config.queue_capacity = 2;
    config.batch_size = 32;
    config.flush_interval = std::chrono::milliseconds(1000);
    config.start_flusher_thread = false;

    JsonlAsyncStorage storage(config);

    ASSERT_TRUE(storage.append(submitted_event(1, "BTC_USDT", 100)));
    ASSERT_TRUE(storage.append(submitted_event(2, "BTC_USDT", 110)));
    EXPECT_FALSE(storage.append(submitted_event(3, "BTC_USDT", 120)));

    auto metrics = storage.metrics();
    EXPECT_EQ(metrics.accepted, 2u);
    EXPECT_EQ(metrics.rejected_backpressure, 1u);
    EXPECT_EQ(metrics.queue_depth, 2u);

    EXPECT_TRUE(storage.flush(std::chrono::milliseconds(200)));

    metrics = storage.metrics();
    EXPECT_EQ(metrics.committed, 2u);
    EXPECT_EQ(metrics.queue_depth, 0u);

    storage.close();
    remove_file_if_exists(path);
}

TEST(JsonlAsync, FlushAndDurability) {
    const std::filesystem::path path = make_temp_file_path();
    remove_file_if_exists(path);

    {
        JsonlAsyncStorageConfig config;
        config.file_path = path;

        JsonlAsyncStorage storage(config);
        ASSERT_TRUE(storage.append(submitted_event(11, "BTC_USDT", 101)));
        ASSERT_TRUE(storage.append(quote_event("ETH_USDT", 102)));
        ASSERT_TRUE(storage.append(market_trade_event(300, "BTC_USDT", 103)));
        ASSERT_TRUE(storage.flush(std::chrono::seconds(2)));

        const auto metrics = storage.metrics();
        EXPECT_EQ(metrics.committed, 3u);
        storage.close();
    }

    {
        JsonlAsyncStorageConfig reopen_config;
        reopen_config.file_path = path;
        reopen_config.start_flusher_thread = false;

        JsonlAsyncStorage reopened(reopen_config);
        ReplayRequest request;
        request.max_events = 0;
        const auto batch = reopened.replay(request);

        ASSERT_EQ(batch.events.size(), 3u);
        EXPECT_EQ(batch.events[0].offset, 0u);
        EXPECT_EQ(batch.events[1].offset, 1u);
        EXPECT_EQ(batch.events[2].offset, 2u);
        reopened.close();
    }

    remove_file_if_exists(path);
}

TEST(JsonlAsync, CloseBehavior) {
    const std::filesystem::path path = make_temp_file_path();
    remove_file_if_exists(path);

    JsonlAsyncStorageConfig config;
    config.file_path = path;
    config.start_flusher_thread = false;

    JsonlAsyncStorage storage(config);
    ASSERT_TRUE(storage.append(submitted_event(1, "BTC_USDT", 100)));
    ASSERT_TRUE(storage.append(quote_event("BTC_USDT", 101)));

    storage.close();
    EXPECT_FALSE(storage.append(submitted_event(2, "BTC_USDT", 102)));

    JsonlAsyncStorageConfig reopen_config;
    reopen_config.file_path = path;
    reopen_config.start_flusher_thread = false;

    JsonlAsyncStorage reopened(reopen_config);
    ReplayRequest request;
    request.max_events = 0;
    const auto batch = reopened.replay(request);
    ASSERT_EQ(batch.events.size(), 2u);
    reopened.close();

    remove_file_if_exists(path);
}

TEST(JsonlAsync, MalformedLineRecovery) {
    const std::filesystem::path path = make_temp_file_path();
    remove_file_if_exists(path);

    {
        nlohmann::json valid_one = {
            {"schema_version", 1},
            {"offset", 0},
            {"ingest_ts_ns", 100},
            {"event_type", "quote_update"},
            {"ticker", "BTC_USDT"},
            {"payload",
             {
                 {"ticker", "BTC_USDT"},
                 {"bid_price", 100.0},
                 {"bid_size", 1.0},
                 {"ask_price", 100.5},
                 {"ask_size", 1.2},
                 {"timestamp_ns", 99},
             }},
        };

        nlohmann::json valid_two = {
            {"schema_version", 1},
            {"offset", 1},
            {"ingest_ts_ns", 101},
            {"event_type", "quote_update"},
            {"ticker", "ETH_USDT"},
            {"payload",
             {
                 {"ticker", "ETH_USDT"},
                 {"bid_price", 200.0},
                 {"bid_size", 2.0},
                 {"ask_price", 200.5},
                 {"ask_size", 2.2},
                 {"timestamp_ns", 100},
             }},
        };

        std::ofstream output(path, std::ios::out | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output << valid_one.dump() << '\n';
        output << "this-is-not-json" << '\n';
        output << valid_two.dump() << '\n';
        output.close();
    }

    JsonlAsyncStorageConfig config;
    config.file_path = path;
    config.start_flusher_thread = false;

    JsonlAsyncStorage storage(config);
    const auto metrics = storage.metrics();
    EXPECT_EQ(metrics.parse_errors, 1u);

    ReplayRequest request;
    request.max_events = 0;
    const auto batch = storage.replay(request);
    ASSERT_EQ(batch.events.size(), 2u);
    EXPECT_EQ(batch.events[0].offset, 0u);
    EXPECT_EQ(batch.events[1].offset, 1u);

    storage.close();
    remove_file_if_exists(path);
}
