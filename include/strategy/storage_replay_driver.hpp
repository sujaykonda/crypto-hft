#pragma once

#include "storage/storage_interface.hpp"
#include "strategy/strategy_base.hpp"

#include <cstdint>
#include <unordered_map>

namespace hft {

struct ReplayStats {
    uint64_t events_processed{0};
    uint64_t responses_routed{0};
    uint64_t unmatched_responses{0};
};

class StorageReplayDriver {
public:
    StorageReplayDriver(const storage::IStorage& storage, StrategyManager& strategy_manager)
        : storage_(storage), strategy_manager_(strategy_manager) {}

    ReplayStats run(storage::ReplayRequest request) const {
        ReplayStats stats;
        std::unordered_map<uint64_t, uint32_t> order_to_strategy;

        while (true) {
            const storage::ReplayBatch batch = storage_.replay(request);
            for (const storage::StorageEvent& event : batch.events) {
                ++stats.events_processed;

                switch (event.type) {
                    case storage::StorageEventType::TradeSubmitted: {
                        const Trade& trade = std::get<Trade>(event.payload);
                        order_to_strategy[trade.client_order_id] = trade.strategy_id;
                        break;
                    }
                    case storage::StorageEventType::OrderResponse: {
                        const OrderResponse& response = std::get<OrderResponse>(event.payload);
                        const auto it = order_to_strategy.find(response.client_order_id);
                        if (it == order_to_strategy.end()) {
                            ++stats.unmatched_responses;
                            break;
                        }

                        strategy_manager_.dispatch_order_response(it->second, response);
                        ++stats.responses_routed;
                        break;
                    }
                    case storage::StorageEventType::MarketTrade:
                        strategy_manager_.replay_trade(std::get<TradeEvent>(event.payload));
                        break;
                    case storage::StorageEventType::QuoteUpdate:
                        strategy_manager_.replay_quote(std::get<QuoteUpdate>(event.payload));
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
};

}  // namespace hft
