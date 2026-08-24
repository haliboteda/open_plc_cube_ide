# 本仓库的项目笔记 —— 去哪查什么

**这里只有 bootloader 自己的东西。** 产品级的（仓库之间怎么分工、跨仓镜像清单、需求与用例总表、写文档的约定）在 `$PROD/docs/`，装机的在 `$PORT/docs/` —— 2026-08-24 搬走的，路径写法见 `$PROD/docs/CONVENTIONS.md`。

## 碰到什么，查哪份

| 想知道 | 去哪 |
|---|---|
| 需求做到哪一步、用例覆盖了没有 | `$PROD/docs/STATUS.md` |
| 某个编号（`T1`、`OW2`、`P7`、`ISS-B5`……）是什么 | `$PROD/docs/ID-MAP.md` |
| 三个仓库怎么分工、哪些代码跨仓镜像、RTC 备份寄存器谁占了哪个 | `$PROD/docs/design/ARCHITECTURE.md` |
| 用例的判据、怎么跑 | `$TOOL/TestCase/TEST-CASES.md` |
| 换台机器要 clone / 装 / 配什么 | `$PORT/docs/WORKSPACES.md`、`PREREQS.md`、`CONFIGURE.md` |

## 五个目录，按「多久变一次」分

### `design/` —— 为什么这么设计（很少变）

| 文件 | 装什么 |
|---|---|
| [design/OWNERSHIP.md](design/OWNERSHIP.md) | owner 槽与信任根：出厂公钥、A+/B 两种模式、状态机、三个操作、记录格式、诚实的限制 |
| [design/JOURNAL.md](design/JOURNAL.md) | journal 机制：扇区布局、启动扫描、`server_decide()` 的判定链、为什么验签而不是存哈希、擦除规则 |
| [design/HARDWARE-FACTS.md](design/HARDWARE-FACTS.md) | **核实过的硬件事实**，每条带日期和核实方式。RS232 走 PC10/PC11/PB10、UART4 双用、PG9 就是 BOOT0、39 根 FMC 引脚、SRAM4 不初始化 |
| [design/DECISIONS.md](design/DECISIONS.md) | 已定的设计决策，每条带理由 + **什么情况下值得重新讨论** |
| [design/DEFERRED-DESIGNS.md](design/DEFERRED-DESIGNS.md) | 刻意推后的设计：每板生产密钥、故障自愈（含为什么 IWDG 是死路）、Arduino 发现声明 |
| [design/KEYS.md](design/KEYS.md) | `IAPServer/keys/` 那五个文件、没有生成器这件事、`rotate_keys.sh`、两个坑 |
| [design/CUBEMX-RULES.md](design/CUBEMX-RULES.md) | ⚠️ **改这个 CubeIDE 工程的硬规矩**：生成区不能手工改、重新生成后必查两项、`.ld` 的 FLASH LENGTH 必须是 120K |

### `test/` —— 怎么证明（每次跑用例都可能变）

| 文件 | 装什么 |
|---|---|
| [test/MEASUREMENTS.md](test/MEASUREMENTS.md) | **每一个实测数字的唯一出处**。别处引用，不要抄第二份 |
| [test/CASE-DESIGNS.md](test/CASE-DESIGNS.md) | 每条用例为什么这么设计：三条原则、四层速度、还没跑的那几条、落点决定 |
| [test/COVERAGE-GAPS.md](test/COVERAGE-GAPS.md) | 诚实列出**没测到**的东西。「陈旧结论是一类没被测出来的缺陷」 |

### `work/` —— 手头在干什么（经常变）

| 文件 | 装什么 |
|---|---|
| [work/ISSUES.md](work/ISSUES.md) | 已知问题按优先级排，每条：症状 + 已核实的事实 + 下一步 |
| [work/BACKLOG.md](work/BACKLOG.md) | M1–M8 各是什么、怎么写一份模块文件、建议顺序 |
| [work/M1-owner-slot.md](work/M1-owner-slot.md) | owner 槽实现记录，六步全部实测。含「出厂复位＝永久变砖」那次近失事故 |
| [work/M2-cert-chain.md](work/M2-cert-chain.md) | 证书链（C11/C12），依赖 M1 |
| [work/M3-app-sdram.md](work/M3-app-sdram.md) | app 侧 SDRAM 库 ✅ 19/19 验收 |
| [work/M4-fmc-pin-guard.md](work/M4-fmc-pin-guard.md) | FMC 39 引脚守卫 ✅ —— 选了「可发现」而不是「拦截」 |
| [work/M5-serial-conflict.md](work/M5-serial-conflict.md) | `Serial_Test` 与 `Serial4` 抢 UART4 ✅ |
| [work/M6-test-gaps.md](work/M6-test-gaps.md) | 补没测到的那几项 —— 单位投入产出最高 |
| [work/M7-python-scripts.md](work/M7-python-scripts.md) | 把全套 PowerShell 测试脚本改写成 Python 3 |
| [work/investigations/sdram-d1.md](work/investigations/sdram-d1.md) | SDRAM D1（PD15）弱导通 —— **2026-08-23 换 upper deck 收尾，根因始终没定位到**。再出同样症状从这份开始看 |

⚠️ M8（一句「初始化」就能开工）**已搬出本仓库**，在 `$PORT/docs/M8-onboard-skill.md` —— 它是装机 skill 和 `bootstrap.py` 的立项文件，那两样都在那边。

### `archive/` —— 只增不改

| 文件 | 装什么 |
|---|---|
| [archive/RETRACTED.md](archive/RETRACTED.md) | **13 条被推翻的结论**（「我们以为 X，实测说不是」）。SDRAM D1 断路、可续传、备份域虚警、PG9/RESET…… |
| [archive/artifacts/RENDERED-SNAPSHOTS.md](archive/artifacts/RENDERED-SNAPSHOTS.md) | 六份 HTML 配图页的索引。⚠️ **渲染快照，不是事实来源**，打架以 `docs/` 为准 |

## 本仓库根目录的四份

| 文件 | 装什么 |
|---|---|
| `README.md` | 英文，对外：烧 bootloader、Arduino 侧怎么装 |
| `RELEASE-NOTES.md` | 英文，对外：升级规则、known issues、未验证项、发版检查单。**升级风险只靠它兜着** |
| `OpenPLC_Bootloader.md` | 工程结构：flash 分区、模块清单、lwIP 配置、构建配置 |
| `CLAUDE.md` | 开工入口：这个仓库是什么、源码在哪、改它有哪些硬规矩 |
