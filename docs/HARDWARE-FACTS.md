# 硬件实测事实

这里每一条都核实过，标注了核实方式和日期。**没核实的不要往这里写。**

> 排查引脚问题时 `Hardware/Production/UpperDeck/netlist.ipc`（IPC-D-356 网表）比原理图 PDF 快得多：
> `grep -E "U5 +-[0-9]+" netlist.ipc` 直接列出每个引脚在哪个网络。

## RS232 路径（PC10 / PC11 / PB10）

核实：KiCad 原理图 `OpenPLC_UpperDeck_R3.pdf` 第 5 页（2026-08-07）+ `netlist.ipc` 逐脚（2026-08-10）。

MAX3221EIPWR（U5），网名 `TXD_RS232_PC10`、`RXD_RS232_PC11`、`ENABLE_RS232_PB10`：

- **PC10 → U5 DIN**（收发器输入，高阻）
- **PC11 ← U5 ROUT**（收发器**输出**，推挽 —— 绝不能从外部往这里灌信号，曾因此出现"发得出收不到"的现象）
- **PB10 = 使能**，高有效，经 MOSFET Q5
- 外部设备接**端子 C05 (TxD) / C06 (RxD) + GND (C02/C11/C12)**，那里是真正的 RS232 电平（±12V）—— **TTL 适配器接上去可能烧掉适配器**

### PB10 拉低 = 整片关断，不只是"不能读"

一个 GPIO 驱动 MAX3221 的**两个**控制脚，其中一条经 MOSFET **反相**：

| PB10 | U5-12 `FORCEOFF` | Q5 (SI2356DS) | U5-1 `EN` | 芯片状态 | 能发能收 |
|---|---|---|---|---|---|
| 低（R68 下拉，**默认**） | 0 | 关断 → R67 上拉 | 1 = 接收器输出高阻 | **Shutdown，电荷泵停转** | 都不行 |
| 高 | 1（`FORCEON` U5-16 永久接 3V3） | 导通 → 漏极接地 | 0 = 接收器输出使能 | Normal | 都行 |

关键：RS-232 的 ±5V 线电平由芯片内部**电荷泵**升压产生，泵一停，发送驱动器**物理上产生不出电平** —— 所以 **PB10 为低时 printf 一个字节都到不了线上**。

⚠️ 函数名 `Enable_RX_RS232` 只描述了 `EN`（接收器三态）那一半，漏掉了 `FORCEOFF`（整片开关）这更重要的一半，容易误判成"只影响接收"。

### 默认是关的

Arduino core 的 `cores/arduino/main.cpp` 只做了 `pinMode(PB_10, OUTPUT)`，从不写电平，所以引脚停在低位、收发器**默认关闭**。sketch 必须自己 `digitalWrite(RS232_EN_Pin, HIGH)`。

bootloader 侧在 `Core/Src/main.c:174` 显式调 `Enable_RX_RS232()`，**在 `MX_UART4_Init()` 和所有 printf 之前**，所以 bootloader 的日志（含 `bootloader_state_init()` 在 `server_decide()` 里打的那些）是通的。

⚠️ **推论：core 在 `main.cpp:168` 打的 `[BOOT] millis=` 到不了端子。** 那行在 171 行的 `pinMode` 之前，更在 sketch 拉高之前 —— 收发器还关着。**在 core 里加"启动时打印"之前先想清楚这一条**，否则加了也看不见。详见 [IAP-STATUS.md](IAP-STATUS.md)。

## UART4 被重复占用（潜在 bug，未修）

`HardwareSerial Serial_Test(PC_11, PC_10)`（core `main.cpp`）解析到 **UART4**（AF8），而 `Serial4` / `Serial` 在 PH13/PH14 上**也是 UART4**。

`uart_handlers[]` 每个外设只有一个槽位，**最后一次 `begin()` 赢**。所以将来任何 `Serial4.begin()`（或把 USB 菜单切到 "CDC (no generic 'Serial')"）都会**静默掐掉 `Serial_Test` 的接收**，而发送看起来一切正常。

修法：`HardwareSerial Serial_Test(PC_11_ALT1, PC_10_ALT1)` 挪到没人用的 USART3（AF7）。**尚未实施** —— 需要硬件验证，并且要提交到共享的 core 包。

## USB 菜单影响 `Serial` 的含义

当前 FQBN 用的是 `usb=CDCgen`，此时 `Serial` 被 `#define` 成 `SerialUSB`，**它不是硬件串口**。已在设备上证实：全用 `Serial` 的回显 sketch 是通过 USB CDC 回显的。

## PG9 就是 BOOT0 网

核实：Bridge 原理图第 5 页网表 + 实测（2026-08-12）。

网络 `KNX_Prog_KEY` = **PG9（U1 ball C10）+ BOOT0（ball D6）+ SW2 pin1 + R58 (10k) + J2 pin9**。

**PG9 不是普通 GPIO，它决定 MCU 的启动源。**

**R58 = 下拉到 GND**（实测：PG9 配 PULLUP / PULLDOWN / NOPULL 各读一次，三次全 0）。由此：

1. BOOT0 网静态为低 → 复位必然从 flash 启动，不会进 ROM DFU
2. `boot0_is_pressed()` 的 active-high 判断**正确**（SW2 按下拉到 3V3）
3. PG9 **不需要**配成输出。当前的推挽输出驱动低有隐患：**按下 SW2 = 3V3 经引脚对地短路**。用户已知情并选择保持现状

> 早先"改成 input 后 RESET 失效"的真正原因，是同一次改动里删掉了 `SystemClock_Config()`（VOS3 + 64MHz + 0 等待周期超规格读 flash），已修复。**不是 PG9 的问题。**

## SRAM4 的 no-init 机制

链接脚本 `STM32H743IIKX_FLASH.ld` 第 58 行把 SRAM4 前 32 字节挖出来给 boot handoff 记录（`RAM_D3 ORIGIN = 0x38000020`）。

启动代码不碰这 32 字节，所以放在那里的变量**能活过软复位**（掉电仍然丢）。

**将来需要"跨软复位保留、又不想依赖 VBAT"的状态时，用这个机制扩展即可**，不要另起炉灶去用 RTC 备份寄存器。

> 曾经有三个链接脚本、其中两个没做这个保留，导致 cmake 路径会静默踩掉 handoff 记录。**已解决**：那两个已删除，`CMakeLists.txt` 和 `cmake/gcc-arm-none-eabi.cmake` 现在都指向唯一的那个。

## 文档错误

`Hardware/UpperDeck_overview.txt` 第 77 行（端子表 C05 那行）把 PB10 和 PC10 写反了。同文件第 60 行是对的。**以 KiCad 为准。**

## 蓝牙 COM 口会冻住串口工具（主机侧问题，与固件无关）

**现象**：板子一上电，PC 上另一个串口工具（**不同端口**）卡死约 30 秒；拔掉板子 USB 就正常。

**原因**：Windows 的"蓝牙链接上的标准串行"COM 端口。设备接入 → 广播 `WM_DEVICECHANGE` → 串口工具重扫端口 → 打开蓝牙 SPP 口时会真的去连蓝牙，对端不在就等到超时。工具在 UI 线程同步阻塞，整个程序冻住。

**处理**：设备管理器里禁用所有"蓝牙链接上的标准串行"。2026-08-12 实测有效。不影响蓝牙耳机（走 A2DP/HFP，与 SPP 无关）。**配对新蓝牙设备后可能重新出现。**

**教训**：遇到"插上 A 就影响 B"，先分清卡住的是**整个应用**还是**那个端口的数据**。前者几乎必然是主机侧端口枚举被拖住，与固件无关。这次在固件侧查了 DFU、LPM、PHY 时钟门三个方向，全不是原因。
