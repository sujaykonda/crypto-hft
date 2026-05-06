#pragma once

#include "backtest/simulated_execution_sink.hpp"
#include "storage/storage_interface.hpp"
#include "strategy/strategy_base.hpp"

#include <cstdint>
#include <variant>

namespace hft::backtest {

struct BacktestReplayStats {
    uint64_t events_processed{0};
    uint64_t quote_events{0};
    uint64_t trade_events{0};
    uint64_t ignored_execution_events{0};
};

class BacktestReplayDriver {
public:
    BacktestReplayDriver(const storage::IStorage& storage,
                         StrategyManager& strategy_manager,
                         SimulatedExecutionSink& execution_sink)
        : storage_(storage),
          strategy_manager_(strategy_manager),
          execution_sink_(execution_sink) {}

    BacktestReplayStats run(storage::ReplayRequest request) {
        BacktestReplayStats stats;

        while (true) {
            const storage::ReplayBatch batch = storage_.replay(request);
            for (const storage::StorageEvent& event : batch.events) {
                ++stats.events_processed;

                switch (event.type) {
                    case storage::StorageEventType::QuoteUpdate: {
                        const QuoteUpdate& quote = std::get<QuoteUpdate>(event.payload);
                        execution_sink_.on_quote(quote);
                        strategy_manager_.replay_quote(quote);
                        ++stats.quote_events;
                        break;
                    }
                    case storage::StorageEventType::MarketTrade: {
                        const TradeEvent& trade = std::get<TradeEvent>(event.payload);
                        execution_sink_.on_trade(trade);
                        strategy_manager_.replay_trade(trade);
                        ++stats.trade_events;
                        break;
                    }
                    case storage::StorageEventType::TradeSubmitted:
                    case storage::StorageEventType::OrderResponse:
                        ++stats.ignored_execution_events;
                        break;
                }
            }

            if (batch.end_of_stream || !batch.next_offset.has_value()) {
                break;
            }

            request.start_offset = batch.next_offset.value();
        }

        return stats;
    }

private:
    const storage::IStorage& storage_;
    StrategyManager& strategy_manager_;
    SimulatedExecutionSink& execution_sink_;
};

}  // namespace hft::backtest
