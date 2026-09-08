# 本仓库的项目笔记 —— 去哪查什么

**这里只有 bootloader 自己的东西。** 产品级的（仓库之间怎么分工、跨仓镜像清单、需求与用例总表、写文档的约定）在 `$PROD/docs/`，路径写法见 `$PROD/docs/CONVENTIONS.md`。

## 碰到什么，查哪份

| 想知道 | 去哪 |
|---|---|
| 需求做到哪一步、用例覆盖了没有 | `$PROD/docs/STATUS.md` |
| 某个编号（`T1`、`OW2`、`P7`、`ISS-B2`……）是什么 | `$PROD/docs/ID-MAP.md` |
| 三个仓库怎么分工、哪些代码跨仓镜像、RTC 备份寄存器谁占了哪个 | `$PROD/docs/design/ARCHITECTURE.md` |
| 用例的判据、怎么跑 | `$TOOL/TestCase/TEST-CASES.md` |

## 三个目录，按「多久变一次」分

### `design/` —— 硬件事实与设计决策（很少变）

| 文件 | 装什么 |
|---|---|
| [design/HARDWARE-FACTS.md](design/HARDWARE-FACTS.md) | **核实过的硬件事实**，每条带出处。RS232 路径、UART4 与 USART3、PG9 就是 BOOT0、39 根 FMC 引脚、SRAM4 不初始化、Digital In 上拉、模拟量与 VREFBUF、模拟前端九个跳线、**CAN**、**KNX**、**引脚分配 xlsx 管什么不管什么** |
| [design/OWNERSHIP.md](design/OWNERSHIP.md) | owner 槽与信任根：出厂公钥、A+/B 两种模式、状态机、三个操作、记录格式、限制 |
| [design/JOURNAL.md](design/JOURNAL.md) | journal 机制：扇区布局、启动扫描、`server_decide()` 的判定链、为什么验签而不是存哈希、擦除规则 |
| [design/DECISIONS.md](design/DECISIONS.md) | 已定的设计决策，每条带理由 + **什么情况下值得重新讨论** |
| [design/DEFERRED-DESIGNS.md](design/DEFERRED-DESIGNS.md) | 刻意推后的设计：每板生产密钥、故障自愈（含为什么 IWDG 是死路）、Arduino 发现声明 |
| [design/KEYS.md](design/KEYS.md) | `IAPServer/keys/` 那五个文件、没有生成器这件事、`rotate_keys.sh`、两个坑 |
| [design/CUBEMX-RULES.md](design/CUBEMX-RULES.md) | ⚠️ **改这个 CubeIDE 工程的硬规矩**：生成区不能手工改、重新生成后必查两项、`.ld` 的 FLASH LENGTH 必须是 120K |
| [design/security-design.html](design/security-design.html) | **整份安全设计的配图版**，浏览器打开、可缩放。**前半「怎么走」**：场景索引（8 个场景 → 该看哪张图）、五张泳道流程图（上传 / 认领 / 发证书 / 换根 / 恢复出厂，每个节点标清入→做→出）、串口诊断表。**后半「具体是什么」**：12 张主题图，信任模型、on-flash 字节格式、flash 布局、链解析、nonce、BOOT0 手势、日志、边界 |
| [design/PORTTOOL-FLOW.md](design/PORTTOOL-FLOW.md) | 端口测试工装，总分两部分。**A 总**：上位机和板子的完整契约 —— 控制什么（会话 vs 交权）、9 条 `pt.*` 命令（含 `pt.echo` 回环与 `pt.run` 一次性动作）、每条的确切应答、`!` 采样帧字段。只看这部分就够写上位机。**B 分**：每个端口怎么接、敲什么、看到什么算过、哪些坑会让判据静默失效。**C 附**：`PORTTOOL_ENABLE` 怎么把业务线整条绕开、物理连接、上位机形态与分发。图是 Mermaid，可缩放 |
| [design/PRODUCTION-TEST-GAP.md](design/PRODUCTION-TEST-GAP.md) | **硬件工程师的产线测试指南逐项要求什么，软件侧到哪一步。**八组提取表（电源 / 主控 / 输入通信 / 输出 / 校准 / 固件版本 / 产线追溯 / 外部仪器），判据是「**能不能自动判定**」不是「有没有人能手动测」。附两件还要问硬件侧的事，其中 **AI/AO 的 0.1 % 是否硬指标**挡着整块校准 |
| [design/PRODUCTION-FRAMEWORK.md](design/PRODUCTION-FRAMEWORK.md) | **产线上位机的形状**：方案文件（JSON）怎么写、六个框架级通用字段（`execute_condition` / 重试 / 前后延时 / 超时）的语义、三个 `Pt*` 步骤类型 + 四个非设备类型、执行语义、报告要装什么。开头一节是用户给的参考架构（另一个项目的产线工装截图）读解 —— **三条要抄的、一条不抄的** |
| [design/FIXTURE-INTERFACE.md](design/FIXTURE-INTERFACE.md) | **给硬件工程师的工装板接口清单**：每个通道要什么激励、读什么量、上电默认态、工装板↔上位机走什么总线。开头三条硬约束先看 —— 控制通道是 RS232 端子不许碰、扩展口的 `PH13/PH14` 就是那个 UART、模拟前端档位已定且焊上不可逆 |

### `test/` —— 怎么测、测出什么

| 文件 | 装什么 |
|---|---|
| [test/BOARD-BRINGUP-CASES.md](test/BOARD-BRINGUP-CASES.md) | **板级 10 项测试用例：接哪几个端子、判据是什么、结果** |
| [test/PORTTOOL-FIRST-BENCH.md](test/PORTTOOL-FIRST-BENCH.md) | **端口工装第一次上板的清单，带待填结果栏。**三个只有真硬件能回答的数（VNQ5160K 的 PWM 上限、模拟跳线焊没焊、AOUT 实际电流）就在这里等着填 |

### `work/` —— 手头还没完的

| 文件 | 装什么 |
|---|---|
| [work/ISSUES.md](work/ISSUES.md) | 已知问题按优先级排，每条：症状 + 已核实的事实 + 下一步 |
| [work/BACKLOG.md](work/BACKLOG.md) | 还需要立项的模块，以及每份模块文档怎么写 |

## 本仓库根目录的四份

| 文件 | 装什么 |
|---|---|
| `README.md` | 英文，对外：烧 bootloader、Arduino 侧怎么装 |
| `RELEASE-NOTES.md` | 英文，对外：升级规则、known issues、未验证项、发版检查单。**升级风险只靠它兜着** |
| `OpenPLC_Bootloader.md` | 工程结构：flash 分区、模块清单、lwIP 配置、构建配置 |
| `CLAUDE.md` | 开工入口：这个仓库是什么、源码在哪、改它有哪些硬规矩 |
