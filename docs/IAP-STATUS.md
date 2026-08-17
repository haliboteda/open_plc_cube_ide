# IAP 验证状态

截至 **2026-08-16**。所有"已验证"都是真机实测，不是推理。

## 已验证

| 项 | 证据 |
|---|---|
| **CDC 全流程** | 连跑多次，含"bootloader 无有效 app"和"app 在跑 → 1200 touch → SRAM4 交接"两条路径 |
| **以太网全流程（两条路径）** | bootloader 侧：UDP 发现 → `BOOTLD-INVALID`/`BOOTLD` → getpubkey → TCP 烧写<br>app 侧：UDP 发现 → `CUSAPP` → 认证 reboot → 广播发现 + UID 匹配 → TCP 烧写 |
| **journal 新格式** | 一次成功升级 = 5 槽（4 metadata + 1 event）；满了不擦、只丢事件 |
| **journal reclaim** | 旧格式数据占满扇区时 `Reclaiming state sector (4096 slots discarded)` 正常执行并恢复 |
| **通道隔离** | `BOOT_REQ_CDC` 时**不启动以太网**；无有效 app 时启动。**这是设计不是故障** |
| **MAC 唯一化** | 两侧串口都打印出 `02:BB:49:3E:A8:02`，同 IP。改前 bootloader 写死 `00:80:E1:00:43:21`，所有板子相同 |
| **复位原因** | `PIN`（复位键）、`SOFT`（app 请求进上传模式）、`POR`（掉电重上电）三种全部正确 |
| **TCP 会话规则（T1–T4）** | 空闲 60s 踢人（实测 1m01s）、50s 不踢、拒绝第二连接、传输中不打断、关闭后可重连 |
| **签名校验（S1）** | 上传时先过 CRC、算哈希、再回 `Signature Failed`；下次启动 `metadata present` + `App signature invalid` —— 与"metadata 没写成"区分开 |
| **密钥不匹配（S2）** | 2026-08-17 实测：用一把板子不认的密钥**真实签名**（格式完全合法）的镜像 → `Signature Failed`；复位后旧 app 照常启动。**和 S1 分开测**：一个是"没有任何密钥能产生的签名"，一个是"签方不对" |
| **启动期签名校验（S3）** | 2026-08-17 实测：ST-Link 把已装好的 app 第 41858/83716 字节 `0x12`→`0xED` → 复位后 `metadata present` + `App signature invalid or absent`，**不启动 app**；重烧一次即恢复。⚠️ 注意 `App signature invalid or absent` 和 `no valid application` **是同一个分支打的**（`IAP_server.c:482-494`），必然同时出现 |
| **nonce 跨掉电不重复（AU1）** | 2026-08-17 实测：真断电（`Reset cause: POR`）前 counter 62→69，上电后 71→78，**16 个 nonce 全不同、计数器没归零**。中间 69→71 那两个是 `enter-bootloader` 自己的认证 reboot 消耗的 |
| **★ 所有权全链路（OW1/OW2/OW3）** | 2026-08-18 实测完整生命周期：认领 G1 → 换 owner G2（现任签名）→ 恢复出厂 cleared → 重新认领 G3。**认领后用旧公开密钥签的 app 被拒**（`UPLOAD Mod ... (no valid application)`）；坏签名的换 owner 被拒且什么都没动；**无签名的高 generation 记录夺不走已认领的板子**（链停在 G1，`getpubkey` 仍返回原主人） |
| **恢复出厂手势** | 2026-08-18 实测：复位后按住 BOOT0 超过 10 秒 → 三下快咔哒（ARMED）→ 松手 → `FACTORY RESET DONE at generation 2`。**旧的 1.5 秒判据一个字没改** —— 判据仍是"t=1.5s 那一瞬手按着"，轮询只在那之后才开始 |
| **PG9 改成输入后 RESET 正常** | 2026-08-18 受控实验（只改这一个变量）：**6 次物理复位全部 `Reset cause: PIN`**，4 次正常进 app、2 次按住 BOOT0 进上传模式。**否定了"改 input 会让 RESET 失效"那条挂了很久的怀疑** —— 真因是当年同一次改动删掉了 `SystemClock_Config()` |
| **★ `IAPTool` 退出 ≠ 升级完成** | 2026-08-17 实测踩到：IAPTool 送完最后一个字节就退，**板子此时才开始校验 → 擦除 → 从 SDRAM 写回**，要好几秒。那几秒里复位会毁掉 app（现象：`metadata present` + `App signature invalid`，重烧可恢复）。**任何自动化都要等板子自己说 `Checksum and signature OK`** |
| ~~传输中断可恢复~~ | ~~进程被杀 → app 区半写 → `BOOTLD-INVALID` → 重烧恢复~~ **⚠️ 已被 SDRAM staging 作废，见下** |
| ~~掉电中断（S4）~~ | ~~写到 49152 字节时拔电 → `Reset cause: POR` + `metadata present` + `App signature invalid`~~ **⚠️ 同上** |
| **SDRAM staging 正向路径** | 2026-08-17 实测（以太网，83,716 B 镜像）：`SDRAM staging buffer OK (2 MiB at C0000000)` → `Staging in SDRAM.` → 逐块 `Staged …` → `Transfer complete, verifying the staged image...` → **`Erasing application region` 出现在校验之后** → `Writing 83744 bytes from SDRAM to flash`（83,716 补齐到 32 的倍数）→ `Checksum and signature OK` → app 正常启动 |
| **SDRAM 自检** | 同上一次启动打印 OK。⚠️ 它在 `MX_FMC_Init()` 里，属于 **Phase 2** —— **只有停在 bootloader 时才会跑**，正常跳 app 的启动看不到这行，那不是失败 |
| **★ `IAPTool` 退出 ≠ 升级完成** | 2026-08-17 实测踩到：IAPTool 送完最后一个字节就打 `File transfer complete.` 并退出，**板子此时才开始校验 → 擦除 → 从 SDRAM 往 flash 写**，要好几秒。那几秒里复位（当时是 ST-Link 复位）会毁掉 app，现象是 `metadata present` + `App signature invalid or absent`，重烧可恢复。**任何自动化都要等板子自己说 `Checksum and signature OK. Rebooting...`，不能以 IAPTool 退出为准** |
| **★ 失败的上传不再破坏 app（G1）** | 2026-08-17 实测：S1 送一个签名故意写错的镜像 → `Signature verification FAILED - firmware not trusted. Application region untouched.` → **复位后 `** APP Mod ...`，旧 app 正常启动**。改前这里是 `BOOTLD-INVALID`、必须重刷。**这是 staging 存在的全部理由** |
| **回归：T1 / T1b / T2 / T3 / T4** | 2026-08-17 全过。T1 实测 1m1s 踢人，T1b 50s 不踢，T2 第二连接不被服务且第一个不受影响，T3 传输中闯入被 RST 拒，T4 关闭后可重连 |
| **回归：N1 / N2 / N3 / N5** | 2026-08-17 全过。N5：3 秒内 1200 次查询 → **150 次回复（正好 50/s）**，且随后正常发现仍然可用 |
| **回归：N4 浸泡** | 2026-08-17，app 侧：**10 分钟 240 次查询零失败，最慢 36ms**。对照 2026-08-15 全设备限流改造前的 25 分钟浸泡 18 次无应答 —— 那次的根因是我们自己的两个工具抢同一份配额 |
| **全局限流** | 同条件（6 分钟 / 4 秒间隔 / IDE 轮询中）：改前 **12/60 失败** → 改后 **0/90**；app 侧 0/60；泛洪 1187 次查询只回 150 次 |
| **上传锁** | 日志出现 `broadcast skipped: an upload is in progress on this host`，锁释放后下一节拍立即恢复广播 |
| BOOT0 长按进上传模式 | ✅ |

## 已知未修

> 这一节写**现象和已核实的事实**；对应要做什么、排在什么优先级，在 [TODO.md](TODO.md)。**同一件事不要在两边各描述一遍。**

### ~~`iap_auth_report_backup_domain()` 报"备份域丢失"~~ → **2026-08-17 已定位并修复**

**根因：跨仓备份寄存器撞车。** bootloader 把 VBAT witness 放在 `DR2`，而 app 侧（`core:libraries/OpenPLC_IAP/src/iap_auth.c:21`）用 `DR2` 当自己的 nonce 计数器。

app 侧的注释写着理由是"两个镜像不同时运行，所以没有冲突风险" —— **这个推理是错的**。备份寄存器的意义就是跨交接保存状态，**先后访问同一份持久状态就是冲突**。

**双向破坏，第二个方向更严重：**

| 方向 | 后果 |
|---|---|
| app 的计数器覆盖 witness | bootloader 每次启动误报"备份域丢失" —— 这只是**表象** |
| witness 的写入重置 app 的计数器 | 每次经过 bootloader，`DR2` 被写成 `0x56424154`，**app 随后重复发放同一批 nonce 编号** —— 重放保护只剩 `tick` 在撑。**这才是真正的缺陷** |

**修复**：witness 挪到空着的 `DR3`（`IAPServer/iap_auth.c`），**只改 bootloader，不动 core**，一处修好两个方向。

**实测**：连续三次进 bootloader，第一次是 DR3 的首次写入（正常报 missing），之后稳定 `Backup domain retained, nonce counter = 48/49`。

⚠️ 完整的寄存器分配表已进 [ARCHITECTURE.md](ARCHITECTURE.md) 的跨仓镜像一节。**`DR1` 还有一个潜在冲突**：bootloader 用它做 nonce 计数器，而 core 的 `backup.h:34` 把 `RTC_BKP_INDEX` 也定义成 `DR1`（当前无人写，谁引入 STM32RTC 库谁踩）。

<details><summary>定位过程（旧记录）</summary>

### `iap_auth_report_backup_domain()` 报过一次"备份域丢失"

**现象**：一次软复位报"备份域丢失 / 计数器归零 / 重放保护被削弱"，但**真掉电后**的下一次启动显示 `counter = 35`，接在旧序列上 —— 备份域连真掉电都活下来了。

原代码还有个独立问题：它断言"计数器已归零"，但**从未写过那个寄存器**。2026-08-15（`99db0ce`）改为两个分支都打印实际读到的 counter，并把"witness 丢了但 counter 非零"单独报为读取不可靠。

~~根因是 witness 寄存器读到错值，未查明。~~

~~2026-08-16 推测：witness 是新加的寄存器，第一次执行必然不匹配，报出来是正确行为。~~ **2026-08-17 实测否定** —— 又报了一次：

```
** Backup domain witness missing, but nonce counter = 39. **
```

而 counter 从上次的 35 涨到了 **39**。所以现在的事实比之前两个假设都更准：

> **同样的代码模式、同一组备份寄存器，`DR1`（计数器）写入持久，`DR2`（witness）不持久。**

- `IAPServer/iap_auth.c:45-48` 写 DR1 —— 持久，跨复位递增 ✅
- `IAPServer/iap_auth.c:148-150` 写 DR2 —— **每次都写，每次下次启动又读不到** ❌
- 两处都用 `HAL_PWR_EnableBkUpAccess()` 包着，`hrtc` 在 `Core/Src/main.c:198` 的 `MX_RTC_Init()` 里已初始化（`:199` 才调用报告函数）
- `Core/Src/rtc.c` 的 `MX_RTC_Init()` 没有任何清备份寄存器的动作

**机理仍未查明，不要再猜。** 下一步是实测：读回刚写完的 DR2 看写入当场是否生效，能把"写不进去"和"写进去了但没保住"分开。

**那次实测的结果**：`Witness write-back: 56424154 (want 56424154) - stuck, so it is lost later` —— 写得进去，是**事后被抹掉**。顺着这条线才找到 app 侧占用了同一个寄存器。诊断代码用完已删。

</details>

### 固定窗口限流的偏差

标称 50 次/秒，**实测可达 60 次/秒** —— 突发跨窗口边界时最多接近 2 倍。要硬上限得改成连续补充的令牌桶。

⚠️ **别把 50 当作保证值。** 2026-08-17 又复现了一次：同一天连着两轮 N5，一轮**正好 50/s**（1200 次查询 → 150 回复），另一轮 **58/s**（1199 次 → 175 回复）。**"测到 50" 只是窗口对齐的运气**，不能当成上限生效的证据。

### `[BOOT] millis=` 能出来，但是靠电荷泵的余电

~~2026-08-16 逐行核实代码后断言：这行根本到不了 RS232 端子。~~ **2026-08-17 实测否定** —— 它就在串口上：

```
** APP Mod ...
[?[BOOT] millis=12
UART echo ready
```

代码事实没错：core 在 `cores/arduino/main.cpp:168` 打 banner，`pinMode(RS232_EN_Pin, OUTPUT)` 在 **171 行**，且 core **从不写电平**，所以打印那一刻 PB10 确实是低的、MAX3221 名义上已经 shutdown。

**错的是"所以发不出去"这个推论。** 关键数字是 `millis=12`：bootloader 在 `server_jump_to_app()` 里刚把 PB10 拉低，**电荷泵靠外部电容的余电还没塌下来**，12 毫秒后仍能产生合规的 RS-232 电平。banner 是蹭着余电挤出去的。

⚠️ **它是靠时序巧合工作的，不是设计出来的路径。** app 启动只要慢过电荷泵的衰减时间，这行就没了 —— 而没有任何东西保证那个时间。**不要依赖它，也不要在 core 里往这个位置加更多启动打印。**

⚠️ **所以以前在串口上看到的 `[BOOT]` 一定不是从 C05/C06 出来的**（要么别的观测口，要么不同版本的 core/sketch）。**这一条待你确认当时的观测条件。**

修不修是设计决策：core 主动拉高 PB10 能让 banner 可用，但会抢走用户 app 对收发器初始状态的控制权 —— 违反[「设计不能限制用户的 app」](WORKING-AGREEMENTS.md)的可能性要先讨论。

### `[BOOT]` 行前的乱码字节：原记的根因是错的

~~"bootloader 末次 printf 和 app 重新 `begin()` UART 撞车"~~ —— **不成立**。bootloader 的 `__io_putchar`（`Core/Src/main.c:87`）是 `HAL_UART_Transmit(&huart4, &ch, 1, 10)`，**逐字节阻塞、末尾等 TC 标志**，函数返回时最后一个字节已完整移出。没有"未发完的 printf"可撞（115200 下单字节 87 µs，10 ms 超时碰不到）。

**2026-08-17 实测把范围缩小了。** 串口上抓到的是：

```
** APP Mod ...
[?[BOOT] millis=12      ← 乱码字节紧贴在 [BOOT] 前面
UART echo ready         ← 前面是干净的
```

`UART echo ready` 是 sketch 在 `setup()` 里 `digitalWrite(RS232_EN_Pin, HIGH)` **之后**打的，**它前面没有乱码**。所以 ⚠3（sketch 拉高时电荷泵爬升）**被排除** —— 而那正是我原先标成"最高把握"的一个。

乱码来自 **bootloader 交权那一刻**，也就是 ⚠1 或 ⚠2：

真实候选（⚠3 已排除）：

| | 窗口 | 机理 |
|---|---|---|
| ⚠1 | `server_jump_to_app()` 里 `HAL_UART_DeInit()` 之后、`Disable_RX_RS232()` 之前 | 芯片还开着，PC10（DIN）已被释放成浮空 → 输出跟噪声抖。**取决于 MAX3221 DIN 有无内部上拉，没查数据手册** |
| ⚠2 | `Disable_RX_RS232()` | 电荷泵停转，线电平从 mark 塌向 0V → 接收端可能判成起始位 |
| ⚠3 | sketch `digitalWrite(RS232_EN_Pin, HIGH)` | 电荷泵启动爬升，输出扫过整个范围 → 接收端判成起始位 + 帧错误。**最像**，因为紧挨着下一行有效输出 |

**先做实验再改代码**：sketch 里把 `digitalWrite(RS232_EN_Pin, HIGH)` 提到 `setup()` 最前面、加 `delay(50)` 再 `begin()`。乱码消失 = ⚠3 坐实；还在 = ⚠1/⚠2，那就把 `Disable_RX_RS232()` 挪到 `HAL_UART_DeInit()` 之前（先静默再放引脚）。**一次编译一次上电，比先改 bootloader 再重烧便宜得多。**

### 其他

- ~~bootloader 自报版本仍是 `0.1.2`~~ → **2026-08-17 已改成 `0.1.3`**，实测身份串两侧一致：`BOOTLD_0.1.3` / `CUSAPP_0.1.3`。⚠️ 两处仍靠人工同步（`Core/Inc/IAP_config.h` 与 core `boards.txt`），已进发版检查单
- **`[BOOT]` 行前的乱码字节仍在** —— ⚠1（DeInit 后引脚浮空）已通过调换顺序**排除**，剩 ⚠2（电荷泵塌陷本身）。见 [TODO.md](TODO.md) A2

## 仍未验证

### SDRAM staging 还剩三条没测

正向路径和验签失败（G1）都已实测，见「已验证」。剩下的都需要**人工断电**或难以构造：

| 场景 | 期待 | 为什么还没做 |
|---|---|---|
| 传输中拔电 | 旧 app 照常启动 | 要人工断电 |
| 擦写阶段拔电 | 报 app 无效、可重传 | 要人工断电，且窗口只剩**几秒**，不好命中 |
| `IAP_EVT_FLASH_WRITE_FAIL`（事件 8） | 镜像验过但拷 flash 失败时记这条 | 难以人为制造 flash 写失败 |

⚠️ 前两条就是 S4 拆开的两半 —— **原来那个 S4（写到 49152 字节拔电）已经失去意义**，那个时刻 flash 压根没被碰过。清单在 `$TOOL/TestTool/TEST-CASES.md` 的「未覆盖」。

### 其他

- **两块板的 MAC 互不相同** —— 手上只有一块板。**建议进量产检查单**
- 真实 OpenPLC 主程序下的长期稳定性

## 已决策（不要重新讨论）

### 迁移风险：bootloader 与 app 必须捆绑升级

**metadata 存在 journal 里**（机制见 [JOURNAL.md](JOURNAL.md)）。新 bootloader 读不懂旧格式 journal → 读不到 metadata → 判定 app 无效 → **停在 bootloader 且不会自愈**，现象和变砖一样。

app 镜像本身一个字节都没变，是 bootloader 找不到那张"出生证明"了。

**处理方式：不改代码，写进发布说明** —— bootloader 升级必须和 app 升级捆绑。

⚠️ **这是唯一一个纯靠流程兜的风险。** 2026-08-16 已写进 [../RELEASE-NOTES.md](../RELEASE-NOTES.md) 的 Upgrade rules（含"看着像变砖其实没砖、重传 sketch 即可"的补救步骤）。**再改动 journal 格式或 metadata 位置，第一件事是同步那一节。**

### 不再追求 boot / app 的 MAC 必须一致

用户 app 可能自定义 MAC。设备定位继续靠 **UDP 广播 + UID 匹配**，**这套兜底不能删**。

保留 bootloader 侧的 UID 派生 MAC，理由变成"每板唯一"（写死的 MAC 会让同网段两台板子直接冲突），而不是"和 app 一致"。

### 发现限流用全设备封顶，不按源 IP

**根因**：Arduino 核心包自带的 `network_discovery.exe`（IDE 一开就常驻）每 30 秒向板子发一次发现请求。板子按**源 IP** 限流（2 秒一次），同一台机器上的 IDE 插件和 IAPTool **共用配额**，谁落在对方 2 秒内谁被拒 —— 概率约 2/30。工具侧表现为 `No response, exiting`，极像板子挂了。

**按源计的粒度才是病根** —— 一台主机上永远可能有第二个程序，调窗口只能让碰撞变稀，消不掉。

放大倍数只有 **1.9 倍**（请求 24 字节 / 回复 46 字节），本就够不上反射放大器（DNS 约 50 倍、NTP 约 500 倍），按源限流买到的安全性远小于代价。

现在是 `DISCOVERY_MAX_REPLIES_PER_SEC = 50`，全设备封顶，bootloader 和 app 两边同步（**app 之前完全没有限流**）。合法用量约 2 次/秒，离上限差 25 倍，永不误伤。

**超限日志每个窗口只打一行** —— 否则泛洪时 UART 会被自己的日志拖死，日志本身成了比攻击更有效的拒绝服务。

配套还有两处：IAPTool 首次发现重试 3 次（间隔 2.5s 是刻意的，要跨过设备端的节流窗口），以及上传锁（见 [ARCHITECTURE.md](ARCHITECTURE.md) 的跨仓镜像表）。
