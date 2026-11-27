/*
 * ecg_inference.c — 预测推理模块
 *
 * AI推理原理：
 * 1. 硬性规则打分：基于医学阈值（HR>100→过速+6, HR<60→过缓+6, CV>0.20→不齐+6等）
 * 2. 高斯概率软评分：利用训练学到的正常分布做贝叶斯似然估计
 * 3. CV评分仅在HR正常范围参与，避免压制过速/过缓判断
 * 4. 最终取6类得分最高者作为诊断，并输出完整得分向量到inference_scores
 */

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "ecg_inference.h"
#include "ecg_trainer.h"

#define WINDOW_BEATS 8
#define WINDOW_STEP  4

/* -------- 异常类型转字符串 -------- */
const char *anomaly_name(AnomalyType t) {
    switch (t) {
        case ANOMALY_NONE:       return "normal";
        case ANOMALY_TACHY:      return "tachycardia";
        case ANOMALY_BRADY:      return "bradycardia";
        case ANOMALY_ARRHYTHMIA: return "arrhythmia";
        case ANOMALY_PAUSE:      return "pause";
        case ANOMALY_MIXED:      return "arrhythmia";
        default:                 return "unknown";
    }
}

/* -------- 局部推理：对一段RR序列判断异常类型 -------- */
/*
 * 输入: RR间期序列(ms)、长度
 * 输出: 异常类型、局部心率、变异系数、6类得分向量
 *
 * 决策路径（显式，体现AI可解释性）：
 *   硬规则预筛 → 高斯概率软评分 → CV门控 → 混合异常判断 → 取最高分
 */
AnomalyType infer_segment(const double *rr_ms, int n_rr,
                           double *out_hr, double *out_rr_cv,
                           const ModelParams *params,
                           double *out_scores) {
    if (n_rr <= 0) {
        *out_hr = 0; *out_rr_cv = 0;
        for (int c = 0; c < 6; c++) out_scores[c] = 0;
        return ANOMALY_NONE;
    }

    /* 统计RR间期 */
    double rr_sum = 0.0, rr_sq = 0.0, rr_max = rr_ms[0];
    for (int i = 0; i < n_rr; i++) {
        rr_sum += rr_ms[i];
        rr_sq  += rr_ms[i] * rr_ms[i];
        if (rr_ms[i] > rr_max) rr_max = rr_ms[i];
    }
    double rr_mean = rr_sum / n_rr;
    double rr_var  = rr_sq / n_rr - rr_mean * rr_mean;
    double rr_std  = rr_var > 0 ? sqrt(rr_var) : 0.0;
    double rr_cv   = rr_mean > 1e-9 ? rr_std / rr_mean : 0.0;
    double hr      = rr_mean > 1e-9 ? 60000.0 / rr_mean : 0.0;

    *out_hr    = hr;
    *out_rr_cv = rr_cv;

    /* 6类得分向量 */
    double score[6] = {0};

    /* ---- 硬性规则（医学阈值加权）---- */
    if (hr > HR_TACHY_THRESHOLD)    score[ANOMALY_TACHY]       += 6.0;
    if (hr < HR_BRADY_THRESHOLD && hr > 0.0)
                                    score[ANOMALY_BRADY]       += 6.0;
    if (hr > 90.0 && hr <= HR_TACHY_THRESHOLD)
                                    score[ANOMALY_TACHY]       += 2.0;
    if (hr >= 50.0 && hr < HR_BRADY_THRESHOLD)
                                    score[ANOMALY_BRADY]       += 2.0;
    if (rr_cv > HR_ARRHYTHMIA_CV
        && hr >= HR_BRADY_THRESHOLD && hr <= HR_TACHY_THRESHOLD)
                                    score[ANOMALY_ARRHYTHMIA]  += 6.0;
    if (rr_cv > 0.28)
                                    score[ANOMALY_ARRHYTHMIA]  += 3.0;
    if (rr_cv > 0.18 && rr_cv <= HR_ARRHYTHMIA_CV)
                                    score[ANOMALY_ARRHYTHMIA]  += 2.0;
    if (rr_max > rr_mean * HR_PAUSE_RATIO && n_rr >= 2)
                                    score[ANOMALY_PAUSE]        += 2.0;

    /* 正常区间支持 */
    if (hr >= HR_BRADY_THRESHOLD && hr <= HR_TACHY_THRESHOLD
        && rr_cv <= HR_ARRHYTHMIA_CV)
        score[ANOMALY_NONE] += 3.0;

    /* ---- 高斯概率软评分（利用训练参数）---- */
    if (params->is_trained) {
        double normal_hr_mean = params->trained_hr_mean;
        double normal_hr_std  = params->trained_hr_std > 8.0 ? params->trained_hr_std : 10.0;
        double mu_normal = normal_hr_mean;
        double mu_tachy  = 120.0, mu_brady = 48.0;
        double sigma     = normal_hr_std;
        double sigma_ab  = 15.0;

        score[ANOMALY_NONE]  += gaussian_pdf(hr, mu_normal, sigma) * 8.0;
        score[ANOMALY_TACHY] += gaussian_pdf(hr, mu_tachy,  sigma_ab) * 8.0;
        score[ANOMALY_BRADY] += gaussian_pdf(hr, mu_brady,  sigma_ab) * 8.0;

        /* CV评分仅在HR正常范围内参与 */
        if (hr >= HR_BRADY_THRESHOLD && hr <= HR_TACHY_THRESHOLD) {
            double cv_normal_mean = params->trained_rr_cv_mean > 0 ? params->trained_rr_cv_mean : 0.10;
            score[ANOMALY_NONE]       += gaussian_pdf(rr_cv, cv_normal_mean, 0.08) * 3.0;
            score[ANOMALY_ARRHYTHMIA] += gaussian_pdf(rr_cv, 0.35, 0.10) * 3.0;
        }
    }

    /* 混合异常 */
    if (score[ANOMALY_TACHY] > 1.0 && score[ANOMALY_ARRHYTHMIA] > 1.0)
        score[ANOMALY_MIXED] = score[ANOMALY_TACHY] + score[ANOMALY_ARRHYTHMIA];
    if (score[ANOMALY_BRADY] > 1.0 && score[ANOMALY_ARRHYTHMIA] > 1.0)
        score[ANOMALY_MIXED] = fmax(score[ANOMALY_MIXED],
                                    score[ANOMALY_BRADY] + score[ANOMALY_ARRHYTHMIA]);

    /* 取最高分 */
    AnomalyType best = ANOMALY_NONE;
    double best_val = score[ANOMALY_NONE];
    for (int c = 1; c <= 5; c++) {
        if (score[c] > best_val) {
            best_val = score[c];
            best = (AnomalyType)c;
        }
    }

    /* 输出得分向量 */
    if (out_scores)
        memcpy(out_scores, score, 6 * sizeof(double));

    return best;
}

/* -------- 全局心律分析 -------- */
void analyze_global_rhythm(ECGRecord *rec, const ModelParams *params) {
    double hr, rr_cv;
    double scores[6] = {0};

    /* 单R波特殊处理 */
    if (rec->n_peaks == 1 && rec->n_rr == 0 && rec->sample_rate > 0) {
        double duration_sec = (double)rec->n_samples / rec->sample_rate;
        double est_hr = duration_sec > 0.0 ? (1.0 / duration_sec) * 60.0 : 0.0;
        if (est_hr > 0.0 && est_hr <= 50.0) {
            rec->global_type = ANOMALY_BRADY;
            snprintf(rec->label_pred, sizeof(rec->label_pred), "%s", "bradycardia(est)");
            rec->hr_mean = est_hr;
            rec->rr_mean = 60000.0 / est_hr;
            memset(rec->inference_scores, 0, sizeof(rec->inference_scores));
            rec->inference_scores[ANOMALY_BRADY] = 10.0;
            return;
        }
        rec->hr_mean = est_hr;
    }

    AnomalyType t = infer_segment(rec->rr_ms, rec->n_rr, &hr, &rr_cv, params, scores);

    if (hr > 0.0) rec->hr_mean = hr;
    rec->global_type = t;
    snprintf(rec->label_pred, sizeof(rec->label_pred), "%s", anomaly_name(t));

    /* 保存推理得分到记录 */
    memcpy(rec->inference_scores, scores, 6 * sizeof(double));
}

/* -------- 滑动窗口异常片段检测 -------- */
void analyze_anomaly_segments(ECGRecord *rec) {
    rec->n_anomaly_segments = 0;
    if (rec->n_rr < WINDOW_BEATS) return;

    ModelParams dummy;
    memset(&dummy, 0, sizeof(dummy));
    dummy.is_trained = 0;

    for (int i = 0; i + WINDOW_BEATS <= rec->n_rr; i += WINDOW_STEP) {
        const double *rr_win = rec->rr_ms + i;
        int win_n = (i + WINDOW_BEATS <= rec->n_rr) ? WINDOW_BEATS : rec->n_rr - i;

        double local_hr, local_cv, local_scores[6];
        AnomalyType t = infer_segment(rr_win, win_n, &local_hr, &local_cv, &dummy, local_scores);

        double rr_sum = 0.0;
        for (int k = 0; k < win_n; k++) rr_sum += rr_win[k];
        double rr_win_mean = win_n > 0 ? rr_sum / win_n : 0.0;

        if (t != ANOMALY_NONE && rec->n_anomaly_segments < ECG_MAX_SEGMENTS) {
            int peak_start = i + 1;
            int peak_end   = i + win_n;
            if (peak_end >= rec->n_peaks) peak_end = rec->n_peaks - 1;

            int samp_start = rec->peak_idx[peak_start];
            int samp_end   = rec->peak_idx[peak_end];

            /* 合并相邻同类型 */
            if (rec->n_anomaly_segments > 0) {
                AnomalySegment *prev = &rec->anomaly_segs[rec->n_anomaly_segments - 1];
                if (prev->type == t && samp_start <= prev->end_idx + rec->sample_rate) {
                    prev->end_idx = samp_end;
                    prev->rr_mean = (prev->rr_mean + rr_win_mean) * 0.5;
                    prev->rr_cv   = (prev->rr_cv   + local_cv) * 0.5;
                    prev->hr_bpm  = (prev->hr_bpm  + local_hr) * 0.5;
                    continue;
                }
            }

            AnomalySegment *seg = &rec->anomaly_segs[rec->n_anomaly_segments++];
            seg->start_idx = samp_start;
            seg->end_idx   = samp_end;
            seg->type      = t;
            seg->rr_mean   = rr_win_mean;
            seg->rr_cv     = local_cv;
            seg->hr_bpm    = local_hr;
        }
    }
}
