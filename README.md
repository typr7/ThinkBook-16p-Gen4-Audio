# ThinkBook 16p Gen4 Audio

**macOS internal speaker support for the Lenovo ThinkBook 16p Gen4.**

ALC287 · Dual CS35L41 · Intel I2C7 · x86_64

[中文](README_zh.md)

ThinkBook 16p Gen4 Audio is a board-specific port of [hoaug-tran's CirrusAudioFixup](https://github.com/hoaug-tran/CirrusAudioFixup). It handles amplifier initialization, OEM DSP firmware, speaker tuning, factory calibration, playback monitoring and fault shutdown.

AppleHDA and AppleALC provide the codec route. `ThinkBookAudio.kext` manages the two CS35L41 amplifiers on that route, and `ThinkBookI2C.kext` provides bounded-polling I2C transfers. A launchd service starts the audio clock and initializes the amplifiers after login or wake.

This project was completed by Codex + GPT-6.1 Sol.

## Supported hardware

| Component | Configuration |
| --- | --- |
| Laptop | Lenovo ThinkBook 16p Gen4 |
| Codec | Realtek ALC287, `10ec:0287` |
| Subsystem ID | `17AA:38A9` |
| Amplifiers | Two CS35L41 rev B2, I2C addresses `0x40` and `0x41` |
| Boost topology | External boost with VSPK switch |
| I2C controller | Intel I2C7, `8086:51D9`, PCI `00:10.1` |

**Validated configuration:** version 1.0.0 on macOS Sequoia 15.7.8, x86_64, with AppleALC 1.9.8, Lilu 1.7.3, `alcid=11` and 48 kHz stereo output. Both speaker channels produce sound.

Initialization uses the board's OEM firmware and tuning, together with the target machine's `CirrusSmartAmpCalibrationData` EFI variable. The driver matches the calibration records to both amplifier UIDs and checks them again after OTP initialization.

## Build from source

### Requirements

- Intel macOS host.
- Xcode Command Line Tools: `xcode-select --install`.
- Python 3 and Git.
- MacKernelSDK at the commit listed below.
- The three OEM firmware/tuning files and the VoodooI2C helper plugins listed below.

### Dependencies and build

Run these steps from the project root.

1. Fetch MacKernelSDK:

   ```sh
   git clone https://github.com/acidanthera/MacKernelSDK.git MacKernelSDK
   git -C MacKernelSDK checkout 7af1933c27aefcbdf4809ee44478829aad30f9c1
   ```

2. Download Lenovo's [Realtek Audio Driver for ThinkBook 16p G4 IRH (DS562451)](https://support.lenovo.com/us/en/downloads/ds562451-realtek-audio-driver-for-windows-11-64-bit-thinkbook-16p-g4-irh). The package is **Realtek_Audio_6.0.9715.1_WHQL_TB16P_DAX**, filename [`mxth077frkj92df0.exe`](https://download.lenovo.com/consumer/mobiles/mxth077frkj92df0.exe).

   Run the package on Windows and extract the driver files to a folder such as `C:\Drivers\ThinkBookAudio`. The three files are under `source/ThirdParty/cirrus/csaudioext/` in the extracted package. Copy them into the project's `firmware/` directory:

   | File | Subdirectory under `csaudioext/` | Purpose |
   | --- | --- | --- |
   | `halo_cspl_RAM_revB2_29.63.1.wmfw` | `PB_6.61.1/FW/CS35L41/RevB2/` | DSP firmware |
   | `17AA38A9_221222_V0_A0.bin` | `tunings/Lenovo_17AA38A9_20221222/` | Left-channel tuning |
   | `17AA38A9_221222_V0_A1.bin` | `tunings/Lenovo_17AA38A9_20221222/` | Right-channel tuning |

   For an installed OEM driver, the same filenames can also be located under `C:\Windows\System32\DriverStore\FileRepository`. The build checks their size and SHA256 against [Resources/OEM-firmware.json](Resources/OEM-firmware.json).

3. Download and extract the official [VoodooI2C 2.9.1 Release package](https://github.com/VoodooI2C/VoodooI2C/releases/tag/v2.9.1):

   ```sh
   mkdir -p dependencies
   curl -fL --retry 3 -o dependencies/VoodooI2C-2.9.1-RELEASE.zip \
     https://github.com/VoodooI2C/VoodooI2C/releases/download/v2.9.1/VoodooI2C-2.9.1-RELEASE.zip
   echo "368e5eb60794583c340764813bd7e8244e94d3d3c06ea5b67034addd539487f4  dependencies/VoodooI2C-2.9.1-RELEASE.zip" | shasum -a 256 -c -
   ditto -x -k dependencies/VoodooI2C-2.9.1-RELEASE.zip dependencies/VoodooI2C-2.9.1
   ```

   This creates the directory used by the build:

   ```text
   dependencies/VoodooI2C-2.9.1/VoodooI2C.kext/Contents/PlugIns/
   ├── VoodooGPIO.kext
   ├── VoodooI2CServices.kext
   └── VoodooInput.kext
   ```

4. Run the host tests and build:

   ```sh
   python3 test.py
   python3 build.py --sdk ./MacKernelSDK --firmware-dir ./firmware \
     --plugins ./dependencies/VoodooI2C-2.9.1/VoodooI2C.kext/Contents/PlugIns \
     --out ./dist
   ```

   `build.py` calls `Tools/embed_firmware.py` to validate the three files, convert their contents to C byte arrays, and write `dist/.build/generated/Devices/CS35L41/Resources/Firmware.hpp`. The compiler includes this header in `ThinkBookAudio.kext`.

   To generate and inspect the header separately, run:

   ```sh
   python3 Tools/embed_firmware.py --firmware-dir ./firmware \
     --out ./dist/.build/generated/Devices/CS35L41/Resources/Firmware.hpp
   ```

Host tests cover calibration matching, OTP UID extraction, I2C FIFO handling and timeouts, output limits, playback shutdown, and firmware input validation.

### Build output

| Output in `dist/` | Purpose |
| --- | --- |
| `ThinkBookAudio.kext` | CS35L41 amplifier and DSP driver |
| `ThinkBookI2C.kext` | I2C controller driver and imported helper plugins |
| `audioctl`, `audio-clock` | Driver control and audio-clock helpers |
| `start-audio.sh`, `local.numw.thinkbook-audio.plist` | launchd initialization service |
| `BUILD-INPUTS.json`, `SHA256.json` | Build-input records and output hashes |

## Install the build output

### File locations

Mount the EFI partition used to boot macOS, then place the build output as follows:

| Build output | Destination |
| --- | --- |
| `dist/ThinkBookI2C.kext` | `EFI/OC/Kexts/ThinkBookI2C.kext` |
| `dist/ThinkBookAudio.kext` | `EFI/OC/Kexts/ThinkBookAudio.kext` |
| `dist/audioctl`, `dist/audio-clock`, `dist/start-audio.sh` | `/Library/Application Support/ThinkBookAudio/` |
| `dist/local.numw.thinkbook-audio.plist` | `/Library/LaunchDaemons/local.numw.thinkbook-audio.plist` |
| `dist/BUILD-INPUTS.json`, `dist/SHA256.json` | Build archive, for input and output verification |

Copy each complete kext bundle, including `ThinkBookI2C.kext/Contents/PlugIns`.

### OpenCore configuration

Merge the five entries in [Resources/OpenCore-Kernel-Add.plist](Resources/OpenCore-Kernel-Add.plist) into the existing `config.plist → Kernel → Add`, in the listed order. Update existing VoodooI2C entries to the ThinkBookI2C paths. Each bundle ID uses one enabled entry.

| Order | `BundlePath` | `ExecutablePath` |
| --- | --- | --- |
| 1 | `ThinkBookI2C.kext/Contents/PlugIns/VoodooInput.kext` | `Contents/MacOS/VoodooInput` |
| 2 | `ThinkBookI2C.kext/Contents/PlugIns/VoodooI2CServices.kext` | `Contents/MacOS/VoodooI2CServices` |
| 3 | `ThinkBookI2C.kext/Contents/PlugIns/VoodooGPIO.kext` | `Contents/MacOS/VoodooGPIO` |
| 4 | `ThinkBookI2C.kext` | `Contents/MacOS/VoodooI2C` |
| 5 | `ThinkBookAudio.kext` | `Contents/MacOS/ThinkBookAudio` |

All five entries use `PlistPath = Contents/Info.plist`, `Enabled = True` and `Arch = x86_64`. Load Lilu and AppleALC before these entries, and set `alcid=11` in `NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82 → boot-args`.

### macOS initialization service

From the project root, install the helpers with root ownership and load the service:

```sh
sudo install -d -o root -g wheel -m 755 "/Library/Application Support/ThinkBookAudio"
sudo install -o root -g wheel -m 755 dist/audioctl dist/audio-clock dist/start-audio.sh "/Library/Application Support/ThinkBookAudio/"
sudo install -o root -g wheel -m 644 dist/local.numw.thinkbook-audio.plist /Library/LaunchDaemons/
sudo launchctl bootout system/local.numw.thinkbook-audio 2>/dev/null || true
sudo launchctl enable system/local.numw.thinkbook-audio
sudo launchctl bootstrap system /Library/LaunchDaemons/local.numw.thinkbook-audio.plist
```

Restart, log in, select the internal speakers and set the output to 48 kHz stereo in Audio MIDI Setup. Check the driver state:

```sh
"/Library/Application Support/ThinkBookAudio/audioctl" status
```

`ARMED_PROTECTED_PLAYBACK` indicates initialization is complete and playback monitoring is active. Service logs are saved to `/var/log/thinkbook-audio.log`.

## References

| Project | Used for |
| --- | --- |
| [hoaug-tran/CirrusAudioFixup](https://github.com/hoaug-tran/CirrusAudioFixup) | Original amplifier driver |
| [Ayase-kira/ThinkBook-16p-G5-IRX-Audio](https://github.com/Ayase-kira/ThinkBook-16p-G5-IRX-Audio) | Board-port and calibration workflow reference |
| [hoaug-tran/VoodooI2C](https://github.com/hoaug-tran/VoodooI2C), [VoodooI2C/VoodooI2C](https://github.com/VoodooI2C/VoodooI2C) | I2C transport |
| [VoodooGPIO](https://github.com/VoodooI2C/VoodooGPIO), [VoodooInput](https://github.com/acidanthera/VoodooInput) | Helper interfaces |
| [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) | Kernel build headers and libraries |
| [AppleALC](https://github.com/acidanthera/AppleALC), [Lilu](https://github.com/acidanthera/Lilu) | Codec routing and runtime dependencies |

Original tools and documentation use BSD-3-Clause. The amplifier derivative retains the CirrusAudioFixup license; vendor components retain their GPL terms. OEM firmware follows its vendor license. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for component scope and attribution.
