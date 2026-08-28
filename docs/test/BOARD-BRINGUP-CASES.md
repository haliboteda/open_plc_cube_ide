# 板级测试 11 项对表

**拿去和硬件工程师逐行过的表。** 2026-08-27 建立，每条结论都出自原理图 / IPC-356 网表 / BOM 的核实，不是凭记忆。

- **测试项**：需求原文
- **测试步骤**：固件做什么 + 人做什么
- **能否直接测量**：✅ 现在就能 / ⚠️ 缺东西 / ❌ 硬件不支持
- **需要的硬件、设备**：没有它就测不了的东西
- **目前步骤的问题**：和需求描述不一致的地方、硬阻塞、判据不成立的原因

## 写法约定

| 说什么 | 怎么写 | 例 |
|---|---|---|
| **固件里配哪个引脚** | **MCU 引脚号**，只写这一种 | `PB13`、`PC3_C` |
| **人往哪接线** | **外接端口的功能名**在前，端子丝印 / 连接器管脚在括号里 | **Digital Out 1**（端子 A03）、**RS232 TxD**（端子 C05，UpperDeck J10-4） |
| BGA ball 号 | 只出现在文末[速查附录](#附引脚与端子速查)，正文不用 | — |

引脚号与信号名的出处是 `Hardware/STM32H743IIK6_GPIO_ASSIGNMENT_Schaeffer_Bridge_20260822.xlsx` 的 `GPIO_ASSIGNMENT` 表；端子与连接器管脚的出处是各板的 `netlist.ipc`。

⚠️ **板上有两个 `J4`，写的时候必须带板名**：**UpperDeck J4** 是模拟量端子的连接器（Analog In 2 / Analog Out 1 / Analog Out 2 / GND），**JunctionLink J4** 是 SPI / I2C 的 20 pin 扩展口。两者的 `J4-3` 完全不是一回事。

⚠️ 端子丝印字母有冲突的见 [§3](#3-端子编号有冲突需要确认)；接线以功能名和连接器管脚为准。

---

## 1. 主表

| # | 测试项 | 测试步骤 | 能否直接测量 | 需要的硬件、设备 | 目前步骤的问题 |
|---|---|---|---|---|---|
| **1** | **Digital IN 回环**<br>PB5+PC6、PB6+PB7、PH10+PH11、PI5+PI6 各一组，一个发电平一个收，比对后打 PASS/FAIL | ① 硬件工程师把每组的**两个 MCU 引脚直接短接**（2026-08-27 确认的接法）<br>② 固件让组里一个脚发、另一个收：发低 → 收端应读 0；释放 → 外部 10k 上拉把线拉高，收端应读 1<br>③ **收发互调再测一遍**，两个方向都通才算这组过<br>④ 四组各自独立核定，失败的报是哪个方向、卡在哪 | ✅ **能，而且不需要 24 V** | 四根短接线（硬件负责）<br>不需要额外仪器 | ✅ **发送端用开漏，不是推挽。** 这八个脚都挂着 LM339 开集输出 + 10k 上拉 + 50R 串阻：开漏拉低走 3V3/10k ≈ 0.33 mA，放开由外部上拉抬高，**万一比较器同时在灌地也不会打架**；推挽在那种情况下是 62 mA，超过 STM32H7 单脚 25 mA 上限<br>✅ **不需要 24 V**：比较器没供电时输出是高阻，线上只剩上拉和那根短接线<br>❗**用例会自检发送端本身有没有拉低**，用来把「线没接」和「这个脚根本没在驱动」分开 —— 两者的现象一模一样，不分开就会误判 |
| **2** | **RELAY**<br>PI8、PI10、PI11、PG7、PG3、PD3 同时循环 2 s 高 / 2 s 低，示波器看波形 | ① 固件六路同时 2 s 吸合 / 2 s 释放，无限循环（复用已有的 `Core/Inc/relay.h`）<br>② **量 MCU 侧**：探头夹 MCU 引脚或 LowerDeck 上 T2–T7 的栅极<br>③ **或量触点**：在某一路**继电器触点**的两端之间串电压源 + 限流电阻，探头量电阻两端 | ⚠️ **要先定量哪里** | 示波器（1 通道够）<br>量触点还要：直流电压源 + 限流电阻 | ❗**继电器触点是干接点，示波器直接夹上去没有任何波形。** 网表里每路只有两个触点管脚（`RYOUT-nA` / `RYOUT-nB`），是同一个接点的两端<br>· 量 MCU 侧最简单，但**证不了继电器真的吸合**<br>· ⚠️ `Hardware_overview.txt:59` 说 Klemmblock B 是「常开/常闭触点对」，网表不支持 —— **待硬件确认** |
| **3** | **AI**<br>PC3_C、PA6 打印检测到的电压值 | ① 可调电压源接 **Analog In 1**（UpperDeck J3-4）或 **Analog In 2**（UpperDeck J4-1），地接 **Analog GND**（UpperDeck J4-4 / J4-5）<br>② 万用表量注入的真实电压<br>③ 固件读 ADC，打印原始计数、ADC 引脚电压、**以及按两种档位分别折算的端子电压**<br>④ 对照跳线实物取其中一个 | ⚠️ **能读数，但判据不成立** | 可调直流电压源 0–10 V<br>数字万用表 | ❗**四处和需求不一致**：<br>① `PC3_C` 是 **ADC3_INP1**，不是 ADC1 通道 13 —— 需要 SYSCFG 模拟开关<br>② **外接端口标号和引脚是反的**：硬件上 Analog In 1 = PC3_C、Analog In 2 = PA6<br>③ **输入有分压，比例约 0.25 不是 1.0**（电压档 68k/22k6 → 端子电压 = ADC × 4.0089；电流档 124 Ω 取样）<br>④ ❗**档位由 4 个焊接跳线决定（JP9+JP5、JP8+JP6），出厂状态在任何文档里都查不到**，而且**注电压分辨不出来**（两档比例 0.2494 vs 0.2490 几乎一样）—— 只能目视看跳线 |
| **4** | **AO**<br>PA4 输出 0.5 V、PA5 输出 1.5 V，DAC 按 RSET=1024 转成电流，期望 4.88 mA / 14.65 mA | ① 电流表串在回路里：**Analog Out 1**（UpperDeck J4-2）→ 表 → **Analog GND**（UpperDeck J4-4 / J4-5）；**Analog Out 2**（UpperDeck J4-3）同理<br>② 固件设 DAC1_OUT1（PA4）= 0.5 V、DAC1_OUT2（PA5）= 1.5 V<br>③ 打印设定电压和按 `Iout = Vin × 10 / 1024` 算的期望电流<br>④ 读表对照 | ⚠️ **能测，但期望值有前提** | 直流电流表 0–25 mA（万用表 mA 档即可） | ✅ **RSET 完全正确** —— BOM 就是 `1024R 1%`（R9/R10），芯片 `XTR111AIDGQT`。4.883 / 14.648 mA 算得准<br>❗**但只在 JP3/JP4 开路时成立**。PA4 到 XTR111 的 VIN 之间是 `10k(R13) + 40k2(R11)→JP3→/VREF` 的网络：JP3 桥接 → VIN 变求和节点 `0.8008×V(PA4) + 0.1992×V(VREF)`，成为 4–20 mA 活零点网络，**两个期望值全变**。JP3/JP4 状态查不到<br>❗**`/VREF` 的来源和电压也查不到**（经 UpperDeck J7-17 从 Bridge 进来，Bridge p5 上找不到）<br>· 顺便确认 JP1/JP2 也开路<br>· **没有软件使能**（OD 脚经 10k 硬接地）<br>· 故障标志 PI4/PE3 **读不出来**（10k 串 + 1k 上拉方向疑似画反，断言低电平只到 ~3.0 V）<br>· ⚠️ R17/R18 = 1 kΩ 直接挂在 DAC 输出上，20 mA 时要从 DAC 缓冲抽约 2 mA —— 重负载，值得实测 |
| **5** | **SPI2**<br>接 SPI 转 USB 调试器到电脑调试 | ① **引脚级**：PI0 / PI1 / PI3 / PC2_C 四根当 GPIO 推挽翻转 + 回读，证明 MCU 球到扩展口这段通<br>② **图样**：外设初始化，持续发固定字节，示波器 / 逻辑分析仪在**扩展口**（JunctionLink J4-03 / J4-04 / J4-05 / J4-06）上量时钟和数据<br>③ **自环**：一根线把**扩展口 SPI2_MOSI**（JunctionLink J4-04）接到 **SPI2_MISO**（JunctionLink J4-03），固件自动比对收发，打 PASS/FAIL<br>④ 有调试器后：板子当主机，对端当从机 | ⚠️ **只有①能**<br>②③④ 缺扩展口接入手段 | **扩展口转接板**<br>示波器 / 逻辑分析仪<br>一根自环线<br>（USB-SPI 调试器，可选） | ❗**扩展口 = `SAMTEC ERF8-010-01-L-D-EM2-TR`，0.8 mm 间距 20 pin —— 杜邦线插不进去**。这是 SPI2 / SPI6 / I2C2 唯一的对外引出<br>❗**MISO 在 `PC2_C` 双焊盘球**，数字的 PC2 只能经 **SYSCFG 模拟开关**到达，**本工程没有任何代码做这件事，这条路从未验证过** —— 项 5 最大的未知项<br>· 引脚不冲突：PI0/PI1/PI3 的 FMC_D24/25/27 复用用不到（本工程 FMC 是 16 位）<br>· NSS 走的是另一条路（Bridge J2 → UpperDeck J8-30），其余三根走 Bridge J3 |
| **6** | **SPI6**<br>接 SPI 转 USB 调试器到电脑调试 | 同项 5 三段，引脚是 PG13 / PG14 / PB4。自环线接**扩展口 SPI6_MOSI**（JunctionLink J4-14）→ **SPI6_MISO**（JunctionLink J4-15） | ⚠️ **只有①能** | 同项 5 | ❗**扩展口接入问题同上**<br>❗**全板找不到 NSS 片选** —— Bridge 原理图、三份网表、扩展口引脚表都没有。单从机可以不用（从机 CS 接地），多从机必须指定一个 GPIO 做软件片选 —— **待硬件确认**<br>· ⚠️ **MISO = PB4 = NJTRST**，要覆盖复用功能。本板调试只用 SWD，所以安全<br>· 不和以太网冲突：本板 RMII 用 PB12/PG12，PG13/PG14 是自由的 |
| **7** | **I2C2**<br>接 I2C 转 USB 调试器到电脑调试 | ① **空闲电平检查**：PH4 / PH5 当输入读，两根都为高才算总线没卡死（有一根恒低直接报，不用往下扫）<br>② **地址扫描**：0x00–0x7F 逐个发地址，记录谁应答<br>③ **引脚级**：GPIO 翻转 + 回读<br>④ 挂从机模块到**扩展口**：SCL = JunctionLink J4-09、SDA = JunctionLink J4-08、GND = JunctionLink J4-07 | ⚠️ **①③能<br>②要有从机才有意义** | **扩展口转接板**<br>**自带上拉的 I2C 从机模块**（EEPROM / RTC / 温湿度模块都行） | ❗**扩展口接入问题同项 5**<br>❗**全板没有任何 I2C 上拉电阻** —— 四块板的网表和 BOM 都查过。**没有上拉，I2C2 一个 bit 都不会动**，挂上去的模块必须自带（多数 breakout 有 4.7k/10k）<br>❗**板上没有任何 I2C 从机** —— 四块板 BOM 全扫（EEPROM/RTC/IO 扩展/温度/ADC/DAC 全无匹配），必须外挂<br>· ⚠️ 信号上串了 `EMI7204MUTAG`（JunctionLink U1–U5），**R/C 值本仓库查不到**，影响上升时间和能选的速率 —— 保守起点 100 kHz<br>· ✅ 好消息：**I2C 的 HAL 驱动已经在工程里而且已启用**，不用复制任何文件<br>· ⚠️ 模块用 **3V3** 供电，不要用扩展口 JunctionLink J4-01 的 5 V 供 5V 逻辑模块再直连 SCL / SDA |
| **8** | **I2C3**<br>接 I2C 转 USB 调试器到电脑调试 | **只能做引脚级自检**：PH7 / PH8 / PH9 当 GPIO 推挽翻转 + 回读，证明 MCU 球和到板间连接器那段走线没问题。到不了板外，证不了任何总线行为 | ❌ **硬件不支持** | 改板，或飞线焊到 Bridge J2 / UpperDeck J8 焊盘 | ❗**完全没有引出。** Bridge 原理图 p5 把 `I2C3-SCL/SDA/SMBA` 画在 **J2** 上，J2 对插 UpperDeck **J8**；而 `UpperDeck/netlist.ipc:390-419` 里 J8 的 pin 1、4、11–18、22、23、24、28、29 全是 `NET-(J8-PIN_11-PAD11)` 这种**单管脚未连接网络**。全仓 grep `I2C` 在 UpperDeck / JunctionLink / LowerDeck 三份网表里**只命中 `I2C_2_*`**<br>**没端子、没插座、没测试点。** 引脚本身是自由的（`.ioc` 没占） |
| **9** | **I2C4**<br>接 I2C 转 USB 调试器到电脑调试 | 同项 8，引脚是 PB8 / PD13 / PD11 | ❌ **硬件不支持** | 同项 8 | ❗**同项 8，完全没有引出** |
| **10** | **UART4**<br>接 UART 转 USB 调试器到电脑调试 | ① USB-**RS232** 适配器接 **RS232 TxD**（端子 C05，UpperDeck J10-4）和 **RS232 RxD**（端子 C06，UpperDeck J10-3），GND 接端子 C02 / C11 / C12<br>② 开机能看到串口菜单打印 → **发送方向通**<br>③ 敲键选用例、菜单响应 → **接收方向通**<br>④ 另有 `TestCase/RS232/rs232_test.c` 的回显模式备用 | ✅ **能，零额外接线** | USB-**RS232** 适配器 | ❗**UART4 就是当前的 printf 控制台**（配在 PC10 / PC11 → MAX3221 → RS232 端子）。`PH13 / PH14` 那对也是 UART4（接到扩展口 JunctionLink J4-11 / J4-12），**但改指过去就会把控制台拿走** —— 这正是 `docs/work/M5-serial-conflict.md` 那次事故（一句 `Serial4.begin()` 挂死 app、UDP 发现失效、要 ST-Link 擦除才能恢复）<br>· PH13/PH14 只有 UART4 和 FDCAN1 两个可选功能，**没有办法同时在扩展口上有 TTL 串口又保住 RS232 端子上的 UART4**<br>· ❗**必须用 RS232 电平的适配器，TTL 适配器接上去可能烧掉适配器**<br>· PB10 必须先拉高（MAX3221 使能），bootloader 已经做了 |
| **11** | **温度检测**<br>PA0、PA3 电压读取，转成温度，`V = 10mV×T + 500mV` | ① 不需要接线，板载传感器<br>② 固件读 PA0 / PA3，打印 ADC 电压和 `T = (mV − 500) / 10`<br>③ 想对照：万用表量测试点 `TP3` / `TP4`（丝印 **T-PS**）和 `TP9` / `TP10`（丝印 **T-HS**）<br>④ 想看读数变化：让 Digital Out 带载发热，PA3（T-HS）会跟着升 | ✅ **能，零接线** | 万用表（可选，用来和测试点对照） | ✅ **公式直接可用。** 传感器 `LM50BIM3/NOPB`（LowerDeck U1/U2），10 mV/°C，−25…+100 °C。**VO 到 MCU 之间逐板核实过：没有任何分压、缓冲、串阻或滤波电容**<br>· PA0 = ADC1_INP16、PA3 = ADC12_INP15，都是普通球，不需要 SYSCFG<br>· 🎁 **板上有测试点**，能立刻分清「传感器不对」还是「ADC 读错」<br>· ⚠️ 输出没有滤波电容，LM50 直接驱动 ADC 采样电容 → **采样时间必须给足**，否则读数偏低<br>· ⚠️ 命名冲突：LowerDeck 原理图把 U1 那路叫 **Verpolschutz（防反接）**，GPIO 表叫 **Short-Circuit Protection**。U1 紧邻 24 V 输入的 F1/D1/D2 组，支持原理图的说法 —— **待确认这一路到底测什么** |

**通用前提**（不接这两条，上面一项都测不了）：

| 接什么 | 到哪个外接端口 | 说明 |
|---|---|---|
| **24 V 电源** | + 接 **+12/+24 V 输入**（端子 A15 / A16，LowerDeck X1-2 / X1-1），− 接 **GND Stromversorgung**（端子 A14，LowerDeck X1-3） | **不只是 Digital Out 要它** —— 整块板的 3V3 和 5V 都从 24 V 降下来，没它 MCU 不跑 |
| **USB-RS232 适配器** | **RS232 TxD**（端子 C05）、**RS232 RxD**（端子 C06），GND 接端子 C02 / C11 / C12 | ⚠️ **必须 RS232 电平** |

---

## 2. 缺件汇总 —— 要请硬件回答的

| # | 缺什么 | 挡住哪几项 | 要回答什么 |
|---|---|---|---|
| **1** | **扩展口的转接手段。** SPI2 / SPI6 / I2C2 唯一的对外引出是 JunctionLink J4，`SAMTEC ERF8-010-01-L-D-EM2-TR`，0.8 mm 间距 20 pin | 5、6、7 | 有没有对插的扩展板？没有能不能做一块把它引成 2.54 排针的转接板？ |
| **2** | **I2C3 / I2C4 完全没有引出**，在 Bridge J2 ↔ UpperDeck J8 对插面就断了 | 8、9 | 产品需求上要不要这两条？要的话是缺口，得改板 |
| ~~3~~ | ~~八个焊接跳线的出厂状态~~ **✅ 2026-08-27 已解决：JP1–JP9 九个全部出厂开路**，判据同 JP7（有焊盘、无贴片坐标、无物料）。档位也已定：AIN1 电压、AIN2 电流。见 [design/HARDWARE-FACTS.md](../design/HARDWARE-FACTS.md) | — | — |
| **4** | **`/VREF` 是什么、多少伏**（经 UpperDeck J7-17 从 Bridge 进来，Bridge p5 上找不到）。⚠️ 这个 `/VREF` 是模拟输出侧 JP3/JP4 的活零点网络，**和 MCU 的 VREF+ 不是一回事** | 4 | 网络来源和电压 |
| **5** | **I2C 全板没有上拉电阻**（四块板都没有） | 7 | 刻意让模块自带，还是漏了？ |
| **6** | **SPI6 全板找不到 NSS 片选** | 6 | 刻意（软件片选）还是漏了？ |
| **7** | **AOUT_EF 电阻方向疑似画反**：`~EF ──10k(R79)── PI4 ──1k(R81)── +3V3`，断言低电平只到 ~3.0 V | 4 的故障读取 | R79/R81、R80/R82 是不是该互换？ |
| **8** | **继电器是干接点**，示波器夹上去没波形 | 2 | Klemmblock B 的两个触点管脚是同一接点两端（网表如此）还是 NO/NC 对（overview 文档如此）？ |
| **9** | **`EMI7204MUTAG` 的 R/C 值查不到**（串在 SPI / I2C / UART 全部信号上） | 7 选速率 | 数据手册或确认料号 |
| **10** | **VNQ5160K-E 的 8 个 STATUS 脚全部未连线**（网表里都是单节点网络） | 无 | 刻意省掉的吗？Digital Out 过流 / 开路 / 过温软件完全看不到 |
| **11** | **PI4 的 BGA ball 号冲突**：GPIO 表 xlsx 说 `D4`，Bridge p5 说 `D14` | 无 | 哪个对 |

**测试设备清单**

| 设备 | 用于 | 状态 |
|---|---|---|
| 24 V 直流电源 | 全部 | ✅ 已接 |
| USB-**RS232** 适配器 | 全部（控制台 + 项 10） | 待确认 |
| 数字万用表 | 项 3、4、11 | 待确认 |
| 示波器（2 通道够） | 项 2；项 5、6 量扩展口 | 待确认 |
| 可调直流电压源 0–10 V | 项 3 | 待确认 |
| 直流电流表 0–25 mA | 项 4 | 待确认 |
| 8 根短接线 | 项 1 | 待确认 |
| **扩展口转接板** | 项 5、6、7 | ❌ **缺件 1** |
| **I2C 从机模块（必须自带上拉）** | 项 7 | ❌ **缺件 5** |
| USB-CAN 分析仪（PCAN 克隆板） | CAN（不在这 11 项内，已单独实现） | ✅ 已有 |

---

## 3. 端子编号有冲突，需要确认

两次独立核实对**同一个连接器管脚**给出了不同的端子丝印字母。**接线以外接端口的功能名和连接器管脚号为准**（网表直接给出），丝印字母请确认一次。

| 外接端口 | 连接器管脚 | 网络（无争议） | 读法 A | 读法 B |
|---|---|---|---|---|
| Analog In 1 | UpperDeck J3-4 | `/AUSGANG/AIN_1` | D12 | **B12** |
| Analog In 2 | UpperDeck J4-1 | `/AUSGANG/AIN_2` | D13 | B13 |
| Analog Out 1 | UpperDeck J4-2 | `AUSGANG/AOUT_1` | D14 | B14 |
| Analog Out 2 | UpperDeck J4-3 | `AUSGANG/AOUT_2` | D15 | B15 |
| Relay 6 触点 | LowerDeck X5-2 / X5-3 | `RYOUT-6A` / `RYOUT-6B` | — | **B11 / B12** |

⚠️ **`B12` 被 Analog In 1 和 Relay 6 触点同时声明了**（两者在不同的板上）。可能是两块板各自的 Klemmblock 字母不同、某次读错了，也可能文档本身重号。

---

## 4. 硬件文档错误 —— 照这些接线会接错

| # | 文件 | 错在哪 | 正确的 |
|---|---|---|---|
| 1 | `Klemmblockzuordnung.pdf` **第 4 页** | 「MPU Pin」整列相对「Klemme」列**错位约 2 行**，连带四处：**CAN_GND 完全漏掉**、RS485 整体挪位、Digital In 8 的引脚写成 PC6、RS232 TxD 的 PB10 / PC10 写反 | CAN_GND = 端子 A09（UpperDeck J11-4）；Digital In 8（端子 D09）= `PI6`；RS232 TxD（端子 C05）的数据脚是 `PC10`，`PB10` 是 MAX3221 使能 |
| 2 | `UpperDeck_overview.txt:20,22` | **J6/J8 写反** | UpperDeck J6 ↔ Bridge J3，UpperDeck J8 ↔ Bridge J2 |
| 3 | `UpperDeck_overview.txt:37` | 说有 **5 颗** LM339LVRTER | BOM 是 **2 颗**（U3/U4，各 4 路 = 8 路 Digital In）。「U3E/U4E」是电源/去耦符号 |
| 4 | `LowerDeck_overview.txt:35-37` | 把 **LM50BIM3 温度传感器说成线性稳压器**，还说 LowerDeck 上有 24V→5V 稳压器 | LM50 是温度传感器。**LowerDeck 上没有 24V→5V 稳压器**，5V0 从 JunctionLink 经 J1-1 送来 |
| 5 | `Hardware_overview.txt:59` | 说 Klemmblock B 是常开 / 常闭触点对 | 网表里每路只有两个触点管脚，是同一接点两端。**待确认（缺件 8）** |
| 6 | GPIO 表 `xlsx` | 行 65 把 `SPI2_MISO` 拼成 `SPI2_MIMO`；`Num` 列 107 出现两次；PI4 的 ball 号说 `D4`，原理图说 `D14` | 前两个笔误。第三个**冲突未解决（缺件 11）** |

---

## 5. 固件侧的状态（不用问硬件，记录用）

**项 1、2、3、4、11 的用例 2026-08-27 已经写好，一次烧录同时跑。** 入口 `TestCase/common/bringup_test.c` 的 `BringUp_Test_Run()`，在 `Core/Src/main.c` 里由 `BRINGUP_TEST_ENABLE` 打开（和 `KNX_TEST_ENABLE` / `CAN_TEST_ENABLE` 互斥，多开一个会 `#error`）。

| 项 | 代码在哪 | 用什么外设 |
|---|---|---|
| 1 Digital IN 回环 | `TestCase/DIN/din_test.c` | 纯 GPIO |
| 2 RELAY | `bringup_test.c` 直接复用 `Core/Src/relay.c` | 纯 GPIO |
| 3 AI | `TestCase/ADC/adc_test.c` | ADC3（PC3_C）+ ADC1（PA6） |
| 4 AO | `TestCase/DAC/dac_test.c` | DAC1 两个通道 |
| 11 温度 | `TestCase/ADC/adc_test.c` | ADC1（PA0、PA3） |

**能同时跑的依据**：五项没有一个引脚重叠，也没有一个碰到 SDRAM 那 39 根 FMC 引脚（PG3 = FMC_A13、PD3 = FMC_CLK 的复用本工程没用，SDRAM 时钟走 PG8、地址只到 A12）。唯一的共享资源是 **ADC1**，被项 3 和项 11 同时需要 —— 所以两项写在同一个文件里、共用一个句柄；两次 `HAL_ADC_Init()` 打同一个实例会把前一次的配置静默覆盖掉。

每一项都是非阻塞 tick。**项 2 要给示波器看 2 s 方波，所以任何一项都不许长时间阻塞** —— 项 1 的八通道扫描因此写成状态机而不是 `HAL_Delay` 循环。

串口按键（RS232 端子 C05/C06，115200 8N1）：`1` `2` `3` `4` `b` 单独开关某一项（`b` = 项 11），`a` 全开，`?` 看帮助。

⚠️ **一处物理耦合不是故障**：项 1 在切 Digital Out 的负载，而那份热量正是项 11 的 T-HS 传感器测的东西。跑起来 T-HS 缓慢上升是板子在正常工作。

⚠️ **项 3 有一个从未验证过的点**：`PC3_C` 的 SYSCFG 模拟开关（`SYSCFG_PMCR.PC3SO`）复位默认是关闭（PC3_C 与数字 PC3 相连）。该开还是该关本工程从来没在硬件上验过，所以固件**开关各读一次、两个值都打出来**，一次上电就把它实测掉。

**还缺的：**

| 缺什么 | 挡住 | 怎么补 |
|---|---|---|
| **SPI 的 HAL 驱动** | 项 5、6 | 从 `STM32Cube_FW_H7_V1.12.1` 复制到 `TestCase/common/` —— **该包 HAL 版本正是 1.11.5，和本工程一致**（ADC / FDCAN / DAC 三次都走通了这条路） |
| I2C 的 HAL 驱动 | 项 7 | **已在 `Drivers/` 且已启用**，不用动 |

**`.ioc` 一律不动**（Digital In / Digital Out / ADC / DAC / SPI / I2C 全都不在里面），`Core/Inc/stm32h7xx_hal_conf.h` 一律不动 —— 用 `TestCase/common/testcase_hal_guard.h` 那套本地模块开关的做法。DAC 那条守卫 2026-08-27 已经补上。

---

## 附：引脚与端子速查

**BGA ball 号只在这一节出现**，供硬件量球时用；正文一律用 MCU 引脚号。

**项 1 · Digital Out → Digital In 回环对照**

| 通道 | 外接端口（发） | 端子 | MCU 引脚 | BGA | 外接端口（收） | 端子 | MCU 引脚 | BGA |
|---|---|---|---|---|---|---|---|---|
| 1 | Digital Out 1 | A03 | PB13 | P13 | Digital In 1 | D02 | PC6 | H15 |
| 2 | Digital Out 2 | A04 | PB0 | R5 | Digital In 2 | D03 | PB5 | A6 |
| 3 | Digital Out 3 | A05 | PH15 | D13 | Digital In 3 | D04 | PB6 | B6 |
| 4 | Digital Out 4 | A06 | PE4 | B1 | Digital In 4 | D05 | PB7 | B5 |
| 5 | Digital Out 5 | A07 | PA8 | F15 | Digital In 5 | D06 | PH10 | L13 |
| 6 | Digital Out 6 | A08 | PA9 | E15 | Digital In 6 | D07 | PH11 | L12 |
| 7 | Digital Out 7 | A09 | PI7 | C2 | Digital In 7 | D08 | PI5 | C4 |
| 8 | Digital Out 8 | A10 | PE5 | B2 | Digital In 8 | D09 | PI6 | C3 |

GPIO 表里 Digital Out 的信号名是 `HSFET_1`–`HSFET_8`（高端驱动 FET 控制输出），Digital In 是 `DIN1`–`DIN8`（兼作编码器 Enc1a–Enc4b）。

**项 2 · 继电器对照**

| 外接端口 | MCU 引脚 | BGA | 驱动 MOSFET | 触点连接器 | 文档标的端子 |
|---|---|---|---|---|---|
| Relay 1 触点 | PI8 | D2 | T7 (SI2356DS) | LowerDeck X8-1 / X8-2 | B01 / B02 |
| Relay 2 触点 | PI10 | E3 | T6 | LowerDeck X8-3 / X7-1 | B03 / B04 |
| Relay 3 触点 | PI11 | E4 | T5 | LowerDeck X7-2 / X7-3 | B05 / B06 |
| Relay 4 触点 | PG7 | J14 | T4 | LowerDeck X6-1 / X6-2 | B07 / B08 |
| Relay 5 触点 | PG3 | K15 | T3 | LowerDeck X6-3 / X5-1 | B09 / B10 |
| Relay 6 触点 | PD3 | D11 | T2 | LowerDeck X5-2 / X5-3 | B11 / B12 |

继电器 `HF41F/005-HST`（**5 V 线圈**，从 `5V0` 供电），续流 `BAS516`（D3–D8），栅极下拉 33k（R3–R8）。六路都已在 `.ioc` 里配成 GPIO 输出且 `Locked=true`。

**项 3、4、10、11 · 模拟量、温度、串行端子对照**

| 外接端口 | 端子 / 连接器管脚 | MCU 引脚 | BGA | ADC / DAC 通道 |
|---|---|---|---|---|
| Analog In 1 | UpperDeck J3-4 | PC3_C | M5 | ADC3_INP1（需 SYSCFG 模拟开关） |
| Analog In 2 | UpperDeck J4-1 | PA6 | P3 | ADC12_INP3 |
| Analog Out 1 | UpperDeck J4-2 | PA4 | N4 | DAC1_OUT1 |
| Analog Out 2 | UpperDeck J4-3 | PA5 | P4 | DAC1_OUT2 |
| （AOUT1 故障标志，读不出来） | 板内 | PI4 | D4 / D14 冲突 | — |
| （AOUT2 故障标志，读不出来） | 板内 | PE3 | A1 | — |
| （板载温度，防反接侧 T-PS） | 测试点 TP3 / TP4 | PA0 | N3 | ADC1_INP16 |
| （板载温度，高端开关侧 T-HS） | 测试点 TP9 / TP10 | PA3 | R2 | ADC12_INP15 |
| RS232 TxD | 端子 C05，UpperDeck J10-4 | PC10 | B14 | — |
| RS232 RxD | 端子 C06，UpperDeck J10-3 | PC11 | B13 | — |
| （RS232 收发器使能，必须先拉高） | 板内 | PB10 | R12 | — |
| CAN H / CAN L | 端子 C08 / C07，UpperDeck J10-1 / J10-2 | PB9（TX）/ PI9（RX） | B4 / D3 | — |
| RS485 A / B | 端子 A10 / A11，UpperDeck J11-3 / J11-2 | PD5（TX）/ PD6（RX） | C11 / B11 | — |
| （RS485 使能 / 方向） | 板内 | PI2 / PD4 | C14 / D10 | — |

**项 5–7 · 扩展口 JunctionLink J4**（`SAMTEC ERF8-010-01-L-D-EM2-TR`，0.8 mm 20 pin）

| 管脚 | 信号 | MCU 引脚 | BGA |
|---|---|---|---|
| J4-01 | +5 V ⚠️ 信号都是 3V3 | — | — |
| J4-02 / J4-07 | GND | — | — |
| J4-03 | SPI2_MISO | **PC2_C** | M4 |
| J4-04 | SPI2_MOSI | PI3 | C13 |
| J4-05 | SPI2_SCK | PI1 | D14 |
| J4-06 | SPI2_NSS | PI0 | E14 |
| J4-08 | I2C2_SDA | PH5 | J4 |
| J4-09 | I2C2_SCL | PH4 | H4 |
| J4-10 | I2C2_SMBA | PH6 | M11 |
| J4-11 | UART4_TX | PH13 | E12 |
| J4-12 | UART4_RX | PH14 | E13 |
| J4-13 | SPI6_SCK | PG13 | A8 |
| J4-14 | SPI6_MOSI | PG14 | A7 |
| J4-15 | SPI6_MISO | **PB4** = NJTRST | A9 |
| J4-16 – J4-19 | SWO / JTDI / SWCLK / SWDIO | PB3 / PA15 / PA14 / PA13 | A10 / A13 / A14 / A15 |
| J4-20 | DEBUG_TRIGGER | PC7 | G15 |

**未引出的**：I2C3 = PH7 / PH8 / PH9（N12 / M12 / M13），I2C4 = PB8 / PD13 / PD11（A5 / M15 / N14）。

**端子块 —— 外接端口一览**

| 块 | 外接端口 | 端子 | 连接器管脚 |
|---|---|---|---|
| **A**（LowerDeck） | GND Digital Out | A02 / A12 | X4-3 / X2-1 |
| | **Digital Out 1 – 8** | **A03 – A10** | X4-2, X4-1, X3-4, X3-3, X3-2, X3-1, X2-4, X2-3 |
| | GND Stromversorgung | A14 | X1-3 |
| | **+12/+24 V 输入** | **A15 / A16** | X1-2 / X1-1 |
| **D**（UpperDeck） | GND | D01 | J1-1 |
| | **Digital In 1 – 8** | **D02 – D09** | J1-2, J1-3, J1-4, J2-1, J2-2, J2-3, J2-4, J3-1 |
| | GND | D10 / D11 | J3-2 / J3-3 |
| **C**（UpperDeck） | **RS232 TxD / RxD**（控制台） | **C05 / C06** | J10-4 / J10-3 |
| | CAN L / CAN H | C07 / C08 | J10-2 / J10-1 |
| | CAN_GND | A09 | J11-4 |
| | RS485 A / B | A10 / A11 | J11-3 / J11-2 |
| **模拟**（UpperDeck，丝印字母待确认见 [§3](#3-端子编号有冲突需要确认)） | Analog In 1 / Analog In 2 | 见 §3 | J3-4 / J4-1 |
| | Analog Out 1 / Analog Out 2 | 见 §3 | J4-2 / J4-3 |
| | Analog GND | — | J4-4 / J4-5 |

（A01、A11、A13 的丝印未核实。C 和 A 两套字母并存指同一批端子 —— `Klemmblockzuordnung.pdf` 叫 C，KiCad 原理图第 1 页叫 A。）
