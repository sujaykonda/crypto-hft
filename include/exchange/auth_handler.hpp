#pragma once

#include <string>
#include <cstdint>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <utility>
#include <vector>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <nlohmann/json.hpp>

namespace hft {

// ═══════════════════════════════════════════════════════════════════════════
// Authentication Handler - HMAC-SHA256 Signature Generation for Crypto.com
// ═══════════════════════════════════════════════════════════════════════════

class AuthHandler {
    std::string api_key_;
    std::string api_secret_;

public:
    AuthHandler() = default;
    
    AuthHandler(const std::string& api_key, const std::string& api_secret)
        : api_key_(api_key), api_secret_(api_secret) {}

    AuthHandler(std::string&& api_key, std::string&& api_secret)
        : api_key_(std::move(api_key)), api_secret_(std::move(api_secret)) {}

    // ───────────────────────────────────────────────────────────────────
    // Generate signature for request
    // Signature = HMAC-SHA256(
    //   method + id + api_key + params_string + nonce,
    //   api_secret
    // )
    // ───────────────────────────────────────────────────────────────────
    std::string sign(const std::string& method, 
                     uint64_t id, 
                     const std::string& params_string, 
                     uint64_t nonce) const {
        
        std::string payload = method + 
                             std::to_string(id) + 
                             api_key_ + 
                             params_string + 
                             std::to_string(nonce);
        
        return hmac_sha256(payload);
    }

    std::string sign(const std::string& method,
                     uint64_t id,
                     const nlohmann::json& params,
                     uint64_t nonce) const {
        return sign(method, id, parameter_string(params), nonce);
    }

    static std::string parameter_string(const nlohmann::json& params) {
        if (params.is_null()) {
            return "";
        }

        return append_parameter_string(params, 0);
    }

    // ───────────────────────────────────────────────────────────────────
    // Get current timestamp in milliseconds
    // ───────────────────────────────────────────────────────────────────
    static uint64_t current_timestamp_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    // ───────────────────────────────────────────────────────────────────
    // Accessors
    // ───────────────────────────────────────────────────────────────────
    const std::string& api_key() const { return api_key_; }
    bool is_configured() const { return !api_key_.empty() && !api_secret_.empty(); }

private:
    static std::string scalar_to_string(const nlohmann::json& value) {
        if (value.is_string()) {
            return value.get<std::string>();
        }
        if (value.is_null()) {
            return "null";
        }
        if (value.is_boolean()) {
            return value.get<bool>() ? "true" : "false";
        }
        return value.dump();
    }

    static std::string append_parameter_string(const nlohmann::json& value, int level) {
        constexpr int kMaxNestedLevel = 3;
        if (level >= kMaxNestedLevel || value.is_primitive()) {
            return scalar_to_string(value);
        }

        std::string result;
        if (value.is_array()) {
            for (const auto& item : value) {
                result += append_parameter_string(item, level + 1);
            }
            return result;
        }

        if (!value.is_object()) {
            return scalar_to_string(value);
        }

        std::vector<std::string> keys;
        keys.reserve(value.size());
        for (auto it = value.begin(); it != value.end(); ++it) {
            keys.push_back(it.key());
        }
        std::sort(keys.begin(), keys.end());

        for (const std::string& key : keys) {
            result += key;
            result += append_parameter_string(value.at(key), level + 1);
        }

        return result;
    }

    std::string hmac_sha256(const std::string& data) const {
        unsigned char digest[SHA256_DIGEST_LENGTH];
        unsigned int digest_len = SHA256_DIGEST_LENGTH;
        
        HMAC(EVP_sha256(), 
             api_secret_.data(), 
             static_cast<int>(api_secret_.size()),
             reinterpret_cast<const unsigned char*>(data.data()), 
             data.size(),
             digest, 
             &digest_len);
        
        // Convert to lowercase hex string
        std::ostringstream ss;
        ss << std::hex << std::setfill('0');
        for (unsigned int i = 0; i < digest_len; ++i) {
            ss << std::setw(2) << static_cast<int>(digest[i]);
        }
        return ss.str();
    }
};

} // namespace hft
