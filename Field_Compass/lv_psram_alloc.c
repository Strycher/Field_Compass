/**
 * @file lv_psram_alloc.c
 * @brief LVGL LV_STDLIB_CUSTOM memory backend — allocates from PSRAM.
 *
 * Why this file exists (issue #164):
 *
 * LVGL's BUILTIN allocator declares its heap as a static array (`work_mem_int`)
 * placed in internal DRAM. With LV_MEM_SIZE at 160KB (raised from 128KB by #119
 * to fix an OOM crash in fcListPickerOpen) that single symbol occupied 163,840
 * bytes — 48% of the ESP32-S3's 333.8KB `dram0_0_seg`. As of ESP32 core 3.3.8
 * the link failed outright: "region `dram0_0_seg' overflowed by 4296 bytes".
 *
 * The board has 2MB of PSRAM and was using ~96KB of it (the LVGL draw buffers,
 * allocated in initLVGL() with the same heap_caps_malloc call used below).
 * Moving LVGL's widget heap there frees 160KB of internal DRAM against a 4KB
 * shortfall: DRAM went from over-by-4,296 to 103,096 bytes used (31%).
 *
 * Selecting LV_STDLIB_CUSTOM in lv_conf.h means LVGL compiles no allocator of
 * its own and links against the functions below instead. The set mirrors
 * lvgl/src/stdlib/clib/lv_mem_core_clib.c for LVGL 9.5.0 — see the version
 * assertion below.
 *
 * LV_MEM_SIZE is inert under CUSTOM: there is no fixed pool, so LVGL's heap is
 * bounded by free PSRAM rather than by a compile-time constant. That gives
 * #119's failure path substantially more headroom than 160KB.
 *
 * UNVERIFIED ON HARDWARE (tracked in #164's acceptance criteria):
 *   - UI responsiveness. QSPI PSRAM has higher latency than internal SRAM, and
 *     a widget heap is a pointer-chasing, small-allocation workload. The S3
 *     cache should absorb most of it, but this is a claim only a device can
 *     settle. Measure redraw timing against the pre-change baseline.
 *   - Allocation from interrupt context. LVGL is driven from the main loop here
 *     and lv_timer callbacks run from lv_timer_handler(), not from an ISR — but
 *     that has not been exhaustively proven across the library. PSRAM is not
 *     accessible from an ISR while the cache is disabled, so if any allocation
 *     ever happens in that context it will fault rather than degrade.
 */

#include <lvgl.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include <esp_heap_caps.h>
#include <stdatomic.h>
#include <string.h>

/* The function set below is the LVGL 9.5.0 contract. If LVGL is upgraded and it
 * expects a function that is not here, the link fails with an undefined
 * reference — loud, which is what we want. The reverse case (LVGL adding a
 * *weakly* defined hook) would link silently, so pin the expectation here: if
 * this assertion trips after an upgrade, re-diff against
 * lvgl/src/stdlib/clib/lv_mem_core_clib.c before assuming this file is complete. */
#if LVGL_VERSION_MAJOR != 9 || LVGL_VERSION_MINOR != 5
#warning "lv_psram_alloc.c was written against LVGL 9.5.x — re-verify the LV_STDLIB_CUSTOM function set against lv_mem_core_clib.c for this version (#164)."
#endif

/* Counts allocations that could not be served from PSRAM and fell back to
 * internal DRAM. Non-zero means PSRAM is exhausted or unavailable — the
 * condition that preceded this bug. Atomic because LVGL allocation is not
 * guaranteed to be single-task; a lost increment would under-report memory
 * pressure, which is the one thing this counter exists to catch. */
static atomic_uint_fast32_t psram_fallback_count = 0;

uint32_t lvPsramFallbackCount(void)
{
    return (uint32_t)atomic_load(&psram_fallback_count);
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
         *degrades rather than dying outright — though note LVGL's own handling
         *of a NULL return is not uniformly graceful, so a fallback that also
         *fails may still crash. The counter is the early warning.*/
        atomic_fetch_add(&psram_fallback_count, 1);
        p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return p;
}

void * lv_realloc_core(void * p, size_t new_size)
{
    void * q = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM);
    if(q != NULL || new_size == 0) return q;

    /*PSRAM realloc failed. Do NOT hand the original PSRAM pointer to a second
     *heap_caps_realloc with different caps — cross-heap realloc is not a
     *documented guarantee, and relying on it risks a leak or a double free.
     *Do the move explicitly: allocate in internal DRAM, copy, free the original.*/
    atomic_fetch_add(&psram_fallback_count, 1);

    void * n = heap_caps_malloc(new_size, MALLOC_CAP_8BIT);
    if(n == NULL) return NULL; /*Original block left intact and still owned by the caller.*/

    if(p != NULL) {
        size_t old_size = heap_caps_get_allocated_size(p);
        size_t copy = (old_size && old_size < new_size) ? old_size : new_size;
        memcpy(n, p, copy);
        heap_caps_free(p);
    }
    return n;
}

void lv_free_core(void * p)
{
    /*heap_caps_free resolves the owning heap from the block header, so this is
     *correct for blocks from either capability.*/
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t * mon_p)
{
    if(mon_p == NULL) return;

    /*Report BOTH heaps LVGL can draw from, not just PSRAM. Reporting PSRAM
     *alone would show megabytes free while internal DRAM — where every
     *fallback allocation lands — was about to run dry, i.e. it would be blind
     *to precisely the failure this backend can produce.*/
    multi_heap_info_t psram;
    multi_heap_info_t internal;
    heap_caps_get_info(&psram, MALLOC_CAP_SPIRAM);
    heap_caps_get_info(&internal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    const size_t free_bytes  = psram.total_free_bytes + internal.total_free_bytes;
    const size_t used_bytes  = psram.total_allocated_bytes + internal.total_allocated_bytes;
    const size_t total_bytes = free_bytes + used_bytes;
    const size_t biggest     = psram.largest_free_block > internal.largest_free_block
                               ? psram.largest_free_block : internal.largest_free_block;

    lv_memzero(mon_p, sizeof(lv_mem_monitor_t));
    mon_p->total_size        = total_bytes;
    mon_p->free_cnt          = psram.free_blocks + internal.free_blocks;
    mon_p->free_size         = free_bytes;
    mon_p->free_biggest_size = biggest;
    mon_p->used_cnt          = psram.allocated_blocks + internal.allocated_blocks;
    mon_p->used_pct          = total_bytes ? (uint8_t)((used_bytes * 100) / total_bytes) : 0;
    mon_p->frag_pct          = free_bytes  ? (uint8_t)(100 - ((biggest * 100) / free_bytes)) : 0;
}

lv_result_t lv_mem_test_core(void)
{
    /*heap_caps has its own integrity checking. Check both heaps in use.*/
    if(!heap_caps_check_integrity(MALLOC_CAP_SPIRAM, false)) return LV_RESULT_INVALID;
    if(!heap_caps_check_integrity(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, false)) return LV_RESULT_INVALID;
    return LV_RESULT_OK;
}

#endif /*LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM*/
