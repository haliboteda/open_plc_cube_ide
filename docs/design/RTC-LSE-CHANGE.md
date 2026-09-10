# RTC 换 LSE：要在 CubeMX 里点哪几下，之后查什么

决定和理由在 [DECISIONS.md 第 36 条](DECISIONS.md)。这份只回答「怎么做」。

⚠️ **这份是给人照着做的，不是给 AI 手改 `.ioc` 用的。** 下面第四节写了为什么不能手改。

---

## 一、现在是什么样

| | 现状 | 出处 |
|---|---|---|
| RTC 时钟源 | `RCC_RTCCLKSOURCE_LSI` | `Core/Src/rtc.c` 的 `HAL_RTC_MspInit()` |
| `.ioc` 里的 RTC 频率 | `RCC.RTCFreq_Value=32000`（LSI 的标称值） | `open_plc_cube_ide.ioc` |
| `.ioc` 里的 LSE | **完全没有** —— `RCC.IPParameters` 里没有任何 LSE 项，`PC14`/`PC15` 也不在引脚表里 | 同上 |
| 板上的晶振 | **XTAL2**，32.768 kHz，ABS07 系列，Bridge 板顶层，**在贴片坐标里** | `$HW/Bridge_overview.txt`、`$HW/Production/Bridge/Pick Place ...txt` |
| 备份电池 | **BAT1 = VL1220/1HF**，可充电纽扣电池 | `$HW/Bridge_overview.txt` |

**硬件是齐的，固件从来没用上。**

---

# 🍍 二、在 CubeMX 里点这四下

打开 `open_plc_cube_ide.ioc`：

1. **System Core → RCC** → 把 **Low Speed Clock (LSE)** 从 `Disable` 改成 **`Crystal/Ceramic Resonator`**
   （这一步会自动把 `PC14` / `PC15` 变成 `RCC_OSC32_IN` / `RCC_OSC32_OUT`）
2. **Clock Configuration** 页 → 把 **RTC 那一路的输入**从 `LSI` 切到 **`LSE`**
3. 确认那一页上 RTC 的频率显示成 **32.768 kHz**（不再是 32 kHz）
4. **Project → Generate Code**

---

## 三、生成之后必须查这些

除了 [CUBEMX-RULES.md](CUBEMX-RULES.md) 里那两项常规必查，这次多四项：

| # | 查什么 | 怎么算对 |
|---|---|---|
| 1 | `Core/Src/rtc.c` 的 `HAL_RTC_MspInit()` | `RTCClockSelection` 变成 `RCC_RTCCLKSOURCE_LSE` |
| 2 | `Core/Src/main.c` 的 `SystemClock_Config()` | 多了 `RCC_OSCILLATORTYPE_LSE` 和 `LSEState = RCC_LSE_ON` |
| 3 | `PC14` / `PC15` 没有被别的东西占 | 生成前 `.ioc` 里这两个脚是空的，本来就没人用 |
| 4 | **启动时间** | 见下面那条 ⚠️ |

⚠️ **最要紧的一条：LSE 起振比 LSI 慢得多（几百 ms 到 2 s），而 HAL 的 `HAL_RCC_OscConfig()` 在等 LSE 就绪时是阻塞的，超时值 `LSE_TIMEOUT_VALUE` 是 5000 ms。**

后果有两个方向：

- **正常情况**：启动多花几百毫秒。bootloader 的超时窗口要重新看一遍 —— 它靠 `HAL_GetTick()` 计时，而这段等待发生在时钟配置里，会把整条启动路径整体推后
- **晶振没起振**（没焊、焊虚、负载电容不对）：**每次上电白等 5 秒**，然后 HAL 回退或报错。这在产线上是很贵的 5 秒，而且看起来像板子挂了

**所以生成之后第一件事是量启动时间**，和改之前对比。工装镜像最省事：上电到第一条 `!` 帧出来的时间。

---

## 四、为什么这份是「照着点」而不是「我改好了」

`.ioc` 是 CubeMX 的私有格式：引脚表是 `Mcu.Pin0` … `Mcu.Pin67` 加一个 `Mcu.PinsNb=68`，加两个脚要同时改编号、改计数、改 `RCC.IPParameters` 那一长串，而且引脚名的写法（`PC14-OSC32_IN(PC14)` 这类）必须和 CubeMX 对这个封装的内部叫法完全一致。

**写错了最坏的结果不是报错，是被静默丢弃** —— 重新生成出来的工程照旧用 LSI，而所有人都以为已经换过了。**在 CubeMX 界面里点，是唯一一条工具本身会校验的路。**

---

## 五、换完之后才谈得上的事

`rtc.write` / 校准这一批（[../test/PORT-BRINGUP-PLAN.md](../test/PORT-BRINGUP-PLAN.md) 第 2 批第 2.2 节）。在 LSI 上谈校准没有意义 —— 它一天能漂一个多小时，校谁都对不上。
