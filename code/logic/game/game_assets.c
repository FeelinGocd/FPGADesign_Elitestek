//==============================================================================
//  game_assets.c —— 素材表的唯一一份实例
//------------------------------------------------------------------------------
//  干什么      : 给全工程提供唯一的素材描述表指针。
//  上一级是谁  : game/game.h（其它模块通过 g_assets[i] 访问）
//  接口契约    : 只有本文件可以碰 assets_desc.h 里那张 static 表。
//  为什么单独一个文件:
//    · assets_desc.h 是 pic2pram.py 生成的，里面是
//          static const asset_desc_t g_assets[] = { ... };
//      表项只存四个 16 位数（8 字节）；名称在生成注释与 report.txt 中，
//      不再为游戏从不读取的 name 指针和字符串占 RAM。
//      当前 105 项共 840 字节；多个 .c 各放一份仍会浪费片上 RAM。
//      ⇒ 让本文件拿着那一份，别的模块通过 game_asset_table() 访问同一份。
//  参考        : tools/img2hex/pic2pram.py（生成的表）、docs/spec/硬件_加速器.md §7.3
//==============================================================================
#include "type.h"
#include "assets_desc.h"     /* 注意：这里【不能】包含 game.h，否则宏会把 g_assets 替换掉 */

_Static_assert(sizeof(asset_desc_t) == 8, "PRAM asset descriptor must stay 8 bytes");

const asset_desc_t *const g_assets_ptr = g_assets;

const asset_desc_t *game_asset_table(void)
{
    return g_assets;         /* 本文件里的这一份就是全工程唯一的一份 */
}
