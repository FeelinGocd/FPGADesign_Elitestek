//==============================================================================
//  game_frame.h —— 标准矩形房间框架（1×1 小房间 / 2×2 大房间）
//------------------------------------------------------------------------------
//  干什么      :
//    · 房间的墙 / 地板 / 角块 / 门其实只有十来个参数，不用整张 45×35 的表去存：
//      两张框架从 6370 B 降到几十 B，加新房间只要加一行参数；
//    · 这里只放"参数 + 两个纯函数"，不依赖任何硬件头文件 —— PC 上也能编译，
//      可以直接和 tools/map_designer 导出的图逐格比对。
//
//  坐标系      : 格（32 px），(0,0) = 画布左上角；'.' = 黑边。
//
//  生成规则    :
//    · 竞技场 ax0..ax1 × ay0..ay1 = 地板 'B'（含边界格）；
//    · 竞技场外扩 2 格的一圈 = 墙 'W'，再外面 = '.'（黑边）；
//    · 四扇门各占 1 格 'D'（在墙带里），它的"另一半"是 '.'：
//        N 门：x = door[0]，y = ay0-2，另一半在下一行；
//        E 门：y = door[1]，x = ax1+1，另一半在右一格；
//        S 门：x = door[2]，y = ay1+1，另一半在下一行；
//        W 门：y = door[3]，x = ax0-2，另一半在右一格；
//      door[d] = 0xFF 表示这条边没有门。
//    · corner != 0 时四角放 X / C / Z / Y（只影响贴图翻转，不影响碰撞）；
//    · 墙的木纹角度按边取 band_rot[N / E / S / W]；墙角处"外圈那一行/列"优先。
//
//  和设计器的关系:
//    tools/map_designer 的「小房间 40×23 / 大房间 45×35」模板生成的就是同一套
//    规则；check 的方法见 tools/map_designer/README.md（导出后逐格对比）。
//==============================================================================
#ifndef GAME_FRAME_H
#define GAME_FRAME_H

#define FRAME_DOOR_NONE 0xFFu

typedef struct {
    unsigned char cols, rows;        /* 画布格数（40×23 / 45×35） */
    unsigned char ax0, ay0;          /* 竞技场左上（格，含） */
    unsigned char ax1, ay1;          /* 竞技场右下（格，含） */
    unsigned char door[4];           /* N 的 x / E 的 y / S 的 x / W 的 y；0xFF = 没有 */
    unsigned char band_rot[4];       /* 上 / 右 / 下 / 左 的木纹角度（0..3） */
    unsigned char corner;            /* 1 = 四角放 X/C/Z/Y 角块，0 = 纯木墙 */
    /*  ★ 贴图种类（"皮肤"）：几何完全一样，只换字符。0 = 用默认值。
     *    floor: 'B' 木 / 'K' 隐藏房地板；wall: 'W' 木墙 / 'w' 隐藏房墙；
     *    door: 'D' 普通门 / 'H' 隐藏门；corner: 0 = C/X/Y/Z 家族 / 'c' = c/x/y/z 家族。
     *    有了这四个字段，新房间只要换皮肤，不用再存一张 3.2 KB 的地形表。  */
    unsigned char floor_ch;          /* 0 = 'B' */
    unsigned char wall_ch;           /* 0 = 'W' */
    unsigned char door_ch;           /* 0 = 'D' */
    unsigned char corner_ch;         /* 0 = 'C' 家族（C/X/Y/Z）；'c' = 隐藏房家族（c/x/y/z） */
} frame_geom_t;

/*  这一格是什么字符：'.' 黑边 / 地板 / 墙 / 门 / 'C''X''Y''Z' 角块
 *  （地板/墙/门的具体字符由 frame_geom_t 的 floor_ch/wall_ch/door_ch 决定，
 *    默认 'B' / 'W' / 'D'）  */
char frame_tile(const frame_geom_t *g, int x, int y);

/*  这一格的旋转（0..3）：墙的木纹方向 / 门的方向  */
unsigned char frame_rot(const frame_geom_t *g, int x, int y);

#endif /* GAME_FRAME_H */
