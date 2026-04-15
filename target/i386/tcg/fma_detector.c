#include "qemu/osdep.h"
#include "cpu.h"
#include "fma_detector.h"
#include <stdio.h>
#include <stdlib.h>

GHashTable *gemm_meta_cache = NULL;
extern uint64_t my_demo_guest_base; 

void init_gemm_detector(void) {
    if (gemm_meta_cache != NULL) {
        return; 
    }

    gemm_meta_cache = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    // printf("\n[FMA Profiler] Initializing Offline Profile Loader...\n");

    FILE *fp = fopen("/home/pihaoxuan/detectorAndQEMU/fma_profile.txt", "r");
    if (fp == NULL) {
        printf("[FMA Profiler] Warning: 'fma_profile.txt' not found.\n\n");
        return;
    }

    char line[256];
    target_ulong offset = 0;
    int length = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (sscanf(line, "META: %lx %d", &offset, &length) == 2) {
            
            target_ulong absolute_pc = offset;

            GEMMBlockMeta *meta = g_malloc0(sizeof(GEMMBlockMeta));
            meta->start_pc = absolute_pc;
            meta->length = length;
            meta->is_gemm_loop = true;

            g_hash_table_insert(gemm_meta_cache, (gpointer)(uintptr_t)absolute_pc, meta);
        }
    }
    fclose(fp);
}

GEMMBlockMeta* getMeta(target_ulong pc) {
    if (gemm_meta_cache == NULL) {
        return NULL;
    }
    
    GEMMBlockMeta *meta = g_hash_table_lookup(gemm_meta_cache, (gpointer)(uintptr_t)pc);
    
    // if(meta != NULL){
    	// printf("find address(0x%lx) have fma operate...\n", (unsigned long)pc);
    // }
    return meta;
}
