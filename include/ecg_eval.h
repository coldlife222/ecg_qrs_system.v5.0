/*
 * ecg_eval.h — 评价指标与错误样本分析模块接口
 */

#ifndef ECG_EVAL_H
#define ECG_EVAL_H

#include <stdio.h>
#include "ecg_types.h"

/**
 * 从ground truth文件加载真实R波位置
 * @param filename  _gt.csv文件路径
 * @param gt_idx    输出数组
 * @param max_gt    最大容量
 * @return          真实R波数量，失败返回-1
 */
int load_ground_truth(const char *filename, int *gt_idx, int max_gt);

/**
 * 评价QRS检测性能（TP/FP/FN/Precision/Recall/F1）
 * 容忍窗口: ±tolerance_ms 毫秒内算匹配
 * @param rec           ECG记录（已检测）
 * @param gt_idx        真实R波位置数组
 * @param n_gt          真实R波数量
 * @param tolerance_ms  容忍窗口(ms)，默认150ms
 * @param metrics       输出评价指标
 */
void eval_qrs_detection(const ECGRecord *rec, const int *gt_idx, int n_gt,
                         double tolerance_ms, EvalMetrics *metrics);

/**
 * 输出错误样本分析（假正/假负位置列表）到日志文件
 */
void eval_print_errors(const ECGRecord *rec, const int *gt_idx, int n_gt,
                        double tolerance_ms, FILE *log_fp);

/**
 * 累积多记录评价指标
 */
void eval_accumulate(EvalMetrics *total, const EvalMetrics *single,
                     int pred_correct);

/**
 * 计算并填充最终汇总指标（Precision/Recall/F1/Accuracy）
 */
void eval_finalize(EvalMetrics *metrics);

/**
 * 打印评价指标摘要
 */
void eval_print_summary(const EvalMetrics *metrics, FILE *fp);

#endif /* ECG_EVAL_H */
