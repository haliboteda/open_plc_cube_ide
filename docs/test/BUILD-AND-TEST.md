# 构建与测试

路径变量（`$IDE`、`$CORE_LIVE`、`$TOOL` 等）的定义见 [../design/ARCHITECTURE.md#路径变量](../design/ARCHITECTURE.md#路径变量)。装什么、配什么在 [../../CLAUDE.md](../../CLAUDE.md)。

> **写路径一律用 `/`。** Windows 的 .NET 路径 API 全都接受正斜杠，Linux 不接受反斜杠 —— `/` 是唯一两边都对的写法。可执行文件后缀（`.exe` 或空）不要写死，脚本里用 `_common.ps1` 的 `$EXE`。

## 命令行重编 app（不用开 Arduino IDE）

IDE 自带的 CLI 在 `$IDE/resources/app/lib/backend/resources/arduino-cli`（Windows 上带 `.exe`）—— **不在 PATH 上**，配置在 `$TOOL/TestCase/config/machine.ps1` 的 `$ARDUINO_CLI`。

```powershell
& $ARDUINO_CLI compile --warnings all `
  --config-file $ARDUINO_CLI_CONFIG `
  --fqbn "OpenPLC_Alpha:stm32:OPEN-PLC:pnum=PLC_H743,usb=CDCgen,xusb=FS,upload_method=cdcMethod,knxrole=dual_device,downgrade=refuse" `
  --build-path "<IDE 的 sketch 缓存目录>/<hash>" `
  "$TOOL/TestCase/onboard/rs232/SerialPort"
```

`$ARDUINO_CLI_CONFIG` 默认是 `$HOME/.arduinoIDE/arduino-cli.yaml`，两个平台同名。

- **`--config-file` 必须带**，否则找不到 Arduino15 packages 和用户库目录
- **`--warnings all` 必须带** —— arduino-cli 默认 `-w`，什么警告都不打；IDE 里那个 All 档是 IDE 自己的设置，CLI 不继承
- FQBN 和 sketch 路径能从缓存目录的 `build.options.json` 读出来，**别硬猜**（2026-08-15 起多了 `downgrade` 菜单项，旧 FQBN 会缺这一段）
- `--build-path` 指向 IDE 自己用的缓存目录，这样 `.bin` 落在 IAPTool 一直用的老位置

### 签名和版本是自动的，不要手工做

- 签名逻辑在 IAPTool 里（`sign.go` 的 `signBinFile`），`IAPTool cdc/ether` 在内存里直接签，**不需要 `.sig` 文件**
- 版本由 `boards.txt` 的 `OPEN-PLC.build.fw_version` 单一决定；编译后钩子调 `IAPTool version` 生成 `<sketch>.ino.version`，IAPTool 用它做降级检查

> 早期的 `sign_firmware.sh` / `sign_and_flash_cdc.sh` **已删除**，别再找了。

## 其他构建

| 目标 | 命令 |
|---|---|
| IAPTool（三平台） | `$TOOL/compile_tool.sh` —— 别手搓 `go build`，输出布局是约定好的 |
| TestCase（本机） | `cd $TOOL && go build -o Output/<GOOS>/TestCase ./TestCase`。脚本里用 `_common.ps1` 的 `Get-GoBin "TestCase"` 找它，别自己拼路径 |
| network_discovery（四平台） | `$CORE_LIVE/tools/discovery/build.sh`（或 `build.ps1`）—— **Arduino IDE 开着会因文件占用失败** |
| bootloader | STM32CubeIDE。（cmake 路径存在，但 cmake/ninja 通常不在 PATH 上） |

单文件语法检查 bootloader 的改动，不必开 CubeIDE：用 core 包里的
`$A15/packages/OpenPLC_Alpha/tools/xpack-arm-none-eabi-gcc/*/bin/arm-none-eabi-gcc`（Windows 上带 `.exe`），加上 `-fsyntax-only`、
`-mcpu=cortex-m7 -mthumb -DSTM32H743xx -DUSE_HAL_DRIVER` 和工程的 `-I` 路径。
能在烧板之前挡住低级错误。

## 测试分工

**`IAPTool` 只管上传烧写，不加任何测试专用功能。**

所有为验证设备行为而存在的东西放 `$TOOL/TestCase/`（同一个 Go module 的子目录），按用例编号分文件，`README.md` 维护用例表 / 前置条件 / 判据 / 未覆盖清单。

**三条原则在 [CASE-DESIGNS.md](CASE-DESIGNS.md)**，不在这里重复。

### 用例总览 → 不在这里

**全部用例、它们覆盖哪条需求、最近一次跑出什么结果，在 [../STATUS.md](../STATUS.md)。**

> ⚠️ **这里以前有一张自己的总览表，2026-08-22 删掉了。** 它只列了 T / S1 / N 三组，而那时实际已经有 OW1-3、S2、S3、S4a/S4b、G1、SD1、M5、BG1、AU1、DG1 和全部主机用例 —— **一份重复的表最后总是变成过期的那一份**，而它看起来和权威的那份一模一样。这正是这套文档反复踩的同一个坑。

**反向用例和正向用例一样重要**（T1b「不该踢时没踢」、T4「拒绝状态没卡死」）。它们防的是只在现场暴露的毛病，而且最容易在测试里被漏掉 —— 详见 [CASE-DESIGNS.md](CASE-DESIGNS.md) 的三条原则。

## 排查间歇性故障的方法

2026-08-15 那个"UDP 发现偶发无应答"的经验，四条可复用：

**1. 短测抓不到周期性故障。** N1/N2/N3（几秒到一分钟）反复跑全是干净的；同一个问题在 25 分钟浸泡里稳定复现 18 次。**抽查式测试对"每 30 秒一次"这类故障基本无效** —— N4 就是为此存在的。

**2. 静默的 `return` 会让问题无限期隐藏。** 那条 UDP 回复路径有三个失败分支全部不打日志（限流拒绝、pbuf 耗尽、`udp_sendto` 返回值被丢弃）。加上诊断后一轮就定案。**任何丢包 / 拒绝路径都要能说出自己为什么。**

**3. 改变探测参数，区分"被测系统"和"测量工具"。** 把探测间隔从 2.5s 改成 4s，失败的**轮数**变了而**墙钟周期（30s）不变** —— 这一步排除了"我的循环被锁相"，把周期归属到设备侧。

**4. 用不受影响的同类流量做对照。** ICMP 和 UDP 一样无重传、走同一条路径、同一个 lwIP 收包流程。ICMP 300/300 零丢包，一步排除网络路径。
反过来：**"TCP 正常"不能证明 UDP 有问题** —— TCP 会重传，本来就藏得住单个丢包。

> 那次排查里推理错了三次，每次都靠加测量纠正：判断"app 没拿到 IP"（其实一直在 192.168.0.7，ARP 表里被当成无关设备）；判断"回包迟到超时被丢"（其实是设备主动拒绝）；判断"锁没被验证"（其实验证了，看错了日志文件 —— 格式和代码里 `logf()` 对不上时就该发现）。
>
> 最终根因也不在固件，而是**我们自己的两个工具在抢同一个配额**。**先怀疑"自己人打架"，再怀疑对方有 bug。**

## 排查时容易看错的日志位置

| 想看什么 | 真实位置 |
|---|---|
| network_discovery 的日志 | 系统临时目录下的 `network_discovery.log`（带 `[HH:MM:SS.mmm]` 时间戳）。Windows 是 `%TEMP%`，Linux 是 `$TMPDIR` 或 `/tmp` |
| ~~network_discovery 的日志~~ | ~~`tools/discovery/bin/<平台>/err.log`~~ —— 那是 IDE 抓的 stderr，格式不同，**不是这个** |
| bootloader / app 的 printf | SWO/ITM **和** UART4 → RS232 端子 C05/C06 |
