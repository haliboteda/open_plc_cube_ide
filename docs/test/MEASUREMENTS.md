# 实测记录

**这里是所有实测数字的唯一出处。** 别处（`$PROD/docs/STATUS.md`、`RELEASE-NOTES.md`、给硬件的报告）都只引用，不重述。

> ⚠️ **为什么要有这条规矩**：2026-08-22 清点时发现同一个数字在三个文件里写成三个值 —— T1 是 1m01s 还是 1m02s、N3 是 36ms 还是 40ms、P2 是 8 个锚点还是 9 个。前两个其实是**两次不同的跑**，只是谁都没标日期；第三个是真错。
>
> **所以这份表的每一行都带日期，同一个用例跑了两次就两行。** `$PROD/docs/STATUS.md` 的「最近结果」取最新那行。

**这里只放"测出来是什么"。** 要什么、证明了哪条需求、现在是什么状态 → `$PROD/docs/STATUS.md`。为什么这么设计 → [../design/](../design/)。还欠什么 → [../work/ISSUES.md](../work/ISSUES.md)。

---

## 设备行为 · TCP 会话（T1–T4）

| 日期 | 用例 | 结果 |
|---|---|---|
| 2026-08-17 | T1 T1b T2 T3 T4 | 全过。T1 踢人 **1m01s**；T1b 50s 不踢；T2 第二连接不被服务且第一个不受影响；T3 传输中闯入被 RST 拒；T4 关闭后可重连 |
| 2026-08-18 | T1 T1b T2 T3 T4 | 全过。T1 踢人 **1m02s**（同一条用例的第二次跑，判据是"约 60s"，两次都过） |

## 设备行为 · UDP 发现（N1–N5）

| 日期 | 用例 | 结果 |
|---|---|---|
| 2026-08-17 | N1 N2 N3 N5 | 全过。N5：3 秒内 1200 次查询 → **150 次回复（正好 50/s）**，随后正常发现仍可用 |
| 2026-08-17 | N4 浸泡 | app 侧 **10 分钟 240 次查询零失败，最慢 36ms**。对照 2026-08-15 限流改造前的 25 分钟浸泡 18 次无应答 —— 那次根因是我们自己的两个工具抢同一份配额 |
| 2026-08-18 | N1 N2 N3 N4 N5 | 全过。N3 最慢 **40ms**；N4 10min / 240 次 / 0 失败，最慢 **39ms**；N5 **1223 次 → 150 回复** |

⚠️ **N5 的 50/s 不是保证值。** 同一天连着两轮，一轮正好 50/s（1200→150），另一轮 **58/s**（1199→175）。**"测到 50"只是窗口对齐的运气**，不能当成上限生效的证据。机理见 [../design/DECISIONS.md](../design/DECISIONS.md) 第 3 条。

## 全局限流改造前后

| 日期 | 条件 | 改前 | 改后 |
|---|---|---|---|
| 2026-08-15 | 6 分钟 / 4 秒间隔 / IDE 轮询中 | **12/60 失败** | **0/90** |
| 2026-08-15 | app 侧同条件 | — | 0/60 |
| 2026-08-15 | 泛洪 | — | 1187 次查询只回 150 次 |

**上传锁**：日志出现 `broadcast skipped: an upload is in progress on this host`，锁释放后下一节拍立即恢复广播。

## 签名与完整性

| 日期 | 用例 | 结果 |
|---|---|---|
| — | S1 上传时签名校验 | 先过 CRC、算哈希、再回 `Signature Failed`；下次启动 `metadata present` + `App signature invalid` —— 与"metadata 没写成"区分开 |
| 2026-08-17 | **S2** 密钥不匹配 | 用一把板子不认的密钥**真实签名**（格式完全合法）的镜像 → `Signature Failed`；复位后旧 app 照常启动。journal 88→89 槽，说明拒绝这件事被记了事件。**和 S1 分开测**：一个是"没有任何密钥能产生的签名"，一个是"签方不对" |
| 2026-08-17 | **S3** 启动期签名校验 | ST-Link 把已装好的 app 第 **41858/83716** 字节 `0x12`→`0xED` → 复位后 `metadata present` + `App signature invalid or absent`，**不启动 app**；重烧一次即恢复 |
| 2026-08-17 | **G1** 失败的上传不破坏 app | S1 送一个签名故意写错的镜像 → `Signature verification FAILED - firmware not trusted. Application region untouched.` → **复位后 `** APP Mod ...`，旧 app 正常启动**。改前这里是 `BOOTLD-INVALID`、必须重刷。**这是 staging 存在的全部理由** |

⚠️ `App signature invalid or absent` 和 `no valid application` **是同一个分支打的**（`IAPServer/IAP_server.c:482-494`），必然同时出现。真正区分"app 坏了"和"metadata 也没了"的是 `Bootloader state:` 那行里的 `metadata present` / `absent`（`bootloader_state.c:155`）—— [../design/JOURNAL.md](../design/JOURNAL.md) 的表格一直是对的。

## 会话认证

| 日期 | 用例 | 结果 |
|---|---|---|
| 2026-08-17 | **AU1** nonce 跨掉电不重复 | 真断电（`Reset cause: POR`）前 counter **62→69**，上电后 **71→78**，**16 个 nonce 全不同、计数器没归零**。中间 69→71 那两个是 `enter-bootloader` 自己的认证 reboot 消耗的 —— 断言的是**严格递增**，不是"恰好加一"（阶段**内**才要求恰好加一） |
| 2026-08-17 | **H2** 主机侧 C 单测 | **首次真正跑起来**，11 条断言全过（MinGW-w64 gcc 16.1.0）。⚠️ 此前长期 SKIP，而它**早就编不过了** —— 见 [COVERAGE-GAPS.md](COVERAGE-GAPS.md) |
| 2026-08-17 | **X1 X2** 加密交叉验证 | X1 309 个向量通过；X2 8/8 通过 |
| 2026-08-17 | **K1–K6** 传输前密钥匹配 | 六种情况全过 |
| 2026-08-17 | **DG1** 降级拦截 | 五个用例全过：`refuse-older` / `allow-older` / `ask-no-console` / `same-version` / `newer` |

## 所有权（owner 槽）

| 日期 | 用例 | 结果 |
|---|---|---|
| 2026-08-18 | **★ OW1 / OW2 / OW3** 全链路 | 完整生命周期：认领 G1 → 换 owner G2（现任签名）→ 恢复出厂 cleared → 重新认领 G3。**认领后用旧公开密钥签的 app 被拒**（`UPLOAD Mod ... (no valid application)`）；坏签名的换 owner 被拒且什么都没动；**无签名的高 generation 记录夺不走已认领的板子**（链停在 G1，`getpubkey` 仍返回原主人） |
| 2026-08-18 | 恢复出厂手势 | 复位后按住 BOOT0 超过 10 秒 → 三下快咔哒（ARMED）→ 松手 → `FACTORY RESET DONE at generation 2`。**旧的 1.5 秒判据一个字没改** —— 判据仍是"t=1.5s 那一瞬手按着"，轮询只在那之后才开始 |
| 2026-08-18 | **P6** 公开根指纹 | 通过，负向对照验过会响 |

★ **OW1 当场抓到「认领曾经是安全剧场」** —— 验签写死了公开密钥，认领之后照样能用出厂密钥烧固件。已修；**为什么只有这条用例抓得到**，见 [../work/M1-owner-slot.md](../work/M1-owner-slot.md)。

## 升级通道

| 日期 | 项 | 结果 |
|---|---|---|
| — | **CDC 全流程** | 连跑多次，含"bootloader 无有效 app"和"app 在跑 → 1200 touch → SRAM4 交接"两条路径 |
| — | **以太网全流程（两条路径）** | bootloader 侧：UDP 发现 → `BOOTLD-INVALID`/`BOOTLD` → getpubkey → TCP 烧写<br>app 侧：UDP 发现 → `CUSAPP` → 认证 reboot → 广播发现 + UID 匹配 → TCP 烧写 |
| — | **通道隔离** | `BOOT_REQ_CDC` 时**不启动以太网**；无有效 app 时启动。**这是设计不是故障** |
| — | BOOT0 长按进上传模式 | 通过 |

### ★ `IAPTool` 退出 ≠ 升级完成（2026-08-17 实测踩到）

IAPTool 送完最后一个字节就打 `File transfer complete.` 并退出，**板子此时才开始校验 → 擦除 → 从 SDRAM 往 flash 写**，要好几秒。

那几秒里复位或断电**会毁掉 app**：现象是 `metadata present` + `App signature invalid or absent`，重烧可恢复。（那次是 ST-Link 复位，不是断电，所以不算 S4b 通过。）

⚠️ **任何自动化都要等板子自己说 `Checksum and signature OK. Rebooting...`，不能以 IAPTool 退出为准。**

这条也说明 **S4b 的窗口比想象中好命中** —— 不用掐秒表，等 IAPTool 一退出就动手即可。

## SDRAM

| 日期 | 项 | 结果 |
|---|---|---|
| 2026-08-17 | **staging 正向路径** | 以太网，83,716 B 镜像：`SDRAM staging buffer OK (2 MiB at C0000000)` → `Staging in SDRAM.` → 逐块 `Staged …` → `Transfer complete, verifying the staged image...` → **`Erasing application region` 出现在校验之后** → `Writing 83744 bytes from SDRAM to flash`（83,716 补齐到 32 的倍数）→ `Checksum and signature OK` → app 正常启动 |
| 2026-08-17 | **SD1** `OpenPLC_SDRAM` 库 | 19/19 通过。清零 **91 MB/s**（64MB ≈ **701 ms**），故有 `allocUninitialized()`。⚠️ **这份证据已失效** —— 见下 |
| 2026-08-18 | **BG1** 启动门禁 | 通过，`SDRAM staging buffer OK` |
| **2026-08-21** | **BG1** 启动门禁 | ❌ **失败**，`** SDRAM SELF-TEST FAILED at offset 00000000 **`，3/3 复现 |

⚠️ **2026-08-21 起，上面 SDRAM 的两条 ✅ 都不再可信** —— D1 线导通极弱，`OpenPLC_SDRAM` 库用的是同一片内存。测量数据、已排除的解释、还剩的两个候选全在 [../work/investigations/sdram-d1.md](../work/investigations/sdram-d1.md)。

⚠️ SDRAM 自检在 `MX_FMC_Init()` 里，属于 **Phase 2** —— **只有停在 bootloader 时才会跑**，正常跳 app 的启动看不到这行，那不是失败。

## journal 与诊断

| 日期 | 项 | 结果 |
|---|---|---|
| — | **journal 新格式** | 一次成功升级 = 5 槽（4 metadata + 1 event）；满了不擦、只丢事件 |
| — | **journal reclaim** | 旧格式数据占满扇区时 `Reclaiming state sector (4096 slots discarded)` 正常执行并恢复 |
| — | **复位原因** | `PIN`（复位键）、`SOFT`（app 请求进上传模式）、`POR`（掉电重上电）三种全部正确 |
| 2026-08-17 | **备份域 witness 修复后** | 连续三次进 bootloader，第一次是 DR3 的首次写入（正常报 missing），之后稳定 `Backup domain retained, nonce counter = 48/49` |
| — | **MAC 唯一化** | 两侧串口都打印出 `02:BB:49:3E:A8:02`，同 IP。改前 bootloader 写死 `00:80:E1:00:43:21`，所有板子相同。⚠️ **"两块板不同"从未观察过** |

## 硬件与平台

| 日期 | 项 | 结果 |
|---|---|---|
| 2026-08-18 | **PG9 改成输入后 RESET 正常** | 受控实验（只改这一个变量）：**6 次物理复位全部 `Reset cause: PIN`**，4 次正常进 app、2 次按住 BOOT0 进上传模式。**否定了"改 input 会让 RESET 失效"那条挂了很久的怀疑** —— 真因是当年同一次改动删掉了 `SystemClock_Config()`。见 [../archive/RETRACTED.md](../archive/RETRACTED.md) 第 10 条 |
| 2026-08-17 | **M5** `Serial_Test` 抗 `Serial4.begin()` | 5/5 回显通过（挪到 USART3，同引脚 AF7 而非 AF8）。**修之前实测：app 挂死、板子失联、只能 ST-Link 救** |
| 2026-08-17 | **P4** 变体 FMC 保留脚断言 | 通过，负向对照验过会响 |
| 2026-08-17 | **P1 P2 P3** 静态检查 | 全过。P1 版本号三处一致（0.1.3）；P2 跨仓镜像 **9 个锚点** + 备份寄存器占用（当天新增第 9 个：FMC 39 脚映射，负向对照验过会响）；P3 core live 与 git 一致（仅 `.gitignore` 单边） |
| 2026-08-17 | **P5** 每个 example 都编得过 | 首跑就抓到 KNX 默认配置用不了 group object，修后 7/7 通过 |
| 2026-08-17 | **E3** 镜像尺寸 | **96,944 B** / 上限 122,880 B，余量 **21%** —— M1 第 1 步之后（只预留，没加代码） |
| 2026-08-17 | **E3** 镜像尺寸 | **97,580 B**，M1 第 2 步之后（owner 槽只读扫描，+636 B） |
| **2026-08-23** | **E3** 镜像尺寸 | **101,340 B** / 上限 122,880 B，余量 **21,540 B = 17.5%**。0 errors 0 warnings；`text 100948 · data 384 · bss 209395` |

**08-17 到 08-23 涨了 4,396 B，绝大部分是产品代码** —— M1 第 3–6 步（公开根告警、`takeown`、`setowner` 链上验签、恢复出厂）。

⚠️ **我上一版把这 4,396 B 全归给了 `sdram_diag.c`，那是错的。** 实测：把 `sdram_diag` 整个摘掉，镜像从 102,328 降到 101,340 —— **诊断代码只占 988 B**，其余是 owner 槽。**没核实就归因，这正是「不要想当然」那条规矩要防的**。

（`sdram_diag` 2026-08-23 已按用户要求从工程移除，所以 101,340 就是当前产品镜像。）

⚠️ **P2 的锚点数是 9，不是 8。** 第 9 个（FMC 39 脚映射）2026-08-17 加的，当时有两处文档没跟着改，2026-08-22 统一成 9。

## 语言与单位约定

- **日期一律写全**（`2026-08-17`），不写"上周""前几天"。一条 2026-08 的结论，两年后读的人得能判断它还算不算数。
- **没验证的东西标出来。** 写"未验证"比写一个看起来很确定的猜测好得多。
- **推断和实测分行写**，别混在一句里 —— [../archive/RETRACTED.md](../archive/RETRACTED.md) 第 6 条就是混在一句里造成的。
