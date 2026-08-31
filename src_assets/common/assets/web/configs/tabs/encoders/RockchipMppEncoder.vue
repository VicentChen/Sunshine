<script setup>
import { ref } from 'vue'
import Checkbox from '../../../Checkbox.vue'

const props = defineProps([
  'platform',
  'config',
])

const config = ref(props.config)
</script>

<template>
  <div id="rockchip-mpp-encoder" class="config-page">
    <Checkbox class="mb-3"
              id="vulkan_ui"
              locale-prefix="config"
              v-model="config.vulkan_ui"
              default="true"
    ></Checkbox>

    <Checkbox class="mb-3"
              id="rkmpp_profile"
              locale-prefix="config"
              v-model="config.rkmpp_profile"
              default="false"
    ></Checkbox>

    <Checkbox class="mb-3"
              id="rkmpp_profile_overlay"
              locale-prefix="config"
              v-model="config.rkmpp_profile_overlay"
              default="false"
    ></Checkbox>

    <Checkbox class="mb-3"
              id="rkmpp_low_delay"
              locale-prefix="config"
              v-model="config.rkmpp_low_delay"
              default="false"
    ></Checkbox>

    <Checkbox class="mb-3"
              id="rkmpp_disable_reencode"
              locale-prefix="config"
              v-model="config.rkmpp_disable_reencode"
              default="false"
    ></Checkbox>
  </div>
</template>

<style scoped>
</style>
