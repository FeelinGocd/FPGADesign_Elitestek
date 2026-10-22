# img2hex —— 素材 → PRAM / 调色板 / C 素材表

这套脚本是**离线**跑的，不是游戏固件的一部分。依赖声明在仓库根的 `pyproject.toml`
（`pillow`），用 `uv` 管理：

```powershell
uv sync                                   # 第一次 / 换机器
uv run tools/img2hex/pic2pram.py          # 重新打包 res/pic → hex / 调色板 / assets_desc.h
```

## 脚本

| 脚本 | 作用 |
|---|---|
| `pic2pram.py` | `res/pic/*` → `assets.hex` / `palette.hex`（直接写进 Efinity 工程）、`assets_desc.h` / `report.txt`（`tools/img2hex/output/`）；并复制 `assets_desc.h` 到各 C 工程的 `peripheral/APB0/gpu2d/` |

产物落点：

- `project/GamingPlayUnit/hex/`：发布工程的 `assets.hex` / `palette.hex`
- `project/verified/06_SoC_2DGPU/project/`：已验证工程的副本（RTL 的 `INIT_FILE` 写在工程根）
- `tools/img2hex/output/`：`report.txt` 和 `assets_desc.h`
- 各 C 工程 `peripheral/APB0/gpu2d/`：`assets_desc.h`

不再生成 `preview_*.png` / `alpha_demo_*.png`。

## 规矩（改素材前先看）

1. α 只用固定档位（0 / 128 / 255）；连续 α 会把 256 项调色板拆爆，脚本会报错。
2. 尺寸对齐到 4，最大边 256；`--max` 触发缩放也会把 α 变成连续值。
3. 新素材加在目录顺序的最后（`PIC_DIRS_DEFAULT`），已有编号不动。
4. 不透明区不能有纯白（键色 = 白，会被 ck 抠掉）；脚本会自检。
5. **hex / palette 是 BRAM 初值**：改完必须重新综合；`assets_desc.h` 下标会变，必须重编固件，两者一起烧。
