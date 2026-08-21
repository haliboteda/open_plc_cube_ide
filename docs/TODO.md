# 待办

**大框架（证书 / owner 槽 / SDRAM staging）处理完之后再动这些。** 建于 2026-08-16。

每条按 [WORKING-AGREEMENTS.md](WORKING-AGREEMENTS.md) 的骨架写：**是什么 / 做什么用的 / 在哪找 / 可能的影响 / 解决方案**，有多条路就给对比表。

路径约定：不带前缀 = 本仓库；`core:` = Arduino 板卡包；`$TOOL:` = IAPTranfer_Tool。行号会漂，改到附近时顺手更新。

**这里只放动作，不放事实**（事实在 IAP-STATUS / HARDWARE-FACTS / JOURNAL / OWNERSHIP）。**做完就删掉，不要打勾留着。**

**这里也只放零散的动作。** 需要立项的模块（有设计空间、要分步做）在 [handover/Todo/BACKLOG.md](handover/Todo/BACKLOG.md)，那边一个模块一份，含设计思路、分步计划和验收用例。下面几条已经搬过去了，正文保留是因为它们记着排查细节：

| 这里 | 立项版 |
|---|---|
| C2 `Serial_Test` 抢 UART4 | [handover/Todo/M5-serial-conflict.md](handover/Todo/M5-serial-conflict.md) |
| C3 FMC 的 39 个脚 | [handover/Todo/M4-fmc-pin-guard.md](handover/Todo/M4-fmc-pin-guard.md) |
| D3 app 侧 SDRAM | [handover/Todo/M3-app-sdram.md](handover/Todo/M3-app-sdram.md) |

---

# A · 一次上板顺手做完

攒够了一起烧，别为单条跑一趟 ST-Link。

## A4 · 所有权命令还没进出货工具 IAPTool

| | |
|---|---|
| **是什么** | bootloader 的 `takeown` / `setowner` / `getowner` 三个命令 |
| **做什么用的** | 客户把板子绑到自己的签名密钥上（需求 C10） |
| **在哪找** | 板子侧 `IAPServer/IAP_server.c` + `IAPServer/owner_slot.c`，**已完成并实测**<br>主机侧只有 `$TOOL:TestTool/tools/run-takeown.ps1` / `run-setowner.ps1`，**那是内部测试脚本** |
| **可能的影响** | ⚠️ **客户拿不到这个功能。** 板子侧齐了，但出货工具 `IAPTool` 一个入口都没有 —— 客户没有受支持的办法认领自己的板子。[RELEASE-NOTES.md](../RELEASE-NOTES.md) 已写明这一点，别让文档跑到实现前面 |

**要做什么**：`IAPTool takeown <ip>`、`IAPTool setowner <ip> --current-key=... --new-key=...`。签名那半已经有了（`IAPTool signraw`，2026-08-18 加的），剩下的是命令面和把私钥管理讲清楚。

⚠️ **`takeown` 要求操作员按住 BOOT0**，所以工具必须能把这件事讲明白并等人操作 —— 这是个 UX 问题，不只是加个子命令。

---

## A3 · PG9 改输入应该挪到 `.ioc` 里（用户已认领）

| | |
|---|---|
| **是什么** | `BOOT0_ConfigureAsInput()`，`Core/Src/gpio.c` 的 USER CODE 块，由 `main.c` 在 `MX_GPIO_Init()` 之后调用 |
| **做什么用的** | 把 CubeMX 生成的 PG9 推挽输出改回输入。理由和实测见 [HARDWARE-FACTS.md](HARDWARE-FACTS.md) |
| **为什么现在长这样** | 2026-08-18 那次是**受控实验**，只想动一个变量；重新生成整个工程会引入第二个变量 |
| **要做什么** | 在 `.ioc` 里把 PG9 设成 GPIO_Input，重新生成，然后**删掉 `BOOT0_ConfigureAsInput()` 和 main.c 里的调用** |
| **不做会怎样** | 🟢 功能上没影响 —— 它只是把已经配好的引脚再配一遍。但会变成"没人敢动的重复代码"，而且下一个人看到生成段仍写着 `OUTPUT_PP` 会困惑 |

⚠️ **`.ioc` 改完必须重新验一次 RESET 按钮和 BOOT0 长按** —— 重新生成会动整个 `MX_GPIO_Init()`，不只是这一个脚。

## ~~A1 · bootloader 自报版本号还是 `0.1.2`~~ ✅ 2026-08-17 已做

改成 `"0.1.3"`，实测两侧一致：`BOOTLD_0.1.3` / `CUSAPP_0.1.3`。检查单条目保留在 [../RELEASE-NOTES.md](../RELEASE-NOTES.md)。**下次发版仍要人工核对这两处。**

<details><summary>原始记录</summary>

### ~~bootloader 自报版本号还是 `0.1.2`~~

| | |
|---|---|
| **是什么** | `OPENPLC_FW_VERSION`，一个字符串宏 |
| **做什么用的** | bootloader 的自我标识。拼进 UDP 发现回复和 CDC 探测回复的身份串（`name_uid_role_version`），也是 `info` 命令的返回值 |
| **在哪找** | `Core/Inc/IAP_config.h:16` 定义<br>`IAPServer/IAP_server.c:119` 拼身份串<br>`Core/Inc/IAP_config.h:19` 拼 `BOOT_LOADER_VERSION`<br>对应的 app 侧是 `core:boards.txt:25` 的 `OPEN-PLC.build.fw_version` = `0.1.3` |
| **可能的影响** | **不影响功能**，纯误导。IAPTool 日志里同一块板出现两个版本号，排查时会以为固件没升上去 |
| **解决方案** | 改 `IAP_config.h:16` 为 `"0.1.3"`，一行。已进 [../RELEASE-NOTES.md](../RELEASE-NOTES.md) 发版检查单 |

**为什么不做得更彻底**（已否决，别重新讨论）：

| 方案 | 代价 | 结论 |
|---|---|---|
| 手工同步两处 + 检查单 | 0 | **采用**。发版时核一次 |
| 让工具同时报 boot/app 两个版本 | 要动 identity 字符串格式 → Go 侧 `parseBoardInfoFromReply` + core + bootloader **三仓**，还要重跑发现流程回归 | 否决。为省一次人工核对不值得动刚验证完的协议 |

</details>

---

## A2 · `[BOOT]` 行前有个乱码字节

| | |
|---|---|
| **是什么** | bootloader 交权给 app 前后，串口上多出一个乱码字节 |
| **做什么用的** | —（纯故障现象） |
| **可能的影响** | **纯显示问题**，不影响任何功能。但会让人怀疑串口配置错了，浪费排查时间 |

**原记的根因（"printf 没发完撞上 app 的 begin()"）已确认不成立** —— `Core/Src/main.c:87` 的 `__io_putchar` 是 `HAL_UART_Transmit(&huart4, &ch, 1, 10)`，**逐字节阻塞、末尾等 TC 标志**，返回时最后一个字节已完整移出。

**2026-08-17 实测把范围缩到两个。** 串口原文：

```
** APP Mod ...
[?[BOOT] millis=12      ← 乱码紧贴在 [BOOT] 前面
UART echo ready         ← 前面是干净的
```

`UART echo ready` 是 sketch 在 `setup()` 里拉高 PB10 **之后**打的，**它前面没有乱码** —— 所以电荷泵爬升不产生乱码。

| | 窗口 | 在哪找 | 机理 | 状态 |
|---|---|---|---|---|
| ⚠1 | DeInit 之后、关收发器之前 | `IAPServer/IAP_server.c:481` | PC10（MAX3221 的 DIN）被释放成浮空，而芯片还开着 → 输出跟噪声抖 | **候选** |
| ⚠2 | 关收发器那一刻 | `IAPServer/IAP_server.c:482`（定义在 `Core/Src/usart.c:151`） | 电荷泵停转，线电平从 mark 塌向 0V → 接收端判成起始位 | **候选** |
| ~~⚠3~~ | ~~sketch 打开收发器那一刻~~ | — | ~~电荷泵启动爬升~~ | ❌ **已排除**（我原先标的"最高把握"） |

### 2026-08-17 已改顺序，⚠1 也排除了 —— 乱码仍在

`Disable_RX_RS232()` 已挪到 `HAL_UART_DeInit()` **之前**（先让线上静默，再释放引脚）。**这个改动本身是对的并已保留** —— 让一颗还通电的收发器的输入悬空，本来就不该做。

但重烧后 `[?[BOOT]` **一模一样还在**。所以 **⚠1 也排除**，只剩：

> **⚠2：关断收发器时电荷泵塌陷，线电平从 mark 滑向 0V，接收端把它判成一个起始位。**

这是**主动关断收发器的固有代价**，不是 bug。而"交权时让 app 拿到一块冷板子的收发器状态"是刻意设计（见 `server_jump_to_app()` 注释）。

**剩下的选项，都不便宜：**

| 方案 | 代价 |
|---|---|
| **接受它**，文档写明 | 0。一个纯显示字节 |
| 关断前多发几个空闲位，把塌陷推到有效数据之后 | 要试出需要几个位，且塌陷时间随电容容差变 |
| 干脆不关收发器 | ❌ 违反"app 拿到冷板子状态"的设计 |

**我建议接受。** 它不影响任何功能，而且现在根因是清楚的 —— 有人再问就指这里，不用重查。

> ⚠️ 顺带：这条也解释了为什么 `[BOOT] millis=12` 能出来 —— 塌陷不是瞬时的，12 毫秒后余电仍够。见 B1。

---

# B · 卡在别人身上

## B4 · SDRAM 的 D1 线要修（要硬件动手，挡住 S4a/S4b）

**是什么**：MCU `PD15` 到 SDRAM（AS4C32M16SB-7BIN）D1 脚之间开路或接触不良。现象、证据链和排除过程在 [IAP-STATUS.md](IAP-STATUS.md) 的「SDRAM 数据线 D1」一节，**这里不重复**。

**做什么用的**：SDRAM 是 IAP 的暂存区。不修，**任何上传都停在 checksum**，[handover/TEST-PLAN.md](handover/TEST-PLAN.md) 的 **S4a / S4b** 一条都跑不了，**E8** 也就永远停在 ⬜。

**在哪找**：`Core/Src/fmc.c` 的 `HAL_FMC_MspInit()`（引脚表）；D1 = `PD15`；SDRAM 在 `0xC0000000`。

**可能的影响**：⚠️ **这是推断，不是实测** —— 除 IAP 暂存之外，`OpenPLC_SDRAM` 库（[handover/Todo/M3-app-sdram.md](handover/Todo/M3-app-sdram.md)）也用这片内存，所以 app 侧 SDRAM 功能在这块板上同样不可信。M3 的 19/19 是 2026-08-17 在这块板上过的，**那时这根线还是好的**。

**解决方案**：

1. **万用表量通断**：MCU `PD15` ↔ SDRAM D1 脚。原理图在 `Hardware/`（**只有内网 Forgejo 一份**）。
2. 通的话，量一下有没有虚焊 —— 间歇性开路万用表可能量不出来，轻压/加热能让它变化。
3. 修好后跑 `pwsh ./tools/flash-bootloader.ps1`，看 T0 是否回到 `SDRAM staging buffer OK`。
4. **第二块板到货后先跑一次 T0** —— 如果新板子也失败，那就不是这一块的偶发焊接问题，而是设计/工艺问题，要回头找硬件。

**已排除**（细节见 IAP-STATUS，别重新试）：软件回归、上一个 app 留下的坏状态、SDCLK 时序、MCU 引脚损坏、外部接线拉住 `PD15`。

## B1 · `[BOOT] millis=` 靠电荷泵余电才出得来（要不要修，等你定）

**2026-08-17 实测已回答"从哪个口看到的"：COM5，就是 RS232 端子 C05/C06。** 下面那个"到不了端子"的推论是错的，机理见 [IAP-STATUS.md](IAP-STATUS.md) —— `millis=12`，电荷泵余电还没塌，banner 蹭出去了。

**剩下的是设计决策**：

| 方案 | 代价 | 风险 |
|---|---|---|
| **不改**，文档写明"靠余电，不可依赖" | 0 | app 启动一慢这行就没了，而没有任何东西保证那个时间 |
| core 在打 banner 前主动拉高 PB10 | 小 | ⚠️ **抢走用户 app 对收发器初始状态的控制权**，可能违反[「设计不能限制用户的 app」](WORKING-AGREEMENTS.md) |

⚠️ 无论选哪个，**都不要再往 core 的这个位置加启动打印** —— 后加的越多，越可能超过余电窗口而静默丢失。

<details><summary>原始记录（推论已被实测否定，保留以免重走）</summary>

### ~~到底是从哪个口看到的（等用户确认）~~

| | |
|---|---|
| **是什么** | app 启动时打的一行诊断 banner |
| **做什么用的** | 让人知道 app 已经接管、并看到启动耗时 |
| **在哪找** | `core:cores/arduino/main.cpp:60` 打印<br>`core:cores/arduino/main.cpp:168` 调用点<br>`core:cores/arduino/main.cpp:171` `pinMode(RS232_EN_Pin, OUTPUT)` —— **在 168 之后**<br>对照：`Core/Src/main.c:174` bootloader 的 `Enable_RX_RS232()` 在所有 printf **之前**，所以它的日志是通的 |
| **可能的影响** | **代码上这行到不了 RS232 端子** —— 打印时 PB10 还是低，MAX3221 处于 shutdown、电荷泵停转，物理上产生不出 ±5V。所以以前在串口看到的 `[BOOT]` **一定不是从 C05/C06 出来的**。这个矛盾不解开，A2 就不知道该从哪查 |

**要你回答的**：当时是在哪个口看到 `[BOOT]` 的？

**实测答案：COM5，就是 RS232 端子。** 代码事实（PB10 打印时确实是低的）没错，错的是"所以发不出去"这个推论 —— 没考虑电荷泵的余电。

</details>

---

## B2 · 两块板的 MAC 是否互不相同（等第二块板）

| | |
|---|---|
| **是什么** | MAC 地址从芯片 UID 派生的算法 |
| **做什么用的** | 取代原先写死的 `00:80:E1:00:43:21` —— 那个值所有板子相同，同一网段放两台直接冲突 |
| **在哪找** | bootloader `LWIP/Target/ethernetif.c` 的 USER CODE MACADDRESS 块<br>core `core:libraries/OpenPLC_Net/src/ethernetif.c`（**跨仓镜像，改一处必须改另一处**） |
| **可能的影响** | 单板已验证：两侧串口都打印 `02:BB:49:3E:A8:02`，同 IP。但**"两块板不同"从未观察过** —— 如果派生算法有缺陷，量产时才会暴露成大面积 IP 冲突 |
| **解决方案** | 拿到第二块板，同网段同时上电，比对两边串口打印的 MAC。**这条进量产检查单** —— 检查单本身现在就能写，不用等板子 |

---

## ~~B3 · `OpenPLC_Bootloader.md` 已经严重过期~~ ✅ 2026-08-17 已同步

**没删，改成"只讲工程结构、不讲行为"**，并挂进了 [INDEX.md](INDEX.md)。四处错误已订正（`.bin` 96,944 而非 140,100；验签不止 CRC32；停留依据是 SRAM4 不是 RTC；1200 touch 已实测通过）；`md5.c` 那条经核实**仍然成立**。

**行为那整节删了** —— 那些内容在 `docs/` 下有唯一出处，写两遍必漂，这份文档自己就是证据。

<details><summary>原始记录</summary>

### ~~`OpenPLC_Bootloader.md` 已经严重过期~~

| | |
|---|---|
| **是什么** | 仓库根目录一份 2026-08-12 的项目结构与功能概述 |
| **做什么用的** | 当时的项目概览。定位和 [ARCHITECTURE.md](ARCHITECTURE.md) 重叠 |
| **在哪找** | `../OpenPLC_Bootloader.md`，**不在 `docs/` 索引里** |
| **可能的影响** | ⚠️ **会把排查带偏。** 它描述的安全模型比现状弱得多，照着它查问题会从一开始就找错方向 |

**逐条对照：**

| 它写的 | 位置 | 实际 |
|---|---|---|
| `.bin` = 140,100 字节（超 128K 预算） | `:23` | **96,388**，余量 26.5% |
| "CRC32 校验后复位跳转 App" | `:45` | CRC32 **+ SHA-256 + ECDSA 验签**（`IAPServer/fw_verify.c`、`IAPServer/bootloader_state.c`） |
| 停留依据是 "RTC 备份寄存器 magic flag" | `:45` | 现在是 SRAM4 的 `boot_handoff_t`（`IAPServer/IAP_boot_handoff.c`） |
| `IAP_CDC_reboot_trigger()` 未被调用 | `:49` | 1200 touch 路径**已实测通过**（[IAP-STATUS.md](IAP-STATUS.md) 第一条） |
| `md5.c` 已包含但未被调用 | `:53` | 待核实还在不在 |

**解决方案：**

| 方案 | 代价 | 风险 |
|---|---|---|
| **删掉** | 要先逐条确认没有独有信息（flash 分区、模块清单已分散在 `docs/` 和代码注释里） | **推荐** —— 重复文档必然漂移，这份不到四个月已经错了五处 |
| 就地更正 + 挂进 `docs/` 索引 | 中 | 又多一份要跟着漂的文档，正是它现在这个状态的成因 |
| 留着不管 | 0 | ⚠️ 下一个人会照着它的错误结论排查 |

按约定，删之前要问。

</details>

**实际选的是第二条（就地更正 + 挂进索引），2026-08-17。** 表里说它的风险是"又多一份要跟着漂的文档" —— 这个风险靠**缩小它的职责**来压：现在它只写工程结构（构建方式、文件清单、产物大小、lwIP 配置），**行为、安全模型、验证状态全部改成指向 `docs/`**。漂移的前提是重复，不重复就没什么可漂的。

---

# C · 代码债

## C1 · `RESERVED_TAIL_SECTORS` 这个常量被当成两个意思用

| | |
|---|---|
| **是什么** | 一个值为 1 的宏 |
| **做什么用的** | 表达"flash 尾部预留了几个扇区不给 app 用"。那个扇区是 bootloader 的 state/journal 区 |
| **在哪找** | `Core/Inc/usbd_cdc_flash.h:65` 定义 |
| **可能的影响** | 🟢 **当前为零。** 三处数字全对，而且那个截断分支根本到不了 —— 见下 |

**为什么现在没事**（`FLASH_SECTOR_TOTAL = 8`，见 `Drivers/CMSIS/.../stm32h743xx.h:10664`）：

| 计算 | 当前值 | 应该是 | |
|---|---|---|---|
| `maxSectors = 8*2 - 1 - 1`（`usbd_cdc_flash.c:237`） | 14 | app 占全局扇区 1..14 = 14 | ✅ |
| `Flash_If_Erase(0x081E0000, 1)`（`bootloader_state.c:317`） | 擦 1 个 | state 扇区就 1 个 | ✅ |
| `IAP_APP_MAX_SIZE`（`usbd_cdc_flash.h:67`） | 上界 `0x081E0000` | 对 | ✅ |

而且 `usbd_cdc_flash.c:238-241` 的截断分支**永远进不去** —— `:226` 的地址守卫已把最大可能的 `NbSectors` 限死在 14，正好等于 `maxSectors`。

**那为什么还记一条**：它是给**未来**埋的雷。假如有人把它改成 2 想多预留一个尾部扇区：

| 位置 | 用它做什么 | 改成 2 之后 |
|---|---|---|
| `usbd_cdc_flash.h:67` `IAP_APP_MAX_SIZE` | **不用它**，直接由 `IAP_STATE_SECTOR_ADDR` 算 | app 上限不变，`IAPServer/IAP_server.c:206` 的尺寸检查照旧放行会盖住新扇区的镜像 ❌ |
| `usbd_cdc_flash.c:226` 地址守卫 | **不用它** | 同样管不到新扇区 ❌ |
| `usbd_cdc_flash.c:237` `maxSectors` | 「尾部预留几个」✅ | 数值对，但它只**截断** `NbSectors`。地址检查已放行的话，截断 = 擦得比要写的少 → 往没擦的 flash 上编程 |
| `bootloader_state.c:317` reclaim | 「reclaim 擦几个」❌ | 从 `0x081E0000` 擦两个，第二个是 `0x08200000`，**越过 2MB flash 末尾** |

**三处地址守卫全锚在 `IAP_STATE_SECTOR_ADDR` 上，一个都不看这个常量** —— 改它保护不了任何东西，只会弄坏 reclaim。而且**没有一处会编译报错**。

⚠️ 但 owner 槽走 bootloader 扇区尾部（[OWNERSHIP.md](OWNERSHIP.md)），**根本不加尾部扇区，所以这条在当前路线上碰不到**。

**解决方案：**

| 方案 | 代价 | 推荐 |
|---|---|---|
| **不动** | 0 | ✅ **当前路线下够用。** 留这条记录，等真要加尾部扇区时再看 |
| 拆成三个常量：「app 区上界地址」「reclaim 擦几个（恒 1）」「尾部预留几个」，并让 `IAP_APP_MAX_SIZE` 和 `:226` 都锚到第一个 | 半小时 + 重跑烧写回归 | 想彻底清掉再做 |
| 顺手把 `:238-241` 的静默截断改成 `return HAL_ERROR` | 很小 | 独立的小改进 —— **截断一个擦除长度本来就不该静默** |

---

## ~~C2 · `Serial_Test` 和 `Serial4` 抢同一个 UART4~~ ✅ 2026-08-17 已修（M5）

挪到 USART3（`PC_11_ALT1, PC_10_ALT1`，同引脚不同 AF），实测 5/5 回显通过。

⚠️ **下面写的"影响"低估了**：实测不是"能发不能收"，而是 **app 挂死、板子失联、只能 ST-Link 恢复**。详见 [handover/Todo/M5-serial-conflict.md](handover/Todo/M5-serial-conflict.md)。

<details><summary>原始记录</summary>

### ~~`Serial_Test` 和 `Serial4` 抢同一个 UART4~~

| | |
|---|---|
| **是什么** | 两个 Arduino 串口对象解析到了同一个硬件外设 |
| **做什么用的** | `Serial_Test`（PC11/PC10）是 core 用来打诊断日志的；`Serial4`/`Serial`（PH13/PH14）是给用户 sketch 的。**两者都是 UART4** |
| **在哪找** | `core:cores/arduino/main.cpp:40` 的 `HardwareSerial Serial_Test(PC_11, PC_10)`<br>机制说明在 [HARDWARE-FACTS.md](HARDWARE-FACTS.md) |
| **可能的影响** | ⚠️ `uart_handlers[]` 每个外设只有一个槽位，**最后一次 `begin()` 赢**。将来任何 `Serial4.begin()`（或把 USB 菜单切到 "CDC (no generic 'Serial')"）都会**静默掐掉 `Serial_Test` 的接收**，而发送看起来一切正常 —— 是那种"能发不能收、查半天"的典型 |

**解决方案：**

| 方案 | 代价 | 风险 |
|---|---|---|
| 把 `Serial_Test` 挪到没人用的 USART3：`Serial_Test(PC_11_ALT1, PC_10_ALT1)` | 需硬件验证 + **提交到共享 core**（分发给其他工程师的基础设施） | **推荐**，治本 |
| 不动，文档写"别用 Serial4" | 0 | 靠自觉，而且 USB 菜单那条路径用户不会想到 |

</details>

---

## ~~C3 · 变体把 FMC 的 39 个脚也暴露成 Arduino 引脚~~ ✅ 2026-08-17 已做（M4）

变体头里已加 `FMC_RESERVED_*`（39 个），事实表进了 [HARDWARE-FACTS.md](HARDWARE-FACTS.md)，验收是编译期断言。**采用可发现性方案，不是拦截** —— `digitalWrite(PE7, ...)` 仍然编得过，刻意的。详见 [handover/Todo/M4-fmc-pin-guard.md](handover/Todo/M4-fmc-pin-guard.md)。

<details><summary>原始记录</summary>

### ~~变体把 FMC 的 39 个脚也暴露成 Arduino 引脚~~

| | |
|---|---|
| **是什么** | `NUM_DIGITAL_PINS 140` —— 变体把 MCU 全部引脚都注册成可用的 Arduino 数字引脚 |
| **做什么用的** | 让用户能用 `digitalWrite(PXn, ...)` 访问任意引脚，是 STM32duino 的常规做法 |
| **在哪找** | `core:variants/STM32H7xx/H743/variant_PLC_H743.h:338`<br>FMC 的 39 脚清单：`Core/Src/fmc.c:153-193`（`HAL_FMC_MspInit` 注释块，逐脚列了功能）<br>对外 IO 定义：`core:variants/.../variant_PLC_H743.h:194-225` |
| **可能的影响** | ⚠️ 用户写 `digitalWrite(PE7, HIGH)`（PE7 = `FMC_D4`）**能编译、能运行、把 SDRAM 数据线拽死**。现象是"SDRAM 偶发读到垃圾"，极难查 |

FMC 占用的 39 个脚：

```
PC0 · PD0,1,8,9,10,14,15 · PE0,1,7-15 · PF0-5,11-15 · PG0,1,2,4,5,8,15 · PH2,3
```

**注意这不是设计冲突** —— 这 39 个脚在 PCB 上只连 AS4C32M16SB 那颗 SDRAM，和变体里所有对外 IO（DOUT×8 / DIN×8 / AIN×2 / AOUT×2 / RS232 / RS485）**一个都不撞**，连 BOOT0 的 PG9 都恰好夹在 PG8 和 PG15 中间空着。**是缺一道防护，不是排布错误。**

**解决方案：**

| 方案 | 代价 | 效果 |
|---|---|---|
| 文档里列出这 39 个脚，写"不要碰" | 0 | 没人看 |
| **变体头里给它们起名 `FMC_RESERVED_*` 并注释** | 小 | **推荐** —— 用户在头文件里就能看见，可发现性最好。顺带写进 [HARDWARE-FACTS.md](HARDWARE-FACTS.md) |
| `pinMode`/`digitalWrite` 里加运行时拦截 | 每次 IO 调用加开销 | 不像 Arduino 风格，否决 |

</details>

---

# D · 需要立项

## D1 · 发现限流是固定窗口，实测能超标 20%

| | |
|---|---|
| **是什么** | UDP 发现回复的限流器，`DISCOVERY_MAX_REPLIES_PER_SEC = 50` |
| **做什么用的** | 防止板子被发现请求泛洪时把 UART 和网络打满。**全设备封顶，不按源 IP** —— 按源计会让同一台主机上的 IDE 插件和 IAPTool 抢配额（那个坑的完整来历见 [IAP-STATUS.md](IAP-STATUS.md)） |
| **在哪找** | bootloader `IAPServer/udp_server.c:37` 常量、`:39` 函数体、`:110` 调用点<br>core `core:libraries/OpenPLC_IAP/src/udp_server.c:44`、`:46`、`:141`<br>⚠️ **跨仓镜像，改一处不改另一处会静默分叉** |
| **可能的影响** | 标称 50 次/秒，**实测可达 60** —— 突发跨窗口边界时最多接近 2 倍。合法用量约 2 次/秒，离上限差 25 倍，**所以从不误伤**。真正的影响只是：**不能把 50 当作对外承诺的保证值** |

**解决方案：**

| 方案 | 代价 | 推荐 |
|---|---|---|
| **不改，文档写明"约 50，非硬上限"** | 0 | ✅ 合法用量差 25 倍，硬不硬上限没有实际差别 |
| 换成连续补充的令牌桶 | 改两份镜像 + 重跑 N5 泛洪用例（`$TOOL:TestTool/`） | 要对外承诺数字时再做 |

---

## ~~D2 · `DR2` 见证寄存器存不住~~ ✅ 2026-08-17 已定位并修复

**根因是跨仓备份寄存器撞车** —— bootloader 的 witness 和 app 的 nonce 计数器都占 `DR2`。witness 已挪到 `DR3`，只改 bootloader。实测连续三次进 bootloader 稳定报 `Backup domain retained`。

完整经过和寄存器分配表见 [IAP-STATUS.md](IAP-STATUS.md) 与 [ARCHITECTURE.md](ARCHITECTURE.md)。

⚠️ **`DR1` 还留着一个潜在冲突**：bootloader 用它做 nonce 计数器，core 的 `backup.h:34` 也把 `RTC_BKP_INDEX` 定义成 `DR1`。**当前无人写它**，谁引入 STM32RTC 库谁踩。

⚠️ 顺带把"撤销状态不要放 RTC 备份域"这条正式写进了 [OWNERSHIP.md](OWNERSHIP.md) —— 之前只在对话里说过，文档里其实没有。**理由也换了**：不再是"读取不可靠"（那条已修），而是"备份域是三个仓库共享、且无人统一分配的资源，已经撞过一次车"。

<details><summary>定位过程（旧记录）</summary>

### ~~`DR2` 见证寄存器写不进去（或存不住）~~

### 这套机制是什么

板子**没有硬件随机数**（HAL_RNG 没配置）。所以 IAP 认证的挑战 nonce 是拼出来的：

```
nonce = 单调计数器(4B) ‖ UID字0(4B) ‖ HAL_GetTick()(4B) ‖ 0(4B)
```

—— `IAPServer/iap_auth.c:62-81`。

**计数器存在 RTC 备份寄存器 `DR1` 里**（`:28`），每发一次挑战就 +1 并写回（`next_counter()`，`:43-50`）。它放在备份域，是为了**跨掉电也不重复**：

> 如果计数器每次上电归零，nonce 就会跨掉电重复，一对被抓到的 `(nonce, HMAC)` 就能重放进来完成认证。
> 注意这里靠的是**唯一性不是不可预测性** —— 攻击者真正需要的是 HMAC 密钥，那个从观察 nonce 得不到。（源码注释 `:9-14`）

而**备份域只在 VBAT 电池在位且有电时才保得住**。所以第二个寄存器 `DR2` 存一个固定魔数 `'VBAT'`（`0x56424154`，`:35-37`）当见证：**它能读回来，就说明备份域活着，计数器可信**。

`iap_auth_report_backup_domain()`（`:125-151`，从 `Core/Src/main.c:199` 调用）就是每次启动把这个结论打到串口上 —— **这是唯一能让人发现"RTC 电池没了、重放保护变弱了"的途径**。

### 现在的问题

| | |
|---|---|
| **在哪找** | `IAPServer/iap_auth.c:125-151`，调用点 `Core/Src/main.c:199` |
| **来历** | 功能在 `64edc2e`（2026-08-15）引入，那次 commit 自己注明「尚未上板」。上板后报了一次"备份域丢失"，`99db0ce` 把日志改得不再谎称重放保护被削弱，但根因当时没查 |
| **可能的影响** | 取决于下面哪个解释成立 |

### 2026-08-17 实测：它又报了，之前的推断作废

```
** Backup domain witness missing, but nonce counter = 39. **
```

counter 从上次的 35 涨到 **39**。所以**不是"首次运行"** —— 那个解释（"witness 是新加的寄存器，第一次当然没值，之后就不报了"）**被直接否定**。

现在确定的事实，比之前任何一个假设都更准：

> **`DR1`（计数器）写入持久、跨复位递增；`DR2`（witness）每次都写、每次下次启动又读不到。两处代码模式完全相同。**

已排除的：

| 排除项 | 依据 |
|---|---|
| `hrtc` 没初始化 | `Core/Src/main.c:198` 的 `MX_RTC_Init()` 就在调用点 `:199` 前一行；而且 DR1 读得出正确值 |
| 备份域整个被清 | 那样 DR1 也会归零，但它在递增 |
| `MX_RTC_Init()` 清了备份寄存器 | `Core/Src/rtc.c:30-59` 没有任何相关动作 |
| DBP 没打开 | 两处都用 `HAL_PWR_EnableBkUpAccess()` 包着，且 DR1 用同一个包装写得进去 |

**机理仍未查明。不要继续猜。**

### 解决方案 —— 下一步是加一行诊断，不是推理

在 `IAPServer/iap_auth.c:149` 写完 DR2 之后**立刻读回并打印**。这一步**不改任何行为**，只是把问题一分为二：

| 读回结果 | 结论 | 接着查 |
|---|---|---|
| 读回**不等于** `'VBAT'` | **当场就没写进去** | 为什么同一个包装写 DR1 行、写 DR2 不行 —— 查 DR2 是否被别的东西占用/保护 |
| 读回**等于** `'VBAT'` | **写进去了但没保住** | 是掉电/复位路径上被清掉的 —— 查 app 侧（core 的 `MX_RTC_Init()`）和 `HAL_RCCEx_PeriphCLKConfig(RCC_PERIPHCLK_RTC)` 是否触发过备份域复位 |

⚠️ **在结论出来之前**，[OWNERSHIP.md](OWNERSHIP.md) 那条约束仍然成立：**不要把撤销状态等安全关键数据放进 RTC 备份域。**

</details>

---

## ~~D3 · app 侧要不要能用 SDRAM~~ ✅ 2026-08-17 需求已确认

**要做，而且要封装好，用户直接调封装好的能力。** 这半句否掉了原来"用户自己在 linker script 段里声明大数组"的方案 —— **`memset` 的义务必须由库承担，不能落到用户头上**。设计和还没定的四个 API 问题在 [handover/Todo/M3-app-sdram.md](handover/Todo/M3-app-sdram.md)。

<details><summary>原始记录</summary>

### ~~app 侧要不要能用 SDRAM~~

| | |
|---|---|
| **是什么** | 板载 64MB 外部 SDRAM（AS4C32M16SB-7BIN），映射在 `0xC0000000` |
| **做什么用的** | bootloader 侧：接收 CDC/以太网传来的镜像的暂存区，校验通过后再写 flash。**app 侧目前没有用途，也用不了** |
| **在哪找** | bootloader 初始化 `Core/Src/main.c:201`（`MX_FMC_Init()`），实现和上电时序 `Core/Src/fmc.c:44-126`<br>bootloader 交权时关掉它：`IAPServer/IAP_server.c:501` 的 `HAL_DeInit()`<br>core 侧：`core:variants/STM32H7xx/H743/` 下 `.c/.cpp/.h` **全搜零命中**，`ldscript.ld:41-55` 的 `MEMORY` 块也没声明 SDRAM |
| **可能的影响** | 现状下 **app 完全用不了这 64MB**。不做的话，用户想跑大数据缓冲（数据记录、大点表、图像）就只能用片内 512K |

**解决顺序**：先定 app 是否真需要 → 需要的话在 core 里加 FMC 初始化（⚠️ **改共享 core**，按 [ARCHITECTURE.md](ARCHITECTURE.md) 是分发给其他工程师的基础设施，要单独立项）→ 再选内存声明方式。

**声明方式已经想清楚了，落地时照抄：**

| | 做法 | 理由 |
|---|---|---|
| **bootloader** | **裸地址** `(uint8_t*)0xC0000000`，**保持现状不动** | 一次性 staging；不希望链接器往里放任何东西；SDRAM 在 startup 时还没初始化，**声明它反而危险** |
| **app / core** | **写进 linker script，用 `(NOLOAD)` 段** | 用户会声明大数组，需要链接器做冲突检测和溢出报错。裸指针的话两个库都用 `0xC0000000` 会静默互踩 |

app 侧三个关键点。可抄 `core:variants/STM32H7xx/H743/DAISY_SEED.ld:74`（`SDRAM (xrw) : ORIGIN = 0xC0000000, LENGTH = 64M`）和 `:217-227`（`.sdram_bss (NOLOAD)`）—— ⚠️ **那是上游 Electrosmith Daisy Seed 板子的脚本，`core:boards.txt:50` 写死了 OPEN-PLC 用 `ldscript.ld`，构建里没引用它**，只是一份可抄的写法：

| | 要求 | 不这么做会怎样 |
|---|---|---|
| 1 | 段必须标 **`(NOLOAD)`** | 段会进 `.bin` —— 一个 1MB 数组让固件镜像涨 1MB |
| 2 | 段必须放在 **`_sbss` … `_ebss` 之外** | startup 的清零循环会扫到它。**清零发生在 FMC 初始化之前，往未初始化的 SDRAM 写 = 故障** |
| 3 | ⚠️ **只能是 BSS 风格，不能有初值** | `.data` 风格的段会被 startup 的拷贝循环在 FMC 之前写 |

⚠️ **代价：这些变量不是零初始化的。** 用户必须在 FMC 初始化后自己 `memset` —— **这条必须写进用户文档**，否则是一类极难查的间歇性 bug。

</details>

⚠️ **上面最后那条代价，正是 2026-08-17「要封装好」否掉这个方案的原因** —— 清零该由库做，不该写进用户文档让用户记着。三条链接脚本要求仍然是底层实现的硬约束，只是不再暴露给用户。

---

# E · 已知但不打算做

## E1 · `meta.sha256` 写了但全代码没人读

| | |
|---|---|
| **是什么** | firmware metadata 记录里的一个 32 字节字段 |
| **做什么用的** | 存上次成功烧写时算出的 app 镜像哈希 |
| **在哪找** | 字段声明 `IAPServer/bootloader_state.h:54`<br>唯一写入点 `IAPServer/bootloader_state.c:205`<br>启动校验 `IAPServer/IAP_server.c:390-393` —— **现算 app 区哈希再验签名，从不读这个副本** |
| **可能的影响** | **无。** 签名已经绑定了哈希，再比一次存下来的副本不增加任何安全性（见 [JOURNAL.md](JOURNAL.md)） |
| **解决方案** | 不动。记在这里只为让下一个看到这个字段的人别以为它参与校验。**将来要给工具加核对命令（如 `getappinfo`），字段现成就在** |
