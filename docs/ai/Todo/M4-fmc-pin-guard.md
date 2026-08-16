# M4 · FMC 引脚防护

**覆盖需求：[E6](../REQUIREMENTS.md)** —— 用户 `digitalWrite` 碰不到 FMC 占用的 39 个脚。

**成本：小。随时可做。**

## 是什么

`NUM_DIGITAL_PINS 140` —— 变体把 MCU 全部引脚都注册成可用的 Arduino 数字引脚。这是 STM32duino 的常规做法，让用户能 `digitalWrite(PXn, ...)` 访问任意引脚。

其中 39 个脚接的是 SDRAM。

## 在哪找

| | |
|---|---|
| 引脚总数 | `core:variants/STM32H7xx/H743/variant_PLC_H743.h:338` |
| FMC 的 39 脚清单 | `Core/Src/fmc.c:153-193`（`HAL_FMC_MspInit` 注释块，逐脚列了功能） |
| 对外 IO 定义 | `core:variants/.../variant_PLC_H743.h:194-225` |

```
PC0 · PD0,1,8,9,10,14,15 · PE0,1,7-15 · PF0-5,11-15 · PG0,1,2,4,5,8,15 · PH2,3
```

## 不做会怎样

用户写 `digitalWrite(PE7, HIGH)`（PE7 = `FMC_D4`）**能编译、能运行、把 SDRAM 数据线拽死**。

现象是"SDRAM 偶发读到垃圾"，**极难查** —— 因为看起来完全无关的两行代码之间没有任何提示。

## 这不是排布错误

这 39 个脚在 PCB 上**只连** AS4C32M16SB 那颗 SDRAM，和变体里所有对外 IO（DOUT×8 / DIN×8 / AIN×2 / AOUT×2 / RS232 / RS485）**一个都不撞**，连 BOOT0 的 PG9 都恰好夹在 PG8 和 PG15 中间空着。

**是缺一道防护，不是排布错误。** 写方案时不要把它描述成设计冲突。

## 方案对比

| 方案 | 代价 | 效果 |
|---|---|---|
| 文档里列出这 39 个脚，写"不要碰" | 0 | 没人看 |
| **变体头里给它们起名 `FMC_RESERVED_*` 并注释** | 小 | ✅ **推荐** —— 用户在头文件里就能看见，可发现性最好 |
| `pinMode`/`digitalWrite` 里加运行时拦截 | 每次 IO 调用加开销 | ❌ 不像 Arduino 风格，否决 |

## 分步计划

| 步 | 做什么 |
|---|---|
| 1 | 从 `Core/Src/fmc.c:153-193` 的注释块把 39 个脚和它们的 FMC 功能抄成一张表 |
| 2 | 在 `variant_PLC_H743.h` 里给每个起名 `FMC_RESERVED_<功能>`，并加一段注释说明后果 |
| 3 | 同步写进 [../../HARDWARE-FACTS.md](../../HARDWARE-FACTS.md) |
| 4 | ⚠️ 同步到 `$CORE_REPO` 并提交 —— `tools/check-core-sync.ps1` 会盯着 |

⚠️ **这是改共享 core**，按 [../../ARCHITECTURE.md](../../ARCHITECTURE.md) 属于分发给其他工程师的基础设施。改动本身很小，但要走 live → 验证 → repo 的流程。

## 验收

- 39 个脚在头文件里都有名字和注释
- 编译一个用到 `digitalWrite(PE7, ...)` 的 sketch，**仍然能编译** —— 这条是刻意的：起名不阻止用户，只让他看得见
- `tools/check-core-sync.ps1` 通过
- [../../HARDWARE-FACTS.md](../../HARDWARE-FACTS.md) 里有这张表

## 已否决

**运行时拦截。** 每次 IO 调用加开销，而且 Arduino 的心智模型里 `digitalWrite` 就是直通硬件的 —— 一个会拒绝执行的 `digitalWrite` 比一个会拽死 SDRAM 的更让人困惑。
