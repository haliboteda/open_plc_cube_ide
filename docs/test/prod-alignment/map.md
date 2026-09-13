# 地图：产测指导文件 V0.1 与固件/上位机对齐

## Destination

《OPENPLC 生产测试指导文件 V0.1》（`Hardware/OPENPLC生产测试指导文件_V0.1_20260904.docx`）里每一条「软件需求」都有明确归属：**现有能力已覆盖** / **我方要补** / **建议改文档**。

终点是两份东西：**给硬件工程师的审核意见**（哪些增删改）+ **我方的补齐清单**。

## Notes

**域**：产线测试。固件 `$BOOT/TestCase/porttool/` + `$BOOT/TestCase/<模块>/`，上位机 `$TOOL/internal/` 下的 `pt…` 各包，判据 `$TOOL/TestCase/TEST-CASES.md` 与 `plans/*.json`。

**这张图允许执行**：审核意见和补齐都是产出物，不只是决策。

**每个会话先读**（这几条已拍板，不要重开）：

| | |
|---|---|
| `DECISIONS.md` 22 | 判定一律在上位机，固件只吐原始值 |
| `DECISIONS.md` 35 | DIN 导轨扩展口不测 |
| `DECISIONS.md` 37 | 测试方式 = 单次 / 持续（选时长），`soak` 端口已删 |
| `DECISIONS.md` 38 | **上位机是监工**：板子只干活并把执行日志全发回来 |
| `DECISIONS.md` 39 | **没有陪测板、没有外接仪器**，板子自证 + log 上来；板子测不了任何电压电流 |
| `DECISIONS.md` 40 | 14 个交权入口全部退出面板，能力改成会话参数 |
| `DECISIONS.md` 41 | **测试的每一项都是上位机配置项**：分类 + 说明 + 用法 + 互斥。总表 [PROD-CONFIG-ITEMS.md](../PROD-CONFIG-ITEMS.md) |

**权威链**：硬件事实只能出自 `Hardware/`（原理图、netlist、GPIO 分配表、数据手册）。固件和面板都是抄件。

## Decisions so far

- [没有陪测板和外接仪器时产测的边界](issues/00-scope-from-user.md)：板子自证的全做成会话/`pt.run`，板子没通路的（功耗、3V3/5V/5V_EXT、高边电流、继电器电流）列为人工项，硬件自己跳线量。已落 `DECISIONS.md` 39。
- [交权 14 个入口全部退出](issues/01-handover-retreat.md)：`can.scope` 后两步 = 会话的 `extloop`/`normal`；`can.echo` = 会话加一个 `mode=echo`；KNX 逐帧 `raw=/inv=/crc=` 会话已有。已落 `DECISIONS.md` 40。
- [配置项总表](../PROD-CONFIG-ITEMS.md)：九个分类、每项的说明与用法、**十四条互斥关系及其出处**。已落 `DECISIONS.md` 41；落到面板上是 [配置项落进面板](issues/07-config-items-in-panel.md)。
- [五个通信口的判据形状](issues/03-comm-criteria-shape.md)：`eth`/`usb` 留速率；`can`/`rs485`/`knx` 改成**帧数 + 丢帧率 + 总线错误计数器归零**。已落 `DECISIONS.md` 42。⚠️ 「异常可恢复」仍开着。
- [校准先算后存](issues/05-calibration-fit.md)：系数由**上位机**拟合，先落在测试报告里，板子不存；「存哪」仍挂在 `ISS-C1`。

## Not yet specified

- **所有限值** —— 文档里从功耗到模拟精度全是 TBD，等硬件/质量签。判据文件的形状已经有了（`plans/*.json`），缺的是数。
- **失败码体系** —— 文档 2.6 要求每站记「结果和失败码」。现在 `ptreport` 只有通过/失败/超时三态，没有码。
- **跳测的权限与电子签署** —— 文档 3.10 要求，现在面板没有任何权限概念。
- **Golden Sample / 治具班前验证** —— 文档 R10、6.1 都要求，形状没定。
- **老化的温箱与 45±5 °C** —— 谁控温、温度怎么进日志。
- **节拍** —— 要等测试项固定下来才谈得上测。

## Out of scope

- **SN / 标签 / 过站 / 装箱 / MES 追溯**（文档工站 3/5/7/11/12 与 2.6）—— 用户 2026-09-13：**「sn 等内容优先级不高，暂记，当下不做」**。文档自己也把它列成独立的「系统管控软件」，和「功能测试软件」是两个条目。
- **外接仪器驱动**（电压电流模块、信号发生仪、陪测板、JLINK 读状态、摄像头识别安规结果）—— `DECISIONS.md` 39：没有实物和型号，写了也验不了。
- **Hutschienenverbinder 扩展口环回** —— `DECISIONS.md` 35 + 39。
- **SMT / 分板 / 安规 的非软件部分** —— 不是软件的事。
- **离线写程工站的烧录器软件** —— 第三方设备自带。
