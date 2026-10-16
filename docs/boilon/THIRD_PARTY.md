# 第三方组件、字体与许可

本文列出 `boilon-ai-player` 固件实际编进去的第三方代码、字体和素材，以及它们的许可证。本项目是非官方项目，与 BOILON、
九川、乐鑫或小智服务运营方没有隶属、授权或背书关系。

依据：v1.0.0 源码的构建（2026-10-13）。链接映射（`build/xiaozhi.map`）里实际用到的库：AI 变体 139 个，只离线的变体 75 个，
后者是前者的子集。组件版本以仓库里的 `dependencies.lock` 为准；许可证取自各组件的 `idf_component.yml`、许可证文件和 ESP-IDF 的
`docs/en/COPYRIGHT.rst`。依赖或变体有变化时，按同样的方法重新核对：

```sh
grep -oE '[^ /()]+\.a\(' build/xiaozhi.map | sed 's/($//' | sort -u
```

2026-10-14 同步上游 v2.5.2 后，用 v1.2.0 的构建按这个方法重新核对：两个变体链接的库与之前完全相同（139／75）；随上游升级的
4 个组件（esp-ml307 3.7.5、uart-eth-modem 0.7.0、uart-uhci 0.4.0、button 4.2.2）许可证不变。

2026-10-15 换到 ESP-IDF v6.1 后，用 v1.2.1 的构建再核对一次：两个变体各多链接一个 `libesp_hal_systimer.a`，变成 140／76。
它是乐鑫自己的系统定时器驱动（Apache-2.0），6.0.2 里编在 `libhal.a` 里，6.1 拆成了单独的组件；其余的库不变，组件管理器
下载的组件版本也不变，下表的第三方部分不变。6.1 的 `COPYRIGHT.rst` 多了蓝牙 LE Audio 与 ISO 两条（改编自 Zephyr），本固件
没有链接蓝牙协议栈，不含这两部分。

2026-10-16 只离线的变体退役，之后只构建 AI 变体。离线变体链接的库一直是 AI 变体的子集（v1.4.0 的构建：76 个都在 140 个里），
同一台机器上收集许可证时加不加它的链接映射，结果逐字节相同，所以下表不变。下表原来有一列「变体」，标明各部分编进哪个变体，
现在都编进 AI 变体，这一列去掉了。

## 本项目与上游

| 部分 | 来源 | 许可证 |
| --- | --- | --- |
| 小智固件 v2.5.2 及本仓库的 Boilon 代码（`main/`，链接为 `libmain.a`） | [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 与本仓库 | MIT（仓库根目录 `LICENSE`） |
| 喵喵形象、像素画与界面设计 | 本项目原创（P5-1） | 随本仓库 |

## 字体

| 字体 | 用在哪里 | 来源 | 许可证 |
| --- | --- | --- | --- |
| Noto Sans CJK，14 px 单色常用字 6845 个 | `m2_library_common_font.inc` | 78/xiaozhi-fonts 2.0.0（[78/noto-fonts](https://github.com/78/noto-fonts)） | SIL Open Font License 1.1（组件代码 Apache-2.0） |
| unscii-8 | `p5_1_ui_font.h`，由 LVGL 源码生成 | LVGL 自带 | 公有领域 |

## ESP-IDF v6.1

ESP-IDF 本身是 Apache-2.0。其中编进固件的第三方部分：

| 部分 | 许可证 |
| --- | --- |
| FreeRTOS 内核 | MIT |
| C 库（newlib／picolibc，`esp_libc`） | BSD 类，见 ESP-IDF `esp_libc` 的 COPYING 文件 |
| FatFs（ChaN） | BSD 风格 |
| Mbed TLS 与 TF-PSA-Crypto | Apache-2.0（与 GPL-2.0-or-later 双许可，这里按 Apache-2.0） |
| TLSF 内存分配器 | BSD-3-Clause |
| Xtensa HAL（`libxt_hal.a`，Tensilica／Cadence） | MIT |
| lwIP | BSD-3-Clause |
| wpa_supplicant | BSD-3-Clause |
| HTTP Parser（基于 NGINX 与 Joyent http-parser） | MIT 类，见 `http_parser/LICENSE.txt` |
| SPIFFS | MIT |
| Wi-Fi、PHY 与蓝牙基带二进制库（`libnet80211.a`、`libpp.a`、`libcore.a`、`libphy.a`、`libbtbb.a` 等） | 随 ESP-IDF 以二进制提供，Apache-2.0 |

## 工具链运行库

| 部分 | 许可证 |
| --- | --- |
| libgcc、libstdc++（GCC 15.2，Espressif esp-15.2.0） | GPL-3.0-or-later，附 GCC Runtime Library Exception，允许随固件分发 |

## 组件管理器下载的组件

「乐鑫 MIT」（ESPRESSIF MIT／Espressif Modified MIT）只允许用在乐鑫的芯片上；本设备是 ESP32-S3，符合。

| 组件 | 许可证 |
| --- | --- |
| espressif/esp_codec_dev | Apache-2.0 |
| espressif/esp_audio_codec（含 `libesp_audio_codec.a`、`libesp_audio_simple_dec.a` 二进制库） | 乐鑫修改版 MIT |
| espressif/esp_audio_effects（二进制库） | 乐鑫修改版 MIT |
| espressif/esp_new_jpeg（二进制库） | 乐鑫 MIT |
| espressif/cjson（cJSON） | MIT |
| espressif/esp_io_expander、esp_lcd_touch、esp_mmap_assets | Apache-2.0 |
| espressif2022/esp_emote_expression、esp_emote_gfx | Apache-2.0 |
| laride/heatshrink | ISC |
| lvgl/lvgl | MIT |
| 78/esp-ml307、uart-eth-modem、uart-uhci | Apache-2.0 |
| 78/esp-wifi-connect | MIT |
| 78/xiaozhi-fonts | Apache-2.0（字体数据 OFL-1.1，见上） |
| espressif/esp-sr（含语音前端、唤醒词等二进制库） | 乐鑫 MIT |
| espressif/esp_image_effects（二进制库）、esp_video | 乐鑫 MIT |
| espressif/esp-dsp、esp32-camera、esp_cam_sensor、esp_sccb_intf、esp_lvgl_port、mqtt、usb、led_strip | Apache-2.0 |
| espressif/adc_battery_estimation、button、knob、iot_eth、iot_usbh_cdc、iot_usbh_rndis | Apache-2.0 |
| espressif/dl_fft | MIT |

摄像头、视频、USB 主机、4G 模组和以太网等组件被上游的通用代码引用而编进固件，本板并不使用这些硬件。

## 分发

每个 Release 都附 `THIRD_PARTY_LICENSES.txt`。发版工作流（`.github/workflows/boilon-release.yml`）编完固件后，用
`scripts/boilon/collect_licenses.py` 从这次构建实际链接的库找到所属组件，收集许可证全文：本项目的 MIT 与字体的 OFL 1.1，
ESP-IDF 的 Apache-2.0 与 `COPYRIGHT.rst`，各组件自带的许可证文件（乐鑫 MIT、MIT／BSD／ISC 的版权声明等，相同的全文只印一次），
以及工具链运行库的 GCC Runtime Library Exception 说明。有库找不到所属组件时，发版失败。
