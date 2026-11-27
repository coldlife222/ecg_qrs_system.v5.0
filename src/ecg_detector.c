/*
 * ecg_detector.c — QRS波检测与RR间期分析实现
 *
 * 核心算法：Pan-Tompkins自适应双阈值检测
 *
 * AI原理体现（迭代参数优化）：
 *   - 信号峰估计 SPKI：每次检测到QRS峰时，按指数滑动平均更新
 *       SPKI = alpha * peak_i + (1-alpha) * SPKI
 *   - 噪声峰估计 NPKI：非QRS候选峰时更新
 *       NPKI = alpha * peak_i + (1-alpha) * NPKI
 *   - 检测阈值：THRESHOLD1 = NPKI + 0.25*(SPKI - NPKI)
 *              THRESHOLD2 = 0.5 * THRESHOLD1
 *   - 回溯搜索：若超过1.66*RR均值未检测到峰，降低阈值重新搜索
 *   这是一种在线学习/自适应估计，体现了迭代优化的AI基本原理。
 */

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "ecg_detector.h"

/* -------- 内部辅助函数 -------- */

static double sq(double x) { return x * x; }

static double array_mean_d(const double *a, int n) {
    if (n <= 0) return 0.0;
    double s = 0.0;
    for (int i = 0; i < n; i++) s += a[i];
    return s / n;
}

static double array_std_d(const double *a, int n, double mean) {
    if (n <= 1) return 0.0;
    double s = 0.0;
    for (int i = 0; i < n; i++) s += sq(a[i] - mean);
    return sqrt(s / (n - 1));
}

/* -------- 候选峰值检测 -------- */

/**
 * 局部极大值搜索（逐采样点扫描 + 后置不应期过滤）
 *
 * 改进说明（v2.2）：
 *   原算法用"跳过 min_dist"方案（i = peak + min_dist），在 min_dist 较大时
 *   会直接跳进 R 波下降沿，导致 sig[i] > sig[i-1] 条件失败，整条 R 波被漏检。
 *   新算法改为逐采样点扫描，检出后以 last_peak 记录上一个峰位置，
 *   当前峰若位于不应期内则取幅度更大者，否则独立记录。
 */
int detect_candidate_peaks(const double *sig, int n, int min_dist,
                            int *cand_idx, double *cand_amp, int max_cand) {
    int count = 0;
    int last_peak = -min_dist;  /* 上一个峰位置，初始化为足够远 */

    for (int i = 1; i < n - 1 && count < max_cand; i++) {
        /* 检查是否为局部极大值 */
        if (sig[i] <= sig[i - 1] || sig[i] < sig[i + 1]) continue;

        int peak = i;
        double peak_val = sig[i];

        /* 峰值精确定位（二次拟合） */
        if (peak > 0 && peak < n - 1) {
            double y1 = sig[peak - 1];
            double y2 = sig[peak];
            double y3 = sig[peak + 1];
            double denom = y1 - 2.0 * y2 + y3;
            if (fabs(denom) > 1e-12) {
                double delta = 0.5 * (y1 - y3) / denom;
                if (fabs(delta) < 1.0) {
                    peak_val = y2 - 0.25 * (y1 - y3) * delta;
                }
            }
        }

        /* 不应期过滤：若在 min_dist 内，保留幅度更大者 */
        if (peak - last_peak < min_dist) {
            if (count > 0 && peak_val > cand_amp[count - 1]) {
                cand_idx[count - 1] = peak;
                cand_amp[count - 1] = peak_val;
                last_peak = peak;
            }
            continue;
        }

        cand_idx[count]  = peak;
        cand_amp[count]  = peak_val;
        last_peak = peak;
        count++;
    }
    return count;
}

/* -------- Pan-Tompkins自适应阈值QRS检测 -------- */

void detect_qrs_pantompkins(ECGRecord *rec, ModelParams *params) {
    int n = rec->n_samples;
    const double *sig = rec->enhanced;
    int sr = rec->sample_rate;

    /* 最小RR间期（采样点） */
    int min_rr_samp = (int)(sr * QRS_MIN_RR_MS / 1000.0);

    /* 候选峰缓存 */
    static int   cand_idx[ECG_MAX_PEAKS * 3];
    static double cand_amp[ECG_MAX_PEAKS * 3];
    int n_cand = detect_candidate_peaks(sig, n, min_rr_samp / 2,
                                        cand_idx, cand_amp, ECG_MAX_PEAKS * 3);

    if (n_cand == 0) {
        rec->n_peaks = 0;
        return;
    }

    /* 初始化阈值（用前8个候选峰均值作为初始估计） */
    int init_n = n_cand < 8 ? n_cand : 8;
    double init_mean = 0.0;
    for (int i = 0; i < init_n; i++) init_mean += cand_amp[i];
    init_mean /= init_n;

    double SPKI  = params->signal_peak_est > 0 ? params->signal_peak_est : init_mean;
    double NPKI  = params->noise_peak_est  > 0 ? params->noise_peak_est  : init_mean * 0.1;
    double THRESHOLD1 = NPKI + 0.25 * (SPKI - NPKI);
    double THRESHOLD2 = 0.5 * THRESHOLD1;

    double alpha_spki = params->spki_alpha > 0 ? params->spki_alpha : 0.125;
    double alpha_npki = params->npki_alpha > 0 ? params->npki_alpha : 0.125;

    /* RR间期滑动平均（用于回溯检测） */
    double rr_avg1 = (double)(sr * 60.0 / 75.0);  /* 初始假设75bpm */
    double rr_avg2 = rr_avg1;
    double rr_buf[8] = {0};
    int rr_buf_cnt = 0;
    int rr_buf_pos = 0;

    int last_qrs = -1;
    rec->n_peaks = 0;

    /*
     * 双缓冲搜索流程：
     *   第一遍（常规阈值）：遍历候选峰，按THRESHOLD1判断
     *   若当前候选超过 1.66*RR_avg2 未出峰，用THRESHOLD2回溯搜索
     */
    for (int ci = 0; ci < n_cand && rec->n_peaks < ECG_MAX_PEAKS; ci++) {
        int   idx = cand_idx[ci];
        double amp = cand_amp[ci];

        /* 不应期检查 */
        if (last_qrs >= 0 && idx - last_qrs < min_rr_samp) {
            /* 仍在不应期内：若幅度比已检测峰更高，替换之（T波排除） */
            if (rec->n_peaks > 0 && amp > rec->peak_amp[rec->n_peaks - 1]) {
                rec->peak_idx[rec->n_peaks - 1] = idx;
                rec->peak_amp[rec->n_peaks - 1] = amp;
                last_qrs = idx;
            }
            /* 噪声峰更新 */
            NPKI = alpha_npki * amp + (1.0 - alpha_npki) * NPKI;
            THRESHOLD1 = NPKI + 0.25 * (SPKI - NPKI);
            THRESHOLD2 = 0.5 * THRESHOLD1;
            continue;
        }

        /* 长间歇检测：超过1.66*RR_avg2未出峰，激活二级阈值 */
        double active_thresh = THRESHOLD1;
        if (last_qrs >= 0 && (idx - last_qrs) > 1.66 * rr_avg2) {
            active_thresh = THRESHOLD2;
        }

        if (amp >= active_thresh) {
            /* 确认为QRS峰 */
            rec->peak_idx[rec->n_peaks] = idx;
            rec->peak_amp[rec->n_peaks] = amp;
            rec->n_peaks++;

            /* 更新信号峰估计（迭代优化核心） */
            SPKI = alpha_spki * amp + (1.0 - alpha_spki) * SPKI;

            /* 更新RR间期均值 */
            if (last_qrs >= 0) {
                double rr = idx - last_qrs;
                if (rr_buf_cnt < 8) {
                    rr_buf[rr_buf_pos] = rr;
                    rr_buf_pos = (rr_buf_pos + 1) % 8;
                    rr_buf_cnt++;
                } else {
                    rr_buf[rr_buf_pos] = rr;
                    rr_buf_pos = (rr_buf_pos + 1) % 8;
                }
                /* 计算RR均值 */
                double rr_sum = 0.0;
                for (int k = 0; k < rr_buf_cnt; k++) rr_sum += rr_buf[k];
                rr_avg1 = rr_sum / rr_buf_cnt;
                /* rr_avg2 只统计正常范围内的RR */
                if (rr >= 0.92 * rr_avg1 && rr <= 1.16 * rr_avg1) {
                    rr_avg2 = rr_avg1;
                }
            }
            last_qrs = idx;
        } else {
            /* 噪声峰：更新噪声估计 */
            NPKI = alpha_npki * amp + (1.0 - alpha_npki) * NPKI;
        }

        /* 动态更新阈值（体现在线学习） */
        THRESHOLD1 = NPKI + 0.25 * (SPKI - NPKI);
        THRESHOLD2 = 0.5 * THRESHOLD1;
    }

    /* 将最终阈值状态存回模型参数（供下一段信号使用） */
    params->signal_peak_est = SPKI;
    params->noise_peak_est  = NPKI;
    params->threshold1      = THRESHOLD1;
    params->threshold2      = THRESHOLD2;
    params->last_qrs_idx    = last_qrs;
    params->min_rr_samples  = min_rr_samp;
}

/* -------- RR间期计算 -------- */

void compute_rr_intervals(ECGRecord *rec) {
    rec->n_rr = 0;
    if (rec->sample_rate <= 0) return;
    for (int i = 1; i < rec->n_peaks; i++) {
        double rr_ms = (double)(rec->peak_idx[i] - rec->peak_idx[i - 1])
                       * 1000.0 / rec->sample_rate;
        rec->rr_ms[rec->n_rr++] = rr_ms;
    }
}

/* -------- 统计指标计算 -------- */

void compute_hr_statistics(ECGRecord *rec) {
    if (rec->n_rr <= 0) {
        rec->hr_mean = 0.0;
        rec->hr_std  = 0.0;
        rec->rr_mean = 0.0;
        rec->rr_cv   = 0.0;
        return;
    }

    /* RR间期均值 */
    rec->rr_mean = array_mean_d(rec->rr_ms, rec->n_rr);
    double rr_std = array_std_d(rec->rr_ms, rec->n_rr, rec->rr_mean);
    rec->rr_cv = (rec->rr_mean > 1e-9) ? rr_std / rec->rr_mean : 0.0;

    /* 心率均值 */
    rec->hr_mean = (rec->rr_mean > 1e-9) ? 60000.0 / rec->rr_mean : 0.0;

    /* 心率标准差（用误差传播近似：sd_hr ≈ sd_rr * hr^2 / 60000） */
    rec->hr_std = rr_std * rec->hr_mean * rec->hr_mean / 60000.0;
}
