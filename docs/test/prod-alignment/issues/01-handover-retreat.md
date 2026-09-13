# 01 · 14 个交权入口退出面板，能力改成会话参数

Type: task
Status: ✅ resolved（2026-09-13）—— 六个小项全完。`selfcheck` **16/16**、H5 **194 项全过**、station6 在模拟板上 **30/30**

## Question

按 `DECISIONS.md` 38（上位机是监工）和 40，把交权那条路从产线路径上撤掉。**具体改哪几处？**

## 要做的

1. **`porttool_handover.c` 的 14 项全部改成 `HANDOVER_NOT_IN_CAPS`** —— 面板上一个不留，`pt.handover <名字>` 命令行仍然敲得到，供研发排查。
2. **补三样会话侧的能力**（这是「该补的」）：

   | 补什么 | 顶掉哪个交权项 | 出处 |
   |---|---|---|
   | CAN 会话加 `mode=echo` | `can.echo` | 会话已有 `normal/listen/loopback/extloop`，加第五个 |
   | 新增 `pt.run rs485.pins` 报 `PD4` 引脚级状态 | `rs485` 交权版唯一不重复的那一项 | 会话帧 `!rs485` 里没有引脚字段 |
   | 新增 `pt.run sdram.crc` 报 `k=v` | `sdram.crc`（**目前只有交权一条路**） | `porttool_run.c` 的 `targets[]` |

3. **`sd.integrity` / `sdram.retention` 改成会话**，让它们能进「持续」。SD 那个已分块（4 KiB 缓冲 + 种子可重放）。
4. **重生成 `caps_golden.txt`**，改 H4 契约断言，改 `station6-poweron.json` 里引用到交权目标的步骤。

⚠️ **顺序**：先补第 2、3 条，再撤第 1 条，否则中间有一段时间那几项测不了。

## 核实过的，不必重查

- `CAN_Test_Scope_Run`（`$BOOT/TestCase/CAN/can_test.c:800`）三步里，`[2]`=`FDCAN_MODE_EXTERNAL_LOOPBACK` 连发、`[3]`=`FDCAN_MODE_NORMAL` 连发 —— **会话的 `mode=extloop` / `mode=normal` 加小 `period` 完全等价**。只有 `[1]` 的 `can_scope_pin_toggle`（PB9 当 GPIO 打固定方波）是会话没有的，那是给示波器一个干净触发沿的 DVT 手段。
- KNX 会话 `mode=frames` 已经逐帧给 `raw=` / `inv=` / `crc=`（`DECISIONS.md` 32），产线要的就是这个。
- `bringup` 的五项（DIN/继电器/AI/AO/温度）**五个端口都有会话**，复选就能一起跑。


---

## 进度

### ✅ `can` 加 `mode=echo`（2026-09-13，验过）

- `porttool_can.c`：新增 `can_role_t`（`SEND` / `ECHO`）。`echo` 不是 HAL 模式 ——
  控制器照样开 `NORMAL`，区别全在「谁先发」。回声在 `can_collect()` 里干，
  `can_tick()` 不再发起帧。
- 帧里新增 **`replied=`** —— `seq/rx/miss` 在 echo 模式下不是判据（和 `listen` 一样，
  什么都不发起，回路永远闭不上，`miss` 在健康总线上也会涨）。**可判的数是
  `replied=` 对 `rx_frames=`。**
- H4 新增四条断言：不主动发、一帧来一帧回、**同 ID 同长度**、**载荷加一而不是原样退回**。
  假板子加了 `test_can_inject()`（echo 不主动发，回环驱不动它）。

⚠️ **顺手修掉一个真风险**：`can_collect()` 原来是无界 `while`，而回声要在里面发帧 ——
凡是把发出去的帧当收到的帧递回来的场合（H4 的假板子、或者接成自环的总线），
就是一个超时都逃不出来的死循环。加了 `CAN_DRAIN_MAX 64`（FDCAN RX FIFO 深 64）。
这正是项目里那条教训：**紧循环不能只靠 `HAL_GetTick()` 退出，一律再加硬迭代上限。**

### ✅ `pt.run rs485.pins`（2026-09-13，验过）

把独立 RS485 测试的 **阶段 R1**（`../RS485/rs485_test.c`）搬成一个 run 目标：
PD4（/RE 和 DE 共一条网）和 PD5 当普通 GPIO 推 0/1 再读回。**什么都不用接。**

它分开的是「没接线」和「这根脚死了」—— 后者没有这项检查时，**看起来和前者一模一样**。

⚠️ **会话在跑时直接拒绝**（`checked=0 busy=1`）—— 会话把 PD4/PD5 复用成 USART2 的 AF，
在它下面重新复用成 GPIO 会把会话弄坏，而会话会接着报 miss、把责任推给那对线。
`pt.run` 按设计**不停会话**，所以只能由目标自己谢绝。为此给 `porttool.c` 加了
`PortTool_PortRunning()`。

H4 四条断言：占用时谢绝、健康时两脚跟随、**卡住的脚被点名**、**没卡住的那根不跟着遭殃**。
假 HAL 为此加了按引脚的输出锁存 + `test_gpio_stuck_low`（**一项只被看见过通过的检查什么都证明不了**）。

### ✅ `pt.run sdram.crc`（2026-09-13，验过）

只读不写，算一个窗口的 CRC32。配 STM32CubeProgrammer 的「Read & Write Memory」：
工具把文件写进数组，这个说实际落下去的是什么。

`offset=` / `bytes=` 由方案给（第 22 条），**回复里要说清楚算的是哪个窗口** ——
一个不带范围的 CRC 在 PC 上没法和任何东西比。超出映射的窗口**夹回来而不是拒绝** ——
这个器件读越界会静默回绕，那会算出一个「看着像答案」的数。

⚠️ **撞名了，已按旧约定改名**：交权那边已经有一个 `sdram.crc`，H4 有一条断言专盯这种碰撞
（第 17 条）。交权那个改成 **`sdram.crc.soak`**，和 `sd.integrity.soak` / `sdram.retention.soak` 一致。

### ✅ 第 4 步：`sdram` 变会话（2026-09-13）

`SDRAM_Test_RetentionCycle` 拆成 **Write / Verify 两半**（都不打印），
新的 `porttool_sdram.c` 在中间用自己的时钟等 —— **等待本身就是测试**（不刷新的单元
几十毫秒就掉电），也是唯一不能放进 tick 的部分。`wait` 下限 1000 ms，**拒绝而不是夹** ——
方案写 `wait=50` 是以为在测保持，默默给它 1000 等于让那个误解成立。

`sd.integrity` **没改成会话**：默认一轮正好是一个 4 KiB 块
（`SD_TEST_FILE_SIZE` = `SD_TEST_CHUNK` = 4096），「一轮 = 一口」本来就成立，
把 FatFs 改成可跨 tick 续跑是大改造，而「阻塞会拖累别的端口」这件事没有实测依据。
**这是一个可以推翻的取舍，推翻的条件是有板子且量到了真的拖累。**

### ✅ 第 5 / 6 步（2026-09-13）

14 项全改 `HANDOVER_NOT_IN_CAPS`；caps 从 **18 行变 16 行**（两行交权消失、
`sdram` 从 `kind=run` 变 `kind=session`）。跟着改的：H4 断言、三个 Go 测试、
`caps_golden.txt`、`PORTTOOL-CAPS-TEST.md` 那两个数、`TEST-CASES.md` 的 H5 判据 ⑥、
H5 的逐端口期望表、`station6-poweron.json`、以及 `porttool_plan` 里那个手写假板子。

### ⚠️ 这一轮抓出来的三件真事

1. **`can_collect()` 是无界 `while`，而回声要在里面发帧** —— 把发出去的帧递回来的场合
   （H4 假板子、自环的总线）就是死循环。加了 `CAN_DRAIN_MAX 64`。
2. **面板跟命令行发的命令不一样** —— CLI 给 `pt.run` 带上方案参数（`ptseq.go` 的
   `step.ParamArgs()`），**页面不带**。同一条方案步骤两边能得出不同结论 ——
   正好违反 `judge.go` 里写着的那条不变式。已修：`/api/portplan` 多返回 `runs`，
   页面的 `runOne()` 拼上参数。
3. **加了 `pt.run` 目标却没加判据，会把整个端口的结论抹掉** —— 页面对无判据的回复
   `delete verdicts[port]`，于是会话刚挣来的「通过」变成「未测」。**同一类坑第三次**
   （eth、sd、这次 sdram.crc）。

### ⚠️ 还没查完：H5 在日志窗那一段中断

逐端口扫描那一段**已经全过**（包括新的 `sdram`），但跑到「日志窗」那节的
**第二次 `#pause` 点击超时**，75 项之后停下（原本约 189 项）。

**已经排除的**：

- 不是 `redrawLog()` 卡主线程 —— 实测缓冲区 12000 行时它只要 **5–7 ms**
- 不是按钮本身坏了 —— 板子空闲时同一个双击序列 **150 ms 就完成**
- 不是会话没停 —— `pt.stop sdram` 后 `pt.list` 报 `running=none`

**第四个猜想也错了**：按钮并没有在动（坐标恒为 1090），计数恒为 0（没有帧在流）。

## ✅ 真因找到了：`redrawLog()` 重建时每行强制一次布局

关键线索是：**点击其实落下去了**（`paused` 真的翻了），只是页面之后回不到空闲。
拿**真实**日志缓冲区量一次：`redrawLog()` **21738 ms**（4679 条、渲染 4000 条）。

⚠️ **我之前量错过一次并据此排除了这条。** 那次往 `logBuf` 里塞的填充条目字段名是
`{t,k,s}`，而 `appendLine` 读的是 `kind`/`line`/`at` —— 全是 `undefined`，根本没走那条贵路径，
量出来 5–7 ms。**假数据试出来的「排除」不算排除。**

**机制**：`appendLine()` 每行都读 `scrollTop` / `clientHeight` / `scrollHeight` 又写 `scrollTop`。
在 DOM 插入之后读这些属性会**强制同步布局**，在一棵越长越大的树上做四千次，
就是教科书式的 layout thrashing。**流式一行一行来没问题**（它本来就是为那个写的），
整体重建才炸。

**修法**：抽出 `logNode(e)`，`redrawLog()` 先建进 `DocumentFragment`，最后一次性挂上去、
只动一次 `scrollTop`。**21738 ms → 52 ms。**

**为什么现在才冲出来**：扫描多了两个 `pt.run` 目标和一个会话，日志行数过了那个
让冻结时间超过 10 s 点击超时的坑。**它一直在那里，只是之前冻得不够久。**

**补了一条断言**（`run.py` 的日志窗那节）：恢复必须在 3 s 内完成，否则直接点名
`redrawLog()`。上限故意放得很松 —— 它是拦「又改回每行布局」，不是管毫秒。
原来那个点击超时**什么原因都不说**。

### ⚠️ 第 5 步还要回头改的

`$TOOL/TestCase/host/porttool_caps/PORTTOOL-CAPS-TEST.md` 第 96 / 135 / 140 行写着
**「14 个交权目标」和「18 行端口」**。交权全退出 caps 之后这两个数都会变
（18 行 → 16 行，`targets=` 并集变空）。**那份文档的数字没有任何自动检查盯着。**
