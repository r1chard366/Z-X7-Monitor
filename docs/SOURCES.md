# GitHub 检索结论与可复用开源项目清单

## 1. 直接回答：有没有现成的第三方软件？

**没有。** 2026-10-29 在 GitHub 代码/仓库检索 `XWANJIA`、`powertrain-pc`、`Z-X7 PRO`、`ht-1303622853`、`AQ_Life`、`iHAVECPU`、`GAMEPOWER` 等关键词，**没有任何**面向这款支架的第三方驱动、控制工具或协议逆向仓库。

原因：官方程序是一套**白标 OEM 内核**（一个 Electron 工程批量贴牌给 90+ 品牌），终端用户分散，社区没有人单独为 Z-X7 PRO 做第三方工具；数显支架本身只是 HID 从机，上位机协议锁在官方 `app.asar` 主进程里。

可走的两条路：

1. **自己复刻上位机**（本工程已搭好骨架）：协议已从官方主进程逆向出来，用 Python 直接发 HID 报告，再用 PyInstaller 打成单文件 EXE；
2. **复用同类开源项目的成熟模块**（传感器采集、HID 访问、打包流程），而不是指望现成控制软件。

## 2. 同类参考仓库（按可复用部分分类）

### 2.1 同形态：USB 数显/监控面板上位机（结构最值得参考）

| 仓库 | 语言 / 许可证 | 与本设备的关系 | 可复用点 |
| --- | --- | --- | --- |
| [mathoudebine/turing-smart-screen-python](https://github.com/mathoudebine/turing-smart-screen-python) | Python，**GPL-3.0**，约 2.3k stars | 硬件不同（Turing/TURZX 串口 IPS 屏，接收图片），但都是"上位机采集传感器 → 打包发送 → 小屏显示"的同一产品形态 | 传感器聚合代码、主题/布局结构、多平台运行、`requirements` 与打包组织方式 |
| [Lexonight1/thermalright-trcc-linux](https://github.com/Lexonight1/thermalright-trcc-linux) | Python，**GPL-3.0**，231 stars | 同属"水冷/支架数显控制器 HID 协议逆向"，设备是 Thermalright 控制器、Linux 平台 | HID 枚举、输出报告构造、定时上报循环、逆向报文注释的组织方式，是本工程最贴近的同类样本 |
| [syntensa/libre-panel](https://github.com/syntensa/libre-panel) | Python，**GPL-3.0**，配套 Tiling 面板/USB 屏 | 硬件不同 | LibreHardwareMonitor 集成方式、主题编辑器、Windows 下硬件监控数据流 |
| [Agentry433/turing-smart-screen-python-HIDdev2](https://github.com/Agentry433/turing-smart-screen-python-HIDdev2) | 派生，**GPL-3.0** | 把 Turing 库改造给 HID 类设备 | HID 后端替换串口后端的做法 |

### 2.2 传感器采集（本工程缺 CPU 温度 / AMD 显卡时直接接这套）

| 仓库 | 许可证 | 用途 |
| --- | --- | --- |
| [LibreHardwareMonitor/LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) | **MPL-2.0**，C#，9.3k stars | 读取主板 CPU 温度、风扇 RPM、AMD/NVIDIA 显卡温度与频率；可挂 WMI 命名空间 `Root\LibreHardwareMonitor` 供 Python 读取。本工程 `sensors.collect_lhm()` 已预留接口 |
| [giampaolo/psutil](https://github.com/giampaolo/psutil) | BSD-3-Clause | CPU 占用率、内存占用、CPU 频率，跨平台无管理员要求，本工程已使用 |
| NVIDIA 官方 `nvidia-smi` | 随驱动分发，无独立开源协议 | N 卡温度/占用/功耗/核心频率查询，本工程 `_query_nvidia_smi()` 已实现 |

### 2.3 USB / HID 底层访问

| 仓库 | 许可证 | 用途 |
| --- | --- | --- |
| [libusb/hidapi](https://github.com/libusb/hidapi) | BSD-3-Clause / 专有双许可（可商用） | 跨平台 HID 访问库；Python 绑定 `hidapi` 包即基于它，本工程已使用 |
| [node-hid/node-hid](https://github.com/node-hid/node-hid) | MIT | 官方程序实际使用的 HID 绑定，可对照枚举字段（`vendorId/productId/usagePage/path/product/manufacturer`） |
| OpenRGB（主仓库在 [GitLab CalcProgrammer1/OpenRGB](https://gitlab.com/CalcProgrammer1/OpenRGB)，GitHub 有镜像） | **GPL-2.0** | 大量 RGB 控制器的 USB/HID 协议逆向案例；若后续要控制 Z-X7 PRO 的 ARGB 灯条，可查它的设备协议库是否已覆盖同方案控制器 |

### 2.4 打包成 EXE

| 工具 | 许可证 | 用途 |
| --- | --- | --- |
| [pyinstaller/pyinstaller](https://github.com/pyinstaller/pyinstaller) | GPL（带打包例外，生成的 EXE 不被传染） | 本工程 `build_exe.bat` 即用它把 Python 程序打成单文件 Windows EXE |
| [Nuitka/Nuitka](https://github.com/Nuitka/Nuitka) | Apache-2.0 | 备选：编译为原生 C，体积更小、反编译门槛更高 |

## 3. 许可证注意事项

- 上表 **GPL-3.0 / GPL-2.0** 仓库的代码**不能直接抄进闭源分发的 EXE**：只要分发二进制，按协议必须公开对应源码。本工程默认只借鉴架构思路，传感器/HID 代码全部自写；
- `hidapi`（BSD）、`psutil`（BSD）、`LibreHardwareMonitor`（MPL-2.0，文件级 copyleft，改动需回馈但不传染整体工程）可在自有程序中链接调用；
- 本工程未复制官方程序任何代码、图片、主题或字符串；协议位定义属于互操作性逆向分析结果。

## 4. 检索覆盖范围与缺口

- 已检索：GitHub 仓库/代码搜索、通用网页搜索，关键词覆盖品牌名、机型、OEM 内部标识、HID/serial/RGB 同类项目；
- 未覆盖：GitHub 私有仓库、Gitee/国内镜像、B站/知乎/贴吧民间二改工具、付费论坛逆向帖；如果后续发现国内小众二改版本，优先核对它修改的是前端 UI 还是 HID 发送层，后者才是可借鉴对象。
