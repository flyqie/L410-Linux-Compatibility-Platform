# L410 Linux 应用兼容平台

通过 Wayland/XWayland 与鸿蒙 AnCo 接通 Linux 应用窗口，提供输入法、剪贴板、托盘、终端和应用管理。

`linux-compat/src` 为核心与界面源码，`control` 为管理服务源码；HAP 配置和资源位于 `app`、`engine-host`、`hap-installer`；`scripts` 和 `build-support` 包含构建脚本与配置。不包含服务启动和设备部署脚本。

依赖：Python 3、ARM64 GNU 工具链、Clang/LLD 18、HarmonyOS/DevEco SDK、匹配设备的鸿蒙 ABI 库、Debian ARM64 sysroot、Wayland、wlroots 0.18.2、XWayland、GLib/json-glib、IBus、libgbinder/libglibutil、libhybris、图形开发库、dbus-next；ABI 适配生成器需要 Capstone、Keystone、pyelftools。构建路径与签名材料需自行配置。

运行时构建入口为 `linux-compat/scripts/build-anco-linux-runtime.sh`。设置 `TEXT_INPUT_V2_XML`、`TEXT_INPUT_V3_XML` 指向上游 text-input v2/v3 XML；构建时通过 `wayland-scanner`（原构建使用 1.21.0）生成协议绑定，不随源码保存生成文件。HAP 使用 `build-app.py --console`、`--hap-installer` 或 `--engine-host`。

本项目采用 GNU GPL v3（GPL-3.0-only），见 `LICENSE`。第三方文件保留各自原有许可声明。
