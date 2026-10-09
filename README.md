# oes-fnos-gpu-npu

网心云 OES（Amlogic A311D / G12B）在 **飞牛 fnOS ARM** 上启用 NPU 与 H.264 视频硬解的应用包（FPK）。

- NPU：把 Vivante galcore 6.4.8.7 移植到飞牛 6.18 内核，可运行 .nb（NBG）模型，InceptionV3 单帧约 20 ms。
- 视频硬解：H.264（V4L2 m2m，`/dev/video26`，ffmpeg `-c:v h264_v4l2m2m`），与软解逐帧 md5 一致，约 83 fps，CPU 占用约为软解的 1%。

> 下载：请到 [Releases](../../releases) 下载 `oes-gpu-npu-<版本>.fpk`。
> 源码仓库不包含 Vivante 闭源用户态库（OpenVX/ovxlib）、测试模型和预编译 .ko，这些只在 FPK 中提供。

## 安装
1. 飞牛「应用中心 → 手动安装」，选择 fpk。
2. 向导中 NPU 驱动选「移植版 galcore 6.4.8.7」，视频硬解保持开启。
3. 内核 `6.18.18.c1151-trim` 直接使用内置预编译驱动；其他内核自动本机编译（需 gcc/make 与内核头文件）。
4. 建议重启一次。开机服务：`oes-galcore648.service`、`oes-vdec.service`。

## 测试（SSH，root）
```bash
oes-gpu-npu status
oes-gpu-npu npu-test     # goldfish ≈ 0.938，约 20ms
oes-gpu-npu vdec-test    # 硬解/软解逐帧 md5 一致
grep load /sys/kernel/debug/galcore/load   # 空闲应为 0%
```
开关：`oes-gpu-npu npu-on|npu-off|vdec-on|vdec-off`。加载异常导致重启后，下次开机自动退回飞牛原版。

## 限制
- **飞牛相册 AI 硬件加速**只支持 NVIDIA CUDA、Intel OpenVINO、AMD MIGraphX、瑞芯微 RKNN，不含晶晨 NPU，无论用哪个驱动都显示「无可用设备」，需等飞牛官方支持。人脸/智能识别仍可用 CPU 运行。
- **视频编码不可用**：影视切换分辨率（转码）需要硬件编码器。A311D 的编码器驱动（amvenc_multi/encoder）是晶晨私有 ioctl 接口，不是 V4L2 m2m，ffmpeg 与飞牛影视无法调用；Mali GPU 没有视频编码功能。本项目只打通了解码。
- 飞牛自带应用目前基本用不到硬解；适合 Docker/自定义脚本，例如摄像头 RTSP 硬解 + NPU 目标检测、抽帧与缩略图。
- HEVC/VP9 硬解尚未测试。

## 原理（硬解）
飞牛内核带有 Amlogic media_modules，但缺少多个初始化步骤。本包的辅助模块（`app/vdec/src`）补齐：
- `oes_codec_io` / `oes_reg_ops`：codec-io 寄存器映射与寄存器操作表
- `oes_vdec_alias`：旧驱动名映射
- `oes_canvas`：写入 DMC canvas LUT
- `oes_v4l2fix`：MPLANE 下屏蔽错误尺寸的单平面格式
- `oes_clkfix`：vdec_1_sel 固定 fclk_div3，防止时钟计数失衡

## 许可
- galcore 内核驱动源码遵循其原始许可（Vivante/VeriSilicon，MIT/GPLv2 双许可）。
- 本项目新增的内核模块以 GPLv2 发布，脚本以 MIT 发布。
- 闭源用户态库版权归原厂所有，不在本仓库中分发。

风险自负：涉及替换内核驱动，请先备份数据。
