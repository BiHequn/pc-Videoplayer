#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>

#include "media-protocol.h"

namespace {
constexpr char kNetworkKey[] = "VideoPlayer2026!";

std::string base64Encode(const std::vector<unsigned char> &input) {
    if (input.empty()) return {};
    DWORD length = 0;
    if (!CryptBinaryToStringA(input.data(), static_cast<DWORD>(input.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              nullptr, &length)) return {};
    std::string result(length, '\0');
    if (!CryptBinaryToStringA(input.data(), static_cast<DWORD>(input.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              result.data(), &length)) return {};
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}
}

std::string decodeMediaBase64(const std::string &input) {
    if (input.empty()) return {};
    DWORD length = 0;
    if (!CryptStringToBinaryA(input.c_str(), static_cast<DWORD>(input.size()),
                              CRYPT_STRING_BASE64, nullptr, &length, nullptr,
                              nullptr)) return {};
    std::vector<unsigned char> result(length);
    if (!CryptStringToBinaryA(input.c_str(), static_cast<DWORD>(input.size()),
                              CRYPT_STRING_BASE64, result.data(), &length,
                              nullptr, nullptr)) return {};
    result.resize(length);
    return std::string(result.begin(), result.end());
}

std::string encodeMediaProtocol(const std::string &json) {
    std::vector<unsigned char> encrypted(json.begin(), json.end());
    const std::size_t keyLength = sizeof(kNetworkKey) - 1;
    for (std::size_t i = 0; i < encrypted.size(); ++i)
        encrypted[i] ^= static_cast<unsigned char>(kNetworkKey[i % keyLength]);
    return base64Encode(encrypted);
}

std::string decodeMediaProtocol(const std::string &payload) {
    std::string decoded = decodeMediaBase64(payload);
    const std::size_t keyLength = sizeof(kNetworkKey) - 1;
    for (std::size_t i = 0; i < decoded.size(); ++i)
        decoded[i] = static_cast<char>(
            static_cast<unsigned char>(decoded[i]) ^
            static_cast<unsigned char>(kNetworkKey[i % keyLength]));
    return decoded;
}
