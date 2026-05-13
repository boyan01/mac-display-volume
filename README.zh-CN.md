# Mac Display Volume

Mac Display Volume 让 macOS 可以像控制普通扬声器一样，调节外接显示器的音量。

有些显示器通过 HDMI、DisplayPort 或 USB-C 接入 Mac 后，macOS 只能把它们当作
固定音量的音频输出设备。系统音量键和菜单栏音量滑块可能不可用，或者调节的是显示器
自身的硬件音量。这样一来，如果同一台显示器还会连接 Windows、游戏机或其他设备，
音量状态就很容易互相影响。

Mac Display Volume 的做法是在 Mac 上创建一个虚拟音频输出设备：

```text
macOS apps
  -> Mac Display Volume
  -> software volume control
  -> your real display speaker
```

你把系统输出切到 `Mac Display Volume`，然后把真实显示器音频设备设置为它的目标设备。
之后 macOS 的音量控制会先在软件层调整音量，再把声音转发到显示器。显示器自己的硬件音量
可以保持固定，不会因为你在 macOS 上调节音量而被改掉。

## 适合谁

- 你使用带扬声器或音频输出的外接显示器。
- macOS 无法正常调节这台显示器的音量。
- 你希望音量调节只影响当前 Mac，不改变显示器自身音量。
- 同一台显示器还会给 Windows、游戏机或其他设备使用。

## 功能

- 创建一个 macOS 虚拟音频输出设备。
- 支持系统音量和静音控制。
- 将音频低延迟转发到真实显示器音频设备。
- 通过菜单栏应用选择目标输出设备。
- 支持一键切换到虚拟输出设备。
- 保持显示器硬件音量不变。

## 系统要求

- macOS 15 或更新版本。
- Apple Silicon Mac。
- 一台支持音频输出的外接显示器。

## 安装

当前版本面向本地构建和安装，需要安装 Xcode 26 或更新版本。

```sh
Scripts/install-local.sh
```

安装脚本会构建应用和 HAL driver，并安装到：

- `/Applications/Mac Display Volume.app`
- `/Library/Audio/Plug-Ins/HAL/MacDisplayVolumeAudio.driver`

安装过程中可能需要输入管理员密码。安装后如果系统没有立刻识别新的音频设备，可以重启
CoreAudio；如果仍然不出现，重启电脑通常可以清理 CoreAudio 的旧缓存和旧 driver helper。

## 使用

1. 打开 `/Applications/Mac Display Volume.app`。
2. 在目标设备里选择真实的显示器音频设备，例如 `P275MV`。
3. 点击 **Apply Driver Config**。
4. 点击 **Use Virtual Output**。
5. 保持显示器自身硬件音量固定。

之后就可以使用键盘音量键、菜单栏音量滑块或系统设置来调节 `Mac Display Volume` 的音量。

## 注意事项

- 目标输出设备需要支持 `48 kHz` 采样率。
- 不要把 `Mac Display Volume` 自己设置成目标设备。
- 如果声音延迟开始累积，先使用 **Reset Relay**。
- 如果 CoreAudio 状态异常，可以使用 **Restart coreaudiod**，或者直接重启电脑。

## 卸载

```sh
Scripts/uninstall-local.sh
```

卸载脚本会删除应用和 HAL driver，并重启 CoreAudio。

## 工作原理

Mac Display Volume 包含两部分：

- SwiftUI 菜单栏应用：负责选择目标设备、应用配置、切换默认输出。
- CoreAudio HAL driver：负责提供虚拟输出设备、处理软件音量和转发音频。

driver 使用一个固定大小的 relay buffer 转发音频。当排队音频过多时，会丢弃最旧的
音频帧并重新对齐时间线，以避免累积数秒级延迟。

## 许可证

Apache License 2.0。
