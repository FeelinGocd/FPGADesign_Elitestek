# Ti60F225-DK 板载 FT4232HL 驱动问题排查记录（已解决）

板子: Ti60F225-DK，板载下载芯片 **FT4232HL**（USB VID:PID = `0403:6011`）
一根 Type-C 同时做 **供电 + JTAG + UART**。

---

## 一、故障现象

OpenOCD 报：
```
Error: libusb_open() failed with LIBUSB_ERROR_ACCESS
Error: no device found
Error: unable to open ftdi device with vid 0403, pid 6011, description 'Quad RS232-HS'
```

同时串口终端里找不到对应的 COM 口（可见的 8 个 COM 全是 `BthModem*` 蓝牙虚拟串口）。

## 二、根因

### 2.1 先解决 COM 口

复合父设备（`USB\VID_0403&PID_6011\5&28735835&0&1`）被 **libusbK** 驱动抢走了。
libusbK 直接接管**父设备**后，4 个子接口不再枚举 → Windows 一个 COM 口都不建。

> **教训**：用 Zadig 时只能选 `Quad RS232-HS (Interface N)`，
> **绝对不能选 `Quad RS232-HS (Composite Parent)`**。

把父设备驱动恢复成 `usbccgp` 后，MI_02 拿到 FTDIBUS → **COM5** 出现。

### 2.2 再解决 JTAG

用 libusb 日志回调抓到的原文：
```
libusb: debug [libusb_open] open 1.11
libusb: error [winusbx_open] could not open device
    \\?\USB#VID_0403&PID_6011&MI_03#6&2FB5A2C6&2&0003#{D4590DF9-...}
    (interface 3): [5] Access is denied.
libusb: debug [libusb_open] open 1.11 returns -3
```

**libusb 的 WinUSB 后端去开 MI_03 失败（ACCESS），导致整个 `libusb_open` 返回 -3。**

已验证 MI_03 本身完全正常：`CreateFile` 能开、`WinUsb_Initialize` 成功、
两个 OpenOCD（含易灵思 debugger 自带的）libusb 版本相同（1.0.29）都失败。
换成 **UsbDk 后端**（`libusb_set_option(ctx, LIBUSB_OPTION_USE_USBDK, 1)`）则**能成功打开** ——
证明是 libusb WinUSB 后端在多驱动复合设备上的缺陷，不是硬件/驱动问题。

### 2.3 关键规律

libusb 的枚举日志：
```
MI_00 → no DeviceInterfaceGUID registered   ← 跳过 ✅
MI_02 → no DeviceInterfaceGUID registered   ← 跳过 ✅
MI_01 → interface[1] = {C5C118BB-...}       ← 打开 ✅
MI_03 → interface[3] = {D4590DF9-...}       ← 打开 ❌ ACCESS
```

**绑 FTDI VCP 驱动（FTDIBUS）的接口不会注册 `DeviceInterfaceGUID`，libusb 直接跳过它们。**
而 Zadig 装的 libusbK / libusb-win32 / WinUSB 都会注册 GUID，libusb 就一定要去开。

FT4232HL 有 4 个通道，本板只用两个：

| 通道 | 接口 | 用途 | 应有的驱动 |
|---|---|---|---|
| A | MI_00 | **未使用** | FTDIBUS（无 GUID，被跳过） |
| B | **MI_01** | **JTAG** | **libusbK** |
| C | **MI_02** | **UART** | FTDIBUS → COM 口 |
| D | MI_03 | **未使用** | **不装驱动**（或 FTDIBUS） |

---

## 三、最终解决方案

### 目标状态

```
MI_00 → FTDIBUS  → COM19   无 DeviceInterfaceGUID   libusb 跳过
MI_01 → libusbK            有 DeviceInterfaceGUID   ← JTAG
MI_02 → FTDIBUS  → COM5    无 DeviceInterfaceGUID   libusb 跳过
MI_03 → 不装驱动            无 DeviceInterfaceGUID   libusb 跳过
复合父设备 → usbccgp
```

### 操作步骤

1. **设备管理器 → 「libusbK USB Devices」→ `Quad RS232-HS (Interface 1)`**
   —— 这个**保留不动**，是 JTAG 要用的。

2. **设备管理器 → 「libusb-win32 devices」→ `Quad RS232-HS (Interface 0)`**
   → 右键**卸载设备** → 拔插板子。
   Windows 会自动补装 FTDI VCP → 变成 `USB Serial Converter A` + COM19。

3. **设备管理器 → 「通用串行总线设备」→ `Quad RS232-HS (Interface 3)`**
   → 右键**卸载设备** → 拔插板子。
   （注意是「通用串行总线**设备**」，不是「通用串行总线**控制器**」）

4. 拔掉板子 Type-C，等 5 秒，重新插上。

### 验证结果

```
Info : clock speed 800 kHz
Info : JTAG tap: fpga_spinal.bridge tap/device found: 0x10660a79
       (mfg: 0x53c (Efinix Inc), part: 0x0660, ver: 0x1)
[fpga_spinal.cpu0] Target successfully examined.
Info : starting gdb server for fpga_spinal.cpu0 on 3333
```

**JTAG 和 UART 同时可用**（COM5 打开正常）。

---

## 四、OpenOCD 手工调用方法

```powershell
# 工作目录必须是 SoC 软件根（debug_ti.cfg 靠向上找 cpu0.yaml）
# E:\...\embedded_sw\SoC_RISCV32\software\standalone\project

& 'E:\FPGA\Efinity\EfinityRISCV\openocd\bin\openocd.exe' `
  -c "set bsp_loc {E:\...\embedded_sw\SoC_RISCV32}" `
  -f "E:\...\SoC_RISCV32\bsp\efinix\EfxSapphireSoc\openocd\ftdi_ti.cfg" `
  -f "E:\...\SoC_RISCV32\bsp\efinix\EfxSapphireSoc\openocd\debug_ti.cfg"
```

- `ftdi_ti.cfg`：`ftdi vid_pid 0x0403 0x6011` + `ftdi channel 1`（JTAG 走 Channel B / Interface 1）
- `debug_ti.cfg`：靠 `..` 向上 3 层找 `cpu0.yaml` 定位 `proj_loc`，找不到会报
  `cpuConfigFile /cpu0.yaml not found`

## 五、UART 参数

```
115200 / 8 / N / 1，无流控
FPGA 侧：system_uart_0_io_rxd = GPIOL_02 (R4)   FPGA 为 input
        system_uart_0_io_txd = GPIOR_28 (R14)  FPGA 为 output
```

## 六、排查中排除掉的可能性

| 检查项 | 结果 |
|---|---|
| `CreateFile` 开各接口路径 | ✅ 全部成功（含 `FILE_FLAG_OVERLAPPED`） |
| 同时持有父设备 + 4 接口句柄 | ✅ 无冲突 |
| `WinUsb_Initialize` on MI_03 | ✅ 成功，驱动正常 |
| 火绒（HipsDaemon/usysdiag）拦截 | ❌ 不是 |
| VMware USB Arbitration / UsbDk | ❌ 不是 |
| 重启电脑 | ❌ 无效 |
| 换另一个 OpenOCD（Efinity debugger 自带） | ❌ libusb 版本完全相同 |
