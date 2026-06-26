# Third-Party Notices

This notice file applies only to the ClutchReframe ROI Filter for OBS Studio in this directory.

## ClutchReframe ROI Filter for OBS Studio

License: GPL-2.0-only

Copyright (C) 2026 ClutchReframe

Source provenance:

- Plugin source, headers, CMake, locale, and protocol reference files are ClutchReframe-authored unless a file states otherwise.
- No proprietary ClutchReframe Live companion, ROI detection, strategy, private UI host code, release infrastructure, service credentials, or service analytics implementation is included in this plugin package.
- If future files are derived from OBS plugin templates or third-party examples, the source and compatible notice must be recorded here before release.

## OBS Studio / libobs

This plugin links against OBS Studio's `libobs` plugin API.

OBS Studio is distributed under the GNU General Public License version 2 or later. The OBS license text is available from the OBS Studio source repository:

```text
https://github.com/obsproject/obs-studio/blob/master/COPYING
```

OBS Studio project:

```text
https://github.com/obsproject/obs-studio
```

## Windows SDK and Microsoft Visual C++ Runtime

The plugin uses Windows APIs and is built with Microsoft Visual Studio toolchains for Windows x64. These components are provided under their respective Microsoft license terms and are not included as source code in this plugin package.

## Proprietary Companion Boundary

The optional ClutchReframe Live companion is a separate process and is not part of this GPL plugin package. Its product or commercial license must not be mixed into this plugin notice in a way that restricts GPL rights for the plugin.
