#pragma once
#include <atomic>
#include <filesystem>
#include <string>
#include "service/protocol.h"
namespace service {
constexpr unsigned long PasswordIterations = 600000;
std::string randomHex(std::size_t bytes);
std::string sha256(const std::string& bytes);
std::string sha256File(const std::filesystem::path& path, const std::atomic<bool>* stop = nullptr);
std::string passwordHash(const std::string& password, const std::string& saltHex);
bool constantEqual(const std::string& a, const std::string& b);
void atomicWrite(const std::filesystem::path& path, const std::string& bytes);
Json loadRecord(const std::filesystem::path& path);
}
