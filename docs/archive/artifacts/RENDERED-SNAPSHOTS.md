# 配图版设计说明

六份带图的说明页，讲的都是密钥和信任模型 —— 那部分光靠文字很难讲清。

**源文件本来只存在于 `%TEMP%`**，换台电脑就没了；2026-08-17 归档到这里，同时把发布 URL 一并记下。

## ⚠️ 这些不是事实来源

> **和 `docs/` 打架时，一律以 `docs/` 为准。**

它们是某个时刻的**渲染快照**，不会随代码更新。归档是为了能改能重发，不是为了被引用。**不要照着它们排查问题**，也不要在别处引用它们的结论。

⚠️ **里面的数字是渲染那天的，不要回填。** 一份被改过数字的快照既不是当时的真相、也不是现在的，比两边都不是更糟。当前值去 [../../test/MEASUREMENTS.md](../../test/MEASUREMENTS.md)。

| 文件 | 讲什么 | 权威文本在 | 发布于 | 状态 |
|---|---|---|---|---|
| [owner-slot.html](owner-slot.html) | 板子归谁：owner 槽、TOFU、Setup/User Mode | [../../design/OWNERSHIP.md](../../design/OWNERSHIP.md) | [89bd5218](https://claude.ai/code/artifact/89bd5218-b53d-4e1b-a570-d10d540971c7) | 2026-08-16，最新 |
| [key-models-cn.html](key-models-cn.html) | 一层还是两层：单密钥 vs 授权链的取舍 | [../../design/OWNERSHIP.md](../../design/OWNERSHIP.md) | [1f07b7ba](https://claude.ai/code/artifact/1f07b7ba-1591-4490-a66f-e88629c1f2c4) | 2026-08-13 |
| [delegation-chain.html](delegation-chain.html) | The Delegation Chain：根签叶、叶签固件 | [../../work/M2-cert-chain.md](../../work/M2-cert-chain.md) | [3fe7a857](https://claude.ai/code/artifact/3fe7a857-d23e-4de3-bcdb-25cccf4f9bae) | 2026-08-13 |
| [final-plan-cn.html](final-plan-cn.html) | 方案 B 实施计划 | [../../work/M1-owner-slot.md](../../work/M1-owner-slot.md) | [8df5730b](https://claude.ai/code/artifact/8df5730b-3530-4c5c-a660-9cbd01609214) | 2026-08-13，⚠️ **计划已演进，以 M1 为准** |
| [iap-keys.html](iap-keys.html) | Two Keys, Two Jobs：占位密码 vs 签名公钥 | `$PROD/docs/design/ARCHITECTURE.md` | [0a443a27](https://claude.ai/code/artifact/0a443a27-150d-4697-b43e-3696cf65dfc1) | 2026-08-13 |
| [iap-key-flow.html](iap-key-flow.html) | 设备密钥的存放位置与挑战-响应流程 | `$PROD/docs/design/ARCHITECTURE.md`、`IAPServer/iap_auth.c` | [34a57a78](https://claude.ai/code/artifact/34a57a78-38c8-4c9c-908c-19dbef0f4658) | 2026-08-07，最旧 |

## 要更新其中一份

改本地文件，然后用同一个 URL 重新发布 —— **不要新建**，否则链接会分叉，而 [../../design/OWNERSHIP.md](../../design/OWNERSHIP.md) 里引的是旧的那个。
