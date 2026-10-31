//==============================================================================
//  render_list.c —— 渲染队列的实现（说明见 render_list.h）
//------------------------------------------------------------------------------
//  数据结构:
//    s_items[]  登记表（z / arg / 画法），数组下标就是提交顺序
//    s_order[]  排序用的下标表（2 字节/项，移动它比移动 12 字节的结构便宜）
//  排序:
//    · 先看一眼是不是已经有序（子弹每帧只挪几像素时几乎总是有序）→ O(n)
//    · 无序时对下标做两趟稳定基数排序
//    · 同 z 用提交下标兜底 ⇒ 与 push 顺序一致，画面不会逐帧抖
//==============================================================================
#include "render_list.h"

/* 热点队列保持紧凑：每个元素直接存函数指针要多 4 字节，
 * 而一帧通常只复用不到 16 种绘制回调。 */
static short          s_z[RL_MAX];
/* 5 位回调编号 + 11 位实体下标共用同一个 16 位载荷。 */
static u16            s_payload[RL_MAX];
static rl_draw_fn     s_fns[32];
static unsigned char  s_fn_n;
static u16       s_order[RL_MAX];
static u16       s_radix_count[256];
static int       s_n;           /* 本帧登记了几个 */
static u32       s_dropped;     /* 累计丢了多少个（正常恒 0） */

/*----------------------------------------------------------------------------*/
void rl_begin(void)
{
    s_n = 0;
}

/*----------------------------------------------------------------------------*/
int rl_register_draw(rl_draw_fn fn)
{
    int i;
    if (fn == 0) return -1;
    for (i = 0; i < s_fn_n; ++i)
        if (s_fns[i] == fn) break;
    if (i >= 32) return -1;
    if (i == s_fn_n) s_fns[s_fn_n++] = fn;
    return i;
}

int rl_push_id(int z, int draw_id, int arg)
{
    if (s_n >= RL_MAX || (unsigned)draw_id >= s_fn_n || (unsigned)arg > RL_ARG_MAX || z < -32768 || z > 32767) {
        ++s_dropped;
        return 0;
    }
    s_z[s_n] = (short)z;
    s_payload[s_n] = (u16)((draw_id << RL_ARG_BITS) | arg);
    ++s_n;
    return 1;
}

int rl_push(int z, rl_draw_fn fn, int arg)
{
    /* 被拒绝的元素不能占用回调表项。 */
    if (s_n >= RL_MAX || fn == 0 || (unsigned)arg > RL_ARG_MAX || z < -32768 || z > 32767) {
        ++s_dropped;
        return 0;
    }
    return rl_push_id(z, rl_register_draw(fn), arg);
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
    if (s_z[a] != s_z[b]) return s_z[a] < s_z[b];
    return a < b;
}

/*----------------------------------------------------------------------------*/
/*  已经按 z 非降序排好了吗（常见情况：这一帧没有元素互相穿过） */
static int rl_already_sorted(void)
{
    int i;

    for (i = 1; i < s_n; ++i)
        if (s_z[i] < s_z[i - 1]) return 0;

    return 1;
}

/*----------------------------------------------------------------------------*/
static void rl_sift(int root, int end)
{
    u16 value = s_order[root];
    int child;

    while ((child = root * 2 + 1) < end) {
        if (child + 1 < end && rl_before(s_order[child], s_order[child + 1])) ++child;
        if (!rl_before(value, s_order[child])) break;
        s_order[root] = s_order[child];
        root = child;
    }
    s_order[root] = value;
}

/* Scratch is live only while sorting, before any drawing callback executes.
 * Keep this frame separate so callbacks do not inherit its stack footprint. */
static __attribute__((noinline, optimize("O2"))) void rl_sort(void)
{
    int i, b;
    u16 radix_tmp[RL_MAX];
    u16 *in = s_order;
    u16 *out = radix_tmp;

    for (i = 0; i < s_n; ++i) s_order[i] = (u16)i;

    if (s_n <= 1 || rl_already_sorted()) return;

    /* 对 z+32768 做稳定的 LSD 基数排序，以提交顺序作为同 z 的次序。
     * 结果与旧堆排序一致，但数百发弹幕帧不再承担 O(n log n) 次比较。 */
    for (b = 0; b < 2; ++b) {
        int shift = b * 8;
        for (i = 0; i < 256; ++i) s_radix_count[i] = 0;
        for (i = 0; i < s_n; ++i) {
            int key = (int)s_z[in[i]] + 32768;
            ++s_radix_count[(key >> shift) & 255];
        }
        {
            u16 sum = 0;
            for (i = 0; i < 256; ++i) {
                u16 n = s_radix_count[i];
                s_radix_count[i] = sum;
                sum = (u16)(sum + n);
            }
        }
        for (i = 0; i < s_n; ++i) {
            int key = (int)s_z[in[i]] + 32768;
            int bucket = (key >> shift) & 255;
            out[s_radix_count[bucket]++] = in[i];
        }
        {
            u16 *tmp = in;
            in = out;
            out = tmp;
        }
    }
    if (in != s_order)
        for (i = 0; i < s_n; ++i) s_order[i] = in[i];
}

/*----------------------------------------------------------------------------*/
void rl_draw_all(u32 fb, u32 now, int buf)
{
    int i;

    rl_sort();

    for (i = 0; i < s_n; ++i) {
        u16 idx = s_order[i];
        u16 payload = s_payload[idx];
        s_fns[payload >> RL_ARG_BITS](fb, now, buf, (int)(payload & RL_ARG_MAX));
    }
}
