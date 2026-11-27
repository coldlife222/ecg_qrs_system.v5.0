/*
 * ecg_filter.h — 信号滤波与增强模块接口
 */

#ifndef ECG_FILTER_H
#define ECG_FILTER_H

#include "ecg_types.h"

/**
 * 滑动平均低通滤波
 * @param in    输入信号
 * @param out   输出信号
 * @param n     信号长度
 * @param win   窗口宽度(奇数)
 */
void filter_moving_average(const double *in, double *out, int n, int win);

/**
 * 一阶差分（梯度）计算信号斜率
 * @param in    输入信号
 * @param out   输出差分序列
 * @param n     信号长度
 */
void filter_diff1(const double *in, double *out, int n);

/**
 * 二阶差分
 */
void filter_diff2(const double *in, double *out, int n);

/**
 * 平方增强：突出高幅度区域（全部正值，放大R波能量）
 * @param in    输入信号
 * @param out   输出信号
 * @param n     信号长度
 */
void filter_square(const double *in, double *out, int n);

/**
 * 综合预处理流水线：
 *   raw -> 去直流 -> 移动平均 -> 差分 -> 平方 -> 积分窗口 -> enhanced
 * @param rec   ECG记录（原地修改 filtered 和 enhanced 字段）
 */
void filter_preprocess(ECGRecord *rec);

/**
 * 信号归一化到 [0, 1]
 */
void filter_normalize(double *sig, int n, double *out_min, double *out_max);

#endif /* ECG_FILTER_H */
