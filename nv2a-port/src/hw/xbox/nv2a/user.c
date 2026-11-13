/*
 * QEMU Geforce NV2A implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2021 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "nv2a_int.h"
unsigned nv2a_user_reads;

/* USER - PFIFO MMIO and DMA submission area */
uint64_t user_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    unsigned int channel_id = addr >> 16;
    assert(channel_id < NV2A_NUM_CHANNELS);

    /* nv2a-port: GET/PUT/REF without the PFIFO lock. A title spinning on
     * GET (D3D waiting for the GPU to pass a fence) took the lock once per
     * poll, tens of thousands of times a frame, and the PFIFO thread that
     * has to advance GET kept blocking on it. Aligned 32-bit loads; the
     * value may be a moment old, as on hardware. NV2A_USER_LOCKED=1. */
    static int locked = -1;
    if (locked < 0) locked = getenv("NV2A_USER_LOCKED") != NULL;
    if (!locked) {
        extern unsigned nv2a_user_reads;
        nv2a_user_reads++;
        uint32_t modes = __atomic_load_n(&d->pfifo.regs[NV_PFIFO_MODE], __ATOMIC_RELAXED);
        uint32_t push1 = __atomic_load_n(&d->pfifo.regs[NV_PFIFO_CACHE1_PUSH1], __ATOMIC_RELAXED);
        if ((modes & (1 << channel_id)) &&
            channel_id == GET_MASK(push1, NV_PFIFO_CACHE1_PUSH1_CHID)) {
            uint64_t v = 0;
            switch (addr & 0xFFFF) {
            case NV_USER_DMA_PUT:
                v = __atomic_load_n(&d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT], __ATOMIC_ACQUIRE);
                break;
            case NV_USER_DMA_GET:
                v = __atomic_load_n(&d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET], __ATOMIC_ACQUIRE);
                break;
            case NV_USER_REF:
                v = __atomic_load_n(&d->pfifo.regs[NV_PFIFO_CACHE1_REF], __ATOMIC_ACQUIRE);
                break;
            default:
                break;
            }
            nv2a_reg_log_read(NV_USER, addr, size, v);
            return v;
        }
    }

    qemu_mutex_lock(&d->pfifo.lock);

    uint32_t channel_modes = d->pfifo.regs[NV_PFIFO_MODE];

    uint64_t r = 0;
    if (channel_modes & (1 << channel_id)) {
        /* DMA Mode */

        unsigned int cur_channel_id =
            GET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_PUSH1],
                     NV_PFIFO_CACHE1_PUSH1_CHID);

        if (channel_id == cur_channel_id) {
            switch (addr & 0xFFFF) {
            case NV_USER_DMA_PUT:
                r = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT];
                break;
            case NV_USER_DMA_GET:
                r = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
                break;
            case NV_USER_REF:
                r = d->pfifo.regs[NV_PFIFO_CACHE1_REF];
                break;
            default:
                break;
            }
        } else {
            /* ramfc */
            assert(!"Unsupported: channel_id != cur_channel_id");
        }
    } else {
        /* PIO Mode */
        assert(!"Failed to enter DMA mode - entered PIO mode");
    }

    qemu_mutex_unlock(&d->pfifo.lock);

    nv2a_reg_log_read(NV_USER, addr, size, r);
    return r;
}

void user_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_USER, addr, size, val);

    unsigned int channel_id = addr >> 16;
    assert(channel_id < NV2A_NUM_CHANNELS);

    qemu_mutex_lock(&d->pfifo.lock);

    uint32_t channel_modes = d->pfifo.regs[NV_PFIFO_MODE];
    if (channel_modes & (1 << channel_id)) {
        /* DMA Mode */
        unsigned int cur_channel_id =
            GET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_PUSH1],
                     NV_PFIFO_CACHE1_PUSH1_CHID);

        if (channel_id == cur_channel_id) {
            switch (addr & 0xFFFF) {
            case NV_USER_DMA_PUT:
                { extern void nv2a_tl(int, uint32_t); nv2a_tl(3, (uint32_t)val); }
                d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT] = val;
                break;
            case NV_USER_DMA_GET:
                d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET] = val;
                break;
            case NV_USER_REF:
                d->pfifo.regs[NV_PFIFO_CACHE1_REF] = val;
                break;
            default:
                NV2A_DPRINTF(true, "Unsupported NV_USER write: channel=%u offset=0x%04x\n", channel_id, (unsigned)(addr & 0xFFFF));
                assert(!"Unsupported NV_USER DMA register write offset");
                break;
            }

            pfifo_kick(d);

        } else {
            /* ramfc */
            assert(!"Invalid channel id");
        }
    } else {
        /* PIO Mode */
        assert(!"Failed to enter DMA mode - entered PIO mode");
    }

    qemu_mutex_unlock(&d->pfifo.lock);

}
