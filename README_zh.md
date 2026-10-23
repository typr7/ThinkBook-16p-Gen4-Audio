# ThinkBook 16p Gen4 Audio

**联想 ThinkBook 16p Gen4 的 macOS 内建扬声器驱动。**

ALC287 · 双 CS35L41 · Intel I2C7 · x86_64

[English](README.md)

ThinkBook 16p Gen4 Audio 基于 [hoaug-tran 的 CirrusAudioFixup](https://github.com/hoaug-tran/CirrusAudioFixup) 移植，负责功放初始化、OEM DSP 固件、扬声器调音、工厂校准、播放监测和故障停止。

AppleHDA 和 AppleALC 提供声卡输出路径。`ThinkBookAudio.kext` 管理该路径上的两颗 CS35L41 功放，`ThinkBookI2C.kext` 提供有界轮询 I2C 传输。launchd 服务在登录或唤醒后启动音频时钟并初始化功放。

项目由 Codex + GPT-6.1 Sol 完成。

## 适用硬件

| 组件 | 配置 |
| --- | --- |
| 机型 | 联想 ThinkBook 16p Gen4 |
| 声卡 | Realtek ALC287，`10ec:0287` |
| 子系统 ID | `17AA:38A9` |
| 功放 | 两颗 CS35L41 rev B2，I2C 地址 `0x40` 和 `0x41` |
| 升压电路 | 带 VSPK 开关的外部升压电路 |
| I2C 控制器 | Intel I2C7，`8086:51D9`，PCI `00:10.1` |

**已验证配置：**1.0.0 版本运行于 macOS Sequoia 15.7.8、x86_64，使用 AppleALC 1.9.8、Lilu 1.7.3、`alcid=11` 和 48 kHz 立体声输出，左右声道均正常发声。

初始化使用该主板对应的 OEM 固件和调音文件，以及目标机器的 `CirrusSmartAmpCalibrationData` EFI 变量。驱动将校准记录与两颗功放的 UID 匹配，并在 OTP 初始化后再次核对。

## 从源码构建

### 构建环境

- Intel macOS。
- Xcode Command Line Tools：`xcode-select --install`。
- Python 3 和 Git。
- 下方指定版本的 MacKernelSDK。
- 下方列出的三份 OEM 固件/调音文件和 VoodooI2C 助手插件。

### 准备依赖并构建

以下步骤在项目根目录执行。

1. 获取 MacKernelSDK：

   ```sh
   git clone https://github.com/acidanthera/MacKernelSDK.git MacKernelSDK
   git -C MacKernelSDK checkout 7af1933c27aefcbdf4809ee44478829aad30f9c1
   ```

2. 下载联想官方的 [ThinkBook 16p G4 IRH Realtek 音频驱动（DS562451）](https://support.lenovo.com/us/en/downloads/ds562451-realtek-audio-driver-for-windows-11-64-bit-thinkbook-16p-g4-irh)。驱动包版本为 **Realtek_Audio_6.0.9715.1_WHQL_TB16P_DAX**，文件名为 [`mxth077frkj92df0.exe`](https://download.lenovo.com/consumer/mobiles/mxth077frkj92df0.exe)。

   在 Windows 运行安装包，将驱动文件提取到一个目录，例如 `C:\Drivers\ThinkBookAudio`。三份文件位于解包目录的 `source/ThirdParty/cirrus/csaudioext/` 下，将它们复制到项目的 `firmware/` 目录：

   | 文件 | `csaudioext/` 下的子目录 | 用途 |
   | --- | --- | --- |
   | `halo_cspl_RAM_revB2_29.63.1.wmfw` | `PB_6.61.1/FW/CS35L41/RevB2/` | DSP 固件 |
   | `17AA38A9_221222_V0_A0.bin` | `tunings/Lenovo_17AA38A9_20221222/` | 左声道调音 |
   | `17AA38A9_221222_V0_A1.bin` | `tunings/Lenovo_17AA38A9_20221222/` | 右声道调音 |

   已安装 OEM 驱动的 Windows 系统，也可以在 `C:\Windows\System32\DriverStore\FileRepository` 下按上述文件名查找。构建时按 [Resources/OEM-firmware.json](Resources/OEM-firmware.json) 校验文件大小和 SHA256。

3. 下载并解压官方 [VoodooI2C 2.9.1 Release 包](https://github.com/VoodooI2C/VoodooI2C/releases/tag/v2.9.1)：

   ```sh
   mkdir -p dependencies
   curl -fL --retry 3 -o dependencies/VoodooI2C-2.9.1-RELEASE.zip \
     https://github.com/VoodooI2C/VoodooI2C/releases/download/v2.9.1/VoodooI2C-2.9.1-RELEASE.zip
   echo "368e5eb60794583c340764813bd7e8244e94d3d3c06ea5b67034addd539487f4  dependencies/VoodooI2C-2.9.1-RELEASE.zip" | shasum -a 256 -c -
   ditto -x -k dependencies/VoodooI2C-2.9.1-RELEASE.zip dependencies/VoodooI2C-2.9.1
   ```

   解压后，构建使用的目录为：

   ```text
   dependencies/VoodooI2C-2.9.1/VoodooI2C.kext/Contents/PlugIns/
   ├── VoodooGPIO.kext
   ├── VoodooI2CServices.kext
   └── VoodooInput.kext
   ```

4. 运行主机测试并构建：

   ```sh
   python3 test.py
   python3 build.py --sdk ./MacKernelSDK --firmware-dir ./firmware \
     --plugins ./dependencies/VoodooI2C-2.9.1/VoodooI2C.kext/Contents/PlugIns \
     --out ./dist
   ```

   `build.py` 会调用 `Tools/embed_firmware.py`，校验这三份文件，将文件内容转换为 C 字节数组，写入 `dist/.build/generated/Devices/CS35L41/Resources/Firmware.hpp`。编译器通过该头文件将固件嵌入 `ThinkBookAudio.kext`。

   单独生成并检查该头文件的命令为：

   ```sh
   python3 Tools/embed_firmware.py --firmware-dir ./firmware \
     --out ./dist/.build/generated/Devices/CS35L41/Resources/Firmware.hpp
   ```

主机测试覆盖校准匹配、OTP UID 提取、I2C FIFO 与超时处理、输出限制、播放停止流程和固件输入校验。

### 构建产物

| `dist/` 内的产物 | 用途 |
| --- | --- |
| `ThinkBookAudio.kext` | CS35L41 功放与 DSP 驱动 |
| `ThinkBookI2C.kext` | I2C 控制器驱动及导入的助手插件 |
| `audioctl`、`audio-clock` | 驱动控制与音频时钟助手 |
| `start-audio.sh`、`local.numw.thinkbook-audio.plist` | launchd 初始化服务 |
| `BUILD-INPUTS.json`、`SHA256.json` | 构建输入记录与产物哈希 |

## 安装构建产物

### 文件放置位置

挂载用于启动 macOS 的 EFI 分区，按下表放置构建产物：

| 构建产物 | 目标位置 |
| --- | --- |
| `dist/ThinkBookI2C.kext` | `EFI/OC/Kexts/ThinkBookI2C.kext` |
| `dist/ThinkBookAudio.kext` | `EFI/OC/Kexts/ThinkBookAudio.kext` |
| `dist/audioctl`、`dist/audio-clock`、`dist/start-audio.sh` | `/Library/Application Support/ThinkBookAudio/` |
| `dist/local.numw.thinkbook-audio.plist` | `/Library/LaunchDaemons/local.numw.thinkbook-audio.plist` |
| `dist/BUILD-INPUTS.json`、`dist/SHA256.json` | 构建归档目录，用于核对输入与产物 |

复制完整的 kext 包，保留 `ThinkBookI2C.kext/Contents/PlugIns` 内的插件。

### 配置 OpenCore

将 [Resources/OpenCore-Kernel-Add.plist](Resources/OpenCore-Kernel-Add.plist) 中的五个条目，按表中顺序合并到现有 `config.plist → Kernel → Add`。已有的 VoodooI2C 条目同步更新为 ThinkBookI2C 路径；每个 bundle ID 使用一个启用条目。

| 顺序 | `BundlePath` | `ExecutablePath` |
| --- | --- | --- |
| 1 | `ThinkBookI2C.kext/Contents/PlugIns/VoodooInput.kext` | `Contents/MacOS/VoodooInput` |
| 2 | `ThinkBookI2C.kext/Contents/PlugIns/VoodooI2CServices.kext` | `Contents/MacOS/VoodooI2CServices` |
| 3 | `ThinkBookI2C.kext/Contents/PlugIns/VoodooGPIO.kext` | `Contents/MacOS/VoodooGPIO` |
| 4 | `ThinkBookI2C.kext` | `Contents/MacOS/VoodooI2C` |
| 5 | `ThinkBookAudio.kext` | `Contents/MacOS/ThinkBookAudio` |

五个条目均设置 `PlistPath = Contents/Info.plist`、`Enabled = True`、`Arch = x86_64`。将 Lilu、AppleALC 放在这些条目前，并在 `NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82 → boot-args` 中设置 `alcid=11`。

### 安装 macOS 初始化服务

在项目根目录执行以下命令，以 root 所有权安装助手程序并加载服务：

```sh
sudo install -d -o root -g wheel -m 755 "/Library/Application Support/ThinkBookAudio"
sudo install -o root -g wheel -m 755 dist/audioctl dist/audio-clock dist/start-audio.sh "/Library/Application Support/ThinkBookAudio/"
sudo install -o root -g wheel -m 644 dist/local.numw.thinkbook-audio.plist /Library/LaunchDaemons/
sudo launchctl bootout system/local.numw.thinkbook-audio 2>/dev/null || true
sudo launchctl enable system/local.numw.thinkbook-audio
sudo launchctl bootstrap system /Library/LaunchDaemons/local.numw.thinkbook-audio.plist
```

重启并登录后，选择内建扬声器，在“音频 MIDI 设置”中将输出设为 48 kHz 立体声，再检查驱动状态：

```sh
"/Library/Application Support/ThinkBookAudio/audioctl" status
```

`ARMED_PROTECTED_PLAYBACK` 表示初始化完成，播放监测已启动。服务日志位于 `/var/log/thinkbook-audio.log`。

## References

| 项目 | 用途 |
| --- | --- |
| [hoaug-tran/CirrusAudioFixup](https://github.com/hoaug-tran/CirrusAudioFixup) | 原功放驱动 |
| [Ayase-kira/ThinkBook-16p-G5-IRX-Audio](https://github.com/Ayase-kira/ThinkBook-16p-G5-IRX-Audio) | 主板移植与校准流程参考 |
| [hoaug-tran/VoodooI2C](https://github.com/hoaug-tran/VoodooI2C)、[VoodooI2C/VoodooI2C](https://github.com/VoodooI2C/VoodooI2C) | I2C 传输实现 |
| [VoodooGPIO](https://github.com/VoodooI2C/VoodooGPIO)、[VoodooInput](https://github.com/acidanthera/VoodooInput) | 助手接口 |
| [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) | 内核构建头文件和库 |
| [AppleALC](https://github.com/acidanthera/AppleALC)、[Lilu](https://github.com/acidanthera/Lilu) | 声卡路由和运行依赖 |

独立工具和文档采用 BSD-3-Clause。功放派生代码沿用 CirrusAudioFixup 许可，第三方组件保留各自的 GPL 条款，OEM 固件遵循厂商许可。各部分的许可范围与署名见 [LICENSE](LICENSE) 和 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
