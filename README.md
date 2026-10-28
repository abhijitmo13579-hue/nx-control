# NX Control

A tiny Windows tray app to control **Nothing Headphone (1)** from your PC. No browser, no Electron, no install.

- **95 KB** single `.exe`
- About **2 MB of RAM** while running
- Talks to the headphones directly over Bluetooth, the same way the phone app does

> Unofficial project. Not affiliated with, sponsored by, or endorsed by Nothing Technology Limited.
> "Nothing" and "Nothing Headphone (1)" are trademarks of their owner and are used here only to describe compatibility.

## Features

| Feature | Options |
|---|---|
| Noise control | High, Mid, Low, Adaptive, Transparency, Off |
| Equalizer | Balanced, More bass, More treble, Voice, Custom |
| Bass enhancement | Off, Level 1 to 5 |
| Low latency mode | On / Off |
| Dual connection | On / Off |
| Battery | Shown in the menu and on hover |
| Extras | Device picker, start with Windows, global hotkeys |

**Hotkeys** (work from any app):

- `Ctrl + Alt + Shift + N` cycles Noise cancelling, Transparency, Off
- `Ctrl + Alt + Shift + L` toggles low latency

**Custom EQ** uses the custom curve saved from the phone app. Set it once on your phone, then switch to it from the PC anytime.

## Install

1. Pair your Nothing Headphone (1) with Windows (Settings, Bluetooth & devices) and make sure it shows **Connected**.
2. Download `NXControl.exe` from [Releases](../../releases).
3. Run it. The file is not code-signed, so Windows SmartScreen may warn you. Click **More info**, then **Run anyway**.
4. A headphone icon appears near the clock. On Windows 11 new icons start hidden under `^`. To pin it: Settings, Personalization, Taskbar, Other system tray icons, turn **NX Control** on.
5. Click the icon to open the menu.

To update, right-click the icon, choose **Exit**, replace the `.exe`, and run it again.

## Requirements

- Windows 10 or 11 (64-bit)
- A Bluetooth adapter
- Nothing Headphone (1). Other Nothing and CMF devices use the same protocol and may partly work, but they are untested and some menu options may not apply.

## Troubleshooting

| Problem | Fix |
|---|---|
| Icon is grey / "not connected" | Make sure the headphones are connected to this PC in Bluetooth settings, then click **Refresh**. |
| "Did not accept the change" | Close the Nothing X app on your phone. The headphones allow only one control connection at a time. |
| Dual connection will not turn on | It usually cannot be used together with LDAC. Switch the codec to standard (AAC) on your phone first. |
| Turning dual connection off drops a device | Expected. The headphones keep only one link. |

## Build from source

The app is one C file using only the Win32 API. It cross-compiles from Linux with MinGW-w64:

```sh
sudo apt install gcc-mingw-w64-x86-64
./build.sh
```

`build.sh` produces `NXControl.exe`. Icons can be regenerated with `python3 tools/make_icons.py src` (needs Pillow).

On Windows you can build with MSYS2 (MinGW-w64 toolchain) using the same commands found in `build.sh`.

## How it works

The headphones expose a Bluetooth RFCOMM channel (channel 15, service "NT LINK") that accepts small binary frames:

```
55 60 01 | command (LE16) | length (LE16) | sequence | payload | CRC-16/MODBUS (LE16)
```

NX Control opens that channel only when you refresh or change a setting, then closes it again. Nothing runs against the radio in the background, apart from a quiet battery refresh every 10 minutes.

| Setting | Read | Write |
|---|---|---|
| Noise control | `C01E` | `F00F` |
| EQ preset | `C01F` | `F010` |
| Advanced EQ switch | `C04C` | `F04F` |
| Bass enhancement | `C04E` | `F051` |
| Low latency | `C041` | `F040` |
| Dual connection | `C027` | `F01A` |
| Battery | `C007` | |

## Credits

The protocol work was done by these MIT-licensed projects. NX Control would not exist without them:

- [omarchy-nothing-ear](https://github.com/r-witz/omarchy-nothing-ear) by Raphaël Witz
- [nothing-toolbox](https://github.com/marc-alexis-com/nothing-toolbox) by Marc-Alexis
- [nothing-x-mac](https://github.com/Senooy/nothing-x-mac) by Younes Khelifi

Their license notices are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

[MIT](LICENSE)
