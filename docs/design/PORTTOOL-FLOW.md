# 端口测试工装 —— 控制模型与逐端口测试规程

**总分两部分。** [A 部分](#a-总--上位机怎么控制板子)讲**上位机和板子之间的完整契约**：控制什么、怎么控制、如何响应、响应什么 —— 只看这一部分就够写上位机。[B 部分](#b-分--每个端口怎么测)讲**每一个端口具体怎么测**：怎么接、敲什么、看到什么算过、哪些坑会让判据静默失效。[C 部分](#c-附--镜像开关物理连接用法)是环境与前提。

代码在 `TestCase/porttool/`（固件侧）和 `$TOOL` 的 `internal/pt*` + `cmd/porttool`（上位机侧）。形态与分发方式见 [C.4](#c4-上位机怎么分发怎么起来)，待办见 [C.5](#c5-还没做的--三期计划)。

> 本文描述固件 **`0.5.0`** 的现状。取舍理由在 [DECISIONS.md 第 9–15 条](DECISIONS.md)，还没做的在 [C.5](#c5-还没做的--三期计划)。
引脚事实以 [HARDWARE-FACTS.md](HARDWARE-FACTS.md) 为准，本文不重复推导，只引用结论。
专用镜像（一次烧一个用例）的接线细节在 [BOARD-BRINGUP-CASES.md](../test/BOARD-BRINGUP-CASES.md)，B 部分链过去，不抄。

> 图是 Mermaid，渲染成 SVG，可任意缩放。VSCode 装 Markdown Preview Mermaid Support，或直接在 GitHub 上看。

---
---

# A 总 —— 上位机怎么控制板子

## A.1 一张图：一条串口，两个平面

上位机和板子之间只有**一条 115200 8N1 的 RS232**。这条线上跑两个方向、三种行。

```mermaid
flowchart TB
    subgraph PC["上位机 PC"]
        UI["浏览器 UI<br/>面板 + 日志窗格"]
        GO["Go 工装<br/>解析 ! 帧，做判定"]
        UI <-->|"本地 HTTP"| GO
    end

    subgraph BOARD["板子 · PORTTOOL_ENABLE=1"]
        LOOP["PortTool_Run 超循环<br/>收命令 + 跑 tick"]
        SESS["会话表<br/>din · dout · relay · ain<br/>aout · temp · rs232 · rs485"]
        HAND["交权入口表<br/>14 个独占测试"]
        LOOP --> SESS
        LOOP --> HAND
    end

    HW(("端口硬件<br/>端子 / 引脚"))

    GO -->|"控制面：pt.* 命令行<br/>PC → 板"| LOOP
    LOOP -->|"应答面：OK / ERR<br/>板 → PC，一问一答"| GO
    SESS -->|"数据面：! 采样帧<br/>板 → PC，周期推送"| GO
    LOOP -.->|"裸 printf 日志<br/>给人看"| GO

    SESS <--> HW
    HAND <--> HW

    style LOOP fill:#e8f0fe,stroke:#4285f4,stroke-width:2px
    style SESS fill:#e8f0fe,stroke:#4285f4
    style HAND fill:#fef7e0,stroke:#f9ab00
```

**判定和序列全在上位机** —— 固件只报原始采样，一个字都不判。改判据不用重烧固件。

## A.2 控制什么 —— 三类控制对象

板子上能被控制的东西有三类，**行为完全不同，上位机必须分开对待**：

| | ✅ **会话** | 🎯 **一次性动作** | 🔶 **交权** |
|---|---|---|---|
| 是什么 | 一个可启停的周期采样器，`porttool_port_t` 结构（[porttool.h:52-77](../../TestCase/porttool/porttool.h)） | 一个跑完就返回、把量到的数写成一行 `OK` 的函数 | 一个独占的传统 bring-up 测试入口函数 |
| 现有几个 | **11 个**：`din`、`dout`、`relay`、`ain`、`aout`、`temp`、`rs232`、`rs485`、`can`、`knx`、`soak` | **8 个**：见 [A.3](#a3-怎么控制--全部-9-条命令) 的 `pt.run` 表 | **14 个**：见 [B.3](#b3-交权目标--14-个独占入口) |
| 怎么启动 | `pt.start <port> [k=v …]` | `pt.run <target>` | `pt.handover <target>` |
| 能并存吗 | **能**，多个会话同时跑，主循环轮流 tick | 跑的时候独占 CPU，跑完就还回来 | **不能**，进去就不出来 |
| 参数能热改吗 | **能**，`pt.set` 不重启会话 | **不收参数**，尺度编在固件里并写进应答 | 不能，参数编在固件里 |
| 怎么退出 | `pt.stop <port>` 或 `pt.stop all` | 自己就结束了 | **只能复位板子** |
| 输出什么 | `!<port> …` 结构化帧，机器可解析 | 散文若干行 + **一行 `OK <target> k=v …`** | 裸 printf，给人看 |
| 主机能判吗 | **能** | **能** —— 产线序列就是拿这一类搭的 | **不能**，散文解析不出结果 |
| 副作用 | `stop` 负责恢复（继电器全释放） | 自己按需初始化用到的外设 | 接管同一批外设，自己重新初始化 |

⚠️ **`pt.caps` 里三类各占自己的行**（`kind=session` / `kind=run` / `kind=handover`），但**一块硬件只占一行** —— 一次性动作和交权入口共用同一颗芯片时，交权那几个挂到 `kind=run` 那行的 `targets=` 上（`sd`、`sdram` 就是这样），见 [DECISIONS.md 第 17 条](DECISIONS.md)。

**惰性初始化**：外设只有在 `pt.start` 到达时才配置（各个会话自己的 `*_inited` 标志），没启动过的端口保持复位状态。

## A.3 怎么控制 —— 全部 9 条命令

**一行一条，`\r` 或 `\n` 结尾，不回显。**单行上限 192 字节（`PORTTOOL_LINE_MAX`），超长整行丢弃并回 `ERR line too long` —— 不截断成另一条含义的命令。空行忽略。

派发在 [porttool.c:191-233](../../TestCase/porttool/porttool.c)。

| 命令 | 参数 | 干什么 | 前置条件 | 不满足会怎样 |
|---|---|---|---|---|
| `pt.caps` | 无 | 报固件版本 + 每个会话端口的通道数与当前参数 | 无 | — |
| `pt.id` | 无 | 报 MCU 96 位 UID + 工装版本 | 无 | — |
| `pt.list` | 无 | 报哪些会话正在跑 | 无 | — |
| `pt.start` | `<port> [k=v …]` | 启动一个会话 | 端口名存在；**所有给出的参数都合法** | `ERR no such port` / `ERR <port> start refused: <原因>`，**会话不启动** |
| `pt.set` | `<port> k=v …` | 改一个**正在跑**的会话的参数 | 端口名存在 **且正在跑**；参数合法 | `ERR no such port` / `ERR <port> is not running` / `ERR <port> set refused: <原因>` |
| `pt.stop` | `<port>` 或 `all` | 停会话并恢复副作用 | 端口名存在（`all` 总是合法） | `ERR no such port` |
| `pt.handover` | `<target>`，**或不带参数** | 不带参数 = 列出全部交权目标；带参数 = **先停掉所有会话**再进去，不返回 | 目标名存在 | `ERR no such handover target "<name>" - run pt.handover with no argument to list them` |
| `pt.echo` | `<port> <数>` | **把板子刚发的那个数原样送回去**，板子在此基础上累加。这是需求 R4 的回环 | 端口存在、**正在跑**、且 `loop=ctrl` | `ERR no such port` / `ERR <port> is not running` / `ERR <port> takes its echo on its own link, not on this one` |
| `pt.run` | `<target>`，**或不带参数** | 不带参数 = 列出全部一次性动作；带参数 = 跑一次、**回到命令循环**、用一行 `OK <target> k=v …` 报出量到的数 | 目标名存在 | `ERR no such run target "<name>" - run pt.run with no argument to list them` |

**`pt.run` 现有八个目标**（`TestCase/porttool/porttool_run.c` 里的表），按硬件归成四组：

| 目标 | 报什么 | 注意 |
|---|---|---|
| `sdram.probe` | `base` `size` `ready` `databus` `addrbus` | 自己按需 `MX_FMC_Init()` —— 工装镜像跑在 Phase 1，那里 FMC 还没起来 |
| `sdram.sweep` | `ready` `patterns` `words_each` `mismatches` `first_bad` `bad_pattern` `write_ms` `verify_ms` | **整片 64 MiB 四种花样**，产线要的那个「压力测试零错误」。⚠️ 几十秒，方案里的 `timeout_ms` 要留够 |
| `sdram.retention` | `ready` `checked` `failed` `wait_ms` `first_bad` `seed` | **写 64 个随机地址、等 5 秒、读回**。证明自动刷新真的在跑 —— 过了全片扫描的板子照样可能栽在这一项 |
| `sd.probe` | `detected` `ready` `blocks` `block_size` `mib` `v2x` `class` | `detected=0` 是没插卡，`detected=1 ready=0` 是接口 |
| `sd.integrity` | `mounted` `wrote` `read_back` `identical` `bytes` 两个 CRC `fresult` | 一轮 4 KiB |
| `sd.stress` | `mounted` `passes` `passed` `bytes_each` `bytes_total` `elapsed_ms` `first_bad_pass` `fresult` | **64 轮**。⚠️ 判据要**同时**看 `passes` 和 `passed` —— 中途停下来时 `passes` 跟着变小，只看 `passed` 会把「跑一轮就放弃」判成通过 |
| `rtc.read` | `init` `clk` `date` `time` | 自己按需 `MX_RTC_Init()`。**只读不写** —— 备份域里住着 iap_auth 的 nonce 计数器 |
| `led.blink` | `pin` `pulses` `half_ms` `observed=unknown` | PE2 闪六次。**没有回读，`observed` 永远是 unknown**，看没看见是人的判断 |

> ⚠️ **`sd.integrity` 这个名字在 `pt.run` 和 `pt.handover` 里都出现过。** 2026-09-08 把交权那个改名成 `sd.integrity.soak`（`sdram.retention` 同理改成 `sdram.retention.soak`），因为一个名字同时对应「跑完回来」和「一去不回」两种行为，caps 行上摆在一起谁也分不清。

> ⚠️ **`pt.run` 和 `pt.handover` 的区别只有一条，但是全部**：`pt.run` 回来，`pt.handover` 不回来。所以只有 `pt.run` 的结果**主机能判**（[DECISIONS.md 第 22 条](DECISIONS.md)：判定一律在上位机）。
>
> ⚠️ **`pt.run` 的应答里是量到的数，不是结论。** 没有任何目标打 PASS 或 FAIL。限值住在上位机的方案文件里，这才是「改一个限值不用重烧板子」成立的原因。
>
> ⚠️ **它执行的检查会先打自己的散文**（例如 `SDRAM_TEST: data bus OK …`），**然后**才是那一行 `OK`。上位机必须按「裸日志行不结束应答」来读 —— 这正是 `ptproto` 的四类行分流本来就做的事。⚠️ 也因此 `pt.run` **不停掉正在跑的会话**（`pt.handover` 会），中间可能夹着 `!` 采样帧。
>
> ⚠️ **`pt.echo` 对 `loop=link` 的端口是被拒绝的**，这是刻意的。那些端口从**被测链路**上取回复；在控制口上答它，会让计数器在那条链路已经死掉的情况下照样往上涨 —— 那正是这套工装不许产生的假通过。取舍见 [DECISIONS.md 第 9 条](DECISIONS.md)。

> ⚠️ **`pt.handover` 的名字校验发生在停掉所有会话之后**（[porttool.c:172-186](../../TestCase/porttool/porttool.c)）：目标名敲错，**会话已经全停了**，然后才回 ERR。上位机不能假设「交权失败 = 什么都没变」，收到这条 ERR 之后必须重新 `pt.caps` 或 `pt.list` 同步状态。

### 参数语法

`k=v`，空格分隔，值可以用双引号包住（引号内的 `=` 不会被当成下一个参数）。解析器在 [porttool_cmd.c](../../TestCase/porttool/porttool_cmd.c)。三种值类型：

| 类型 | 写法 | 合法范围 | 例 |
|---|---|---|---|
| 通道掩码 | 逗号分隔的十进制 | `1..<channels>`，**不能为空集** | `ch=1,3,5` |
| 无符号整数 | 纯十进制数字 | 见各端口 | `period=200` |
| 枚举 | 裸字符串 | 见各端口 | `mode=square` |
| **逐路取值** | `<路>:<值>` 逗号分隔 | 路 `1..<channels>`，值见各端口，**同一路不能出现两次** | `duty=1:20,5:75` |

**逐路取值的参数同时接受单值写法**（`duty=100` = 所有选中路都设成 100），因为命令行和文档里原本就是那么写的。解析器是共用的 `PortCmd_GetPairs()`。

⚠️ **拒绝就是「什么都没变」。**参数解析先写进临时变量，整行都合法才提交 —— 所以一条在第三个参数上被拒的命令，前两个参数也没有生效。不这样的话「命令被拒」和「状态没动」就不是同一件事，而面板上看不出差别。

**没给的参数保持上次的值**，给了但写错**拒绝整条命令** —— 见 [A.6 铁律 1](#a6-三条铁律)。

## A.4 如何响应 —— 四类行，看第一个字符就能分

**串口上回来的每一行都属于且只属于以下四类之一。**上位机按第一个字符分流，不需要状态机。

| 行首 | 类别 | 何时出现 | 谁消费 |
|---|---|---|---|
| `OK ` | 成功应答 | 每条命令之后，**可能多行**（`pt.caps` / `pt.list` / `pt.handover` 无参） | 上位机，配对到刚发的命令 |
| `ERR ` | 失败应答 | 命令被拒 | 上位机，**原文显示给人**，不要改写 |
| `!` | 采样帧 | 会话周期到点，**与命令无关地异步冒出来** | 上位机解析，进面板 |
| 其他 | 裸日志 | 开机 banner、交权测试的 printf | 人看，进日志窗格 |

### 每条命令的确切应答

照 [porttool.c](../../TestCase/porttool/porttool.c) 的 `printf` 抄的，格式串不要自己猜。

```text
pt.caps
  OK porttool=0.5.0 ports=17 lines=45
  OK port=din kind=session blk=D term=D02-D09 channels=8 loop=ctrl params=ch,period running=0
  OK vals=din ch=1,2,3,4,5,6,7,8 period=200
  OK limits=din ch:1..8 period:50..
  OK port=relay kind=session blk=B term=B01-B12 channels=6 loop=ctrl params=ch,mode,on,period running=0
  OK vals=relay ch=1,2,3,4,5,6 mode=hold on=1:0,2:0,3:0,4:0,5:0,6:0 period=2000
  OK terms=relay B01+B02,B03+B04,B05+B06,B07+B08,B09+B10,B11+B12
  OK port=can kind=session blk=C term=C07,C08 channels=1 loop=link params=baud,mode,period running=0 targets=can,can.soak,can.scope,can.echo
  OK port=bringup kind=handover blk=- term=- channels=1 loop=none targets=bringup
  OK port=sdram kind=run blk=- term=U6 channels=1 loop=none runs=sdram.probe,sdram.sweep,sdram.retention targets=sdram.capacity,sdram.retention.soak,sdram.crc
  OK port=led kind=run blk=- term=- channels=1 loop=none runs=led.blink
  …
    ^ 表头声明 lines=<n>，后面**恰好**跟 n 行。**行数不等于 1 + ports** ——
      一个端口可能占 2~3 行（port= 形状、vals= 当前取值、terms= 端子标签），
      而以后还会有更多。**上位机读固定行数，不要靠「读到没有了」判断结束** ——
      这条链上没有结束标记，而正在跑的会话会把 ! 帧插到回复中间。

  为什么形状和取值分两行：**一个逐路参数就塞不进 192 字节**。dout 光
  `duty=1:0,2:0,…,8:0` 就占 31 个字符，合成一行是 195 字节 —— 超了。
  超了会被截断，而**被截断的 caps 行就是面板永远不会渲染的那个参数**。
  这条是 2026-09-07 由用例 H4 的行长检查当场抓出来的。

pt.id
  OK uid=00340041XXXXXXXXXXXXXXXX porttool=0.5.0

pt.list
  OK running=din          <- 每个在跑的一行
  OK running=relay
  OK running=none         <- 一个都没跑时的唯一一行

pt.start din ch=1,3,5 period=200
  OK started din
  ERR no such port "dinn"
  ERR din start refused: ch="1,9" must be channels 1..8 separated by commas
  ERR din start refused: period="abc" is not a number

pt.set din ch=2,4
  OK set din
  ERR din is not running

pt.stop din  /  pt.stop all
  OK stopped din
  OK stopped all

pt.handover                 <- 不带参数 = 目录
  OK handover=bringup          DIN, relays, analog in/out and temperature together, with a key menu
  OK handover=can              five phases: report, internal loopback, external loopback, listen, normal
  …共 14 行

pt.handover can
  OK handing over to can - this does not come back, reset the board to return to the port tool
  （之后是那个测试自己的裸 printf，再也不回命令循环）

（未知命令）
  ERR unknown command "pt.foo"
```

> **`pt.handover pwm` 是唯一可能回来的**：`PWM_Test_Run()` 在定时器起不来时会 return，这时打 `OK pwm returned - back at the port tool` 然后回到命令循环（[porttool_handover.c:69-73](../../TestCase/porttool/porttool_handover.c)）。其余 13 个都不返回。

## A.5 响应什么 —— 采样帧的字段

```text
!<port> t=<ms> <字段…>
```

`t` 是 `HAL_GetTick()`，板子开机以来的毫秒数，**会在 49.7 天回绕**。生成在 [porttool.c:39-49](../../TestCase/porttool/porttool.c)，整帧上限 192 字节。

### `!din`

```text
!din t=48213 seq=3 rx=2 miss=0 v=0x16 ch1=0 ch3=1 ch5=1
```

| 字段 | 含义 |
|---|---|
| `v` | **八位全量位域**，端子序 bit0 = DI1 … bit7 = DI8。**无论选了哪几路都是全的** |
| `ch<n>` | 只有 `ch=` 选中的那几路，值 0/1，和 `v` 对应位一致 |

### `!relay`

```text
!relay t=48213 seq=3 rx=2 miss=0 mode=square ch1=1 ch2=0
```

| 字段 | 含义 |
|---|---|
| `mode` | `hold` 或 `square` |
| `level` | 这一拍**命令的**电平。`square` 模式下每帧翻转一次 |
| `ch<n>` | 选中的那几路，**每路自己的电平**（`on=1:1,2:0,3:1` 设，和 dout 的 `duty=` 共用 `ch:值` 解析器）。`mode=square` 下每半周期每路各自翻转，所以起始时电平不同的一组会保持那个花样来回翻 |

> ⚠️ **没有单独的 `level=` 字段。**六路各自独立，一个数只在它们恰好一致时才是对的，其余时候是静默的错。这是 0.2.0 相对 0.1.0 的破坏性改动（[DECISIONS.md 第 12 条](DECISIONS.md)）。

> ⚠️ **`!relay` 报的是「驱动成什么」，不是「触点真的动了没有」** —— 继电器是 HF41F 干接点，板上没有回读。要证明触点动作必须靠外部回路或人耳，见 [B.2](#b2-relay--继电器输出会话-)。

### 每一帧都带的回环三个字段

```text
!<port> t=<ms> seq=<n> rx=<n> miss=<n> <该端口自己的字段…>
```

| 字段 | 含义 | 上位机拿它干什么 |
|---|---|---|
| `seq` | 本帧发出的数 | 显示计数 |
| `rx` | 板子最近一次**真正收回来**的数 | `seq - rx` 恒为 1 就是闭环正常 |
| `miss` | 连续没等到有效回复的拍数 | 一直涨 = 回程断了 |

**上位机每收到一帧就回一次 `pt.echo <port> <seq>`**（`loop=ctrl` 的端口），板子在收回来的数上加一继续。

⚠️ **三条容易读错的**：

1. **没回复时 `seq` 不动。**只有 `miss` 涨。一个还在往上爬的 `seq` 会让断掉的回环看起来是活的，所以宁可让它停住。
2. **过期回复算 miss，不算闭合。**收回来的数和当前 `seq` 不相等，说明它回答的是更早那一帧。
3. **第一帧的 `miss` 是 0。**那一拍上位机还没机会回复，判它 miss 会让每个健康的回环都从 1 起步。

⚠️ **`loop=ctrl` 的端口，这三个数不是该端口的判据。**它们只说明控制口和循环还活着。DI 的判据永远是 `v=` 位域，AOUT 的判据永远是端子上的电流。面板必须把它们和读数分区显示（[DECISIONS.md 第 9 条](DECISIONS.md)）。

### `!dout`

```text
!dout t=51200 seq=3 rx=2 miss=0 mode=hold freq=1000 ch1=20 ch5=75
```

| 字段 | 含义 |
|---|---|
| `mode` | `hold` 或 `blink` |
| `freq` | 定时器**实际落在**的频率，不是请求的那个（预分频是整数） |
| `ch<n>` | 选中那几路**此刻正在驱动的占空比**，百分比。`blink` 的暗半周期里这里是 0，不是设定值 —— 帧要说的是引脚上现在是什么 |

### `!ain`

```text
!ain t=48213 seq=1 rx=0 miss=0 vdda=3287 ok=1 ch1=21850/2071 ch2=530/50
```

| 字段 | 含义 |
|---|---|
| `vdda` | 用 VREFINT 量出来的 VDDA，毫伏 |
| `ok` | **1 = VDDA 量到了且落在合理带内**。`ok=0` 时这一帧所有读数都没有意义 |
| `ch<n>` | `原始码/引脚毫伏`。**是 MCU 引脚上的值，不是端子上的值** |

⚠️ **端子换算不在固件里算**，因为它取决于板子焊成了哪个档位 —— 那是一次性的焊接选择，固件读不回来。上位机做换算，并且要说清自己假设的是哪个档。

⚠️ **`ok=0` 时读数照样报。**藏起来的话就没有东西可诊断了 —— 而板上没有基准芯片，VREFBUF 没使能时 ADC 会返回 `0x8000` 这类**看起来完全像真数据**的值。所以判据是 `ok`，不是读数本身。

### `!aout`

```text
!aout t=48213 seq=1 rx=0 miss=0 ch1=500/488/4883 ch2=1500/1465/14648
```

| 字段 | 含义 |
|---|---|
| `ch<n>` | `请求值/量化后的值/预期微安`。三个都报：只报请求会藏掉几毫伏的量化误差，只报量化值会让面板看起来没理会输入 |

⚠️ **微安那个数是「应该是多少」，不是测量值** —— 板上没有任何通道能读回那个电流。**万用表串在回路里才是唯一的判据**，这个端口存在的意义就在这里。

⚠️ **它只在跳线 JP3/JP4 开路时成立。**闭合的话 VREF 经 40k2 灌进 XTR111 的求和节点，输出变成 4–20 mA live-zero（DAC 设 0 时已有约 4.85 mA）。那也是固件读不回来的焊接选择，所以这个数假设开路。

### `!temp`

```text
!temp t=48213 seq=1 rx=0 miss=0 vdda=3287 ok=1 ch1=738/238 ch2=751/251
```

| 字段 | 含义 |
|---|---|
| `ch<n>` | `毫伏/十分之一摄氏度`。用十分之一度而不是整度，是为了让缓慢漂移看得见，同时不让浮点数上串口 |

板上两个 LM50：ch1 在短路保护上，ch2 在高侧开关上。**它们不是端子**，接不了外部东西 —— 值得看的是 DO 驱动带载发热，所以它是独立会话而不是 dout 上的一个字段：正好在别的端口被大力驱动时最该看它。

## A.6 三条铁律

从现有代码那条「不许假通过」来的，上位机必须跟着守：

1. **参数给了但写错，拒绝启动，并把原因说清楚** —— 不静默沿用旧值。沿用了就是在读别的引脚，而面板上看不出来（[porttool_din.c:30-32](../../TestCase/porttool/porttool_din.c) 的注释就是这条）。上位机**必须原样显示 ERR 文本**，不要归纳成「启动失败」。
2. **`!` 帧永远带完整位域** —— 一条采样自己就说得清来自哪个脚，不依赖上位机记得当初选了什么。
3. **交权前先停所有会话** —— 被交权的入口会重新初始化同一批外设，而没人看着的继电器不能留在吸合状态。

## A.7 一次完整会话的时序

```mermaid
sequenceDiagram
    autonumber
    participant U as 操作者
    participant PC as 上位机 (Go)
    participant FW as 固件 (PortTool)
    participant HW as 端口硬件

    Note over FW: 上电，打 banner，进命令循环
    FW-->>PC: === port tool 0.1.0 ===

    rect rgb(232, 240, 254)
    Note over PC,FW: ① 握手：先问能力，再渲染界面
    U->>PC: 打开页面
    PC->>FW: pt.caps
    FW-->>PC: OK porttool=0.1.0 ports=2
    FW-->>PC: OK port=din running=0 channels=8 …
    FW-->>PC: OK port=relay running=0 channels=6 …
    PC->>FW: pt.id
    FW-->>PC: OK uid=… porttool=0.1.0
    Note over PC: 按 caps 渲染卡片<br/>固件没报的端口不显示
    end

    rect rgb(230, 244, 234)
    Note over PC,FW: ② 会话：启动 → 周期推送 → 热改参数
    U->>PC: 勾 DI1/DI3/DI5，周期 200ms，点启动
    PC->>FW: pt.start din ch=1,3,5 period=200
    alt 参数合法
        FW->>HW: PortDin_Init 配八路为高阻输入
        FW-->>PC: OK started din
        loop 每 period 一次，直到停止
            FW->>HW: PortDin_ReadBits
            HW-->>FW: 八位电平
            FW-->>PC: !din t=48213 seq=3 rx=2 miss=0 v=0x16 ch1=0 ch3=1 ch5=1
            PC->>PC: 更新面板：电平、保持时长、翻转次数
        end
    else 参数写错
        FW-->>PC: ERR din start refused: ch="1,9" must be channels 1..8 …
        Note over PC: 不启动，原因原样显示<br/>绝不静默沿用上次的通道集
    end

    U->>PC: 改成 DI2/DI4
    PC->>FW: pt.set din ch=2,4
    FW-->>PC: OK set din
    Note over FW: 会话不重启，下一帧就是新通道
    end

    rect rgb(254, 247, 224)
    Note over PC,FW: ③ 交权：单向门
    U->>PC: 需要 CAN 五相位深度诊断
    PC->>FW: pt.handover can
    FW->>FW: 先停掉所有会话，释放继电器
    FW-->>PC: OK handing over to can - this does not come back …
    Note over FW,HW: 进入 CAN_Test_Run()<br/>只有复位才能回到工装
    end

    Note over U,HW: —— 或者正常收尾 ——
    U->>PC: 结束
    PC->>FW: pt.stop all
    FW->>HW: 继电器全部释放
    FW-->>PC: OK stopped all
```

## A.8 固件主循环

```mermaid
stateDiagram-v2
    [*] --> Banner
    Banner --> Idle : 打印命令提示

    Idle --> Idle : 轮询所有 running 会话的 tick()
    Idle --> Parse : UART4 收到一整行

    Parse --> Reply : pt.caps · pt.id · pt.list
    Parse --> Start : pt.start
    Parse --> Set : pt.set
    Parse --> Stop : pt.stop
    Parse --> Hand : pt.handover
    Parse --> Reply : 其他 · ERR unknown command

    Start --> Refuse : 参数非法
    Start --> Running : 惰性初始化外设
    Refuse --> Idle : ERR start refused 带原因
    Running --> Idle : OK started

    Set --> Idle : OK set 或 ERR
    Stop --> Idle : 释放执行器 · OK stopped
    Reply --> Idle

    Hand --> Handover : 无论目标名对不对，先停掉所有会话
    Handover --> Idle : 目标名不存在 · ERR
    Handover --> [*] : 进入独占入口，不返回
```

**采样周期下限 50 ms**（`PORTTOOL_PERIOD_MIN_MS`）：115200 ≈ 11.5 KB/s，而 `printf` 是阻塞的，周期太小会把主循环拖死。小于 50 的请求被**静默夹到 50**，不报错 —— 上位机想知道实际周期就读 `pt.caps` 回来的 `period=`。

---
---

# B 分 —— 每个端口怎么测

图例：**✅ 会话**（`pt.start`，可并存可启停） · **🔶 交权**（`pt.handover`，独占且不返回） · **⬜ 未实现**

**通用前置**（每一项都成立，下面不重复）：`PORTTOOL_ENABLE=1` 的镜像，ST-Link 烧进去，USB-RS232 适配器接端子 **C05(TxD) / C06(RxD) / C02(GND)**，115200 8N1。见 [C.2](#c2-物理连接一条串口两种行)。

## B.1 `din` —— 数字输入（会话 ✅）

Klemmblock D，Upper Deck。代码 [porttool_din.c](../../TestCase/porttool/porttool_din.c) + [port_din.c](../../TestCase/common/port_din.c)。

| | |
|---|---|
| **测什么** | 八路数字输入的**电平**、**保持时长**、**翻转次数**。前级是 LM339 比较器 + 分压，耐 24 V 现场电平 |
| **端子 / 引脚** | D02–D09 → `PC6 PB5 PB6 PB7 PH10 PH11 PI5 PI6`（bit0 = DI1 起算） |
| **怎么接** | 要么不接（验高阻输入本身），要么把 24 V 现场信号接上。也可以**从 DO 引一根 8 芯线过来**（DO 输出 24 V，DI 耐 24 V，一根线覆盖 16 个通道）。完整接线见 [BOARD-BRINGUP-CASES.md](../test/BOARD-BRINGUP-CASES.md) 的 Digital In 行 |
| **敲什么** | `pt.start din ch=1,3,5 period=200`<br/>参数：`ch=` 1..8 逗号分隔（默认全 8 路）、`period=` 毫秒（默认 200，下限 50）<br/>热改：`pt.set din ch=2,4`　停：`pt.stop din` |
| **看到什么算过** | `!din` 帧的 `v=` 位域跟着外部驱动变；DI\<n\> 拉高时对应 bit 为 1，拉低为 0，**和实际驱动逐位一致** |
| **坑** | ⚠️ MCU 侧有外部 10k 上拉，固件配 `GPIO_NOPULL`，**不开内部上拉**。<br/>⚠️ **「恒读 0」和「恒读 1」是两个不同故障**（[HARDWARE-FACTS.md:152](HARDWARE-FACTS.md)）：恒 0 = 有 24 V 但那一路没接线；恒 1 = **整个 24 V / 模拟供电没上来**。上位机必须分开报，不能都说成「低电平」 |

**同一组引脚上还没做的**：编码器 1–4（`TIM3 / TIM4 / TIM5 / TIM8` 的 CH1+CH2，正交计数与方向）⬜

## B.2 `relay` —— 继电器输出（会话 ✅）

Klemmblock B，Lower Deck。代码 [porttool_relay.c](../../TestCase/porttool/porttool_relay.c) + [relay.c](../../Core/Src/relay.c)。

| | |
|---|---|
| **测什么** | 六路继电器的**吸合 / 释放**，以及**方波连续翻转**（老化、听咔哒声、示波器看触点） |
| **端子 / 引脚** | B01–B12 → `PI8 PI10 PI11 PG7 PG3 PD3`（HF41F 干接点） |
| **怎么接** | 判「驱动到了」：量 MCU 引脚或 Lower Deck T2–T7 的栅极。<br/>判「触点真的动了」：某一路触点串电压源 + 限流电阻，探头量电阻两端。详见 [BOARD-BRINGUP-CASES.md](../test/BOARD-BRINGUP-CASES.md) 的继电器行 |
| **敲什么** | 保持某个电平：`pt.start relay ch=1,2 mode=hold on=1`<br/>连续方波：`pt.start relay ch=1,2,3,4,5,6 mode=square period=2000`<br/>参数：`ch=` 1..6（默认全 6 路）、`mode=hold` 或 `square`（默认 hold）、`on=0` 或 `1`（hold 时的电平，默认 0）、`period=` 方波**半周期**毫秒（默认 2000，下限 50） |
| **看到什么算过** | `mode=square` 下每 `period` 听到一次咔哒 + 收到一帧 `!relay`，`level` 在 0/1 之间交替；串了负载的那一路，探头上电压跟着翻 |
| **坑** | ⚠️ **`!relay` 只报驱动意图，不报触点状态** —— 板上没有回读通道。<br/>⚠️ **`pt.start` 会先把六路全部释放再驱动选中的**（`relay_release_all()` 后 `relay_drive()`），所以重启会话 = 未选中的路必定回到释放态。<br/>⚠️ 自动判通断需要把电送过触点再被某个输入检测到，但 **DI 只有 8 路、DO 有 14 路**，占不过来。第一期靠人听咔哒 |

## B.3 交权目标 —— 14 个独占入口

**每一个都是 `pt.handover <target>`，进去不返回，要复位才能回工装。**输出是裸 printf，不是 `!` 帧。目标表在 [porttool_handover.c:51-72](../../TestCase/porttool/porttool_handover.c#L51-L72)。

⚠️ **14 个 target 不等于面板上 14 行。** 2026-09-08 之后 `pt.caps` 里只有 **2 行** `kind=handover`（`bringup` `pwm`）：`can` `knx` `rs485` 挂在自己的会话行上、**`sd` `sdram` 挂在自己的 `kind=run` 行上**（都是 `HANDOVER_ON_PORT_ROW`），`rs232` 刻意不进 caps（`HANDOVER_NOT_IN_CAPS`，见 [DECISIONS.md 第 17 条](DECISIONS.md)）。`pt.handover` 无参列表仍然列全 14 个。

### B.3.1 `rs485` —— RS485（Klemmblock C）

| | |
|---|---|
| **测什么** | 三件事：**引脚级**推 0/1 读回、**发**周期帧、**收**并回显 |
| **端子 / 引脚** | C10 / C11 (A/B) → `PD5 TX · PD6 RX · PD4 DIR` |
| **怎么接** | USB-RS485 适配器 A 接端子 **A10**（Upper Deck J11-3），B 接 **A11**（J11-2）。**必须有第二台设备**，脚本 `$TOOL/TestCase/tools/rs485_echo.py` |
| **看到什么算过** | ① `PD4` / `PD5` 当 GPIO 推 0/1 读回一致；② 适配器每 **3 s** 收到一帧 `RS485 HELLO <n>`；③ 主机发的探针原样回来，日志口同时打 ASCII + hex 两列 |
| **坑** | ⚠️ **半双工，`PD4` 同时驱动 /RE 和 DE** —— 发送时接收器是关的，**板子听不到自己**，没有对端就永远收不到东西 |

### B.3.2 `can` / `can.soak` / `can.scope` / `can.echo` —— CAN（Klemmblock C）

| | |
|---|---|
| **测什么** | `can` 走五个相位，逐步缩小故障范围；三个变体是长跑 / 示波器 / 回声 |
| **端子 / 引脚** | C07 / C08 (L/H) → `PB9 TX · PI9 RX`，`CAN_GND` = A09（J11-4）。500 kbit/s，Classic CAN，标准帧 |
| **怎么接** | P0/P1 不接；P2 外部回环**自带 120 Ω**；P3/P4 接第二个节点。CAN H = C08（J10-1），CAN L = C07（J10-2） |
| **看到什么算过** | **P0** 打印外设与配置寄存器 → **P1** 内部回环收到自己发的帧（控制器活着）→ **P2** 外部回环同样收到（收发器和引脚活着）→ **P3** 监听模式收到对端的帧 → **P4** 正常模式与第二节点双向收发成功 |
| **变体** | `can.soak` 正常模式跑到复位（浸泡）· `can.scope` 方波 + 背靠背帧给示波器 · `can.echo` 收到什么就 +1 回发，配 `tools/can_send.py` |
| **坑** | ⚠️ **整板未端接** —— R69 串在 JP7 上，出厂开路，外部回环必须自带 120 Ω。<br/>⚠️ 收发器 ISO1044 是**隔离型**，隔离侧电源在板子底面，那边没电就全程 P1 过、P2 挂 |

### B.3.3 `rs232` —— RS232（Klemmblock C）

| | |
|---|---|
| **测什么** | 每个字节原样回显 —— 发送通不通、接收通不通 |
| **端子 / 引脚** | C05 TXD / C06 RXD → `PC10 · PC11`，`PB10` = MAX3221 EN |
| **怎么接** | 就是工装自己那条线，GND 接 C02 / C11 / C12 |
| **看到什么算过** | 开机看得到打印 = 发送通；敲键每个字符原样回显 = 接收通 |
| **坑** | ⚠️ **`pt.handover rs232` 进去就没有命令循环了**，只能复位才能回工装 —— 所以上位机面板上**不给这个按钮**。<br/>⚠️ **端子是真 ±12V，必须用 USB-RS232 适配器，接 TTL 适配器可能烧掉**（[HARDWARE-FACTS.md:17](HARDWARE-FACTS.md)）。<br/>⚠️ `PB10` 拉低 = MAX3221 整片关断，printf 一个字节都出不来 |

> ⚠️ **这里曾经写着「工装占用的就是这条通道，所以会话模式测不了 RS232 端子本身」——那句话是错的**，2026-09-07 更正（[DECISIONS.md 第 13 条](DECISIONS.md)）。
>
> 命令字节和应答字节本来就物理穿过 C05/C06、MAX3221、PC10/PC11。所以 0.2.0 会给 rs232 开一个 `loop=link` 会话（周期发帧，上位机回 `pt.echo rs232 <n>`，板子累加）——**走的就是被测链路本身**，和 rs485 是同一个机制，只是这条链路碰巧也承载命令。上面这个交权目标仍然保留给命令行用户，**刻意不出现在 `pt.caps` 里** —— 面板上放它等于放一个会打死面板的按钮。会话见 [B.7](#b7-rs232--自动发自动收会话-)。

### B.3.4 `knx` —— KNX TP1（Klemmblock C）

| | |
|---|---|
| **测什么** | 总线安静后整串打印，**raw 与取反两种读法各解析一遍** |
| **端子 / 引脚** | C03 / C04 → `PB14 TX · PA10 RX · PD7 OK · PH12 VCC_OK · PG11 Prog_LED` |
| **怎么接** | KNX 总线接 Upper Deck 的 KNX 端子 |
| **看到什么算过** | **收**：总线安静 `KNX_RX_FLUSH_MS` 后整串打印，raw 和 bit-inverted 两种读法都按 KNX 服务解析。**发**：`KNX_TX_ENABLE` 置 1，发出的脉冲经收发器回到 RX 被自己收到 |
| **报文层** | `mode=frames` 组一条真的 L_Data GroupValueWrite 整帧发出（`ga=` / `src=` / `val=` 从上位机设），收发各一行事件帧 `!knx.rx` / `!knx.tx`。每一帧同时给 `raw=` 和 `inv=` 两种读法，`crc=` 说哪种通过校验字节（`raw` / `inv` / `ack` / `bad`）—— 极性由校验字节裁决，不用人看，见 [DECISIONS.md 第 32 条](DECISIONS.md)。⚠️ 默认发到 **31/7/255**，`ga=none` 停发 |
| **坑** | ⚠️ **`PG9`（Prog_KEY）和 `BOOT0` 是同一条网络** —— 误置会改变下次复位的启动模式。<br/>⚠️ STKNX 是**裸 TP1 收发器**，104 µs 的位时序由 MCU 自己产生，时钟不对就整串是垃圾 |

### B.3.5 `pwm` —— Digital Out 6 的 PWM（Klemmblock A）

| | |
|---|---|
| **测什么** | 1 kHz 呼吸灯，验证 TIM1_CH2 这条定时器通道活着 |
| **端子 / 引脚** | A08 → `PA9 = TIM1_CH2` |
| **怎么接** | LED + 限流电阻接 Digital Out 6 |
| **看到什么算过** | 占空比 0% → 100% → 0% 缓慢来回，LED 呼吸式亮灭 |
| **坑** | ⚠️ **这是 14 个交权目标里唯一可能返回的** —— 定时器起不来时它 return，工装打 `OK pwm returned` 并回到命令循环 |

### B.3.6 `sd.info` / `sd.integrity.soak` —— microSD（Bridge 板）

⚠️ 同上：产线那条路是 `pt.run sd.probe` / `sd.integrity` / `sd.stress`。

| | |
|---|---|
| **测什么** | `sd.info` 查卡；`sd.integrity.soak` 端到端读写比对，一直循环 |
| **引脚** | `PC12 CLK · PD2 CMD · PC8 D0 · PE6 CD` |
| **怎么接** | microSD 插进 Bridge 板 J6 |
| **看到什么算过** | `sd.info`：打印卡类型、容量、块大小、速度等级，**拔插卡时 2 秒内跟着变**。<br/>`sd.integrity`：FatFs 写 4 KiB 的 `0:/PLCTEST.BIN` 再读回，**逐字节相同且 CRC32 一致** |
| **坑** | ⚠️ `PE6` 卡检测**低 = 已插入**，极性反了会一直报「没卡」 |

### B.3.7 `sdram.capacity` / `sdram.retention.soak` / `sdram.crc` —— SDRAM（Bridge 板）

⚠️ **这三个是「一去不回」那条路。** 产线要的机器可读结果走 `pt.run sdram.probe` / `sdram.sweep` / `sdram.retention`（见 [A.3](#a3-怎么控制--全部-9-条命令) 的表）。留着交权这三个是因为它们打的散文对排查有用，caps 里挂在 `sdram` 那行 `kind=run` 的 `targets=` 上。

| | |
|---|---|
| **测什么** | 三个角度：地址空间对不对、长时间稳不稳、和 PC 侧算的一不一致 |
| **位置** | 板内 U6 = AS4C32M16SB-7BIN，64 MiB，FMC 映射在 `0xC0000000` |
| **怎么接** | 不接 |
| **看到什么算过** | `capacity` 容量与地址回绕正确（写高地址不会绕回低地址）· `retention` 长时间反复写读不出错 · `crc` 用 STM32CubeProgrammer 写进去的字节，**板子算出的 CRC32 与 PC 侧一致** |

### B.3.8 `bringup` —— 五项同时跑

| | |
|---|---|
| **测什么** | Digital In + 继电器 + 模拟输入 + 模拟输出 + 板载温度，五项非阻塞 tick 同时跑 |
| **怎么接** | 五项各自的接线，见 [BOARD-BRINGUP-CASES.md](../test/BOARD-BRINGUP-CASES.md) |
| **敲什么** | 交权之后在**同一个串口上按键**：`1` `2` `3` `4` `b` 单独开关某一项（`b` = 板载温度），`a` 全开，`?` 看帮助 |
| **坑** | 交权之后 `pt.*` 命令全部失效，那个按键菜单接管了串口 |

## B.7 `rs232` —— 自动发自动收（会话 ✅）

Klemmblock C。代码 [porttool_rs232.c](../../TestCase/porttool/porttool_rs232.c)。

| | |
|---|---|
| **测什么** | 端子 C05/C06 这条通道**持续**双向通 —— 不是开机时通一次 |
| **端子 / 引脚** | C05 TXD / C06 RXD → `PC10 · PC11`，`PB10` = MAX3221 EN |
| **怎么接** | 就是工装自己那条线，GND 接 C02 / C11 / C12 |
| **敲什么** | `pt.start rs232 period=3000`。参数只有 `period=` |
| **看到什么算过** | `!rs232` 帧的 `seq − rx` 恒为 1、`miss` 保持 0。`rxlines=` 跟着涨说明接收方向一直在过真流量 |
| **坑** | ⚠️ **`loop=self`，不是 `loop=ctrl`。**回环确实走控制口，但对这个端口来说那正是重点：字节双向穿过端子、MAX3221 和 PC10/PC11。**别的端口那个计数器只说明控制口活着；这里它就是判据。**<br/>⚠️ 面板上**没有**交权按钮（`pt.handover rs232`）。那个进去就没有命令循环了，只能按复位键。命令行敲得到，那是刻意的行为而不是一次点击 |

## B.8 `rs485` —— RS485 链路回环（会话 ✅）

Klemmblock C。代码 [porttool_rs485.c](../../TestCase/porttool/porttool_rs485.c) + [port_rs485.c](../../TestCase/common/port_rs485.c)。

**这是第一个真正的 `loop=link`** —— 回环走被测的那对差分线，所以这个计数器是**那对线的判据**，不像 `loop=ctrl` 只说明控制口活着。

| | |
|---|---|
| **测什么** | A/B 差分对双向通：板子发一个数，从**同一对线**上收回来，在此基础上累加 |
| **端子 / 引脚** | C10 / C11 (A/B) → `PD5 TX · PD6 RX · PD4 DIR`，USART2 |
| **怎么接** | **必须有对端。**USB-RS485 适配器 A 接 **A10**（J11-3）、B 接 **A11**（J11-2）。对端把收到的那行原样送回去 —— 上位机的链路应答器，或 `$TOOL/TestCase/tools/rs485_echo.py` |
| **敲什么** | `pt.start rs485 baud=115200 period=3000`。`baud=` 只收 9600 / 19200 / 38400 / 57600 / 115200 |
| **看到什么算过** | `!rs485` 的 `seq − rx` 恒为 1、`miss` 保持 0。`junk=` 保持 0（有数说明线上有东西但不是应答） |
| **坑** | ⚠️ **`pt.echo` 对这个端口是被拒绝的**（`ERR rs485 takes its echo on its own link, not on this one`）。在控制口上答它会让计数器在这对线已经死掉时照样往上涨 —— 那是这套工装绝不允许的假通过。<br/>⚠️ **半双工，而且是一条网络**：`PD4` 同时驱动 /RE 和 DE，发送时接收器是关的，**板子听不到自己**。没有对端就永远闭合不了 —— 那是接线不对，不是会话有问题。<br/>⚠️ **状态仍然从控制口上报。**上位机在一个地方读所有端口的状态；用 RS485 汇报 RS485 的健康，恰好在最需要的时候读不到。<br/>⚠️ 同一块硬件还有一个深度入口（五项引脚级诊断），它挂在**同一张卡**上，`pt.caps` 的会话行带 `targets=rs485` |

## B.4 模拟量 —— 三个会话 ✅

Klemmblock D，Upper Deck 与板载。三个会话：`ain`（[porttool_ain.c](../../TestCase/porttool/porttool_ain.c)）、`aout`（[porttool_aout.c](../../TestCase/porttool/porttool_aout.c)）、`temp`（[porttool_temp.c](../../TestCase/porttool/porttool_temp.c)），共用层是 `TestCase/common/port_adc.c` / `port_dac.c` / `port_vref.c`。

**敲什么**：`pt.start ain period=500` · `pt.start aout ch=1,2 mv=1:500,2:1500` · `pt.start temp period=1000`。

⚠️ **三个都在启动时先过 VREFBUF 这道闸**：使能不了就**拒绝启动**，而不是启动后报一堆看起来像真数据的数。帧格式与各自的坑见 [A.5](#a5-响应什么--采样帧的字段)。

| 端口 | 端子 | 引脚 | 测什么 | 换算 |
|---|---|---|---|---|
| Analog IN 1 | D12 | `PC3_C · ADC3_INP1` | 原始码 + 引脚 mV，**电压档** | 端子 V = 引脚 mV × 4.0089 |
| Analog IN 2 | D13 | `PA6 · ADC1_INP3` | 原始码 + 引脚 mV，**电流档** | 端子 µA = 引脚 mV × 1000 / 124 |
| Analog OUT 1 | D14 | `PA4 · DAC1_OUT1` | 设定电流 | `Iout = Vin × 10 / 1024`；设 500 / 1500 mV 应读到 **4.883 / 14.648 mA** |
| Analog OUT 2 | D15 | `PA5 · DAC1_OUT2` | 同上 | 同上 |
| AOUT 故障反馈 | — | `PI4 / PE3` | XTR111 开路 / 过载报警 | — |
| 板载温度 ×2 | — | `PA0 · ADC1_CH16`（短路保护）<br/>`PA3 · ADC1_CH15`（高侧开关） | LM50 | `T = (mV − 500) / 10`，合理带 −25…+100 °C |

**怎么接**：可调电压源接 Analog In 1（J3-4）或 In 2（J4-1），地接 Analog GND（J4-4 / J4-5）；电流表串在 Analog Out → 表 → Analog GND 的回路里。

### 模拟量启动前必须先过这道闸

```mermaid
flowchart TD
    A[pt.start ain 或 aout] --> V{VREFBUF 已使能?}
    V -->|否| VE["PortVref_Enable<br/>scale 0，等 VRR 再稳定 20ms"]
    V -->|是| D
    VE --> D{"VDDA 在 2000–3600 mV?"}
    D -->|否| REF["拒绝启动<br/>ERR VREF+ 崩了<br/>所有读数无意义"]
    D -->|是| J{"AOUT 变化时<br/>AIN 有反应?"}
    J -->|"读数跟着动"| OK["跳线已焊<br/>按档位换算显示"]
    J -->|"不动，落在悬空抖动带<br/>约 380 mV, 200–650 LSB"| NA["报「模拟前端未装配」<br/>只跑芯片级，标未覆盖"]
    J -->|"不动但读数稳定"| BAD["报异常<br/>可能短路或焊错档位"]

    style REF fill:#fce8e6,stroke:#d93025
    style BAD fill:#fce8e6,stroke:#d93025
    style NA fill:#fef7e0,stroke:#f9ab00
    style OK fill:#e6f4ea,stroke:#34a853
```

> ⚠️ **四个会静默毁掉判据的坑**（[HARDWARE-FACTS.md:154-222](HARDWARE-FACTS.md)）：
> 1. **VREFBUF 不使能** → ADC 读出 `0x8000` / `0x4000` 这类**看起来像真数据**的值。板上没有基准芯片
> 2. **模拟跳线 JP1–JP9 出厂全开路** → `PC3_C` 和 `PA6` 彻底悬空，读到的只是悬空脚。**焊上去不可逆**
> 3. **`SYSCFG_PMCR.PC3SO` 复位默认闭合** → 数字单元加载该节点，读数差约 16%。模拟采样一律置开
> 4. **AOUT 的电流公式只在 JP3 / JP4 开路时成立** —— 那两个跳线把 VREF 经 40k2 灌进 XTR111 的 VIN 求和节点，闭合后变成 4–20 mA live-zero（DAC 设 0 时已有约 4.85 mA）

## B.6 `dout` —— 数字输出（会话 ✅）

Klemmblock A，Lower Deck。代码 [porttool_dout.c](../../TestCase/porttool/porttool_dout.c) + [port_dout.c](../../TestCase/common/port_dout.c)。

| | |
|---|---|
| **测什么** | 八路 24 V 输出的**开关**与**各自独立的 PWM 占空比** |
| **端子 / 引脚** | A03–A10 → `PB13 PB0 PH15 PE4 PA8 PA9 PI7 PE5`，前级 U3/U4 两颗 VNQ5160K-E 智能高侧开关，各管四路 |
| **怎么接** | 万用表或示波器量端子。**最有价值的接法是一根八芯线把 A03–A10 接到 D02–D09** —— DO 出 24 V、DI 耐 24 V，一次覆盖 16 个通道，配 `mode=blink` 就能看 DI 跟着翻 |
| **敲什么** | 逐路占空比：`pt.start dout ch=1,5 duty=1:20,5:75 freq=1000`<br/>全部开：`pt.start dout duty=100`<br/>翻转给 DI 看：`pt.start dout ch=1,2,3,4,5,6,7,8 mode=blink duty=100 period=500`<br/>参数：`ch=` 1..8、`mode=hold|blink`、`duty=` 单值或逐路（0..100 %）、`freq=` 1..2000 Hz（默认 1000）、`period=` blink 半周期毫秒 |
| **看到什么算过** | 端子电压随占空比变；`blink` 下 DI 会话的 `v=` 位域跟着 DO 翻转逐位对应 |
| **坑** | ⚠️ **软件 PWM，不是硬件定时器通道。**八路引脚确实都在定时器通道上，但**两两共用一个比较单元**（DO1/DO5 = TIM1_CH1 及其互补输出、DO2/DO6 = TIM1_CH2、DO3/DO7 = TIM8_CH3、DO4/DO8 = TIM15_CH1），硬件方案只能给 4 个独立占空比。理由见 [DECISIONS.md 第 10 条](DECISIONS.md)。<br/>⚠️ **中断跑在 TIM7 上**，频率 = `freq × 100`，所以 `freq` 封顶 2000 Hz（中断 200 kHz）。<br/>⚠️ **VNQ5160K-E 的 PWM 上限没有数据手册可查** —— 它是智能高侧开关，内部有电荷泵和保护逻辑。`freq` 刻意放开就是为了让工程师扫频量出来，**实测结果要补进 [HARDWARE-FACTS.md](HARDWARE-FACTS.md)**。<br/>⚠️ **`pt.handover pwm` 和这个端口抢 PA9**（Digital Out 6）。交权前会先停所有会话，`dout` 的 stop 会关掉 TIM7 中断，所以不会打架 —— 但那个交权目标只驱动 DO6 一路，逐路 PWM 用这个会话。 |

## B.5 其余未实现的端口 ⬜

这些还没有任何入口，列在这里是为了说清**工装当前覆盖不到哪里**。怎么测的规程等实现时再写。

### Klemmblock A —— 数字输出（Lower Deck）

| 端口 | 端子 | 引脚 | 要测什么 |
|---|---|---|---|
| ~~Digital Out 1–8~~ | ~~A03–A10~~ | — | **已实现，移到 [B.6](#b6-dout--数字输出会话-)** |

前级是 **VNQ5160K 智能高侧开关**，输出 24 V。DI 耐 24 V，所以 **DO→DI 线束互接可行**，一根 8 芯线覆盖 16 个通道。

### Bridge 板自带接口

| 端口 | 引脚 | 要测什么 |
|---|---|---|
| USB-C（仅 Device） | `PA11 / PA12` | 枚举出 CDC 串口 |
| 以太网 | RMII + LAN8742A | 拿到 IP、链路速率、丢包 |
| RTC 电池 | VBAT + 32.768 kHz | 走时、备份域是否失效 |
| 状态 LED | `PE2`（高亮低灭） | 目视 |

### Junction Link —— DIN 导轨扩展口（J4，20 pin）

| 端口 | 引脚 | 要测什么 |
|---|---|---|
| UART4 | `PH13 TX · PH14 RX` | TX↔RX 短接自环 |
| SPI2 | `PI1 SCK · PI3 MOSI · PC2_C MISO · PI0 NSS` | MOSI↔MISO 短接自环 |
| SPI6 | `PG13 SCK · PG14 MOSI · PB4 MISO` | 同上 |
| I2C2 | `PH4 SCL · PH5 SDA · PH6 SMBA` | 需要从设备（一颗 24C02 即可） |
| Debug Trigger | `PC7` | 逻辑分析仪触发点 |

> ⚠️ `PH13 / PH14` 也是 **UART4**，和 RS232 端子的控制台共用同一个外设 —— **同一个 UART 只有一个句柄槽，最后一次 `begin()` 赢**。扩展口上用 UART4 就会顶掉工装自己的控制通道。

---
---

# C 附 —— 镜像开关、物理连接、用法

## C.0 一条范围规矩：**工装镜像里没有 bootloader 这件事**

**用户 2026-09-08 强调过两次。** 工装是一个独立的镜像，ST-Link 整片烧、自己有链接脚本（`FLASH` 拿满 2048K）。所以：

- **不许用「反正产线那一站会烧 bootloader」来论证任何测试怎么做。** 2026-09-08 我用这条理由把 USB 测试推到主机侧看 CDC 枚举，被驳回 —— 那是把 bootloader 拉进了工装的世界。
- 工装要测某个外设，就**在工装镜像里把那个外设跑起来**。尺寸不再是理由（见 [DECISIONS.md 第 14 条](DECISIONS.md) 的 2026-09-08 补充）。
- 反过来也成立：工装镜像里的东西不进 bootloader，`PORTTOOL_ENABLE` 就是那道墙。

## C.1 `PORTTOOL_ENABLE` 怎么把业务线整条绕开

这个宏决定这块板是业务模式还是硬件测试模式。**默认 0。**

```mermaid
flowchart TD
    RESET([上电 / 复位]) --> INIT["HAL_Init · SystemClock_Config<br/>MX_GPIO_Init · BOOT0_ConfigureAsInput<br/>Enable_RX_RS232 · MX_UART4_Init"]
    INIT --> SW{PORTTOOL_ENABLE}

    SW -->|"1 · 硬件测试模式"| PT["PortTool_Run<br/>永不返回"]
    SW -->|"0 · 业务模式（默认）"| BIZ["boot_window_relay<br/>BOOT0 手势窗口"]

    BIZ --> DECIDE["server_decide"]
    DECIDE -->|IAP_NONE| JUMP["server_jump_to_app<br/>交权给 app"]
    DECIDE -->|CDC / ETH / ALL| IAP["IAP 服务器<br/>等待烧写"]

    PT --> LOOP["命令循环<br/>见 A.8"]

    style PT fill:#e8f0fe,stroke:#4285f4,stroke-width:2px
    style LOOP fill:#e8f0fe,stroke:#4285f4,stroke-width:2px
    style JUMP fill:#e6f4ea,stroke:#34a853
    style IAP fill:#e6f4ea,stroke:#34a853
```

**测试模式下 `PortTool_Run()` 永不返回**，所以 BOOT0 手势、启动决策、跳 app、IAP 服务器**一个都不执行**。

调用点在 [main.c](../../Core/Src/main.c) 的 `USER CODE BEGIN SysInit` 块里，紧跟 `MX_UART4_Init()` 之后 —— UART4 起来了，工装要的就只有这个，它既是命令通道也是日志。位置和写法跟旁边那排 `#if KNX_TEST_ENABLE` 完全一样，并且**并进了同一条互斥 `#error`**：这些入口没有一个会返回，所以同时只能开一个。

> ⚠️ **`PORTTOOL_ENABLE` 不在任何构建配置里，要在 CubeIDE 的工程属性里手工加**（2026-09-07 定，[DECISIONS.md 第 14 条](DECISIONS.md)）。**忘了改回 0 就发出一个开不了机的 bootloader** —— 那块板永远到不了 IAP 服务器，USB 和以太网都升不了级。所以 `PORTTOOL_ENABLE=1` 时编译会打一条 `#warning` 把这件事写进构建日志。
不涉及烧写工具、不涉及授权 —— ST-Link 直接烧固件，开串口看结果。

`PORTTOOL_ENABLE=0` 时 `TestCase/porttool/` 的每个 `.c` 整个函数体都被宏关掉，**一个字节都不进镜像**（工程没开 `--gc-sections`，只靠「不调用」省不下 Flash）。

## C.2 物理连接：一条串口，两种行

```mermaid
flowchart LR
    subgraph PC["上位机 PC"]
        UI["浏览器 UI"]
        GO["Go 工装"]
        UI <-->|"本地 HTTP"| GO
    end

    GO <-->|"USB-RS232 适配器<br/>115200 8N1"| ADP(("端子<br/>C05 TxD<br/>C06 RxD<br/>C02 GND"))
    ADP <--> MAX["MAX3221<br/>±12V 收发器"]
    MAX <--> UART["UART4<br/>PC10 / PC11"]
    UART <--> FW["PortTool_Run"]

    FW -.->|"!din t=… v=0x16"| UART
    FW -.->|"OK started din"| UART
    FW -.->|"[T1 ] inputs: …"| UART
```

⚠️ **端子是真 ±12V，必须用 USB-RS232 适配器，接 TTL 适配器可能烧掉**（[HARDWARE-FACTS.md:17](HARDWARE-FACTS.md)）。
⚠️ **这条通道被工装占用，所以工装测不了 RS232 端子本身** —— 见 [B.3.3](#b33-rs232--rs232klemmblock-c)。
⚠️ `PB10` 拉低 = MAX3221 整片关断，printf 一个字节都出不来。`main.c` 在 `MX_UART4_Init()` 前已 `Enable_RX_RS232()`。

## C.3 三种用法，固件只有一套

```mermaid
flowchart LR
    FW["固件<br/>pt 命令 + 感叹号帧<br/>一套，不区分用法"]

    FW --- M["手动<br/>工程师敲命令或点 UI<br/>看面板、追波形"]
    FW --- P["产线<br/>上位机一键跑预设序列<br/>出 Pass/Fail 与报告"]
    FW --- A["自动化<br/>脚本调 Go 工装的 CLI<br/>机器可读结果"]

    style FW fill:#e8f0fe,stroke:#4285f4,stroke-width:2px
```

**判定和序列全在上位机** —— 固件只报原始采样。改判据不用重烧固件，产线和研发用同一份固件。

## C.4 上位机怎么分发、怎么起来

**一个 exe，两种模式。**2026-09-04 拍板，见 [DECISIONS.md 第 7、8 条](DECISIONS.md)。

> **和 IAPTool 的关系**（2026-09-07 定）：两者住在 `$TOOL` 同一个 Go module 里，共享 `internal/`（serialx 的开口与重试 · logx · config · buildinfo），`compile_tool.sh` 一次产出 **`IAPTool.exe` 和 `PortTool.exe` 两个文件**。
>
> 这不和上面那句冲突 —— 「一个 exe 两种模式」说的是 **PortTool 自己**既是面板又是 CLI，不是说这两个工具要合成一个。受众不同：硬件工程师的工具里不该出现固件签名和 `takeown`。
>
> **面板上的文字是中文、大白话；固件 printf、`OK`/`ERR` 应答、两个 exe 的 stdout 仍然是英文**（[DECISIONS.md 第 11 条](DECISIONS.md)）。

```mermaid
flowchart TD
    EXE(["porttool.exe<br/>单文件，Go 交叉编译<br/>HTML/JS 用 embed 打在里面"])

    EXE -->|"双击 · 不带参数"| SRV["绑 127.0.0.1 起本地 HTTP<br/>自动拉起默认浏览器"]
    EXE -->|"命令行 · 带子命令"| CLI["机器可读输出<br/>产线序列 / 自动化脚本"]

    SRV --> PANEL["浏览器面板<br/>! 帧经 SSE 推到页面"]
    CLI --> OUT["stdout / 退出码"]

    SRV -.->|"同一份串口驱动、同一份判据"| CLI

    style EXE fill:#e8f0fe,stroke:#4285f4,stroke-width:2px
    style PANEL fill:#e6f4ea,stroke:#34a853
    style OUT fill:#e6f4ea,stroke:#34a853
```

**零安装是硬约束**：拷一个文件过去就能跑，目标机器上不装运行时、不装 Python、不解压 DLL、不要管理员权限。**浏览器不算安装** —— Win10/11 自带 Edge，它是唯一一个「已经在那儿、且不需要我们分发或保证」的 UI 运行时，选它就是为了这个。

**两种模式必须共用同一份判定代码** —— [C.3](#c3-三种用法固件只有一套) 那三种用法共用一份固件，上位机拆成两个程序会立刻出现两边判据不一致。

> ⚠️ **三件还没验证的，都得在一台干净机器上才试得出来**：
> 1. **USB-RS232 适配器的 COM 驱动** —— 它装在操作系统里，不在 exe 里，**这是唯一一个真的可能要装东西的地方**。选型要挑 Windows 自带或 Windows Update 能自动装上的芯片
> 2. **SmartScreen** —— 从网络下载的未签名 exe 首次运行会被拦。真要消掉需要 Authenticode 代码签名证书，**和 `$TOOL` 里那套固件签名是两回事**
> 3. **防火墙** —— 显式绑 `127.0.0.1` 而不是 `0.0.0.0`，理论上不会弹窗要管理员，没实测过

## C.5 还没做的 —— 三期计划

2026-09-07 定的开工顺序。**上半部分描述的是固件 `0.5.0` 的现状（期一、期二完成；期三已做：`pt.run` 八个目标、CAN 会话、KNX 会话、SD / SDRAM 一次性动作、老化、caps 限值、caps 的 `kind=run` 行）。**协议增量与逐端口的设计取舍见 [DECISIONS.md 第 9–13 条](DECISIONS.md)。

**每一期结束都是工程师能拿去用的东西，不是半成品。**

### 期一 · 骨架 —— 工程师第一次能用

**前置：先让工装能被烧出来** ✅ 2026-09-07 完成

核实时发现工装**根本没接进构建** —— `PortTool_Run()` 全仓没有调用点。已补上 [main.c](../../Core/Src/main.c) 的调用点；`PORTTOOL_ENABLE` 按 [DECISIONS.md 第 14 条](DECISIONS.md) **不进构建配置，在 CubeIDE 工程属性里手工加**。

**固件**（协议改动只有一处）

- `reply_caps()` 从「只遍历 2 个会话端口」改成**统一清单**，交权目标并进来。新增 5 个字段：`kind=`（session / handover）、`blk=` + `term=`（面板分组与端子标签）、`loop=`（ctrl / link / self / none）、`params=`（这个端口收哪些参数，面板据此渲染控件）。子通道的端子标签用 `terms=D02,D03,…` 另起一行
- 每个 `porttool_*.c` 的 `caps` 回调补上这些字段
- 版本 `0.1.0` → `0.2.0`

**为什么这一处必须先改**：不改的话端口清单只能硬编码在上位机里，**固件加一个端口就要重发 exe**，两边还会漂。改完之后面板完全由 caps 驱动，固件是唯一事实源。

**上位机**（`$TOOL` 仓库，见 [C.4](#c4-上位机怎么分发怎么起来)）

- 抽 `internal/`（serialx · logx · config），`compile_tool.sh` 出两个 exe
- 串口枚举（带 VID/PID 与描述串）+ 「角色 → COM 口」映射表，存进 `local_config.json`
- 本地 HTTP + SSE + `go:embed` 页面；四类行分流（`OK` / `ERR` / `!` / 裸日志）。**推送用 SSE 不用 WebSocket** —— [DECISIONS.md 第 15 条](DECISIONS.md)
- caps 驱动的端口树、子通道逐路复选、参数控件、会话启停
- 日志窗格：分色 / 暂停继续 / 过滤 / 导出。⚠️ **暂停的是渲染，不是读取** —— 停读会让串口接收缓冲溢出，而且回环的 `pt.echo` 跟着停，板子的 `miss` 会开始涨
- 交权目标**逐个摆出来**（14 项各占一行）+ 确认框 + printf 转发
- 底部常驻原始命令输入框

**交付**：`PortTool.exe` 拷过去双击，能测 DI 八路和继电器六路，其余端口进交权看原始日志。

### 期二 · 回环 + 模拟量 + DOUT/PWM

**固件** —— ✅ 全部完成（2026-09-07）

- ✅ `pt.echo <port> <n>` 命令；`porttool.h` 的通用 `seq` / `rx` / `miss`（[DECISIONS.md 第 9 条](DECISIONS.md)）。三种结局都能从帧本身分辨：闭合、断开（**`seq` 停住，只有 `miss` 涨**）、过期回复（算 miss 不算闭合）
- ✅ `PortCmd_GetPairs()` —— `ch:值` 解析器，dout 的 `duty=`、relay 的 `on=`、aout 的 `mv=` 共用。**先暂存后提交**，所以被拒的命令一个参数都没生效
- ✅ **新写 `port_dout.c`** —— 八路 GPIO + TIM7 软件 PWM（[DECISIONS.md 第 10 条](DECISIONS.md)）；`porttool_dout.c` 会话，逐路 `duty=` 加 `mode=blink`。见 [B.6](#b6-dout--数字输出会话-)
- ✅ relay 改逐路设电平，去掉 `level=` 字段
- ✅ `porttool_ain.c` / `porttool_aout.c` / `porttool_temp.c` —— 含 VREFBUF 闸门：**使能不了就拒绝启动**，而不是启动后报一堆看起来像真数据的数。见 [B.4](#b4-模拟量--三个会话-)
- ✅ `porttool_rs485.c` —— 第一个 `loop=link`。见 [B.8](#b8-rs485--rs485-链路回环会话-)。`pt.echo` 对它**被拒绝**，因为在控制口上答它会让计数器在那对线已死时照样涨
- ✅ `porttool_rs232.c` —— 自动发自动收，`loop=self`（新增的第四种回环类型，[DECISIONS.md 第 16 条](DECISIONS.md)）。见 [B.7](#b7-rs232--自动发自动收会话-)

**上位机**

- ✅ 回环应答器 —— 收到 `loop=ctrl` 端口的帧就自动回 `pt.echo <port> <seq>`。**可以关掉**，那是唯一能从面板证明这个计数器有意义的办法：关掉以后 `miss` 必须开始涨而 `seq` 停住
- ✅ 逐路参数在面板上是**每路一个控件**，不是一个装着 `1:20,5:75` 的文本框
- ✅ 链路健康区（`seq` / `rx` / `miss`）与读数区视觉隔开，且 `loop=ctrl` 的端口下面写明「这个数只说明控制口活着」
- ✅ **多 COM 角色绑定** —— `loop=link` 的端口卡上多一行「对端串口」，绑一个第二适配器上去，面板就在**那条线**上把板子发来的每一行**原样**送回去。RS485 因此在面板上能闭合，不用再另开 Python 脚本当对端。<br/>⚠️ **原样送回，不加一** —— 板子自己比对送出和收回的数（`seq` 对 `got`），改动内容看起来会和链路死了一模一样。<br/>⚠️ 绑给 `loop=ctrl` 的端口会被拒（那种回环走控制口，第二个串口什么也不做，只会让人去查一段不存在的接线故障）；控制口本身也不能兼任对端
- ✅ 模拟量换算显示（V / µA / mA）+ 悬空带（raw 200–650）告警 —— AIN1 ×4.0089 / AIN2 ÷124 / temp 0.1 ℃ / aout µA，卡片上写明「按已定档位换算」这个前提
- ~~DO→DI 对照视图~~ ✅ **2026-09-08 做完**（一根八芯线把 A03–A10 接到 D02–D09，一次覆盖 16 个通道）。逐路只驱动一个输出，看是不是只有对应那一路输入跟着动；⚠️ **判据是「矩阵对角」不是「读到 1」** —— 高低电平取决于怎么接的和工装怎么做，写死极性会把好板子判成坏的，而对角这个判据与极性无关，且正好定位**错位 / 短路 / 断路**三种线缆故障。dout 和 din 两张卡上都有入口（那根线连的是两个端口）。H5 用模拟板的 `sim.cable 1|2|3` 验过三种接线，含两种故障。

⚠️ **整套仍然没上过真板。**主机侧覆盖是用例 H4（固件侧 82 项 + 上位机侧 23 项，带 `-race`），但 `port_dout.c` 的中断本身、模拟前端有没有焊跳线、VNQ5160K-E 的 PWM 上限，都只能在板子上量。


### 期三 · 剩下的端口 + 产线

**2026-09-07 按硬件工程师的产线测试指南（`OPLC-MFG-TEST-001`）重排过顺序，当天又按用户给的参考架构调了一次。** 那份文档逐项要什么、我们有什么、架构契合度多少 → [PRODUCTION-TEST-GAP.md](PRODUCTION-TEST-GAP.md)。产线上位机长什么样 → [PRODUCTION-FRAMEWORK.md](PRODUCTION-FRAMEWORK.md)。范围与形态七条已拍板，见 [DECISIONS.md 第 19–25 条](DECISIONS.md)。

**为什么顺序是这个**：产线要的架构核心是**方案文件数据化** —— 后面每一步的产出都要落进那个格式，格式定晚了就要返工。

**前五件不依赖任何新硬件**

1. **接口清单 [FIXTURE-INTERFACE.md](FIXTURE-INTERFACE.md)** ✅ 已写好，等发给硬件工程师 —— 他做板要时间，所以先出手。
2. **方案文件格式 + 执行器** —— 格式定义已经写好（[PRODUCTION-FRAMEWORK.md](PRODUCTION-FRAMEWORK.md) 第二至四节），要落成代码。**这是现在一行都没有的那一层。** 注意判据**不是独立一层**，是步骤自己的参数（[第 24 条](DECISIONS.md)）；步骤类型只做三个通用的（[第 25 条](DECISIONS.md)）。
3. **协议增量 `pt.run <target>`** ✅ **2026-09-07 做完，2026-09-08 长到八个目标**（固件 `0.3.0` → `0.5.0`，H4 已覆盖含 `-race`；**真板子上还没跑过**，见 [PORTTOOL-FIRST-BENCH.md](../test/PORTTOOL-FIRST-BENCH.md) 的 B5–B7）。一次性动作，**跑完回到命令循环**。产线有一整类是「跑一次给个机器可读结果」（SDRAM / SD 压力、以太网 / USB 吞吐、RTC、导轨回环），而交权是一去不回的裸 printf，上位机解析不了。新增 `TestCase/porttool/porttool_run.c` + 动作表（形状照 [B.3](#b3-交权目标--14-个独占入口) 那张学），派发点在 `porttool.c` 的命令表加一个 verb。
   ⚠️ **结果里是原始值不是结论** —— `OK <target> errors=0 capacity=67108864 crc=0x…`，按[第 22 条](DECISIONS.md)。
4. **SDRAM / SD 接上 `pt.run`** —— 它们的逐项检查本来就是**能返回的函数**（`TestCase/SDRAM/sdram_test.c` 里的 `SDRAM_Test_Bringup` / `DataBus` / `AddressBus` 都返回 `int`），只有三个对外入口死循环。给 `pt.run` 加目标去调那些函数、报 `k=v`，**交权镜像本身不用改**（[第 22 条](DECISIONS.md)不管交权那条路）。
5. ~~caps 补参数约束~~ ✅ **2026-09-08 做完**：每个会话多一行 `OK limits=<port> <名>:<spec> …`，四种 spec —— `lo..hi`、`lo..`、`a|b|c`、`ch:1..n`。
   ⚠️ **值是从 apply() 用的同一批宏 `snprintf` 出来的**，不是手写第二份；H4 还把它和固件真实的拒绝消息交叉比对，所以限值和检查一旦漂开就当场失败。
   上位机 `ptproto.Limit` 解析它，`ptplan.CheckAgainstCaps` 用它**离线判方案里的参数值**（`period=10` 会被指出「firmware says it must be at least 50」）。

**固件**

- ~~CAN 的硬件层抽取~~ ✅ **2026-09-08 做完**：`TestCase/common/port_can.{c,h}` 拿走了时序表、时钟/引脚初始化、开关收发；`can_test.c` 只剩编排与诊断，五个交权入口行为不变。会话是 `porttool_can.c`（`loop=link`，`mode=normal|listen|loopback`）。按[第 17 条](DECISIONS.md)四个交权项改挂到会话行上。
- ~~SD / KNX~~ ✅ **2026-09-08 做完，但走的不是抽层那条路**：目的是机器可读结果，抽层只是手段之一。
  - **KNX 成了会话** `porttool_knx.c`（`loop=link`，`mode=loopback|listen|frames`）—— 回路是 MCU → STKNX → 总线 → STKNX → MCU，闭合就证明了收发器和总线，`bus=`/`vcc=` 把「总线没电」和「芯片坏」分开。
  - **SD 挂上了 `pt.run`**：`sd.probe` / `sd.integrity` / `sd.stress`（64 轮）。交权那两个改名 `sd.info` / `sd.integrity.soak`。
  - **SDRAM 其余两项也挂上了 `pt.run`**：`sdram.sweep`（整片四花样）/ `sdram.retention`（一个周期就返回）。交权侧改名 `sdram.retention.soak`。
- ~~caps 里没有 `pt.run` 目标~~ ✅ **2026-09-08 补上** `kind=run` 行 + `runs=` 字段。**这才让方案文件里的 `pt.run` 目标能离线校验** —— 打错一个字以前只能在产线上、板子面前才发现。上位机 `ptplan.CheckAgainstCaps` 现在报「this firmware does not report」。
- ~~产线方案文件~~ ✅ `$TOOL/TestCase/plans/station6-poweron.json` —— 工站 6 那 20 步，由 `porttool_plan` 的测试拿假板子真跑一遍
- **RTC 写校准** —— 读有了，写没有。⚠️ 两个前置：备份域里住着 iap_auth 的 nonce 计数器；`rtc.c:74` 选的是 **LSI 不是 LSE**，精度先天不够，谈校准之前要先定这个
- **DIN 导轨连接器回环** —— 小件，等 `FIXTURE-INTERFACE.md` 有回音
- ~~老化模式~~ ✅ **`soak` 会话**（`porttool_soak.c`）：高边输出轮转、继电器慢速翻转、温度与基准监控、自计时、`faults>0` 时指示灯闪。⚠️ **继电器 30 秒才翻一次是硬约束不是调参** —— HF41F 额定 3×10⁴ 次，每秒翻一次跑两小时就吃掉四分之一寿命
- ~~以太网会话与吞吐~~ ✅ **2026-09-08 做完**（固件 `0.7.0`，`porttool_eth.c`）：`eth` 从 `kind=run` 改成 `kind=session`，`eth.link` 以 `runs=` 留在同一行（[第 28 条](DECISIONS.md)）。三个模式 `echo` / `sink` / `source`，`ip=` 收静态地址兜没有 DHCP 的工位。⚠️ **这让工装镜像把 lwIP 链了回来**，从 ~100K 涨到 153,956 字节 —— 整片烧没有尺寸门禁。H4 有 31 条断言（含 lwIP 只初始化一次）。**真板子上还没跑过。**
- ~~USB-CDC 会话与吞吐~~ ✅ **2026-09-08 做完**（`porttool_usb.c`，四个模式 echo / sink / source / info）。⚠️ **`usbd_cdc_if.c` 的 `CDC_Receive_FS` 在 `PORTTOOL_ENABLE` 下改调 `PortUsb_Received`，那不只是行为分流 —— 它是唯一挡住整个 IAP 服务器被 USB 栈拖进工装镜像的东西**（[第 31 条](DECISIONS.md)）。镜像 153,956 → 159,224 字节，`nm` 核过 `IAP_data_recv` / `IAP_task` / mbedTLS 一个都不在。**真板子上还没跑过。**
- 编码器 1–4；扩展口总线（⚠️ 扩展口的 `PH13/PH14` 也是 UART4，会顶掉工装自己的控制通道，见 [B.5](#b5-其余未实现的端口-)）

**上位机**（`$TOOL`，四个新包 + 面板一页）

| 包 | 干什么 | 硬约束 |
|---|---|---|
| `internal/ptplan` | 方案文件的读写与校验（步骤序列 + 每步参数） | 判据在步骤参数里，**不单独成层**（[第 24 条](DECISIONS.md)） |
| `internal/ptseq` | 执行器：按序跑步骤，管六个通用字段（条件 / 重试 / 前后延时 / 超时） | 每次尝试都进报告，**重试不覆盖原失败**；超时与判定失败分开记 |
| `internal/ptcheck` | 判定算子：`range` / `eq` / `ne` / `contains` / `count_zero` | **面板和产线序列共用**，判据不许有第二份 —— 延续 `ptproto` 已有的原则 |
| `internal/ptreport` | 逐项原始值出 CSV/JSON，含方案名与限值版本 | **判据本身也要进报告**，否则事后不知道当时按什么判的 |

✅ **面板的「方案」页有了**（`internal/ptpanel/plan.go` + `web/index.html` 的 `方案` tab）：左边方案文件、中间步骤表带复选框、右边属性网格（[第 24 条](DECISIONS.md)）。

✅ **面板的手工页按「选端口 → 看怎么测 → 按一下 → 等结论」重排了**（2026-09-08，真板子上逐端口验过，H5 95 项全过）：

- **参数与判据都来自方案文件那一步**，参数预填进卡上的输入框，按下去发的就是屏幕上那条命令（[第 26 条](DECISIONS.md)）。**逐通道参数也预填** —— 漏了这一条，继电器和模拟输出会被拿默认值 0 启动，好板子判失败
- **结论四种加一种**：未测 / 测试中 / 通过 / 失败，加「人工判」给只有交权入口的端口（[第 27 条](DECISIONS.md)）
- **带 `minutes=` 的会话按 `done=1` 收尾**，跑的时候显示板子自己报的 `left_s` 倒计时、`faults`、最高温，并且有「中止」按钮 —— 十小时的老化不能只有复位键一条退路
- **下拉框的取值来自固件的 `limits=` 行**，不是页面里的表；数值框的上下界同源
- 对端串口（RS485 / CAN）预选上次绑的那个 COM

仪器与工装板先走 `Tool` 步骤类型（起进程），不单独写驱动层 —— 等 [FIXTURE-INTERFACE.md](FIXTURE-INTERFACE.md) 第三节有回音再决定要不要 `Fixture` 类型。

✅ **CLI 有了**（2026-09-07）：`cmd/porttool` 的 `validate` / `run` 两个子命令，仍然一个 exe（[DECISIONS.md 第 8 条](DECISIONS.md)）。`ports` / `version` 还在。

**挂起：AI / AO 逐板校准。** 产线指南要求「写补偿值再重测」，但规格书 5.2 自己标注 AI/AO 精度 0.1 % 偏激进（主流 0.3 % / 0.6 %）。**先问硬件工程师这个指标硬不硬** —— 松了这一整块不存在。详见 [PRODUCTION-TEST-GAP.md](PRODUCTION-TEST-GAP.md)。

### 三件零安装风险，还没在干净机器上验证过

见 [C.4](#c4-上位机怎么分发怎么起来) 末尾。另外 2026-09-07 新识别一条：**VNQ5160K-E 的 PWM 上限没有数据手册可查**，要示波器实测（[DECISIONS.md 第 10 条](DECISIONS.md)）。
