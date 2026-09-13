# 01 · 14 个交权入口退出面板，能力改成会话参数

Type: task
Status: open（已定方案，等开工）

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
