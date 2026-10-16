# 刷机说明：备份、写入、刷回

写给别的 BOILON S3 二代用户：第一次把本固件装进设备要用 USB，之后从 TF 卡升级。动手前先把本文读完。

> **风险自负。** 这不是官方固件，刷机可能让设备开不了机。一定先完整备份，出了问题用你自己的备份刷回。下面的步骤都是开发者在
> 自己那台上做过的操作（整片备份与恢复、首次写入），整理成本文的写法之后没有逐条重测。

## 先看这几条

- **只用你自己的备份。** 备份里有你的 Wi-Fi 密码、设备令牌和激活信息，原厂固件也是 BOILON 的版权：不要分享，不要上传。
- **硬件要相同。** 本固件按一台 BOILON S3 二代校准：ESP32-S3、16 MB Flash、8 MB PSRAM，屏幕、按键和分区见
  [板子说明](../../main/boards/boilon-s3-gen2/README.md)「硬件」。别的批次可能开得了机，显示或声音却不对，这时刷回备份。
- **只写这两处。** 写入时只写 app 和 8 KB 的启动选择（`otadata`），不碰 Bootloader、分区表、NVS 和 `assets`。不要用
  `erase_flash`、`idf.py flash` 或网页烧录器：它们会写 Bootloader、分区表，或者擦掉 NVS。刷回时才把备份整片写回。

## 准备

- 电脑：macOS、Linux、Windows 都行，装好 Python 3。
- esptool 固定用 4.12.0：`pip install esptool==4.12.0`。这是验证过的版本；5.0.2 在这台机器上读整片时中断过，保电脚本也只认
  4.12.0。
- 本仓库的 [`scripts/boilon/rom_latch_gpio15.py`](../../scripts/boilon/rom_latch_gpio15.py)。
- Release 里的文件，先用 `SHA256SUMS` 核对：
  - `boilon-ai-player-vX.Y.Z.bin`：固件（AI 变体；不配网就只在本地放故事）；
  - `boilon-otadata-ota1.bin`：8 KB 的启动选择，让设备从 `ota_1` 启动。也可以自己生成，内容相同：
    `python3 scripts/boilon/make_otadata.py boilon-otadata-ota1.bin`。
- 一根能传数据的 USB-C 线；设备充好电；电脑接上电源，别让它睡眠。
- 串口名：macOS 是 `/dev/cu.usbmodem…`，Linux 多半是 `/dev/ttyACM0`，Windows 是 `COM…`。下文写作 `PORT`。

下文的 esptool 命令都很长，在 macOS 和 Linux 上可以先定义一个函数（bash 和 zsh 都能用）：

```sh
PORT=/dev/cu.usbmodem1101   # 换成你的串口
esp() { python3 -m esptool --chip esp32s3 --port "$PORT" --baud 921600 --before no_reset --after no_reset_stub "$@"; }
```

Windows 上把下文的 `esp` 换成整条 `python -m esptool --chip esp32s3 --port COM5 --baud 921600 --before no_reset --after no_reset_stub`。

## 第 1 步：进下载模式，并保持供电

这台机器的供电靠 GPIO15 维持。芯片一进 ROM 下载模式，就没有程序维持它，松开电源键就会断电。

1. 设备关机，用 USB 接上电脑。
2. 按住顶部电源键开机，一直按着。
3. 电脑上出现串口后运行：

   ```sh
   python3 scripts/boilon/rom_latch_gpio15.py --port "$PORT" --confirm-volatile-write
   ```

4. 看到 `PASS: ROM established GPIO15 RTC push-pull high` 才松手。没看到 PASS 就别松手、别往下做：松手会断电，从头重来即可，
   这一步不写 Flash。之后若串口名变了，改用新的。

## 第 2 步：检查

```sh
esp flash_id              # 应是 ESP32-S3 和 16MB Flash
esp get_security_info     # Flash 加密和 Secure Boot 都应是关的
esp read_flash 0x8000 0x1000 partition-table.bin
```

用 ESP-IDF 的 `components/partition_table/gen_esp32part.py`（单个 Python 脚本）解析 `partition-table.bin`，和板子说明对照：
`nvs` 在 `0x9000`、`otadata` 在 `0x11000`、`phy_init` 在 `0x13000`、`ota_0` 在 `0x20000`、`ota_1` 在 `0x400000`（各 `0x3e0000`）、
`assets` 在 `0x800000`。有一项不符就停，不要往下做。

## 第 3 步：整片备份，读两遍

```sh
esp read_flash 0x0 0x1000000 backup-1.bin
esp read_flash 0x0 0x1000000 backup-2.bin
```

每遍要几分钟。比对两份的 SHA-256（macOS 和 Linux 用 `shasum -a 256 backup-1.bin backup-2.bin`，Windows 用
`certutil -hashfile backup-1.bin SHA256`），完全相同才算备份成功。把备份放到安全的地方，最好另存一份。

## 第 4 步：写入本固件

先写 app，再写启动选择，顺序不能反：

```sh
esp write_flash 0x400000 boilon-ai-player-vX.Y.Z.bin
esp write_flash 0x11000 boilon-otadata-ota1.bin
```

每条都要看到 `Hash of data verified.`。启动选择指向 `ota_1`，所以 app 必须先写好。`ota_0` 里原来的程序这时还在，但别把它当
退路：退路是你的整片备份。

## 第 5 步：关机，再开机

```sh
python3 -m esptool --chip esp32s3 --port "$PORT" --baud 115200 --before no_reset --after watchdog_reset read_mem 0x60008400
```

这条命令让设备受控关机：屏幕熄灭，串口消失。不要用 `hard_reset`，在这台机器上它会停在下载模式。之后按顶部键开机。

插上有故事的 TF 卡就能播。要和喵喵对话：同时长按中间和底部约 5 秒，打开配网热点 `Boilon-XXXX`；手机连上后打开
`192.168.4.1` 填 Wi-Fi。连上之后屏幕显示 6 位激活码，到 xiaozhi.me 用你自己的账号添加设备。屏幕上的像素猫叫喵喵；它说话的
角色和声音在 xiaozhi.me 的智能体里设置。不配网也能一直这样用，只是没有喵喵。

## 以后升级

把新版本的 `boilon-ai-player-vX.Y.Z.bin` 拷到 TF 卡根目录，文件名别改；关机插卡再开机，自动升级。规则见板子说明「版本与
SD 卡升级」。

装的是 v1.4.0 或更早的只离线变体（`-offline.bin`）的：它不能从 TF 卡升级，也不再有新版本。按第 1、4、5 步用 USB 写入
`boilon-ai-player-vX.Y.Z.bin`，以后就能从 TF 卡升级。

## 刷回备份（回到原厂）

按第 1 步进下载模式并保持供电，然后：

```sh
esp write_flash --flash_mode keep --flash_freq keep --flash_size keep 0x0 backup-1.bin
esp read_flash 0x0 0x1000000 readback.bin
```

比对 `readback.bin` 和 `backup-1.bin` 的 SHA-256，相同之后再按第 5 步关机、开机。设备回到备份那一刻：Wi-Fi、激活信息和设置
都回来。原厂固件联网后可能自己升级，介意的话先断网再开机。

## 出问题时

| 情况 | 怎么办 |
| --- | --- |
| 没看到 PASS 就松了手，设备断电 | 从第 1 步重来；这一步没写 Flash |
| 检查有一项不符 | 停下，不要写入；这台可能不是同一批硬件 |
| 写入或读回报错 | 不要拔线、不要开机，从第 1 步重新进下载模式再写一次；写坏了就刷回备份 |
| 能开机，但显示或声音不对 | 可能是别的硬件批次，刷回备份 |
| AI 变体联不上网 | 看板子说明「AI 变体」里的联网规则；离线播放不受影响 |
