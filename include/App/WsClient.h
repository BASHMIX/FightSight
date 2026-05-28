#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fightsight {

// Non-blocking WebSocket client tailored for Streamer.bot.
//
// Lifecycle contract (very important - websocketpp is strict here):
//   - Start(url) does the ONE-TIME endpoint init (init_asio, start_perpetual,
//     handlers, io thread spawn) and kicks off the first connection attempt.
//     Calling Start a second time with a new URL is safe; it transparently
//     delegates to Reconnect(url) instead of re-initing Asio.
//   - Reconnect(url) is the proper API for the UI "Apply + Reconnect" button.
//     It updates the URL and closes the current connection, letting the
//     auto-reconnect handler chain establish a new connection to the new URL.
//   - Stop() does the FULL teardown and is only valid on app exit. After
//     Stop() the object is no longer usable - destroy and recreate if you
//     need to spin up another WsClient.
//
// The io thread is kept alive across connection drops by start_perpetual()'s
// internal work guard, so we never have to recreate it or call io::restart().
class WsClient {
public:
    WsClient();
    ~WsClient();

    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    // Idempotent w.r.t. init: first call sets up Asio + handlers + thread;
    // subsequent calls just forward to Reconnect(url).
    void Start(const std::string& url);

    // Switch URL + force a reconnect without tearing down Asio/thread.
    // Safe to call before Start (will do Start). Safe to call repeatedly.
    void Reconnect(const std::string& newUrl);

    // App-exit only. Releases the work guard, closes the connection, joins
    // the io thread. NOT followed by another Start() in the same lifetime.
    void Stop();

    // Thread-safe enqueue. Drop-with-counter if disconnected and queue full.
    void Send(std::string payload);

    bool        IsConnected()  const { return m_connected.load(); }
    bool        IsRunning()    const { return m_running.load();   }
    size_t      QueueSize()    const;
    uint64_t    TotalSent()    const { return m_totalSent.load();    }
    uint64_t    TotalDropped() const { return m_totalDropped.load(); }
    std::string Url()          const;
    std::string LastError()    const;

    // ----- Action discovery (Phase 5) ---------------------------------
    // Auto-fetched from Streamer.bot via "GetActions" on every successful
    // connect. RefreshActions() forces a re-fetch (e.g. user added actions
    // after FightSight launched). All accessors are thread-safe.
    std::vector<std::string> GetAvailableActions() const;
    size_t                   AvailableActionsCount() const;
    void                     RefreshActions();

    static constexpr size_t kMaxQueueWhileDisconnected = 100;

private:
    // All of these run inside the Asio io thread context.
    void AttemptConnect();
    void ScheduleReconnect();
    void DrainOutbox();
    void SetupHandlers();
    void HandleIncomingMessage(const std::string& payload);
    void SetError(const std::string& e);
    std::string CurrentUrl() const;
    void        SetCurrentUrl(const std::string& url);

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    // Lifecycle flags.
    std::atomic<bool>     m_inited{false};            // init_asio() called
    std::atomic<bool>     m_running{false};           // io thread alive
    std::atomic<bool>     m_suppressReconnect{false}; // skip auto-reconnect (used during Stop)
    std::atomic<bool>     m_connected{false};

    // Stats.
    std::atomic<uint64_t> m_totalSent{0};
    std::atomic<uint64_t> m_totalDropped{0};

    // URL is mutated from any thread (UI Reconnect), read from io thread
    // (AttemptConnect). Keep it under a mutex.
    mutable std::mutex    m_urlMutex;
    std::string           m_url;

    mutable std::mutex    m_errorMutex;
    std::string           m_lastError;

    // Streamer.bot action catalogue. Written from the io thread (in the
    // message handler); read from any thread via GetAvailableActions().
    mutable std::mutex             m_actionsMutex;
    std::vector<std::string>       m_availableActions;
};

} // namespace fightsight
