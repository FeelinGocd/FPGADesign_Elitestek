# code/ —— C 代码归档

> **核心原则：`project/GamingPlayUnit/.../GamingPlayUnit/` 是当前开发与编译的唯一现场，`code/` 是提交前的归档副本。**
>
> 工程自包含（不引用仓库外路径）；改动顺序是 **project → code**，不是反向。
> `code/` 不参与编译；`pic2pram.py` / `map_designer.py` 默认只写 `project/`（`--target all` / `archive` 才写归档），`item_designer.py` 只写 `tools/item_designer/` 和 `docs/spec/`。
> 云端同步管的是仓库（`code/` + `res/` + `RTL/`），工程本身不入库。

**为什么工程自包含？**

Efinity 的 C 工程被强制创建在 `software/standalone/` 下，离 `code/` 有 8 层目录。
跨目录引用会带来一串麻烦：makefile 里要写一堆 `..`、`.cproject` 要配 include 路径、
`.project` 要加 linked resource —— **少配一样 IDE 就报红，工程挪个位置就全崩**。

所以工程里放一份完整源码；新建工程时才从 `code/` 拷入作为起点，之后以工程为准。

---

## 一、目录职责

```
code/                        ← 提交前归档（与工程目录同构）
├── peripheral/              外设驱动（和 RTL/Peripherial 一一镜像）
│   └── APB0/
│       ├── key/             APB 槽 0：按键
│       │   ├── user_key.h   对外 API（只暴露函数，不暴露寄存器结构体）
│       │   └── user_key.c   实现（寄存器结构体藏在这里）
│       ├── lineBuffer/      APB 槽 2 的显示子块（0x2040 起）；AXI S1 只是显示读主口编号，不是 APB 槽号
│       │   ├── user_lineBuffer.h
│       │   └── user_lineBuffer.c
│       ├── gpu2d/           APB 槽 2 的观测子块（0x2000）+ 加速器命令 / 描述表
│       │   ├── user_gpu2d.h
│       │   ├── user_gpu2d.c
│       │   └── assets_desc.h  素材描述表（pic2pram.py 生成）
│       └── checker/         APB 槽 1 原为 DDR 自检，已从系统摘除；驱动保留备用，
│           ├── user_lineBuffer.h  寄存器表 / 帧缓存格式约定 / 全部 API
│           └── user_lineBuffer.c  实现 + 几种上板验证图案
│               （checker/ 里的两个文件同前，恢复时要另选空槽）
├── app/                     应用入口归档：main.c / userDef.h / IRQ_frame.* /
│                            compare_demo.* / game_demo.h / makefile / game_memory.ld
└── logic/
    └── game/                游戏 demo（与工程 game/ 同构：game*.c / game.h）

res/example/<编号>_<名字>/    ← 里程碑快照（换台设备能复现）
├── RTL/                     该里程碑的 RTL
├── Code/                    main.c / userDef.h / game_demo.* / compare_demo.* / makefile
└── project/project.peri.xml 该里程碑的引脚约束
```

| 工程里的位置 | 归档到 `code/` 的哪 |
|---|---|
| `<工程>/peripheral/` | `code/peripheral/`（**保持同构**） |
| `<工程>/game/` | `code/logic/game/` |
| `<工程>/src/` | `code/app/`（`main.c` / `userDef.h` / `IRQ_frame.*` / `compare_demo.*` / `game_demo.h`） |
| `<工程>/makefile` | `code/app/makefile` |
| `<工程>/game_memory.ld` | `code/app/game_memory.ld`（链接脚本，RAM 132 KiB） |

> 新建工程时才反向：从 `code/` 把上述内容拷进工程，之后**在工程里改、编译、上板**，提交前再同步回 `code/`。

---

## 二、工程结构（新建工程时照此搭）

### 目标结构

```
project/GamingPlayUnit/
└── embedded_sw/SoC_RISCV32/
    └── software/standalone/
        └── GamingPlayUnit/              ← Efinity 强制的位置（当前唯一主工程）
            ├── makefile                 ← 用仓库里这份（覆盖向导生成的）
            ├── game_memory.ld           ← RAM 132 KiB 链接脚本
            ├── .project / .cproject
            ├── src/                     ← main.c / userDef.h / IRQ_frame.* / compare_demo.*
            ├── game/                    ← game*.c / game.h（游戏 demo）
            └── peripheral/              ← 与 code/peripheral/ 同构
                └── APB0/
                    └── key/
                        ├── user_key.h
                        └── user_key.c
```

> `peripheral/` 在工程里**保持和 `code/` 一样的层级**，这样和 RTL 一看就能对上。
> 当前 makefile 已把 `game/*.c` 收进 `SRCS`，并把 `src` / `game` 加进 `INC`。

### 新建工程时的步骤

**① 建工程** —— IDE 里 `File > New > Efinix Project`，起个名字（比如 `GamingPlayUnit`）。

**② 拷文件（只在新工程时做一次）**

| 从 `code/` | 到工程 |
|---|---|
| `code/peripheral/` 整个目录 | `peripheral/`（保持层级） |
| `code/app/` 的 main.c / userDef.h / IRQ_frame.* / compare_demo.* / game_demo.h | `src/` |
| `code/logic/game/` 全部 | `game/` |
| `code/app/makefile` | `makefile`（**覆盖**向导生成的那份） |
| `code/app/game_memory.ld` | `game_memory.ld` |

**③ 编译** —— `Project > Build Project`（F7）。

**就这三步。** 不用改 `.cproject`，不用改 `.project`，不用配 include 路径 ——
因为文件都在工程里，Eclipse 天然就能索引。之后新增文件**直接写在工程里**，提交前再归档回 `code/`。

---

## 三、makefile 说明

模板在各例程的 `Code/makefile`（例如 `res/example/04_SoC_APB0_key/Code/makefile`），当前主工程在此基础上多了 `game/*.c`、`-Igame`、`-MMD -MP` 和 `LDSCRIPT = game_memory.ld`。基本结构：

```makefile
PROJ_NAME=project
STANDALONE = ..

SRCS = $(wildcard src/*.c) \
       $(wildcard src/*.cpp) \
       $(wildcard src/*.S) \
       $(wildcard peripheral/*.c) \
       $(wildcard peripheral/*/*.c) \
       $(wildcard peripheral/*/*/*.c) \
       $(wildcard game/*.c) \
       ${STANDALONE}/common/start.S \
       ${STANDALONE}/common/trap.S

INC += -Iperipheral/APB0/key
INC += -Igame
INC += -Isrc

include ${STANDALONE}/common/bsp.mk
include ${STANDALONE}/common/riscv64-unknown-elf.mk
include ${STANDALONE}/common/standalone.mk

CFLAGS += -MMD -MP          # 头文件依赖 → .d，下面 -include 进来
LDSCRIPT = game_memory.ld   # 主工程：RAM 132 KiB；例程模板用 ${BSP_PATH}/linker/default.ld
```

之后 `-include $(OBJS:.o=.d)`，改头文件会自动重编相关 .c。

**三段通配符覆盖三种深度**（make 的 `wildcard` 不支持 `**` 递归）：

| 通配 | 覆盖 |
|---|---|
| `peripheral/*.c` | `peripheral/xxx.c` |
| `peripheral/*/*.c` | `peripheral/APB0/xxx.c` |
| `peripheral/*/*/*.c` | `peripheral/APB0/key/user_key.c` ← 当前用的 |

**`INC` 是头文件搜索路径**，每加一个外设目录要加一行。`SRCS` 不用改，通配符自动收。

> `INC` 在 Efinity 的 `standalone.mk` 里只用在编译行、不在链接行，加东西很干净。

---

## 四、同步约定（最重要的一条）⚠️

工程和 `code/` 是两份源码，没有任何机制自动保持一致 —— 所以方向必须记死。

### 约定：`project/` 是开发真源，`code/` 是提交前归档

```
project/…/GamingPlayUnit/   ──── 手工同步 ────>   code/
（改这里、编译这里、上板）                        （归档快照，不参与编译）
```

- **要改代码，改 `project/` 工程里的那份**，编译、验证都在工程里做
- **`code/` 只在提交前同步**：把 `src/`、`game/`、`peripheral/`、`makefile`、`game_memory.ld` 的最新版本覆盖过去
- 不要在 `code/` 里长期开发；两边不一致时，以 `project/` 为准
- 工具默认也写 `project/`（`pic2pram.py --target project`、`map_designer.py --target project`）；只有 `--target all/archive` 才碰 `code/`

### 什么时候必须归档

- 改了 `peripheral/`、`game/`、`src/` 或 `makefile` / `game_memory.ld` → 提交前同步对应目录
- 提交/打包/给评审的版本 → 全量同步一次
- 新建工程 → 反过来从 `code/` 拷入（见 §二）

---

## 五、验证编译

在工程目录下：

```powershell
$env:PATH = 'E:\FPGA\Efinity\EfinityRISCV\toolchain\bin;E:\FPGA\Efinity\EfinityRISCV\build_tools\bin;' + $env:PATH
$env:BSP  = 'efinix/EfxSapphireSoc'

Remove-Item -Recurse -Force build      # 需要全量时用；日常改源码直接 make（-MMD/-MP 会跟踪头文件）
& 'E:\FPGA\Efinity\EfinityRISCV\build_tools\bin\make.exe'
```

**成功的输出长这样（数字随工程变化）：**

```
CC src/main.c
CC peripheral/APB0/key/user_key.c
CC ../common/start.S
CC ../common/trap.S
CC syscalls.c
LD project
Memory region         Used Size  Region Size  %age Used
             ram:        6384 B       132 KB      4.72%
```

**关键：`src/main.c` 和 `peripheral/.../user_key.c` 都编了，exit code 是 0。**

---

## 六、新增一个外设驱动

假设要加加速器，RTL 在 `RTL/Peripherial/APB0/accel/`：

**① 在工程的 `peripheral/` 里建目录（和 RTL 镜像）**

```
project/.../GamingPlayUnit/peripheral/APB0/accel/
├── user_accel.h     对外 API
└── user_accel.c     实现
```

**② 归档时同步到 `code/peripheral/APB0/accel/`**

**③ 工程 makefile 加一行 include 路径**

```makefile
INC += -Iperipheral/APB0/key
INC += -Iperipheral/APB0/accel       # 新增
```

`SRCS` 不用动 —— 三段通配符会自动收进去。

**④ 没被调用的函数会被链接器自动回收**

`peripheral/` 下**所有** `.c` 都会被编译，但 makefile 里开着
`-ffunction-sections -Wl,--gc-sections`，没被调用的函数不会进最终镜像，不浪费空间。

---

## 七、写驱动的约定

**`.h` 只暴露 API，寄存器结构体藏在 `.c` 里。**

参考 `peripheral/APB0/key/user_key.c`：

```c
/* user_key.c 里（外部看不见） */
typedef struct {
    volatile u32 key_data;      /* 0x00  RO */
    volatile u32 key_irq_en;    /* 0x04  RW */
    volatile u32 key_id;        /* 0x08  RO */
} KEYType;
#define KEY  ((KEYType *)KEY_BASE)
```

```c
/* user_key.h 里（对外） */
u32  key_getData(void);
u32  key_getIrqEn(void);
void key_setIrqEn(u32 mask);
u32  key_getId(void);
```

**好处**：以后 RTL 里寄存器挪位置、加寄存器，只改 `.c` 一个文件，所有调用方自动跟上。

**注意事项**：

- 结构体**成员顺序 = 地址偏移顺序**，不能改、不能删、**不能插空位**
  （以后要在后面加寄存器，得先补一个 `volatile u32 reserved;` 占位）
- 成员必须带 `volatile` —— 否则编译器会把重复读优化掉
- 用 `u32`（平台类型，来自 `driver/type.h`），别用 `uint32_t`

---

## 八、平台文件不要改

`embedded_sw/SoC_RISCV32/` 下的这些是**工具生成的**，重新生成 SoC IP 时会**被覆盖回原样**：

```
bsp/**                          BSP（soc.h / bsp.h / 链接脚本）
tool/**                         生成脚本
software/freeRTOS/**
software/standalone/driver/**  易灵思的平台驱动（gpio.h / uart.h / plic.h ...）
```

**要改就在 `code/` 里重新写一份，不要直接改平台文件。**

> 重新生成 IP 时 `software/standalone/` 是「合并覆盖」（`shutil.copytree(..., dirs_exist_ok=True)`）：
> 同名文件会被覆盖，**你新建的文件和目录会保留**。
> 但为了保险，重新生成 IP 后还是检查一下工程里的 `peripheral/` 和 `src/` 是否还在。

---

## 九、常见问题

### 9.1 删 `.o` 不会触发重编 ⚠️

`standalone.mk` 第 76 行有：

```makefile
.SECONDARY: $(OBJS)
```

它把 `.o` 标记成 **中间文件**。GNU Make 对中间文件的规则是：
**只要最终目标比源文件新，中间文件即使不存在也认为「无需重建」**。

所以：

```powershell
Remove-Item build\obj_files\*.o      # 删了所有 .o
make                                  # -> Nothing to be done for all.   根本没编！
```

**要强制全量重编，必须删 `build/` 整个目录（或至少 `build/project.elf`）。**

### 9.2 Eclipse 报红怎么办

**新模型下基本不会出现了** —— 因为文件都在工程里，Eclipse 天然能索引。

万一遇到，分两种：

| 症状 | 原因 | 改哪 |
|---|---|---|
| `Unresolved inclusion: "xxx.h"` | 头文件不在工程的 include 路径里 | `.cproject` 的 Include Paths |
| `Symbol XXX could not be resolved` | 头文件所在目录**不在工程内**，索引器不解析 | 把目录拷进工程，或加 linked resource |

**第二种以前会踩到，就是因为 `code/` 在工程外。** 现在源码就在工程内，这个坑自然消失了。

改完 `.cproject` / `.project` 后要**关掉工程再打开**
（linked resource 只在打开工程时读取），或者右键工程 → `Index` → `Rebuild`。

---

## 十、一句话总结

```
project/…/GamingPlayUnit/  是当前开发、编译、上板的真源
  ↓ 提交前手工同步（单向）
code/                      是仓库里的归档快照，不参与编译
```

**代价**：手工同步两边会漂移，靠「以 `project/` 为准、提交前归档」这条约定控制。
**收益**：工程零外部依赖，IDE 零配置，位置随便挪，交给别人也能直接编译。
