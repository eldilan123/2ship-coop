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

// Bump when the meaning of any message changes; checked in the handshake alongside the build hash.
constexpr int kProtocolVersion = 1;

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
