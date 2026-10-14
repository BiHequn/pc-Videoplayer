#include "service/local-server.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {
std::atomic<bool> stopping{false};
BOOL WINAPI handleSignal(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        stopping = true; return TRUE;
    }
    return FALSE;
}
std::string utf8(const wchar_t* text) {
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, nullptr, 0, nullptr, nullptr);
    if (!length) throw std::runtime_error("Invalid Unicode argument");
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, result.data(), length, nullptr, nullptr);
    result.pop_back(); return result;
}
}
int wmain(int argc, wchar_t** argv) {
    std::uint16_t port = 18760;
    std::string data = "service-data";
    try {
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--help") {
                std::cout << "native-service [--port 18760] [--data directory]\nLoopback listener: 127.0.0.1\n";
                return 0;
            }
            if ((arg != L"--port" && arg != L"--data") || i + 1 == argc) throw std::runtime_error("Unknown or incomplete argument");
            const std::wstring value = argv[++i];
            if (arg == L"--data") {
                if (value.empty()) throw std::runtime_error("Data directory is empty");
                data = utf8(value.c_str());
            } else {
                if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos) throw std::runtime_error("Port must be decimal");
                const auto parsed = std::stoul(value);
                if (parsed > 65535) throw std::runtime_error("Port exceeds 65535");
                port = static_cast<std::uint16_t>(parsed);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nUsage: native-service [--port 18760] [--data directory]\n"; return 2;
    }
    LocalServer server(data, port);
    if (!server.start()) { std::cerr << "Cannot start loopback service or read its records\n"; return 1; }
    SetConsoleCtrlHandler(handleSignal, TRUE);
    std::cout << "native-service listening on 127.0.0.1:" << server.port()
              << " data=" << std::filesystem::absolute(std::filesystem::u8path(data)).u8string() << std::endl;
    while (!stopping) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    server.stop(); return 0;
}
