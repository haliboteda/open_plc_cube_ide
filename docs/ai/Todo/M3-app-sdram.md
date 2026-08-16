# M3 · app 侧能不能用 SDRAM

**覆盖需求：[E5](../REQUIREMENTS.md)**

**状态：⬜ 卡在一个问题上 —— app 到底需不需要这 64MB？** 需求没确认之前不要动手。

## 是什么

板载 64MB 外部 SDRAM（AS4C32M16SB-7BIN），映射在 `0xC0000000`。

| | |
|---|---|
| **bootloader 侧** | 接收 CDC/以太网镜像的暂存区，校验通过后才写 flash。**已在用，是 SDRAM staging 的基础** |
| **app 侧** | **目前没有用途，也用不了** |

## 在哪找

| | |
|---|---|
| bootloader 初始化 | `Core/Src/main.c:201`（`MX_FMC_Init()`），实现和上电时序 `Core/Src/fmc.c:44-126` |
| bootloader 交权时关掉它 | `IAPServer/IAP_server.c:501` 的 `HAL_DeInit()` |
| core 侧 | `core:variants/STM32H7xx/H743/` 下 `.c/.cpp/.h` **全搜零命中**；`ldscript.ld:41-55` 的 `MEMORY` 块也没声明 SDRAM |

## 不做会怎样

app 完全用不了这 64MB。用户想跑大数据缓冲（数据记录、大点表、图像）就只能用片内 512K。

**这是产品能力问题，不是 bug。** 所以第一步是确认需求，不是写代码。

## 设计思路 —— 声明方式已经想清楚，照抄

| | 做法 | 理由 |
|---|---|---|
| **bootloader** | **裸地址** `(uint8_t*)0xC0000000`，**保持现状不动** | 一次性 staging；不希望链接器往里放任何东西；SDRAM 在 startup 时还没初始化，**声明它反而危险** |
| **app / core** | **写进 linker script，用 `(NOLOAD)` 段** | 用户会声明大数组，需要链接器做冲突检测和溢出报错。裸指针的话两个库都用 `0xC0000000` 会静默互踩 |

app 侧三个硬要求：

| | 要求 | 不这么做会怎样 |
|---|---|---|
| 1 | 段必须标 **`(NOLOAD)`** | 段会进 `.bin` —— 一个 1MB 数组让固件镜像涨 1MB |
| 2 | 段必须放在 **`_sbss` … `_ebss` 之外** | startup 的清零循环会扫到它。**清零发生在 FMC 初始化之前，往未初始化的 SDRAM 写 = 故障** |
| 3 | ⚠️ **只能是 BSS 风格，不能有初值** | `.data` 风格的段会被 startup 的拷贝循环在 FMC 之前写 |

**可抄的写法**：`core:variants/STM32H7xx/H743/DAISY_SEED.ld:74`（`SDRAM (xrw) : ORIGIN = 0xC0000000, LENGTH = 64M`）和 `:217-227`（`.sdram_bss (NOLOAD)`）。

⚠️ **那是上游 Electrosmith Daisy Seed 板子的脚本**，`core:boards.txt:50` 写死了 OPEN-PLC 用 `ldscript.ld`，构建里没引用它。只是一份可抄的写法，不是现成的配置。

## 分步计划

| 步 | 做什么 | 门槛 |
|---|---|---|
| 0 | **确认 app 是否真需要** | ⛔ 卡在这里 |
| 1 | core 里加 FMC 初始化 | ⚠️ **改共享 core**，按 [../../ARCHITECTURE.md](../../ARCHITECTURE.md) 是分发给其他工程师的基础设施，**要单独立项** |
| 2 | `ldscript.ld` 加 `SDRAM` MEMORY 区和 `.sdram_bss (NOLOAD)` 段 | 三个硬要求逐条核对 |
| 3 | 写用户文档 | 见下面那条代价 |
| 4 | 一个示例 sketch，声明大数组、memset、读写校验 | 就是验收用例 |

## ⚠️ 代价必须写进用户文档

**这些变量不是零初始化的。** 用户必须在 FMC 初始化后自己 `memset`。

不写清楚的话，这是一类极难查的间歇性 bug —— 数组里是上次掉电前的残留，看起来"大部分时候是对的"。

## 验收

用例还不存在。至少要有：

- 示例 sketch 能声明 1MB 数组、写满、读回校验一致
- **`.bin` 尺寸不因为那个数组变大**（验第 1 条硬要求）
- 冷启动后数组内容**不保证为零**（验的是文档说得对，不是代码错）
- SDRAM 自检失败时 app 的行为是可预期的，不是随机崩

## 已否决

无。这条还没到有方案可否决的阶段 —— 卡在需求确认上。
