# M6 · 补齐没测到的东西

**覆盖需求：[C5](../STATUS.md) [C6](../STATUS.md) [E1](../STATUS.md) [E8](../STATUS.md)**

**这是性价比最高的一项。** 它不加任何功能，只把 [../STATUS.md](../STATUS.md) 里的 🟡 变成 ✅ 或者变成红字。

## 为什么排第一

🟡 的意思是"功能在跑，但没有任何东西能证明它还在跑"。这比 ⬜（没做）**更危险** —— 没做的东西没人依赖，🟡 的东西所有人都当它是好的。

2026-08-17 刚有一个现成的例子：bootloader 的 VBAT witness 和 app 的 nonce 计数器都占 `DR2`，**app 每次经过 bootloader 就重复发放同一批 nonce**，重放保护只剩 `tick` 在撑。这个缺陷存在了两天，是靠肉眼看串口日志发现的 —— **AU1 那个用例能直接抓到它**。

## 清单

按性价比排序。骨架已经在 [../test/CASE-DESIGNS.md](../test/CASE-DESIGNS.md) 写全了，这里只讲取舍。

| 用例 | 覆盖 | 要板子 | 成本 | 状态 |
|---|---|---|---|---|
| **S4a** 传输中拔电 | E8 C4 | ✅ + 人工断电 | 中 | ⬜ 窗口宽，好命中。🔴 **卡在 SDRAM D1 线** |
| **S4b** 擦写阶段拔电 | E8 | ✅ + 人工断电 | 高 | ⬜ ⚠️ 窗口只剩几秒。🔴 同上 |
| **M3** 两块板 MAC 不同 | E1 | ⛔ **第二块板** | — | ⬜ 阻塞。拿到板子的第一件事 |

**已经做完的四条**（DG1、AU1、S2、S3）不再列在这里 —— 结果在 [../test/MEASUREMENTS.md](../test/MEASUREMENTS.md)，状态在 [../STATUS.md](../STATUS.md)。

## 做完的那四条留下的东西

**DG1 比原计划的"两个用例"多做了三个**，理由见 [../test/CASE-DESIGNS.md](../test/CASE-DESIGNS.md) 的 DG1 段：光断言工具打了什么，挡不住"打印了拒绝然后照样上传"。

**AU1 + S2 一趟上板做完**（2026-08-17）。

**S3 同一天也做完了。** 起初卡在"这台机器上没有任何已签名的 app 镜像"，解法是现编译一个：

1. 编译 `$TOOL/TestTool/onboard/rs232/SerialPort/`，按 [../test/BUILD-AND-TEST.md](../test/BUILD-AND-TEST.md) 的 FQBN（arduino-cli 的路径已进 `config/machine.ps1` 的 `$ARDUINO_CLI`）
2. `<sketch>.ino.bin` 和 `.version` 都由编译自动产出（版本 66304 = 0.1.3）
3. **先用它正常烧一次并确认板子起得来**，再动 ST-Link —— `run-s3.ps1` 把这一步做成了脚本的第 1/4 步，恢复路径不通就拒绝往下走

⚠️ 顺序不能反。**先证明恢复路径是通的，再去破坏。** 反过来做，一旦恢复镜像本身有问题，板子就卡在起不来 app 的状态，而你手上没有任何已知可用的东西。

**S4a/S4b 单独安排。** 它们要人工在特定时刻断电，节奏和自动化用例完全不同，混在一起跑会互相干扰（S4b 跑完板子处于 app 无效状态）。

**M3 不用等** —— 产线检查单里那一条（`acceptance/checklist.md` C3）现在就能写，只是执行不了。**要留 MAC 记录，否则"不重复"无从判起。**

## 顺带收掉的两笔债，都留下了一条教训

**装上 gcc 之后 H2 第一次真跑，发现它早就编不过了** —— stub 缺 `RTC_BKP_DR3`，SKIP 把这事盖了两天。**一个长期 SKIP 的检查要当成"状态未知"**，不能当成"缺一个环境就能补上"。完整经过在 [../test/COVERAGE-GAPS.md](../test/COVERAGE-GAPS.md)。

**`RELEASE-NOTES.md` 那条过期的版本号**是"过期结论"这类缺陷的又一个例子。⚠️ **stale 的 known-issue 比没有 known-issue 更糟** —— 下一个人会照着它去查一个已经不存在的问题。这一类缺陷 2026-08-22 才第一次有东西管：[../STATUS.md](../STATUS.md) 的「证据还成立吗」列。

## 验收

- ✅ selfcheck 里 DG1 出现并通过（2026-08-17）
- ✅ [../STATUS.md](../STATUS.md) 的 **C5 和 C6 都已从 🟡 变成 ✅**，分别指向 AU1 和 DG1；C2 另外多了 S2
- ✅ [../test/MEASUREMENTS.md](../test/MEASUREMENTS.md) 里 DG1 / S2 / AU1 的结果都填了日期和数字

**这份还剩什么：** S4a/S4b（要人工卡时机断电，🔴 卡在 SDRAM D1）、M3（⛔ 等第二块板）。

**S4b 现在好命中多了**：2026-08-17 无意中撞出来一次 —— **`IAPTool` 退出 ≠ 升级完成**，它送完最后一个字节就退，板子此时才开始擦写，有好几秒。等它一退出就动手即可，不用掐秒表。详见 [../test/CASE-DESIGNS.md](../test/CASE-DESIGNS.md) 的 S4b 段。
