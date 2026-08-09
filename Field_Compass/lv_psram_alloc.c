/**
 * @file lv_psram_alloc.c
 * @brief LVGL LV_STDLIB_CUSTOM memory backend — allocates from PSRAM.
 *
 * Why this file exists (issue #164):
 *
 * LVGL's BUILTIN allocator declares its heap as a static array (`work_mem_int`)
 * placed in internal DRAM. With LV_MEM_SIZE at 160KB (raised from 128KB by #119
 * to fix an OOM crash in fcListPickerOpen) that single symbol occupied 163,840
 * bytes — 48% of the ESP32-S3's 333,760-byte `dram0_0_seg`. As of ESP32 core
 * 3.3.8 the link failed outright: "region `dram0_0_seg' overflowed by 4296 bytes".
 *
 * The board has 2MB of PSRAM and was using ~96KB of it (the LVGL draw buffers,
 * allocated in initLVGL() with the same heap_caps_malloc call used below). LVGL's
 * widget heap has no reason to live in internal DRAM: it is not DMA'd from, and
 * it is not touched from an ISR. Moving it frees 160KB of internal DRAM against
 * a 4KB shortfall.
 *
 * Selecting LV_STDLIB_CUSTOM in lv_conf.h means LVGL compiles no allocator of its
 * own and links against the eight functions below instead. The set mirrors
 * lvgl/src/stdlib/clib/lv_mem_core_clib.c — if LVGL is upgraded and that file
 * gains a function, this one must gain it too or the link will fail with an
 * undefined reference (loudly, which is the desired failure mode).
 *
 * LV_MEM_SIZE is inert under CUSTOM: there is no fixed pool, so LVGL's heap is
 * bounded by free PSRAM rather than by a compile-time constant. That gives #119's
 * failure path roughly 2MB of headroom instead of 160KB.
 */

#include <lvgl.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include <esp_heap_caps.h>

/* Counts allocations that could not be served from PSRAM and fell back to
 * internal DRAM. Non-zero means PSRAM is exhausted or unavailable — the
 * condition that preceded this bug. Exposed via lvPsramFallbackCount() so it can
 * be surfaced on the diagnostics screen rather than failing silently. */
static uint32_t psram_fallback_count = 0;

uint32_t lvPsramFallbackCount(void)
{
    return psram_fallback_count;
}

void lv_mem_init(void)
{
    return; /*Nothing to init — heap_caps is brought up by the ESP-IDF startup code*/
}

void lv_mem_deinit(void)
{
    return; /*Nothing to deinit*/
}

lv_mem_pool_t lv_mem_add_pool(void * mem, size_t bytes)
{
    /*Not supported — heap_caps owns the pool*/
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    /*Not supported*/
    LV_UNUSED(pool);
    return;
}

void * lv_malloc_core(size_t size)
{
    void * p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if(p == NULL) {
        /*PSRAM exhausted or unavailable. Fall back to internal DRAM so the UI
         *degrades instead of dying, but count it — a non-zero counter is the
         *signal that the PSRAM assumption no longer holds.*/
        psram_fallback_count++;
        p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return p;
}

void * lv_realloc_core(void * p, size_t new_size)
{
    void * q = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM);
    if(q == NULL && new_size > 0) {
        psram_fallback_count++;
        q = heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
    }
    return q;
}

void lv_free_core(void * p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t * mon_p)
{
    /*Report PSRAM state so LVGL's monitor reflects the heap actually in use.*/
    if(mon_p == NULL) return;

    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);

    lv_memzero(mon_p, sizeof(lv_mem_monitor_t));
    mon_p->total_size = info.total_free_bytes + info.total_allocated_bytes;
    mon_p->free_cnt   = info.free_blocks;
    mon_p->free_size  = info.total_free_bytes;
    mon_p->free_biggest_size = info.largest_free_block;
    mon_p->used_cnt   = info.allocated_blocks;
    mon_p->used_pct   = mon_p->total_size ? (uint8_t)((info.total_allocated_bytes * 100) / mon_p->total_size) : 0;
    mon_p->frag_pct   = info.total_free_bytes ? (uint8_t)(100 - ((info.largest_free_block * 100) / info.total_free_bytes)) : 0;
}

lv_result_t lv_mem_test_core(void)
{
    /*heap_caps has its own integrity checking; report on the PSRAM heap.*/
    return heap_caps_check_integrity(MALLOC_CAP_SPIRAM, false) ? LV_RESULT_OK : LV_RESULT_INVALID;
}

#endif /*LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM*/
