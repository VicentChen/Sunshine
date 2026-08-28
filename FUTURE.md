# Future Enhancements

This document outlines planned features and improvements for Sunshine.

## 1. RKMPP Encoder Web UI Integration
**Description:**
Expose Rockchip MPP (RKMPP) specific configurations to the Web UI via a dedicated encoder tab.

**Implementation Details:**
- **Frontend:** Create a new Vue component `src_assets/common/assets/web/configs/tabs/encoders/RkmppEncoder.vue`.
- Register the new tab in `ContainerEncoders.vue` so it appears alongside NVENC, VAAPI, etc.
- **Backend:** Ensure any new RKMPP-specific variables (e.g., rate control modes, QP limits, preset profiles) are parsed in `src/config.cpp` and exposed to the REST API, replacing or augmenting the current `Advanced` tab toggles (`rkmpp_profile`).

## 2. Audio Device Dropdown UI
**Description:**
Improve the user experience for selecting audio sinks and sources by replacing the current free-form text input with an interactive dropdown list.

**Implementation Details:**
- **Backend:** Create a new API endpoint (e.g., `/api/audio-devices`) in `src/confighttp.cpp` that queries the system audio server (PulseAudio, PipeWire, or ALSA) and returns a JSON list of available device names and descriptions.
- **Frontend:** Modify `src_assets/common/assets/web/configs/tabs/AudioVideo.vue`. Replace the `<input type="text">` for `audio_sink` and `audio_source` with a `<select>` dropdown (or an autocomplete combobox) that populates asynchronously from the new API endpoint. This eliminates the need for users to manually run commands like `pactl list short sources` and copy-paste device strings.
