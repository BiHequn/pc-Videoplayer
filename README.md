# Native VideoPlayer1 client

This repository contains the Windows-native replacement for the Qt desktop
client. It uses Win32 controls, Direct3D 11, Media Foundation, FFmpeg, WinHTTP,
and Winsock. Qt is not required at build time or runtime.

## Features

- Play local media files.
- Play or cache HTTP-FLV and HLS VOD URLs.
- Log in through the VideoPlayer1 FastAPI service.
- Authenticate with the Linux media service.
- List, download, cache, and play server videos.
- Report a play only after the first video frame opens successfully.

The initial server defaults are `192.168.62.129:8000` for the API and
`192.168.62.129:8888` for media. They can be edited in the server-video dialog.

## Local build

Requirements:

- Visual Studio 2022 with Desktop development with C++.
- Windows 10 SDK.
- A current vcpkg checkout.

From a Developer PowerShell:

```powershell
vcpkg install --triplet x64-windows-static --x-manifest-root=. --x-install-root=.\vcpkg_installed
msbuild Project1\Project1.sln /m /p:Configuration=Release /p:Platform=x64
```

The executable is written to `bin/x64/Release/native-player.exe`.

Create the standalone ZIP with:

```powershell
.\scripts\package-release.ps1
```

The project itself currently has no repository license. Before public binary
distribution, choose an application license and retain the third-party notices.

## Server compatibility

The client expects the VideoPlayer1 services:

- FastAPI login endpoint on port 8000.
- TCP media protocol on port 8888.
- Commands 6, 23, 24, 30, and 32.

The media protocol is intended for a trusted network. The token is kept only in
memory for the selected playback and is not written to disk.

The current FastAPI login uses HTTP and the media protocol uses reversible
XOR/Base64 framing rather than TLS. Do not expose either service to the public
Internet or an untrusted LAN. HTTPS and authenticated TLS for the media socket
are required before public or cross-network deployment.

The current vcpkg configuration links FFmpeg statically. The generated ZIP is
for internal validation only and must not be published or redistributed until
the application license and LGPL static-link relinking/source obligations have
been reviewed, or the build has been switched to dynamic FFmpeg libraries.
