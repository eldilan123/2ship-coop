#include "CoopProtocol.h"

#include <algorithm>
#include <cstring>
#include <fmt/format.h>

namespace Coop::Protocol {

void FrameDecoder::Feed(const uint8_t* data, size_t size) {
    if (HasError()) {
        return;
    }
    Compact();
    mBuffer.insert(mBuffer.end(), data, data + size);
}

void FrameDecoder::Compact() {
    if (mReadPos == 0) {
        return;
    }
    mBuffer.erase(mBuffer.begin(), mBuffer.begin() + mReadPos);
    mReadPos = 0;
}

bool FrameDecoder::Next(Frame& out) {
    if (HasError() || mReadPos >= mBuffer.size()) {
        return false;
    }

    const uint8_t* start = mBuffer.data() + mReadPos;
    const size_t available = mBuffer.size() - mReadPos;

    if (start[0] == '{') {
        const uint8_t* terminator = static_cast<const uint8_t*>(memchr(start, '\0', available));
        if (terminator == nullptr) {
            if (available > kMaxJsonSize) {
                mError = fmt::format("JSON message exceeds {} bytes without a terminator", kMaxJsonSize);
            }
            return false;
        }
        const size_t length = terminator - start;
        if (length > kMaxJsonSize) {
            mError = fmt::format("JSON message of {} bytes exceeds the {} byte limit", length, kMaxJsonSize);
            return false;
        }

        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(start, terminator);
        } catch (const nlohmann::json::exception& e) {
            mError = fmt::format("invalid JSON: {}", e.what());
            return false;
        }
        if (!parsed.is_object() || !parsed.contains("type") || !parsed["type"].is_string()) {
            mError = "JSON message is not an object with a string \"type\" field";
            return false;
        }

        mReadPos += length + 1;
        out.kind = Frame::Kind::Json;
        out.json = std::move(parsed);
        out.channel = 0;
        out.payload.clear();
        return true;
    }

    if (start[0] == kBinaryMarker) {
        if (available < kBinaryHeaderSize) {
            return false;
        }
        const uint8_t channel = start[1];
        // Explicit little-endian decode so the format does not depend on the host byte order.
        const uint32_t length = static_cast<uint32_t>(start[2]) | (static_cast<uint32_t>(start[3]) << 8) |
                                (static_cast<uint32_t>(start[4]) << 16) | (static_cast<uint32_t>(start[5]) << 24);
        // Validate before buffering or allocating anything for the payload.
        if (length > kMaxBinarySize) {
            mError = fmt::format("binary message of {} bytes exceeds the {} byte limit", length, kMaxBinarySize);
            return false;
        }
        if (available < kBinaryHeaderSize + length) {
            return false;
        }

        out.kind = Frame::Kind::Binary;
        out.json = nullptr;
        out.channel = channel;
        out.payload.assign(start + kBinaryHeaderSize, start + kBinaryHeaderSize + length);
        mReadPos += kBinaryHeaderSize + length;
        return true;
    }

    mError = fmt::format("unexpected frame start byte 0x{:02X}", start[0]);
    return false;
}

std::string EncodeJson(const nlohmann::json& message) {
    // Replace invalid UTF-8 (e.g. from a typed name) instead of throwing.
    std::string encoded = message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    encoded.push_back('\0');
    return encoded;
}

std::string EncodeBinary(uint8_t channel, const void* data, size_t size) {
    std::string encoded;
    if (size > kMaxBinarySize) {
        return encoded;
    }
    const uint32_t length = static_cast<uint32_t>(size);
    encoded.reserve(kBinaryHeaderSize + size);
    encoded.push_back(static_cast<char>(kBinaryMarker));
    encoded.push_back(static_cast<char>(channel));
    encoded.push_back(static_cast<char>(length & 0xFF));
    encoded.push_back(static_cast<char>((length >> 8) & 0xFF));
    encoded.push_back(static_cast<char>((length >> 16) & 0xFF));
    encoded.push_back(static_cast<char>((length >> 24) & 0xFF));
    if (size > 0) {
        encoded.append(static_cast<const char*>(data), size);
    }
    return encoded;
}

std::string SanitizeName(const std::string& name) {
    std::string clean;
    for (unsigned char c : name) {
        if (c >= 0x20 && c != 0x7F) {
            clean.push_back(static_cast<char>(c));
        }
    }

    const size_t first = clean.find_first_not_of(' ');
    if (first == std::string::npos) {
        return "Player";
    }
    clean = clean.substr(first, clean.find_last_not_of(' ') - first + 1);

    if (clean.size() > kMaxNameLength) {
        size_t cut = kMaxNameLength;
        // Step back over UTF-8 continuation bytes so a multi-byte character is never split.
        while (cut > 0 && (static_cast<unsigned char>(clean[cut]) & 0xC0) == 0x80) {
            cut--;
        }
        clean.resize(cut);
    }
    return clean.empty() ? "Player" : clean;
}

nlohmann::json ColorToJson(const Color& color) {
    return nlohmann::json::array({ color.r, color.g, color.b });
}

bool ColorFromJson(const nlohmann::json& value, Color& out) {
    if (!value.is_array() || value.size() != 3) {
        return false;
    }
    uint8_t components[3];
    for (size_t i = 0; i < 3; i++) {
        if (!value[i].is_number_integer()) {
            return false;
        }
        const int64_t component = value[i].get<int64_t>();
        if (component < 0 || component > 255) {
            return false;
        }
        components[i] = static_cast<uint8_t>(component);
    }
    out = { components[0], components[1], components[2] };
    return true;
}

std::string GetString(const nlohmann::json& message, const char* key, const std::string& fallback) {
    auto it = message.find(key);
    if (it == message.end() || !it->is_string()) {
        return fallback;
    }
    return it->get<std::string>();
}

int64_t GetInt(const nlohmann::json& message, const char* key, int64_t fallback) {
    auto it = message.find(key);
    if (it == message.end() || !it->is_number_integer()) {
        return fallback;
    }
    return it->get<int64_t>();
}

} // namespace Coop::Protocol
