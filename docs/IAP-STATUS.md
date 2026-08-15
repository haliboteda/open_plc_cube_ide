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
| **传输中断可恢复** | 进程被杀 → app 区半写 → `BOOTLD-INVALID` → 重烧恢复 |
| **掉电中断（S4）** | 写到 49152 字节时拔电 → `Reset cause: POR` + `metadata present` + `App signature invalid`，板子保持可再烧写 |
| **全局限流** | 同条件（6 分钟 / 4 秒间隔 / IDE 轮询中）：改前 **12/60 失败** → 改后 **0/90**；app 侧 0/60；泛洪 1187 次查询只回 150 次 |
| **上传锁** | 日志出现 `broadcast skipped: an upload is in progress on this host`，锁释放后下一节拍立即恢复广播 |
| BOOT0 长按进上传模式 | ✅ |

## 已知未修

### `iap_auth_report_backup_domain()` 会误报（日志已改进，根因未定位）

一次软复位报"备份域丢失 / 计数器归零 / 重放保护被削弱"，但**真掉电后**的下一次启动显示 `counter = 35`，接在旧序列上 —— 备份域连真掉电都活下来了，那次软复位更不可能丢。是 **witness 寄存器读到错值**。

原代码还有个独立问题：它断言"计数器已归零"，但**从未写过那个寄存器**。

2026-08-15 改为两个分支都打印实际读到的 counter，并把"witness 丢了但 counter 非零"单独报为读取不可靠，不再谎称重放保护被削弱。**为什么会读错仍未查明。**

### 固定窗口限流的偏差

标称 50 次/秒，**实测可达 60 次/秒** —— 突发跨窗口边界时最多接近 2 倍。要硬上限得改成连续补充的令牌桶。

⚠️ **别把 50 当作保证值。**

### 其他

- `[BOOT]` 行前有个乱码字节 —— bootloader 末次 printf 和 app 重新 `begin()` UART 撞车，纯显示问题
- **bootloader 自报版本仍是 `0.1.2`，app 是 `0.1.3`** —— 工具日志里两个版本号对不上，会误导排查

## 仍未验证

- **两块板的 MAC 互不相同** —— 手上只有一块板。**建议进量产检查单**
- 真实 OpenPLC 主程序下的长期稳定性

## 已决策（不要重新讨论）

### 迁移风险：bootloader 与 app 必须捆绑升级

**metadata 存在 journal 里。** 新 bootloader 读不懂旧格式 journal → 读不到 metadata → 判定 app 无效 → **停在 bootloader 且不会自愈**，现象和变砖一样。

app 镜像本身一个字节都没变，是 bootloader 找不到那张"出生证明"了。

**处理方式：不改代码，写进发布说明** —— bootloader 升级必须和 app 升级捆绑。
⚠️ **这是唯一一个纯靠流程兜的风险，忘了写文档就等于没有。**

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
