PROJ_NAME=project
STANDALONE = ..

# ---------------------------------------------------------------------------
# Efinity C 工程 makefile 模板
#
#   本文件在 code/app/<工程名>/ 下，新建工程时拷到工程目录，
#   覆盖向导生成的那份 makefile。
#
#   工程是自包含的：
#       src/          应用代码（从 code/app/<工程名>/ 拷）
#       peripheral/   外设驱动（从 code/peripheral/ 拷，保持同构）
#   详见 code/README_如何导入C文件到工程.md
# ---------------------------------------------------------------------------


SRCS = 	$(wildcard src/*.c) \
		$(wildcard src/*.cpp) \
		$(wildcard src/*.S) \
		$(wildcard peripheral/*.c) \
		$(wildcard peripheral/*/*.c) \
		$(wildcard peripheral/*/*/*.c) \
		${STANDALONE}/common/start.S \
		${STANDALONE}/common/trap.S


# 头文件搜索路径（只影响编译，不影响链接）
#   每加一个外设目录，就加一行
#   注：SRCS 用的是 wildcard，peripheral/*/*/*.c 会自动收进来，不用手动加；
#       但 INC 是显式列表，新目录必须手动补一行
INC += -Iperipheral/APB0/key
INC += -Iperipheral/APB0/checker
INC += -Iperipheral/APB0/lineBuffer


include ${STANDALONE}/common/bsp.mk
include ${STANDALONE}/common/riscv64-unknown-elf.mk
include ${STANDALONE}/common/standalone.mk

LDSCRIPT =  ${BSP_PATH}/linker/default.ld
