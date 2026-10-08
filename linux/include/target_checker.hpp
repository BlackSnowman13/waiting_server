#pragma once
#include <asio.hpp>
#include <string>
#include <memory>
#include <chrono>
#include <atomic>
#include <functional>
#include <cstdio>
#include "config.hpp"
#include "wol.hpp"

namespace waiting_server {

class TargetChecker : public std::enable_shared_from_this<TargetChecker> {
public:
    using StatusCallback = std::function<void(bool is_online)>;

    TargetChecker(asio::io_context& io_ctx, const Config& config)
        : io_ctx_(io_ctx)
        , config_(config)
        , poll_timer_(io_ctx)
        , check_timer_(io_ctx)
        , resolver_(io_ctx)
        , is_online_(false)
        , last_wol_time_(std::chrono::steady_clock::time_point::min())
    {}

    void set_status_callback(StatusCallback cb) {
        status_cb_ = std::move(cb);
    }

    bool is_online() const {
        return is_online_.load();
    }

    void start() {
        check_now();
    }

    void stop() {
        poll_timer_.cancel();
        check_timer_.cancel();
        if (check_socket_) {
            std::error_code ec;
            check_socket_->close(ec);
        }
    }

    void trigger_immediate_check() {
        poll_timer_.cancel();
        check_now();
    }

    void trigger_wol() {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_wol_time_).count() >= 10) {
            last_wol_time_ = now;
            send_wake_on_lan(io_ctx_, config_.target_mac);
        }
    }

private:
    void schedule_poll() {
        poll_timer_.expires_after(std::chrono::seconds(config_.poll_interval_sec));
        auto self = shared_from_this();
        poll_timer_.async_wait([this, self](const std::error_code& ec) {
            if (!ec) {
                check_now();
            }
        });
    }

    void check_now() {
        check_socket_ = std::make_unique<asio::ip::tcp::socket>(io_ctx_);
        auto self = shared_from_this();

        resolver_.async_resolve(
            config_.target_host, std::to_string(config_.target_port),
            [this, self](const std::error_code& ec, asio::ip::tcp::resolver::results_type results) {
                if (ec || results.empty()) {
                    update_status(false);
                    schedule_poll();
                    return;
                }

                // Set 1.5s timeout for connection attempt
                check_timer_.expires_after(std::chrono::milliseconds(1500));
                check_timer_.async_wait([this, self](const std::error_code& timer_ec) {
                    if (!timer_ec && check_socket_ && check_socket_->is_open()) {
                        std::error_code close_ec;
                        check_socket_->close(close_ec);
                    }
                });

                asio::async_connect(
                    *check_socket_, results,
                    [this, self](const std::error_code& conn_ec, const asio::ip::tcp::endpoint&) {
                        check_timer_.cancel();
                        bool succeeded = !conn_ec;
                        if (check_socket_) {
                            std::error_code close_ec;
                            check_socket_->close(close_ec);
                        }
                        update_status(succeeded);
                        schedule_poll();
                    }
                );
            }
        );
    }

    void update_status(bool online) {
        bool prev = is_online_.exchange(online);
        if (prev != online) {
            if (online) {
                std::printf("\033[1;32m[TARGET]\033[0m Primary Minecraft server is now ONLINE (%s:%u)\n",
                            config_.target_host.c_str(), config_.target_port);
            } else {
                std::printf("\033[1;33m[TARGET]\033[0m Primary Minecraft server is now OFFLINE (%s:%u)\n",
                            config_.target_host.c_str(), config_.target_port);
            }
            if (status_cb_) {
                status_cb_(online);
            }
        }
    }

    asio::io_context& io_ctx_;
    const Config& config_;
    asio::steady_timer poll_timer_;
    asio::steady_timer check_timer_;
    asio::ip::tcp::resolver resolver_;
    std::unique_ptr<asio::ip::tcp::socket> check_socket_;
    std::atomic<bool> is_online_;
    std::chrono::steady_clock::time_point last_wol_time_;
    StatusCallback status_cb_;
};

} // namespace waiting_server
