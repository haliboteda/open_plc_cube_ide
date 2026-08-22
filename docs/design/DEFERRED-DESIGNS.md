# 设计过但推迟的方案

这里的东西**都已经想清楚了，是主动推迟的，不是遗漏**。有人问"为什么不做 X"时看这里。

## 产线单板密钥

**现状**：`device_key = HMAC-SHA256(FixedPassword, UID)`（三边的 `iap_keyderive.c` / `.go`）。这是**已知的占位方案**，不是产线设计。

**已商定的产线方案（未实现）**：

- 每台设备在一次性"预配"步骤中**在板上生成真随机 32 字节密钥**（需要启用 STM32 HAL_RNG，目前没配），存到 bootloader 和 app 都能读的保留 flash 区
- PC 工具在预配时**一次性读取**该密钥，存进本地加密库（`uid → key`），取代今天的即时派生
- `iap_keyderive_get_device_key()` 应当**运行时自动判别**（从共享 flash 读"这台设备预配过吗"标志），而不是编译期二选一；没预配的设备回退到密码派生方案

**明确否决的做法**：用 Arduino IDE 的 Tools 菜单在编译期选择密钥方案。
**原因**：Arduino 构建只碰 App，Bootloader 是菜单够不到的独立 CubeIDE 工程 —— App 和 Bootloader 可能被编译成不同方案，然后**每次握手都静默失败**。
（菜单用于正交的"严格模式"开关 —— 未预配就完全拒绝认证 —— 是可以的，但不能用来选方案本身。）

**为什么推迟**：工作量大（RNG、flash 存储、PC 侧加密密钥库、预配协议）。先用固定密码 + UID 往前走。

> ⚠️ **2026-08-16：这条可能整个不需要做了。** [OWNERSHIP.md](OWNERSHIP.md) 的证书方案里 `caps` 含 `session` —— 会话认证改成非对称（板子发 nonce、工具用叶私钥签、板子用证书里的叶公钥验）之后，**板子里一个秘密都没有**，不需要预配、不需要 RNG、不需要 PC 侧密钥库，固定密码那套三边同步也一起消失。
> **决定证书方案之前不要动手做这条**，否则会做出两套互不认识的密钥机制。

相关：[ARCHITECTURE.md](ARCHITECTURE.md) 里的固定密码单一来源机制 —— 产线方案落地后整套机制就不需要了。

## 故障自愈

**当前行为**：`Core/Src/stm32h7xx_it.c` 的 `IT_Fault_Report()` 打印完故障报告后停在 `while (1) {}`。**不要改。**

**2026-08-12 决定**：自愈方案已设计过，但**优先级排到最后**，等其他功能全部处理完再回来做。

### 裸的自愈是不可接受的

把 `while(1)` 直接换成 `HAL_NVIC_SystemReset()` 会**同时丢掉现场证据、又没有终止条件** —— 确定性故障会变成无限复位循环，继电器反复抖动。

成立的方案必须三件事齐备：

1. **故障记录先落 SRAM4 空闲区**（`0x38000010`，仅剩 16 字节）—— 写 RAM 不会引发二次故障；下次启动再由 bootloader 在正常上下文里写进 flash 事件日志。
   ⚠️ **不要在 fault handler 里直接写 flash** —— fault 可能正好发生在写 flash 期间，二次故障就是锁死。
2. **连续故障计数**，在 `server_jump_to_app()` 跳转前最后一步清零。
3. **数到 3 就不再复位**，强制 `IAP_ALL` 停在 bootloader（CDC + 以太网全开），现场还能被 IAPTool 重刷救回。

### IWDG 兜底这条路是堵的

H7 的 IWDG 一旦启动**只有上电复位能停**，会跟着跳进用户 app，要求每个 sketch 都喂狗 —— 违反[「设计不能限制用户的 app」](../process/WORKING-AGREEMENTS.md)。

相关：[HARDWARE-FACTS.md](HARDWARE-FACTS.md) 的 SRAM4 no-init 机制。

## Arduino 发现声明形式

`platform.txt` 现在用的是：

```
pluggable_discovery.network_discovery.pattern.windows="{runtime.platform.path}/tools/discovery/bin/windows_amd64/network_discovery.exe"
```

官方文档对这种 `.pattern` 形式有明确警告：

> "We strongly recommend using this syntax only for development purposes and **not on released platforms**."

正式发布应改用 `pluggable_discovery.required=VENDOR:NAME` + 在 package index 里声明 `discoveryDependencies`。

（顺带确认：`.pattern` **支持传参**，官方例子 `pluggable_discovery.teensy.pattern="...\teensy_ports" -J` 就带参数。）

**未处理。**
