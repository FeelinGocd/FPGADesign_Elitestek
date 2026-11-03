# 道具设计器

本地网页：预览/编辑道具，保存 CSV，直接重生成 `docs/spec/道具预览.png` 和规范表，并生成实现请求。

设计器现在是**一个目录**：预览渲染、CSV、服务器、页面都在 `tools/item_designer/`；不再依赖 `tools/item_preview/`。

## 启动

在仓库根目录、普通终端里运行：

    python tools/item_designer/item_designer.py

然后打开：

    http://127.0.0.1:8765

如果 8765 被占用：

    python tools/item_designer/item_designer.py --port 8766

不想自动打开浏览器：

    python tools/item_designer/item_designer.py --no-open

需要 Pillow（仓库环境已有；没有就 `python -m pip install pillow`）。

## 只生成预览 PNG / C 片段（不走网页）

    python tools/item_designer/preview.py
    python tools/item_designer/preview.py --emit-c

- 数据：`tools/item_designer/items.csv`
- 输出：`docs/spec/道具预览.png`

## 文件

| 文件 | 作用 |
|---|---|
| `items.csv` | 唯一道具数据源 |
| `items.example.csv` | items.csv 缺失时读取的默认数据（保存时才写 items.csv）；不是硬编码 |
| `preview.py` | CSV→PNG / C 片段（原 item_preview 合并进来） |
| `item_designer.py` | 本地网页后端（原 `server.py`） |
| `index.html` | 网页前端 |
| `design_brief.md` | 设计说明（保存时生成） |
| `pending_request.md` | 提交实现时生成 |
| `designer_state.json` | 上次保存快照（生成） |

## 页面里做什么

- 编辑 C 名称 / 字幕 / 图标 / 弹种 / 回血 / 加命 / 增加弹幕 / 移速 / 射速 / 弹速 / 射程 / 伤害 / 备注；
- 射程：普通弹 1 点 = 32 px；MagicCard 追踪时 1 点 = 0.8 s 存活时间；玩家基础 5；
- 每张卡下方“功能标签”：点 + 添加一条，点 × 删除；**属性的加减仍用上面的数值字段**；
- 每张卡有“品质”（普通/稀有/史诗/传说，固定）和“道具池”开关；`池=0` 只设计、不随机/不进商店；
- 品质权重：普通 100 / 稀有 45 / 史诗 15 / 传说 4；最终权重 = 基础 2/1 × 品质权重；
- `life_plus=1` 是“生命保险”：死亡判定时按拾取顺序找第一个，消耗并回上一房间；MagicCard 已实装；
- 当前：8 件已实装道具全部 `pool=1`；
- 游戏里按 Y 在脚下生成一个道具；R 仍重开；其它旧快捷键已移除；
- **自动草稿**：编辑时把未保存内容存到浏览器 localStorage；刷新/误关后自动恢复，点“丢弃草稿”才放弃；
- **改了 `item_designer.py` 必须重启 Python 服务**；只刷新网页不会重启后端。旧后端缺少 `level/pool` 时页面会提示，并禁止保存以免丢列；
- 底图 + 图标实时合成预览，右侧看整张规范 PNG；
- **保存 CSV + 生成 PNG**：
  - 写 `tools/item_designer/items.csv`
  - 重新生成 `docs/spec/道具预览.png`
  - 自动更新 `docs/spec/道具_规范.md` 的 `ITEM_TABLE` 标记区
- **提交实现**：
  - 先保存上面的文件
  - 生成 `tools/item_designer/pending_request.md`（设计说明 + diff + 每项 note + C 片段 + 实现清单）
  - 页面弹窗可复制请求；也发浏览器桌面通知
- 设计说明保存到 `tools/item_designer/design_brief.md`
- 特殊功能描述模板：`docs/spec/道具_规范.md` 第 11 节；页面顶部“插入特殊功能模板”可一键填入

## 为什么没有 DEFAULT_ROWS

旧版 server.py 里硬编码了 4 件道具作为 CSV 缺失时的 fallback，会与 CSV/C 漂移（候选道具、改过的字幕都会丢）。
现在：

- **CSV 是唯一数据源**；
- `items.csv` 缺失时只读同目录的 `items.example.csv` 作为默认数据（还是数据文件，不是 Python 列表）；保存时才写 `items.csv`；
- 两个都没有就返回空列表，由页面“新增道具”创建。

## 提交后怎么让 AI 实现

页面不能直接给对话发消息；提交后回到对话，发送：

    请读取 tools/item_designer/pending_request.md 并实现道具。

AI 会读 CSV / 说明 / diff / C 片段，改 `game.h`、`game_item.c`，必要时重跑 PRAM 和重编固件。

## 注意

- 只监听 127.0.0.1，不对外网开放。
- 启用道具的“字幕”只能 A-Z 和空格（16×24 字模限制）。
- 图标要 32×32 RGBA；新增图标先放进 `res/pic/accessory/` 并跑 `uv run tools/img2hex/pic2pram.py`。
- `triple=1` 现在数据驱动（每次增加一条弹道）；`coins` 表示拾取时一次性增加的金币数；`hp_max` 用于调整生命上限。
- 保存会改仓库文件；请在普通终端运行，不要放在 DSH 的沙箱 shell 里跑（沙箱只读时保存会失败）。
