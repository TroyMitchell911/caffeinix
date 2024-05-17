/*
 * @Author: TroyMitchell
 * @Date: 2024-04-13
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-17
 * @FilePath: /caffeinix/arch/riscv/boot/timer.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include <riscv.h>
#include <mem_layout.h>
#include <kernel_config.h>

struct timer_buffer {
        /* 0, 8, 16 */
        uint64 save_regs[3];
        /* 24 */
        uint64 clint_mtimecmp;
        /* 32 */
        uint64 interval;
};

static struct timer_buffer timer_buff[NCPU];
extern void timer_vec(void);

void timer_init(uint8 hartid)
{
        struct timer_buffer *scratch;
        uint64 id;
        /* Read hatid */
        id = mhartid_r();
        /* Get the buffer corresponding to each hart */
        scratch = &timer_buff[id];
        /* About 100 ms */
        int interval = 10000 * TICK_INTERVAL;
        /* Write new CMP value */
        *(uint64*)CLINT_MTIMECMP(id) = *(uint64*)CLINT_MTIME + interval;
        /* Fill the data of buffer */
        scratch->clint_mtimecmp = (uint64)CLINT_MTIMECMP(id);
        scratch->interval = interval;
        /* Write the address of buffer into the register 'scratch' */
        mscratch_w((uint64)scratch);
        /* Write the address of interrupt entry into the register 'timer_vec' */
        mtvec_w((uint64)timer_vec);

        /* Enable the global interrupt in the machine-mode */
        mstatus_w(mstatus_r() | MSTATUS_MIE);
        /* Enable the timer interrupt */
        mie_w(mie_r() | MIE_MTIE);
}