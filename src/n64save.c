/* n64save.c - the saved games' home: 256 Kbit of battery-backed SRAM on the
 * cartridge, the kind Ocarina of Time used.
 *
 * SRAM sits in the PI bus's second domain at 0x08000000 and is read and
 * written with DMA transfers. That domain's bus timing has to be set for it
 * first; the values are the ones commercial SRAM games program. The ROM
 * header asks for SRAM (N64_ROM_SAVETYPE in the Makefile), which is how
 * emulators and flash carts know to provide it.
 */
#include <string.h>
#include <libdragon.h>
#include "wolf.h"

#define SRAM_ADDRESS     0x08000000

#define PI_BSD_DOM2_LAT  ((volatile uint32_t *)0xA4600024)
#define PI_BSD_DOM2_PWD  ((volatile uint32_t *)0xA4600028)
#define PI_BSD_DOM2_PGS  ((volatile uint32_t *)0xA460002C)
#define PI_BSD_DOM2_RLS  ((volatile uint32_t *)0xA4600030)

static void sram_timing(void)
{
    *PI_BSD_DOM2_LAT = 0x05;
    *PI_BSD_DOM2_PWD = 0x0c;
    *PI_BSD_DOM2_PGS = 0x0d;
    *PI_BSD_DOM2_RLS = 0x02;
}

bool save_medium_read(uint8_t *buf)
{
    sram_timing();
    dma_read_async(buf, SRAM_ADDRESS, SAVE_MEDIUM_BYTES);
    dma_wait();
    return true;
}

/* Written, then read back: a cartridge without SRAM reads back open bus,
 * and the save is reported as failed instead of silently lost. */
bool save_medium_write(const uint8_t *buf)
{
    static uint8_t check[SAVE_MEDIUM_BYTES] __attribute__((aligned(8)));

    sram_timing();
    data_cache_hit_writeback(buf, SAVE_MEDIUM_BYTES);
    dma_write_raw_async(buf, SRAM_ADDRESS, SAVE_MEDIUM_BYTES);
    dma_wait();

    dma_read_async(check, SRAM_ADDRESS, SAVE_MEDIUM_BYTES);
    dma_wait();
    return memcmp(check, buf, SAVE_MEDIUM_BYTES) == 0;
}
