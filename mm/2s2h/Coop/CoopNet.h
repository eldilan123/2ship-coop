#ifndef COOP_NET_H
#define COOP_NET_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "CoopProtocol.h"

namespace Coop {

constexpr uint32_t kBroadcast = 0;
constexpr uint32_t kHostPlayerId = 1;
constexpr size_t kMaxPlayers = 4;
constexpr uint16_t kDefaultPort = 24242;

enum class Role { None, Host, Client };

enum class SessionState {
    Idle,         // Nothing running
    Connecting,   // Client: resolving / opening the TCP connection
    Handshaking,  // Client: connected, waiting for welcome or reject
    Hosting,      // Host: listening and serving clients
    Connected,    // Client: handshake accepted
    Rejected,     // Client: host refused the handshake (message holds the reason)
    Disconnected, // Session ended after it was established
    Failed,       // Session could not be started (message holds the reason)
};

const char* SessionStateName(SessionState state);

struct PlayerInfo {
    uint32_t id = 0;
    std::string name;
    Protocol::Color color;
    bool isHost = false;
    int32_t rttMs = -1; // Round trip to this player as measured locally; -1 if unknown or not a direct peer
};

struct SessionConfig {
    Role role = Role::None;
    std::string address; // Client only
    uint16_t port = kDefaultPort;
    std::string name;
    Protocol::Color color;
    std::string buildVersion;
    int protocolVersion = Protocol::kProtocolVersion; // Only differs when faked for testing
    int silenceAfterSeconds = 0;                      // Test only: stop heartbeats after this many seconds (0 = never)
    int testNetDelayMs = 0; // Test only: everything sent, and every application message received, waits this long
};

struct SessionSnapshot {
    Role role = Role::None;
    SessionConfig config; // Settings the current (or last) session was started with
    SessionState state = SessionState::Idle;
    std::string message;
    uint32_t localId = 0;
    std::vector<PlayerInfo> players;
};

// Traffic counters for the [Coop] net-stats line, since the previous TakeStats(). Index 0 is JSON (control and
// application messages), 1..kStatsChannels-1 the binary channels.
constexpr size_t kStatsChannels = 8;
struct NetStats {
    double seconds = 0.0;
    uint32_t rttCount = 0; // Pongs received
    int64_t rttSumMs = 0;
    int32_t rttMaxMs = 0;
    uint64_t inBytes[kStatsChannels] = {};
    uint64_t outBytes[kStatsChannels] = {};
    // Longest wait between two consecutive messages of the same channel from the same peer (network thread time)
    int64_t maxGapMs[kStatsChannels] = {};
    uint64_t dropped = 0; // Messages received and thrown away (inbox full, failed checks on the game thread)
};

// A non-control message for the game thread. Control traffic (handshake, ping/pong, player list, bye) never
// reaches this queue: it is handled entirely on the network thread.
struct AppMessage {
    uint32_t from = 0;
    Protocol::Frame frame;
    std::chrono::steady_clock::time_point due = {}; // --coop-test-net-delay: not handed to the game before this
};

// Owns every socket and the network thread. The game thread only talks to it through Start/Stop, the outbox
// (SendJson/SendBinary), the inbox (TakeInbox) and GetSnapshot; it never touches a socket.
class CoopNet {
  public:
    ~CoopNet();

    // Returns false if a session is already running.
    bool Start(const SessionConfig& config);
    // Says goodbye to peers, closes every socket and joins the network thread. Safe to call at any time.
    void Stop();
    bool IsRunning() const {
        return mThreadActive.load();
    }

    SessionSnapshot GetSnapshot() const;

    // `target` is a player id, or kBroadcast. Clients always send to the host.
    void SendJson(uint32_t target, const nlohmann::json& message);
    void SendBinary(uint32_t target, uint8_t channel, const void* data, size_t size);
    std::vector<AppMessage> TakeInbox();
    // Counters since the previous call (then reset). Any thread.
    NetStats TakeStats();
    // A received message was thrown away by the game thread (failed its checks).
    void NoteDropped();

  private:
    using Clock = std::chrono::steady_clock;
    struct Connection;
    struct Outgoing {
        uint32_t target;
        std::string bytes;
    };

    SessionConfig mConfig;
    std::thread mThread;
    std::atomic<bool> mThreadActive = false;
    std::atomic<bool> mStopRequested = false;

    mutable std::mutex mSnapshotMutex;
    SessionSnapshot mSnapshot;

    std::mutex mOutboxMutex;
    std::vector<Outgoing> mOutbox;

    std::mutex mInboxMutex;
    std::vector<AppMessage> mInbox;

    std::mutex mStatsMutex;
    NetStats mStats;
    Clock::time_point mStatsSince = Clock::now();
    std::unordered_map<uint64_t, Clock::time_point> mLastArrival; // (peer << 8 | channel)
    void CountReceived(uint32_t peer, const Protocol::Frame& frame);
    void CountSent(const std::string& bytes);
    void CountRtt(int32_t rttMs);

    // Network thread state
    Clock::time_point mSessionStart;
    bool mSilent = false;
    uint32_t mNextPlayerId = kHostPlayerId + 1;

    void ThreadMain();
    void RunHost();
    void RunClient();

    // Shared by both roles
    bool ReadFrom(Connection& conn, std::vector<Protocol::Frame>& frames);
    void SendRaw(Connection& conn, const std::string& bytes);
    void SendNow(Connection& conn, const std::string& bytes);
    void FlushDelayed(Connection& conn, Clock::time_point now);
    void SendControl(Connection& conn, const nlohmann::json& message);
    bool HandleHeartbeatMessage(Connection& conn, const Protocol::Frame& frame);
    void TickHeartbeat(Connection& conn, Clock::time_point now);
    void UpdateSilence(Clock::time_point now);
    void PushInbox(uint32_t from, Protocol::Frame&& frame);
    std::vector<Outgoing> TakeOutbox();

    // Host only
    void HostHandleHello(Connection& conn, const Protocol::Frame& frame, size_t activePlayers);
    void HostReject(Connection& conn, const std::string& reason);
    std::vector<PlayerInfo> HostBuildPlayerList(const std::vector<std::unique_ptr<Connection>>& conns) const;

    void SetState(SessionState state, const std::string& message = "");
    void SetPlayers(std::vector<PlayerInfo> players, uint32_t localId);
};

} // namespace Coop

#endif // COOP_NET_H
