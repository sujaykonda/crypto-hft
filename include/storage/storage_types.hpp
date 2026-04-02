#pragma once

#include "types/trade.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace hft::storage {

enum class StorageEventType : uint8_t {
    TradeSubmitted = 0,
    OrderResponse = 1,
    MarketTrade = 2,
    QuoteUpdate = 3,
};

using StoragePayload = std::variant<Trade, hft::OrderResponse, TradeEvent, hft::QuoteUpdate>;

struct StorageEvent {
    uint64_t offset{0};
    int64_t ingest_ts_ns{0};
    StorageEventType type{StorageEventType::TradeSubmitted};
    StoragePayload payload;
};

struct ReplayRequest {
    std::optional<uint64_t> start_offset;
    std::optional<int64_t> start_ts_ns;
    std::optional<int64_t> end_ts_ns;
    std::optional<std::string> ticker;
    size_t max_events{1024};
};

struct ReplayBatch {
    std::vector<StorageEvent> events;
    std::optional<uint64_t> next_offset;
    bool end_of_stream{true};
};

struct StorageMetrics {
    uint64_t accepted{0};
    uint64_t rejected_backpressure{0};
    uint64_t committed{0};
    uint64_t flush_count{0};
    uint64_t parse_errors{0};
    uint64_t queue_depth{0};
};

int64_t now_ns();
std::string to_string(StorageEventType type);
std::optional<StorageEventType> storage_event_type_from_string(const std::string& type);
std::optional<std::string> event_ticker(const StorageEvent& event);

}  // namespace hft::storage
