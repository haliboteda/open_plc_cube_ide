# M6 · 补齐没测到的东西

**覆盖需求：[C5](../REQUIREMENTS.md) [C6](../REQUIREMENTS.md) [E1](../REQUIREMENTS.md) [E8](../REQUIREMENTS.md)**

**这是性价比最高的一项。** 它不加任何功能，只把 [REQUIREMENTS.md](../REQUIREMENTS.md) 里的 🟡 变成 ✅ 或者变成红字。

## 为什么排第一

🟡 的意思是"功能在跑，但没有任何东西能证明它还在跑"。这比 ⬜（没做）**更危险** —— 没做的东西没人依赖，🟡 的东西所有人都当它是好的。

2026-08-17 刚有一个现成的例子：bootloader 的 VBAT witness 和 app 的 nonce 计数器都占 `DR2`，**app 每次经过 bootloader 就重复发放同一批 nonce**，重放保护只剩 `tick` 在撑。这个缺陷存在了两天，是靠肉眼看串口日志发现的 —— **AU1 那个用例能直接抓到它**。

## 清单

按性价比排序。骨架已经在 [../TEST-PLAN.md](../TEST-PLAN.md) 写全了，这里只讲取舍。

| 用例 | 覆盖 | 要板子 | 成本 | 备注 |
|---|---|---|---|---|
| **DG1** 降级被拦下 | C6 | ❌ **不要** | 很低 | fakeboard 已经能答 `getversion`，加两个用例即可 |
| **AU1** nonce 跨掉电不重复 | C5 | ✅ | 低 | 要人工断电一次 |
| **S2** 密钥不匹配 | C2 | ✅ | 低 | 和 S1 同一套脚手架 |
| **S3** 纯启动期签名失败 | C3 | ✅ + ST-Link | 低 | 用 ST-Link 改一个字节 |
| **S4a** 传输中拔电 | E8 C4 | ✅ + 人工断电 | 中 | 窗口宽，好命中 |
| **S4b** 擦写阶段拔电 | E8 | ✅ + 人工断电 | 高 | ⚠️ 窗口只剩几秒 |
| **M3** 两块板 MAC 不同 | E1 | ⛔ **第二块板** | — | 阻塞。拿到板子的第一件事 |

## 建议做法

**先做 DG1** —— 它连板子都不要，纯粹加在已有的 `host/fakeboard/` 上，半小时的事。做完 selfcheck 里就多一组自动断言。

**然后 AU1 + S2 + S3 攒一趟上板** —— 三个都要板子，别为单条跑一趟。S3 顺带需要 ST-Link，正好和烧写在一起做。

**S4a/S4b 单独安排。** 它们要人工在特定时刻断电，节奏和自动化用例完全不同，混在一起跑会互相干扰（S4b 跑完板子处于 app 无效状态）。

**M3 不用等** —— 产线检查单里那一条（`acceptance/checklist.md` C3）现在就能写，只是执行不了。**要留 MAC 记录，否则"不重复"无从判起。**

## 顺带能收掉的两笔债

| 债 | 在哪 | 做法 |
|---|---|---|
| `RELEASE-NOTES.md:48` 说 bootloader 报 `0.1.2` | 2026-08-17 已修，条目没删 | `tools/check-version-sync.ps1` 已经会警告它。删掉那条 |
| H2（主机 C 单测）在本机跳过 | 没装 gcc/clang | 装一个。selfcheck 会自动从 SKIP 变 PASS |

⚠️ **stale 的 known-issue 比没有 known-issue 更糟** —— 下一个人会照着它去查一个已经不存在的问题。

## 验收

- `tools/selfcheck.ps1` 里 DG1 出现并通过
- [REQUIREMENTS.md](../REQUIREMENTS.md) 的 C5 / C6 从 🟡 变成 ✅，并能指向具体用例
- [../TEST-PLAN.md](../TEST-PLAN.md) 每条的「实际结果」填上日期和数字，**失败也填**
