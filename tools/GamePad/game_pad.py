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
    W / A / S / D      控制移动（可以斜着按）
    小键盘 ↑ / ↓ / ← / → 按住时控制人物朝向并连续射击
    普通方向键        同样作为瞄准/射击输入，兼容 Num Lock 关闭的情况
    R            按一下重开一局（不用按住）
    Y            在玩家脚下生成道具和底座
    1 / 小键盘1  跳到 Boss 房
    E            放置炸弹
    T            切换测试模式（无敌＋测试伤害；再次按下关闭）
    N            每按一次增加 25 个敌方弹幕上限，最高 600
    B            每按一次降低 25 个敌方弹幕上限，最低 500
    P            暂停 / 继续游戏逻辑，暂停时锁定当前画面
    M            切换游戏 Demo / GPU-CPU 对比 Demo
    /            进入"命令行"：敲完回车发给板子（回车不会当移动键）
    ESC 退出（松手即停，所以也可以随时按 Ctrl+C）

协议（板端 game/game_input.c）:
    三字节帧:  0xA6  +  move  +  aim
    move:       bit0 左 / bit1 右 / bit2 下 / bit3 上 / bit4 R / bit5 E / bit6 Y / bit7 1
    aim:        bit0 左 / bit1 右 / bit2 下 / bit3 上
    掩码变化时立即发送；连接期间每 200 ms 发送一次心跳。板端 500 ms 收不到就回落到板载按键。
    T / M / N / B / P 使用独立文本命令加回车，不占用方向/传送等掩码位。
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

PAD_MAGIC_V2 = 0xA6

# Windows 虚拟键码
VK_MOVE = {
    0x57: 0x8,   # W
    0x53: 0x4,   # S
    0x41: 0x1,   # A
    0x44: 0x2,   # D
}
VK_AIM = {
    0x26: 0x8,   # 上方向键
    0x28: 0x4,   # 下方向键
    0x25: 0x1,   # 左方向键
    0x27: 0x2,   # 右方向键
    0x68: 0x8,   # 小键盘 8
    0x62: 0x4,   # 小键盘 2
    0x64: 0x1,   # 小键盘 4
    0x66: 0x2,   # 小键盘 6
}
VK_ACTION = {
    0x52: 0x10,  # R：按一下重开一局
    0x45: 0x20,  # E：放置炸弹
    0x59: 0x40,  # Y：生成道具和底座
    0x31: 0x80,  # 主键盘 1：直接跳到 Boss 房
    0x61: 0x80,  # 小键盘 1（Num Lock 开启）
}
VK_ESC = 0x1B
VK_TEST = 0x54
VK_SWITCH_DEMO = 0x4D
VK_BULLET_LIMIT = 0x4E
VK_BULLET_LIMIT_DOWN = 0x42
VK_PAUSE = 0x50
VK_CONTROL = 0x77  # F8

user32 = ctypes.windll.user32


def held(vk):
    return (user32.GetAsyncKeyState(vk) & 0x8000) != 0


def read_masks():
    move = 0
    aim = 0
    for vk, bit in VK_MOVE.items():
        if held(vk):
            move |= bit
    for vk, bit in VK_AIM.items():
        if held(vk):
            aim |= bit
    for vk, bit in VK_ACTION.items():
        if held(vk):
            move |= bit
    return move, aim


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
    print("按键：WASD 移动/默认朝向，按住小键盘/方向键瞄准并射击，M 切换 Demo，R 重开，Y 道具，E 炸弹，1 进 Boss，T 测试，N 弹幕上限 +25 / B -25（500～600），P 暂停/继续，/ 命令，ESC 退出。", flush=True)
    sys.stdout.flush()
    if args.keep_log:
        print("采集模式: ESC 释放操控但保留日志，F8 切换操控，Ctrl+C 退出。", flush=True)

    last_move = -1
    last_aim = -1
    last_send = 0.0
    test_was_down = False
    switch_was_down = False
    bullet_limit_was_down = False
    bullet_limit_down_was_down = False
    pause_was_down = False
    cmd = None            # None = 普通模式；list = 正在输入一条命令
    skip_next = False     # 命令行输入时，扩展键会连发两个码，第二个要丢掉
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

            switch_down = held(VK_SWITCH_DEMO)
            if controls and cmd is None and switch_down and not switch_was_down:
                ser.write(b"M\r")
                ser.flush()
            switch_was_down = switch_down

            bullet_limit_down = held(VK_BULLET_LIMIT)
            if controls and cmd is None and bullet_limit_down and not bullet_limit_was_down:
                ser.write(b"N\r")
                ser.flush()
            bullet_limit_was_down = bullet_limit_down

            bullet_limit_decrease_down = held(VK_BULLET_LIMIT_DOWN)
            if controls and cmd is None and bullet_limit_decrease_down and not bullet_limit_down_was_down:
                ser.write(b"B\r")
                ser.flush()
            bullet_limit_down_was_down = bullet_limit_decrease_down

            pause_down = held(VK_PAUSE)
            if controls and cmd is None and pause_down and not pause_was_down:
                ser.write(b"P\r")
                ser.flush()
            pause_was_down = pause_down

            move_mask, aim_mask = read_masks() if controls and cmd is None else (0, 0)
            now = time.time()
            # 变化立即发送；无按键时也定期发心跳，保持串口模式不误回落到板载自动索敌。
            if (move_mask != last_move or aim_mask != last_aim or
                    now - last_send > 0.2):
                ser.write(bytes([PAD_MAGIC_V2, move_mask, aim_mask]))
                ser.flush()
                last_move = move_mask
                last_aim = aim_mask
                last_send = now
                # 状态行（覆盖同一行，别刷屏）
                names = []
                if move_mask & 0x80: names.append("BOSS")
                if move_mask & 0x20: names.append("BOMB(E)")
                if move_mask & 0x40: names.append("SPAWN(Y)")
                if move_mask & 0x10: names.append("RESTART")
                if move_mask & 0x8: names.append("MOVE-UP")
                if move_mask & 0x4: names.append("MOVE-DOWN")
                if move_mask & 0x1: names.append("MOVE-LEFT")
                if move_mask & 0x2: names.append("MOVE-RIGHT")
                if aim_mask & 0x8: names.append("AIM-UP")
                if aim_mask & 0x4: names.append("AIM-DOWN")
                if aim_mask & 0x1: names.append("AIM-LEFT")
                if aim_mask & 0x2: names.append("AIM-RIGHT")
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
            ser.write(bytes([PAD_MAGIC_V2, 0, 0]))  # 松手，别让它一直移动或瞄准
            ser.flush()
            time.sleep(0.05)
            ser.close()
        except Exception:
            pass
        sys.stderr.write("\n已退出。\n")


if __name__ == "__main__":
    main()
