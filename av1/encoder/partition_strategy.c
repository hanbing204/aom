/*
 * Copyright (c) 2019, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#include <float.h>

#include "aom_ports/system_state.h"

#include "av1/common/enums.h"
#include "av1/common/reconinter.h"

#include "av1/encoder/encoder.h"
#include "av1/encoder/partition_model_weights.h"
#include "av1/encoder/partition_strategy.h"
#include "av1/encoder/rdopt.h"

static void simple_motion_search_prune_part_features(
    AV1_COMP *const cpi, MACROBLOCK *x, PC_TREE *pc_tree, int mi_row,
    int mi_col, BLOCK_SIZE bsize, float *features, int features_to_get);

// Performs a simple_motion_search with a single reference frame and extract
// the variance of residues. Here features is assumed to be a length 6 array.
// After this function is called, we will store the following in to features:
// features[0] = log(1 + dc_q**2/256)
// features[1] = log(1 + variance_of_residue)
// for i in [2, 3, 4, 5]:
//  features[i] = log(1 + variance_of_residue_in_block[i]/variance_of_residue)
static void get_res_var_features(AV1_COMP *const cpi, MACROBLOCK *x, int mi_row,
                                 int mi_col, BLOCK_SIZE bsize,
                                 float *features) {
  // TODO(chiyotsai@google.com): The data this model trained on did not also use
  // SIMPLE_TRANSLATION to build the inter_predictor. Retraining and tuning the
  // model with the correct data should give better performance.
  assert(mi_size_wide[bsize] == mi_size_high[bsize]);

  MACROBLOCKD *xd = &x->e_mbd;

  // Perform a single motion search in Y_PLANE to make a prediction
  const int use_subpixel = 0;

  // Start getting the features
  int f_idx = 0;

  // Q_INDEX
  const int dc_q = av1_dc_quant_QTX(x->qindex, 0, xd->bd) >> (xd->bd - 8);
  aom_clear_system_state();
  features[f_idx++] = logf(1.0f + (float)(dc_q * dc_q) / 256.0f);

  // VARIANCE
  unsigned int sse = 0;
  unsigned int var = 0;
  const MV ref_mv_full = { .row = 0, .col = 0 };
  av1_simple_motion_sse_var(cpi, x, mi_row, mi_col, bsize, ref_mv_full,
                            use_subpixel, &sse, &var);
  aom_clear_system_state();
  features[f_idx++] = logf(1.0f + (float)var);

  // Regional
  const uint8_t *src = x->plane[0].src.buf;
  const int src_stride = x->plane[0].src.stride;
  const uint8_t *dst = xd->plane[0].dst.buf;
  const int dst_stride = xd->plane[0].dst.stride;
  const int bw = block_size_wide[bsize];
  const int bh = block_size_high[bsize];
  const BLOCK_SIZE subsize = get_partition_subsize(bsize, PARTITION_SPLIT);
  int r_idx = 0;
  for (r_idx = 0; r_idx < 4; r_idx++) {
    const int x_idx = (r_idx & 1) * bw / 2;
    const int y_idx = (r_idx >> 1) * bh / 2;
    const int src_offset = y_idx * src_stride + x_idx;
    const int dst_offset = y_idx * dst_stride + x_idx;
    const unsigned int sub_var = cpi->fn_ptr[subsize].vf(
        src + src_offset, src_stride, dst + dst_offset, dst_stride, &sse);
    aom_clear_system_state();
    const float var_ratio = (1.0f + (float)sub_var) / (4.0f + (float)var);
    features[f_idx++] = var_ratio;
  }
}

static void simple_motion_search_based_split_fast(
    AV1_COMP *const cpi, MACROBLOCK *x, int mi_row, int mi_col,
    BLOCK_SIZE bsize, int *partition_none_allowed, int *partition_horz_allowed,
    int *partition_vert_allowed, int *do_rectangular_split,
    int *do_square_split) {
  const NN_CONFIG *nn_config = NULL;
  float split_only_thresh = 1.0f;
  if (bsize == BLOCK_128X128) {
    nn_config = &av1_simple_motion_search_based_split_nn_config_128;
    split_only_thresh = av1_simple_motion_search_based_split_thresh_128;
  } else if (bsize == BLOCK_64X64) {
    nn_config = &av1_simple_motion_search_based_split_nn_config_64;
    split_only_thresh = av1_simple_motion_search_based_split_thresh_64;
  } else if (bsize == BLOCK_32X32) {
    nn_config = &av1_simple_motion_search_based_split_nn_config_32;
    split_only_thresh = av1_simple_motion_search_based_split_thresh_32;
  } else if (bsize == BLOCK_16X16) {
    nn_config = &av1_simple_motion_search_based_split_nn_config_16;
    split_only_thresh = av1_simple_motion_search_based_split_thresh_16;
  } else if (bsize == BLOCK_8X8) {
    return;
  } else {
    assert(0 && "Unexpected block size in simple_motion_based_split");
    return;
  }

  float features[FEATURE_SIZE_SMS_SPLIT_FAST] = { 0.0f };
  float score = 0.0f;
  get_res_var_features(cpi, x, mi_row, mi_col, bsize, features);
  av1_nn_predict(features, nn_config, &score);

  if (score > split_only_thresh) {
    *partition_none_allowed = 0;
    *partition_horz_allowed = 0;
    *partition_vert_allowed = 0;
    *do_rectangular_split = 0;
  }
  if (cpi->sf.simple_motion_search_split_only >= 2) {
    if (score < -split_only_thresh) *do_square_split = 0;
    // For larger scores (>split_only_thresh), none and rectangular partitions
    // are skipped. As score reduces, possibility of split decreases. Hence
    // for near larger scores (.875 * split_only_thresh to split_only_thresh)
    // none partition is disabled, but rectangular partitions are evaluated
    // additionally.
    if (score > (split_only_thresh * 0.875)) *partition_none_allowed = 0;
  }
}

void av1_simple_motion_search_based_split(
    AV1_COMP *const cpi, MACROBLOCK *x, PC_TREE *pc_tree, int mi_row,
    int mi_col, BLOCK_SIZE bsize, int *partition_none_allowed,
    int *partition_horz_allowed, int *partition_vert_allowed,
    int *do_rectangular_split, int *do_square_split) {
  if (cpi->sf.simple_motion_search_split_speed >= 2) {
    simple_motion_search_based_split_fast(
        cpi, x, mi_row, mi_col, bsize, partition_none_allowed,
        partition_horz_allowed, partition_vert_allowed, do_rectangular_split,
        do_square_split);

    return;
  }

  aom_clear_system_state();
  const NN_CONFIG *nn_config = NULL;
  const float *ml_mean = NULL, *ml_std = NULL;
  float split_only_thresh = 10.0f, no_split_thresh = -10.0f;
  if (bsize == BLOCK_128X128) {
    ml_mean = av1_simple_motion_search_split_mean_128;
    ml_std = av1_simple_motion_search_split_std_128;
    nn_config = &av1_simple_motion_search_split_nn_config_128;
    split_only_thresh = av1_simple_motion_search_split_thresh_128;
    no_split_thresh = av1_simple_motion_search_no_split_thresh_128;
  } else if (bsize == BLOCK_64X64) {
    ml_mean = av1_simple_motion_search_split_mean_64;
    ml_std = av1_simple_motion_search_split_std_64;
    nn_config = &av1_simple_motion_search_split_nn_config_64;
    split_only_thresh = av1_simple_motion_search_split_thresh_64;
    no_split_thresh = av1_simple_motion_search_no_split_thresh_64;
  } else if (bsize == BLOCK_32X32) {
    ml_mean = av1_simple_motion_search_split_mean_32;
    ml_std = av1_simple_motion_search_split_std_32;
    nn_config = &av1_simple_motion_search_split_nn_config_32;
    split_only_thresh = av1_simple_motion_search_split_thresh_32;
    no_split_thresh = av1_simple_motion_search_no_split_thresh_32;
  } else if (bsize == BLOCK_16X16) {
    ml_mean = av1_simple_motion_search_split_mean_16;
    ml_std = av1_simple_motion_search_split_std_16;
    nn_config = &av1_simple_motion_search_split_nn_config_16;
    split_only_thresh = av1_simple_motion_search_split_thresh_16;
    no_split_thresh = av1_simple_motion_search_no_split_thresh_16;
  } else if (bsize == BLOCK_8X8) {
    ml_mean = av1_simple_motion_search_split_mean_8;
    ml_std = av1_simple_motion_search_split_std_8;
    nn_config = &av1_simple_motion_search_split_nn_config_8;
    split_only_thresh = av1_simple_motion_search_split_thresh_8;
    no_split_thresh = av1_simple_motion_search_no_split_thresh_8;
  } else {
    assert(0 && "Unexpected block size in simple_motion_based_split");
    return;
  }

  float features[FEATURE_SIZE_SMS_SPLIT] = { 0.0f };
  simple_motion_search_prune_part_features(cpi, x, pc_tree, mi_row, mi_col,
                                           bsize, features,
                                           FEATURE_SMS_SPLIT_MODEL_FLAG);
  for (int idx = 0; idx < FEATURE_SIZE_SMS_SPLIT; idx++) {
    features[idx] = (features[idx] - ml_mean[idx]) / ml_std[idx];
  }

  float score = 0.0f;

  av1_nn_predict(features, nn_config, &score);
  aom_clear_system_state();

  if (score > split_only_thresh) {
    *partition_none_allowed = 0;
    *partition_horz_allowed = 0;
    *partition_vert_allowed = 0;
    *do_rectangular_split = 0;
  }

  if (cpi->sf.simple_motion_search_split_only >= 2 && score < no_split_thresh) {
    *do_square_split = 0;
  }
}

// Given a list of ref frames in refs, performs simple_motion_search on each of
// the refs and returns the ref with the smallest sse. Returns -1 if none of the
// ref in the list is available. Also stores the best sse and var in best_sse,
// best_var, respectively. If save_mv_code is -1, don't update mv_ref_fulls in
// pc_tree. If save_mv_code is between 0 and 3, update mv_ref_fulls under
// pc_tree->split[i]. If save_mv_code is 4, update mv_ref_fulls under pc_tree.
static int simple_motion_search_get_best_ref(
    AV1_COMP *const cpi, MACROBLOCK *x, PC_TREE *pc_tree, int mi_row,
    int mi_col, BLOCK_SIZE bsize, const int *const refs, int num_refs,
    int use_subpixel, int save_mv_code, unsigned int *best_sse,
    unsigned int *best_var) {
  // TODO(chiyotsai@google.com): The calculation of variance currently uses
  // bsize, so we might take area outside of the image into account. We need to
  // modify the SIMD functions to fix this later.
  const AV1_COMMON *const cm = &cpi->common;
  int best_ref = -1;

  if (mi_col >= cm->mi_cols || mi_row >= cm->mi_rows) {
    // If the whole block is outside of the image, set the var and sse to 0.
    *best_var = 0;
    *best_sse = 0;

    return best_ref;
  }

  // Otherwise do loop through the reference frames and find the one with the
  // minimum SSE
  const MACROBLOCKD *xd = &x->e_mbd;
  const MV *mv_ref_fulls = pc_tree->mv_ref_fulls;

  const int num_planes = 1;

  *best_sse = INT_MAX;

  for (int ref_idx = 0; ref_idx < num_refs; ref_idx++) {
    const int ref = refs[ref_idx];

    if (cpi->ref_frame_flags & av1_ref_frame_flag_list[ref]) {
      unsigned int curr_sse = 0, curr_var = 0;
      av1_simple_motion_search(cpi, x, mi_row, mi_col, bsize, ref,
                               mv_ref_fulls[ref], num_planes, use_subpixel);
      curr_var = cpi->fn_ptr[bsize].vf(
          x->plane[0].src.buf, x->plane[0].src.stride, xd->plane[0].dst.buf,
          xd->plane[0].dst.stride, &curr_sse);
      if (curr_sse < *best_sse) {
        *best_sse = curr_sse;
        *best_var = curr_var;
        best_ref = ref;
      }

      const int new_mv_row = x->best_mv.as_mv.row / 8;
      const int new_mv_col = x->best_mv.as_mv.col / 8;
      if (save_mv_code == 4) {
        pc_tree->mv_ref_fulls[ref].row = new_mv_row;
        pc_tree->mv_ref_fulls[ref].col = new_mv_col;
      } else if (save_mv_code >= 0 && save_mv_code < 4) {
        // Propagate the new motion vectors to a lower level
        pc_tree->split[save_mv_code]->mv_ref_fulls[ref].row = new_mv_row;
        pc_tree->split[save_mv_code]->mv_ref_fulls[ref].col = new_mv_col;
      } else {
        assert(save_mv_code == -1 &&
               "Unknown code in simple_motion_search_get_best_ref.");
      }
    }
  }

  return best_ref;
}

// Performs fullpixel simple_motion_search with LAST_FRAME and ALTREF_FRAME on
// each subblock and extract the variance and sse of residues. Then store the
// var and sse from each partition subblock to features. The DC qindex is also
// stored in features.
// Here features is assumed to be a length 19 array.
// After this function is called, we will store the following to features:
// features[0:17] = var and sse from subblocks
// features[18] = DC q_index
static void simple_motion_search_prune_part_features(
    AV1_COMP *const cpi, MACROBLOCK *x, PC_TREE *pc_tree, int mi_row,
    int mi_col, BLOCK_SIZE bsize, float *features, int features_to_get) {
  // TODO(chiyotsai@google.com): Cache the result of the motion search from the
  // larger bsize.
  const int w_mi = mi_size_wide[bsize];
  const int h_mi = mi_size_high[bsize];
  assert(mi_size_wide[bsize] == mi_size_high[bsize]);
  assert(cpi->ref_frame_flags & av1_ref_frame_flag_list[LAST_FRAME] ||
         cpi->ref_frame_flags & av1_ref_frame_flag_list[ALTREF_FRAME]);

  // Setting up motion search
  const int ref_list[] = { cpi->rc.is_src_frame_alt_ref ? ALTREF_FRAME
                                                        : LAST_FRAME };
  const int num_refs = 1;
  const int use_subpixel = 1;

  // Doing whole block first to update the mv
  if (!pc_tree->sms_none_valid && features_to_get & FEATURE_SMS_NONE_FLAG) {
    simple_motion_search_get_best_ref(cpi, x, pc_tree, mi_row, mi_col, bsize,
                                      ref_list, num_refs, use_subpixel, 4,
                                      &pc_tree->sms_none_feat[0],
                                      &pc_tree->sms_none_feat[1]);
    pc_tree->sms_none_valid = 1;
  }

  // Split subblocks
  if (!pc_tree->sms_split_valid && features_to_get & FEATURE_SMS_SPLIT_FLAG) {
    const BLOCK_SIZE subsize = get_partition_subsize(bsize, PARTITION_SPLIT);
    for (int r_idx = 0; r_idx < 4; r_idx++) {
      const int sub_mi_col = mi_col + (r_idx & 1) * w_mi / 2;
      const int sub_mi_row = mi_row + (r_idx >> 1) * h_mi / 2;

      simple_motion_search_get_best_ref(
          cpi, x, pc_tree, sub_mi_row, sub_mi_col, subsize, ref_list, num_refs,
          use_subpixel, r_idx, &pc_tree->sms_split_feat[2 * r_idx],
          &pc_tree->sms_split_feat[2 * r_idx + 1]);
    }

    pc_tree->sms_split_valid = 1;
  }

  // Rectangular subblocks
  if (!pc_tree->sms_rect_valid && features_to_get & FEATURE_SMS_RECT_FLAG) {
    // Horz subblock
    BLOCK_SIZE subsize = get_partition_subsize(bsize, PARTITION_HORZ);
    for (int r_idx = 0; r_idx < 2; r_idx++) {
      const int sub_mi_col = mi_col + 0;
      const int sub_mi_row = mi_row + r_idx * h_mi / 2;

      simple_motion_search_get_best_ref(
          cpi, x, pc_tree, sub_mi_row, sub_mi_col, subsize, ref_list, num_refs,
          use_subpixel, -1, &pc_tree->sms_rect_feat[2 * r_idx],
          &pc_tree->sms_rect_feat[2 * r_idx + 1]);
    }

    // Vert subblock
    subsize = get_partition_subsize(bsize, PARTITION_VERT);
    for (int r_idx = 0; r_idx < 2; r_idx++) {
      const int sub_mi_col = mi_col + r_idx * w_mi / 2;
      const int sub_mi_row = mi_row + 0;

      simple_motion_search_get_best_ref(
          cpi, x, pc_tree, sub_mi_row, sub_mi_col, subsize, ref_list, num_refs,
          use_subpixel, -1, &pc_tree->sms_rect_feat[4 + 2 * r_idx],
          &pc_tree->sms_rect_feat[4 + 2 * r_idx + 1]);
    }
    pc_tree->sms_rect_valid = 1;
  }

  if (!features) return;

  aom_clear_system_state();
  int f_idx = 0;
  if (features_to_get & FEATURE_SMS_NONE_FLAG) {
    for (int sub_idx = 0; sub_idx < 2; sub_idx++) {
      features[f_idx++] = logf(1.0f + pc_tree->sms_none_feat[sub_idx]);
    }
  }

  if (features_to_get & FEATURE_SMS_SPLIT_FLAG) {
    for (int sub_idx = 0; sub_idx < 8; sub_idx++) {
      features[f_idx++] = logf(1.0f + pc_tree->sms_split_feat[sub_idx]);
    }
  }

  if (features_to_get & FEATURE_SMS_RECT_FLAG) {
    for (int sub_idx = 0; sub_idx < 8; sub_idx++) {
      features[f_idx++] = logf(1.0f + pc_tree->sms_rect_feat[sub_idx]);
    }
  }

  const MACROBLOCKD *xd = &x->e_mbd;
  set_offsets_for_motion_search(cpi, x, mi_row, mi_col, bsize);

  // Q_INDEX
  const int dc_q = av1_dc_quant_QTX(x->qindex, 0, xd->bd) >> (xd->bd - 8);
  features[f_idx++] = logf(1.0f + (float)(dc_q * dc_q) / 256.0f);

  // Neighbor stuff
  const int has_above = !!xd->above_mbmi;
  const int has_left = !!xd->left_mbmi;
  const BLOCK_SIZE above_bsize = has_above ? xd->above_mbmi->sb_type : bsize;
  const BLOCK_SIZE left_bsize = has_left ? xd->left_mbmi->sb_type : bsize;
  features[f_idx++] = (float)has_above;
  features[f_idx++] = (float)mi_size_wide_log2[above_bsize];
  features[f_idx++] = (float)mi_size_high_log2[above_bsize];
  features[f_idx++] = (float)has_left;
  features[f_idx++] = (float)mi_size_wide_log2[left_bsize];
  features[f_idx++] = (float)mi_size_high_log2[left_bsize];
}

void av1_simple_motion_search_prune_part(
    AV1_COMP *const cpi, MACROBLOCK *x, PC_TREE *pc_tree, int mi_row,
    int mi_col, BLOCK_SIZE bsize, int *partition_none_allowed,
    int *partition_horz_allowed, int *partition_vert_allowed,
    int *do_square_split, int *do_rectangular_split, int *prune_horz,
    int *prune_vert) {
  const AV1_COMMON *const cm = &cpi->common;
  // Get model parameters
  const NN_CONFIG *nn_config = NULL;
  const float *prune_thresh = NULL, *only_thresh = NULL;
  const float *ml_mean = NULL, *ml_std = NULL;
  float features[FEATURE_SIZE_SMS_PRUNE_PART] = { 0.0f };

  if (bsize == BLOCK_128X128) {
    nn_config = &av1_simple_motion_search_prune_part_nn_config_128;
    ml_mean = av1_simple_motion_search_prune_part_mean_128;
    ml_std = av1_simple_motion_search_prune_part_std_128;
    prune_thresh = av1_simple_motion_search_prune_part_prune_thresh_128;
    only_thresh = av1_simple_motion_search_prune_part_only_thresh_128;
  } else if (bsize == BLOCK_64X64) {
    nn_config = &av1_simple_motion_search_prune_part_nn_config_64;
    ml_mean = av1_simple_motion_search_prune_part_mean_64;
    ml_std = av1_simple_motion_search_prune_part_std_64;
    prune_thresh = av1_simple_motion_search_prune_part_prune_thresh_64;
    only_thresh = av1_simple_motion_search_prune_part_only_thresh_64;
  } else if (bsize == BLOCK_32X32) {
    nn_config = &av1_simple_motion_search_prune_part_nn_config_32;
    ml_mean = av1_simple_motion_search_prune_part_mean_32;
    ml_std = av1_simple_motion_search_prune_part_std_32;
    prune_thresh = av1_simple_motion_search_prune_part_prune_thresh_32;
    only_thresh = av1_simple_motion_search_prune_part_only_thresh_32;
  } else if (bsize == BLOCK_16X16) {
    nn_config = &av1_simple_motion_search_prune_part_nn_config_16;
    ml_mean = av1_simple_motion_search_prune_part_mean_16;
    ml_std = av1_simple_motion_search_prune_part_std_16;
    prune_thresh = av1_simple_motion_search_prune_part_prune_thresh_16;
    only_thresh = av1_simple_motion_search_prune_part_only_thresh_16;
  } else if (bsize == BLOCK_8X8) {
    nn_config = &av1_simple_motion_search_prune_part_nn_config_8;
    ml_mean = av1_simple_motion_search_prune_part_mean_8;
    ml_std = av1_simple_motion_search_prune_part_std_8;
    prune_thresh = av1_simple_motion_search_prune_part_prune_thresh_8;
    only_thresh = av1_simple_motion_search_prune_part_only_thresh_8;
  } else {
    assert(0 && "Unexpected block size in simple_motion_prune_part");
  }

  // If there is no valid threshold, return immediately.
  if (!nn_config || (prune_thresh[PARTITION_HORZ] == 0.0f &&
                     prune_thresh[PARTITION_VERT] == 0.0f)) {
    return;
  }
  if (bsize < BLOCK_8X8) {
    return;
  }

  // Get features
  simple_motion_search_prune_part_features(cpi, x, pc_tree, mi_row, mi_col,
                                           bsize, features,
                                           FEATURE_SMS_PRUNE_PART_FLAG);
  for (int f_idx = 0; f_idx < FEATURE_SIZE_SMS_PRUNE_PART; f_idx++) {
    features[f_idx] = (features[f_idx] - ml_mean[f_idx]) / ml_std[f_idx];
  }

  // Get probabilities
  float scores[EXT_PARTITION_TYPES] = { 0.0f },
        probs[EXT_PARTITION_TYPES] = { 0.0f };
  const int num_classes = (bsize == BLOCK_128X128 || bsize == BLOCK_8X8)
                              ? PARTITION_TYPES
                              : EXT_PARTITION_TYPES;

  av1_nn_predict(features, nn_config, scores);
  aom_clear_system_state();

  av1_nn_softmax(scores, probs, num_classes);

  // Determine if we should prune rectangular partitions.
  if (cpi->sf.simple_motion_search_prune_rect && !frame_is_intra_only(cm) &&
      (*partition_horz_allowed || *partition_vert_allowed) &&
      bsize >= BLOCK_8X8 && !av1_superres_scaled(cm)) {
    *prune_horz = probs[PARTITION_HORZ] <= prune_thresh[PARTITION_HORZ];
    *prune_vert = probs[PARTITION_VERT] <= prune_thresh[PARTITION_VERT];
  }

  // Silence compiler warnings
  (void)only_thresh;
  (void)partition_none_allowed;
  (void)do_square_split;
  (void)do_rectangular_split;
}

// Early terminates PARTITION_NONE using simple_motion_search features and the
// rate, distortion, and rdcost of PARTITION_NONE. This is only called when:
//  - The frame is a show frame
//  - The frame is not intra only
//  - The current bsize is > BLOCK_8X8
//  - blk_row + blk_height/2 < total_rows and blk_col + blk_width/2 < total_cols
void av1_simple_motion_search_early_term_none(AV1_COMP *const cpi,
                                              MACROBLOCK *x, PC_TREE *pc_tree,
                                              int mi_row, int mi_col,
                                              BLOCK_SIZE bsize,
                                              const RD_STATS *none_rdc,
                                              int *early_terminate) {
  // TODO(chiyotsai@google.com): There are other features we can extract from
  // PARTITION_NONE. Play with this later.
  float features[FEATURE_SIZE_SMS_TERM_NONE] = { 0.0f };
  simple_motion_search_prune_part_features(cpi, x, pc_tree, mi_row, mi_col,
                                           bsize, features,
                                           FEATURE_SMS_PRUNE_PART_FLAG);
  int f_idx = FEATURE_SIZE_SMS_PRUNE_PART;

  features[f_idx++] = logf(1.0f + (float)none_rdc->rate);
  features[f_idx++] = logf(1.0f + (float)none_rdc->dist);
  features[f_idx++] = logf(1.0f + (float)none_rdc->rdcost);

  assert(f_idx == FEATURE_SIZE_SMS_TERM_NONE);

  const float *ml_mean = NULL;
  const float *ml_std = NULL;
  const float *ml_model = NULL;

  if (bsize == BLOCK_128X128) {
    ml_mean = av1_simple_motion_search_term_none_mean_128;
    ml_std = av1_simple_motion_search_term_none_std_128;
    ml_model = av1_simple_motion_search_term_none_model_128;
  } else if (bsize == BLOCK_64X64) {
    ml_mean = av1_simple_motion_search_term_none_mean_64;
    ml_std = av1_simple_motion_search_term_none_std_64;
    ml_model = av1_simple_motion_search_term_none_model_64;
  } else if (bsize == BLOCK_32X32) {
    ml_mean = av1_simple_motion_search_term_none_mean_32;
    ml_std = av1_simple_motion_search_term_none_std_32;
    ml_model = av1_simple_motion_search_term_none_model_32;
  } else if (bsize == BLOCK_16X16) {
    ml_mean = av1_simple_motion_search_term_none_mean_16;
    ml_std = av1_simple_motion_search_term_none_std_16;
    ml_model = av1_simple_motion_search_term_none_model_16;
  } else {
    assert(0 && "Unexpected block size in simple_motion_term_none");
  }

  if (ml_model) {
    float score = 0.0f;
    for (f_idx = 0; f_idx < FEATURE_SIZE_SMS_TERM_NONE; f_idx++) {
      score +=
          ml_model[f_idx] * (features[f_idx] - ml_mean[f_idx]) / ml_std[f_idx];
    }
    score += ml_model[FEATURE_SIZE_SMS_TERM_NONE];

    if (score >= 0.0f) {
      *early_terminate = 1;
    }
  }
}

static void firstpass_simple_motion_search_features(
    AV1_COMP *const cpi, MACROBLOCK *x, PC_TREE *pc_tree, int mi_row,
    int mi_col, BLOCK_SIZE bsize, float *features) {
  assert(mi_size_wide[bsize] == mi_size_high[bsize]);
  assert(cpi->ref_frame_flags & av1_ref_frame_flag_list[LAST_FRAME] ||
         cpi->ref_frame_flags & av1_ref_frame_flag_list[ALTREF_FRAME]);

  // Setting up motion search
  const int ref_list[] = { LAST_FRAME, ALTREF_FRAME };
  const int num_refs = 2;
  const int use_subpixel = 0;

  unsigned int int_features[10] = { 0 };

  int f_idx = 0;
  // Doing whole block first to update the mv
  simple_motion_search_get_best_ref(
      cpi, x, pc_tree, mi_row, mi_col, bsize, ref_list, num_refs, use_subpixel,
      4, &int_features[f_idx], &int_features[f_idx + 1]);
  f_idx += 2;

  // Split subblocks
  const BLOCK_SIZE subsize = get_partition_subsize(bsize, PARTITION_SPLIT);
  const int w_mi = mi_size_wide[bsize];
  const int h_mi = mi_size_high[bsize];
  for (int r_idx = 0; r_idx < 4; r_idx++) {
    const int sub_mi_col = mi_col + (r_idx & 1) * w_mi / 2;
    const int sub_mi_row = mi_row + (r_idx >> 1) * h_mi / 2;

    simple_motion_search_get_best_ref(
        cpi, x, pc_tree, sub_mi_row, sub_mi_col, subsize, ref_list, num_refs,
        use_subpixel, r_idx, &int_features[f_idx], &int_features[f_idx + 1]);
    f_idx += 2;
  }

  aom_clear_system_state();
  for (int idx = 0; idx < f_idx; idx++) {
    features[idx] = logf(1.0f + (float)int_features[idx]);
  }

  const MACROBLOCKD *xd = &x->e_mbd;
  set_offsets_for_motion_search(cpi, x, mi_row, mi_col, bsize);

  // Q_INDEX
  const int dc_q = av1_dc_quant_QTX(x->qindex, 0, xd->bd) >> (xd->bd - 8);
  features[f_idx++] = logf(1.0f + (float)(dc_q * dc_q) / 256.0f);

  // Neighbor stuff
  const int has_above = !!xd->above_mbmi;
  const int has_left = !!xd->left_mbmi;
  const BLOCK_SIZE above_bsize = has_above ? xd->above_mbmi->sb_type : bsize;
  const BLOCK_SIZE left_bsize = has_left ? xd->left_mbmi->sb_type : bsize;
  features[f_idx++] = (float)has_above;
  features[f_idx++] = (float)mi_size_wide_log2[above_bsize];
  features[f_idx++] = (float)mi_size_high_log2[above_bsize];
  features[f_idx++] = (float)has_left;
  features[f_idx++] = (float)mi_size_wide_log2[left_bsize];
  features[f_idx++] = (float)mi_size_high_log2[left_bsize];
}

void av1_firstpass_simple_motion_search_early_term(AV1_COMP *const cpi,
                                                   MACROBLOCK *x,
                                                   PC_TREE *pc_tree, int mi_row,
                                                   int mi_col, BLOCK_SIZE bsize,
                                                   const RD_STATS *none_rdc,
                                                   int *do_square_split) {
  const NN_CONFIG *nn_config = NULL;
  float thresh = 0.0f;
  const float *ml_mean = NULL, *ml_std = NULL;
  if (bsize == BLOCK_32X32) {
    nn_config = &av1_fp_simple_motion_search_term_none_nn_config_32;
    ml_mean = av1_fp_simple_motion_search_term_none_mean_32;
    ml_std = av1_fp_simple_motion_search_term_none_std_32;
    thresh = av1_fp_simple_motion_search_term_none_thresh_32;
  } else if (bsize == BLOCK_16X16) {
    nn_config = &av1_fp_simple_motion_search_term_none_nn_config_16;
    ml_mean = av1_fp_simple_motion_search_term_none_mean_16;
    ml_std = av1_fp_simple_motion_search_term_none_std_16;
    thresh = av1_fp_simple_motion_search_term_none_thresh_16;
  } else if (bsize == BLOCK_8X8) {
    nn_config = &av1_fp_simple_motion_search_term_none_nn_config_8;
    ml_mean = av1_fp_simple_motion_search_term_none_mean_8;
    ml_std = av1_fp_simple_motion_search_term_none_std_8;
    thresh = av1_fp_simple_motion_search_term_none_thresh_8;
  } else {
    assert(0 &&
           "Unexpected bsize in firstpass_simple_motion_search_early_term");
    return;
  }

  float ml_features[FEATURE_SIZE_FP_SMS_TERM_NONE] = { 0.0f };

  firstpass_simple_motion_search_features(cpi, x, pc_tree, mi_row, mi_col,
                                          bsize, ml_features);
  int f_idx = 17;

  ml_features[f_idx++] = logf(1.0f + (float)none_rdc->rate);
  ml_features[f_idx++] = logf(1.0f + (float)none_rdc->dist);
  ml_features[f_idx++] = logf(1.0f + (float)none_rdc->rdcost);

  for (f_idx = 0; f_idx < 20; f_idx++) {
    ml_features[f_idx] = (ml_features[f_idx] - ml_mean[f_idx]) / ml_std[f_idx];
  }

  // Get probabilities
  float score = 0.0f;

  av1_nn_predict(ml_features, nn_config, &score);
  aom_clear_system_state();

  // Determine if we should prune square partitions.
  if (score < thresh) {
    *do_square_split = 0;
  }
}

void av1_get_max_min_partition_features(AV1_COMP *const cpi, MACROBLOCK *x,
                                        int mi_row, int mi_col,
                                        float *features) {
  AV1_COMMON *const cm = &cpi->common;
  MACROBLOCKD *xd = &x->e_mbd;
  const BLOCK_SIZE sb_size = cm->seq_params.sb_size;

  assert(sb_size == BLOCK_128X128);

  int f_idx = 0;

  const int dc_q = av1_dc_quant_QTX(x->qindex, 0, xd->bd) >> (xd->bd - 8);
  aom_clear_system_state();
  const float log_q_sq = logf(1.0f + (float)(dc_q * dc_q) / 256.0f);

  // Perform full-pixel single motion search in Y plane of 16x16 mbs in the sb
  float sum_mv_row_sq = 0;
  float sum_mv_row = 0;
  float min_abs_mv_row = FLT_MAX;
  float max_abs_mv_row = 0;

  float sum_mv_col_sq = 0;
  float sum_mv_col = 0;
  float min_abs_mv_col = FLT_MAX;
  float max_abs_mv_col = 0;

  float sum_log_sse_sq = 0;
  float sum_log_sse = 0;
  float min_log_sse = FLT_MAX;
  float max_log_sse = 0;

  const BLOCK_SIZE mb_size = BLOCK_16X16;
  const int mb_rows = block_size_high[sb_size] / block_size_high[mb_size];
  const int mb_cols = block_size_wide[sb_size] / block_size_wide[mb_size];
  const int mb_in_mi_size_high_log2 = mi_size_high_log2[mb_size];
  const int mb_in_mi_size_wide_log2 = mi_size_wide_log2[mb_size];

  for (int mb_row = 0; mb_row < mb_rows; mb_row++)
    for (int mb_col = 0; mb_col < mb_cols; mb_col++) {
      const int this_mi_row = mi_row + (mb_row << mb_in_mi_size_high_log2);
      const int this_mi_col = mi_col + (mb_col << mb_in_mi_size_wide_log2);
      unsigned int sse = 0;
      unsigned int var = 0;
      const MV ref_mv_full = { .row = 0, .col = 0 };

      av1_simple_motion_sse_var(cpi, x, this_mi_row, this_mi_col, mb_size,
                                ref_mv_full, 0, &sse, &var);

      aom_clear_system_state();
      const float mv_row = (float)(x->best_mv.as_mv.row / 8);
      const float mv_col = (float)(x->best_mv.as_mv.col / 8);
      const float log_sse = logf(1.0f + (float)sse);
      const float abs_mv_row = fabsf(mv_row);
      const float abs_mv_col = fabsf(mv_col);

      sum_mv_row_sq += mv_row * mv_row;
      sum_mv_row += mv_row;
      sum_mv_col_sq += mv_col * mv_col;
      sum_mv_col += mv_col;

      if (abs_mv_row < min_abs_mv_row) min_abs_mv_row = abs_mv_row;
      if (abs_mv_row > max_abs_mv_row) max_abs_mv_row = abs_mv_row;
      if (abs_mv_col < min_abs_mv_col) min_abs_mv_col = abs_mv_col;
      if (abs_mv_col > max_abs_mv_col) max_abs_mv_col = abs_mv_col;

      sum_log_sse_sq += log_sse * log_sse;
      sum_log_sse += log_sse;
      if (log_sse < min_log_sse) min_log_sse = log_sse;
      if (log_sse > max_log_sse) max_log_sse = log_sse;
    }
  aom_clear_system_state();
  const float avg_mv_row = sum_mv_row / 64.0f;
  const float var_mv_row = sum_mv_row_sq / 64.0f - avg_mv_row * avg_mv_row;

  const float avg_mv_col = sum_mv_col / 64.0f;
  const float var_mv_col = sum_mv_col_sq / 64.0f - avg_mv_col * avg_mv_col;

  const float avg_log_sse = sum_log_sse / 64.0f;
  const float var_log_sse = sum_log_sse_sq / 64.0f - avg_log_sse * avg_log_sse;

  features[f_idx++] = avg_log_sse;
  features[f_idx++] = avg_mv_col;
  features[f_idx++] = avg_mv_row;
  features[f_idx++] = log_q_sq;
  features[f_idx++] = max_abs_mv_col;
  features[f_idx++] = max_abs_mv_row;
  features[f_idx++] = max_log_sse;
  features[f_idx++] = min_abs_mv_col;
  features[f_idx++] = min_abs_mv_row;
  features[f_idx++] = min_log_sse;
  features[f_idx++] = var_log_sse;
  features[f_idx++] = var_mv_col;
  features[f_idx++] = var_mv_row;

  assert(f_idx == FEATURE_SIZE_MAX_MIN_PART_PRED);
}

BLOCK_SIZE av1_predict_max_partition(AV1_COMP *const cpi, MACROBLOCK *const x,
                                     const float *features) {
  float scores[MAX_NUM_CLASSES_MAX_MIN_PART_PRED] = { 0.0f },
        probs[MAX_NUM_CLASSES_MAX_MIN_PART_PRED] = { 0.0f };
  const NN_CONFIG *nn_config = &av1_max_part_pred_nn_config;

  assert(cpi->sf.auto_max_partition_based_on_simple_motion != NOT_IN_USE);

  aom_clear_system_state();
  av1_nn_predict(features, nn_config, scores);
  av1_nn_softmax(scores, probs, MAX_NUM_CLASSES_MAX_MIN_PART_PRED);

  int result = MAX_NUM_CLASSES_MAX_MIN_PART_PRED - 1;
  if (cpi->sf.auto_max_partition_based_on_simple_motion == DIRECT_PRED) {
    result = 0;
    float max_prob = probs[0];
    for (int i = 1; i < MAX_NUM_CLASSES_MAX_MIN_PART_PRED; ++i) {
      if (probs[i] > max_prob) {
        max_prob = probs[i];
        result = i;
      }
    }
  } else if (cpi->sf.auto_max_partition_based_on_simple_motion ==
             RELAXED_PRED) {
    for (result = MAX_NUM_CLASSES_MAX_MIN_PART_PRED - 1; result >= 0;
         --result) {
      if (result < MAX_NUM_CLASSES_MAX_MIN_PART_PRED - 1) {
        probs[result] += probs[result + 1];
      }
      if (probs[result] > 0.2) break;
    }
  } else if (cpi->sf.auto_max_partition_based_on_simple_motion == ADAPT_PRED) {
    const BLOCK_SIZE sb_size = cpi->common.seq_params.sb_size;
    MACROBLOCKD *const xd = &x->e_mbd;
    // TODO(debargha): x->source_variance is unavailable at this point,
    // so compute. The redundant recomputation later can be removed.
    const unsigned int source_variance =
        is_cur_buf_hbd(xd)
            ? av1_high_get_sby_perpixel_variance(cpi, &x->plane[0].src, sb_size,
                                                 xd->bd)
            : av1_get_sby_perpixel_variance(cpi, &x->plane[0].src, sb_size);
    if (source_variance > 16) {
      const double thresh = source_variance < 128 ? 0.05 : 0.1;
      for (result = MAX_NUM_CLASSES_MAX_MIN_PART_PRED - 1; result >= 0;
           --result) {
        if (result < MAX_NUM_CLASSES_MAX_MIN_PART_PRED - 1) {
          probs[result] += probs[result + 1];
        }
        if (probs[result] > thresh) break;
      }
    }
  }

  return (BLOCK_SIZE)((result + 2) * 3);
}

// Get the minimum partition block width and height(in log scale) under a
// PC_TREE.
static void get_min_bsize(const PC_TREE *pc_tree, int *min_bw, int *min_bh) {
  if (!pc_tree) return;

  const BLOCK_SIZE bsize = pc_tree->block_size;
  if (bsize == BLOCK_4X4) {
    *min_bw = 0;
    *min_bh = 0;
    return;
  }

  PARTITION_TYPE part_type = pc_tree->partitioning;
  if (part_type == PARTITION_INVALID) return;

  if (part_type == PARTITION_SPLIT) {
    for (int i = 0; i < 4; ++i) {
      get_min_bsize(pc_tree->split[i], min_bw, min_bh);
    }
  } else {
    if (part_type == PARTITION_HORZ_A || part_type == PARTITION_HORZ_B ||
        part_type == PARTITION_VERT_A || part_type == PARTITION_VERT_B)
      part_type = PARTITION_SPLIT;
    const BLOCK_SIZE subsize = get_partition_subsize(bsize, part_type);
    if (subsize != BLOCK_INVALID) {
      *min_bw = AOMMIN(*min_bw, mi_size_wide_log2[subsize]);
      *min_bh = AOMMIN(*min_bh, mi_size_high_log2[subsize]);
    }
  }
}

static INLINE void add_rd_feature(int64_t rd, int64_t best_rd, float *features,
                                  int *feature_idx) {
  const int rd_valid = rd > 0 && rd < INT64_MAX;
  const float rd_ratio = rd_valid ? (float)rd / best_rd : 1.0f;
  features[(*feature_idx)++] = (float)rd_valid;
  features[(*feature_idx)++] = rd_ratio;
}

#define FEATURES 31
void av1_ml_early_term_after_split(AV1_COMP *const cpi, MACROBLOCK *const x,
                                   PC_TREE *const pc_tree, BLOCK_SIZE bsize,
                                   int64_t best_rd, int64_t part_none_rd,
                                   int64_t part_split_rd,
                                   int64_t *split_block_rd, int mi_row,
                                   int mi_col,
                                   int *const terminate_partition_search) {
  if (best_rd <= 0 || best_rd == INT64_MAX || *terminate_partition_search)
    return;

  const AV1_COMMON *const cm = &cpi->common;
  const int is_480p_or_larger = AOMMIN(cm->width, cm->height) >= 480;
  const NN_CONFIG *nn_config = NULL;
  float thresh = -1e6;
  switch (bsize) {
    case BLOCK_128X128: break;
    case BLOCK_64X64:
      nn_config = &av1_early_term_after_split_nnconfig_64;
      thresh = is_480p_or_larger ? -2.0f : -1.2f;
      break;
    case BLOCK_32X32:
      nn_config = &av1_early_term_after_split_nnconfig_32;
      thresh = is_480p_or_larger ? -2.6f : -2.3f;
      break;
    case BLOCK_16X16:
      nn_config = &av1_early_term_after_split_nnconfig_16;
      thresh = is_480p_or_larger ? -2.0f : -2.4f;
      break;
    case BLOCK_8X8:
      nn_config = &av1_early_term_after_split_nnconfig_8;
      thresh = is_480p_or_larger ? -1.0f : -1.4f;
      break;
    case BLOCK_4X4: break;
    default:
      assert(0 && "Invalid block size in av1_ml_early_term_after_split().");
      break;
  }
  if (!nn_config) return;

  const MACROBLOCKD *const xd = &x->e_mbd;
  const int dc_q = av1_dc_quant_QTX(x->qindex, 0, xd->bd) >> (xd->bd - 8);
  const int bs = block_size_wide[bsize];
  int f_idx = 0;
  float features[FEATURES] = { 0.0f };

  aom_clear_system_state();

  features[f_idx++] = logf(1.0f + (float)dc_q / 4.0f);
  features[f_idx++] = logf(1.0f + (float)best_rd / bs / bs / 1024.0f);

  add_rd_feature(part_none_rd, best_rd, features, &f_idx);
  add_rd_feature(part_split_rd, best_rd, features, &f_idx);

  for (int i = 0; i < 4; ++i) {
    add_rd_feature(split_block_rd[i], best_rd, features, &f_idx);
    int min_bw = MAX_SB_SIZE_LOG2;
    int min_bh = MAX_SB_SIZE_LOG2;
    get_min_bsize(pc_tree->split[i], &min_bw, &min_bh);
    features[f_idx++] = (float)min_bw;
    features[f_idx++] = (float)min_bh;
  }

  simple_motion_search_prune_part_features(cpi, x, pc_tree, mi_row, mi_col,
                                           bsize, NULL,
                                           FEATURE_SMS_PRUNE_PART_FLAG);

  features[f_idx++] = logf(1.0f + (float)pc_tree->sms_none_feat[1]);

  features[f_idx++] = logf(1.0f + (float)pc_tree->sms_split_feat[1]);
  features[f_idx++] = logf(1.0f + (float)pc_tree->sms_split_feat[3]);
  features[f_idx++] = logf(1.0f + (float)pc_tree->sms_split_feat[5]);
  features[f_idx++] = logf(1.0f + (float)pc_tree->sms_split_feat[7]);

  features[f_idx++] = logf(1.0f + (float)pc_tree->sms_rect_feat[1]);
  features[f_idx++] = logf(1.0f + (float)pc_tree->sms_rect_feat[3]);
  features[f_idx++] = logf(1.0f + (float)pc_tree->sms_rect_feat[5]);
  features[f_idx++] = logf(1.0f + (float)pc_tree->sms_rect_feat[7]);

  assert(f_idx == FEATURES);

  float score = 0.0f;
  av1_nn_predict(features, nn_config, &score);
  // Score is indicator of confidence that we should NOT terminate.
  if (score < thresh) *terminate_partition_search = 1;
}
#undef FEATURES

#define FEATURE_SIZE 19
int av1_ml_prune_2pass_split_partition(const PC_TREE_STATS *pc_tree_stats,
                                       BLOCK_SIZE bsize, int *split_score,
                                       int *none_score) {
  if (!pc_tree_stats->valid) return 0;
  const float *split_weights = NULL;
  const float *none_weights = NULL;
  switch (bsize) {
    case BLOCK_4X4: break;
    case BLOCK_8X8:
      split_weights = av1_2pass_split_partition_weights_8;
      none_weights = av1_2pass_none_partition_weights_8;
      break;
    case BLOCK_16X16:
      split_weights = av1_2pass_split_partition_weights_16;
      none_weights = av1_2pass_none_partition_weights_16;
      break;
    case BLOCK_32X32:
      split_weights = av1_2pass_split_partition_weights_32;
      none_weights = av1_2pass_none_partition_weights_32;
      break;
    case BLOCK_64X64:
      split_weights = av1_2pass_split_partition_weights_64;
      none_weights = av1_2pass_none_partition_weights_64;
      break;
    case BLOCK_128X128:
      split_weights = av1_2pass_split_partition_weights_128;
      none_weights = av1_2pass_none_partition_weights_128;
      break;
    default: assert(0 && "Unexpected bsize.");
  }
  if (!split_weights || !none_weights) return 0;

  aom_clear_system_state();

  float features[FEATURE_SIZE];
  int feature_index = 0;
  features[feature_index++] = (float)pc_tree_stats->split;
  features[feature_index++] = (float)pc_tree_stats->skip;
  const int rdcost = (int)AOMMIN(INT_MAX, pc_tree_stats->rdcost);
  const int rd_valid = rdcost > 0 && rdcost < 1000000000;
  features[feature_index++] = (float)rd_valid;
  for (int i = 0; i < 4; ++i) {
    features[feature_index++] = (float)pc_tree_stats->sub_block_split[i];
    features[feature_index++] = (float)pc_tree_stats->sub_block_skip[i];
    const int sub_rdcost =
        (int)AOMMIN(INT_MAX, pc_tree_stats->sub_block_rdcost[i]);
    const int sub_rd_valid = sub_rdcost > 0 && sub_rdcost < 1000000000;
    features[feature_index++] = (float)sub_rd_valid;
    // Ratio between the sub-block RD and the whole-block RD.
    float rd_ratio = 1.0f;
    if (rd_valid && sub_rd_valid && sub_rdcost < rdcost)
      rd_ratio = (float)sub_rdcost / (float)rdcost;
    features[feature_index++] = rd_ratio;
  }
  assert(feature_index == FEATURE_SIZE);

  float score_1 = split_weights[FEATURE_SIZE];
  float score_2 = none_weights[FEATURE_SIZE];
  for (int i = 0; i < FEATURE_SIZE; ++i) {
    score_1 += features[i] * split_weights[i];
    score_2 += features[i] * none_weights[i];
  }
  *split_score = (int)(score_1 * 100);
  *none_score = (int)(score_2 * 100);
  return 1;
}
#undef FEATURE_SIZE

void av1_ml_prune_rect_partition(const AV1_COMP *const cpi,
                                 const MACROBLOCK *const x, BLOCK_SIZE bsize,
                                 int64_t best_rd, int64_t none_rd,
                                 int64_t *split_rd, int *const dst_prune_horz,
                                 int *const dst_prune_vert) {
  if (bsize < BLOCK_8X8 || best_rd >= 1000000000) return;
  best_rd = AOMMAX(best_rd, 1);
  const NN_CONFIG *nn_config = NULL;
  const float prob_thresholds[5] = { 0.01f, 0.01f, 0.004f, 0.002f, 0.002f };
  float cur_thresh = 0.0f;
  switch (bsize) {
    case BLOCK_8X8:
      nn_config = &av1_rect_partition_nnconfig_8;
      cur_thresh = prob_thresholds[0];
      break;
    case BLOCK_16X16:
      nn_config = &av1_rect_partition_nnconfig_16;
      cur_thresh = prob_thresholds[1];
      break;
    case BLOCK_32X32:
      nn_config = &av1_rect_partition_nnconfig_32;
      cur_thresh = prob_thresholds[2];
      break;
    case BLOCK_64X64:
      nn_config = &av1_rect_partition_nnconfig_64;
      cur_thresh = prob_thresholds[3];
      break;
    case BLOCK_128X128:
      nn_config = &av1_rect_partition_nnconfig_128;
      cur_thresh = prob_thresholds[4];
      break;
    default: assert(0 && "Unexpected bsize.");
  }
  if (!nn_config) return;
  aom_clear_system_state();

  // 1. Compute input features
  float features[9];

  // RD cost ratios
  for (int i = 0; i < 5; i++) features[i] = 1.0f;
  if (none_rd > 0 && none_rd < 1000000000)
    features[0] = (float)none_rd / (float)best_rd;
  for (int i = 0; i < 4; i++) {
    if (split_rd[i] > 0 && split_rd[i] < 1000000000)
      features[1 + i] = (float)split_rd[i] / (float)best_rd;
  }

  // Variance ratios
  const MACROBLOCKD *const xd = &x->e_mbd;
  int whole_block_variance;
  if (is_cur_buf_hbd(xd)) {
    whole_block_variance = av1_high_get_sby_perpixel_variance(
        cpi, &x->plane[0].src, bsize, xd->bd);
  } else {
    whole_block_variance =
        av1_get_sby_perpixel_variance(cpi, &x->plane[0].src, bsize);
  }
  whole_block_variance = AOMMAX(whole_block_variance, 1);

  int split_variance[4];
  const BLOCK_SIZE subsize = get_partition_subsize(bsize, PARTITION_SPLIT);
  struct buf_2d buf;
  buf.stride = x->plane[0].src.stride;
  const int bw = block_size_wide[bsize];
  for (int i = 0; i < 4; ++i) {
    const int x_idx = (i & 1) * bw / 2;
    const int y_idx = (i >> 1) * bw / 2;
    buf.buf = x->plane[0].src.buf + x_idx + y_idx * buf.stride;
    if (is_cur_buf_hbd(xd)) {
      split_variance[i] =
          av1_high_get_sby_perpixel_variance(cpi, &buf, subsize, xd->bd);
    } else {
      split_variance[i] = av1_get_sby_perpixel_variance(cpi, &buf, subsize);
    }
  }

  for (int i = 0; i < 4; i++)
    features[5 + i] = (float)split_variance[i] / (float)whole_block_variance;

  // 2. Do the prediction and prune 0-2 partitions based on their probabilities
  float raw_scores[3] = { 0.0f };
  av1_nn_predict(features, nn_config, raw_scores);
  aom_clear_system_state();
  float probs[3] = { 0.0f };
  av1_nn_softmax(raw_scores, probs, 3);

  // probs[0] is the probability of the fact that both rectangular partitions
  // are worse than current best_rd
  if (probs[1] <= cur_thresh) (*dst_prune_horz) = 1;
  if (probs[2] <= cur_thresh) (*dst_prune_vert) = 1;
}

// Use a ML model to predict if horz_a, horz_b, vert_a, and vert_b should be
// considered.
void av1_ml_prune_ab_partition(BLOCK_SIZE bsize, int part_ctx, int var_ctx,
                               int64_t best_rd, int64_t horz_rd[2],
                               int64_t vert_rd[2], int64_t split_rd[4],
                               int *const horza_partition_allowed,
                               int *const horzb_partition_allowed,
                               int *const verta_partition_allowed,
                               int *const vertb_partition_allowed) {
  if (bsize < BLOCK_8X8 || best_rd >= 1000000000) return;
  const NN_CONFIG *nn_config = NULL;
  switch (bsize) {
    case BLOCK_8X8: nn_config = NULL; break;
    case BLOCK_16X16: nn_config = &av1_ab_partition_nnconfig_16; break;
    case BLOCK_32X32: nn_config = &av1_ab_partition_nnconfig_32; break;
    case BLOCK_64X64: nn_config = &av1_ab_partition_nnconfig_64; break;
    case BLOCK_128X128: nn_config = &av1_ab_partition_nnconfig_128; break;
    default: assert(0 && "Unexpected bsize.");
  }
  if (!nn_config) return;

  aom_clear_system_state();

  // Generate features.
  float features[10];
  int feature_index = 0;
  features[feature_index++] = (float)part_ctx;
  features[feature_index++] = (float)var_ctx;
  const int rdcost = (int)AOMMIN(INT_MAX, best_rd);
  int sub_block_rdcost[8] = { 0 };
  int rd_index = 0;
  for (int i = 0; i < 2; ++i) {
    if (horz_rd[i] > 0 && horz_rd[i] < 1000000000)
      sub_block_rdcost[rd_index] = (int)horz_rd[i];
    ++rd_index;
  }
  for (int i = 0; i < 2; ++i) {
    if (vert_rd[i] > 0 && vert_rd[i] < 1000000000)
      sub_block_rdcost[rd_index] = (int)vert_rd[i];
    ++rd_index;
  }
  for (int i = 0; i < 4; ++i) {
    if (split_rd[i] > 0 && split_rd[i] < 1000000000)
      sub_block_rdcost[rd_index] = (int)split_rd[i];
    ++rd_index;
  }
  for (int i = 0; i < 8; ++i) {
    // Ratio between the sub-block RD and the whole-block RD.
    float rd_ratio = 1.0f;
    if (sub_block_rdcost[i] > 0 && sub_block_rdcost[i] < rdcost)
      rd_ratio = (float)sub_block_rdcost[i] / (float)rdcost;
    features[feature_index++] = rd_ratio;
  }
  assert(feature_index == 10);

  // Calculate scores using the NN model.
  float score[16] = { 0.0f };
  av1_nn_predict(features, nn_config, score);
  aom_clear_system_state();
  int int_score[16];
  int max_score = -1000;
  for (int i = 0; i < 16; ++i) {
    int_score[i] = (int)(100 * score[i]);
    max_score = AOMMAX(int_score[i], max_score);
  }

  // Make decisions based on the model scores.
  int thresh = max_score;
  switch (bsize) {
    case BLOCK_16X16: thresh -= 150; break;
    case BLOCK_32X32: thresh -= 100; break;
    default: break;
  }
  *horza_partition_allowed = 0;
  *horzb_partition_allowed = 0;
  *verta_partition_allowed = 0;
  *vertb_partition_allowed = 0;
  for (int i = 0; i < 16; ++i) {
    if (int_score[i] >= thresh) {
      if ((i >> 0) & 1) *horza_partition_allowed = 1;
      if ((i >> 1) & 1) *horzb_partition_allowed = 1;
      if ((i >> 2) & 1) *verta_partition_allowed = 1;
      if ((i >> 3) & 1) *vertb_partition_allowed = 1;
    }
  }
}

#define FEATURES 18
#define LABELS 4
// Use a ML model to predict if horz4 and vert4 should be considered.
void av1_ml_prune_4_partition(const AV1_COMP *const cpi, MACROBLOCK *const x,
                              BLOCK_SIZE bsize, int part_ctx, int64_t best_rd,
                              int64_t horz_rd[2], int64_t vert_rd[2],
                              int64_t split_rd[4],
                              int *const partition_horz4_allowed,
                              int *const partition_vert4_allowed,
                              unsigned int pb_source_variance, int mi_row,
                              int mi_col) {
  if (best_rd >= 1000000000) return;
  const NN_CONFIG *nn_config = NULL;
  switch (bsize) {
    case BLOCK_16X16: nn_config = &av1_4_partition_nnconfig_16; break;
    case BLOCK_32X32: nn_config = &av1_4_partition_nnconfig_32; break;
    case BLOCK_64X64: nn_config = &av1_4_partition_nnconfig_64; break;
    default: assert(0 && "Unexpected bsize.");
  }
  if (!nn_config) return;

  aom_clear_system_state();

  // Generate features.
  float features[FEATURES];
  int feature_index = 0;
  features[feature_index++] = (float)part_ctx;
  features[feature_index++] = (float)get_unsigned_bits(pb_source_variance);

  const int rdcost = (int)AOMMIN(INT_MAX, best_rd);
  int sub_block_rdcost[8] = { 0 };
  int rd_index = 0;
  for (int i = 0; i < 2; ++i) {
    if (horz_rd[i] > 0 && horz_rd[i] < 1000000000)
      sub_block_rdcost[rd_index] = (int)horz_rd[i];
    ++rd_index;
  }
  for (int i = 0; i < 2; ++i) {
    if (vert_rd[i] > 0 && vert_rd[i] < 1000000000)
      sub_block_rdcost[rd_index] = (int)vert_rd[i];
    ++rd_index;
  }
  for (int i = 0; i < 4; ++i) {
    if (split_rd[i] > 0 && split_rd[i] < 1000000000)
      sub_block_rdcost[rd_index] = (int)split_rd[i];
    ++rd_index;
  }
  for (int i = 0; i < 8; ++i) {
    // Ratio between the sub-block RD and the whole-block RD.
    float rd_ratio = 1.0f;
    if (sub_block_rdcost[i] > 0 && sub_block_rdcost[i] < rdcost)
      rd_ratio = (float)sub_block_rdcost[i] / (float)rdcost;
    features[feature_index++] = rd_ratio;
  }

  // Get variance of the 1:4 and 4:1 sub-blocks.
  unsigned int horz_4_source_var[4] = { 0 };
  unsigned int vert_4_source_var[4] = { 0 };
  {
    BLOCK_SIZE horz_4_bs = get_partition_subsize(bsize, PARTITION_HORZ_4);
    BLOCK_SIZE vert_4_bs = get_partition_subsize(bsize, PARTITION_VERT_4);
    av1_setup_src_planes(x, cpi->source, mi_row, mi_col,
                         av1_num_planes(&cpi->common), bsize);
    const int src_stride = x->plane[0].src.stride;
    uint8_t *src = x->plane[0].src.buf;
    const MACROBLOCKD *const xd = &x->e_mbd;

    struct buf_2d horz_4_src, vert_4_src;
    horz_4_src.stride = src_stride;
    vert_4_src.stride = src_stride;

    for (int i = 0; i < 4; ++i) {
      horz_4_src.buf = src + i * block_size_high[horz_4_bs] * src_stride;
      vert_4_src.buf = src + i * block_size_wide[vert_4_bs];

      if (is_cur_buf_hbd(xd)) {
        horz_4_source_var[i] = av1_high_get_sby_perpixel_variance(
            cpi, &horz_4_src, horz_4_bs, xd->bd);
        vert_4_source_var[i] = av1_high_get_sby_perpixel_variance(
            cpi, &vert_4_src, vert_4_bs, xd->bd);
      } else {
        horz_4_source_var[i] =
            av1_get_sby_perpixel_variance(cpi, &horz_4_src, horz_4_bs);
        vert_4_source_var[i] =
            av1_get_sby_perpixel_variance(cpi, &vert_4_src, vert_4_bs);
      }
    }
  }

  const float denom = (float)(pb_source_variance + 1);
  const float low_b = 0.1f;
  const float high_b = 10.0f;
  for (int i = 0; i < 4; ++i) {
    // Ratio between the 4:1 sub-block variance and the whole-block variance.
    float var_ratio = (float)(horz_4_source_var[i] + 1) / denom;
    if (var_ratio < low_b) var_ratio = low_b;
    if (var_ratio > high_b) var_ratio = high_b;
    features[feature_index++] = var_ratio;
  }
  for (int i = 0; i < 4; ++i) {
    // Ratio between the 1:4 sub-block RD and the whole-block RD.
    float var_ratio = (float)(vert_4_source_var[i] + 1) / denom;
    if (var_ratio < low_b) var_ratio = low_b;
    if (var_ratio > high_b) var_ratio = high_b;
    features[feature_index++] = var_ratio;
  }
  assert(feature_index == FEATURES);

  // Calculate scores using the NN model.
  float score[LABELS] = { 0.0f };
  av1_nn_predict(features, nn_config, score);
  aom_clear_system_state();
  int int_score[LABELS];
  int max_score = -1000;
  for (int i = 0; i < LABELS; ++i) {
    int_score[i] = (int)(100 * score[i]);
    max_score = AOMMAX(int_score[i], max_score);
  }

  // Make decisions based on the model scores.
  int thresh = max_score;
  switch (bsize) {
    case BLOCK_16X16: thresh -= 500; break;
    case BLOCK_32X32: thresh -= 500; break;
    case BLOCK_64X64: thresh -= 200; break;
    default: break;
  }
  *partition_horz4_allowed = 0;
  *partition_vert4_allowed = 0;
  for (int i = 0; i < LABELS; ++i) {
    if (int_score[i] >= thresh) {
      if ((i >> 0) & 1) *partition_horz4_allowed = 1;
      if ((i >> 1) & 1) *partition_vert4_allowed = 1;
    }
  }
}
#undef FEATURES
#undef LABELS

#define FEATURES 4
int av1_ml_predict_breakout(const AV1_COMP *const cpi, BLOCK_SIZE bsize,
                            const MACROBLOCK *const x,
                            const RD_STATS *const rd_stats,
                            unsigned int pb_source_variance) {
  const NN_CONFIG *nn_config = NULL;
  int thresh = 0;
  switch (bsize) {
    case BLOCK_8X8:
      nn_config = &av1_partition_breakout_nnconfig_8;
      thresh = cpi->sf.ml_partition_search_breakout_thresh[0];
      break;
    case BLOCK_16X16:
      nn_config = &av1_partition_breakout_nnconfig_16;
      thresh = cpi->sf.ml_partition_search_breakout_thresh[1];
      break;
    case BLOCK_32X32:
      nn_config = &av1_partition_breakout_nnconfig_32;
      thresh = cpi->sf.ml_partition_search_breakout_thresh[2];
      break;
    case BLOCK_64X64:
      nn_config = &av1_partition_breakout_nnconfig_64;
      thresh = cpi->sf.ml_partition_search_breakout_thresh[3];
      break;
    case BLOCK_128X128:
      nn_config = &av1_partition_breakout_nnconfig_128;
      thresh = cpi->sf.ml_partition_search_breakout_thresh[4];
      break;
    default: assert(0 && "Unexpected bsize.");
  }
  if (!nn_config || thresh < 0) return 0;

  // Generate feature values.
  float features[FEATURES];
  int feature_index = 0;
  aom_clear_system_state();

  const int num_pels_log2 = num_pels_log2_lookup[bsize];
  float rate_f = (float)AOMMIN(rd_stats->rate, INT_MAX);
  rate_f = ((float)x->rdmult / 128.0f / 512.0f / (float)(1 << num_pels_log2)) *
           rate_f;
  features[feature_index++] = rate_f;

  const float dist_f =
      (float)(AOMMIN(rd_stats->dist, INT_MAX) >> num_pels_log2);
  features[feature_index++] = dist_f;

  features[feature_index++] = (float)pb_source_variance;

  const int dc_q = (int)x->plane[0].dequant_QTX[0];
  features[feature_index++] = (float)(dc_q * dc_q) / 256.0f;
  assert(feature_index == FEATURES);

  // Calculate score using the NN model.
  float score = 0.0f;
  av1_nn_predict(features, nn_config, &score);
  aom_clear_system_state();

  // Make decision.
  return (int)(score * 100) >= thresh;
}
#undef FEATURES
