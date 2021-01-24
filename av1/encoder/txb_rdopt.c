/*
 * Copyright (c) 2021, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#include "av1/encoder/txb_rdopt.h"

#include "av1/common/idct.h"
#include "av1/encoder/encodetxb.h"

static const int golomb_bits_cost[32] = {
  0,       512,     512 * 3, 512 * 3, 512 * 5, 512 * 5, 512 * 5, 512 * 5,
  512 * 7, 512 * 7, 512 * 7, 512 * 7, 512 * 7, 512 * 7, 512 * 7, 512 * 7,
  512 * 9, 512 * 9, 512 * 9, 512 * 9, 512 * 9, 512 * 9, 512 * 9, 512 * 9,
  512 * 9, 512 * 9, 512 * 9, 512 * 9, 512 * 9, 512 * 9, 512 * 9, 512 * 9
};
static const int golomb_cost_diff[32] = {
  0,       512, 512 * 2, 0, 512 * 2, 0, 0, 0, 512 * 2, 0, 0, 0, 0, 0, 0, 0,
  512 * 2, 0,   0,       0, 0,       0, 0, 0, 0,       0, 0, 0, 0, 0, 0, 0
};

static INLINE int get_dqv(const int16_t *dequant, int coeff_idx,
                          const qm_val_t *iqmatrix) {
  int dqv = dequant[!!coeff_idx];
  if (iqmatrix != NULL)
    dqv =
        ((iqmatrix[coeff_idx] * dqv) + (1 << (AOM_QM_BITS - 1))) >> AOM_QM_BITS;
  return dqv;
}

static INLINE int64_t get_coeff_dist(tran_low_t tcoeff, tran_low_t dqcoeff,
                                     int shift) {
  const int64_t diff = (tcoeff - dqcoeff) * (1 << shift);
  const int64_t error = diff * diff;
  return error;
}

static int get_eob_cost(int eob, const LV_MAP_EOB_COST *txb_eob_costs,
                        const LV_MAP_COEFF_COST *txb_costs, TX_CLASS tx_class) {
  int eob_extra;
  const int eob_pt = av1_get_eob_pos_token(eob, &eob_extra);
  int eob_cost = 0;
  const int eob_multi_ctx = (tx_class == TX_CLASS_2D) ? 0 : 1;
  eob_cost = txb_eob_costs->eob_cost[eob_multi_ctx][eob_pt - 1];

  if (av1_eob_offset_bits[eob_pt] > 0) {
    const int eob_ctx = eob_pt - 3;
    const int eob_shift = av1_eob_offset_bits[eob_pt] - 1;
    const int bit = (eob_extra & (1 << eob_shift)) ? 1 : 0;
    eob_cost += txb_costs->eob_extra_cost[eob_ctx][bit];
    const int offset_bits = av1_eob_offset_bits[eob_pt];
    if (offset_bits > 1) eob_cost += av1_cost_literal(offset_bits - 1);
  }
  return eob_cost;
}

static INLINE int get_golomb_cost(int abs_qc) {
  if (abs_qc >= 1 + NUM_BASE_LEVELS + COEFF_BASE_RANGE) {
    const int r = abs_qc - COEFF_BASE_RANGE - NUM_BASE_LEVELS;
    const int length = get_msb(r) + 1;
    return av1_cost_literal(2 * length - 1);
  }
  return 0;
}

static INLINE int get_br_cost(tran_low_t level, const int *coeff_lps) {
  const int base_range = AOMMIN(level - 1 - NUM_BASE_LEVELS, COEFF_BASE_RANGE);
  return coeff_lps[base_range] + get_golomb_cost(level);
}

static INLINE int get_br_cost_with_diff(tran_low_t level, const int *coeff_lps,
                                        int *diff) {
  const int base_range = AOMMIN(level - 1 - NUM_BASE_LEVELS, COEFF_BASE_RANGE);
  int golomb_bits = 0;
  if (level <= COEFF_BASE_RANGE + 1 + NUM_BASE_LEVELS)
    *diff += coeff_lps[base_range + COEFF_BASE_RANGE + 1];

  if (level >= COEFF_BASE_RANGE + 1 + NUM_BASE_LEVELS) {
    int r = level - COEFF_BASE_RANGE - NUM_BASE_LEVELS;
    if (r < 32) {
      golomb_bits = golomb_bits_cost[r];
      *diff += golomb_cost_diff[r];
    } else {
      golomb_bits = get_golomb_cost(level);
      *diff += (r & (r - 1)) == 0 ? 1024 : 0;
    }
  }

  return coeff_lps[base_range] + golomb_bits;
}

static AOM_FORCE_INLINE int get_two_coeff_cost_simple(
    int ci, tran_low_t abs_qc, int coeff_ctx,
    const LV_MAP_COEFF_COST *txb_costs, int bwl, TX_CLASS tx_class,
    const uint8_t *levels, int *cost_low) {
  // this simple version assumes the coeff's scan_idx is not DC (scan_idx != 0)
  // and not the last (scan_idx != eob - 1)
  assert(ci > 0);
  int cost = txb_costs->base_cost[coeff_ctx][AOMMIN(abs_qc, 3)];
  int diff = 0;
  if (abs_qc <= 3) diff = txb_costs->base_cost[coeff_ctx][abs_qc + 4];
  if (abs_qc) {
    cost += av1_cost_literal(1);
    if (abs_qc > NUM_BASE_LEVELS) {
      const int br_ctx = get_br_ctx(levels, ci, bwl, tx_class);
      int brcost_diff = 0;
      cost += get_br_cost_with_diff(abs_qc, txb_costs->lps_cost[br_ctx],
                                    &brcost_diff);
      diff += brcost_diff;
    }
  }
  *cost_low = cost - diff;

  return cost;
}

static INLINE int get_coeff_cost_eob(int ci, tran_low_t abs_qc, int sign,
                                     int coeff_ctx, int dc_sign_ctx,
                                     const LV_MAP_COEFF_COST *txb_costs,
                                     int bwl, TX_CLASS tx_class) {
  int cost = 0;
  cost += txb_costs->base_eob_cost[coeff_ctx][AOMMIN(abs_qc, 3) - 1];
  if (abs_qc != 0) {
    if (ci == 0) {
      cost += txb_costs->dc_sign_cost[dc_sign_ctx][sign];
    } else {
      cost += av1_cost_literal(1);
    }
    if (abs_qc > NUM_BASE_LEVELS) {
      int br_ctx;
      br_ctx = get_br_ctx_eob(ci, bwl, tx_class);
      cost += get_br_cost(abs_qc, txb_costs->lps_cost[br_ctx]);
    }
  }
  return cost;
}

static INLINE int get_coeff_cost_general(int is_last, int ci, tran_low_t abs_qc,
                                         int sign, int coeff_ctx,
                                         int dc_sign_ctx,
                                         const LV_MAP_COEFF_COST *txb_costs,
                                         int bwl, TX_CLASS tx_class,
                                         const uint8_t *levels) {
  int cost = 0;
  if (is_last) {
    cost += txb_costs->base_eob_cost[coeff_ctx][AOMMIN(abs_qc, 3) - 1];
  } else {
    cost += txb_costs->base_cost[coeff_ctx][AOMMIN(abs_qc, 3)];
  }
  if (abs_qc != 0) {
    if (ci == 0) {
      cost += txb_costs->dc_sign_cost[dc_sign_ctx][sign];
    } else {
      cost += av1_cost_literal(1);
    }
    if (abs_qc > NUM_BASE_LEVELS) {
      int br_ctx;
      if (is_last)
        br_ctx = get_br_ctx_eob(ci, bwl, tx_class);
      else
        br_ctx = get_br_ctx(levels, ci, bwl, tx_class);
      cost += get_br_cost(abs_qc, txb_costs->lps_cost[br_ctx]);
    }
  }
  return cost;
}

static INLINE void get_qc_dqc_low(tran_low_t abs_qc, int sign, int dqv,
                                  int shift, tran_low_t *qc_low,
                                  tran_low_t *dqc_low) {
  tran_low_t abs_qc_low = abs_qc - 1;
  *qc_low = (-sign ^ abs_qc_low) + sign;
  assert((sign ? -abs_qc_low : abs_qc_low) == *qc_low);
  tran_low_t abs_dqc_low = (abs_qc_low * dqv) >> shift;
  *dqc_low = (-sign ^ abs_dqc_low) + sign;
  assert((sign ? -abs_dqc_low : abs_dqc_low) == *dqc_low);
}

static INLINE void update_coeff_general(
    int *accu_rate, int64_t *accu_dist, int si, int eob, TX_SIZE tx_size,
    TX_CLASS tx_class, int bwl, int height, int64_t rdmult, int shift,
    int dc_sign_ctx, const int16_t *dequant, const int16_t *scan,
    const LV_MAP_COEFF_COST *txb_costs, const tran_low_t *tcoeff,
    tran_low_t *qcoeff, tran_low_t *dqcoeff, uint8_t *levels,
    const qm_val_t *iqmatrix) {
  const int dqv = get_dqv(dequant, scan[si], iqmatrix);
  const int ci = scan[si];
  const tran_low_t qc = qcoeff[ci];
  const int is_last = si == (eob - 1);
  const int coeff_ctx = get_lower_levels_ctx_general(
      is_last, si, bwl, height, levels, ci, tx_size, tx_class);
  if (qc == 0) {
    *accu_rate += txb_costs->base_cost[coeff_ctx][0];
  } else {
    const int sign = (qc < 0) ? 1 : 0;
    const tran_low_t abs_qc = abs(qc);
    const tran_low_t tqc = tcoeff[ci];
    const tran_low_t dqc = dqcoeff[ci];
    const int64_t dist = get_coeff_dist(tqc, dqc, shift);
    const int64_t dist0 = get_coeff_dist(tqc, 0, shift);
    const int rate =
        get_coeff_cost_general(is_last, ci, abs_qc, sign, coeff_ctx,
                               dc_sign_ctx, txb_costs, bwl, tx_class, levels);
    const int64_t rd = RDCOST(rdmult, rate, dist);

    tran_low_t qc_low, dqc_low;
    tran_low_t abs_qc_low;
    int64_t dist_low, rd_low;
    int rate_low;
    if (abs_qc == 1) {
      abs_qc_low = qc_low = dqc_low = 0;
      dist_low = dist0;
      rate_low = txb_costs->base_cost[coeff_ctx][0];
    } else {
      get_qc_dqc_low(abs_qc, sign, dqv, shift, &qc_low, &dqc_low);
      abs_qc_low = abs_qc - 1;
      dist_low = get_coeff_dist(tqc, dqc_low, shift);
      rate_low =
          get_coeff_cost_general(is_last, ci, abs_qc_low, sign, coeff_ctx,
                                 dc_sign_ctx, txb_costs, bwl, tx_class, levels);
    }

    rd_low = RDCOST(rdmult, rate_low, dist_low);
    if (rd_low < rd) {
      qcoeff[ci] = qc_low;
      dqcoeff[ci] = dqc_low;
      levels[get_padded_idx(ci, bwl)] = AOMMIN(abs_qc_low, INT8_MAX);
      *accu_rate += rate_low;
      *accu_dist += dist_low - dist0;
    } else {
      *accu_rate += rate;
      *accu_dist += dist - dist0;
    }
  }
}

static AOM_FORCE_INLINE void update_coeff_simple(
    int *accu_rate, int si, int eob, TX_SIZE tx_size, TX_CLASS tx_class,
    int bwl, int64_t rdmult, int shift, const int16_t *dequant,
    const int16_t *scan, const LV_MAP_COEFF_COST *txb_costs,
    const tran_low_t *tcoeff, tran_low_t *qcoeff, tran_low_t *dqcoeff,
    uint8_t *levels, const qm_val_t *iqmatrix) {
  const int dqv = get_dqv(dequant, scan[si], iqmatrix);
  (void)eob;
  // this simple version assumes the coeff's scan_idx is not DC (scan_idx != 0)
  // and not the last (scan_idx != eob - 1)
  assert(si != eob - 1);
  assert(si > 0);
  const int ci = scan[si];
  const tran_low_t qc = qcoeff[ci];
  const int coeff_ctx =
      get_lower_levels_ctx(levels, ci, bwl, tx_size, tx_class);
  if (qc == 0) {
    *accu_rate += txb_costs->base_cost[coeff_ctx][0];
  } else {
    const tran_low_t abs_qc = abs(qc);
    const tran_low_t abs_tqc = abs(tcoeff[ci]);
    const tran_low_t abs_dqc = abs(dqcoeff[ci]);
    int rate_low = 0;
    const int rate = get_two_coeff_cost_simple(
        ci, abs_qc, coeff_ctx, txb_costs, bwl, tx_class, levels, &rate_low);
    if (abs_dqc < abs_tqc) {
      *accu_rate += rate;
      return;
    }

    const int64_t dist = get_coeff_dist(abs_tqc, abs_dqc, shift);
    const int64_t rd = RDCOST(rdmult, rate, dist);

    const tran_low_t abs_qc_low = abs_qc - 1;
    const tran_low_t abs_dqc_low = (abs_qc_low * dqv) >> shift;
    const int64_t dist_low = get_coeff_dist(abs_tqc, abs_dqc_low, shift);
    const int64_t rd_low = RDCOST(rdmult, rate_low, dist_low);

    if (rd_low < rd) {
      const int sign = (qc < 0) ? 1 : 0;
      qcoeff[ci] = (-sign ^ abs_qc_low) + sign;
      dqcoeff[ci] = (-sign ^ abs_dqc_low) + sign;
      levels[get_padded_idx(ci, bwl)] = AOMMIN(abs_qc_low, INT8_MAX);
      *accu_rate += rate_low;
    } else {
      *accu_rate += rate;
    }
  }
}

static AOM_FORCE_INLINE void update_coeff_eob(
    int *accu_rate, int64_t *accu_dist, int *eob, int *nz_num, int *nz_ci,
    int si, TX_SIZE tx_size, TX_CLASS tx_class, int bwl, int height,
    int dc_sign_ctx, int64_t rdmult, int shift, const int16_t *dequant,
    const int16_t *scan, const LV_MAP_EOB_COST *txb_eob_costs,
    const LV_MAP_COEFF_COST *txb_costs, const tran_low_t *tcoeff,
    tran_low_t *qcoeff, tran_low_t *dqcoeff, uint8_t *levels, int sharpness,
    const qm_val_t *iqmatrix) {
  const int dqv = get_dqv(dequant, scan[si], iqmatrix);
  assert(si != *eob - 1);
  const int ci = scan[si];
  const tran_low_t qc = qcoeff[ci];
  const int coeff_ctx =
      get_lower_levels_ctx(levels, ci, bwl, tx_size, tx_class);
  if (qc == 0) {
    *accu_rate += txb_costs->base_cost[coeff_ctx][0];
  } else {
    int lower_level = 0;
    const tran_low_t abs_qc = abs(qc);
    const tran_low_t tqc = tcoeff[ci];
    const tran_low_t dqc = dqcoeff[ci];
    const int sign = (qc < 0) ? 1 : 0;
    const int64_t dist0 = get_coeff_dist(tqc, 0, shift);
    int64_t dist = get_coeff_dist(tqc, dqc, shift) - dist0;
    int rate =
        get_coeff_cost_general(0, ci, abs_qc, sign, coeff_ctx, dc_sign_ctx,
                               txb_costs, bwl, tx_class, levels);
    int64_t rd = RDCOST(rdmult, *accu_rate + rate, *accu_dist + dist);

    tran_low_t qc_low, dqc_low;
    tran_low_t abs_qc_low;
    int64_t dist_low, rd_low;
    int rate_low;
    if (abs_qc == 1) {
      abs_qc_low = 0;
      dqc_low = qc_low = 0;
      dist_low = 0;
      rate_low = txb_costs->base_cost[coeff_ctx][0];
      rd_low = RDCOST(rdmult, *accu_rate + rate_low, *accu_dist);
    } else {
      get_qc_dqc_low(abs_qc, sign, dqv, shift, &qc_low, &dqc_low);
      abs_qc_low = abs_qc - 1;
      dist_low = get_coeff_dist(tqc, dqc_low, shift) - dist0;
      rate_low =
          get_coeff_cost_general(0, ci, abs_qc_low, sign, coeff_ctx,
                                 dc_sign_ctx, txb_costs, bwl, tx_class, levels);
      rd_low = RDCOST(rdmult, *accu_rate + rate_low, *accu_dist + dist_low);
    }

    int lower_level_new_eob = 0;
    const int new_eob = si + 1;
    const int coeff_ctx_new_eob = get_lower_levels_ctx_eob(bwl, height, si);
    const int new_eob_cost =
        get_eob_cost(new_eob, txb_eob_costs, txb_costs, tx_class);
    int rate_coeff_eob =
        new_eob_cost + get_coeff_cost_eob(ci, abs_qc, sign, coeff_ctx_new_eob,
                                          dc_sign_ctx, txb_costs, bwl,
                                          tx_class);
    int64_t dist_new_eob = dist;
    int64_t rd_new_eob = RDCOST(rdmult, rate_coeff_eob, dist_new_eob);

    if (abs_qc_low > 0) {
      const int rate_coeff_eob_low =
          new_eob_cost + get_coeff_cost_eob(ci, abs_qc_low, sign,
                                            coeff_ctx_new_eob, dc_sign_ctx,
                                            txb_costs, bwl, tx_class);
      const int64_t dist_new_eob_low = dist_low;
      const int64_t rd_new_eob_low =
          RDCOST(rdmult, rate_coeff_eob_low, dist_new_eob_low);
      if (rd_new_eob_low < rd_new_eob) {
        lower_level_new_eob = 1;
        rd_new_eob = rd_new_eob_low;
        rate_coeff_eob = rate_coeff_eob_low;
        dist_new_eob = dist_new_eob_low;
      }
    }

    if (rd_low < rd) {
      lower_level = 1;
      rd = rd_low;
      rate = rate_low;
      dist = dist_low;
    }

    if (sharpness == 0 && rd_new_eob < rd) {
      for (int ni = 0; ni < *nz_num; ++ni) {
        int last_ci = nz_ci[ni];
        levels[get_padded_idx(last_ci, bwl)] = 0;
        qcoeff[last_ci] = 0;
        dqcoeff[last_ci] = 0;
      }
      *eob = new_eob;
      *nz_num = 0;
      *accu_rate = rate_coeff_eob;
      *accu_dist = dist_new_eob;
      lower_level = lower_level_new_eob;
    } else {
      *accu_rate += rate;
      *accu_dist += dist;
    }

    if (lower_level) {
      qcoeff[ci] = qc_low;
      dqcoeff[ci] = dqc_low;
      levels[get_padded_idx(ci, bwl)] = AOMMIN(abs_qc_low, INT8_MAX);
    }
    if (qcoeff[ci]) {
      nz_ci[*nz_num] = ci;
      ++*nz_num;
    }
  }
}

static INLINE void update_skip(int *accu_rate, int64_t accu_dist, int *eob,
                               int nz_num, int *nz_ci, int64_t rdmult,
                               int skip_cost, int non_skip_cost,
                               tran_low_t *qcoeff, tran_low_t *dqcoeff,
                               int sharpness) {
  const int64_t rd = RDCOST(rdmult, *accu_rate + non_skip_cost, accu_dist);
  const int64_t rd_new_eob = RDCOST(rdmult, skip_cost, 0);
  if (sharpness == 0 && rd_new_eob < rd) {
    for (int i = 0; i < nz_num; ++i) {
      const int ci = nz_ci[i];
      qcoeff[ci] = 0;
      dqcoeff[ci] = 0;
      // no need to set up levels because this is the last step
      // levels[get_padded_idx(ci, bwl)] = 0;
    }
    *accu_rate = 0;
    *eob = 0;
  }
}

// TODO(angiebird): use this function whenever it's possible
static int get_tx_type_cost(const MACROBLOCK *x, const MACROBLOCKD *xd,
                            int plane, TX_SIZE tx_size, TX_TYPE tx_type,
                            int reduced_tx_set_used) {
  if (plane > 0) return 0;

  const TX_SIZE square_tx_size = txsize_sqr_map[tx_size];

  const MB_MODE_INFO *mbmi = xd->mi[0];
  const int is_inter = is_inter_block(mbmi);
  if (get_ext_tx_types(tx_size, is_inter, reduced_tx_set_used) > 1 &&
      !xd->lossless[xd->mi[0]->segment_id]) {
    const int ext_tx_set =
        get_ext_tx_set(tx_size, is_inter, reduced_tx_set_used);
    if (is_inter) {
      if (ext_tx_set > 0)
        return x->mode_costs
            .inter_tx_type_costs[ext_tx_set][square_tx_size][tx_type];
    } else {
      if (ext_tx_set > 0) {
        PREDICTION_MODE intra_dir;
        if (mbmi->filter_intra_mode_info.use_filter_intra)
          intra_dir = fimode_to_intradir[mbmi->filter_intra_mode_info
                                             .filter_intra_mode];
        else
          intra_dir = mbmi->mode;
        return x->mode_costs.intra_tx_type_costs[ext_tx_set][square_tx_size]
                                                [intra_dir][tx_type];
      }
    }
  }
  return 0;
}

int av1_optimize_txb(const struct AV1_COMP *cpi, MACROBLOCK *x, int plane,
                     int block, TX_SIZE tx_size, TX_TYPE tx_type,
                     const TXB_CTX *const txb_ctx, int *rate_cost,
                     int sharpness) {
  MACROBLOCKD *xd = &x->e_mbd;
  const struct macroblock_plane *p = &x->plane[plane];
  const SCAN_ORDER *scan_order = get_scan(tx_size, tx_type);
  const int16_t *scan = scan_order->scan;
  const int shift = av1_get_tx_scale(tx_size);
  int eob = p->eobs[block];
  const int16_t *dequant = p->dequant_QTX;
  const qm_val_t *iqmatrix =
      av1_get_iqmatrix(&cpi->common.quant_params, xd, plane, tx_size, tx_type);
  const int block_offset = BLOCK_OFFSET(block);
  tran_low_t *qcoeff = p->qcoeff + block_offset;
  tran_low_t *dqcoeff = p->dqcoeff + block_offset;
  const tran_low_t *tcoeff = p->coeff + block_offset;
  const CoeffCosts *coeff_costs = &x->coeff_costs;

  // This function is not called if eob = 0.
  assert(eob > 0);

  const AV1_COMMON *cm = &cpi->common;
  const PLANE_TYPE plane_type = get_plane_type(plane);
  const TX_SIZE txs_ctx = get_txsize_entropy_ctx(tx_size);
  const TX_CLASS tx_class = tx_type_to_class[tx_type];
  const MB_MODE_INFO *mbmi = xd->mi[0];
  const int bwl = get_txb_bwl(tx_size);
  const int width = get_txb_wide(tx_size);
  const int height = get_txb_high(tx_size);
  assert(width == (1 << bwl));
  const int is_inter = is_inter_block(mbmi);
  const LV_MAP_COEFF_COST *txb_costs =
      &coeff_costs->coeff_costs[txs_ctx][plane_type];
  const int eob_multi_size = txsize_log2_minus4[tx_size];
  const LV_MAP_EOB_COST *txb_eob_costs =
      &coeff_costs->eob_costs[eob_multi_size][plane_type];

  const int rshift =
      (sharpness +
       (cpi->oxcf.q_cfg.aq_mode == VARIANCE_AQ && mbmi->segment_id < 4
            ? 7 - mbmi->segment_id
            : 2) +
       (cpi->oxcf.q_cfg.aq_mode != VARIANCE_AQ &&
                cpi->oxcf.q_cfg.deltaq_mode == DELTA_Q_PERCEPTUAL &&
                cm->delta_q_info.delta_q_present_flag && x->sb_energy_level < 0
            ? (3 - x->sb_energy_level)
            : 0));
  const int64_t rdmult =
      (((int64_t)x->rdmult *
        (plane_rd_mult[is_inter][plane_type] << (2 * (xd->bd - 8)))) +
       2) >>
      rshift;

  uint8_t levels_buf[TX_PAD_2D];
  uint8_t *const levels = set_levels(levels_buf, width);

  if (eob > 1) av1_txb_init_levels(qcoeff, width, height, levels);

  // TODO(angirbird): check iqmatrix

  const int non_skip_cost = txb_costs->txb_skip_cost[txb_ctx->txb_skip_ctx][0];
  const int skip_cost = txb_costs->txb_skip_cost[txb_ctx->txb_skip_ctx][1];
  const int eob_cost = get_eob_cost(eob, txb_eob_costs, txb_costs, tx_class);
  int accu_rate = eob_cost;
  int64_t accu_dist = 0;
  int si = eob - 1;
  const int ci = scan[si];
  const tran_low_t qc = qcoeff[ci];
  const tran_low_t abs_qc = abs(qc);
  const int sign = qc < 0;
  const int max_nz_num = 2;
  int nz_num = 1;
  int nz_ci[3] = { ci, 0, 0 };
  if (abs_qc >= 2) {
    update_coeff_general(&accu_rate, &accu_dist, si, eob, tx_size, tx_class,
                         bwl, height, rdmult, shift, txb_ctx->dc_sign_ctx,
                         dequant, scan, txb_costs, tcoeff, qcoeff, dqcoeff,
                         levels, iqmatrix);
    --si;
  } else {
    assert(abs_qc == 1);
    const int coeff_ctx = get_lower_levels_ctx_eob(bwl, height, si);
    accu_rate +=
        get_coeff_cost_eob(ci, abs_qc, sign, coeff_ctx, txb_ctx->dc_sign_ctx,
                           txb_costs, bwl, tx_class);
    const tran_low_t tqc = tcoeff[ci];
    const tran_low_t dqc = dqcoeff[ci];
    const int64_t dist = get_coeff_dist(tqc, dqc, shift);
    const int64_t dist0 = get_coeff_dist(tqc, 0, shift);
    accu_dist += dist - dist0;
    --si;
  }

#define UPDATE_COEFF_EOB_CASE(tx_class_literal)                            \
  case tx_class_literal:                                                   \
    for (; si >= 0 && nz_num <= max_nz_num; --si) {                        \
      update_coeff_eob(&accu_rate, &accu_dist, &eob, &nz_num, nz_ci, si,   \
                       tx_size, tx_class_literal, bwl, height,             \
                       txb_ctx->dc_sign_ctx, rdmult, shift, dequant, scan, \
                       txb_eob_costs, txb_costs, tcoeff, qcoeff, dqcoeff,  \
                       levels, sharpness, iqmatrix);                       \
    }                                                                      \
    break;
  switch (tx_class) {
    UPDATE_COEFF_EOB_CASE(TX_CLASS_2D);
    UPDATE_COEFF_EOB_CASE(TX_CLASS_HORIZ);
    UPDATE_COEFF_EOB_CASE(TX_CLASS_VERT);
#undef UPDATE_COEFF_EOB_CASE
    default: assert(false);
  }

  if (si == -1 && nz_num <= max_nz_num) {
    update_skip(&accu_rate, accu_dist, &eob, nz_num, nz_ci, rdmult, skip_cost,
                non_skip_cost, qcoeff, dqcoeff, sharpness);
  }

#define UPDATE_COEFF_SIMPLE_CASE(tx_class_literal)                             \
  case tx_class_literal:                                                       \
    for (; si >= 1; --si) {                                                    \
      update_coeff_simple(&accu_rate, si, eob, tx_size, tx_class_literal, bwl, \
                          rdmult, shift, dequant, scan, txb_costs, tcoeff,     \
                          qcoeff, dqcoeff, levels, iqmatrix);                  \
    }                                                                          \
    break;
  switch (tx_class) {
    UPDATE_COEFF_SIMPLE_CASE(TX_CLASS_2D);
    UPDATE_COEFF_SIMPLE_CASE(TX_CLASS_HORIZ);
    UPDATE_COEFF_SIMPLE_CASE(TX_CLASS_VERT);
#undef UPDATE_COEFF_SIMPLE_CASE
    default: assert(false);
  }

  // DC position
  if (si == 0) {
    // no need to update accu_dist because it's not used after this point
    int64_t dummy_dist = 0;
    update_coeff_general(&accu_rate, &dummy_dist, si, eob, tx_size, tx_class,
                         bwl, height, rdmult, shift, txb_ctx->dc_sign_ctx,
                         dequant, scan, txb_costs, tcoeff, qcoeff, dqcoeff,
                         levels, iqmatrix);
  }

  const int tx_type_cost = get_tx_type_cost(x, xd, plane, tx_size, tx_type,
                                            cm->features.reduced_tx_set_used);
  if (eob == 0)
    accu_rate += skip_cost;
  else
    accu_rate += non_skip_cost + tx_type_cost;

  p->eobs[block] = eob;
  p->txb_entropy_ctx[block] =
      av1_get_txb_entropy_context(qcoeff, scan_order, p->eobs[block]);

  *rate_cost = accu_rate;
  return eob;
}

static INLINE void update_coeff_eob_fast(int *eob, int shift,
                                         const int16_t *dequant_ptr,
                                         const int16_t *scan,
                                         const tran_low_t *coeff_ptr,
                                         tran_low_t *qcoeff_ptr,
                                         tran_low_t *dqcoeff_ptr) {
  // TODO(sarahparker) make this work for aomqm
  int eob_out = *eob;
  int zbin[2] = { dequant_ptr[0] + ROUND_POWER_OF_TWO(dequant_ptr[0] * 70, 7),
                  dequant_ptr[1] + ROUND_POWER_OF_TWO(dequant_ptr[1] * 70, 7) };

  for (int i = *eob - 1; i >= 0; i--) {
    const int rc = scan[i];
    const int qcoeff = qcoeff_ptr[rc];
    const int coeff = coeff_ptr[rc];
    const int coeff_sign = AOMSIGN(coeff);
    int64_t abs_coeff = (coeff ^ coeff_sign) - coeff_sign;

    if (((abs_coeff << (1 + shift)) < zbin[rc != 0]) || (qcoeff == 0)) {
      eob_out--;
      qcoeff_ptr[rc] = 0;
      dqcoeff_ptr[rc] = 0;
    } else {
      break;
    }
  }

  *eob = eob_out;
}

static AOM_FORCE_INLINE int warehouse_efficients_txb(
    const MACROBLOCK *x, const int plane, const int block,
    const TX_SIZE tx_size, const TXB_CTX *const txb_ctx,
    const struct macroblock_plane *p, const int eob,
    const PLANE_TYPE plane_type, const LV_MAP_COEFF_COST *const coeff_costs,
    const MACROBLOCKD *const xd, const TX_TYPE tx_type, const TX_CLASS tx_class,
    int reduced_tx_set_used) {
  const tran_low_t *const qcoeff = p->qcoeff + BLOCK_OFFSET(block);
  const int txb_skip_ctx = txb_ctx->txb_skip_ctx;
  const int bwl = get_txb_bwl(tx_size);
  const int width = get_txb_wide(tx_size);
  const int height = get_txb_high(tx_size);
  const SCAN_ORDER *const scan_order = get_scan(tx_size, tx_type);
  const int16_t *const scan = scan_order->scan;
  uint8_t levels_buf[TX_PAD_2D];
  uint8_t *const levels = set_levels(levels_buf, width);
  DECLARE_ALIGNED(16, int8_t, coeff_contexts[MAX_TX_SQUARE]);
  const int eob_multi_size = txsize_log2_minus4[tx_size];
  const LV_MAP_EOB_COST *const eob_costs =
      &x->coeff_costs.eob_costs[eob_multi_size][plane_type];
  int cost = coeff_costs->txb_skip_cost[txb_skip_ctx][0];

  av1_txb_init_levels(qcoeff, width, height, levels);

  cost += get_tx_type_cost(x, xd, plane, tx_size, tx_type, reduced_tx_set_used);

  cost += get_eob_cost(eob, eob_costs, coeff_costs, tx_class);

  av1_get_nz_map_contexts(levels, scan, eob, tx_size, tx_class, coeff_contexts);

  const int(*lps_cost)[COEFF_BASE_RANGE + 1 + COEFF_BASE_RANGE + 1] =
      coeff_costs->lps_cost;
  int c = eob - 1;
  {
    const int pos = scan[c];
    const tran_low_t v = qcoeff[pos];
    const int sign = AOMSIGN(v);
    const int level = (v ^ sign) - sign;
    const int coeff_ctx = coeff_contexts[pos];
    cost += coeff_costs->base_eob_cost[coeff_ctx][AOMMIN(level, 3) - 1];

    if (v) {
      // sign bit cost
      if (level > NUM_BASE_LEVELS) {
        const int ctx = get_br_ctx_eob(pos, bwl, tx_class);
        cost += get_br_cost(level, lps_cost[ctx]);
      }
      if (c) {
        cost += av1_cost_literal(1);
      } else {
        const int sign01 = (sign ^ sign) - sign;
        const int dc_sign_ctx = txb_ctx->dc_sign_ctx;
        cost += coeff_costs->dc_sign_cost[dc_sign_ctx][sign01];
        return cost;
      }
    }
  }
  const int(*base_cost)[8] = coeff_costs->base_cost;
  for (c = eob - 2; c >= 1; --c) {
    const int pos = scan[c];
    const int coeff_ctx = coeff_contexts[pos];
    const tran_low_t v = qcoeff[pos];
    const int level = abs(v);
    cost += base_cost[coeff_ctx][AOMMIN(level, 3)];
    if (v) {
      // sign bit cost
      cost += av1_cost_literal(1);
      if (level > NUM_BASE_LEVELS) {
        const int ctx = get_br_ctx(levels, pos, bwl, tx_class);
        cost += get_br_cost(level, lps_cost[ctx]);
      }
    }
  }
  // c == 0 after previous loop
  {
    const int pos = scan[c];
    const tran_low_t v = qcoeff[pos];
    const int coeff_ctx = coeff_contexts[pos];
    const int sign = AOMSIGN(v);
    const int level = (v ^ sign) - sign;
    cost += base_cost[coeff_ctx][AOMMIN(level, 3)];

    if (v) {
      // sign bit cost
      const int sign01 = (sign ^ sign) - sign;
      const int dc_sign_ctx = txb_ctx->dc_sign_ctx;
      cost += coeff_costs->dc_sign_cost[dc_sign_ctx][sign01];
      if (level > NUM_BASE_LEVELS) {
        const int ctx = get_br_ctx(levels, pos, bwl, tx_class);
        cost += get_br_cost(level, lps_cost[ctx]);
      }
    }
  }
  return cost;
}

// Look up table of individual cost of coefficient by its quantization level.
// determined based on Laplacian distribution conditioned on estimated context
static const int costLUT[15] = { -1143, 53,   545,  825,  1031,
                                 1209,  1393, 1577, 1763, 1947,
                                 2132,  2317, 2501, 2686, 2871 };
static const int const_term = (1 << AV1_PROB_COST_SHIFT);
static const int loge_par = ((14427 << AV1_PROB_COST_SHIFT) + 5000) / 10000;
int av1_cost_coeffs_txb_estimate(const MACROBLOCK *x, const int plane,
                                 const int block, const TX_SIZE tx_size,
                                 const TX_TYPE tx_type) {
  assert(plane == 0);

  int cost = 0;
  const struct macroblock_plane *p = &x->plane[plane];
  const SCAN_ORDER *scan_order = get_scan(tx_size, tx_type);
  const int16_t *scan = scan_order->scan;
  tran_low_t *qcoeff = p->qcoeff + BLOCK_OFFSET(block);

  int eob = p->eobs[block];

  // coeffs
  int c = eob - 1;
  // eob
  {
    const int pos = scan[c];
    const tran_low_t v = abs(qcoeff[pos]) - 1;
    cost += (v << (AV1_PROB_COST_SHIFT + 2));
  }
  // other coeffs
  for (c = eob - 2; c >= 0; c--) {
    const int pos = scan[c];
    const tran_low_t v = abs(qcoeff[pos]);
    const int idx = AOMMIN(v, 14);

    cost += costLUT[idx];
  }

  // const_term does not contain DC, and log(e) does not contain eob, so both
  // (eob-1)
  cost += (const_term + loge_par) * (eob - 1);

  return cost;
}

static AOM_FORCE_INLINE int warehouse_efficients_txb_laplacian(
    const MACROBLOCK *x, const int plane, const int block,
    const TX_SIZE tx_size, const TXB_CTX *const txb_ctx, const int eob,
    const PLANE_TYPE plane_type, const LV_MAP_COEFF_COST *const coeff_costs,
    const MACROBLOCKD *const xd, const TX_TYPE tx_type, const TX_CLASS tx_class,
    int reduced_tx_set_used) {
  const int txb_skip_ctx = txb_ctx->txb_skip_ctx;

  const int eob_multi_size = txsize_log2_minus4[tx_size];
  const LV_MAP_EOB_COST *const eob_costs =
      &x->coeff_costs.eob_costs[eob_multi_size][plane_type];
  int cost = coeff_costs->txb_skip_cost[txb_skip_ctx][0];

  cost += get_tx_type_cost(x, xd, plane, tx_size, tx_type, reduced_tx_set_used);

  cost += get_eob_cost(eob, eob_costs, coeff_costs, tx_class);

  cost += av1_cost_coeffs_txb_estimate(x, plane, block, tx_size, tx_type);
  return cost;
}

int av1_cost_coeffs_txb(const MACROBLOCK *x, const int plane, const int block,
                        const TX_SIZE tx_size, const TX_TYPE tx_type,
                        const TXB_CTX *const txb_ctx, int reduced_tx_set_used) {
  const struct macroblock_plane *p = &x->plane[plane];
  const int eob = p->eobs[block];
  const TX_SIZE txs_ctx = get_txsize_entropy_ctx(tx_size);
  const PLANE_TYPE plane_type = get_plane_type(plane);
  const LV_MAP_COEFF_COST *const coeff_costs =
      &x->coeff_costs.coeff_costs[txs_ctx][plane_type];
  if (eob == 0) {
    return coeff_costs->txb_skip_cost[txb_ctx->txb_skip_ctx][1];
  }

  const MACROBLOCKD *const xd = &x->e_mbd;
  const TX_CLASS tx_class = tx_type_to_class[tx_type];

  return warehouse_efficients_txb(x, plane, block, tx_size, txb_ctx, p, eob,
                                  plane_type, coeff_costs, xd, tx_type,
                                  tx_class, reduced_tx_set_used);
}

int av1_cost_coeffs_txb_laplacian(const MACROBLOCK *x, const int plane,
                                  const int block, const TX_SIZE tx_size,
                                  const TX_TYPE tx_type,
                                  const TXB_CTX *const txb_ctx,
                                  const int reduced_tx_set_used,
                                  const int adjust_eob) {
  const struct macroblock_plane *p = &x->plane[plane];
  int eob = p->eobs[block];

  if (adjust_eob) {
    const SCAN_ORDER *scan_order = get_scan(tx_size, tx_type);
    const int16_t *scan = scan_order->scan;
    tran_low_t *tcoeff = p->coeff + BLOCK_OFFSET(block);
    tran_low_t *qcoeff = p->qcoeff + BLOCK_OFFSET(block);
    tran_low_t *dqcoeff = p->dqcoeff + BLOCK_OFFSET(block);
    update_coeff_eob_fast(&eob, av1_get_tx_scale(tx_size), p->dequant_QTX, scan,
                          tcoeff, qcoeff, dqcoeff);
    p->eobs[block] = eob;
  }

  const TX_SIZE txs_ctx = get_txsize_entropy_ctx(tx_size);
  const PLANE_TYPE plane_type = get_plane_type(plane);
  const LV_MAP_COEFF_COST *const coeff_costs =
      &x->coeff_costs.coeff_costs[txs_ctx][plane_type];
  if (eob == 0) {
    return coeff_costs->txb_skip_cost[txb_ctx->txb_skip_ctx][1];
  }

  const MACROBLOCKD *const xd = &x->e_mbd;
  const TX_CLASS tx_class = tx_type_to_class[tx_type];

  return warehouse_efficients_txb_laplacian(
      x, plane, block, tx_size, txb_ctx, eob, plane_type, coeff_costs, xd,
      tx_type, tx_class, reduced_tx_set_used);
}
