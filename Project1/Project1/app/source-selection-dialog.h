#pragma once

#include<string>

struct PlaybackSelection {
    std::string source;
    std::string accessToken;
    std::wstring serverHost;
    unsigned short mediaPort = 0;
    int videoId = 0;
};

bool showSourceSelectionDialog(PlaybackSelection* selection);
