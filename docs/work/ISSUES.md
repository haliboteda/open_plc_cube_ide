# 已知问题

**一条问题一个条目：现象 + 已核实的事实 + 要做什么。** 需要立项的模块在 [BACKLOG.md](BACKLOG.md)。

路径约定：不带前缀 = 本仓库；`core:` = Arduino 板卡包；`$TOOL:` = IAPTranfer_Tool。全部编号的登记表在 `$PROD/docs/ID-MAP.md`。

## 按优先级排

| 优先级 | 编号 | 一句话 | 挡着什么 | 谁能动 |
|---|---|---|---|---|
| **P1** | [ISS-B5](#iss-b5--★-某些-app-装上去之后板子对-iaptool-不可达) | 某些 app 让板子对 IAPTool 不可达 | 需求 **A3** 的成立范围；无人值守的网络升级 | 我们（加个编译宏就能定位） |
| **P2** | [ISS-A4](#iss-a4--所有权命令还没进出货工具-iaptool) | `takeown`/`setowner` 没进 IAPTool | **客户拿不到 C10 这个功能** | 我们 |
| **P3** | [ISS-B2](#iss-b2--两块板的-mac-是否互不相同) | 两块板 MAC 是否不同 | 需求 **E1**、逐板检查单 **F2** | 等第二块板 |
| P4 | [ISS-A3](#iss-a3--pg9-改输入应该挪到-ioc-里) | PG9 改输入应挪进 `.ioc` | 无（整洁问题） | 我们 |
| P5 | [ISS-D1](#iss-d1--发现限流是固定窗口实测能超标-20) | 限流是固定窗口，能超标 20% | 无（合法用量差 25 倍） | 我们，要对外承诺数字时再做 |
| — | [ISS-A2](#iss-a2--boot-行前有个乱码字节) | `[BOOT]` 前一个乱码字节 | 无。**根因已查清，建议接受** | **要你定**（曾被误记成已决策，已更正） |
| — | [ISS-B1](#iss-b1--boot-millis-靠电荷泵余电才出得来) | `[BOOT] millis=` 靠电荷泵余电 | 无。**等一个设计决策** | 要你定 |

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
| **SDRAM 用法本身（2026-08-31 新增）** | 板上那个 `PROBE B5v2` sketch = `SerialPort` + `OpenPLC_SDRAM::begin()` + 16 MB alloc/zero，**UDP 发现照常应答** —— `python tools/enter_bootloader.py` 走以太网把它请进了 bootloader，那条路径非应答不可。所以锅不在「用了 SDRAM」，在 `SDRAM_Acceptance` 的其它部分 |

**由此支持的解释（推断，不是实测）**：ICMP 通而绑在 56865 的 UDP pcb 不应答，正是 **`udp_bind` 失败**的形状 —— 那个 pcb 是独立的，绑不上不影响 lwIP 其余部分。

⚠️ **为什么两个 sketch 会不一样，还没查明。不要猜。**

**在哪找**：`core:cores/arduino/main.cpp:184`（应答器启动点）；`core:libraries/OpenPLC_IAP/src/udp_server.c`（应答器本体）；`core:libraries/OpenPLC_IAP/src/OpenPLC_IAP_Autostart.h`（诊断计数器声明）。

**可能的影响**：⚠️ **用户的 PLC 程序可能把自己锁在网络升级之外。** 不是变砖 —— BOOT0 长按、CDC、ST-Link 都还能进上传模式 —— 但**无人值守的网络升级会失效**，而那是这套 IAP 的主要用法。

顺带两条：
- **需求 A3 的成立范围要收窄**，它假设了「app 会应答发现」，现在已知有反例。A3 在 `$PROD/docs/STATUS.md` 因此从 ✅ 降成 🟡。
- **SD1 必须是任何序列里最后一条依赖网络的用例**，否则它后面所有用例都够不到板子。板级用例的标准载荷因此选 `SerialPort`，不是 `SDRAM_Acceptance`。

🚧 **下一步是现成的，不用猜**：用 `-DOPENPLC_DIAG_HEARTBEAT` 编一版那个 sketch（`core:cores/arduino/main.cpp:104-128`），它会周期打印 `udp_start` / `udp_rx` / `udp_tx` / **`bind_fail`** 计数 —— 那几个计数器**存在本身就说明这类事以前出过**，一眼就能确认或否掉。

⚠️ **改 core 是跨仓的共享基础设施**（见 `$PROD/docs/design/ARCHITECTURE.md`），所以定位之后再决定谁改、怎么发版。**定位本身不需要动 core，只要加一个编译宏。**

## ISS-A4 · 所有权命令还没进出货工具 IAPTool

| | |
|---|---|
| **是什么** | bootloader 的 `takeown` / `setowner` / `getowner` 三个命令 |
| **做什么用的** | 客户把板子绑到自己的签名密钥上（需求 C10） |
| **在哪找** | 板子侧 `IAPServer/IAP_server.c` + `IAPServer/owner_slot.c`，**已完成并实测**<br>主机侧只有 `$TOOL:TestCase/tools/run_takeown.py` / `run_setowner.py`，**那是内部测试脚本** |
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

2026-08-17 实测已回答"从哪个口看到的"：**COM5，就是 RS232 端子 C05/C06。** 原来"代码上这行到不了端子"那个推论是错的 —— `millis=12`，电荷泵余电还没塌，banner 蹭出去了。

| 方案 | 代价 | 风险 |
|---|---|---|
| **不改**，文档写明"靠余电，不可依赖" | 0 | app 启动一慢这行就没了，而没有任何东西保证那个时间 |
| core 在打 banner 前主动拉高 PB10 | 小 | ⚠️ **抢走用户 app 对收发器初始状态的控制权**，可能违反`$PROD/docs/CONVENTIONS.md` |

⚠️ 无论选哪个，**都不要再往 core 的这个位置加启动打印** —— 后加的越多，越可能超过余电窗口而静默丢失。

---

## 维护

- **做完就删掉，不要打勾留着。** 结论在 `$PROD/docs/STATUS.md` 里转 ✅ 就是记录。
- **一条问题只在这里描述一次。** 别在 [../test/MEASUREMENTS.md](../test/MEASUREMENTS.md) 再写一遍现象，那边只放实测数字。
- 新条目按上面的优先级表插进去，**编号只增不改**。
