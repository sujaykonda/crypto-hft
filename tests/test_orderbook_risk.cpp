#include "backtest/simulated_execution_sink.hpp"
#include "orderbook/order_book.hpp"
#include "risk/risk_manager.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace {

using hft::OrderCallback;
using hft::OrderResponse;
using hft::OrderSide;
using hft::OrderStatus;
using hft::QuoteUpdate;
using hft::Trade;
using hft::backtest::SimulatedExecutionSink;
using hft::orderbook::BookLevel;
using hft::orderbook::MultiExchangeOrderBook;
using hft::orderbook::OrderBook;
using hft::risk::RiskCheckedExecutionSink;
using hft::risk::RiskLimits;
using hft::risk::RiskManager;

template <size_t N>
void copy_string(const std::string& source, char (&destination)[N]) {
    std::strncpy(destination, source.c_str(), N - 1);
    destination[N - 1] = '\0';
}

QuoteUpdate make_quote(const std::string& ticker, double bid, double ask) {
    QuoteUpdate quote{};
    copy_string(ticker, quote.ticker);
    quote.bid_price = bid;
    quote.bid_size = 1.0;
    quote.ask_price = ask;
    quote.ask_size = 2.0;
    quote.timestamp_ns = 100;
    return quote;
}

TEST(OrderBook, SnapshotDeltaAndBestQuote) {
    OrderBook book("BTC_USDT");
    book.apply_snapshot(
        {BookLevel{100.0, 1.0}, BookLevel{99.0, 3.0}},
        {BookLevel{101.0, 2.0}, BookLevel{102.0, 4.0}},
        100,
        7);

    auto quote = book.best_quote();
    ASSERT_TRUE(quote.has_value());
    EXPECT_STREQ(quote->ticker, "BTC_USDT");
    EXPECT_DOUBLE_EQ(quote->bid_price, 100.0);
    EXPECT_DOUBLE_EQ(quote->ask_price, 101.0);

    book.apply_delta({BookLevel{100.0, 0.0}, BookLevel{100.5, 1.5}},
                     {BookLevel{101.0, 0.0}, BookLevel{100.75, 2.5}},
                     110,
                     8);

    quote = book.best_quote();
    ASSERT_TRUE(quote.has_value());
    EXPECT_DOUBLE_EQ(quote->bid_price, 100.5);
    EXPECT_DOUBLE_EQ(quote->bid_size, 1.5);
    EXPECT_DOUBLE_EQ(quote->ask_price, 100.75);
    EXPECT_DOUBLE_EQ(quote->ask_size, 2.5);

    const auto snapshot = book.snapshot(1);
    ASSERT_EQ(snapshot.bids.size(), 1u);
    ASSERT_EQ(snapshot.asks.size(), 1u);
    EXPECT_EQ(snapshot.sequence, 8u);
}

TEST(OrderBook, MultiExchangeBookCoalescesBestBidAndAsk) {
    MultiExchangeOrderBook books;
    books.book_for("crypto_com", "BTC_USDT")
        .apply_snapshot({BookLevel{100.0, 1.0}}, {BookLevel{101.0, 1.0}}, 100);
    books.book_for("coinbase", "BTC_USDT")
        .apply_snapshot({BookLevel{100.5, 2.0}}, {BookLevel{101.5, 2.0}}, 110);
    books.book_for("kraken", "BTC_USDT")
        .apply_snapshot({BookLevel{99.5, 3.0}}, {BookLevel{100.75, 3.0}}, 120);

    const auto quote = books.best_quote("BTC_USDT");
    ASSERT_TRUE(quote.has_value());
    EXPECT_DOUBLE_EQ(quote->quote.bid_price, 100.5);
    EXPECT_EQ(quote->bid_exchange, "coinbase");
    EXPECT_DOUBLE_EQ(quote->quote.ask_price, 100.75);
    EXPECT_EQ(quote->ask_exchange, "kraken");
}

TEST(RiskManager, RejectsOrdersThatExceedLimitsAndTracksFilledPosition) {
    RiskLimits limits;
    limits.max_order_quantity = 1.0;
    limits.max_order_notional = 200.0;
    limits.max_net_position_abs = 2.0;
    RiskManager risk(limits);
    risk.update_quote(make_quote("BTC_USDT", 99.0, 101.0));

    Trade accepted = Trade::create_limit_order(4, "BTC_USDT", OrderSide::BUY, 1.0, 100.0);
    EXPECT_TRUE(risk.check_order(accepted).accepted);

    Trade too_large = Trade::create_limit_order(4, "BTC_USDT", OrderSide::BUY, 1.5, 100.0);
    EXPECT_FALSE(risk.check_order(too_large).accepted);

    Trade too_much_notional = Trade::create_limit_order(4, "BTC_USDT", OrderSide::BUY, 1.0, 250.0);
    EXPECT_FALSE(risk.check_order(too_much_notional).accepted);

    OrderResponse fill;
    fill.client_order_id = accepted.client_order_id;
    fill.status = OrderStatus::FILLED;
    fill.filled_quantity = 1.0;
    fill.avg_price = 100.0;
    risk.on_fill(accepted, fill);

    const auto position = risk.position("BTC_USDT");
    EXPECT_DOUBLE_EQ(position.net_quantity, 1.0);
    EXPECT_DOUBLE_EQ(position.avg_entry_price, 100.0);
}

TEST(RiskCheckedExecutionSink, BlocksRejectedOrdersAndUpdatesOnFill) {
    SimulatedExecutionSink downstream;
    downstream.on_quote(make_quote("BTC_USDT", 99.0, 100.0));

    RiskLimits limits;
    limits.max_order_quantity = 1.0;
    limits.max_net_position_abs = 1.0;
    RiskManager risk(limits);
    risk.update_quote(make_quote("BTC_USDT", 99.0, 100.0));
    RiskCheckedExecutionSink sink(downstream, risk);

    Trade accepted = Trade::create_market_order(9, "BTC_USDT", OrderSide::BUY, 1.0);
    OrderResponse accepted_response;
    ASSERT_TRUE(sink.submit_trade(
        accepted,
        [&accepted_response](const OrderResponse& response) { accepted_response = response; },
        nullptr));
    EXPECT_EQ(accepted_response.status, OrderStatus::FILLED);
    EXPECT_DOUBLE_EQ(risk.position("BTC_USDT").net_quantity, 1.0);

    Trade rejected = Trade::create_market_order(9, "BTC_USDT", OrderSide::BUY, 0.1);
    OrderResponse rejected_response;
    EXPECT_FALSE(sink.submit_trade(
        rejected,
        [&rejected_response](const OrderResponse& response) { rejected_response = response; },
        nullptr));
    EXPECT_EQ(rejected_response.status, OrderStatus::REJECTED);
    EXPECT_EQ(sink.rejected_count(), 1u);
}

}  // namespace
