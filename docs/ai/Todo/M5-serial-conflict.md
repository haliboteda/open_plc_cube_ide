# M5 · `Serial_Test` 和 `Serial4` 抢同一个 UART4

**覆盖需求：[E7](../REQUIREMENTS.md)** —— core 的诊断串口不会被用户 sketch 静默掐掉。

**成本：小，但需要硬件验证 + 改共享 core。**

## 是什么

两个 Arduino 串口对象解析到了同一个硬件外设：

| 对象 | 引脚 | 给谁用 | 外设 |
|---|---|---|---|
| `Serial_Test` | PC11 / PC10 | core 打诊断日志 | **UART4** |
| `Serial4` / `Serial` | PH13 / PH14 | 用户 sketch | **UART4** |

## 在哪找

- `core:cores/arduino/main.cpp:40` 的 `HardwareSerial Serial_Test(PC_11, PC_10)`
- 机制说明在 [../../HARDWARE-FACTS.md](../../HARDWARE-FACTS.md)

## 不做会怎样

`uart_handlers[]` 每个外设只有一个槽位，**最后一次 `begin()` 赢**。

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

## 验收

新用例，做之前先去 [../TEST-PLAN.md](../TEST-PLAN.md) 补骨架：

- sketch 里 `Serial4.begin()` **之后**，`Serial_Test` 仍能**收**（不只是能发）
- USB 菜单切到 "CDC (no generic 'Serial')" 时同样成立
- `tools/check-core-sync.ps1` 通过

⚠️ **判据必须是"能收"**。当前的故障模式恰恰是发送看起来正常 —— 只测发送会得到一个假通过。

## 已否决

暂无。"不动 + 写文档"这条留着作为兜底，但它不解决 USB 菜单那条隐蔽路径。
