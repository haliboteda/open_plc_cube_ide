# 编号登记表：哪个编号住在哪

**说到任何编号，在同一句话里给出这里的路径。** 2026-08-22 建。

**为什么需要这份文件**：清点时发现全项目有 **13 套编号**在混用，其中 `A1` 同时是四样东西 ——「USB CDC 能烧写」（需求）、「go test」（selfcheck 第 1 步）、「主机侧 Go 测试」（验收单第 1 项）、「已修的版本号问题」（待办）。**说"A7 过了"字面上有四个意思。**

2026-08-22 消掉了三套：**selfcheck 的步骤号整套删除**（每一步本来就只是某个用例的别名）、待办加 `ISS-` 前缀、验收单加 `CHK-` 前缀。**还剩 10 套，两两不撞。**

---

## 现在有效的编号

| 编号形态 | 是什么 | 定义在哪 | 谁实现 | 结果记在哪 |
|---|---|---|---|---|
| `A1`–`A7` `B1`–`B11` `C1`–`C14` `D1`–`D9` `E1`–`E8` `F1`–`F5` | **产品需求**（54 条，每条一句可判定的话） | [STATUS.md](STATUS.md) 第三节 | — 它们是主张，不是可执行的东西 | 同一张表的「状态」列 |
| `T1` `T1b` `T2` `T3` `T4` | **TCP 会话用例** | `$TOOL:TestTool/TEST-CASES.md` | `$TOOL:TestTool/tcp_session.go`（`register(testCase{id:"T1"…})`—— **编号写死在 Go 代码里，不能改**） | [test/MEASUREMENTS.md](test/MEASUREMENTS.md) |
| `N1`–`N5` | **UDP 发现用例** | `$TOOL:TestTool/TEST-CASES.md` | `$TOOL:TestTool/udp_discovery.go` | [test/MEASUREMENTS.md](test/MEASUREMENTS.md) |
| `S1` `S2` `S3` `S4a` `S4b` `G1` | **签名 / 掉电 / staging 用例** | `$TOOL:TestTool/TEST-CASES.md`；S4a/S4b 的完整设计在 [test/CASE-DESIGNS.md](test/CASE-DESIGNS.md) | S1/S2 → `signature.go` + `signature_wrongkey.go`；S3 → `tools/run-s3.ps1`；**S4a/S4b → `tools/run_s4.py`**；G1 → `tools/run-case.ps1 -Case S1 -ThenReset` | [test/MEASUREMENTS.md](test/MEASUREMENTS.md) |
| `OW1` `OW1-neg` `OW2` `OW2-attack` `OW3` | **所有权用例** | `$TOOL:TestTool/TEST-CASES.md` | `tools/run-takeown.ps1`、`run-setowner.ps1`、`inject-owner-record.ps1` | [test/MEASUREMENTS.md](test/MEASUREMENTS.md) |
| `AU1` | **nonce 跨掉电不重复** | `$TOOL:TestTool/TEST-CASES.md` | `$TOOL:TestTool/nonce_replay.go`，由 `tools/run-au1.ps1` 编排 | [test/MEASUREMENTS.md](test/MEASUREMENTS.md) |
| `H1` `H2` `H3` `K1`–`K6` `X1` `X2` `DG1` | **主机侧用例**（不需要板子） | `$TOOL:TestTool/TEST-CASES.md`；DG1 的落地决定在 [test/CASE-DESIGNS.md](test/CASE-DESIGNS.md) | H1 → `go test ./TestTool/...`；**H3 → `go vet ./...`（2026-08-22 新建）**；H2 → `host/bootloader_unit/build.py`；K → `host/fakeboard/run_cases.py`；X → `host/crypto_ref/run_checks.py`；DG1 → `host/fakeboard/run_downgrade.py` | [test/MEASUREMENTS.md](test/MEASUREMENTS.md) |
| `P1`–`P10` | **静态检查**（P1–P6 看代码，P7–P9 看文档，**P10 看本机 `.claude/` 权限配置，纯建议性、不进 selfcheck 的 15 步**——那份文件本机专属不进 git，不能当发版门禁）| `$TOOL:TestTool/TEST-CASES.md` | `tools/check_version_sync.py`(P1)、`check_mirror_sync.py`(P2)、`check_core_sync.py`(P3)、`host/variant_check/build.py`(P4)、`host/examples_build/build.py`(P5)、`check_public_root.py`(P6)、`check_status_sync.py`(P7)、`check_doc_dupes.py`(P8)、`check_doc_paths.py`(P9)、`check_allow_hygiene.py`(P10) | [test/MEASUREMENTS.md](test/MEASUREMENTS.md) |
| `BG1` | **启动门禁**（SDRAM 自检 + 日志口通不通） | `$TOOL:TestTool/acceptance/checklist.md` 的「BG1 · 启动门禁」节 | `tools/flash_bootloader.py` 判读；日志文案在 `Core/Src/fmc.c` | [test/MEASUREMENTS.md](test/MEASUREMENTS.md) |
| `SD1` `SD2` `M3` `M5` `O1` `EV1` | **零散的板级用例** | `$TOOL:TestTool/TEST-CASES.md` | SD1 → `onboard/sdram/`；**SD2 不是用例，是仪器**（`Core/Src/sdram_diag.c`，命令 `sdramdiag`/`sdramlive`，没有 PASS/FAIL 判据）；M3 要第二块板；M5 → `onboard/`；O1 → `onboard/rs232/SerialPort`；EV1 ⛔ 难以构造 | [test/MEASUREMENTS.md](test/MEASUREMENTS.md) |
| `CHK-A1`–`A7` `CHK-B1`–`B7` `CHK-C1`–`C7` | **三张验收单**（改动后自检 / 发版 / 单板出厂） | `$TOOL:TestTool/acceptance/checklist.md` | 大部分是"跑某条命令"或人工 | ⬜ **没有去处** —— 逐板记录不存在，这就是 F2 为什么是 🟡 |
| `ISS-A2` `ISS-A3` `ISS-A4` `ISS-B1` `ISS-B2` `ISS-B4` `ISS-B5` `ISS-C1` `ISS-C4` `ISS-D1` `ISS-E1` | **已知问题 / 技术债** | [work/ISSUES.md](work/ISSUES.md) | — 它们是问题，不是测试 | 同一个文件，做完就删 |
| `M1`–`M8` | **设计模块**（要立项、分步做的） | [work/BACKLOG.md](work/BACKLOG.md)，一个模块一份 `work/M<n>-*.md` | 每份自己的「分步计划」 | 各自的「验收」节 + BACKLOG 的状态列 |

---

## 2026-08-22 删掉了什么

### selfcheck 的 `A0`–`A14`（整套删除）

12 步每一步都只是某个用例的别名。改成用例号本身之后，**一整套编号消失，`A1`/`A2`/`A3`/`A7` 的四义歧义少掉一个来源**。

| 旧步骤号 | 现在叫 | 跑的是什么 |
|---|---|---|
| `A0` | `ENV` | 这台机器有哪些工具链（不是用例，是探测） |
| `A1` | **`H1`** | `go test ./TestTool/...` |
| `A2` | **`H2`** | 主机侧 C 单测（真实 `sha256.c` / `iap_keyderive.c` / `iap_auth.c`） |
| `A3` | **`H3`** ← **新建的编号** | `go vet ./...` |
| `A7` | **`P1`** | 版本号三处一致 |
| `A8` | **`P2`** | 跨仓镜像没分叉 |
| `A9` | **`P3`** | core live 与 git 一致 |
| `A10` | **`K1–K6`** | IAPTool 传输前的密钥匹配决策 |
| `A11` | **`X1 X2`** | 加密交叉验证 |
| `A12` | **`DG1`** | 降级被拦下 |
| `A13` | **`P4`** | Arduino 变体的 FMC 保留脚断言 |
| `A14` | **`P6`** | 公开根指纹 |
| `A4` `A5` `A6` | — | **从来不存在。** 对话里说的 "A4/A5" 一般指验收单的 `CHK-A4`（构建）/ `CHK-A5`（烧写） |
| `A15` | — | **不是步骤号。** 那是配置变量 `$A15` / `A15_DIR`（Arduino15 数据目录），在 `tools/_common.ps1` / `tools/common.py` 里 |

★ **顺带补掉一个真实缺口**：`A1`（go test）和 `A3`（go vet）此前**映射不到任何用例号，所以它们的结果从来没有被写进任何文件**。给 `go vet` 建了 `H3` 之后，每一步在 [STATUS.md](STATUS.md) 里都有行。

同日又新增三步：**`P7`**（总表和用例不得漂移）、**`P8`**（一个事实只能写在一个文件里）、**`P9`**（文档里提到的路径必须存在）。**它们看的是文档，不是固件** —— 因为 2026-08-22 证明了这两类漂移靠眼睛找不出来。

### `T0` → `BG1`

`T0` 不属于 `T1`–`T4` 那一系列：它在另一个仓库、另一个文件、另一种性质（启动门禁不是设备行为用例），`TEST-CASES.md` 里根本没有 `T0` 这一条。

改名之后，提交信息里"T0 fails"这种话不再和 T 系列混淆。**`Core/Src/fmc.c` 的日志文案没动** —— 那是日志不是编号。

### 待办和验收单加前缀

- 原来 docs/TODO.md 里的裸 `A1`/`B4`/`C2` → `work/ISSUES.md` 的 `ISS-A2`/`ISS-B4`/…。**字母数字保留没变**，这样旧提交信息和聊天记录里的编号还查得到。
- `checklist.md` 的三张表 → `CHK-A*`/`CHK-B*`/`CHK-C*`。

---

## 还剩的三处会看错的地方

**这三处不是 bug，是同一个字母数字在不同层次上的合理重用。列在这里，看到时不用重新推。**

| 看到 | 可能是 | 怎么分辨 |
|---|---|---|
| **`M3`** | ① **用例**：两块板的 MAC 不同<br>② **设计模块**：app 侧 SDRAM 库 | 上下文提"MAC"或"第二块板"→ 用例；提"SDRAM 库"或"链接脚本"→ 模块。两者不相关 |
| **`M5`** | ① **用例**：`Serial_Test` 抗 `Serial4.begin()`<br>② **设计模块**：串口冲突 | **两者是同一个主题**，不会导致误解 |
| **`D1`** | ① **需求**：journal metadata 一次升级 5 槽<br>② **已知问题** `ISS-D1`：限流是固定窗口<br>③ **硬件网络名**：SDRAM 的第 1 根数据线（`PD15`） | ③ 是最常出现的那个。提"线""`PD15`""SDRAM"→ 硬件网络。⚠️ `D0`–`D15` 整套都是网络名 |

`CHK-B1`/`CHK-B2`/`CHK-B3` 和 `P1`/`P2`/`P3` **是同一批检查的两个名字**（`TEST-CASES.md` 明说了"前三个对应发版检查单的 B1/B2/B3"）。**没有合并** —— 按 [process/WORKING-AGREEMENTS.md](process/WORKING-AGREEMENTS.md) 的「不要擅自 dedup 或删除」，这个要单独问过再动。

---

## 加新编号的规矩

1. **先看这张表有没有撞。** 撞了就换一个前缀，不要"反正上下文能分清"。
2. **编号只增不改。** 用例号写死在 Go 代码和脚本里；需求号被用例引用。
3. **新起一套编号之前先问：它能不能就用现有某一套？** selfcheck 原来那套步骤号就是没问这句话的后果 —— 一整套编号，零信息量，一个四义歧义。
4. **加完回来更新这张表。**
