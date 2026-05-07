#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hft::ipc {

enum class CommandType {
    Subscribe,
    Snapshot,
    Ping,
    Unknown,
};

struct ClientCommand {
    CommandType type{CommandType::Unknown};
    std::vector<std::string> tickers;
};

inline CommandType command_type_from_string(const std::string& value) {
    if (value == "subscribe") {
        return CommandType::Subscribe;
    }
    if (value == "snapshot") {
        return CommandType::Snapshot;
    }
    if (value == "ping") {
        return CommandType::Ping;
    }
    return CommandType::Unknown;
}

inline std::optional<ClientCommand> parse_command_line(const std::string& line,
                                                       std::string* error = nullptr) {
    const nlohmann::json decoded = nlohmann::json::parse(line, nullptr, false);
    if (decoded.is_discarded() || !decoded.is_object()) {
        if (error) {
            *error = "invalid JSON object";
        }
        return std::nullopt;
    }

    const std::string command = decoded.value("cmd", "");
    ClientCommand parsed;
    parsed.type = command_type_from_string(command);
    if (parsed.type == CommandType::Unknown) {
        if (error) {
            *error = "unknown command";
        }
        return std::nullopt;
    }

    if (decoded.contains("ticker") && decoded["ticker"].is_string()) {
        parsed.tickers.push_back(decoded["ticker"].get<std::string>());
    }

    if (decoded.contains("tickers") && decoded["tickers"].is_array()) {
        for (const auto& ticker : decoded["tickers"]) {
            if (ticker.is_string()) {
                parsed.tickers.push_back(ticker.get<std::string>());
            }
        }
    }

    if ((parsed.type == CommandType::Subscribe || parsed.type == CommandType::Snapshot) &&
        parsed.tickers.empty()) {
        if (error) {
            *error = "ticker is required";
        }
        return std::nullopt;
    }

    return parsed;
}

inline std::string make_ok_response(nlohmann::json result = nlohmann::json::object()) {
    return nlohmann::json{{"ok", true}, {"result", std::move(result)}}.dump() + "\n";
}

inline std::string make_error_response(const std::string& message) {
    return nlohmann::json{{"ok", false}, {"error", message}}.dump() + "\n";
}

}  // namespace hft::ipc
