#pragma once

#include "types/trade.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace hft {

struct MarketDataClientConfig {
    std::string host{"stream.crypto.com"};
    std::string port{"443"};
    std::string target{"/exchange/v1/market"};
    int book_depth{10};
};

class CryptoComMarketDataClient {
public:
    using QuoteCallback = std::function<void(const QuoteUpdate&)>;
    using TradeCallback = std::function<void(const TradeEvent&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    CryptoComMarketDataClient(std::vector<std::string> instruments,
                              QuoteCallback on_quote,
                              TradeCallback on_trade,
                              ErrorCallback on_error = {},
                              MarketDataClientConfig config = {});
    ~CryptoComMarketDataClient();

    CryptoComMarketDataClient(const CryptoComMarketDataClient&) = delete;
    CryptoComMarketDataClient& operator=(const CryptoComMarketDataClient&) = delete;

    void start();
    void stop();
    bool is_running() const;
    void process_text_message(const std::string& message);

private:
    void run();
    void handle_trade_result(const nlohmann::json& result);
    void handle_book_result(const nlohmann::json& result);
    void report_error(const std::string& error);

    std::vector<std::string> instruments_;
    QuoteCallback on_quote_;
    TradeCallback on_trade_;
    ErrorCallback on_error_;
    MarketDataClientConfig config_;

    std::atomic<bool> running_{false};
    std::thread thread_;
};

std::vector<std::string> parse_market_instruments_env(const std::vector<std::string>& defaults);
MarketDataClientConfig make_crypto_com_market_config(bool use_sandbox, int book_depth = 10);

}  // namespace hft
