// The project's precompiled header already brings windows.h, which includes the old winsock.h; winsock2.h would
// clash with it. winsock.h has everything needed here (setsockopt, getsockopt, TCP_NODELAY).
#ifdef _WIN32
#include <winsock.h>
using CoopRawSocket = SOCKET;
using socklen_t = int;
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
using CoopRawSocket = int;
#endif

#include "CoopNet.h"

#include <algorithm>
#include <SDL2/SDL_net.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>

#define COOP_INFO(...) SPDLOG_INFO("[Coop] " __VA_ARGS__)
#define COOP_WARN(...) SPDLOG_WARN("[Coop] " __VA_ARGS__)

// TODO (milestone 3): the network thread polls with SDLNet_CheckSockets every kPollIntervalMs, so an outgoing
// message can wait up to that long in the outbox. Player states (20 Hz) tolerate it thanks to the interpolation
// delay; for snapshots of the world, wake the thread when the outbox is filled, or move that channel to UDP.
// Also note that SDLNet_TCP_Open (client connect) and SDLNet_TCP_Send block, so a peer that stops reading or an
// unreachable address can stall this thread; acceptable for now, revisit with non-blocking sockets.

// SDL_net does not expose socket options. SDLNet_TCP_Open already disables Nagle, but SDLNet_TCP_Accept does not, so
// the host's connections would batch small packets. The raw socket is read from SDL_net's private struct, whose
// layout is only valid for this exact version (SDLnetTCP.c in SDL_net 2.2.x).
static_assert(SDL_NET_MAJOR_VERSION == 2 && SDL_NET_MINOR_VERSION == 2,
              "Check struct _TCPsocket in SDLnetTCP.c before changing the SDL_net version");
struct SdlNetTcpSocketMirror {
    int ready;
    CoopRawSocket channel;
    IPaddress remoteAddress;
    IPaddress localAddress;
    int sflag;
};

namespace Coop {

namespace {

constexpr uint32_t kPollIntervalMs = 50;
constexpr auto kHeartbeatInterval = std::chrono::seconds(1);
constexpr auto kHeartbeatTimeout = std::chrono::seconds(5);
constexpr auto kHandshakeTimeout = std::chrono::seconds(5);
constexpr size_t kMaxConnections = 8; // Includes connections still in the handshake
constexpr size_t kMaxInboxMessages = 10000;
constexpr size_t kMaxListedPlayers = 16;

// Control messages are consumed by the network thread and may not be sent by the game.
bool IsControlType(const std::string& type) {
    return type == "hello" || type == "welcome" || type == "reject" || type == "players" || type == "ping" ||
           type == "pong" || type == "bye";
}

// Untrusted strings from the wire are cut and stripped before they reach the log or the UI.
std::string Printable(const std::string& text, size_t maxLength) {
    std::string out;
    for (unsigned char c : text) {
        if (out.size() >= maxLength) {
            out += "...";
            break;
        }
        out.push_back(c >= 0x20 && c != 0x7F ? static_cast<char>(c) : '?');
    }
    return out;
}

std::string PeerAddress(TCPsocket socket) {
    IPaddress* ip = SDLNet_TCP_GetPeerAddress(socket);
    if (ip == nullptr) {
        return "unknown";
    }
    const uint32_t host = SDLNet_Read32(&ip->host);
    return fmt::format("{}.{}.{}.{}:{}", (host >> 24) & 0xFF, (host >> 16) & 0xFF, (host >> 8) & 0xFF, host & 0xFF,
                       SDLNet_Read16(&ip->port));
}

// Turns Nagle off on `socket` and reads it back, so the log shows whether it really is off.
bool EnableNoDelay(TCPsocket socket) {
    const CoopRawSocket raw = reinterpret_cast<SdlNetTcpSocketMirror*>(socket)->channel;
    int yes = 1;
    setsockopt(raw, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
    int value = 0;
    socklen_t length = sizeof(value);
    if (getsockopt(raw, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&value), &length) != 0) {
        return false;
    }
    return value != 0;
}

std::string DescribePlayers(const std::vector<PlayerInfo>& players, uint32_t localId) {
    std::string out;
    for (const auto& player : players) {
        if (!out.empty()) {
            out += ", ";
        }
        out += fmt::format("'{}' (id {}{}{})", player.name, player.id, player.isHost ? ", host" : "",
                           player.id == localId ? ", you" : "");
    }
    return out;
}

nlohmann::json PlayersMessage(const std::vector<PlayerInfo>& players) {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& player : players) {
        list.push_back({ { "id", player.id },
                         { "name", player.name },
                         { "color", Protocol::ColorToJson(player.color) },
                         { "host", player.isHost } });
    }
    return { { "type", "players" }, { "players", list } };
}

} // namespace

struct CoopNet::Connection {
    TCPsocket socket = nullptr;
    std::string peer;
    Protocol::FrameDecoder decoder;
    bool active = false; // Handshake completed
    bool closed = false;
    std::string closeReason;
    bool loggedFirstPong = false;
    uint32_t playerId = 0;
    std::string name;
    Protocol::Color color;
    Clock::time_point openedAt;
    Clock::time_point lastReceived;
    Clock::time_point lastPingSent;
    int32_t rttMs = -1;
};

const char* SessionStateName(SessionState state) {
    switch (state) {
        case SessionState::Idle:
            return "Idle";
        case SessionState::Connecting:
            return "Connecting";
        case SessionState::Handshaking:
            return "Handshaking";
        case SessionState::Hosting:
            return "Hosting";
        case SessionState::Connected:
            return "Connected";
        case SessionState::Rejected:
            return "Rejected";
        case SessionState::Disconnected:
            return "Disconnected";
        case SessionState::Failed:
            return "Failed";
    }
    return "Unknown";
}

CoopNet::~CoopNet() {
    Stop();
}

bool CoopNet::Start(const SessionConfig& config) {
    if (mThreadActive.load()) {
        return false;
    }
    if (mThread.joinable()) {
        mThread.join();
    }

    mConfig = config;
    mStopRequested = false;
    {
        std::lock_guard<std::mutex> lock(mOutboxMutex);
        mOutbox.clear();
    }
    {
        std::lock_guard<std::mutex> lock(mInboxMutex);
        mInbox.clear();
    }
    {
        std::lock_guard<std::mutex> lock(mSnapshotMutex);
        mSnapshot = {};
        mSnapshot.role = config.role;
        mSnapshot.config = config;
        mSnapshot.state = SessionState::Connecting;
    }

    mThreadActive = true;
    mThread = std::thread(&CoopNet::ThreadMain, this);
    return true;
}

void CoopNet::Stop() {
    mStopRequested = true;
    if (mThread.joinable()) {
        mThread.join();
    }
}

SessionSnapshot CoopNet::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mSnapshotMutex);
    return mSnapshot;
}

void CoopNet::SendJson(uint32_t target, const nlohmann::json& message) {
    const std::string type = message.is_object() ? Protocol::GetString(message, "type") : "";
    if (type.empty() || IsControlType(type)) {
        SPDLOG_ERROR("[Coop] Refusing to send message with type '{}': empty or reserved for control", type);
        return;
    }
    if (!mThreadActive.load()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mOutboxMutex);
    mOutbox.push_back({ target, Protocol::EncodeJson(message) });
}

void CoopNet::SendBinary(uint32_t target, uint8_t channel, const void* data, size_t size) {
    if (size > Protocol::kMaxBinarySize) {
        SPDLOG_ERROR("[Coop] Refusing to send {} byte binary message on channel {}: limit is {} bytes", size, channel,
                     Protocol::kMaxBinarySize);
        return;
    }
    if (!mThreadActive.load()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mOutboxMutex);
    mOutbox.push_back({ target, Protocol::EncodeBinary(channel, data, size) });
}

std::vector<AppMessage> CoopNet::TakeInbox() {
    std::vector<AppMessage> messages;
    std::lock_guard<std::mutex> lock(mInboxMutex);
    messages.swap(mInbox);
    return messages;
}

std::vector<CoopNet::Outgoing> CoopNet::TakeOutbox() {
    std::vector<Outgoing> messages;
    std::lock_guard<std::mutex> lock(mOutboxMutex);
    messages.swap(mOutbox);
    return messages;
}

void CoopNet::PushInbox(uint32_t from, Protocol::Frame&& frame) {
    std::lock_guard<std::mutex> lock(mInboxMutex);
    if (mInbox.size() >= kMaxInboxMessages) {
        COOP_WARN("Inbox full ({} messages), dropping a message from player {}", mInbox.size(), from);
        return;
    }
    mInbox.push_back({ from, std::move(frame) });
}

void CoopNet::SetState(SessionState state, const std::string& message) {
    std::lock_guard<std::mutex> lock(mSnapshotMutex);
    mSnapshot.state = state;
    mSnapshot.message = message;
}

void CoopNet::SetPlayers(std::vector<PlayerInfo> players, uint32_t localId) {
    std::lock_guard<std::mutex> lock(mSnapshotMutex);
    mSnapshot.players = std::move(players);
    mSnapshot.localId = localId;
}

void CoopNet::ThreadMain() {
    mSessionStart = Clock::now();
    mSilent = false;

    if (SDLNet_Init() != 0) {
        SetState(SessionState::Failed, fmt::format("SDL_net failed to initialize: {}", SDLNet_GetError()));
        COOP_WARN("SDL_net failed to initialize: {}", SDLNet_GetError());
    } else {
        if (mConfig.role == Role::Host) {
            RunHost();
        } else {
            RunClient();
        }
        SDLNet_Quit();
    }

    {
        std::lock_guard<std::mutex> lock(mOutboxMutex);
        mOutbox.clear();
    }
    mThreadActive = false;
}

// MARK: - Shared connection handling

bool CoopNet::ReadFrom(Connection& conn, std::vector<Protocol::Frame>& frames) {
    uint8_t buffer[4096];
    const int received = SDLNet_TCP_Recv(conn.socket, buffer, sizeof(buffer));
    if (received <= 0) {
        conn.closed = true;
        conn.closeReason = "connection closed by peer";
        return false;
    }

    // Any data counts as a sign of life, not only heartbeats.
    conn.lastReceived = Clock::now();
    conn.decoder.Feed(buffer, static_cast<size_t>(received));

    Protocol::Frame frame;
    while (conn.decoder.Next(frame)) {
        frames.push_back(std::move(frame));
    }
    if (conn.decoder.HasError()) {
        COOP_WARN("Malformed data from {}: {}. Dropping the connection.", conn.peer, conn.decoder.Error());
        conn.closed = true;
        conn.closeReason = "malformed data";
        return false;
    }
    return true;
}

void CoopNet::SendRaw(Connection& conn, const std::string& bytes) {
    if (conn.closed || bytes.empty()) {
        return;
    }
    const int length = static_cast<int>(bytes.size());
    if (SDLNet_TCP_Send(conn.socket, bytes.data(), length) < length) {
        conn.closed = true;
        conn.closeReason = "send failed";
    }
}

void CoopNet::SendControl(Connection& conn, const nlohmann::json& message) {
    SendRaw(conn, Protocol::EncodeJson(message));
}

bool CoopNet::HandleHeartbeatMessage(Connection& conn, const Protocol::Frame& frame) {
    if (frame.kind != Protocol::Frame::Kind::Json) {
        return false;
    }
    const std::string type = Protocol::GetString(frame.json, "type");
    if (type == "ping") {
        if (!mSilent) {
            SendControl(conn, { { "type", "pong" }, { "t", Protocol::GetInt(frame.json, "t") } });
        }
        return true;
    }
    if (type == "pong") {
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - mSessionStart).count();
        const int64_t sentAt = Protocol::GetInt(frame.json, "t", -1);
        if (sentAt >= 0 && sentAt <= now) {
            conn.rttMs = static_cast<int32_t>(std::min<int64_t>(now - sentAt, INT32_MAX));
            if (!conn.loggedFirstPong) {
                conn.loggedFirstPong = true;
                COOP_INFO("Heartbeat established with {}: round trip {} ms", conn.peer, conn.rttMs);
            }
        }
        return true;
    }
    return false;
}

void CoopNet::TickHeartbeat(Connection& conn, Clock::time_point now) {
    if (conn.closed) {
        return;
    }
    if (now - conn.lastReceived > kHeartbeatTimeout) {
        const double silentFor = std::chrono::duration<double>(now - conn.lastReceived).count();
        COOP_WARN("Heartbeat lost with {}: nothing received for {:.1f} s", conn.peer, silentFor);
        conn.closed = true;
        conn.closeReason = fmt::format("heartbeat lost (nothing received for {:.1f} s)", silentFor);
        return;
    }
    if (!mSilent && now - conn.lastPingSent >= kHeartbeatInterval) {
        conn.lastPingSent = now;
        const int64_t t = std::chrono::duration_cast<std::chrono::milliseconds>(now - mSessionStart).count();
        SendControl(conn, { { "type", "ping" }, { "t", t } });
    }
}

void CoopNet::UpdateSilence(Clock::time_point now) {
    if (mConfig.silenceAfterSeconds > 0 && !mSilent &&
        now - mSessionStart >= std::chrono::seconds(mConfig.silenceAfterSeconds)) {
        mSilent = true;
        COOP_WARN("TEST: --coop-silence-after {} reached, no longer answering or sending heartbeats",
                  mConfig.silenceAfterSeconds);
    }
}

// MARK: - Host

void CoopNet::RunHost() {
    IPaddress listenAddress;
    if (SDLNet_ResolveHost(&listenAddress, nullptr, mConfig.port) != 0) {
        SetState(SessionState::Failed, fmt::format("Could not prepare port {}: {}", mConfig.port, SDLNet_GetError()));
        COOP_WARN("Could not prepare port {}: {}", mConfig.port, SDLNet_GetError());
        return;
    }
    TCPsocket server = SDLNet_TCP_Open(&listenAddress);
    if (server == nullptr) {
        SetState(SessionState::Failed, fmt::format("Could not listen on port {}: {}", mConfig.port, SDLNet_GetError()));
        COOP_WARN("Could not listen on port {}: {}", mConfig.port, SDLNet_GetError());
        return;
    }

    SDLNet_SocketSet socketSet = SDLNet_AllocSocketSet(static_cast<int>(kMaxConnections + 1));
    SDLNet_TCP_AddSocket(socketSet, server);
    std::vector<std::unique_ptr<Connection>> conns;
    mNextPlayerId = kHostPlayerId + 1;

    SetState(SessionState::Hosting, fmt::format("Hosting on port {}", mConfig.port));
    COOP_INFO("Hosting on port {} as '{}' (build {}, protocol {})", mConfig.port, mConfig.name, mConfig.buildVersion,
              mConfig.protocolVersion);
    bool playersChanged = true;

    while (!mStopRequested.load()) {
        const int ready = SDLNet_CheckSockets(socketSet, kPollIntervalMs);
        if (ready < 0) {
            // select() failed; avoid spinning and try again.
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
        }
        const Clock::time_point now = Clock::now();

        if (ready > 0 && SDLNet_SocketReady(server)) {
            TCPsocket socket = SDLNet_TCP_Accept(server);
            if (socket != nullptr) {
                const std::string peer = PeerAddress(socket);
                if (conns.size() >= kMaxConnections) {
                    COOP_WARN("Refusing connection from {}: too many open connections", peer);
                    SDLNet_TCP_Close(socket);
                } else {
                    auto conn = std::make_unique<Connection>();
                    conn->socket = socket;
                    conn->peer = peer;
                    COOP_INFO("TCP_NODELAY {} on the connection from {}",
                              EnableNoDelay(socket) ? "enabled" : "NOT enabled", peer);
                    conn->openedAt = conn->lastReceived = conn->lastPingSent = now;
                    SDLNet_TCP_AddSocket(socketSet, socket);
                    COOP_INFO("Incoming connection from {}", peer);
                    conns.push_back(std::move(conn));
                }
            }
        }

        size_t activePlayers =
            1 + std::count_if(conns.begin(), conns.end(), [](const auto& c) { return c->active && !c->closed; });

        for (auto& conn : conns) {
            if (conn->closed || ready <= 0 || !SDLNet_SocketReady(conn->socket)) {
                continue;
            }
            std::vector<Protocol::Frame> frames;
            ReadFrom(*conn, frames);
            for (auto& frame : frames) {
                if (conn->closed) {
                    break;
                }
                const bool isJson = frame.kind == Protocol::Frame::Kind::Json;
                const std::string type = isJson ? Protocol::GetString(frame.json, "type") : "";

                if (!conn->active) {
                    if (type == "hello") {
                        HostHandleHello(*conn, frame, activePlayers);
                        if (conn->active) {
                            activePlayers++;
                            playersChanged = true;
                        }
                    } else {
                        HostReject(*conn, "The first message must be a hello handshake.");
                    }
                    continue;
                }
                if (HandleHeartbeatMessage(*conn, frame)) {
                    continue;
                }
                if (type == "bye") {
                    conn->closed = true;
                    conn->closeReason =
                        fmt::format("left the session ({})", Printable(Protocol::GetString(frame.json, "reason"), 80));
                    continue;
                }
                if (isJson && IsControlType(type)) {
                    COOP_WARN("Player {} sent unexpected control message '{}'. Dropping the connection.",
                              conn->playerId, type);
                    conn->closed = true;
                    conn->closeReason = "protocol error";
                    continue;
                }
                PushInbox(conn->playerId, std::move(frame));
            }
        }

        for (auto& message : TakeOutbox()) {
            for (auto& conn : conns) {
                if (conn->active && (message.target == kBroadcast || message.target == conn->playerId)) {
                    SendRaw(*conn, message.bytes);
                }
            }
        }

        UpdateSilence(now);
        for (auto& conn : conns) {
            if (conn->closed) {
                continue;
            }
            if (!conn->active) {
                if (now - conn->openedAt > kHandshakeTimeout) {
                    HostReject(*conn, "Handshake timed out.");
                }
            } else {
                TickHeartbeat(*conn, now);
            }
        }

        for (auto it = conns.begin(); it != conns.end();) {
            Connection& conn = **it;
            if (!conn.closed) {
                ++it;
                continue;
            }
            if (conn.active) {
                COOP_INFO("Player '{}' (id {}) disconnected: {}", conn.name, conn.playerId, conn.closeReason);
                playersChanged = true;
            } else {
                COOP_INFO("Connection from {} closed: {}", conn.peer, conn.closeReason);
            }
            SDLNet_TCP_DelSocket(socketSet, conn.socket);
            SDLNet_TCP_Close(conn.socket);
            it = conns.erase(it);
        }

        std::vector<PlayerInfo> players = HostBuildPlayerList(conns);
        if (playersChanged) {
            playersChanged = false;
            const nlohmann::json message = PlayersMessage(players);
            for (auto& conn : conns) {
                if (conn->active) {
                    SendControl(*conn, message);
                }
            }
            COOP_INFO("Players ({}): {}", players.size(), DescribePlayers(players, kHostPlayerId));
        }
        SetPlayers(std::move(players), kHostPlayerId);
    }

    for (auto& conn : conns) {
        if (conn->active && !conn->closed) {
            SendControl(*conn, { { "type", "bye" }, { "reason", "The host closed the session." } });
        }
        SDLNet_TCP_DelSocket(socketSet, conn->socket);
        SDLNet_TCP_Close(conn->socket);
    }
    SDLNet_TCP_DelSocket(socketSet, server);
    SDLNet_TCP_Close(server);
    SDLNet_FreeSocketSet(socketSet);

    COOP_INFO("Stopped hosting");
    SetPlayers({}, 0);
    SetState(SessionState::Idle, "Session stopped");
}

void CoopNet::HostHandleHello(Connection& conn, const Protocol::Frame& frame, size_t activePlayers) {
    const nlohmann::json& hello = frame.json;
    const int64_t protocol = Protocol::GetInt(hello, "protocol", -1);
    const std::string version = Printable(Protocol::GetString(hello, "version", "unknown"), 64);
    conn.name = Protocol::SanitizeName(Protocol::GetString(hello, "name"));

    if (protocol != mConfig.protocolVersion) {
        HostReject(conn, fmt::format("Coop protocol mismatch: the host uses protocol {} but you use {}. Both players "
                                     "must run the same build.",
                                     mConfig.protocolVersion, protocol));
        return;
    }
    if (version != mConfig.buildVersion) {
        HostReject(conn, fmt::format("Build mismatch: the host is running build '{}' but you are running '{}'. Both "
                                     "players must run the exact same build.",
                                     mConfig.buildVersion, version));
        return;
    }
    if (activePlayers >= kMaxPlayers) {
        HostReject(conn, fmt::format("The session is full ({}/{} players).", activePlayers, kMaxPlayers));
        return;
    }

    auto colorIt = hello.find("color");
    if (colorIt == hello.end() || !Protocol::ColorFromJson(*colorIt, conn.color)) {
        conn.color = {};
    }
    conn.active = true;
    conn.playerId = mNextPlayerId++;
    conn.lastReceived = conn.lastPingSent = Clock::now();
    SendControl(conn, { { "type", "welcome" }, { "id", conn.playerId }, { "version", mConfig.buildVersion } });
    COOP_INFO("Handshake accepted: '{}' (id {}) from {}", conn.name, conn.playerId, conn.peer);
}

void CoopNet::HostReject(Connection& conn, const std::string& reason) {
    COOP_WARN("Rejected {}{}: {}", conn.peer, conn.name.empty() ? "" : fmt::format(" ('{}')", conn.name), reason);
    SendControl(conn, { { "type", "reject" }, { "reason", reason } });
    conn.closed = true;
    conn.closeReason = "rejected";
}

std::vector<PlayerInfo> CoopNet::HostBuildPlayerList(const std::vector<std::unique_ptr<Connection>>& conns) const {
    std::vector<PlayerInfo> players;
    players.push_back({ kHostPlayerId, mConfig.name, mConfig.color, true, -1 });
    for (const auto& conn : conns) {
        if (conn->active && !conn->closed) {
            players.push_back({ conn->playerId, conn->name, conn->color, false, conn->rttMs });
        }
    }
    return players;
}

// MARK: - Client

void CoopNet::RunClient() {
    const std::string target = fmt::format("{}:{}", mConfig.address, mConfig.port);
    SetState(SessionState::Connecting, fmt::format("Connecting to {}", target));
    COOP_INFO("Connecting to {} as '{}' (build {}, protocol {})", target, mConfig.name, mConfig.buildVersion,
              mConfig.protocolVersion);

    IPaddress hostAddress;
    if (SDLNet_ResolveHost(&hostAddress, mConfig.address.c_str(), mConfig.port) != 0) {
        SetState(SessionState::Failed, fmt::format("Could not resolve '{}'", mConfig.address));
        COOP_WARN("Could not resolve '{}': {}", mConfig.address, SDLNet_GetError());
        return;
    }
    TCPsocket socket = SDLNet_TCP_Open(&hostAddress);
    if (socket == nullptr) {
        SetState(SessionState::Failed, fmt::format("Could not connect to {}", target));
        COOP_WARN("Could not connect to {}: {}", target, SDLNet_GetError());
        return;
    }

    COOP_INFO("TCP_NODELAY {} on the connection to {}", EnableNoDelay(socket) ? "enabled" : "NOT enabled", target);
    SDLNet_SocketSet socketSet = SDLNet_AllocSocketSet(1);
    SDLNet_TCP_AddSocket(socketSet, socket);

    Connection host;
    host.socket = socket;
    host.peer = target;
    host.playerId = kHostPlayerId;
    host.openedAt = host.lastReceived = host.lastPingSent = Clock::now();

    SetState(SessionState::Handshaking, fmt::format("Connected to {}, waiting for the host to accept", target));
    COOP_INFO("Connected to {}, sending handshake", target);
    SendControl(host, { { "type", "hello" },
                        { "protocol", mConfig.protocolVersion },
                        { "version", mConfig.buildVersion },
                        { "name", mConfig.name },
                        { "color", Protocol::ColorToJson(mConfig.color) } });

    SessionState endState = SessionState::Disconnected;
    std::string endMessage;
    std::vector<PlayerInfo> players;
    uint32_t localId = 0;

    while (!mStopRequested.load() && !host.closed) {
        const int ready = SDLNet_CheckSockets(socketSet, kPollIntervalMs);
        if (ready < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
        }
        const Clock::time_point now = Clock::now();

        if (ready > 0 && SDLNet_SocketReady(socket)) {
            std::vector<Protocol::Frame> frames;
            ReadFrom(host, frames);
            for (auto& frame : frames) {
                if (host.closed) {
                    break;
                }
                const bool isJson = frame.kind == Protocol::Frame::Kind::Json;
                const std::string type = isJson ? Protocol::GetString(frame.json, "type") : "";

                if (type == "reject") {
                    endState = SessionState::Rejected;
                    endMessage = Printable(Protocol::GetString(frame.json, "reason", "No reason given."), 300);
                    COOP_WARN("Rejected by host: {}", endMessage);
                    host.closed = true;
                    host.closeReason = "rejected";
                    break;
                }
                if (type == "bye") {
                    endState = SessionState::Disconnected;
                    endMessage = Printable(Protocol::GetString(frame.json, "reason", "The host left."), 300);
                    COOP_INFO("Host ended the session: {}", endMessage);
                    host.closed = true;
                    host.closeReason = "host left";
                    break;
                }
                if (!host.active) {
                    if (type == "welcome") {
                        host.active = true;
                        localId = static_cast<uint32_t>(Protocol::GetInt(frame.json, "id"));
                        host.lastPingSent = now;
                        SetState(SessionState::Connected, fmt::format("Connected to {}", target));
                        COOP_INFO("Handshake accepted by host, our player id is {}", localId);
                    } else {
                        COOP_WARN("Host sent '{}' before accepting the handshake. Dropping the connection.", type);
                        host.closed = true;
                        host.closeReason = "protocol error";
                    }
                    continue;
                }
                if (HandleHeartbeatMessage(host, frame)) {
                    continue;
                }
                if (type == "players") {
                    auto listIt = frame.json.find("players");
                    if (listIt == frame.json.end() || !listIt->is_array()) {
                        COOP_WARN("Ignoring malformed player list from host");
                        continue;
                    }
                    players.clear();
                    for (const auto& entry : *listIt) {
                        if (players.size() >= kMaxListedPlayers) {
                            break;
                        }
                        if (!entry.is_object()) {
                            continue;
                        }
                        PlayerInfo player;
                        player.id = static_cast<uint32_t>(Protocol::GetInt(entry, "id"));
                        player.name = Protocol::SanitizeName(Protocol::GetString(entry, "name"));
                        auto colorIt = entry.find("color");
                        if (colorIt != entry.end()) {
                            Protocol::ColorFromJson(*colorIt, player.color);
                        }
                        auto hostIt = entry.find("host");
                        player.isHost = hostIt != entry.end() && hostIt->is_boolean() && hostIt->get<bool>();
                        players.push_back(player);
                    }
                    COOP_INFO("Players ({}): {}", players.size(), DescribePlayers(players, localId));
                    continue;
                }
                if (isJson && IsControlType(type)) {
                    COOP_WARN("Host sent unexpected control message '{}'. Dropping the connection.", type);
                    host.closed = true;
                    host.closeReason = "protocol error";
                    continue;
                }
                PushInbox(kHostPlayerId, std::move(frame));
            }
        }

        if (host.active) {
            for (auto& message : TakeOutbox()) {
                SendRaw(host, message.bytes);
            }
        }

        UpdateSilence(now);
        if (!host.closed) {
            if (!host.active) {
                if (now - host.openedAt > kHandshakeTimeout) {
                    endState = SessionState::Failed;
                    endMessage = "The host did not answer the handshake.";
                    COOP_WARN("Handshake with {} timed out", target);
                    host.closed = true;
                }
            } else {
                TickHeartbeat(host, now);
            }
        }

        std::vector<PlayerInfo> withRtt = players;
        for (auto& player : withRtt) {
            if (player.isHost) {
                player.rttMs = host.rttMs;
            }
        }
        SetPlayers(std::move(withRtt), localId);
    }

    if (host.closed) {
        if (endMessage.empty()) {
            endState = host.active ? SessionState::Disconnected : SessionState::Failed;
            endMessage = fmt::format("Lost connection to the host: {}", host.closeReason);
            COOP_WARN("Disconnected from {}: {}", target, host.closeReason);
        }
    } else {
        if (host.active) {
            SendControl(host, { { "type", "bye" }, { "reason", "Player left." } });
        }
        endState = SessionState::Idle;
        endMessage = "Left the session";
        COOP_INFO("Left the session");
    }

    SDLNet_TCP_DelSocket(socketSet, socket);
    SDLNet_TCP_Close(socket);
    SDLNet_FreeSocketSet(socketSet);

    SetPlayers({}, 0);
    SetState(endState, endMessage);
}

} // namespace Coop
