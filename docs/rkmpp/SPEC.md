- 此文档用于记录约定的规范和目标, 必须由用户手动维护, Agent不能修改此文档.

# rkmpp

**优先使用硬件处理**:
- 编码时通过rkmpp进行硬件编码
- 缩放时通过RGA进行硬件缩放

**减少不必要的开销**:
- 优先通过EDID调整HDMI输入, 使HDMI输入分辨率与Moonlight请求分辨率一致
- Moonlight请求分辨率与HDMI输入分辨率不一致时, 在HDMI输入所支持的分辨率中, 选择与Moonlight请求分辨率最接近的分辨率
- 当HDMI输入分辨率与Moonlight请求分辨率一致时, 不得使用RGA进行重新缩放