#include "exchange/market_data_client.hpp"

#include "exchange/crypto_com_api.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <openssl/err.h>

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace hft {

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;
using json = nlohmann::json;

template <size_t N>
void copy_string(const std::string& source, char (&destination)[N]) {
    std::strncpy(destination, source.c_str(), N - 1);
    destination[N - 1] = '\0';
}

uint64_t as_u64(const json& value) {
    if (value.is_number_unsigned()) {
        return value.get<uint64_t>();
    }
    if (value.is_number_integer()) {
        const auto parsed = value.get<int64_t>();
        return parsed > 0 ? static_cast<uint64_t>(parsed) : 0;
    }
    if (value.is_string()) {
        try {
            return std::stoull(value.get<std::string>());
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

double as_double(const json& value) {
    if (value.is_number()) {
        return value.get<double>();
    }
    if (value.is_string()) {
        try {
            return std::stod(value.get<std::string>());
        } catch (...) {
            return 0.0;
        }
    }
    return 0.0;
}

int64_t exchange_time_to_ns(uint64_t raw_time) {
    if (raw_time == 0) {
        return 0;
    }
    if (raw_time > 10'000'000'000'000'000ULL) {
        return static_cast<int64_t>(raw_time);
    }
    if (raw_time > 10'000'000'000'000ULL) {
        return static_cast<int64_t>(raw_time * 1'000ULL);
    }
    return static_cast<int64_t>(raw_time * 1'000'000ULL);
}

json build_subscribe_message(const std::vector<std::string>& instruments, int book_depth) {
    json channels = json::array();
    for (const auto& instrument : instruments) {
        channels.push_back("trade." + instrument);
        channels.push_back("book." + instrument + "." + std::to_string(book_depth));
    }

    return {
        {"id", 1},
        {"method", "subscribe"},
        {"params", {{"channels", std::move(channels)}}},
    };
}

std::vector<std::string> split_csv(const std::string& csv) {
    std::vector<std::string> values;
    std::stringstream stream(csv);
    std::string value;
    while (std::getline(stream, value, ',')) {
        if (!value.empty()) {
            values.push_back(value);
        }
    }
    return values;
}

}  // namespace

CryptoComMarketDataClient::CryptoComMarketDataClient(std::vector<std::string> instruments,
                                                     QuoteCallback on_quote,
                                                     TradeCallback on_trade,
                                                     ErrorCallback on_error,
                                                     MarketDataClientConfig config)
    : instruments_(std::move(instruments)),
      on_quote_(std::move(on_quote)),
      on_trade_(std::move(on_trade)),
      on_error_(std::move(on_error)),
      config_(std::move(config)) {
    if (instruments_.empty()) {
        throw std::invalid_argument("CryptoComMarketDataClient requires at least one instrument");
    }
}

CryptoComMarketDataClient::~CryptoComMarketDataClient() {
    stop();
}

void CryptoComMarketDataClient::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    thread_ = std::thread(&CryptoComMarketDataClient::run, this);
}

void CryptoComMarketDataClient::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        if (thread_.joinable()) {
            thread_.join();
        }
        return;
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

bool CryptoComMarketDataClient::is_running() const {
    return running_.load(std::memory_order_acquire);
}

void CryptoComMarketDataClient::run() {
    try {
        net::io_context ioc;
        ssl::context ssl_ctx{ssl::context::tlsv12_client};
        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_verify_mode(ssl::verify_peer);

        tcp::resolver resolver{ioc};
        websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws{ioc, ssl_ctx};

        const auto results = resolver.resolve(config_.host, config_.port);
        beast::get_lowest_layer(ws).connect(results);

        if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), config_.host.c_str())) {
            throw beast::system_error(
                beast::error_code(static_cast<int>(::ERR_get_error()),
                                  net::error::get_ssl_category()),
                "failed to set SNI hostname");
        }

        ws.next_layer().handshake(ssl::stream_base::client);
        beast::get_lowest_layer(ws).expires_never();
        ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        ws.set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
            req.set(http::field::user_agent, "HFT-Platform-MarketData/1.0");
        }));
        ws.handshake(config_.host, config_.target);

        ws.write(net::buffer(build_subscribe_message(instruments_, config_.book_depth).dump()));

        beast::flat_buffer buffer;
        while (running_.load(std::memory_order_acquire)) {
            buffer.clear();
            beast::error_code ec;
            ws.read(buffer, ec);
            if (ec == websocket::error::closed) {
                break;
            }
            if (ec) {
                report_error("market data read failed: " + ec.message());
                break;
            }

            const std::string message = beast::buffers_to_string(buffer.data());

            json decoded = json::parse(message, nullptr, false);
            if (decoded.is_discarded()) {
                report_error("market data JSON parse failed");
                continue;
            }

            if (decoded.value("method", "") == "public/heartbeat") {
                const json response{
                    {"id", decoded.value("id", 0)},
                    {"method", "public/respond-heartbeat"},
                };
                ws.write(net::buffer(response.dump()), ec);
                if (ec) {
                    report_error("market data heartbeat response failed: " + ec.message());
                    break;
                }
                continue;
            }

            handle_message(message);
        }

        beast::error_code close_ec;
        ws.close(websocket::close_code::normal, close_ec);
    } catch (const std::exception& e) {
        if (running_.load(std::memory_order_acquire)) {
            report_error(std::string("market data connection failed: ") + e.what());
        }
    }

    running_.store(false, std::memory_order_release);
}

void CryptoComMarketDataClient::handle_message(const std::string& message) {
    const json decoded = json::parse(message, nullptr, false);
    if (decoded.is_discarded() || !decoded.contains("result") || !decoded["result"].is_object()) {
        return;
    }

    const auto& result = decoded["result"];
    const std::string channel = result.value("channel", "");
    if (channel == "trade") {
        handle_trade_result(result);
    } else if (channel == "book") {
        handle_book_result(result);
    }
}

void CryptoComMarketDataClient::handle_trade_result(const nlohmann::json& result) {
    const std::string fallback_ticker = result.value("instrument_name", "");
    const auto& data = result.value("data", json::array());
    if (!data.is_array()) {
        return;
    }

    for (const auto& item : data) {
        const std::string ticker = item.value("i", fallback_ticker);
        const double price = item.contains("p") ? as_double(item["p"]) : 0.0;
        const double quantity = item.contains("q") ? as_double(item["q"]) : 0.0;
        if (ticker.empty() || price <= 0.0 || quantity <= 0.0) {
            continue;
        }

        TradeEvent trade{};
        copy_string(ticker, trade.ticker);
        trade.price = price;
        trade.quantity = quantity;
        trade.is_buyer_maker = item.value("s", "") == "SELL";
        trade.timestamp_ns = exchange_time_to_ns(item.contains("t") ? as_u64(item["t"]) : 0);
        trade.trade_id = item.contains("d") ? as_u64(item["d"]) : 0;

        if (on_trade_) {
            on_trade_(trade);
        }
    }
}

void CryptoComMarketDataClient::handle_book_result(const nlohmann::json& result) {
    const std::string ticker = result.value("instrument_name", "");
    const auto& data = result.value("data", json::array());
    if (ticker.empty() || !data.is_array() || data.empty() || !data.front().is_object()) {
        return;
    }

    const auto& book = data.front();
    if (!book.contains("bids") || !book.contains("asks") ||
        !book["bids"].is_array() || !book["asks"].is_array() ||
        book["bids"].empty() || book["asks"].empty()) {
        return;
    }

    const auto& best_bid = book["bids"].front();
    const auto& best_ask = book["asks"].front();
    if (!best_bid.is_array() || best_bid.size() < 2 ||
        !best_ask.is_array() || best_ask.size() < 2) {
        return;
    }

    uint64_t timestamp = 0;
    if (book.contains("t")) {
        timestamp = as_u64(book["t"]);
    } else if (book.contains("tt")) {
        timestamp = as_u64(book["tt"]);
    }

    QuoteUpdate quote{};
    copy_string(ticker, quote.ticker);
    quote.bid_price = as_double(best_bid[0]);
    quote.bid_size = as_double(best_bid[1]);
    quote.ask_price = as_double(best_ask[0]);
    quote.ask_size = as_double(best_ask[1]);
    quote.timestamp_ns = exchange_time_to_ns(timestamp);

    if (on_quote_) {
        on_quote_(quote);
    }
}

void CryptoComMarketDataClient::report_error(const std::string& error) {
    if (on_error_) {
        on_error_(error);
    }
}

std::vector<std::string> parse_market_instruments_env(const std::vector<std::string>& defaults) {
    const char* configured = std::getenv("CRYPTO_HFT_MARKET_INSTRUMENTS");
    if (!configured || std::string(configured).empty()) {
        return defaults;
    }

    const std::vector<std::string> parsed = split_csv(configured);
    return parsed.empty() ? defaults : parsed;
}

MarketDataClientConfig make_crypto_com_market_config(bool use_sandbox, int book_depth) {
    MarketDataClientConfig config;
    config.host = use_sandbox ? CryptoComEndpoints::HOST_SANDBOX : CryptoComEndpoints::HOST_PROD;
    config.port = CryptoComEndpoints::PORT;
    config.target = CryptoComEndpoints::PATH_MARKET;
    config.book_depth = book_depth;
    return config;
}

}  // namespace hft
