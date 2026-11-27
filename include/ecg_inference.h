/*
 * ecg_inference.h — 预测推理模块接口
 * 负责对待测ECG记录进行异常类型推断和滑动窗口片段检测
 */
#ifndef ECG_INFERENCE_H
#define ECG_INFERENCE_H

#include "ecg_types.h"

/* 异常类型转字符串 */
const char *anomaly_name(AnomalyType t);

/* 对一段RR序列进行局部推理（返回异常类型+得分） */
AnomalyType infer_segment(const double *rr_ms, int n_rr,
                           double *out_hr, double *out_rr_cv,
                           const ModelParams *params,
                           double *out_scores);

/* 全局心律分析（写入rec->global_type/label_pred/inference_scores） */
void analyze_global_rhythm(ECGRecord *rec, const ModelParams *params);

/* 滑动窗口异常片段检测 */
void analyze_anomaly_segments(ECGRecord *rec);

#endif
