# 已知问题

**零散的问题：现象 + 已核实的事实 + 要做什么，一条一个地方。** 2026-08-22 从原 `TODO.md` 重写。

之前这些内容分在两个文件里 —— `IAP-STATUS.md` 写"现象和事实"、`TODO.md` 写"要做什么" —— 结果是**看一条问题要开两个文件**，而且两边都想避免重复，谁都没写全。现在合并：**一条问题一个条目，从现象到动作。**

**需要立项的模块**（有设计空间、要分步做）不在这里，在 [BACKLOG.md](BACKLOG.md)，那边一个模块一份。

**编号 `ISS-` 前缀是 2026-08-22 加的。** 原来是裸 `A1`/`B4`/`C2`，和 [../STATUS.md](../STATUS.md) 的需求号 `A1`–`F5` 完全撞车 —— 说"A7 过了"字面上有四个意思。字母数字保留没变，这样旧提交信息里的编号还查得到。全部编号的登记表在 [../ID-MAP.md](../ID-MAP.md)。

路径约定：不带前缀 = 本仓库；`core:` = Arduino 板卡包；`$TOOL:` = IAPTranfer_Tool。行号会漂，改到附近时顺手更新。

---

## 按优先级排

| 优先级 | 编号 | 一句话 | 挡着什么 | 谁能动 |
|---|---|---|---|---|
| **P1** | [ISS-B5](#iss-b5--★-某些-app-装上去之后板子对-iaptool-不可达) | 某些 app 让板子对 IAPTool 不可达 | 需求 **A3** 的成立范围；无人值守的网络升级 | 我们（加个编译宏就能定位） |
| **P2** | [ISS-A4](#iss-a4--所有权命令还没进出货工具-iaptool) | `takeown`/`setowner` 没进 IAPTool | **客户拿不到 C10 这个功能** | 我们 |
| **P3** | [ISS-B2](#iss-b2--两块板的-mac-是否互不相同) | 两块板 MAC 是否不同 | 需求 **E1**、逐板检查单 **F2** | 等第二块板 |
| P4 | [ISS-A3](#iss-a3--pg9-改输入应该挪到-ioc-里) | PG9 改输入应挪进 `.ioc` | 无（整洁问题） | 我们 |
| P4 | [ISS-C4](#iss-c4--两个-fakeboard-的-powershell-版有一个偶发竞态) | fakeboard PS 版偶发竞态 | 无（M7 第 6 步会删掉那些文件） | 我们 |
| P5 | [ISS-D1](#iss-d1--发现限流是固定窗口实测能超标-20) | 限流是固定窗口，能超标 20% | 无（合法用量差 25 倍） | 我们，要对外承诺数字时再做 |
| — | [ISS-A2](#iss-a2--boot-行前有个乱码字节) | `[BOOT]` 前一个乱码字节 | 无。**根因已查清，建议接受** | **要你定**（曾被误记成已决策，已更正） |
| — | [ISS-B1](#iss-b1--boot-millis-靠电荷泵余电才出得来) | `[BOOT] millis=` 靠电荷泵余电 | 无。**等一个设计决策** | 要你定 |
| — | [ISS-C1](#iss-c1--reserved_tail_sectors-被当成两个意思用) | `RESERVED_TAIL_SECTORS` 一名两义 | 无。**当前路线碰不到** | 留档 |
| — | [ISS-E1](#iss-e1--metasha256-写了但全代码没人读) | `meta.sha256` 写了没人读 | 无。**不打算做** | 留档 |

---

# P1–P3 · 卡在别人身上或影响客户的

## ISS-B5 · ★ 某些 app 装上去之后板子对 IAPTool 不可达

**是什么**：装上 `$TOOL:TestCase/onboard/sdram/SDRAM_Acceptance` 之后，板子**回 ICMP ping，但完全不回 UDP 发现**，`IAPTool ether` 报 `No response from <ip>, exiting`。换成 `SerialPort` 就好。

**A/B 实测（2026-08-22，同一块板、同一个 bootloader、同一个网络）**：

| 板上装的 | ICMP | UDP 发现（56865） |
|---|---|---|
| `SDRAM_Acceptance` | ✅ | ❌ **60 秒超时** |
| 只有 bootloader，无 app | ✅ | ✅ 复位后 5.1 s |
| `SerialPort` | ✅ | ✅ 0.1 s |

**已经排除的**：

| 排除项 | 依据 |
|---|---|
| 板子掉线 / IP 变了 | 该 app 自己的串口打印 `[NET] ip=192.168.0.35`，且 ICMP 到那个地址通 |
| 以太网 DMA / lwIP 整体坏了 | ICMP 应答由 lwIP 核心处理，它是通的 |
| 端口不对 | app 侧和 bootloader 侧都用 `getPort()` = 56865（`$TOOL:IAP_Ether.go:441-445`） |
| sketch 没启动应答器 | `core:cores/arduino/main.cpp:184` **无条件**调用 `openplc_udp_server_start(NULL)`，而且在 `setup()` **之前** |
| sketch 的 `loop()` 堵住了主循环 | `SDRAM_Acceptance.ino:129` 是 `void loop() {}`，且核心循环每轮都调 `openplc_net_process()` |

**由此支持的解释（推断，不是实测）**：ICMP 通而绑在 56865 的 UDP pcb 不应答，正是 **`udp_bind` 失败**的形状 —— 那个 pcb 是独立的，绑不上不影响 lwIP 其余部分。

⚠️ **为什么两个 sketch 会不一样，还没查明。不要猜。**

**在哪找**：`core:cores/arduino/main.cpp:184`（应答器启动点）；`core:libraries/OpenPLC_IAP/src/udp_server.c`（应答器本体）；`core:libraries/OpenPLC_IAP/src/OpenPLC_IAP_Autostart.h`（诊断计数器声明）。

**可能的影响**：⚠️ **用户的 PLC 程序可能把自己锁在网络升级之外。** 不是变砖 —— BOOT0 长按、CDC、ST-Link 都还能进上传模式 —— 但**无人值守的网络升级会失效**，而那是这套 IAP 的主要用法。

顺带两条：
- **需求 A3 的成立范围要收窄**，它假设了「app 会应答发现」，现在已知有反例。A3 在 [../STATUS.md](../STATUS.md) 因此从 ✅ 降成 🟡。
- **SD1 必须是任何序列里最后一条依赖网络的用例**，否则它后面所有用例都够不到板子。板级用例的标准载荷因此选 `SerialPort`，不是 `SDRAM_Acceptance`。

🚧 **下一步是现成的，不用猜**：用 `-DOPENPLC_DIAG_HEARTBEAT` 编一版那个 sketch（`core:cores/arduino/main.cpp:104-128`），它会周期打印 `udp_start` / `udp_rx` / `udp_tx` / **`bind_fail`** 计数 —— 那几个计数器**存在本身就说明这类事以前出过**，一眼就能确认或否掉。

⚠️ **改 core 是跨仓的共享基础设施**（见 [../design/ARCHITECTURE.md](../design/ARCHITECTURE.md)），所以定位之后再决定谁改、怎么发版。**定位本身不需要动 core，只要加一个编译宏。**

## ISS-A4 · 所有权命令还没进出货工具 IAPTool

| | |
|---|---|
| **是什么** | bootloader 的 `takeown` / `setowner` / `getowner` 三个命令 |
| **做什么用的** | 客户把板子绑到自己的签名密钥上（需求 C10） |
| **在哪找** | 板子侧 `IAPServer/IAP_server.c` + `IAPServer/owner_slot.c`，**已完成并实测**<br>主机侧只有 `$TOOL:TestCase/tools/run-takeown.ps1` / `run-setowner.ps1`，**那是内部测试脚本** |
| **可能的影响** | ⚠️ **客户拿不到这个功能。** 板子侧齐了，但出货工具 `IAPTool` 一个入口都没有 —— 客户没有受支持的办法认领自己的板子。[../../RELEASE-NOTES.md](../../RELEASE-NOTES.md) 已写明这一点，别让文档跑到实现前面 |

**要做什么**：`IAPTool takeown <ip>`、`IAPTool setowner <ip> --current-key=... --new-key=...`。签名那半已经有了（`IAPTool signraw`，2026-08-18 加的），剩下的是命令面和把私钥管理讲清楚。

⚠️ **`takeown` 要求操作员按住 BOOT0**，所以工具必须能把这件事讲明白并等人操作 —— 这是个 UX 问题，不只是加个子命令。

## ISS-B2 · 两块板的 MAC 是否互不相同

**等第二块板。**

| | |
|---|---|
| **是什么** | MAC 地址从芯片 UID 派生的算法 |
| **做什么用的** | 取代原先写死的 `00:80:E1:00:43:21` —— 那个值所有板子相同，同一网段放两台直接冲突 |
| **在哪找** | bootloader `LWIP/Target/ethernetif.c` 的 USER CODE MACADDRESS 块<br>core `core:libraries/OpenPLC_Net/src/ethernetif.c`（**跨仓镜像，改一处必须改另一处**） |
| **可能的影响** | 单板已验证：两侧串口都打印 `02:BB:49:3E:A8:02`，同 IP。但**"两块板不同"从未观察过** —— 如果派生算法有缺陷，量产时才会暴露成大面积 IP 冲突 |
| **要做什么** | 拿到第二块板，同网段同时上电，比对两边串口打印的 MAC。用例骨架是 [../test/CASE-DESIGNS.md](../test/CASE-DESIGNS.md) 的 **M3** |

⚠️ **P2 已经自动比对了两侧派生算法的一致性**，剩下的只有"算法本身会不会撞"，那需要真的两块板。

⚠️ **产线要留 MAC 记录**，否则"不重复"无从判起 —— `$TOOL:TestCase/acceptance/checklist.md` 的 `CHK-C3`。检查单本身现在就能写，不用等板子。

---

# P4–P5 · 攒一趟做完

## ISS-A3 · PG9 改输入应该挪到 `.ioc` 里

**用户已认领。攒够了一起烧，别为单条跑一趟 ST-Link。**

| | |
|---|---|
| **是什么** | `BOOT0_ConfigureAsInput()`，`Core/Src/gpio.c` 的 USER CODE 块，由 `main.c` 在 `MX_GPIO_Init()` 之后调用 |
| **做什么用的** | 把 CubeMX 生成的 PG9 推挽输出改回输入。理由和实测见 [../design/HARDWARE-FACTS.md](../design/HARDWARE-FACTS.md) |
| **为什么现在长这样** | 2026-08-18 那次是**受控实验**，只想动一个变量；重新生成整个工程会引入第二个变量 |
| **要做什么** | 在 `.ioc` 里把 PG9 设成 GPIO_Input，重新生成，然后**删掉 `BOOT0_ConfigureAsInput()` 和 main.c 里的调用** |
| **不做会怎样** | 🟢 功能上没影响 —— 它只是把已经配好的引脚再配一遍。但会变成"没人敢动的重复代码"，而且下一个人看到生成段仍写着 `OUTPUT_PP` 会困惑 |

⚠️ **`.ioc` 改完必须重新验一次 RESET 按钮和 BOOT0 长按** —— 重新生成会动整个 `MX_GPIO_Init()`，不只是这一个脚。

## ISS-C4 · 两个 fakeboard 的 PowerShell 版有一个偶发竞态

| | |
|---|---|
| **是什么** | `Stop-Process` 停掉假板子之后**不等它真的退出**，就开始下一个用例 |
| **做什么用的** | `$TOOL:TestCase/host/fakeboard/run-cases.ps1`（K1–K6）和 `run-downgrade.ps1`（DG1） |
| **在哪找** | `run-cases.ps1:159`、`run-downgrade.ps1:171`；根因在 `host/fakeboard/fake_board.py:130` 的 `SO_REUSEADDR` |
| **可能的影响** | 上一个进程的 socket 还占着，而 `SO_REUSEADDR` 让下一个用例**照样能 bind 同一个端口** → 两个进程同时监听，谁 accept 未定义。表现是某条用例报「board was never sent a flash command」而它的日志里只有启动那一行 —— 连接被上一个进程接走了。**偶发**，重跑就过。⚠️ **偶发的用例比没有用例更糟**：它训练人重跑一次就当过了 |
| **怎么发现的** | 2026-08-22 移植 Python 版时撞出来的，详见 [M7-python-scripts.md](M7-python-scripts.md) 第 3 步。Python 版已修成确定的（`kill()` + `wait()`，读日志前先等日志停止增长） |

**解决方案：**

| 方案 | 代价 | 风险 |
|---|---|---|
| **不改 `.ps1`** | 0 | ✅ **推荐**。PS 版在 M7 第 6 步之前是 Python 版的**对照基准** —— 改基准就等于在验收过程中动标尺。而 M7 第 6 步会把它删掉，那时这条自动消失 |
| 一并修 `.ps1` | 小（`Stop-Process` 后加 `Wait-Process`） | 两版都改就要重跑一次 10 对比对；而且这是给一个即将删除的文件付利息 |

⚠️ **在 M7 第 6 步删掉 `.ps1` 之前，K1–K6 和 DG1 仍然会走 PowerShell 版**，所以这个偶发仍然会遇到。遇到了就是这一条，不用重新排查。

## ISS-D1 · 发现限流是固定窗口，实测能超标 20%

| | |
|---|---|
| **是什么** | UDP 发现回复的限流器，`DISCOVERY_MAX_REPLIES_PER_SEC = 50` |
| **做什么用的** | 防止板子被发现请求泛洪时把 UART 和网络打满。**全设备封顶，不按源 IP** —— 那个决定的完整来历见 [../design/DECISIONS.md](../design/DECISIONS.md) 第 3 条 |
| **在哪找** | bootloader `IAPServer/udp_server.c:37` 常量、`:39` 函数体、`:110` 调用点<br>core `core:libraries/OpenPLC_IAP/src/udp_server.c:44`、`:46`、`:141`<br>⚠️ **跨仓镜像，改一处不改另一处会静默分叉** |
| **可能的影响** | 标称 50 次/秒，**实测可达 60**（突发跨窗口边界时最多接近 2 倍）。合法用量约 2 次/秒，离上限差 25 倍，**所以从不误伤**。真正的影响只是：**不能把 50 当作对外承诺的保证值** |

**解决方案：**

| 方案 | 代价 | 推荐 |
|---|---|---|
| **不改，文档写明"约 50，非硬上限"** | 0 | ✅ 合法用量差 25 倍，硬不硬上限没有实际差别 |
| 换成连续补充的令牌桶 | 改两份镜像 + 重跑 N5 泛洪用例 | 要对外承诺数字时再做 |

---

# 已查清根因，等一个决定

## ISS-A2 · `[BOOT]` 行前有个乱码字节

**根因已查清。三个候选排除了两个。建议接受。**

| | |
|---|---|
| **是什么** | bootloader 交权给 app 前后，串口上多出一个乱码字节 |
| **可能的影响** | **纯显示问题**，不影响任何功能。但会让人怀疑串口配置错了，浪费排查时间 |

串口原文：

```
** APP Mod ...
[?[BOOT] millis=12      ← 乱码紧贴在 [BOOT] 前面
UART echo ready         ← 前面是干净的
```

| 候选 | 窗口 | 在哪找 | 机理 | 状态 |
|---|---|---|---|---|
| ⚠1 | DeInit 之后、关收发器之前 | `IAPServer/IAP_server.c:481` | PC10（MAX3221 的 DIN）被释放成浮空，而芯片还开着 → 输出跟噪声抖 | ❌ **已排除** —— 改了顺序，乱码一模一样还在 |
| **⚠2** | 关收发器那一刻 | `IAPServer/IAP_server.c:482`（定义在 `Core/Src/usart.c:151`） | 电荷泵停转，线电平从 mark 塌向 0V → 接收端判成起始位 | ✅ **就是这个** |
| ⚠3 | sketch 打开收发器那一刻 | — | 电荷泵启动爬升 | ❌ **已排除** —— 而这是我原先标的"最高把握" |

被否定的那两个候选和原来那个错根因，收在 [../archive/RETRACTED.md](../archive/RETRACTED.md) 第 7–9 条。

**这是主动关断收发器的固有代价，不是 bug。** 而"交权时让 app 拿到一块冷板子的收发器状态"是刻意设计（见 `server_jump_to_app()` 注释）。

**剩下的选项，都不便宜：**

| 方案 | 代价 |
|---|---|
| **接受它**，文档写明 | 0。一个纯显示字节 |
| 关断前多发几个空闲位，把塌陷推到有效数据之后 | 要试出需要几个位，且塌陷时间随电容容差变 |
| 干脆不关收发器 | ❌ 违反"app 拿到冷板子状态"的设计 |

**我建议接受，但这条还没拍板 —— 等你定。** 根因已经查清，所以不用重查；要定的只是"接不接受"。定了之后再进 [../design/DECISIONS.md](../design/DECISIONS.md)。

## ISS-B1 · `[BOOT] millis=` 靠电荷泵余电才出得来

**要你定改不改。**

2026-08-17 实测已回答"从哪个口看到的"：**COM5，就是 RS232 端子 C05/C06。** 原来"代码上这行到不了端子"那个推论是错的 —— `millis=12`，电荷泵余电还没塌，banner 蹭出去了。完整经过见 [../archive/RETRACTED.md](../archive/RETRACTED.md) 第 6 条。

| 方案 | 代价 | 风险 |
|---|---|---|
| **不改**，文档写明"靠余电，不可依赖" | 0 | app 启动一慢这行就没了，而没有任何东西保证那个时间 |
| core 在打 banner 前主动拉高 PB10 | 小 | ⚠️ **抢走用户 app 对收发器初始状态的控制权**，可能违反[「设计不能限制用户的 app」](../process/WORKING-AGREEMENTS.md) |

⚠️ 无论选哪个，**都不要再往 core 的这个位置加启动打印** —— 后加的越多，越可能超过余电窗口而静默丢失。

---

# 留档：知道，但当前不动

## ISS-C1 · `RESERVED_TAIL_SECTORS` 被当成两个意思用

| | |
|---|---|
| **是什么** | 一个值为 1 的宏，`Core/Inc/usbd_cdc_flash.h:65` 定义 |
| **做什么用的** | 表达"flash 尾部预留了几个扇区不给 app 用"。那个扇区是 bootloader 的 state/journal 区 |
| **可能的影响** | 🟢 **当前为零。** 三处数字全对，而且那个截断分支根本到不了 |

**为什么现在没事**（`FLASH_SECTOR_TOTAL = 8`）：`maxSectors = 8*2-1-1 = 14`，app 正好占全局扇区 1..14；reclaim 擦 1 个，state 扇区就 1 个；`IAP_APP_MAX_SIZE` 上界 `0x081E0000` 正确。而 `usbd_cdc_flash.c:238-241` 的截断分支**永远进不去** —— `:226` 的地址守卫已把最大可能的 `NbSectors` 限死在 14。

**那为什么还记一条**：它是给**未来**埋的雷。假如有人把它改成 2 想多预留一个尾部扇区：

| 位置 | 用它做什么 | 改成 2 之后 |
|---|---|---|
| `usbd_cdc_flash.h:67` `IAP_APP_MAX_SIZE` | **不用它**，直接由 `IAP_STATE_SECTOR_ADDR` 算 | app 上限不变，`IAPServer/IAP_server.c:206` 的尺寸检查照旧放行会盖住新扇区的镜像 ❌ |
| `usbd_cdc_flash.c:226` 地址守卫 | **不用它** | 同样管不到新扇区 ❌ |
| `usbd_cdc_flash.c:237` `maxSectors` | 「尾部预留几个」✅ | 数值对，但它只**截断** `NbSectors`。地址检查已放行的话，截断 = 擦得比要写的少 → 往没擦的 flash 上编程 |
| `bootloader_state.c:317` reclaim | 「reclaim 擦几个」❌ | 从 `0x081E0000` 擦两个，第二个是 `0x08200000`，**越过 2MB flash 末尾** |

**三处地址守卫全锚在 `IAP_STATE_SECTOR_ADDR` 上，一个都不看这个常量** —— 改它保护不了任何东西，只会弄坏 reclaim。而且**没有一处会编译报错**。

⚠️ 但 owner 槽走 bootloader 扇区尾部（[../design/OWNERSHIP.md](../design/OWNERSHIP.md)），**根本不加尾部扇区，所以这条在当前路线上碰不到**。

| 方案 | 代价 | 推荐 |
|---|---|---|
| **不动** | 0 | ✅ **当前路线下够用。** 留这条记录，等真要加尾部扇区时再看 |
| 拆成三个常量并让 `IAP_APP_MAX_SIZE` 和 `:226` 都锚到「app 区上界地址」 | 半小时 + 重跑烧写回归 | 想彻底清掉再做 |
| 顺手把 `:238-241` 的静默截断改成 `return HAL_ERROR` | 很小 | 独立的小改进 —— **截断一个擦除长度本来就不该静默** |

## ISS-E1 · `meta.sha256` 写了但全代码没人读

| | |
|---|---|
| **是什么** | firmware metadata 记录里的一个 32 字节字段 |
| **做什么用的** | 存上次成功烧写时算出的 app 镜像哈希 |
| **在哪找** | 字段声明 `IAPServer/bootloader_state.h:54`<br>唯一写入点 `IAPServer/bootloader_state.c:205`<br>启动校验 `IAPServer/IAP_server.c:390-393` —— **现算 app 区哈希再验签名，从不读这个副本** |
| **可能的影响** | **无。** 签名已经绑定了哈希，再比一次存下来的副本不增加任何安全性（见 [../design/JOURNAL.md](../design/JOURNAL.md)） |
| **决定** | 不动。记在这里只为让下一个看到这个字段的人别以为它参与校验。**将来要给工具加核对命令（如 `getappinfo`），字段现成就在** |

---

## 维护

- **做完就删掉，不要打勾留着。** 2026-08-22 清理时删掉了 6 个已完成条目（原 A1 / B3 / C2 / C3 / D2 / D3），连它们的 `<details>` 存档共约 300 行 —— 那些内容的结论已经在 [../STATUS.md](../STATUS.md) 和各自的 `M*.md` 里，留在这里只是噪音。
- **被实测否定的推论例外**：那些搬进 [../archive/RETRACTED.md](../archive/RETRACTED.md)，不要直接删 —— 删了会有人重推一遍。
- **一条问题只在这里描述一次。** 别在 [../test/MEASUREMENTS.md](../test/MEASUREMENTS.md) 再写一遍现象，那边只放实测数字。
- 新条目按上面的优先级表插进去，**编号只增不改**。
