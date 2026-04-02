#include "storage/storage_types.hpp"

#include <chrono>

namespace hft::storage {

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::high_resolution_clock::now().time_since_epoch())
        .count();
}

std::string to_string(StorageEventType type) {
    switch (type) {
        case StorageEventType::TradeSubmitted:
            return "trade_submitted";
        case StorageEventType::OrderResponse:
            return "order_response";
        case StorageEventType::MarketTrade:
            return "market_trade";
        case StorageEventType::QuoteUpdate:
            return "quote_update";
    }
    return "trade_submitted";
}

std::optional<StorageEventType> storage_event_type_from_string(const std::string& type) {
    if (type == "trade_submitted") {
        return StorageEventType::TradeSubmitted;
    }
    if (type == "order_response") {
        return StorageEventType::OrderResponse;
    }
    if (type == "market_trade") {
        return StorageEventType::MarketTrade;
    }
    if (type == "quote_update") {
        return StorageEventType::QuoteUpdate;
    }
    return std::nullopt;
}

std::optional<std::string> event_ticker(const StorageEvent& event) {
    switch (event.type) {
        case StorageEventType::TradeSubmitted:
            if (std::holds_alternative<Trade>(event.payload)) {
                return std::string(std::get<Trade>(event.payload).ticker);
            }
            return std::nullopt;
        case StorageEventType::MarketTrade:
            if (std::holds_alternative<TradeEvent>(event.payload)) {
                return std::string(std::get<TradeEvent>(event.payload).ticker);
            }
            return std::nullopt;
        case StorageEventType::QuoteUpdate:
            if (std::holds_alternative<QuoteUpdate>(event.payload)) {
                return std::string(std::get<QuoteUpdate>(event.payload).ticker);
            }
            return std::nullopt;
        case StorageEventType::OrderResponse:
            return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace hft::storage
