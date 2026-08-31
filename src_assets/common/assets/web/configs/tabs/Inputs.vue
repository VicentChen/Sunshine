<script setup>
import { onBeforeUnmount, onMounted, ref } from 'vue'
import PlatformLayout from '../../PlatformLayout.vue'
import Checkbox from "../../Checkbox.vue";

const props = defineProps([
  'platform',
  'config'
])

const config = ref(props.config)
const xboxRemoteStatus = ref({ state: 'idle', stage: '', failure_stage: '', failure_kind: '', epoch: 0 })
let xboxStatusTimer

async function refreshXboxRemoteStatus() {
  try {
    const response = await fetch('./api/xbox-remote/status')
    if (response.ok) {
      xboxRemoteStatus.value = await response.json()
    }
  } catch (_) {
    xboxRemoteStatus.value = { state: 'unavailable', stage: '', failure_stage: 'status_request', failure_kind: '', epoch: 0 }
  }
}

onMounted(() => {
  refreshXboxRemoteStatus()
  xboxStatusTimer = window.setInterval(refreshXboxRemoteStatus, 2000)
})

onBeforeUnmount(() => window.clearInterval(xboxStatusTimer))
</script>

<template>
  <div id="input" class="config-page">
    <!-- Enable Gamepad Input -->
    <Checkbox class="mb-3"
              id="controller"
              locale-prefix="config"
              v-model="config.controller"
              default="true"
    ></Checkbox>

    <!-- Emulated Gamepad Type -->
    <div class="mb-3" v-if="config.controller === 'enabled' && platform !== 'macos' && config.controller_output !== 'nxbt'">
      <label for="gamepad" class="form-label">{{ $t('config.gamepad') }}</label>
      <select id="gamepad" class="form-select" v-model="config.gamepad">
        <option value="auto">{{ $t('_common.auto') }}</option>

        <PlatformLayout :platform="platform">
          <template #freebsd>
            <option value="generic">{{ $t("config.gamepad_generic") }}</option>
            <option value="x360">{{ $t('config.gamepad_x360') }}</option>
            <option value="xone">{{ $t("config.gamepad_xone") }}</option>
            <option value="xseries">{{ $t("config.gamepad_xseries") }}</option>
            <option value="ds4">{{ $t('config.gamepad_ds4') }}</option>
            <option value="ds5">{{ $t("config.gamepad_ds5") }}</option>
            <option value="switch">{{ $t("config.gamepad_switch") }}</option>
          </template>

          <template #linux>
            <option value="generic">{{ $t("config.gamepad_generic") }}</option>
            <option value="x360">{{ $t('config.gamepad_x360') }}</option>
            <option value="xone">{{ $t("config.gamepad_xone") }}</option>
            <option value="xseries">{{ $t("config.gamepad_xseries") }}</option>
            <option value="ds4">{{ $t('config.gamepad_ds4') }}</option>
            <option value="ds5">{{ $t("config.gamepad_ds5") }}</option>
            <option value="switch">{{ $t("config.gamepad_switch") }}</option>
          </template>

          <template #windows>
            <option value="generic">{{ $t("config.gamepad_generic") }}</option>
            <option value="x360">{{ $t('config.gamepad_x360') }}</option>
            <option value="xone">{{ $t("config.gamepad_xone") }}</option>
            <option value="xseries">{{ $t("config.gamepad_xseries") }}</option>
            <option value="ds4">{{ $t('config.gamepad_ds4') }}</option>
            <option value="ds5">{{ $t("config.gamepad_ds5") }}</option>
            <option value="switch">{{ $t("config.gamepad_switch") }}</option>
          </template>
        </PlatformLayout>
      </select>
      <div class="form-text">{{ $t('config.gamepad_desc') }}</div>
    </div>

    <!-- Controller output routing -->
    <section v-if="config.controller === 'enabled' && platform === 'linux'" class="border rounded p-3 mb-3">
      <h3 class="h5">{{ $t('config.controller_output_heading') }}</h3>
      <div class="mb-3">
        <label for="controller_output" class="form-label">{{ $t('config.controller_output') }}</label>
        <select id="controller_output" class="form-select" v-model="config.controller_output">
          <option value="virtual">{{ $t('config.controller_output_virtual') }}</option>
          <option value="nxbt">{{ $t('config.controller_output_nxbt') }}</option>
          <option value="both">{{ $t('config.controller_output_both') }}</option>
        </select>
        <div class="form-text">{{ $t('config.controller_output_desc') }}</div>
      </div>

      <template v-if="config.controller_output === 'nxbt' || config.controller_output === 'both'">
        <div class="mb-3">
          <label for="nxbt_socket" class="form-label">{{ $t('config.nxbt_socket') }}</label>
          <input id="nxbt_socket" type="text" class="form-control monospace"
                 placeholder="/run/nxbt-bridge/control.sock" v-model="config.nxbt_socket" />
          <div class="form-text">{{ $t('config.nxbt_socket_desc') }}</div>
        </div>
        <div class="mb-3">
          <label for="nxbt_controller_slot" class="form-label">{{ $t('config.nxbt_controller_slot') }}</label>
          <input id="nxbt_controller_slot" type="number" min="0" max="15" class="form-control"
                 placeholder="0" v-model="config.nxbt_controller_slot" />
          <div class="form-text">{{ $t('config.nxbt_controller_slot_desc') }}</div>
        </div>
        <div class="mb-3">
          <label for="nxbt_face_buttons" class="form-label">{{ $t('config.nxbt_face_buttons') }}</label>
          <select id="nxbt_face_buttons" class="form-select" v-model="config.nxbt_face_buttons">
            <option value="labels">{{ $t('config.nxbt_face_buttons_labels') }}</option>
            <option value="positions">{{ $t('config.nxbt_face_buttons_positions') }}</option>
          </select>
          <div class="form-text">{{ $t('config.nxbt_face_buttons_desc') }}</div>
        </div>
        <div class="row">
          <div class="col-md-6 mb-3">
            <label for="nxbt_trigger_press_threshold" class="form-label">{{ $t('config.nxbt_trigger_press_threshold') }}</label>
            <input id="nxbt_trigger_press_threshold" type="number" min="0" max="255" class="form-control"
                   placeholder="64" v-model="config.nxbt_trigger_press_threshold" />
          </div>
          <div class="col-md-6 mb-3">
            <label for="nxbt_trigger_release_threshold" class="form-label">{{ $t('config.nxbt_trigger_release_threshold') }}</label>
            <input id="nxbt_trigger_release_threshold" type="number" min="0" max="255" class="form-control"
                   placeholder="48" v-model="config.nxbt_trigger_release_threshold" />
          </div>
        </div>
        <div class="form-text mb-3">{{ $t('config.nxbt_trigger_thresholds_desc') }}</div>
        <div class="mb-3">
          <label for="nxbt_watchdog_timeout" class="form-label">{{ $t('config.nxbt_watchdog_timeout') }}</label>
          <input id="nxbt_watchdog_timeout" type="number" min="50" max="1000" class="form-control"
                 placeholder="150" v-model="config.nxbt_watchdog_timeout" />
          <div class="form-text">{{ $t('config.nxbt_watchdog_timeout_desc') }}</div>
        </div>
      </template>

      <hr />
      <Checkbox class="mb-3"
                id="xbox_remote_enabled"
                locale-prefix="config"
                v-model="config.xbox_remote_enabled"
                default="true"
      ></Checkbox>
      <div class="alert alert-secondary py-2" role="status">
        {{ $t('config.xbox_remote_status') }}:
        <strong>{{ xboxRemoteStatus.state }}</strong>
        <span v-if="xboxRemoteStatus.epoch"> #{{ xboxRemoteStatus.epoch }}</span>
        <span v-if="xboxRemoteStatus.stage"> / {{ xboxRemoteStatus.stage }}</span>
        <span v-if="xboxRemoteStatus.failure_stage"> — {{ xboxRemoteStatus.failure_stage }}</span>
        <span v-if="xboxRemoteStatus.failure_kind"> ({{ xboxRemoteStatus.failure_kind }})</span>
      </div>
      <template v-if="config.xbox_remote_enabled === 'enabled'">
        <div class="mb-3">
          <label for="xbox_remote_app" class="form-label">{{ $t('config.xbox_remote_app') }}</label>
          <input id="xbox_remote_app" type="text" class="form-control"
                 placeholder="Xbox" v-model="config.xbox_remote_app" />
          <div class="form-text">{{ $t('config.xbox_remote_app_desc') }}</div>
        </div>
        <div class="mb-3">
          <label for="xbox_remote_token_file" class="form-label">{{ $t('config.xbox_remote_token_file') }}</label>
          <input id="xbox_remote_token_file" type="text" class="form-control monospace"
                 placeholder="/home/user/.config/sunshine/xbox-remote/tokens.json" v-model="config.xbox_remote_token_file" />
          <div class="form-text">{{ $t('config.xbox_remote_token_file_desc') }}</div>
        </div>
        <div class="mb-3">
          <label for="xbox_remote_console_id" class="form-label">{{ $t('config.xbox_remote_console_id') }}</label>
          <input id="xbox_remote_console_id" type="password" class="form-control monospace"
                 autocomplete="off" v-model="config.xbox_remote_console_id" />
          <div class="form-text">{{ $t('config.xbox_remote_console_id_desc') }}</div>
        </div>
        <Checkbox class="mb-3"
                  id="xbox_remote_wake"
                  locale-prefix="config"
                  v-model="config.xbox_remote_wake"
                  default="true"
        ></Checkbox>
        <div class="mb-3">
          <label for="xbox_remote_idle_timeout" class="form-label">{{ $t('config.xbox_remote_idle_timeout') }}</label>
          <input id="xbox_remote_idle_timeout" type="number" min="0" max="86400" class="form-control"
                 placeholder="300" v-model="config.xbox_remote_idle_timeout" />
          <div class="form-text">{{ $t('config.xbox_remote_idle_timeout_desc') }}</div>
        </div>
      </template>
    </section>

    <!-- Additional options based on gamepad type -->
    <template v-if="config.controller === 'enabled'">
      <template v-if="config.controller_output !== 'nxbt' && (config.gamepad === 'ds4' || config.gamepad === 'ds5' || (config.gamepad === 'auto' && platform !== 'macos'))">
        <div class="mb-3 accordion">
          <div class="accordion-item">
            <h2 class="accordion-header">
              <button class="accordion-button" type="button" data-bs-toggle="collapse"
                      data-bs-target="#panelsStayOpen-collapseOne">
                {{ $t(config.gamepad === 'auto' ? 'config.gamepad_auto' : 'config.gamepad_ds4_manual') }}
              </button>
            </h2>
            <div id="panelsStayOpen-collapseOne" class="accordion-collapse collapse show"
                 aria-labelledby="panelsStayOpen-headingOne">
              <div class="accordion-body">
                <!-- Automatic PlayStation-style detection options -->
                <template v-if="config.gamepad === 'auto' && (platform === 'windows' || platform === 'linux')">
                  <!-- Gamepad with motion capability as a PlayStation-style controller -->
                  <Checkbox class="mb-3"
                            id="motion_as_ds4"
                            locale-prefix="config"
                            v-model="config.motion_as_ds4"
                            default="true"
                  ></Checkbox>
                  <!-- Gamepad with touch capability as a PlayStation-style controller -->
                  <Checkbox class="mb-3"
                            id="touchpad_as_ds4"
                            locale-prefix="config"
                            v-model="config.touchpad_as_ds4"
                            default="true"
                  ></Checkbox>
                </template>
                <!-- PlayStation-style option: Back/Select as touchpad click -->
                <template v-if="config.gamepad === 'ds4' || config.gamepad === 'ds5' || config.gamepad === 'auto'">
                  <Checkbox class="mb-3"
                            id="ds4_back_as_touchpad_click"
                            locale-prefix="config"
                            v-model="config.ds4_back_as_touchpad_click"
                            default="true"
                  ></Checkbox>
                </template>
                <!-- Virtual HID option: Controller MAC randomization -->
                <template v-if="config.gamepad === 'ds4' || config.gamepad === 'ds5' || (config.gamepad === 'auto' && platform !== 'macos')">
                  <Checkbox class="mb-3"
                            id="virtualhid_randomize_mac"
                            locale-prefix="config"
                            v-model="config.virtualhid_randomize_mac"
                            default="true"
                  ></Checkbox>
                </template>
              </div>
            </div>
          </div>
        </div>
      </template>
    </template>

    <!-- Home/Guide Button Emulation Timeout -->
    <div class="mb-3" v-if="config.controller === 'enabled'">
      <label for="back_button_timeout" class="form-label">{{ $t('config.back_button_timeout') }}</label>
      <input type="text" class="form-control" id="back_button_timeout" placeholder="-1"
             v-model="config.back_button_timeout" />
      <div class="form-text">{{ $t('config.back_button_timeout_desc') }}</div>
    </div>

    <!-- Enable Keyboard Input -->
    <hr>
    <Checkbox class="mb-3"
              id="keyboard"
              locale-prefix="config"
              v-model="config.keyboard"
              default="true"
    ></Checkbox>

    <!-- Key Repeat Delay-->
    <div class="mb-3" v-if="config.keyboard === 'enabled' && platform === 'windows'">
      <label for="key_repeat_delay" class="form-label">{{ $t('config.key_repeat_delay') }}</label>
      <input type="text" class="form-control" id="key_repeat_delay" placeholder="500"
             v-model="config.key_repeat_delay" />
      <div class="form-text">{{ $t('config.key_repeat_delay_desc') }}</div>
    </div>

    <!-- Key Repeat Frequency-->
    <div class="mb-3" v-if="config.keyboard === 'enabled' && platform === 'windows'">
      <label for="key_repeat_frequency" class="form-label">{{ $t('config.key_repeat_frequency') }}</label>
      <input type="text" class="form-control" id="key_repeat_frequency" placeholder="24.9"
             v-model="config.key_repeat_frequency" />
      <div class="form-text">{{ $t('config.key_repeat_frequency_desc') }}</div>
    </div>

    <!-- Always send scancodes -->
    <Checkbox v-if="config.keyboard === 'enabled' && platform === 'windows'"
              class="mb-3"
              id="always_send_scancodes"
              locale-prefix="config"
              v-model="config.always_send_scancodes"
              default="true"
    ></Checkbox>

    <!-- Mapping Key AltRight to Key Windows -->
    <Checkbox v-if="config.keyboard === 'enabled'"
              class="mb-3"
              id="key_rightalt_to_key_win"
              locale-prefix="config"
              v-model="config.key_rightalt_to_key_win"
              default="false"
    ></Checkbox>

    <!-- Enable Mouse Input -->
    <hr>
    <Checkbox class="mb-3"
              id="mouse"
              locale-prefix="config"
              v-model="config.mouse"
              default="true"
    ></Checkbox>

    <!-- High resolution scrolling support -->
    <Checkbox v-if="config.mouse === 'enabled'"
              class="mb-3"
              id="high_resolution_scrolling"
              locale-prefix="config"
              v-model="config.high_resolution_scrolling"
              default="true"
    ></Checkbox>

    <!-- Native pen/touch support -->
    <Checkbox v-if="config.mouse === 'enabled'"
              class="mb-3"
              id="native_pen_touch"
              locale-prefix="config"
              v-model="config.native_pen_touch"
              default="true"
    ></Checkbox>
  </div>
</template>

<style scoped>

</style>
