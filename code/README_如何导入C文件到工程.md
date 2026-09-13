# code/ —— 工作区代码库

> **核心原则：工程自包含 —— C 代码从仓库手工拷进 Efinity 工程。**
>
> 工程里**不引用任何外部路径**，拷进去之后就完全独立了。
> 云端同步管的是仓库（`code/` + `res/` + `RTL/`），工程本身不入库。

**为什么不直接引用？**

Efinity 的 C 工程被强制创建在 `software/standalone/` 下，离 `code/` 有 8 层目录。
跨目录引用会带来一串麻烦：makefile 里要写一堆 `..`、`.cproject` 要配 include 路径、
`.project` 要加 linked resource —— **少配一样 IDE 就报红，工程挪个位置就全崩**。

手工拷一份，工程就是自包含的：**拿到工程文件夹就能编译，IDE 零配置**。

---

## 一、目录职责

```
code/                        ← 给「最终工程」逐步积累
├── peripheral/              外设驱动（和 RTL/soc/Peripherial 一一镜像）
│   └── APB0/
│       └── key/
│           ├── user_key.h   对外 API（只暴露函数，不暴露寄存器结构体）
│           └── user_key.c   实现（寄存器结构体藏在这里）
├── app/                     应用代码（最终工程用，预留）
└── logic/                   算法/逻辑代码（和外设无关的部分，预留）

res/example/<编号>_<名字>/    ← 里程碑快照（换台设备能复现）
├── RTL/                     该里程碑的 RTL
├── Code/                    main.c / userDef.h / makefile
└── project/project.peri.xml 该里程碑的引脚约束
```

| 来源 | 拷到工程的哪 |
|---|---|
| `code/peripheral/` | `<工程>/peripheral/`（**保持同构**） |
| 应用代码 main.c / userDef.h | `<工程>/src/` |
| makefile 模板 | `<工程>/makefile` |

> **应用代码从哪来？**
> - 里程碑（临时测试工程）→ `res/example/<编号>_<名字>/Code/`
> - 最终工程 → `code/app/`

---

## 二、导入流程（核心）

### 目标结构

```
project/<工程>/
└── project/embedded_sw/SoC_RISCV32/
    └── software/standalone/
        └── <工程名>/                    ← Efinity 强制的位置
            ├── makefile                 ← 拷 makefile 模板
            ├── .project
            ├── .cproject
            ├── src/                     ← 拷 main.c / userDef.h
            │   ├── main.c
            │   └── userDef.h
            └── peripheral/              ← 从 code/peripheral/ 拷
                └── APB0/
                    └── key/
                        ├── user_key.h
                        └── user_key.c
```

> `peripheral/` 在工程里**保持和 `code/` 一样的层级**，这样和 RTL 一看就能对上。

### 步骤

**① 建工程** —— IDE 里 `File > New > Efinix Project`，起个名字（比如 `project`）。

**② 拷文件**

| 来源 | 到工程 |
|---|---|
| `code/peripheral/` 整个目录 | `peripheral/`（保持层级） |
| 应用代码的 main.c / userDef.h | `src/` |
| makefile 模板 | `makefile`（**覆盖**向导生成的那份） |

**③ 编译** —— `Project > Build Project`（F7）。

**就这三步。** 不用改 `.cproject`，不用改 `.project`，不用配 include 路径 ——
因为文件都在工程里，Eclipse 天然就能索引。

---

## 三、makefile 说明

模板在各例程的 `Code/makefile`（例如 `res/example/04_SoC_APB0_key/Code/makefile`），内容：

```makefile
PROJ_NAME=project
STANDALONE = ..

SRCS = $(wildcard src/*.c) \
       $(wildcard src/*.cpp) \
       $(wildcard src/*.S) \
       $(wildcard peripheral/*.c) \
       $(wildcard peripheral/*/*.c) \
       $(wildcard peripheral/*/*/*.c) \
       ${STANDALONE}/common/start.S \
       ${STANDALONE}/common/trap.S

INC += -Iperipheral/APB0/key

include ${STANDALONE}/common/bsp.mk
include ${STANDALONE}/common/riscv64-unknown-elf.mk
include ${STANDALONE}/common/standalone.mk

LDSCRIPT =  ${BSP_PATH}/linker/default.ld
```

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

因为是**手工拷贝**，`code/` 和工程两边会漂移 —— 改了这边忘了那边，没有任何机制会提醒你。

### 约定：`code/` 是唯一真源，单向流动

```
code/   ──── 手工拷贝 ────>   工程/
（改这里）                    （只读，别改）
```

- **要改代码，改 `code/` 里的**，然后拷进工程
- **工程里只作为「当前正在编译的那一份」**，不要在里面长期改动
- 调试时实在要临时改工程里的，**记得回写到 `code/`**

### 什么时候必须重新拷

- 改了 `peripheral/` 下任何文件 → 拷 `peripheral/` 进工程
- 改了应用代码（`res/example/<编号>/Code/` 或 `code/app/`）→ 拷进工程 `src/`
- 建了新工程 → 全部拷一遍

---

## 五、验证编译

在工程目录下：

```powershell
$env:PATH = 'E:\FPGA\Efinity\EfinityRISCV\toolchain\bin;E:\FPGA\Efinity\EfinityRISCV\build_tools\bin;' + $env:PATH
$env:BSP  = 'efinix/EfxSapphireSoc'

Remove-Item -Recurse -Force build      # 必须删整个 build/，原因见 9.1
& 'E:\FPGA\Efinity\EfinityRISCV\build_tools\bin\make.exe'
```

**成功的输出长这样：**

```
CC src/main.c
CC peripheral/APB0/key/user_key.c
CC ../common/start.S
CC ../common/trap.S
CC syscalls.c
LD project
Memory region         Used Size  Region Size  %age Used
             ram:        6384 B       124 KB      5.03%
```

**关键：`src/main.c` 和 `peripheral/.../user_key.c` 都编了，exit code 是 0。**

---

## 六、新增一个外设驱动

假设要加加速器，RTL 在 `RTL/soc/Peripherial/APB0/accel/`：

**① 在 `code/` 里建目录（和 RTL 镜像）**

```
code/peripheral/APB0/accel/
├── user_accel.h     对外 API
└── user_accel.c     实现
```

**② 拷进工程的 `peripheral/APB0/accel/`**

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

**第二种以前会踩到，就是因为 `code/` 在工程外。** 现在手工拷进工程，这个坑自然消失了。

改完 `.cproject` / `.project` 后要**关掉工程再打开**
（linked resource 只在打开工程时读取），或者右键工程 → `Index` → `Rebuild`。

---

## 十、一句话总结

```
code/  是仓库里的真源，云端同步只管这个目录
  ↓ 手工拷贝（单向）
工程/  是自包含的编译单元，拷进去就不管它了
```

**代价**：拷贝时两边会漂移，靠「只改 `code/`」这条约定控制。
**收益**：工程零外部依赖，IDE 零配置，位置随便挪，交给别人也能直接编译。
