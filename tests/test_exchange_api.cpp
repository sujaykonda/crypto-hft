#include "exchange/auth_handler.hpp"
#include "exchange/crypto_com_api.hpp"
#include "exchange/market_data_client.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using hft::AuthHandler;
using hft::CryptoComMarketDataClient;
using hft::CryptoComResponseParser;
using hft::OrderStatus;
using hft::QuoteUpdate;
using hft::TradeEvent;
using json = nlohmann::json;

TEST(CryptoComAuth, ParameterStringUsesSortedKeysAndNestedArrays) {
    const json params = {
        {"order_list",
         json::array({
             {{"instrument_name", "ONE_USDT"},
              {"side", "BUY"},
              {"type", "LIMIT"},
              {"price", "0.24"},
              {"quantity", "1.0"}},
             {{"instrument_name", "ONE_USDT"},
              {"side", "BUY"},
              {"type", "STOP_LIMIT"},
              {"price", "0.27"},
              {"quantity", "1.0"},
              {"ref_price", "0.26"}},
         })},
        {"contingency_type", "LIST"},
    };

    EXPECT_EQ(AuthHandler::parameter_string(params),
              "contingency_typeLIST"
              "order_list"
              "instrument_nameONE_USDTprice0.24quantity1.0sideBUYtypeLIMIT"
              "instrument_nameONE_USDTprice0.27quantity1.0ref_price0.26sideBUYtypeSTOP_LIMIT");
}

TEST(CryptoComAuth, SignatureMatchesDocumentedPayloadAlgorithm) {
    const AuthHandler auth("token", "secretKey");

    EXPECT_EQ(auth.sign("public/auth", 11, std::string{}, 1589594102779ULL),
              "9dcebf6eeec155f829227ee447dee73120e0aead42fab74d38ed5d8271793dc8");
}

TEST(CryptoComParser, ParsesMarketTradeAndBookSnapshotMessages) {
    std::vector<TradeEvent> trades;
    std::vector<QuoteUpdate> quotes;
    CryptoComMarketDataClient client(
        {"BTCUSD-PERP"},
        [&quotes](const QuoteUpdate& quote) { quotes.push_back(quote); },
        [&trades](const TradeEvent& trade) { trades.push_back(trade); });

    client.process_text_message(R"json(
        {
          "id": -1,
          "method": "subscribe",
          "code": 0,
          "result": {
            "instrument_name": "BTCUSD-PERP",
            "subscription": "trade.BTCUSD-PERP",
            "channel": "trade",
            "data": [{
              "d": "2030407068",
              "t": 1613581138462,
              "tn": "1613581138462123456",
              "p": "51327.500000",
              "q": "0.000100",
              "s": "SELL",
              "i": "BTCUSD-PERP"
            }]
          }
        }
    )json");

    client.process_text_message(R"json(
        {
          "id": -1,
          "method": "subscribe",
          "code": 0,
          "result": {
            "instrument_name": "BTCUSD-PERP",
            "subscription": "book.BTCUSD-PERP.10",
            "channel": "book",
            "depth": 10,
            "data": [{
              "asks": [["50126.000000", "0.400000", "2"]],
              "bids": [["50113.500000", "0.400000", "3"]],
              "tt": 1647917462799,
              "t": 1647917463000,
              "u": 7845460001
            }]
          }
        }
    )json");

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_STREQ(trades[0].ticker, "BTCUSD-PERP");
    EXPECT_DOUBLE_EQ(trades[0].price, 51327.5);
    EXPECT_DOUBLE_EQ(trades[0].quantity, 0.0001);
    EXPECT_TRUE(trades[0].is_buyer_maker);
    EXPECT_EQ(trades[0].timestamp_ns, 1613581138462123456LL);
    EXPECT_EQ(trades[0].trade_id, 2030407068u);

    ASSERT_EQ(quotes.size(), 1u);
    EXPECT_STREQ(quotes[0].ticker, "BTCUSD-PERP");
    EXPECT_DOUBLE_EQ(quotes[0].bid_price, 50113.5);
    EXPECT_DOUBLE_EQ(quotes[0].bid_size, 0.4);
    EXPECT_DOUBLE_EQ(quotes[0].ask_price, 50126.0);
    EXPECT_DOUBLE_EQ(quotes[0].ask_size, 0.4);
    EXPECT_EQ(quotes[0].timestamp_ns, 1647917463000000000LL);
}

TEST(CryptoComParser, ParsesUserOrderSubscriptionUpdate) {
    const json msg = json::parse(R"json(
        {
          "id": -1,
          "method": "subscribe",
          "code": 0,
          "result": {
            "instrument_name": "BTCUSD-PERP",
            "subscription": "user.order.BTCUSD-PERP",
            "channel": "user.order",
            "data": [{
              "order_id": "19848525",
              "client_oid": "42",
              "quantity": "0.0100",
              "avg_price": "50000.0",
              "cumulative_quantity": "0.0050",
              "cumulative_fee": "0.125",
              "status": "ACTIVE",
              "fee_instrument_name": "USD",
              "transaction_time_ns": "1613570791060827635"
            }]
          }
        }
    )json");

    ASSERT_TRUE(CryptoComResponseParser::is_order_update(msg));
    const auto response = CryptoComResponseParser::parse_order_response(msg);

    EXPECT_EQ(response.client_order_id, 42u);
    EXPECT_EQ(response.exchange_order_id, 19848525u);
    EXPECT_EQ(response.status, OrderStatus::PARTIALLY_FILLED);
    EXPECT_DOUBLE_EQ(response.filled_quantity, 0.005);
    EXPECT_DOUBLE_EQ(response.remaining_quantity, 0.005);
    EXPECT_DOUBLE_EQ(response.avg_price, 50000.0);
    EXPECT_DOUBLE_EQ(response.fee, 0.125);
    EXPECT_STREQ(response.fee_currency, "USD");
    EXPECT_EQ(response.exchange_timestamp_ns, 1613570791060827635LL);
}

}  // namespace
