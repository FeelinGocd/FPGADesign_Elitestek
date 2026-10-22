//==============================================================================
//  render_list.c —— 渲染队列的实现（说明见 render_list.h）
//------------------------------------------------------------------------------
//  数据结构:
//    s_items[]  登记表（z / seq / arg / 画法），按 push 顺序摆
//    s_order[]  排序用的下标表（2 字节/项，移动它比移动 12 字节的结构便宜）
//  排序:
//    · 先看一眼是不是已经有序（子弹每帧只挪几像素时几乎总是有序）→ O(n)
//    · 真乱了才二分插入排序：比较 O(n log n)，只有逆序的部分要搬
//    · 同 z 用 seq 兜底 ⇒ 与 push 顺序一致（稳定），画面不会逐帧抖
//==============================================================================
#include "render_list.h"

typedef struct {
    int        z;       /* 层号：越大越靠上 */
    u16        seq;     /* 本帧的提交序号（同 z 时用） */
    u16        arg;     /* 元素下标（子弹槽号等） */
    rl_draw_fn fn;      /* 这个元素怎么画 */
} rl_item_t;

static rl_item_t s_items[RL_MAX];
static u16       s_order[RL_MAX];
static u16       s_tmp[RL_MAX];  /* 归并排序的临时下标表（稳定，O(n log n)） */
static int       s_n;           /* 本帧登记了几个 */
static u16       s_seq;         /* 本帧已经发了几个序号 */
static u32       s_dropped;     /* 累计丢了多少个（正常恒 0） */

/*----------------------------------------------------------------------------*/
void rl_begin(void)
{
    s_n = 0;
    s_seq = 0u;
}

/*----------------------------------------------------------------------------*/
int rl_push(int z, rl_draw_fn fn, int arg)
{
    rl_item_t *it;

    if (s_n >= RL_MAX || fn == 0) {
        ++s_dropped;
        return 0;
    }

    it = &s_items[s_n];
    it->z   = z;
    it->seq = s_seq++;
    it->arg = (u16)arg;
    it->fn  = fn;
    ++s_n;
    return 1;
}

/*----------------------------------------------------------------------------*/
int rl_count(void)
{
    return s_n;
}

/*----------------------------------------------------------------------------*/
u32 rl_dropped(void)
{
    return s_dropped;
}

/*----------------------------------------------------------------------------*/
/*  a 应该排在 b 前面吗（z 小优先；同 z 看提交顺序） */
static int rl_before(u16 a, u16 b)
{
    const rl_item_t *ia = &s_items[a];
    const rl_item_t *ib = &s_items[b];

    if (ia->z != ib->z) return ia->z < ib->z;
    return ia->seq < ib->seq;
}

/*----------------------------------------------------------------------------*/
/*  已经按 z 非降序排好了吗（常见情况：这一帧没有元素互相穿过） */
static int rl_already_sorted(void)
{
    int i;

    for (i = 1; i < s_n; ++i)
        if (s_items[i].z < s_items[i - 1].z) return 0;

    return 1;
}

/*----------------------------------------------------------------------------*/
static void rl_sort(void)
{
    int i, width;

    for (i = 0; i < s_n; ++i) s_order[i] = (u16)i;

    if (s_n <= 1 || rl_already_sorted()) return;

    /*  自底向上稳定归并：同 z 时左段优先 ⇒ 保持 push 顺序。
     *  旧的二分插入最坏会 O(n²) 搬下标；这里固定 O(n log n)。  */
    for (width = 1; width < s_n; width <<= 1) {
        int left;

        for (left = 0; left < s_n; left += (width << 1)) {
            int mid = left + width;
            int right = left + (width << 1);
            int a, b, k;

            if (mid > s_n) mid = s_n;
            if (right > s_n) right = s_n;

            a = left;
            b = mid;
            k = left;
            while (a < mid && b < right) {
                if (rl_before(s_order[b], s_order[a])) s_tmp[k++] = s_order[b++];
                else                                  s_tmp[k++] = s_order[a++];
            }
            while (a < mid)   s_tmp[k++] = s_order[a++];
            while (b < right) s_tmp[k++] = s_order[b++];
        }
        for (i = 0; i < s_n; ++i) s_order[i] = s_tmp[i];
    }
}

/*----------------------------------------------------------------------------*/
void rl_draw_all(u32 fb, u32 now, int buf)
{
    int i;

    rl_sort();

    for (i = 0; i < s_n; ++i) {
        const rl_item_t *it = &s_items[s_order[i]];

        it->fn(fb, now, buf, (int)it->arg);
    }
}
