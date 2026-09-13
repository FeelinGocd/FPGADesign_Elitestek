# SoC_LED 接口记录

> 状态：草稿（取自当前工程，与 RTL / SoC IP / peri 一致）
> 依据文件：
>   - 有效工程：project/03_SoC_LED/（Efinity 工程 project/project.xml；SoC IP project/ip/SoC_RISCV32/；源码 RTL/；固件 project/embedded_sw/SoC_RISCV32/）
>   - SoC 配置：project/ip/SoC_RISCV32/settings.json（efx_soc 3.4.1）
>   - 引脚：project/project.peri.xml（Interface Designer）+ src/example/01_LED 已验证引脚表
>   - 芯片手册：docs/芯片手册/ti60-ds-v3.8芯片手册.pdf（Table 73/74：Dual-Purpose Configuration Pins）
> 归档：先放本目录（候选 docs/spec）

---

## 1. 工程概览

Sapphire RISC-V → GPIO 控制器 → 点亮板载 LED；另验证 SoC 用户 APB 窗口（0xf8100000）。基于官方 09_co_debug_demo（SoC+DDR3+co-debug）改造，SoC 已用 IP Manager 重新生成（实例名 SoC_RISCV32）。

| 资源 | 值 |
|---|---|
| CPU | VexRiscv RV32IM + Zicsr/Zifence，单核 |
| CPU 时钟 | 100MHz（io_systemClk） |
| 内存 | 片上 RAM 4KB @0xf9000000；DDR3 @0x1000 起（256MB，程序跑 DDR） |
| 调试 | co-debug（hard JTAG，SOFT_TAP=0），DEBUG=1，CO_DEBUG=1 |
| SoC 名 | SoC_RISCV32 |

---

## 2. SoC 外设地址与寄存器（C 侧用）

| 外设 | 基址 | 寄存器偏移 | 中断（PLIC） |
|---|---|---|---|
| GPIO0 | 0xf8015000 | INPUT 0x00 / OUTPUT 0x04 / OUTPUT_ENABLE 0x08 / 中断 0x20~0x2C | 12 / 13 |
| GPIO1 | 0xf8016000 | 同 GPIO0 | 14 / 15 |
| UART0 | 0xf8010000 | — | 1 |
| 用户 APB3 窗（io_apbSlave_0） | 0xf8100000（64KB） | APB3 从机自定 | — |
| 用户中断 | — | userInterruptA | 16 |

> GPIO0/GPIO1 是两个独立片内外设，寄存器布局相同（仅基址/位宽/中断号/引脚组不同），驱动用同一套 gpio.h。

---

## 3. GPIO0 / GPIO1 位宽差异

| | GPIO0 | GPIO1 |
|---|---|---|
| 位宽 | 4（当前 settings 被改为 8，见 §5 警告） | 8 |
| 使能 | 已开 | 关（GPIO1=1'b0） |
| 中断 ID | 12/13 | 14/15 |
| SoC 边界端口 | system_gpio_0_io_* | system_gpio_1_io_*（需重生成 SoC 才出现） |

> IP 配置页 Note：The example design only enables GPIO 0, with up to 4 pins. It does not include GPIO 1, even if you turn them on here. —— 即 GPIO1 即使打开，官方示例工程的顶层/引脚也没给它留位，需自行加端口 + PDC。

---

## 4. 板载 LED/按键引脚对照（01_LED 已验证）

### 4.1 8 个 LED（LED0~7）

| LED | 引脚 | GPIO 名 | Bank | 电压 | 复用功能脚 |
|---|---|---|---|---|---|
| LED0 | P13 | GPIOR_27 | BR | 3.3V | 纯用户 IO |
| LED1 | N13 | GPIOR_24 | BR | 3.3V | 纯用户 IO |
| LED2 | G3 | GPIOL_P_12 | 1B | 1.8V | 纯用户 IO |
| LED3 | N2 | GPIOL_P_02_CSI | 1A | 1.8V | 配置脚（CSI） |
| LED4 | G4 | GPIOL_N_12 | 1B | 1.8V | 纯用户 IO |
| LED5 | E1 | GPIOL_P_15_NSTATUS | 1B | 1.8V | 配置脚（NSTATUS） |
| LED6 | E2 | GPIOL_N_15_TEST_N | 1B | 1.8V | 配置脚（TEST_N） |
| LED7 | M2 | GPIOL_N_02_CSO | 1A | 1.8V | 配置脚（CSO） |

### 4.2 4 个按键（KEY0~3）

| KEY | 引脚 | GPIO 名 | Bank | 电压 |
|---|---|---|---|---|
| KEY0 | C4 | GPIOL_07 | TL | 3.3V |
| KEY1 | P14 | GPIOR_22 | BR | 3.3V |
| KEY2 | N14 | GPIOR_21 | BR | 3.3V |
| KEY3 | A3 | GPIOL_03 | TL | 3.3V |

---

## 5. 关键限制：配置脚不要当 GPIO 用（co-debug + SoC 设计）

LED3（N2/CSI）、LED5（E1/NSTATUS）、LED6（E2/TEST_N）、LED7（M2/CSO）是 Dual-Purpose Configuration Pins（Ti60 手册 Table 73/74）。配置阶段承担配置功能，用户态才三态弱上拉可作为普通 IO：

| 配置脚 | 配置阶段作用 | 用户态 |
|---|---|---|
| CSI | 输入：配置片选；整个配置期间必须为高（JTAG 模式需要） | 三态弱上拉输入 |
| CSO | 输出：菊花链片选中，bitstream 传完置 1 | 三态弱上拉输入 |
| NSTATUS | 输出：配置错误指示；有效 bitstream 置 1 | 三态弱上拉输入 |
| TEST_N | 输入：低有效测试使能；配置期间必须拉高 | 三态弱上拉输入 |

### 实测现象（务必注意）

- GPIO0=4 位（P13/N13/G3/N2/CSI）→ 能正常上板 + co-debug 调试 OK。
- 把 GPIO0 扩到 8 位并将 NSTATUS / TEST_N / CSO 作为 GPIO 输出去驱动 LED5~7 → co-debug 出现 Halt timed out / timed out while waiting for target halted（CPU 能识别但 halt 超时）；回退到 4 位后恢复 OK。

结论：在 SoC + DDR + co-debug 这类依赖配置/JTAG 的设计里，不要用 CSI/NSTATUS/TEST_N/CSO 这些配置脚当 GPIO 输出（上电/配置期间的电平会干扰配置与 JTAG 调试链）。

### 推荐做法

1. 只用纯用户 IO 点 LED：P13、N13、G3、G4（LED0/1/2/4）最稳；或
2. 8 位 LED 走自建 APB3 外设（apb3_led，从自研 RTL 选安全引脚，避开 CSI/NSTATUS/TEST_N/CSO），既稳定又锻炼外部用户总线。

### 其他注意

- 电平分裂：LED0/1 在 3.3V bank（BR），LED2~7 在 1.8V bank（1A/1B）；PDC 的 io_standard 必须按 3.3V/1.8V 分别设置。
- 顶层端口要与 SoC 位宽一致：SoC 的 system_gpio_0_io_write 是 [7:0]，顶层 soc_gpio_OUT/IN/OE 必须同为 8 位，否则高 4 位被截断永远不动（已踩过：顶层仍为 [3:0] → LED4~7 无输出）。

---

## 6. C 配置要点（固件侧）

    #include "bsp.h"
    #include "gpio.h"
    #include "soc.h"
    #include "userDef.h"     // #define GPIO0 SYSTEM_GPIO_0_IO_CTRL 等

    // 输出：LED
    gpio_setOutputEnable(GPIO0, 0xFF);   // 每 bit 决定该脚输入/输出
    gpio_setOutput(GPIO0, value);        // 写 OUTPUT(0x04)

    // 输入：按键
    gpio_setOutputEnable(GPIO0, 0x00);   // 全输入
    uint32_t key = gpio_getInput(GPIO0) & 0xF;

    // 中断（可选，用 ID0=12）
    gpio_setInterruptFallEnable(GPIO0, 0xF);
    plic_set_enable(BSP_PLIC, BSP_PLIC_CPU_0, SYSTEM_PLIC_SYSTEM_GPIO_0_IO_INTERRUPTS_0, 1);

> C 里配置 GPIO 没有魔法：就是往 0xf8015000+{0,4,8}（输入/输出/输出使能）写掩码；地址宏在 soc.h，驱动宏在 gpio.h。

---

## 7. 上板 / 调试流程与注意

1. 改 RTL → 必须重综合 + 重新下载 bit；只改固件 → RISCV IDE 重新编译下载（co-debug 载 DDR）。
2. co-debug 连接后若弹 Perspective Switch → 选 Switch；程序暂停在入口，按 F8 Resume 运行。
3. 若连上但 halt 超时：先断电重上电（等 PLL 锁定）再 Debug；降低 openocd adapter_khz；或与已知可调试的 bit 对照（烧回旧 bit 若恢复 → 问题在新 bit）。
4. 引脚约束以 project.peri.xml（Interface Designer）为准；名字改动要同步 RTL 顶层端口。

---

## 8. 参考

- docs/spec/总线逻辑.md（SoC 总线/地址/中断总表）
- docs/芯片手册/ti60-ds-v3.8芯片手册.pdf（Table 73：Dual-Purpose Configuration Pins）
- src/example/01_LED（LED/按键引脚已验证；LED.pinout.csv）
- 官方 software/standalone/gpio/gpioDemo（GPIO 点亮/中断示例）
