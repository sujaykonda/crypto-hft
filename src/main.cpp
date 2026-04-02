#include "core/order_executor.hpp"
#include "strategy/strategy_base.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <iostream>
#include <thread>
#include <csignal>
#include <cstdlib>

using namespace hft;

// ═══════════════════════════════════════════════════════════════════════════
// Example Market Making Strategy
// ═══════════════════════════════════════════════════════════════════════════

class SimpleMarketMaker : public StrategyBase {
    double spread_bps_;
    double order_size_;
    double mid_price_{0.0};
    
    // Order tracking
    uint64_t current_bid_id_{0};
    uint64_t current_ask_id_{0};
    
    // Rate limiting for order updates
    std::chrono::steady_clock::time_point last_update_;
    static constexpr int MIN_UPDATE_INTERVAL_MS = 100;

public:
    SimpleMarketMaker(uint32_t id, const std::string& ticker, 
                      double spread_bps, double order_size)
        : StrategyBase(id, "MarketMaker_" + ticker, {ticker})
        , spread_bps_(spread_bps)
        , order_size_(order_size)
        , last_update_(std::chrono::steady_clock::now()) {}

    void on_quote(const std::string& ticker, 
                 double bid, double ask,
                 double bid_size, double ask_size) override {
        if (!active_) return;
        
        // Rate limit updates
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_update_
        ).count();
        
        if (elapsed < MIN_UPDATE_INTERVAL_MS) return;
        last_update_ = now;
        
        // Calculate mid price and our quotes
        mid_price_ = (bid + ask) / 2.0;
        double half_spread = mid_price_ * (spread_bps_ / 10000.0) / 2.0;
        
        double my_bid = mid_price_ - half_spread;
        double my_ask = mid_price_ + half_spread;
        
        // Place new orders (in production, would amend existing orders)
        place_limit_order(ticker.c_str(), OrderSide::BUY, order_size_, my_bid,
                         TimeInForce::GTC, ExecType::POST_ONLY);
        place_limit_order(ticker.c_str(), OrderSide::SELL, order_size_, my_ask,
                         TimeInForce::GTC, ExecType::POST_ONLY);
    }

    void on_trade(const std::string& ticker, double price, 
                 double size, bool is_buy) override {
        // Could implement trade flow analysis here
        // Example: adjust spread based on trade imbalance
    }

protected:
    void on_start() override {
        std::cout << "[" << name_ << "] Started with spread=" << spread_bps_ 
                  << "bps, size=" << order_size_ << std::endl;
    }

    void on_stop() override {
        // Cancel all orders when stopping
        if (!tickers_.empty()) {
            cancel_all_orders(tickers_[0]);
        }
        std::cout << "[" << name_ << "] Stopped" << std::endl;
    }

    void on_order_response(const OrderResponse& response) override {
        StrategyBase::on_order_response(response);
        
        if (response.status == OrderStatus::REJECTED) {
            std::cerr << "[" << name_ << "] Order rejected: " 
                      << response.error_message << std::endl;
        } else if (response.status == OrderStatus::FILLED) {
            std::cout << "[" << name_ << "] Order filled: qty=" 
                      << response.filled_quantity 
                      << " @ " << response.avg_price << std::endl;
        }
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Example Momentum Strategy
// ═══════════════════════════════════════════════════════════════════════════

class SimpleMomentum : public StrategyBase {
    double threshold_pct_;
    double order_size_;
    double last_price_{0.0};
    double reference_price_{0.0};
    int trade_count_{0};

public:
    SimpleMomentum(uint32_t id, const std::string& ticker,
                   double threshold_pct, double order_size)
        : StrategyBase(id, "Momentum_" + ticker, {ticker})
        , threshold_pct_(threshold_pct)
        , order_size_(order_size) {}

    void on_quote(const std::string& ticker,
                 double bid, double ask,
                 double bid_size, double ask_size) override {
        // Not used in this momentum strategy
    }

    void on_trade(const std::string& ticker, double price,
                 double size, bool is_buy) override {
        if (!active_) return;
        
        if (reference_price_ == 0.0) {
            reference_price_ = price;
            last_price_ = price;
            return;
        }
        
        double pct_change = (price - reference_price_) / reference_price_ * 100.0;
        
        // Momentum signal
        if (pct_change > threshold_pct_) {
            // Price moved up significantly - buy
            place_market_order(ticker.c_str(), OrderSide::BUY, order_size_);
            reference_price_ = price;
            trade_count_++;
        } else if (pct_change < -threshold_pct_) {
            // Price moved down significantly - sell
            place_market_order(ticker.c_str(), OrderSide::SELL, order_size_);
            reference_price_ = price;
            trade_count_++;
        }
        
        last_price_ = price;
    }

protected:
    void on_start() override {
        std::cout << "[" << name_ << "] Started with threshold=" 
                  << threshold_pct_ << "%" << std::endl;
    }

    void on_stop() override {
        std::cout << "[" << name_ << "] Stopped. Total trades: " 
                  << trade_count_ << std::endl;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Global shutdown flag
// ═══════════════════════════════════════════════════════════════════════════

std::atomic<bool> g_running{true};

void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    g_running = false;
}

// ═══════════════════════════════════════════════════════════════════════════
// Main Application
// ═══════════════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    // Load credentials from environment
    const char* api_key = std::getenv("CRYPTO_COM_API_KEY");
    const char* api_secret = std::getenv("CRYPTO_COM_API_SECRET");
    
    if (!api_key || !api_secret) {
        std::cerr << "Error: Set CRYPTO_COM_API_KEY and CRYPTO_COM_API_SECRET environment variables" << std::endl;
        std::cerr << "Example:" << std::endl;
        std::cerr << "  export CRYPTO_COM_API_KEY='your_api_key'" << std::endl;
        std::cerr << "  export CRYPTO_COM_API_SECRET='your_api_secret'" << std::endl;
        return 1;
    }
    
    // Check for sandbox mode
    bool use_sandbox = true;
    const char* env_sandbox = std::getenv("CRYPTO_COM_SANDBOX");
    if (env_sandbox && std::string(env_sandbox) == "false") {
        use_sandbox = false;
        std::cout << "WARNING: Running in PRODUCTION mode!" << std::endl;
    } else {
        std::cout << "Running in SANDBOX mode (set CRYPTO_COM_SANDBOX=false for production)" << std::endl;
    }
    
    try {
        // Initialize Boost.Asio
        net::io_context ioc;
        ssl::context ssl_ctx{ssl::context::tlsv12_client};
        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_verify_mode(ssl::verify_peer);
        
        // Create WebSocket client
        auto ws_client = std::make_shared<CryptoComWebSocketClient>(
            ioc, ssl_ctx, api_key, api_secret, use_sandbox
        );
        
        // Create order executor
        OrderExecutor executor(ws_client);
        
        // Create strategy manager
        StrategyManager strategy_mgr(&executor);
        
        // Add strategies
        auto btc_mm = strategy_mgr.add_strategy<SimpleMarketMaker>(
            1, "BTC_USDT", 10.0, 0.001  // 10 bps spread, 0.001 BTC size
        );
        
        auto eth_mm = strategy_mgr.add_strategy<SimpleMarketMaker>(
            2, "ETH_USDT", 15.0, 0.01   // 15 bps spread, 0.01 ETH size
        );
        
        auto btc_mom = strategy_mgr.add_strategy<SimpleMomentum>(
            3, "BTC_USDT", 0.5, 0.0005  // 0.5% threshold, 0.0005 BTC size
        );
        
        // Start executor
        executor.start();
        
        // Run IO context in background thread
        std::thread io_thread([&ioc]() {
            ioc.run();
        });
        
        // Wait for connection
        std::cout << "Connecting to exchange..." << std::endl;
        int wait_count = 0;
        while (!ws_client->is_authenticated() && g_running && wait_count < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            wait_count++;
        }
        
        if (!ws_client->is_authenticated()) {
            std::cerr << "Failed to connect/authenticate" << std::endl;
            g_running = false;
        } else {
            // Start strategies
            strategy_mgr.start_all();
            std::cout << "\nHFT Platform running with " << strategy_mgr.count() 
                      << " strategies. Press Ctrl+C to stop.\n" << std::endl;
        }
        
        // Main loop - print stats periodically
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            
            if (!g_running) break;
            
            auto stats = executor.get_stats();
            std::cout << "\n═══════════════════════════════════════" << std::endl;
            std::cout << "       Execution Statistics" << std::endl;
            std::cout << "═══════════════════════════════════════" << std::endl;
            std::cout << "  Orders Sent:     " << stats.orders_sent << std::endl;
            std::cout << "  Orders Filled:   " << stats.orders_filled << std::endl;
            std::cout << "  Orders Rejected: " << stats.orders_rejected << std::endl;
            std::cout << "  Avg Latency:     " << stats.avg_latency_us << " µs" << std::endl;
            std::cout << "═══════════════════════════════════════\n" << std::endl;
        }
        
        // Cleanup
        std::cout << "\nShutting down gracefully..." << std::endl;
        strategy_mgr.stop_all();
        executor.stop();
        ioc.stop();
        
        if (io_thread.joinable()) {
            io_thread.join();
        }
        
        std::cout << "Shutdown complete." << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
