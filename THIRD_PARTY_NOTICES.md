# Third-Party Notices

## obs-plugintemplate (MIT)

The OBS plugin build scaffolding in this repository — `cmake/`, `.github/`,
`build-aux/`, `CMakePresets.json`, `buildspec.json`, and the formatting
configuration (`.clang-format`, `.gersemirc`) — is derived from the official
[`obsproject/obs-plugintemplate`](https://github.com/obsproject/obs-plugintemplate)
project, used under the MIT License. Copyright stays with the original authors.

The decision to pin CI to OBS `31.1.1` with prebuilt obs-deps (in
`buildspec.json`) matches the template defaults and is intentionally kept to
minimize CI churn. The plugin's own source under `src/`, `tests/`, and `data/`
is governed by this project's LICENSE.