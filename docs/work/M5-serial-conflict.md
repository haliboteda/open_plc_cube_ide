# M5 · `Serial_Test` 和 `Serial4` 抢同一个 UART4 ✅ 2026-08-17 已修并实测

**覆盖需求：`$PROD/docs/STATUS.md`**

**改动**：`core:cores/arduino/main.cpp` 的
`HardwareSerial Serial_Test(PC_11, PC_10)` → `(PC_11_ALT1, PC_10_ALT1)`。
**`ALT1` 是全部的关键** —— 同样两个引脚，AF8 是 UART4，AF7 是 USART3。**线一根没动**，端子 C05/C06 和 bootloader 自己的 UART4 日志都不受影响。

## ⚠️ 后果比本文档原来写的严重得多

原文说的是"**静默掐掉接收**，而发送看起来一切正常"。**那是推断，实测否定了它。**

2026-08-17 实测，sketch 只要调一次 `Serial4.begin(115200)`：

```
** APP Mod ...
[?[B          ← [BOOT] 打到一半断掉，之后再无输出
```

**app 卡死 → UDP 发现不应答 → 以太网够不着 → IAPTool 报 `No response ... exiting`。板子只能靠 ST-Link 擦 app 区救回来。**

所以 E7 不是"排查体验"问题，是**用户一行普通代码就能让板子失联、且只有 ST-Link 能恢复**。修之前这条风险一直被低估。

> 这也是为什么"不动 + 写文档"那个兜底方案不成立：让用户"别用 Serial4"挡不住任何人，而代价是板子变砖。

## 是什么

两个 Arduino 串口对象解析到了同一个硬件外设：

| 对象 | 引脚 | 给谁用 | 外设 |
|---|---|---|---|
| `Serial_Test` | PC11 / PC10 | core 打诊断日志 | **UART4** |
| `Serial4` / `Serial` | PH13 / PH14 | 用户 sketch | **UART4** |

## 在哪找

- `core:cores/arduino/main.cpp:40` 的 `HardwareSerial Serial_Test(PC_11, PC_10)`
- 机制说明在 [../design/HARDWARE-FACTS.md](../design/HARDWARE-FACTS.md)

## 不做会怎样

机制（`uart_handlers[]` 每个外设只有一个槽位、最后一次 `begin()` 赢）在 [../design/HARDWARE-FACTS.md](../design/HARDWARE-FACTS.md)。

将来任何 `Serial4.begin()`（或把 USB 菜单切到 "CDC (no generic 'Serial')"）都会**静默掐掉 `Serial_Test` 的接收**，而发送看起来一切正常。

这是那种"能发不能收、查半天"的典型 —— 而且 USB 菜单那条路径用户根本不会想到和串口有关。

## 方案对比

| 方案 | 代价 | 风险 |
|---|---|---|
| **把 `Serial_Test` 挪到没人用的 USART3**：`Serial_Test(PC_11_ALT1, PC_10_ALT1)` | 需硬件验证 + **提交到共享 core** | ✅ **推荐，治本** |
| 不动，文档写"别用 Serial4" | 0 | 靠自觉，而且 USB 菜单那条路径想不到 |

## 分步计划

| 步 | 做什么 | 验什么 |
|---|---|---|
| 1 | 核实 PC11/PC10 的 ALT1 确实映射到 USART3，且 USART3 当前无人占用 | 查数据手册 + 全仓搜 `USART3` |
| 2 | 改 `core:cores/arduino/main.cpp:40` | 编译通过 |
| 3 | **上板验证收发都正常** | ⚠️ 这一步不能跳 —— 引脚复用是"看着对、实际不通"的重灾区 |
| 4 | 写一个同时用 `Serial_Test` 和 `Serial4` 的 sketch | 两个都能收能发 |
| 5 | 同步到 `$CORE_REPO` 并提交 | `tools/check-core-sync.ps1` 通过 |

## 验收 —— 2026-08-17

用例 **M5**，sketch 在 `$TOOL/TestCase/onboard/rs232/M5_SerialConflict/`，编排在 `tools/run-m5.ps1`。

- ✅ sketch 里 `Serial4.begin()` **之后**，`Serial_Test` 仍能**收**：5/5 字节回显
- ✅ **负向对照跑过**：同一个 sketch 打在**未修的 core** 上，板子挂死、连 banner 都出不来
- ✅ `tools/check-core-sync.ps1`（用例 P3）通过
- ⬜ USB 菜单切到 "CDC (no generic 'Serial')" 时同样成立 —— **没测**，见下

⚠️ **判据必须是"能收"**，因为原以为的故障模式是"发送看起来正常"。实测发现发送也死，但判据不用改：**能收是更强的条件**。

## ★ 两个当天踩到的坑

**1. `Serial` 不是 `Serial4`，拿 `Serial` 写的用例是假的。**

第一版 sketch 用的是 `Serial.begin()`，在**未修的 core** 上跑出了**干净的通过**。原因：当前 FQBN 是 `usb=CDCgen`，`WSerial.h` 的 `#if !defined(Serial)` 守卫让 `Serial` 保持为 USB CDC，**根本碰不到 UART4**。

**要不是先跑了基线，我会改完 core、看到绿灯、宣布修好 —— 而用例从头到尾没碰过那个缺陷。** 这就是"负向对照"不是形式主义的原因。

**2. 触发缺陷会让板子失联，别在没有 ST-Link 的时候试。**

未修 core + `Serial4.begin()` = app 挂死 + 以太网不通 = **IAP 救不回来**。恢复靠 `STM32_Programmer_CLI -c port=SWD mode=UR -e 1` 擦掉 app 扇区，让 bootloader 停下来并起以太网，再正常 IAP 烧。

## 还欠的

**USB 菜单切到 "CDC (no generic 'Serial')" 那条路径没测。** 那时 `Serial` 会退回成 `Serial4`（即 UART4），所以**用户连"我用的是 Serial 不是 Serial4"都不能作为安全理由**。修完之后这条路径应该也没问题（`Serial_Test` 已经不在 UART4 上了），但**没有实测**，别当它验过。

## 已否决

**"不动 + 写文档：别用 Serial4"。** 实测后彻底不成立 —— 代价不是"看不到日志"，是板子失联、要 ST-Link 才能恢复。靠自觉挡不住这种后果。
