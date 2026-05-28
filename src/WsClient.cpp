#include "App/WsClient.h"

// ---------------------------------------------------------------------------
// websocketpp + standalone Asio. Quiet the deprecation noise from MSVC.
// ---------------------------------------------------------------------------
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4267) // size_t -> int
#pragma warning(disable: 4996) // deprecated std::* bindings inside websocketpp
#endif

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#ifndef _WEBSOCKETPP_CPP11_STL_
#define _WEBSOCKETPP_CPP11_STL_
#endif

#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <thread>

namespace fightsight {

using ws_client_t = websocketpp::client<websocketpp::config::asio_client>;

namespace {
    // The opaque id we use when asking Streamer.bot for its action list.
    // Echoed back in the response so we can distinguish from other replies.
    constexpr const char* kFetchActionsId = "fightsight_fetch_actions";
    constexpr const char* kFetchActionsPayload =
        R"({"request":"GetActions","id":"fightsight_fetch_actions"})";
}

struct WsClient::Impl {
    ws_client_t                       client;
    std::thread                       thread;
    websocketpp::connection_hdl       hdl;      // only touched on io thread
    std::deque<std::string>           outbox;
    mutable std::mutex                outboxMutex;
    std::atomic<bool>                 connecting{false};
};

// ---------------------------------------------------------------------------
WsClient::WsClient() : m_impl(std::make_unique<Impl>()) {}

WsClient::~WsClient() { Stop(); }

// ---------------------------------------------------------------------------
// One-time setup. Subsequent calls forward to Reconnect.
// ---------------------------------------------------------------------------
void WsClient::Start(const std::string& url) {
    if (m_inited.load()) {
        // Endpoint is already initialized and io thread is running.
        // Just hot-swap the URL and trigger a reconnect.
        Reconnect(url);
        return;
    }

    SetCurrentUrl(url);
    m_suppressReconnect.store(false);

    try {
        // Quiet logs - we surface errors via LastError() ourselves.
        m_impl->client.clear_access_channels(websocketpp::log::alevel::all);
        m_impl->client.clear_error_channels(websocketpp::log::elevel::all);

        // ---- ONE-TIME endpoint init ----
        m_impl->client.init_asio();
        m_impl->client.start_perpetual();   // <- work guard: io thread stays alive
        SetupHandlers();

        m_inited.store(true);
        m_running.store(true);

        // Spawn the io thread BEFORE posting the first connect, so the
        // post is guaranteed to be picked up.
        m_impl->thread = std::thread([this] {
            try { m_impl->client.run(); }
            catch (const std::exception& e) { SetError(e.what()); }
            catch (...) {}
        });

        // Hop into the io thread for the first connection attempt so all
        // connect/close calls live on a single thread.
        m_impl->client.get_io_service().post([this] { AttemptConnect(); });
    } catch (const std::exception& e) {
        SetError(e.what());
    }
}

// ---------------------------------------------------------------------------
// URL hot-swap + force reconnect. Safe to call at any time.
// ---------------------------------------------------------------------------
void WsClient::Reconnect(const std::string& newUrl) {
    if (!m_inited.load()) {
        // Never started - treat as a deferred Start.
        Start(newUrl);
        return;
    }

    SetCurrentUrl(newUrl);
    m_suppressReconnect.store(false);

    try {
        m_impl->client.get_io_service().post([this] {
            if (!m_running.load()) return;

            if (m_connected.load()) {
                // Close the live connection. The close_handler will fire
                // asynchronously, set m_connected=false, and schedule a
                // reconnect that will use the URL we just stored.
                try {
                    m_impl->client.close(
                        m_impl->hdl,
                        websocketpp::close::status::normal,
                        "reconnect requested");
                } catch (...) {}
            } else {
                // Not connected; the connecting/timer guards in
                // AttemptConnect prevent duplicate attempts.
                AttemptConnect();
            }
        });
    } catch (...) {}
}

// ---------------------------------------------------------------------------
// Full teardown. App-exit only - do NOT call Start() afterwards.
// ---------------------------------------------------------------------------
void WsClient::Stop() {
    if (!m_inited.load()) return;
    if (!m_running.load()) return;

    m_suppressReconnect.store(true);
    m_running.store(false);

    try {
        // Release the work guard so run() can return once the close
        // round-trip finishes.
        m_impl->client.stop_perpetual();

        if (m_connected.load()) {
            try {
                m_impl->client.close(
                    m_impl->hdl,
                    websocketpp::close::status::going_away,
                    "shutdown");
            } catch (...) {}
        }
    } catch (...) {}

    if (m_impl->thread.joinable()) m_impl->thread.join();
    m_connected.store(false);
    // Note: m_inited stays true. A second Start() on the same instance
    // is unsupported (websocketpp endpoints cannot be re-init'd). Destroy
    // and recreate the WsClient if you need a fresh one.
}

// ---------------------------------------------------------------------------
void WsClient::Send(std::string payload) {
    if (!m_inited.load()) {
        m_totalDropped.fetch_add(1);
        return;
    }
    const bool wasConnected = m_connected.load();
    {
        std::lock_guard<std::mutex> lk(m_impl->outboxMutex);
        if (!wasConnected &&
            m_impl->outbox.size() >= kMaxQueueWhileDisconnected) {
            m_impl->outbox.pop_front();
            m_totalDropped.fetch_add(1);
        }
        m_impl->outbox.push_back(std::move(payload));
    }
    if (wasConnected) {
        try {
            m_impl->client.get_io_service().post([this] { DrainOutbox(); });
        } catch (...) {}
    }
}

// ---------------------------------------------------------------------------
size_t WsClient::QueueSize() const {
    std::lock_guard<std::mutex> lk(m_impl->outboxMutex);
    return m_impl->outbox.size();
}

std::string WsClient::LastError() const {
    std::lock_guard<std::mutex> lk(m_errorMutex);
    return m_lastError;
}

std::string WsClient::Url() const {
    return CurrentUrl();
}

std::vector<std::string> WsClient::GetAvailableActions() const {
    std::lock_guard<std::mutex> lk(m_actionsMutex);
    return m_availableActions;
}

size_t WsClient::AvailableActionsCount() const {
    std::lock_guard<std::mutex> lk(m_actionsMutex);
    return m_availableActions.size();
}

void WsClient::RefreshActions() {
    if (!IsConnected()) return;
    Send(kFetchActionsPayload);
}

void WsClient::SetError(const std::string& e) {
    std::lock_guard<std::mutex> lk(m_errorMutex);
    m_lastError = e;
}

std::string WsClient::CurrentUrl() const {
    std::lock_guard<std::mutex> lk(m_urlMutex);
    return m_url;
}

void WsClient::SetCurrentUrl(const std::string& url) {
    std::lock_guard<std::mutex> lk(m_urlMutex);
    m_url = url;
}

// ---------------------------------------------------------------------------
// Handler setup. Called exactly once from Start().
// ---------------------------------------------------------------------------
void WsClient::SetupHandlers() {
    m_impl->client.set_open_handler(
        [this](websocketpp::connection_hdl h) {
            m_impl->hdl = h;
            m_connected.store(true);
            m_impl->connecting.store(false);
            SetError({});
            DrainOutbox();

            // Auto-discover Streamer.bot actions every successful connect.
            // We're already on the io thread; the message lands in the
            // outbox and DrainOutbox sends it immediately.
            {
                std::lock_guard<std::mutex> lk(m_impl->outboxMutex);
                m_impl->outbox.push_back(kFetchActionsPayload);
            }
            DrainOutbox();
        });

    m_impl->client.set_close_handler(
        [this](websocketpp::connection_hdl) {
            m_connected.store(false);
            ScheduleReconnect();
        });

    m_impl->client.set_fail_handler(
        [this](websocketpp::connection_hdl h) {
            m_connected.store(false);
            m_impl->connecting.store(false);
            try {
                auto con = m_impl->client.get_con_from_hdl(h);
                SetError(con->get_ec().message());
            } catch (...) {}
            ScheduleReconnect();
        });

    // Rx: parse incoming frames. Right now we only care about the
    // GetActions response (matched by echoed "id"), but the dispatcher
    // can grow to handle other Streamer.bot reply types later.
    m_impl->client.set_message_handler(
        [this](websocketpp::connection_hdl,
               ws_client_t::message_ptr msg) {
            HandleIncomingMessage(msg->get_payload());
        });
}

void WsClient::HandleIncomingMessage(const std::string& payload) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(payload);
    } catch (const std::exception&) {
        return; // ignore non-JSON noise
    }

    // GetActions response - update the cached action catalogue.
    const std::string id = j.value("id", std::string{});
    if (id == kFetchActionsId && j.contains("actions")
        && j["actions"].is_array()) {
        std::vector<std::string> names;
        names.reserve(j["actions"].size());
        for (const auto& act : j["actions"]) {
            if (!act.is_object()) continue;
            if (!act.contains("name")) continue;
            const std::string n = act["name"].get<std::string>();
            if (!n.empty()) names.push_back(n);
        }
        std::sort(names.begin(), names.end(),
            [](const std::string& a, const std::string& b) {
                // case-insensitive compare for nicer alphabetic display
                return std::lexicographical_compare(
                    a.begin(), a.end(), b.begin(), b.end(),
                    [](char x, char y) {
                        return std::tolower((unsigned char)x)
                             < std::tolower((unsigned char)y);
                    });
            });
        {
            std::lock_guard<std::mutex> lk(m_actionsMutex);
            m_availableActions = std::move(names);
        }
    }
    // Other replies (DoAction acknowledgements, Hello frames, etc.) are
    // intentionally ignored - they don't affect FightSight's state.
}

// ---------------------------------------------------------------------------
// All three helpers below run inside the Asio io thread.
// ---------------------------------------------------------------------------
void WsClient::AttemptConnect() {
    if (!m_running.load())            return;
    if (m_connected.load())           return;  // already up
    if (m_impl->connecting.exchange(true)) return; // attempt in flight

    const std::string url = CurrentUrl();
    if (url.empty()) {
        m_impl->connecting.store(false);
        SetError("empty URL");
        return;
    }

    websocketpp::lib::error_code ec;
    auto con = m_impl->client.get_connection(url, ec);
    if (ec) {
        m_impl->connecting.store(false);
        SetError(ec.message());
        ScheduleReconnect();
        return;
    }
    try {
        m_impl->client.connect(con);
        // connecting stays true until on_open / on_fail flips it.
    } catch (const std::exception& e) {
        m_impl->connecting.store(false);
        SetError(e.what());
        ScheduleReconnect();
    }
}

void WsClient::ScheduleReconnect() {
    if (!m_running.load())           return;
    if (m_suppressReconnect.load())  return;

    try {
        auto timer = std::make_shared<asio::steady_timer>(
            m_impl->client.get_io_service());
        timer->expires_after(std::chrono::seconds(1));
        timer->async_wait(
            [this, timer](const asio::error_code& ec) {
                if (ec)                          return; // cancelled
                if (!m_running.load())           return;
                if (m_suppressReconnect.load())  return;
                if (m_connected.load())          return;
                if (m_impl->connecting.load())   return;
                AttemptConnect();
            });
    } catch (...) {}
}

void WsClient::DrainOutbox() {
    while (m_connected.load()) {
        std::string msg;
        {
            std::lock_guard<std::mutex> lk(m_impl->outboxMutex);
            if (m_impl->outbox.empty()) return;
            msg = std::move(m_impl->outbox.front());
            m_impl->outbox.pop_front();
        }
        try {
            m_impl->client.send(m_impl->hdl, msg,
                                websocketpp::frame::opcode::text);
            m_totalSent.fetch_add(1);
        } catch (const std::exception& e) {
            SetError(e.what());
            // Requeue and bail; if the underlying failure was fatal,
            // close/fail handlers will fire and ScheduleReconnect.
            std::lock_guard<std::mutex> lk(m_impl->outboxMutex);
            m_impl->outbox.push_front(std::move(msg));
            return;
        }
    }
}

} // namespace fightsight
