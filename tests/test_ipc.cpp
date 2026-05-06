#include "ipc/client_protocol.hpp"
#include "ipc/unix_domain_server.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using hft::ipc::CommandType;
using hft::ipc::UnixDomainCommandServer;
using hft::ipc::parse_command_line;

std::string request_once(const std::filesystem::path& socket_path, const std::string& request) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return {};
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);

    for (int attempt = 0; attempt < 50; ++attempt) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const std::string line = request + "\n";
    ::write(fd, line.data(), line.size());

    std::string response;
    char buffer[512];
    while (response.find('\n') == std::string::npos) {
        const ssize_t read_count = ::read(fd, buffer, sizeof(buffer));
        if (read_count <= 0) {
            break;
        }
        response.append(buffer, static_cast<size_t>(read_count));
    }
    ::close(fd);
    return response;
}

TEST(IpcProtocol, ParsesSubscribeSnapshotAndErrors) {
    std::string error;
    const auto subscribe =
        parse_command_line(R"json({"cmd":"subscribe","tickers":["BTC_USDT","ETH_USDT"]})json", &error);
    ASSERT_TRUE(subscribe.has_value());
    EXPECT_EQ(subscribe->type, CommandType::Subscribe);
    ASSERT_EQ(subscribe->tickers.size(), 2u);
    EXPECT_EQ(subscribe->tickers[0], "BTC_USDT");

    const auto snapshot = parse_command_line(R"json({"cmd":"snapshot","ticker":"BTC_USDT"})json", &error);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->type, CommandType::Snapshot);
    ASSERT_EQ(snapshot->tickers.size(), 1u);
    EXPECT_EQ(snapshot->tickers[0], "BTC_USDT");

    EXPECT_FALSE(parse_command_line(R"json({"cmd":"snapshot"})json", &error).has_value());
    EXPECT_EQ(error, "ticker is required");
}

TEST(UnixDomainCommandServer, HandlesPingSubscribeAndSnapshot) {
    const auto socket_path = std::filesystem::temp_directory_path() /
                             ("crypto_hft_ipc_test_" + std::to_string(::getpid()) + ".sock");
    std::filesystem::remove(socket_path);

    UnixDomainCommandServer server(socket_path.string());
    server.set_subscribe_callback([](const std::vector<std::string>& tickers) {
        return nlohmann::json{{"subscribed", tickers}};
    });
    server.set_snapshot_callback([](const std::string& ticker) {
        return nlohmann::json{{"ticker", ticker}, {"bid", 100.0}, {"ask", 100.5}};
    });
    server.start();

    const auto ping = nlohmann::json::parse(request_once(socket_path, R"json({"cmd":"ping"})json"));
    EXPECT_TRUE(ping.value("ok", false));
    EXPECT_TRUE(ping["result"].value("pong", false));

    const auto subscribe = nlohmann::json::parse(
        request_once(socket_path, R"json({"cmd":"subscribe","ticker":"BTC_USDT"})json"));
    EXPECT_TRUE(subscribe.value("ok", false));
    ASSERT_EQ(subscribe["result"]["subscribed"].size(), 1u);
    EXPECT_EQ(subscribe["result"]["subscribed"][0], "BTC_USDT");

    const auto snapshot =
        nlohmann::json::parse(request_once(socket_path, R"json({"cmd":"snapshot","ticker":"BTC_USDT"})json"));
    EXPECT_TRUE(snapshot.value("ok", false));
    EXPECT_EQ(snapshot["result"].value("ticker", ""), "BTC_USDT");
    EXPECT_DOUBLE_EQ(snapshot["result"].value("bid", 0.0), 100.0);

    server.stop();
    std::filesystem::remove(socket_path);
}

}  // namespace
