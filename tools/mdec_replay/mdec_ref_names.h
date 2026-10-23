/* mdec_replay_ab: every public name of runtime/src/mdec.c with a ref_
 * prefix, force-included ahead of the reference decoder's source so it links
 * beside this tree's decoder. A name missing here shows up as a duplicate
 * symbol at link time; add it. */
#ifndef PSXRECOMP_MDEC_REF_NAMES_H
#define PSXRECOMP_MDEC_REF_NAMES_H

#define mdec_init ref_mdec_init
#define mdec_read ref_mdec_read
#define mdec_write ref_mdec_write
#define mdec_dma_write_word ref_mdec_dma_write_word
#define mdec_dma_read_word ref_mdec_dma_read_word
#define mdec_dma_write_words ref_mdec_dma_write_words
#define mdec_dma_read_words ref_mdec_dma_read_words
#define mdec_dma_write_ready ref_mdec_dma_write_ready
#define mdec_dma_read_ready ref_mdec_dma_read_ready
#define mdec_recently_active ref_mdec_recently_active
#define mdec_color_age_cycles ref_mdec_color_age_cycles
#define mdec_mean_qscale_x16 ref_mdec_mean_qscale_x16
#define mdec_get_decode_count ref_mdec_get_decode_count
#define mdec_debug_get_state ref_mdec_debug_get_state
#define mdec_debug_get_event_total ref_mdec_debug_get_event_total
#define mdec_debug_copy_events ref_mdec_debug_copy_events
#define mdec_debug_clear ref_mdec_debug_clear
#define mdec_debug_dma_in_start ref_mdec_debug_dma_in_start
#define mdec_debug_dma_in_end ref_mdec_debug_dma_in_end
#define mdec_debug_dma_out_start ref_mdec_debug_dma_out_start
#define mdec_debug_dma_out_end ref_mdec_debug_dma_out_end
#define mdec_snapshot_bytes ref_mdec_snapshot_bytes
#define mdec_snapshot_write ref_mdec_snapshot_write
#define mdec_snapshot_read ref_mdec_snapshot_read
#define mdec_snapshot_validate ref_mdec_snapshot_validate
#define mdec_capture_start ref_mdec_capture_start
#define mdec_capture_stop ref_mdec_capture_stop
#define mdec_core_set_scale ref_mdec_core_set_scale
#define mdec_core_decode_block ref_mdec_core_decode_block
#define mdec_core_color_macroblock ref_mdec_core_color_macroblock

#endif
