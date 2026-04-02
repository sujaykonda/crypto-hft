#pragma once

#include "types/trade.hpp"
#include "exchange/auth_handler.hpp"
#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <nlohmann/json.hpp>

namespace hft {

using json = nlohmann::json;

// ═══════════════════════════════════════════════════════════════════════════
// Crypto.com Exchange Constants
// ═══════════════════════════════════════════════════════════════════════════

namespace CryptoComEndpoints {
    // Production
    constexpr const char* WS_USER     = "wss://stream.crypto.com/exchange/v1/user";
    constexpr const char* WS_MARKET   = "wss://stream.crypto.com/exchange/v1/market";
    constexpr const char* REST_BASE   = "https://api.crypto.com/exchange/v1";
    
    // Sandbox/testnet
    constexpr const char* WS_USER_SANDBOX   = "wss://uat-stream.3ona.co/exchange/v1/user";
    constexpr const char* WS_MARKET_SANDBOX = "wss://uat-stream.3ona.co/exchange/v1/market";
    constexpr const char* REST_BASE_SANDBOX = "https://uat-api.3ona.co/exchange/v1";
    
    // Host extraction helpers
    constexpr const char* HOST_PROD    = "stream.crypto.com";
    constexpr const char* HOST_SANDBOX = "uat-stream.3ona.co";
    constexpr const char* PORT         = "443";
    constexpr const char* PATH_USER    = "/exchange/v1/user";
    constexpr const char* PATH_MARKET  = "/exchange/v1/market";
}

// ═══════════════════════════════════════════════════════════════════════════
// Error Codes from Crypto.com
// ═══════════════════════════════════════════════════════════════════════════

namespace CryptoComErrors {
    constexpr int SUCCESS = 0;
    constexpr int SYS_ERROR = 10001;
    constexpr int UNAUTHORIZED = 10002;
    constexpr int IP_ILLEGAL = 10003;
    constexpr int BAD_REQUEST = 10004;
    constexpr int USER_TIER_INVALID = 10005;
    constexpr int TOO_MANY_REQUESTS = 10006;
    constexpr int INVALID_NONCE = 10007;
    constexpr int METHOD_NOT_FOUND = 10008;
    constexpr int INVALID_DATE_RANGE = 10009;
    constexpr int DUPLICATE_RECORD = 20001;
    constexpr int NEGATIVE_BALANCE = 20002;
    constexpr int SYMBOL_NOT_FOUND = 30003;
    constexpr int SIDE_NOT_SUPPORTED = 30004;
    constexpr int ORDERTYPE_NOT_SUPPORTED = 30005;
    constexpr int MIN_PRICE_VIOLATED = 30006;
    constexpr int MAX_PRICE_VIOLATED = 30007;
    constexpr int MIN_QUANTITY_VIOLATED = 30008;
    constexpr int MAX_QUANTITY_VIOLATED = 30009;
    constexpr int MISSING_ARGUMENT = 30010;
    constexpr int INVALID_PRICE_PRECISION = 30013;
    constexpr int INVALID_QUANTITY_PRECISION = 30014;
    constexpr int MIN_NOTIONAL_VIOLATED = 30016;
    constexpr int MAX_NOTIONAL_VIOLATED = 30017;
    constexpr int MAX_NUM_ORDERS_VIOLATED = 30023;
    constexpr int MAX_NUM_ORD_AMEND_VIOLATED = 30024;
    constexpr int MAX_NUM_ORD_CANCEL_VIOLATED = 30025;
    constexpr int DUPLICATE_CLORDID = 30026;
    constexpr int PRICE_OVER_LIMIT = 30038;
}

// ═══════════════════════════════════════════════════════════════════════════
// Request/Response Message Builders
// ═══════════════════════════════════════════════════════════════════════════

class CryptoComMessageBuilder {
    const AuthHandler& auth_;
    std::atomic<uint64_t> request_id_{1};

public:
    explicit CryptoComMessageBuilder(const AuthHandler& auth) : auth_(auth) {}

    uint64_t next_id() {
        return request_id_.fetch_add(1, std::memory_order_relaxed);
    }

    // ───────────────────────────────────────────────────────────────────
    // Authentication Request
    // ───────────────────────────────────────────────────────────────────
    json build_auth_request() {
        uint64_t id = next_id();
        uint64_t nonce = AuthHandler::current_timestamp_ms();
        std::string method = "public/auth";
        
        std::string params_str = "";
        std::string sig = auth_.sign(method, id, params_str, nonce);
        
        return {
            {"id", id},
            {"method", method},
            {"api_key", auth_.api_key()},
            {"sig", sig},
            {"nonce", nonce}
        };
    }

    // ───────────────────────────────────────────────────────────────────
    // Create Order Request
    // ───────────────────────────────────────────────────────────────────
    json build_create_order(const Trade& trade) {
        uint64_t id = next_id();
        uint64_t nonce = AuthHandler::current_timestamp_ms();
        std::string method = "private/create-order";
        
        json params = {
            {"instrument_name", std::string(trade.ticker)},
            {"side", trade.side == OrderSide::BUY ? "BUY" : "SELL"},
            {"type", order_type_to_string(trade.type)},
            {"quantity", format_double(trade.quantity)},
            {"client_oid", std::to_string(trade.client_order_id)}
        };
        
        if (trade.type == OrderType::LIMIT || 
            trade.type == OrderType::STOP_LIMIT ||
            trade.type == OrderType::TAKE_PROFIT_LIMIT) {
            params["price"] = format_double(trade.price);
        }
        
        if (trade.type == OrderType::STOP_LOSS || 
            trade.type == OrderType::STOP_LIMIT ||
            trade.type == OrderType::TAKE_PROFIT ||
            trade.type == OrderType::TAKE_PROFIT_LIMIT) {
            params["trigger_price"] = format_double(trade.trigger_price);
        }
        
        params["time_in_force"] = time_in_force_to_string(trade.time_in_force);
        
        if (trade.exec_type == ExecType::POST_ONLY) {
            params["exec_inst"] = json::array({"POST_ONLY"});
        }
        
        std::string params_str = params.dump();
        std::string sig = auth_.sign(method, id, params_str, nonce);
        
        return {
            {"id", id},
            {"method", method},
            {"params", params},
            {"sig", sig},
            {"nonce", nonce}
        };
    }

    // ───────────────────────────────────────────────────────────────────
    // Cancel Order Request
    // ───────────────────────────────────────────────────────────────────
    json build_cancel_order(const std::string& instrument_name, 
                            uint64_t order_id = 0,
                            uint64_t client_oid = 0) {
        uint64_t id = next_id();
        uint64_t nonce = AuthHandler::current_timestamp_ms();
        std::string method = "private/cancel-order";
        
        json params = {{"instrument_name", instrument_name}};
        
        if (client_oid != 0) {
            params["client_oid"] = std::to_string(client_oid);
        } else {
            params["order_id"] = std::to_string(order_id);
        }
        
        std::string params_str = params.dump();
        std::string sig = auth_.sign(method, id, params_str, nonce);
        
        return {
            {"id", id},
            {"method", method},
            {"params", params},
            {"sig", sig},
            {"nonce", nonce}
        };
    }

    // ───────────────────────────────────────────────────────────────────
    // Cancel All Orders
    // ───────────────────────────────────────────────────────────────────
    json build_cancel_all_orders(const std::string& instrument_name = "") {
        uint64_t id = next_id();
        uint64_t nonce = AuthHandler::current_timestamp_ms();
        std::string method = "private/cancel-all-orders";
        
        json params = json::object();
        if (!instrument_name.empty()) {
            params["instrument_name"] = instrument_name;
        }
        
        std::string params_str = params.empty() ? "" : params.dump();
        std::string sig = auth_.sign(method, id, params_str, nonce);
        
        return {
            {"id", id},
            {"method", method},
            {"params", params},
            {"sig", sig},
            {"nonce", nonce}
        };
    }

    // ───────────────────────────────────────────────────────────────────
    // Get Open Orders
    // ───────────────────────────────────────────────────────────────────
    json build_get_open_orders(const std::string& instrument_name = "") {
        uint64_t id = next_id();
        uint64_t nonce = AuthHandler::current_timestamp_ms();
        std::string method = "private/get-open-orders";
        
        json params = json::object();
        if (!instrument_name.empty()) {
            params["instrument_name"] = instrument_name;
        }
        
        std::string params_str = params.empty() ? "" : params.dump();
        std::string sig = auth_.sign(method, id, params_str, nonce);
        
        return {
            {"id", id},
            {"method", method},
            {"params", params},
            {"sig", sig},
            {"nonce", nonce}
        };
    }

    // ───────────────────────────────────────────────────────────────────
    // Heartbeat Response
    // ───────────────────────────────────────────────────────────────────
    json build_heartbeat_response(uint64_t request_id) {
        return {
            {"id", request_id},
            {"method", "public/respond-heartbeat"}
        };
    }

    // ───────────────────────────────────────────────────────────────────
    // Subscribe to user events
    // ───────────────────────────────────────────────────────────────────
    json build_subscribe_user_order(const std::string& instrument_name = "") {
        uint64_t id = next_id();
        uint64_t nonce = AuthHandler::current_timestamp_ms();
        std::string method = "subscribe";
        
        json params = {
            {"channels", json::array({
                instrument_name.empty() ? "user.order" : "user.order." + instrument_name
            })}
        };
        
        std::string params_str = params.dump();
        std::string sig = auth_.sign(method, id, params_str, nonce);
        
        return {
            {"id", id},
            {"method", method},
            {"params", params},
            {"sig", sig},
            {"nonce", nonce}
        };
    }

    json build_subscribe_user_trade(const std::string& instrument_name = "") {
        uint64_t id = next_id();
        uint64_t nonce = AuthHandler::current_timestamp_ms();
        std::string method = "subscribe";
        
        json params = {
            {"channels", json::array({
                instrument_name.empty() ? "user.trade" : "user.trade." + instrument_name
            })}
        };
        
        std::string params_str = params.dump();
        std::string sig = auth_.sign(method, id, params_str, nonce);
        
        return {
            {"id", id},
            {"method", method},
            {"params", params},
            {"sig", sig},
            {"nonce", nonce}
        };
    }

private:
    static const char* order_type_to_string(OrderType type) {
        switch (type) {
            case OrderType::LIMIT:             return "LIMIT";
            case OrderType::MARKET:            return "MARKET";
            case OrderType::STOP_LOSS:         return "STOP_LOSS";
            case OrderType::STOP_LIMIT:        return "STOP_LIMIT";
            case OrderType::TAKE_PROFIT:       return "TAKE_PROFIT";
            case OrderType::TAKE_PROFIT_LIMIT: return "TAKE_PROFIT_LIMIT";
            default:                           return "LIMIT";
        }
    }

    static const char* time_in_force_to_string(TimeInForce tif) {
        switch (tif) {
            case TimeInForce::GTC: return "GOOD_TILL_CANCEL";
            case TimeInForce::IOC: return "IMMEDIATE_OR_CANCEL";
            case TimeInForce::FOK: return "FILL_OR_KILL";
            case TimeInForce::GTD: return "GOOD_TILL_DATE";
            default:               return "GOOD_TILL_CANCEL";
        }
    }

    static std::string format_double(double value) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.8f", value);
        std::string s(buf);
        
        size_t dot = s.find('.');
        if (dot != std::string::npos) {
            size_t last = s.find_last_not_of('0');
            if (last != std::string::npos && last > dot) {
                s = s.substr(0, last + 1);
            } else if (last == dot) {
                s = s.substr(0, dot);
            }
        }
        return s;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Response Parser
// ═══════════════════════════════════════════════════════════════════════════

class CryptoComResponseParser {
public:
    static OrderResponse parse_order_response(const json& response) {
        OrderResponse result{};
        
        if (response.contains("code") && response["code"].get<int>() != 0) {
            result.status = OrderStatus::REJECTED;
            result.error_code = response["code"].get<int>();
            if (response.contains("message")) {
                std::string msg = response["message"].get<std::string>();
                std::strncpy(result.error_message, msg.c_str(), 
                            sizeof(result.error_message) - 1);
            }
            return result;
        }
        
        if (response.contains("result")) {
            const auto& res = response["result"];
            
            if (res.contains("client_oid")) {
                auto& val = res["client_oid"];
                result.client_order_id = val.is_string() ? 
                    std::stoull(val.get<std::string>()) : val.get<uint64_t>();
            }
            
            if (res.contains("order_id")) {
                auto& val = res["order_id"];
                result.exchange_order_id = val.is_string() ? 
                    std::stoull(val.get<std::string>()) : val.get<uint64_t>();
            }
            
            if (res.contains("status")) {
                result.status = parse_status(res["status"].get<std::string>());
            }
            
            if (res.contains("cumulative_quantity")) {
                auto& val = res["cumulative_quantity"];
                result.filled_quantity = val.is_string() ? 
                    std::stod(val.get<std::string>()) : val.get<double>();
            }
            
            if (res.contains("remaining_quantity")) {
                auto& val = res["remaining_quantity"];
                result.remaining_quantity = val.is_string() ? 
                    std::stod(val.get<std::string>()) : val.get<double>();
            }
            
            if (res.contains("avg_price")) {
                auto& val = res["avg_price"];
                result.avg_price = val.is_string() ? 
                    std::stod(val.get<std::string>()) : val.get<double>();
            }
            
            if (res.contains("cumulative_fee")) {
                auto& val = res["cumulative_fee"];
                result.fee = val.is_string() ? 
                    std::stod(val.get<std::string>()) : val.get<double>();
            }
            
            if (res.contains("fee_currency")) {
                std::string fc = res["fee_currency"].get<std::string>();
                std::strncpy(result.fee_currency, fc.c_str(), 
                            sizeof(result.fee_currency) - 1);
            }
            
            if (res.contains("update_time")) {
                result.exchange_timestamp_ns = res["update_time"].get<int64_t>() * 1000000;
            }
        }
        
        return result;
    }
    
    static bool is_heartbeat(const json& msg) {
        return msg.contains("method") && msg["method"] == "public/heartbeat";
    }
    
    static uint64_t get_heartbeat_id(const json& msg) {
        return msg.contains("id") ? msg["id"].get<uint64_t>() : 0;
    }

    static bool is_subscription_response(const json& msg) {
        return msg.contains("method") && msg["method"] == "subscribe";
    }

    static bool is_order_update(const json& msg) {
        if (!msg.contains("method")) return false;
        std::string method = msg["method"].get<std::string>();
        return method.find("user.order") != std::string::npos;
    }

    static bool is_trade_update(const json& msg) {
        if (!msg.contains("method")) return false;
        std::string method = msg["method"].get<std::string>();
        return method.find("user.trade") != std::string::npos;
    }

private:
    static OrderStatus parse_status(const std::string& status) {
        if (status == "NEW" || status == "PENDING" || status == "ACTIVE") 
            return OrderStatus::NEW;
        if (status == "FILLED") 
            return OrderStatus::FILLED;
        if (status == "PARTIALLY_FILLED") 
            return OrderStatus::PARTIALLY_FILLED;
        if (status == "CANCELED" || status == "CANCELLED") 
            return OrderStatus::CANCELED;
        if (status == "REJECTED") 
            return OrderStatus::REJECTED;
        if (status == "EXPIRED") 
            return OrderStatus::EXPIRED;
        return OrderStatus::NEW;
    }
};

} // namespace hft
