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
    Y            在玩家脚下生成道具和底座
    1 / 小键盘1  跳到 Boss 房
    E            放置炸弹
    T            切换测试模式（无敌＋测试伤害；再次按下关闭）
    /            进入"命令行"：敲完回车发给板子（回车不会当移动键）
    ESC 退出（松手即停，所以也可以随时按 Ctrl+C）

协议（板端 game/game_input.c）:
    两字节帧:  0xA5  +  mask
    mask:       bit0 左 / bit1 右 / bit2 下 / bit3 上 / bit4 R / bit5 E / bit6 Y / bit7 1
    掩码变化时发送，按住时每 200 ms 补发；板端 500 ms 收不到就回落到板载按键。
    T 使用独立文本命令 T + 回车，不占用方向/传送等掩码位。
"""
import argparse
import codecs
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
    venv_py = Path(__file__).resolve().parents[2] / ".venv" / "Scripts" / "python.exe"
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
    0x45: 0x20,  # E：放置炸弹
    0x59: 0x40,  # Y：生成道具和底座
    0x31: 0x80,  # 主键盘 1：直接跳到 Boss 房
    0x61: 0x80,  # 小键盘 1（Num Lock 开启）
}
VK_ESC = 0x1B
VK_TEST = 0x54
VK_CONTROL = 0x77  # F8

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
    ap.add_argument("--log", type=Path, help="将板子串口输出同步追加到日志文件")
    ap.add_argument("--quiet-keys", action="store_true", help="不逐次打印按键状态")
    ap.add_argument("--keep-log", action="store_true", help="ESC 释放操控但继续记录；F8 恢复操控，Ctrl+C 退出")
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
    print("按键：WASD/方向键移动，R 重开，Y 道具+底座，E 炸弹，1 进 Boss，T 测试，/ 命令，ESC 退出。", flush=True)
    sys.stdout.flush()
    if args.keep_log:
        print("采集模式: ESC 释放操控但保留日志，F8 切换操控，Ctrl+C 退出。", flush=True)

    last_mask = -1
    last_send = 0.0
    test_was_down = False
    cmd = None            # None = 普通模式；list = 正在输入一条命令
    skip_next = False     # 方向键等特殊键会连发两个码，第二个要丢掉
    log = None
    controls = True
    control_was_down = False
    exit_reason = "exception"
    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    try:
        if args.log:
            args.log.parent.mkdir(parents=True, exist_ok=True)
            log = args.log.open("ab")
            log.write(b"GAMEPAD START\r\n")
            log.flush()
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
            if not args.quiet or log is not None:
                data = ser.read(4096)
                if data:
                    if log is not None:
                        log.write(data)
                        log.flush()
                    if not args.quiet:
                        sys.stdout.write(decoder.decode(data))
                        sys.stdout.flush()

            if held(VK_ESC):
                if args.keep_log:
                    controls = False
                else:
                    exit_reason = "ESC"
                    break
            control_down = held(VK_CONTROL)
            if args.keep_log and control_down and not control_was_down:
                controls = not controls
            control_was_down = control_down

            test_down = held(VK_TEST)
            if controls and cmd is None and test_down and not test_was_down:
                ser.write(b"T\r")
                ser.flush()
            test_was_down = test_down  # Holding T must never repeat the toggle.

            mask = read_mask() if controls else 0
            now = time.time()
            # 变化就发；一直按着也每 200 ms 补发一次（板端 500 ms 收不到会掉线）
            if mask != last_mask or (mask and now - last_send > 0.2):
                ser.write(bytes([PAD_MAGIC, mask]))
                ser.flush()
                last_mask = mask
                last_send = now
                # 状态行（覆盖同一行，别刷屏）
                names = []
                if mask & 0x80: names.append("BOSS")
                if mask & 0x20: names.append("BOMB(E)")
                if mask & 0x40: names.append("SPAWN(Y)")
                if mask & 0x10: names.append("RESTART")
                if mask & 0x8: names.append("UP")
                if mask & 0x4: names.append("DOWN")
                if mask & 0x1: names.append("LEFT")
                if mask & 0x2: names.append("RIGHT")
                if not args.quiet_keys:
                    sys.stderr.write("\r  按键: %-22s" % (" ".join(names) if names else "(松开)"))
                    sys.stderr.flush()

            time.sleep(0.01)
    except KeyboardInterrupt:
        exit_reason = "interrupt"
    finally:
        if not args.quiet:
            sys.stdout.write(decoder.decode(b"", final=True))
            sys.stdout.flush()
        if log is not None:
            log.write(("\r\nGAMEPAD EXIT: " + exit_reason + "\r\n").encode("ascii"))
            log.close()
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
