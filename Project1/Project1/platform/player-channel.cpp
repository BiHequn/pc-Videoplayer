#include "player-channel.h"
#include "pip-window.h"

#include <iostream>
#include <string>
#include <windows.h>

static DWORD WINAPI StdinThread(LPVOID)
{
    std::string line;

    while (std::getline(std::cin, line))
    {
        if (line.find("\"command\":\"close\"") !=
            std::string::npos)
        {
            requestPipWindowClose();
            break;
        }

        if (line.find("\"command\":\"pause\"") !=
            std::string::npos)
        {
            requestPipWindowPaused(true);
            continue;
        }

        if (line.find("\"command\":\"play\"") !=
            std::string::npos)
        {
            requestPipWindowPaused(false);
            continue;
        }

        if (line.find("\"command\":\"opacity\"") !=
            std::string::npos)
        {
            if (line.find("\"value\":0.5") !=
                std::string::npos)
            {
                requestPipWindowOpacity(50);
            }
            else if (line.find("\"value\":1.0") !=
                std::string::npos ||
                line.find("\"value\":1}") !=
                std::string::npos)
            {
                requestPipWindowOpacity(100);
            }
            else if (line.find("\"value\":0}") !=
                std::string::npos)
            {
                requestPipWindowOpacity(0);
            }
        }
        if (line.find("\"command\":\"brightness\"") !=
            std::string::npos)
        {
            if (line.find("\"value\":0.5") !=
                std::string::npos)
            {
                requestPipWindowBrightness(50);
            }
            else if (line.find("\"value\":1.5") !=
                std::string::npos)
            {
                requestPipWindowBrightness(150);
            }
            else if (line.find("\"value\":1.0") !=
                std::string::npos ||
                line.find("\"value\":1}") !=
                std::string::npos)
            {
                requestPipWindowBrightness(100);
            }
        }
    }

    return 0;
}

void sendReady()
{
  std::cout << "{\"version\":1,\"event\":\"ready\"}" << std::endl;
}

void sendEnded()
{
  std::cout << "{\"version\":1,\"event\":\"ended\"}" << std::endl;
}

void startCloseWatcher()
{
  CreateThread(0, 0, StdinThread, 0, 0, 0);
}