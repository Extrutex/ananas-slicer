#pragma once

#include <cstddef>
#include <string>

namespace Slic3r::engine {

// Standard base64 (RFC 4648, with padding). The engine hands binary arrays to a front end inside JSON.
inline std::string base64_encode(const void* data, size_t size)
{
    static constexpr char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto*           bytes = static_cast<const unsigned char*>(data);
    std::string           out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        const unsigned value = (unsigned(bytes[i]) << 16) | (i + 1 < size ? unsigned(bytes[i + 1]) << 8 : 0u) | (i + 2 < size ? unsigned(bytes[i + 2]) : 0u);
        out.push_back(ALPHABET[(value >> 18) & 63]);
        out.push_back(ALPHABET[(value >> 12) & 63]);
        out.push_back(i + 1 < size ? ALPHABET[(value >> 6) & 63] : '=');
        out.push_back(i + 2 < size ? ALPHABET[value & 63] : '=');
    }
    return out;
}

} // namespace Slic3r::engine
