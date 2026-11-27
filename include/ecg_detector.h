/*
 * ecg_detector.h — QRS波检测与RR间期分析模块接口
 */

#ifndef ECG_DETECTOR_H
#define ECG_DETECTOR_H

#include "ecg_types.h"

/**
 * 候选峰值检测（局部极大值搜索）
 * @param sig       信号
 * @param n         信号长度
 * @param min_dist  峰间最小间距(采样点)
 * @param cand_idx  候选峰值索引输出数组
 * @param cand_amp  候选峰值幅度输出数组
 * @param max_cand  最大候选数量
 * @return          候选峰值数量
 */
int detect_candidate_peaks(const double *sig, int n, int min_dist,
                            int *cand_idx, double *cand_amp, int max_cand);

/**
 * Pan-Tompkins自适应阈值QRS识别
 * 核心AI原理：双阈值迭代更新（信号峰/噪声峰加权滑动平均）
 * @param rec       ECG记录（读取 enhanced，写入 peak_idx/peak_amp/n_peaks）
 * @param params    模型参数（阈值会被动态更新）
 */
void detect_qrs_pantompkins(ECGRecord *rec, ModelParams *params);

/**
 * 计算RR间期序列（ms）
 * @param rec   ECG记录（读取 peak_idx，写入 rr_ms/n_rr）
 */
void compute_rr_intervals(ECGRecord *rec);

/**
 * 计算统计指标：心率均值/标准差/变异系数
 * @param rec   ECG记录（写入 hr_mean/hr_std/rr_mean/rr_cv）
 */
void compute_hr_statistics(ECGRecord *rec);

#endif /* ECG_DETECTOR_H */
