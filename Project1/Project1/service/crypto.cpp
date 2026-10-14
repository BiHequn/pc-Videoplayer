#include "service/crypto.h"
#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace service {
namespace {
void checked(NTSTATUS result) { if (result < 0) throw std::runtime_error("BCrypt operation failed"); }
struct Algorithm {
    BCRYPT_ALG_HANDLE handle = nullptr;
    explicit Algorithm(ULONG flags = 0) { checked(BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, flags)); }
    ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
};
struct Hasher {
    Algorithm algorithm;
    BCRYPT_HASH_HANDLE handle = nullptr;
    Hasher() { checked(BCryptCreateHash(algorithm.handle, &handle, nullptr, 0, nullptr, 0, 0)); }
    ~Hasher() { if (handle) BCryptDestroyHash(handle); }
    void append(const char* data, std::size_t size) {
        checked(BCryptHashData(handle, reinterpret_cast<PUCHAR>(const_cast<char*>(data)), static_cast<ULONG>(size), 0));
    }
    std::string finish() {
        std::array<unsigned char, 32> digest{};
        checked(BCryptFinishHash(handle, digest.data(), static_cast<ULONG>(digest.size()), 0));
        return hexEncode(digest.data(), digest.size());
    }
};
}
std::string randomHex(std::size_t size) {
    std::vector<unsigned char> bytes(size);
    checked(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    return hexEncode(bytes.data(), bytes.size());
}
std::string sha256(const std::string& bytes) { Hasher hash; hash.append(bytes.data(), bytes.size()); return hash.finish(); }
std::string sha256File(const std::filesystem::path& path, const std::atomic<bool>* stop) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open file for SHA256");
    Hasher hash;
    std::array<char, 64 * 1024> bytes{};
    while (input) {
        if (stop && stop->load()) throw std::runtime_error("File hashing canceled");
        input.read(bytes.data(), bytes.size());
        if (input.gcount()) hash.append(bytes.data(), static_cast<std::size_t>(input.gcount()));
    }
    if (!input.eof()) throw std::runtime_error("Cannot read file for SHA256");
    return hash.finish();
}
std::string passwordHash(const std::string& password, const std::string& saltHex) {
    Algorithm algorithm(BCRYPT_ALG_HANDLE_HMAC_FLAG);
    auto salt = hexDecode(saltHex);
    std::array<unsigned char, 32> hash{};
    checked(BCryptDeriveKeyPBKDF2(algorithm.handle,
        reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())), static_cast<ULONG>(password.size()),
        reinterpret_cast<PUCHAR>(salt.data()), static_cast<ULONG>(salt.size()), PasswordIterations,
        hash.data(), static_cast<ULONG>(hash.size()), 0));
    return hexEncode(hash.data(), hash.size());
}
bool constantEqual(const std::string& a, const std::string& b) {
    unsigned char different = a.size() == b.size() ? 0 : 1;
    for (std::size_t i = 0; i < a.size(); ++i)
        different |= static_cast<unsigned char>(a[i] ^ (i < b.size() ? b[i] : 0));
    return different == 0;
}
void atomicWrite(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path; temporary += L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create atomic record");
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot publish atomic record");
}
Json loadRecord(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return Json::object();
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read service record");
    auto record = Json::parse(file);
    if (!record.is_object()) throw std::runtime_error("Invalid service record");
    return record;
}
}
