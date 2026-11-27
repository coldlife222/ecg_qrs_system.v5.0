/*
 * ecg_eval.c — 评价指标与错误样本分析实现
 *
 * QRS检测评价采用容忍窗口匹配（±150ms），
 * 计算 Precision / Recall / F1 等标准检测评价指标。
 * 额外统计分类准确率（正常/心动过速/心动过缓/节律不齐）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ecg_eval.h"

/* -------- ground truth 加载 -------- */

int load_ground_truth(const char *filename, int *gt_idx, int max_gt) {
    FILE *fp = fopen(filename, "r");
    if (!fp) return -1;
    char line[64];
    int count = 0;
    int header_passed = 0;
    while (fgets(line, sizeof(line), fp) && count < max_gt) {
        if (line[0] == '#') continue;
        if (!header_passed) { header_passed = 1; continue; } /* 跳过 "peak_index" 表头 */
        int idx;
        if (sscanf(line, "%d", &idx) == 1) {
            gt_idx[count++] = idx;
        }
    }
    fclose(fp);
    return count;
}

/* -------- 容忍窗口匹配 -------- */

void eval_qrs_detection(const ECGRecord *rec, const int *gt_idx, int n_gt,
                         double tolerance_ms, EvalMetrics *metrics) {
    int tol_samp = (int)(tolerance_ms * rec->sample_rate / 1000.0);
    
    /* 标记数组（避免一个gt匹配多个pred） */
    char *gt_matched   = (char *)calloc(n_gt, 1);
    char *pred_matched = (char *)calloc(rec->n_peaks, 1);
    if (!gt_matched || !pred_matched) {
        free(gt_matched); free(pred_matched);
        return;
    }

    int tp = 0;
    for (int pi = 0; pi < rec->n_peaks; pi++) {
        int p = rec->peak_idx[pi];
        /* 找最近的未匹配gt峰 */
        int best_gi = -1;
        int best_dist = tol_samp + 1;
        for (int gi = 0; gi < n_gt; gi++) {
            if (gt_matched[gi]) continue;
            int dist = abs(p - gt_idx[gi]);
            if (dist <= tol_samp && dist < best_dist) {
                best_dist = dist;
                best_gi = gi;
            }
        }
        if (best_gi >= 0) {
            tp++;
            gt_matched[best_gi]   = 1;
            pred_matched[pi]      = 1;
        }
    }

    metrics->tp = tp;
    metrics->fp = rec->n_peaks - tp;
    metrics->fn = n_gt - tp;

    free(gt_matched);
    free(pred_matched);
}

/* -------- 错误样本输出 -------- */

void eval_print_errors(const ECGRecord *rec, const int *gt_idx, int n_gt,
                        double tolerance_ms, FILE *log_fp) {
    int tol_samp = (int)(tolerance_ms * rec->sample_rate / 1000.0);
    char *gt_matched   = (char *)calloc(n_gt, 1);
    char *pred_matched = (char *)calloc(rec->n_peaks, 1);
    if (!gt_matched || !pred_matched) {
        free(gt_matched); free(pred_matched);
        return;
    }

    for (int pi = 0; pi < rec->n_peaks; pi++) {
        int p = rec->peak_idx[pi];
        int best_gi = -1, best_dist = tol_samp + 1;
        for (int gi = 0; gi < n_gt; gi++) {
            if (gt_matched[gi]) continue;
            int dist = abs(p - gt_idx[gi]);
            if (dist <= tol_samp && dist < best_dist) {
                best_dist = dist; best_gi = gi;
            }
        }
        if (best_gi >= 0) {
            gt_matched[best_gi] = 1;
            pred_matched[pi] = 1;
        }
    }

    /* 假正例（多余检测） */
    fprintf(log_fp, "  [误检 FP(假正例)]: ");
    int fp_cnt = 0;
    for (int pi = 0; pi < rec->n_peaks; pi++) {
        if (!pred_matched[pi]) {
            fprintf(log_fp, "%d(%.3fs) ", rec->peak_idx[pi],
                    (double)rec->peak_idx[pi] / rec->sample_rate);
            fp_cnt++;
        }
    }
    if (fp_cnt == 0) fprintf(log_fp, "none");
    fprintf(log_fp, "\n");

    /* 假负例（漏检） */
    fprintf(log_fp, "  [漏检 FN(假负例)]: ");
    int fn_cnt = 0;
    for (int gi = 0; gi < n_gt; gi++) {
        if (!gt_matched[gi]) {
            fprintf(log_fp, "%d(%.3fs) ", gt_idx[gi],
                    (double)gt_idx[gi] / rec->sample_rate);
            fn_cnt++;
        }
    }
    if (fn_cnt == 0) fprintf(log_fp, "none");
    fprintf(log_fp, "\n");

    free(gt_matched);
    free(pred_matched);
}

/* -------- 累积指标 -------- */

void eval_accumulate(EvalMetrics *total, const EvalMetrics *single, int pred_correct) {
    total->tp += single->tp;
    total->fp += single->fp;
    total->fn += single->fn;
    total->total_records++;
    if (pred_correct) total->correct_class++;
}

/* -------- 最终汇总计算 -------- */

void eval_finalize(EvalMetrics *m) {
    int tp = m->tp, fp = m->fp, fn = m->fn;
    m->precision = (tp + fp > 0) ? (double)tp / (tp + fp) : 0.0;
    m->recall    = (tp + fn > 0) ? (double)tp / (tp + fn) : 0.0;
    double p = m->precision, r = m->recall;
    m->f1       = (p + r > 1e-9) ? 2.0 * p * r / (p + r) : 0.0;
    m->class_accuracy = (m->total_records > 0)
                        ? (double)m->correct_class / m->total_records : 0.0;
}

/* -------- 打印汇总 -------- */

void eval_print_summary(const EvalMetrics *m, FILE *fp) {
    fprintf(fp, "========================================\n");
    fprintf(fp, "  QRS Detection Evaluation (QRS检测评估)\n");
    fprintf(fp, "----------------------------------------\n");
    fprintf(fp, "  True Pos   TP(真正例):  %d\n", m->tp);
    fprintf(fp, "  False Pos  FP(假正例):  %d\n", m->fp);
    fprintf(fp, "  False Neg  FN(假负例):  %d\n", m->fn);
    fprintf(fp, "  Precision  P(精确率):   %.4f  (%.1f%%)\n", m->precision, m->precision * 100);
    fprintf(fp, "  Recall     R(召回率):   %.4f  (%.1f%%)\n", m->recall,    m->recall    * 100);
    fprintf(fp, "  F1 Score   (F1分数):     %.4f\n",             m->f1);
    fprintf(fp, "----------------------------------------\n");
    fprintf(fp, "  Total Records (总记录):  %d\n", m->total_records);
    fprintf(fp, "  Correct Class (正确分类): %d\n", m->correct_class);
    fprintf(fp, "  Class Accuracy (分类准确率): %.4f  (%.1f%%)\n",
            m->class_accuracy, m->class_accuracy * 100);
    fprintf(fp, "========================================\n");
}
