#include "core/order_executor.hpp"
#include "storage/jsonl_async_storage.hpp"
#include "strategy/example_strategies.hpp"
#include "strategy/live_order_executor_sink.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace hft;

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    g_running = false;
}

std::shared_ptr<storage::IStorage> maybe_create_storage() {
    const char* storage_path = std::getenv("CRYPTO_HFT_STORAGE_PATH");
    if (!storage_path || std::string(storage_path).empty()) {
        return nullptr;
    }

    storage::JsonlAsyncStorageConfig config;
    config.file_path = storage_path;
    const std::string storage_path_string = config.file_path.string();
    auto storage = std::make_shared<storage::JsonlAsyncStorage>(std::move(config));
    std::cout << "Storage enabled at " << storage_path_string << std::endl;
    return storage;
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    const char* api_key = std::getenv("CRYPTO_COM_API_KEY");
    const char* api_secret = std::getenv("CRYPTO_COM_API_SECRET");

    if (!api_key || !api_secret) {
        std::cerr << "Error: Set CRYPTO_COM_API_KEY and CRYPTO_COM_API_SECRET environment variables"
                  << std::endl;
        std::cerr << "Example:" << std::endl;
        std::cerr << "  export CRYPTO_COM_API_KEY='your_api_key'" << std::endl;
        std::cerr << "  export CRYPTO_COM_API_SECRET='your_api_secret'" << std::endl;
        return 1;
    }

    bool use_sandbox = true;
    const char* env_sandbox = std::getenv("CRYPTO_COM_SANDBOX");
    if (env_sandbox && std::string(env_sandbox) == "false") {
        use_sandbox = false;
        std::cout << "WARNING: Running in PRODUCTION mode!" << std::endl;
    } else {
        std::cout << "Running in SANDBOX mode (set CRYPTO_COM_SANDBOX=false for production)"
                  << std::endl;
    }

    try {
        auto storage = maybe_create_storage();

        net::io_context ioc;
        ssl::context ssl_ctx{ssl::context::tlsv12_client};
        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_verify_mode(ssl::verify_peer);

        auto ws_client = std::make_shared<CryptoComWebSocketClient>(
            ioc, ssl_ctx, api_key, api_secret, use_sandbox);

        OrderExecutor executor(ws_client);
        LiveOrderExecutorSink execution_sink(
            [&executor](const Trade& trade, OrderCallback callback) {
                return executor.submit_trade(trade, std::move(callback));
            },
            [&executor](const std::string& instrument, uint64_t order_id, uint64_t client_oid) {
                executor.cancel_order(instrument, order_id, client_oid);
            },
            [&executor](const std::string& instrument) { executor.cancel_all_orders(instrument); },
            storage);
        StrategyManager strategy_mgr(&execution_sink, storage);

        strategy_mgr.add_strategy<SimpleMarketMaker>(1, "BTC_USDT", 10.0, 0.001);
        strategy_mgr.add_strategy<SimpleMarketMaker>(2, "ETH_USDT", 15.0, 0.01);
        strategy_mgr.add_strategy<SimpleMomentum>(3, "BTC_USDT", 0.5, 0.0005);

        executor.start();

        std::thread io_thread([&ioc]() { ioc.run(); });

        std::cout << "Connecting to exchange..." << std::endl;
        int wait_count = 0;
        while (!ws_client->is_authenticated() && g_running && wait_count < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ++wait_count;
        }

        if (!ws_client->is_authenticated()) {
            std::cerr << "Failed to connect/authenticate" << std::endl;
            g_running = false;
        } else {
            strategy_mgr.start_all();
            std::cout << "\nHFT Platform running with " << strategy_mgr.count()
                      << " strategies. Press Ctrl+C to stop.\n"
                      << std::endl;
        }

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (!g_running) {
                break;
            }

            const auto stats = executor.get_stats();
            std::cout << "\n=======================================" << std::endl;
            std::cout << "       Execution Statistics" << std::endl;
            std::cout << "=======================================" << std::endl;
            std::cout << "  Orders Sent:     " << stats.orders_sent << std::endl;
            std::cout << "  Orders Filled:   " << stats.orders_filled << std::endl;
            std::cout << "  Orders Rejected: " << stats.orders_rejected << std::endl;
            std::cout << "  Avg Latency:     " << stats.avg_latency_us << " us" << std::endl;

            if (storage) {
                const auto storage_metrics = storage->metrics();
                std::cout << "  Storage Accepted: " << storage_metrics.accepted << std::endl;
                std::cout << "  Storage Committed:" << storage_metrics.committed << std::endl;
            }

            std::cout << "=======================================\n" << std::endl;
        }

        std::cout << "\nShutting down gracefully..." << std::endl;
        strategy_mgr.stop_all();
        executor.stop();
        ioc.stop();

        if (io_thread.joinable()) {
            io_thread.join();
        }

        if (storage) {
            storage->flush(std::chrono::milliseconds(250));
            storage->close();
        }

        std::cout << "Shutdown complete." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
