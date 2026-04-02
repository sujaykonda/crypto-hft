#pragma once

#include <string>
#include <cstdint>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <openssl/hmac.h>
#include <openssl/sha.h>

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
