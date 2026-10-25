#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""_gen_model.py —— game_room.c 生成器的 Python 镜像（只为验证规则能不能过，不参与构建）。

和 C 的对应关系：
  stage_cells() / room_place_L() / room_would_2x2() / room_try_specials()
  / room_boss_expand() / game_room_validate()
只用来统计：多少轮能同时满足所有规则、L 是否出现、兜底触发率。
"""
import random
import sys

REASONS = {}


def _fail(why):
    REASONS[why] = REASONS.get(why, 0) + 1
    return None


W = H = 7
N = W * H
NONE, START, EMPTY, ITEM, SHOP, BOSS = -1, 0, 1, 2, 3, 4


class Room:
    __slots__ = ("type", "x", "y", "owner", "w", "h")

    def __init__(self, x, y):
        self.type = NONE
        self.x, self.y = x, y
        self.owner = -1
        self.w = self.h = 1


def cid(x, y):
    return y * W + x


def build(start_dir_check=True):
    rooms = [Room(i % W, i // W) for i in range(N)]

    def canon(c):
        return rooms[c].owner if c >= 0 and rooms[c].owner >= 0 else c

    def multicell(c):
        cc = canon(c)
        return cc >= 0 and (rooms[cc].w > 1 or rooms[cc].h > 1 or rooms[c].owner >= 0)

    def neighbor(r, d):
        dx = (0, 1, 0, -1)[d]
        dy = (-1, 0, 1, 0)[d]
        x, y = rooms[r].x + dx, rooms[r].y + dy
        if not (0 <= x < W and 0 <= y < H):
            return -1
        return cid(x, y)

    def degree(r):
        seen = set()
        for d in range(4):
            nb = neighbor(r, d)
            if nb < 0 or rooms[nb].type == NONE:
                continue
            c = canon(nb)
            if c != canon(r):
                seen.add(c)
        return len(seen)

    def cell_present(x, y, cand):
        if not (0 <= x < W and 0 <= y < H):
            return False
        return cid(x, y) == cand or rooms[cid(x, y)].type != NONE

    def would_2x2(cand):
        x, y = rooms[cand].x, rooms[cand].y
        for dy in (-1, 0):
            for dx in (-1, 0):
                x0, y0 = x + dx, y + dy
                if x0 < 0 or y0 < 0 or x0 + 1 >= W or y0 + 1 >= H:
                    continue
                cs = [cid(x0, y0), cid(x0 + 1, y0), cid(x0, y0 + 1), cid(x0 + 1, y0 + 1)]
                if not all(cell_present(c % W, c // W, cand) for c in cs):
                    continue
                if any(multicell(c) for c in cs):
                    continue
                return True
        return False

    def would_cycle(cand):
        seen = set()
        for d in range(4):
            nb = neighbor(cand, d)
            if nb < 0 or rooms[nb].type == NONE:
                continue
            seen.add(canon(nb))
        return len(seen) >= 2

    reserved = set()

    def place_L(c00):
        x, y = rooms[c00].x, rooms[c00].y
        if x + 1 >= W or y + 1 >= H:
            return None
        c10, c11 = cid(x + 1, y), cid(x + 1, y + 1)
        if any(rooms[c].type != NONE for c in (c00, c10, c11)):
            return None
        c01 = cid(x, y + 1)
        if rooms[c01].type != NONE or c01 in reserved:
            return None
        if any(c in reserved for c in (c00, c10, c11)):
            return None
        reserved.add(c01)
        ext = set()
        for c in (c00, c10, c11):
            for d in range(4):
                nb = neighbor(c, d)
                if nb < 0 or nb in (c00, c10, c11) or rooms[nb].type == NONE:
                    continue
                ext.add(canon(nb))
        if len(ext) > 1:
            return None
        rooms[c00].type = EMPTY
        rooms[c00].w = rooms[c00].h = 2
        for c in (c10, c11):
            rooms[c].type = EMPTY
            rooms[c].w = rooms[c].h = 0
            rooms[c].owner = c00
        return (c00, c10, c11)

    # ---- 生长 ----
    start = cid(W // 2, H // 2)
    rooms[start].type = EMPTY
    order = [start]
    from_dir = {start: -1}
    target = 24
    l_left = 1
    guard = 0
    while len(order) < target and guard < 4000:
        guard += 1
        r = order[random.randrange(len(order))]
        d = random.randrange(4)
        if from_dir.get(r) == d and random.randrange(4) != 0:
            continue
        nb = neighbor(r, d)
        if nb < 0 or rooms[nb].type != NONE or nb in reserved:
            continue
        if l_left > 0 and random.randrange(3) == 0:
            res = place_L(nb)
            if res:
                for c in res:
                    from_dir[c] = d
                order.extend(res)
                l_left -= 1
                continue
        if would_cycle(nb):
            continue
        rooms[nb].type = EMPTY
        from_dir[nb] = d
        order.append(nb)
    if len(order) < target:
        return _fail("growth")
    rooms[start].type = START

    # ---- 距离 ----
    dist = [-1] * N
    dist[canon(start)] = 0
    q = [canon(start)]
    while q:
        r = q.pop(0)
        for d in range(4):
            nb = neighbor(r, d)
            if nb < 0 or rooms[nb].type == NONE:
                continue
            c = canon(nb)
            if dist[c] < 0:
                dist[c] = dist[r] + 1
                q.append(c)

    # ---- 特殊房 ----
    def near8(a, b):
        return abs(rooms[a].x - rooms[b].x) <= 1 and abs(rooms[a].y - rooms[b].y) <= 1

    def boss_near(other):
        for i in range(N):
            if rooms[i].type == BOSS and near8(i, other):
                return True
        return False

    leaves = []
    maxd = -1
    for r in order:
        if canon(r) != r or r == start or rooms[r].type == NONE:
            continue
        if rooms[r].w > 1 or rooms[r].h > 1:
            continue
        if degree(r) != 1:
            continue
        leaves.append(r)
        maxd = max(maxd, dist[r])
    if len(leaves) < 3 or maxd < 2:
        return _fail("leaf<3")

    anchors = []
    for i in range(N):
        if rooms[i].type == BOSS:
            anchors.append(i)

    def boss_expand(boss):
        for oy in (-1, 0):
            for ox in (-1, 0):
                x0, y0 = rooms[boss].x + ox, rooms[boss].y + oy
                if x0 < 0 or y0 < 0 or x0 + 1 >= W or y0 + 1 >= H:
                    continue
                block = [cid(x0, y0), cid(x0 + 1, y0), cid(x0, y0 + 1), cid(x0 + 1, y0 + 1)]
                if any(c != boss and (rooms[c].type != NONE or c in reserved) for c in block):
                    continue
                ext = set()
                for c in block:
                    for d in range(4):
                        nb = neighbor(c, d)
                        if nb < 0 or nb in block:
                            continue
                        if rooms[nb].type != NONE:
                            ext.add(canon(nb))
                if len(ext) != 1:
                    continue
                return block
        return None

    boss_ok = False
    for boss in sorted([l for l in leaves if dist[l] >= 2], key=lambda r: -dist[r]):
        for item in leaves:
            if item == boss or near8(boss, item):
                continue
            for shop in leaves:
                if shop in (boss, item) or near8(boss, shop) or near8(item, shop):
                    continue
                block = boss_expand(boss)
                if not block:
                    continue
                # 真正落子
                anchor = block[0]
                for c in block:
                    rooms[c].type = BOSS
                    if c == anchor:
                        rooms[c].w = rooms[c].h = 2
                        rooms[c].owner = -1
                    else:
                        rooms[c].w = rooms[c].h = 0
                        rooms[c].owner = anchor
                if boss_near(item) or boss_near(shop):
                    return _fail("boss_near")
                rooms[item].type = ITEM
                rooms[shop].type = SHOP
                boss_ok = True
                break
            if boss_ok:
                break
        if boss_ok:
            break
    if not boss_ok:
        return _fail("boss slot")

    # ---- validate ----
    bad = 0
    cnt = {START: 0, ITEM: 0, SHOP: 0, BOSS: 0}
    anchors_n = covered_n = 0
    for i in range(N):
        t = rooms[i].type
        if t == NONE:
            continue
        if t in cnt:
            if t == BOSS:
                if rooms[i].w == 2 and rooms[i].owner < 0:
                    cnt[BOSS] += 1
            else:
                cnt[t] += 1
        if t == BOSS:
            if rooms[i].w == 2 and rooms[i].owner < 0:
                anchors_n += 1
            elif rooms[i].w == 0 and rooms[i].owner >= 0 and rooms[rooms[i].owner].type == BOSS:
                covered_n += 1
            else:
                bad += 1
        elif rooms[i].owner >= 0:
            if rooms[rooms[i].owner].type == NONE:
                bad += 1
        elif not (1 <= rooms[i].w <= 2 and 1 <= rooms[i].h <= 2):
            bad += 1
    if cnt[START] != 1 or cnt[ITEM] != 1 or cnt[SHOP] != 1 or cnt[BOSS] != 1:
        bad += 1
    if anchors_n != 1 or covered_n != 3:
        bad += 1

    # 特殊房 8 邻域
    specials = [i for i in range(N) if rooms[i].type in (ITEM, SHOP, BOSS)]
    for a in range(len(specials)):
        for b in range(a + 1, len(specials)):
            i, j = specials[a], specials[b]
            if rooms[i].type == BOSS and rooms[j].type == BOSS:
                continue
            if near8(i, j):
                bad += 1
    # 叶子
    for i in range(N):
        t = rooms[i].type
        if t not in (ITEM, SHOP):
            continue
        if degree(i) != 1:
            bad += 1
            continue
        for d in range(4):
            nb = neighbor(i, d)
            if nb >= 0 and rooms[nb].type != NONE:
                if rooms[nb].type in (ITEM, SHOP, BOSS):
                    bad += 1
                break
    # boss 不直连起点
    for i in range(N):
        if rooms[i].type != BOSS:
            continue
        for d in range(4):
            if neighbor(i, d) == start:
                bad += 1
    # 2×2 回字
    for y in range(H - 1):
        for x in range(W - 1):
            cs = [cid(x, y), cid(x + 1, y), cid(x, y + 1), cid(x + 1, y + 1)]
            if any(rooms[c].type in (BOSS,) for c in cs):
                continue
            if any(rooms[c].owner >= 0 for c in cs):
                continue
            if all(rooms[c].type != NONE for c in cs):
                bad += 1
    # 环（回廊）
    parent = list(range(N))

    def find(x):
        while parent[x] != x:
            x = parent[x]
        return x

    pairs = set()
    for i in range(N):
        if rooms[i].type == NONE or canon(i) != i:
            continue
        for d in range(4):
            nb = neighbor(i, d)
            if nb < 0 or rooms[nb].type == NONE:
                continue
            a, b = canon(i), canon(nb)
            if a < b:
                pairs.add((a, b))
    for a, b in pairs:
        ra, rb = find(a), find(b)
        if ra == rb:
            bad += 1
        else:
            parent[ra] = rb

    # 连通性
    seen = {canon(start)}
    stack = [canon(start)]
    while stack:
        r = stack.pop()
        for d in range(4):
            nb = neighbor(r, d)
            if nb < 0 or rooms[nb].type == NONE:
                continue
            c = canon(nb)
            if c not in seen:
                seen.add(c)
                stack.append(c)
    for i in range(N):
        if rooms[i].type != NONE and canon(i) not in seen:
            bad += 1

    if bad:
        _fail("validate")
    return {
        "bad": bad,
        "l": any(rooms[i].w == 2 and rooms[i].h == 2 and rooms[i].type != BOSS for i in range(N)),
        "cells": sum(1 for i in range(N) if rooms[i].type != NONE),
        "boss_dist": maxd,
    }


def main():
    trials = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
    random.seed(12345)
    ok = fallback = fail = 0
    l_count = 0
    bad_total = 0
    for _ in range(trials):
        for _attempt in range(50):
            res = build()
            if res and res["bad"] == 0:
                ok += 1
                l_count += 1 if res["l"] else 0
                break
            bad_total += 1
        else:
            fallback += 1
    print("试验 %d 轮" % trials)
    print("  成功（50 次内通过全部规则）: %d (%.1f%%)" % (ok, 100.0 * ok / trials))
    print("  触发兜底（50 次都失败）    : %d (%.2f%%)" % (fallback, 100.0 * fallback / trials))
    print("  成功局里带 L 房的比例      : %d/%d" % (l_count, ok))
    print("  平均失败尝试数             : %.1f" % (bad_total / max(1, trials)))


if __name__ == "__main__":
    main()
