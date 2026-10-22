# -*- coding: utf-8 -*-
"""
game_pad.py —— 用电脑键盘通过串口控制板子上的游戏（虚拟手柄）

用法（本仓库的 Python 环境走 uv，依赖在仓库根的 pyproject.toml 里）:
    uv run tools/GamePad/game_pad.py                 # 直接用下面 PORT 常量里的串口
    uv run tools/GamePad/game_pad.py --list          # 先看看有哪些串口
    uv run tools/GamePad/game_pad.py -p COM7         # 临时换一个口
    uv run tools/GamePad/game_pad.py --quiet         # 不显示板子回传的日志

  ★ 换串口最简单的方法：改下面 PORT = "COM5" 这一行，然后直接跑脚本。

按键:
    W / ↑ 上      S / ↓ 下      A / ← 左      D / → 右（可以斜着按）
    R            按一下重开一局（不用按住）
    T            按一下切换【无敌】调试模式（不用按住）
    /            进入"命令行"：敲完回车发给板子（回车不会当移动键）
                 例：/debug0 回车 = 无敌；/debug1 回车 = 恢复正常
    ESC 退出（松手即停，所以也可以随时按 Ctrl+C）

协议（板端 game/game_input.c）:
    两字节帧:  0xA5  +  mask
    mask 低 4 位:  bit0 左 / bit1 右 / bit2 下 / bit3 上
    只在掩码变化时发一次；板端 500 ms 收不到就自动回落到板载按键。
"""
import argparse
import ctypes
import os
import sys
import time

try:
    import msvcrt                     # 控制台按键（只在 Windows 有）
except ImportError:
    msvcrt = None
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    # 没装依赖（比如点了 IDE 的"运行"按钮，解释器是系统 Python）。
    # 仓库里如果有 uv 建的 .venv，就用它把自己重开一遍 —— 用户什么都不用配。
    venv_py = Path(__file__).resolve().parents[1] / ".venv" / "Scripts" / "python.exe"
    if venv_py.exists() and Path(sys.executable).resolve() != venv_py.resolve():
        sys.stderr.write("用 .venv 的 Python 重开一次：%s\n" % venv_py)
        sys.stderr.flush()
        os.execv(str(venv_py), [str(venv_py), str(Path(__file__).resolve())] + sys.argv[1:])
    sys.exit("缺 pyserial：先在仓库根跑一次  uv sync，然后用  uv run tools/GamePad/game_pad.py")

# =============================================================================
#  ★★★ 只改这两行就够了（命令行 -p / -b 仍然可以临时覆盖）★★★
# =============================================================================
PORT = "COM5"          # ← 板子接的那个串口（不确定就跑：uv run tools/GamePad/game_pad.py --list）
BAUD = 115200
# =============================================================================

PAD_MAGIC = 0xA5

# Windows 虚拟键码
VK = {
    0x26: 0x8,   # 上
    0x28: 0x4,   # 下
    0x25: 0x1,   # 左
    0x27: 0x2,   # 右
    0x57: 0x8,   # W
    0x53: 0x4,   # S
    0x41: 0x1,   # A
    0x44: 0x2,   # D
    0x52: 0x10,  # R：按一下重开一局
    0x54: 0x20,  # T：按一下切换"无敌"（调试）
}
VK_ESC = 0x1B

user32 = ctypes.windll.user32


def held(vk):
    return (user32.GetAsyncKeyState(vk) & 0x8000) != 0


def read_mask():
    m = 0
    for vk, bit in VK.items():
        if held(vk):
            m |= bit
    return m


def show_ports():
    ports = list(list_ports.comports())
    if not ports:
        print("没找到任何串口。板子的 USB 线插好了吗？驱动装了吗？")
        return
    print("可用串口:")
    for p in ports:
        print("   %-8s %s" % (p.device, p.description))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default=PORT,
                    help="串口名（默认用脚本顶部的 PORT 常量）")
    ap.add_argument("-b", "--baud", type=int, default=BAUD)
    ap.add_argument("--quiet", action="store_true", help="不显示板子回传的日志")
    ap.add_argument("--list", action="store_true", help="只列串口")
    args = ap.parse_args()

    if args.list:
        show_ports()
        return

    port = args.port
    if not port:
        show_ports()
        sys.exit("脚本顶部没有填 PORT。把 PORT = \"COM几\" 改成你的串口（或加 -p COMx）。")

    try:
        ser = serial.Serial(port, args.baud, timeout=0)
    except serial.SerialException as e:
        show_ports()
        sys.exit("打不开 %s: %s\n（串口被占？先关掉串口助手/IDE；或者改脚本顶部的 PORT）"
                 % (port, e))

    print("已连接 %s @ %d" % (port, args.baud))
    print("按键: W/A/S/D 或方向键移动，/ 输入命令（如 /debug0），ESC 退出。板子日志%s显示。" %
          ("不" if args.quiet else "会"))
    sys.stdout.flush()

    last_mask = -1
    last_send = 0.0
    cmd = None            # None = 普通模式；list = 正在输入一条命令
    skip_next = False     # 方向键等特殊键会连发两个码，第二个要丢掉
    try:
        while True:
            # ---- 控制台文本输入：按 / 进入命令行，回车发到板子 ----
            if msvcrt is not None:
                while msvcrt.kbhit():
                    ch = msvcrt.getwch()
                    if skip_next:
                        skip_next = False
                        continue
                    if ch in ('\x00', '\xe0'):
                        skip_next = True
                        continue

                    if cmd is None:
                        if ch == '/':
                            cmd = []
                            sys.stderr.write("\r输入命令: ")
                            sys.stderr.flush()
                        continue

                    if ch in ('\r', '\n'):
                        text = "".join(cmd)
                        sys.stderr.write("\r  → 发送: %s\n" % text)
                        sys.stderr.flush()
                        if text:
                            ser.write((text + "\r").encode("ascii", "ignore"))
                            ser.flush()
                        cmd = None
                    elif ch == '\x1b':
                        sys.stderr.write("\r  (取消)\n")
                        cmd = None
                    elif ch in ('\b', '\x7f'):
                        if cmd:
                            cmd.pop()
                            sys.stderr.write("\b \b")
                            sys.stderr.flush()
                    elif ch.isprintable():
                        cmd.append(ch)
                        sys.stderr.write(ch)
                        sys.stderr.flush()
            # 板子打印过来的日志，原样转发到控制台
            if not args.quiet:
                data = ser.read(4096)
                if data:
                    sys.stdout.write(data.decode("utf-8", "replace"))
                    sys.stdout.flush()

            if held(VK_ESC):
                break

            mask = read_mask()
            now = time.time()
            # 变化就发；一直按着也每 200 ms 补发一次（板端 500 ms 收不到会掉线）
            if mask != last_mask or (mask and now - last_send > 0.2):
                ser.write(bytes([PAD_MAGIC, mask]))
                ser.flush()
                last_mask = mask
                last_send = now
                # 状态行（覆盖同一行，别刷屏）
                names = []
                if mask & 0x20: names.append("DEBUG")
                if mask & 0x10: names.append("RESTART")
                if mask & 0x8: names.append("UP")
                if mask & 0x4: names.append("DOWN")
                if mask & 0x1: names.append("LEFT")
                if mask & 0x2: names.append("RIGHT")
                sys.stderr.write("\r  按键: %-22s" % (" ".join(names) if names else "(松开)"))
                sys.stderr.flush()

            time.sleep(0.01)
    except KeyboardInterrupt:
        pass
    finally:
        try:
            ser.write(bytes([PAD_MAGIC, 0]))     # 松手，别让它一直走
            ser.flush()
            time.sleep(0.05)
            ser.close()
        except Exception:
            pass
        sys.stderr.write("\n已退出。\n")


if __name__ == "__main__":
    main()