# M4 · FMC 引脚防护 ✅ 2026-08-17 已完成

**覆盖需求：[E6](../STATUS.md)** —— FMC 占用的 39 个脚在变体头里有名字、看得见。

> ⚠️ **E6 的原文是"用户 `digitalWrite` 碰不到"，落地时改写了。** 采用的方案是可发现性，不是拦截 —— 拦截在本文件「已否决」里就砍掉了。`digitalWrite(PE7, ...)` **现在仍然编得过**，那是刻意的。需求原文和实现不符时改需求，不要让实现假装满足了它。

## 是什么

`NUM_DIGITAL_PINS 140` —— 变体把 MCU 全部引脚都注册成可用的 Arduino 数字引脚。这是 STM32duino 的常规做法，让用户能 `digitalWrite(PXn, ...)` 访问任意引脚。

其中 39 个脚接的是 SDRAM。

## 在哪找

| | |
|---|---|
| 引脚总数 | `core:variants/STM32H7xx/H743/variant_PLC_H743.h` 的 `NUM_DIGITAL_PINS` |
| FMC 的 39 脚清单 | `Core/Src/fmc.c` 的 `HAL_FMC_MspInit()` 里那段 `FMC GPIO Configuration` 注释（2026-08-17 时在 `:224-264`）|
| **落地后的名字** | 同一个变体头里的 `FMC_RESERVED_*` 段 |
| 对外 IO 定义 | 同上，`DOUT_*` / `DIN_*` / `AIN_*` 那几节 |

> ⚠️ **原来这里写的是 `fmc.c:153-193`，早就漂了** —— 那个范围现在落在 `MX_FMC_Init()` 的时序参数上，不是引脚表。**按函数名找，别按行号。**

```
PC0 · PD0,1,8,9,10,14,15 · PE0,1,7-15 · PF0-5,11-15 · PG0,1,2,4,5,8,15 · PH2,3
```

## 不做会怎样

用户写 `digitalWrite(PE7, HIGH)`（PE7 = `FMC_D4`）**能编译、能运行、把 SDRAM 数据线拽死**。

现象是"SDRAM 偶发读到垃圾"，**极难查** —— 因为看起来完全无关的两行代码之间没有任何提示。

## 这不是排布错误

那 39 个脚和本板所有对外 IO **一个都不撞**（逐脚核实见 [../design/HARDWARE-FACTS.md](../design/HARDWARE-FACTS.md)）。

**所以这是缺一道防护，不是排布错误。** 写方案时不要把它描述成设计冲突。

## 方案对比

| 方案 | 代价 | 效果 |
|---|---|---|
| 文档里列出这 39 个脚，写"不要碰" | 0 | 没人看 |
| **变体头里给它们起名 `FMC_RESERVED_*` 并注释** | 小 | ✅ **推荐** —— 用户在头文件里就能看见，可发现性最好 |
| `pinMode`/`digitalWrite` 里加运行时拦截 | 每次 IO 调用加开销 | ❌ 不像 Arduino 风格，否决 |

## 分步计划

| 步 | 做什么 |
|---|---|
| 1 | 从 `Core/Src/fmc.c` 的 `HAL_FMC_MspInit()` 注释块把 39 个脚和它们的 FMC 功能抄成一张表（⚠️ **按函数名找，别按行号** —— 见上面那条） |
| 2 | 在 `variant_PLC_H743.h` 里给每个起名 `FMC_RESERVED_<功能>`，并加一段注释说明后果 |
| 3 | 同步写进 [../design/HARDWARE-FACTS.md](../design/HARDWARE-FACTS.md) |
| 4 | ⚠️ 同步到 `$CORE_REPO` 并提交 —— `tools/check-core-sync.ps1` 会盯着 |

⚠️ **这是改共享 core**，按 [../design/ARCHITECTURE.md](../design/ARCHITECTURE.md) 属于分发给其他工程师的基础设施。改动本身很小，但要走 live → 验证 → repo 的流程。

## 验收 —— 2026-08-17 全部通过

- ✅ 39 个脚在头文件里都有名字和注释（16 数据 + 13 地址 + 10 控制）
- ✅ 编译一个用到 `digitalWrite(PE7, ...)` 的 sketch，**仍然能编译**
- ✅ 这个 sketch **进了仓库**（`host/variant_check/`）并接进 selfcheck（用例 **P4**）。⚠️ 它一开始只写在 `%TEMP%` 里 —— 那等于没有，见 [../process/WORKING-AGREEMENTS.md](../process/WORKING-AGREEMENTS.md)。**同一个错误当天犯了第二次**
- ✅ 负向对照：把 `FMC_RESERVED_PIN_COUNT` 改成 38，P4 报 `static assertion failed` 并退 1
- ✅ `tools/check-core-sync.ps1`（用例 P3）通过，live 与 repo 一致
- ✅ [../design/HARDWARE-FACTS.md](../design/HARDWARE-FACTS.md) 里有这张表

**验收是编译期断言，不是肉眼看。** sketch 在 `$TOOL/TestCase/host/variant_check/m4_fmc_pins/`，由 selfcheck 作为用例 **P4** 自动编译。三条 `static_assert`：

| 断言 | 挡住什么 |
|---|---|
| 列出的 39 个名字个数 == `FMC_RESERVED_PIN_COUNT` | 抄漏一个脚 |
| `FMC_RESERVED_D4 == PE7` | 抄错某一个脚的映射 |
| 每个名字都能求值成引脚号 | 名字拼错、宏没定义 |

**为什么加 `FMC_RESERVED_PIN_COUNT`**：这份清单是 `fmc.c` 的第二份副本，按 [../design/ARCHITECTURE.md](../design/ARCHITECTURE.md) 的规律，第二份副本必然漂移。这个常量让"表内部抄漏了一行"在编译期就被抓住。

⚠️ 但它**挡不住 `fmc.c` 单方面改脚** —— 那边改了、这边没改，`FMC_RESERVED_PIN_COUNT` 仍然自洽。那一层由 P2 的新锚点管，见下。

## 已否决

**运行时拦截。** 每次 IO 调用加开销，而且 Arduino 的心智模型里 `digitalWrite` 就是直通硬件的 —— 一个会拒绝执行的 `digitalWrite` 比一个会拽死 SDRAM 的更让人困惑。

## 跨仓比对 ✅ 同日补上

`fmc.c` 的引脚表和变体头的 `FMC_RESERVED_*` 是**两份副本**，而变体头里的编译期断言**只保证它自己内部自洽** —— `fmc.c` 单方面改了它一无所知，那时变体头会开始对用户**撒谎**（还说 PE7 是数据线，而 PE7 已经不是了）。**一张会撒谎的表比没有这张表更糟**，而 E6 的全部价值就是这张表可信。

已作为第 9 个锚点加进 `$TOOL/TestCase/tools/check-mirror-sync.ps1`（用例 **P2**）：两边都归一成 `功能=引脚` 的有序集合再比。

```
OK    FMC pin map (39 SDRAM pins)
```

**用负向对照验过它真的会响**（不是只看它打 OK）：把变体头里 `FMC_RESERVED_D4` 从 `PE7` 改成 `PE6`，脚本报

```
DIFF  FMC pin map (39 SDRAM pins)
        bootloader Core/Src/fmc.c            only here: D4=PE7
        core variants/.../variant_PLC_H743.h only here: D4=PE6
        (38 part(s) agree and are not shown)
```

退出码 1，改回去恢复 0。

> 顺带修了 `Compare-Anchor` 的一个显示缺陷：它把长值截断到 120 字符，而这个锚点有 39 项 —— 两边**都被截在差异之前**，打出来一模一样，看不出差在哪。现在长锚点改为只列不一致的项。**这个缺陷是我加了长锚点之后才暴露的，原来那 8 个都短。**
