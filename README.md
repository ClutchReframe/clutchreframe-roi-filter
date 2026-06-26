# ClutchReframe ROI Filter for OBS Studio

ClutchReframe ROI Filter is a third-party OBS Studio filter plugin. It is not affiliated with, endorsed by, or sponsored by the OBS Project.

The plugin is licensed under GPL-2.0-only. See `LICENSE` and `COPYING`.

## Scope

This repository contains only the OBS execution layer:

- OBS filter registration and rendering application.
- Windows named pipe IPC consumer for `FrameMsgV1`.
- ROI keyframe buffering, interpolation/freeze behavior, and OBS crop/render application.

It does not include ClutchReframe Live companion, ROI detection, strategy logic, profile selection, product readiness logic, private UI host code, release infrastructure, service analytics configuration, or private release systems. The companion service is a separate process that communicates with this plugin through the documented named pipe protocol.

Without the proprietary ClutchReframe Live companion, the plugin can still be built, loaded in OBS, and configured as a filter. If no ROI producer is connected, it keeps the source loaded and remains in its no-input/frozen state until a compatible producer sends frames.

## Supported Target

- OBS Studio 32.0.4 x64
- Windows x64
- Visual Studio 2022
- CMake 3.24 or newer

## Build

The plugin build requires OBS headers, generated config headers, and the `libobs` import library. The simplest supported layout is an OBS 32.0.4 source tree already configured and built for x64 Release:

```cmd
cd /d C:\Dev\clutchreframe-roi-filter
cmake -S . -B build_obs3204_x64 -G "Visual Studio 17 2022" -A x64 -DOBS_SOURCE_ROOT=C:\Dev\obs-studio-32.0.4
cmake --build build_obs3204_x64 --config Release
```

`OBS_SOURCE_ROOT` must contain:

- `libobs\obs-module.h`
- `build_x64\config\obsconfig.h` or an equivalent generated config header path
- `build_x64\libobs\Release\libobs.lib` or `obs.lib`

For custom OBS layouts, pass the paths explicitly:

```cmd
cmake -S . -B build_obs3204_x64 -G "Visual Studio 17 2022" -A x64 ^
  -DOBS_INCLUDE_DIR=C:\Dev\obs-studio-32.0.4\libobs ^
  -DOBS_CONFIG_INCLUDE_DIR=C:\Dev\obs-studio-32.0.4\build_x64\config ^
  -DOBS_LIB_DIR=C:\Dev\obs-studio-32.0.4\build_x64\libobs\Release
cmake --build build_obs3204_x64 --config Release
```

The expected output is `clutchreframe-obs-plugin.dll`.

## Install Layout

OBS expects the plugin under the user plugin directory:

```text
%APPDATA%\obs-studio\plugins\clutchreframe-obs-plugin\
  bin\64bit\clutchreframe-obs-plugin.dll
  data\locale\en-US.ini
```

## IPC

The public protocol reference is `IPC_REFERENCE.md`. The current runtime path is Windows named pipe only. WebSocket documents from older development phases are historical and are not part of this plugin runtime path.

Default pipe name:

```text
\\.\pipe\ClutchReframe_ROI_default
```

Security or service-access controls used by a proprietary producer are intended to protect that service. They do not restrict GPL rights to inspect, modify, rebuild, or redistribute this plugin.

## Source Provenance

The plugin source files in this package are ClutchReframe-authored unless noted in `THIRD_PARTY_NOTICES.md`. The plugin links against OBS Studio's `libobs` and uses Windows SDK/MSVC runtime APIs.

## AI-Assisted Development

This project may use AI-assisted development. Released source and binaries are reviewed, built, and smoke-tested by the maintainer before distribution.
