# Z-X7 PRO 官方软件逆向笔记（HID 数显支架）

> 对应产品：动力火车（PowerTrain）Z-X7 PRO 显卡支架，页面 `https://powertrain-pc.com/products/170/`
> 官方安装包：`XWANJIA Z-X7 PRO Setup 1.0.11.exe`（页面下载地址 `https://powertrain-pc.com/Uploads/Temp/download/6a2f90184a67b.exe`）
> 分析日期：2026-10-29；以下结论均可在解包后的官方源码中逐条复核。

## 1. 官方软件是什么

| 项目 | 结论 | 证据（解包后路径） |
| --- | --- | --- |
| 技术栈 | Electron 桌面应用 + Node.js 主进程 + 前端 Umi/React | `resources/app.asar`；`package.json` |
| 产品性质 | 同一套 OEM 白标程序，一份代码适配 90+ 品牌（ALSEYE、Thermalright 类、AWEST 等），靠 `appConstant.js` 里每个品牌的配置区分 | `main/constant/appConstant.js` |
| 本机型配置 | `XWANJIA_Z_X7_PRO: { hidOnly: true, needMedia: false, sourceDist: 'AQ_Life', version '1.0.12' }` | `main/constant/appConstant.js` 第 1404–1415 行 |
| 通信通道 | **USB HID 输出报告**，不是串口、不接收图片画面 | `main/_baseClass/device_hid.js` |
| 传感器来源 | 随包 C# 小程序 `SystemInfos.exe` 采集后写共享内存，主进程轮询 | `main/util/system.js`；`SystemInfos.exe` |
| 前端界面 | 仅温度 / 利用率 / 循环切换 / CPU-GPU 选择 / 单位 / 循环间隔等开关，无图片主题编辑器 | `ui/XWANJIA_Z_X7_PRO/dist/574.async.js` |

## 2. 设备识别（VID / PID）

官方主进程通过 `node-hid` 的 `HID.devices()` 枚举，再用白名单过滤（`main/_baseClass/device_hid.js` → `getDeviceList()`）。

- 主系列：**VID = 20785（0x5131）**，PID 范围 **8199–8224（0x2007–0x2024）**，共 28 个公版固件编号；
- 客户自有编号：VID 6700 / PID 17229（要求 `usagePage = 65281`）；VID 3504 / PID 39371（MSI EZ 数显）。

**本机实测枚举记录（2026-10-29）**：支架被识别为 `VID=0x5131 PID=0x2007 usagePage=65280`，产品字符串为空、厂商名显示为系统默认设备名；命中主系列白名单第 1 个编号，**不属于** `usagePage==12` 的新固件精简分支，走通用 64 字节报告。

> 备注：官方同程序里还存在另外两套通道，本机型配置 `hidOnly: true` 不走它们；仅在你后续遇到同品牌屏幕款时参考：
> - **串口 JPEG 屏**（`main/_baseClass/device.js` + `main/_baseClass/serialport.js`）：波特率 **2,000,000**，命令帧 `55 AA | len LE16 | key | payload | sum LE16`；视频帧 `len LE32 | JPEG | sum LE16`。
> - **SPI RGB565 屏**（`main/_baseClass/device_spi.js`）：波特率 **921,600**，帧头 `55 25 21`，后接矩形坐标 8 字节大端、长度大端、RGB565 BE 像素流、XOR 校验。
> - **ARGB 灯条**（`main/_baseClass/device_control.js`）：VID `1A86` / PID `8040`，帧头 `55 12`，大端长度，BGR 三元组，XOR 校验。Z-X7 PRO 页面虽标注 ARGB 灯条，但本安装包 `needMedia:false`、`hidOnly:true`，灯条是否由 HID 报告内的某个字节控制，需上硬件实测确认。

## 3. HID 输出报告（本机型实际通道）

### 3.1 发送方式

- 句柄：`new HID.HID(path)`；发送：`device.write(sendList)`；
- **JS 里 64 字节数组的首字节 `0x01` 是 Report ID，不是数据头**；设备输出报告 65 字节 = 1 字节 Report ID + 64 字节数据；
- 所以线上缓冲是「64 字节数组 + 尾部 `0x00`」，固件收到的数据帧整体左移一位，从 `00 01 02` 开始；
- 发送周期 **200 ms**（`getSendIntervalTime() => 200`）；
- 连续写失败最多重试 10 次，之后关闭设备（`startSendData()` 的容错分支）。

> **2026-10-29 真机点灯实测**：三种候选格式里只有「数组 + `00`」能点亮屏幕，
> 屏上显示 62℃（脚本测试值 62.2℃）；`00 + 数组`、`01 + 数组` 都不亮。
> 复核脚本：`tools/screen_probe.py`、`tools/probe3.py`、`tools/probe_reportid.py`。

### 3.2 通用分支 64 字节位定义

下表「偏移」是**官方 JS 数组下标**（`sendData[i]`）。线上 65 字节缓冲里
`wire[0] = 0x01`（Report ID），`wire[i+1] = sendData[i]`，`wire[64] = 0x00`（补位）。

来源：`main/_baseClass/device_hid.js` → `sendPic()` 回调里的 `sendData` 数组。

| 偏移 | 含义 | 取值 / 编码 |
| --- | --- | --- |
| 0 | **Report ID** | `0x01`（线上首字节，固件不当作数据） |
| 1 | 数据帧头 | `0x00` |
| 2 | 数据帧头 | `0x01` |
| 3 | 数据帧头 | `0x02` |
| 4 | CPU 温度整数 | 先按单位换算，再截断取整，上限 999 |
| 5 | CPU 温度小数部分 | `小数 × 100` 截断，0–99 |
| 6 | CPU 温度单位 | `0` = 摄氏度，`1` = 华氏度 |
| 7 | CPU 利用率整数 | 0–99 |
| 8 | CPU 功率个位/十位 | `floor(W) % 100`；超 100 W 的百位进 35 位 |
| 9 | CPU 功率小数 | `(W - floor(W)) × 100` |
| 10 | CPU 频率高位 | `MHz // 100`，如 4700 MHz → 47 |
| 11 | CPU 频率低位 | `MHz % 100`，如 4700 MHz → 00 |
| 12 | CPU VID 整数 | 电压/相关量 |
| 13 | CPU VID 小数 | ×100 |
| 14 | GPU 温度整数 | 同 CPU 温度规则 |
| 15 | GPU 温度小数 | ×100 |
| 16 | GPU 温度单位 | 0 / 1 |
| 17 | GPU 利用率整数 | 0–99 |
| 18 | GPU 功率 `%100` | 百位进 36 位 |
| 19 | GPU 功率小数 | ×100 |
| 20 | GPU 频率高位 | `MHz // 100` |
| 21 | GPU 频率低位 | `MHz % 100` |
| 22 | CPU 风扇 RPM 高位 | `RPM // 100`（不是大端 u16） |
| 23 | CPU 风扇 RPM 低位 | `RPM % 100` |
| 24 | 第二风扇 RPM 高位 | `RPM // 100` |
| 25 | 第二风扇 RPM 低位 | `RPM % 100` |
| 26 | 年份十位 | 如 2026 → `20` |
| 27 | 年份个位 | 如 2026 → `26` |
| 28 | 月份 | 1–12 |
| 29 | 日期 | 1–31 |
| 30 | 小时 | 0–23 |
| 31 | 分钟 | 0–59 |
| 32 | 秒 | 0–59 |
| 33 | 星期 | `0`=周日，`1`=周一 … `6`=周六（JS `Date.getDay()` 口径） |
| 34 | 内存利用率整数 | 0–99 |
| 35 | CPU 功率百位 | `floor(W) // 100` |
| 36 | GPU 功率百位 | `floor(W) // 100` |
| 37 | 显示模式 | `0` 温度，`1` 利用率，`2` 循环切换 |
| 38 | 循环切换间隔 | 秒，前端默认 6（官方代码默认 5，前端设置默认 6） |
| 39 | 通道标记 | `0` 选 CPU，`1` 选 GPU |
| 40 | 暗屏闪烁开关 | `1` 开，`0` 关（官方默认 1） |
| 41 | 显示模式（副本） | 同 37 |
| 42 | 显示模式（副本） | 同 37（不同固件分支复用） |
| 43 | 内存温度整数 | |
| 44 | 内存温度小数 | ×100 |
| 45 | 内存温度单位 | 0 / 1 |
| 46 | 内存频率高位 | `MHz // 100` |
| 47 | 内存频率低位 | `MHz % 100` |
| 48 | 显示模式 | `0` CPU，`1` GPU，`2` RAM，`3` 循环（MSI 分支口径） |
| 49 | CPU 利用率小数 | ×100 |
| 50 | GPU 利用率小数 | ×100 |
| 51 | 内存利用率小数 | ×100 |
| 52–63 | 保留 | 补 `0x00` |

### 3.3 其它固件分支（备用，实测命中再启用）

`device_hid.js` 在运行时按 PID / usagePage 分流，自研程序先按 3.2 通用分支发送，若显示异常再对照：

- **新固件分支**（`usagePage == 12`）：32 字节精简报告，`[0]=0x00, [1]=0x40`，随后 CPU 温度、占用、频率大端、风扇大端、功耗大端、GPU 同构、内存占用与内存总量大端；本工程 `protocol.build_newhid_report()` 已预留。
- **首席玩家分支**（VID 4791/PID 4097 或 0x5131/0x200F）：14 字节短帧，`00 87 65 01 01 …`，前 14 字节求和取模 256 作为校验和写入第 14 字节。
- **YCC 分支**（VID 6700/PID 17229 且 usagePage 65281）：BCD 三位温度帧 `07 FF FF + BCD 三字节`。

## 4. 数值编码规则（官方实现摘录）

- 单位换算：选华氏度时，官方先 `℉ = ℃ × 9/5 + 32`，再取整；摄氏度直接使用原值（`celsiusToFahrenheit` + `getIntValue`）。
- 温度取整：`Math.floor(华氏换算后的值)`；老固件华氏度另做 `+128` 兼容（`device_hid.js` 中 `_cpu_TempStart = tempInt ? _cpu_Temp_Z + 128 : _cpu_Temp_Z`），本工程默认走标准换算，如真机出现华氏度错位再启用该兼容分支。
- 功率：`floor(W)`；个位/十位进 `[8]/[18]`（`%100`），百位进 `[35]/[36]`（`//100`），小数进 `[9]/[19]`（`小数×100`）。
- 频率（CPU/GPU/内存）：原始值单位 **MHz**；`[10]/[20]/[46] = MHz // 100`，`[11]/[21]/[47] = MHz % 100`，固件侧按两位数字重建 MHz 显示。
- 风扇 RPM：`[22]/[24] = RPM // 100`，`[23]/[25] = RPM % 100`；通用分支**不是**大端 u16（大端 `toUint16Be` 只用于新固件 32 字节分支与内存总量、磁盘总量位）。
- 超范围截断：`getIntValue` 温度上限 999，其它上限 9999；写入 Buffer 时单字节自动按 256 取模。

## 5. 上硬件验证步骤（建议顺序）

1. 装好官方程序，打开任务管理器 → 设备管理器，记录支架的 VID/PID、usagePage、产品名；
2. 退出官方程序（HID 与串口一样，同一时刻只能被一个进程占用）；
3. 运行 `python -m src.cli scan --all`，对照第 2 节白名单找到目标 PID；
4. 运行 `python -m src.cli live --index 0 --mode temp --channel cpu`，观察支架是否显示 CPU 温度；
5. 若全黑/乱码：先试 `protocol.py` 中 `PREPEND_REPORT_ID` 切换；再确认是否命中 3.3 的新固件分支；
6. 用 USB 抓包工具（Windows 下 HID 抓包可参考 https://github.com/pfalcon/intelhex 无关；推荐 `hidapitester` 或 USBPcap + Wireshark）对比官方程序与自研程序发出的 64 字节是否一致。

## 6. 尚未在硬件上确认的项目（交付时已标注为待实测）

- [x] 本机枚举：`VID=0x5131 PID=0x2007 usagePage=65280`，走通用 64 字节分支；
- [x] 不命中新固件（usagePage=12）精简报告分支；
- [ ] Report ID 前置与否（`hid_device.PREPEND_REPORT_ID`，当前 `True`，真机无显示时改为 `False` 对比）；
- [ ] 华氏度老固件 `+128` 兼容分支是否需要；
- [ ] ARGB 灯条是否由本 HID 报告控制、灯珠数量与颜色位位置；
- [ ] 屏幕实际显示字段组合（是否同时显示 CPU/GPU 双温、是否显示风扇转速与时间）。
