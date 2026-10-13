#include "app-runtime.h"
#include "desktop-player.h"
#include <cstring>

int main(int argc, char* argv[])
{
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--pip") == 0)
      return runNativePlayer(argc, argv);
  return runDesktopPlayer(argc, argv);
}
