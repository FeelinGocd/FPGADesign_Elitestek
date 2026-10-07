//==============================================================================
//  APB0_Decoder.v  ——  APB0 窗口内的地址译码（只翻片选）
//------------------------------------------------------------------------------
//  功能
//      窗口 0xf8100000 ~ 0xf810FFFF 按 4 KB 切槽，
//      看 PADDR[15:12] 翻出那一个外设的片选。
//
//  槽表
//      槽 0   0x0000   按键 KEY
//      槽 1   0x1000   空（原 DDR 自检，已摘除；显示通道已并入槽 2）
//      槽 2   0x2000   GPU_2D 一整块
//                      槽内由 APB0_gpu2d 按 PADDR[6] 分：
//                      0x2000 观测寄存器 / 0x2040 显示寄存器
//      槽 3~15 0x3000~ 扩展预留
//
//  加外设：照抄一段填自己的槽号，再到 APB0_Interconnect 里补三行回程选择
//==============================================================================
module APB0_Decoder (
    input  wire [15:0]  APB_0_PADDR,
    input  wire         APB_0_PSEL,

    output wire         PSEL_key,
    output wire         PSEL_gpu2d
);

//------------------------------------------------------------------------------
// 按键 KEY
//------------------------------------------------------------------------------
//0x0000   KEY_DATA
//0x0004   KEY_IRQ_EN
//0x0008   KEY_ID
/*Insert KEY decoder code there*/
assign PSEL_key = APB_0_PSEL && (APB_0_PADDR[15:12] == 4'b0000);
/***********************************/

//------------------------------------------------------------------------------
// GPU_2D  ★槽 2, 与 AXI 从口 S2/S3 同号
//   ★ 一整块: 显示通道 + 加速器都在里面, 槽内再由 GPU_2D 用 PADDR[6] 分子块
//------------------------------------------------------------------------------
//  ---- 子块 0: 0x2000 - 0x201F  加速器观测(全只读) ----
//0x2000   GPU2D_ID           RO     = 0x47503244 ("GP2D")
//0x2004   GPU2D_STATUS       RO     bit0 = BUSY(有命令没做完)
//0x2008   GPU2D_CMD_DONE     RO     已完成命令数  ← 软件用它精确等"画完了没"
//0x200C   GPU2D_FRAME_DONE   RO     已完成帧数(FENCE 完成次数)
//0x2010   GPU2D_WR_BEATS     RO     已写出的 AXI 拍数  ┐ 实测带宽
//0x2014   GPU2D_RD_BEATS     RO     已读回的 AXI 拍数  ┘
//0x2018   GPU2D_BUSY_CYC     RO     忙的周期数(算占用率)
//   ★ 全只读: 没有配置寄存器; 写进来一律忽略, 计数器只增不减(软件记快照)
//
//  ---- 子块 1: 0x2040 - 0x206F  显示通道 ----
//0x2040   LINEBUFFER_ID            RO     = 0x4C423031 ("LB01")
//0x2044   FRAMEA_BASE_ADDR_ACC    RW     完整帧·左半(加速器画的, 显示在读)
//0x2048   FRAMEA_BASE_ADDR_CPU    RW     完整帧·右半(CPU 画的, 显示在读)
//0x204C   FRAMEB_BASE_ADDR_ACC    RW     正在画·左半
//0x2050   FRAMEB_BASE_ADDR_CPU    RW     正在画·右半
//0x2054   FRAMEC_FULL_BASE_ADDR   RW     全屏模式·完整帧(加速器独占)
//0x2058   FRAMED_FULL_BASE_ADDR   RW     全屏模式·正在画
//0x205C   STATUS                  RW     bit0=ENABLE bit1=SPLIT
//0x2060   FPS_ACC                 RO     加速器帧率
//0x2064   FPS_CPU                 RO     CPU 帧率
//0x2068   DONE_CPU                RW1S   bit0: CPU 这半幅画完, 写1置位
//0x206C   DONE_ACC                RW1S   bit0: ACC 这半幅画完(加速器也可硬件置位)
//   ★ DONE_CPU / DONE_ACC 都是 RW1S(写 1 置位, 写 0 不动):
//       置位: CPU 写 1 / 加速器硬件置位
//       清零: 只有显示侧 Handler 在帧边界消费掉那一位时才清 ← 软件不许清
//     两位互相独立: acc 快就 acc 先翻, cpu 没画完就一直读它上一帧的地址
//
//  注: 槽 1 上原来例化的是 APB0_DDR_checker, 已整块摘除(源文件保留在 RTL 目录)。
/*Insert gpu2d decoder code there*/
assign PSEL_gpu2d = APB_0_PSEL && (APB_0_PADDR[15:12] == 4'b0010);

endmodule
