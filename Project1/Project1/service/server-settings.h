#pragma once

#include <string>

struct ServerSettings {
    std::wstring host = L"192.168.62.129";
    unsigned short apiPort = 8000;
    unsigned short mediaPort = 8888;
};
