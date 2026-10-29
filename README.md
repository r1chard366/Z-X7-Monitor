# Z-X7 PRO 控制器

动力火车 / XWANJIA **Z-X7 PRO 显卡支架**的第三方上位机。
原生 Win32 + GDI+，单文件 335 KB，常驻内存约 20 MB，无 Python / .NET / Electron 运行时。

- 官方产品页：<https://powertrain-pc.com/products/170/>
- 官方程序是 Electron 白标程序；本工程只是让自己的程序与自己的硬件通信

## 功能

| 功能 | 说明 |
| --- | --- |
| CPU / GPU 温度 | 分段控件一键切换，数据同时上报给支架屏 |
| 多显卡选择 | GPU 通道下出现 `GPU 1/2/3` 芯片，选哪张卡就显示哪张卡的温度/占用/频率 |
| 查询间隔 | 0.5–60 秒：对数滑块、`−` / `+` 按常用档位步进、**点击数字直接输入**、滚轮微调；自动保存 |
| 传感器服务 | 随软件启动（CPU / GPU 通道都会启动）；掉线后 15 秒内自动拉起；退出软件才结束 |
| 浅色 / 深色 | 标题栏右侧太阳 / 月亮切换；首次启动跟随系统，选择会记住；托盘菜单也可切换 |
| 开始 / 停止 | 停止后不再写 HID，屏幕几秒后熄灭 |
| 托盘常驻 | 关闭收进托盘；`Ctrl+Alt+Z` 随时唤回窗口 |
| 开机自启 | 登录计划任务，静默启动、无窗口、不占任务栏 |

设置保存在 `HKCU\Software\ZX7Pro`：`interval`（毫秒）、`dark`（0 浅色 / 1 深色）、`gpu`（选中显卡）、`channel`、`closeToTray`。

界面：无边框圆角窗口、红黄绿交通灯、卡片、分段控件、对数滑块、圆角步进器、开关（macOS 风格），浅色 / 深色两套配色。

## 目录

```
zx7-pro-tool/
├─ ZX7Pro.exe                    编译产物（等于 dist\ZX7Pro.exe）
├─ sensors/                      传感器组件（必须与 exe 同级）
│  └─ LibreHardwareMonitor/      LibreHardwareMonitor + ZX7SensorHost.exe
├─ native/
│  ├─ build.bat                  一键编译
│  ├─ res/app.rc, app.ico        图标资源
│  └─ src/
│     ├─ main.cpp                主程序（界面 / HID / 托盘 / 自启）
│     └─ sensors/SensorHost.cs   传感器宿主源码
└─ docs/PROTOCOL_HID.md          HID 协议说明（65 字节报告位定义）
```

## 权限模型（主程序不需要管理员）

| 功能 | 需要的权限 |
| --- | --- |
| 界面、HID 上报、托盘、GPU 温度、开机自启 | **普通用户**（清单 `asInvoker`，启动不弹 UAC） |
| CPU 核心温度 | 内核驱动读 MSR，需**一次性**安装传感器服务 |

一次性安装（右键以管理员身份运行）：

```
安装传感器服务.bat
```

内部就是创建一个最高权限的按需任务：

```
schtasks /Create /TN ZX7ProSensor /TR "...\ZX7SensorHost.exe" /SC ONCE /ST 00:00 /RL HIGHEST /F
```

之后主程序**启动时**自动 `schtasks /Run /TN ZX7ProSensor`，CPU/GPU 通道切换不再结束宿主；
退出程序时才 `schtasks /End`，**全程不再需要 UAC**。

## 传感器链路

```
ZX7Pro.exe（普通权限）
   └─ schtasks /Run ZX7ProSensor
        └─ ZX7SensorHost.exe（最高权限，隐藏窗口）
             └─ LibreHardwareMonitorLib.dll → PawnIO 驱动 → 读 CPU/GPU 温度
                  └─ 每 0.5 秒写 sensors\LibreHardwareMonitor\zx7sensors.txt
```

主程序启动时自动拉起宿主，之后每轮询一次读这个文本（`cpu=43.5;gpu=61.0;...`）；
宿主意外退出时，界面每 15 秒检查一次并自动重启（看门狗），只有退出软件时才结束宿主。

依赖：

- **PawnIO**：<https://pawnio.eu/>（驱动，装一次；LibreHardwareMonitor 0.9.4+ 必需）
- **LibreHardwareMonitor**：<https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases>
  （解压到 `sensors\LibreHardwareMonitor\`）

## 编译

需要 Visual Studio 生成工具（MSVC x64）+ Windows SDK：

```powershell
cd native
.\build.bat
# 产物 native\build\ZX7Pro.exe
```

`build.bat` 已包含：静态 CRT、按体积优化、`/MANIFEST:EMBED` +
`requireAdministrator`（托盘图标和传感器宿主都需要管理员权限）。

传感器宿主单独编译（一般不用改）：

```powershell
csc /target:winexe /r:sensors\LibreHardwareMonitor\LibreHardwareMonitorLib.dll ^
    /out:sensors\LibreHardwareMonitor\ZX7SensorHost.exe native\src\sensors\SensorHost.cs
```

## 命令行

| 参数 | 作用 |
| --- | --- |
| `-tray` | 静默启动：只驻留托盘，不显示窗口（开机自启用） |
| `-cpu` / `-gpu` | 指定启动时的读数通道 |

## 开机自启

程序内「开机自启动」开关会创建计划任务：

```
schtasks /Create /TN ZX7Pro /TR "\"<exe路径>\" -tray" /SC ONLOGON /RL HIGHEST /F
```

用计划任务而不是 `Run` 键，是因为 `Run` 键只能以普通权限启动，托盘会被系统拒绝
（`Shell_NotifyIcon` 返回 5）。程序同时会删除历史遗留的 `Run` 键，避免两个实例抢跑。

## 常见现象

| 现象 | 原因 / 处理 |
| --- | --- |
| 托盘区看不到图标 | Windows 11 默认收进 `^` 溢出区；拖出来，或设置 → 个性化 → 任务栏 → 其他系统托盘图标 → 打开 ZX7Pro |
| 启动弹 UAC | 正常：托盘图标和 CPU 温度都需要管理员 |
| 弹「Windows 已保护你的电脑」 | 未签名 + 网络标记；右键 exe → 属性 → 勾「解除锁定」，或直接运行本地副本 |
| CPU 显示 `--` | 看 `sensors\LibreHardwareMonitor\zx7sensors.txt`：`adm=0` 宿主没提权；文件不存在宿主没启动；`cpu=-1.0;adm=1` 多半是 PawnIO 没装 |
| 屏幕黑屏 | 官方程序退出后设备会熄灭；本程序运行时会持续供帧 |

## HID 协议（简述）

输出报告固定 **65 字节** = Report ID `0x01` + 64 字节数据：

```
[0]=0x01(ReportID) [1]=0x00 [2]=0x01 [3]=0x02
[4][5]   CPU 温度整数/小数      [6]  温度单位
[7]      CPU 利用率             [10][11] CPU 频率 MHz//100 / %100
[14][15] GPU 温度整数/小数      [17] GPU 利用率
[20][21] GPU 频率               [26..33] 年月日时分秒星期
[37]     显示模式(0温度/1利用率/2循环)   [39] 通道(0 CPU/1 GPU)
```

完整位定义见 `docs/PROTOCOL_HID.md`。
