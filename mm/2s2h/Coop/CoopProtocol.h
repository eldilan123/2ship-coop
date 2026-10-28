#ifndef COOP_PROTOCOL_H
#define COOP_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// Wire format shared by host and clients. Every frame on the TCP stream is one of:
//   JSON control message: '{' ... '}' '\0'   (UTF-8 object with a "type" field, null terminated)
//   Binary message:       0x01, u8 channel, u32 length (little-endian), <length> bytes of payload
// The first byte tells them apart, so both kinds can be interleaved on the same socket.
namespace Coop::Protocol {

// Coop protocol number. BUMP BY HAND whenever the wire format or the meaning of any message changes (new required
// field, changed type, new binary channel layout...). The host rejects a client whose protocol number or build ID
// differs from its own.
constexpr int kProtocolVersion = 9;

// Binary channels (SendBinary). JSON control types are listed in CoopNet.cpp. Application JSON types:
// "enter_request" / "enter_denied" (client enters the host's world), "host_room" (host changed room) and
// "scene_request" (client asks the host to take an exit).
constexpr uint8_t kChannelSnapshot = 1;    // Session snapshot, see CoopSnapshot.h
constexpr uint8_t kChannelClock = 2;       // Host clock, see CoopClock.h
constexpr uint8_t kChannelPlayerState = 3; // Pose of each player's Link, see CoopPlayerSync.h
constexpr uint8_t kChannelEnemies = 4;     // Enemies simulated by the host, see CoopEnemyWire.h
constexpr uint8_t kChannelHits = 5;        // A client's hits on enemies and the host's results, see CoopCombat.cpp
constexpr uint8_t kChannelProjectiles = 6; // Every player's own projectiles, see CoopProjectileWire.h

constexpr uint8_t kBinaryMarker = 0x01;
constexpr size_t kBinaryHeaderSize = 6;
constexpr size_t kMaxBinarySize = 1024 * 1024;
constexpr size_t kMaxJsonSize = 64 * 1024;
constexpr size_t kMaxNameLength = 24;

struct Color {
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
};

struct Frame {
    enum class Kind { Json, Binary };
    Kind kind = Kind::Json;
    nlohmann::json json;
    uint8_t channel = 0;
    std::vector<uint8_t> payload;
};

// Accumulates raw bytes from a socket and splits them into frames. Any malformed input puts the decoder in an
// error state (Error() returns the reason) and the connection must be dropped; it never throws.
class FrameDecoder {
  public:
    void Feed(const uint8_t* data, size_t size);
    // Returns true and fills `out` when a complete frame is available.
    bool Next(Frame& out);
    bool HasError() const {
        return !mError.empty();
    }
    const std::string& Error() const {
        return mError;
    }

  private:
    std::vector<uint8_t> mBuffer;
    size_t mReadPos = 0;
    std::string mError;

    void Compact();
};

std::string EncodeJson(const nlohmann::json& message);
std::string EncodeBinary(uint8_t channel, const void* data, size_t size);

// Trims, drops control characters and cuts to kMaxNameLength bytes without splitting a UTF-8 sequence.
std::string SanitizeName(const std::string& name);

nlohmann::json ColorToJson(const Color& color);
// Returns false if `value` is not an [r, g, b] array of integers in 0..255.
bool ColorFromJson(const nlohmann::json& value, Color& out);

// Reads a string field, returning `fallback` if it is missing or not a string.
std::string GetString(const nlohmann::json& message, const char* key, const std::string& fallback = "");
int64_t GetInt(const nlohmann::json& message, const char* key, int64_t fallback = 0);

} // namespace Coop::Protocol

#endif // COOP_PROTOCOL_H
