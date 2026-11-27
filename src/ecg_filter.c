/*
 * ecg_filter.c — 信号滤波与增强实现
 *
 * 预处理流水线：
 *   原始信号 → 去均值(去直流) → 滑动平均低通滤波 → 一阶差分 → 平方 → 积分窗口
 *
 * 该流水线基于Pan-Tompkins算法的预处理思路，
 * 在差分后通过平方放大高频QRS能量、压制P/T波成分。
 */

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "ecg_filter.h"

/* -------- 内部工具函数 -------- */

static double array_mean(const double *a, int n) {
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += a[i];
    return n > 0 ? sum / n : 0.0;
}

/* -------- 公开接口实现 -------- */

/**
 * 滑动平均低通滤波
 * 使用循环缓冲区，O(n)时间复杂度
 */
void filter_moving_average(const double *in, double *out, int n, int win) {
    if (win <= 0 || win > n) {
        memcpy(out, in, sizeof(double) * n);
        return;
    }
    int half = win / 2;
    double window_sum = 0.0;
    int count = 0;

    /* 初始化前半窗 */
    for (int i = 0; i <= half && i < n; i++) {
        window_sum += in[i];
        count++;
    }

    for (int i = 0; i < n; i++) {
        /* 扩展右侧 */
        if (i + half + 1 < n) {
            window_sum += in[i + half + 1];
            count++;
        }
        /* 收缩左侧 */
        if (i - half - 1 >= 0) {
            window_sum -= in[i - half - 1];
            count--;
        }
        out[i] = window_sum / count;
    }
}

/**
 * 一阶差分
 * out[i] = (in[i+1] - in[i-1]) / 2  (中心差分，两端用单侧)
 */
void filter_diff1(const double *in, double *out, int n) {
    if (n <= 1) return;   /* 至少2点才可差分 */
    out[0] = in[1] - in[0];
    for (int i = 1; i < n - 1; i++) {
        out[i] = (in[i + 1] - in[i - 1]) * 0.5;
    }
    out[n - 1] = in[n - 1] - in[n - 2];
}

/**
 * 二阶差分
 */
void filter_diff2(const double *in, double *out, int n) {
    double tmp[ECG_MAX_SAMPLES];
    if (n > ECG_MAX_SAMPLES) n = ECG_MAX_SAMPLES;
    filter_diff1(in, tmp, n);
    filter_diff1(tmp, out, n);
}

/**
 * 平方增强：out[i] = in[i]^2
 * 全部正值，放大R波能量
 */
void filter_square(const double *in, double *out, int n) {
    for (int i = 0; i < n; i++) {
        out[i] = in[i] * in[i];
    }
}

/**
 * 信号归一化到 [0, 1]
 */
void filter_normalize(double *sig, int n, double *out_min, double *out_max) {
    if (n <= 0) return;
    double vmin = sig[0], vmax = sig[0];
    for (int i = 1; i < n; i++) {
        if (sig[i] < vmin) vmin = sig[i];
        if (sig[i] > vmax) vmax = sig[i];
    }
    double range = vmax - vmin;
    if (range < 1e-9) range = 1e-9;
    for (int i = 0; i < n; i++) {
        sig[i] = (sig[i] - vmin) / range;
    }
    if (out_min) *out_min = vmin;
    if (out_max) *out_max = vmax;
}

/**
 * 综合预处理流水线
 * 步骤：
 *   1. 去直流（减去全局均值）
 *   2. 首段暖机屏蔽（blanking）：前 BLANK_SAMPLES 点置零，消除边界伪影
 *   3. 滑动平均低通滤波（FILTER_MA_WINDOW点）
 *   4. 一阶差分（计算斜率）
 *   5. 平方增强（放大R波能量）
 *   6. 积分窗口平滑（滑动平均，窗口=采样率/10 ≈ 36点@360Hz）
 *   7. 归一化到 [0, 1]
 */
void filter_preprocess(ECGRecord *rec) {
    int n = rec->n_samples;
    if (n <= 0) return;

    double tmp1[ECG_MAX_SAMPLES];
    double tmp2[ECG_MAX_SAMPLES];

    /* 步骤1: 去直流 */
    double mean = array_mean(rec->raw, n);
    for (int i = 0; i < n; i++) {
        tmp1[i] = rec->raw[i] - mean;
    }

    /* 步骤2: 首段暖机屏蔽（自适应 v2.2）
     *
     * Pan-Tompkins 标准做法屏蔽前 0.2 秒以消除边界伪影/启动毛刺，
     * 但若信号早期就存在真实的 R 波（如 bradycardia 短记录），
     * 刚性清零会吃掉首个 QRS 复合波 -> 漏检。
     *
     * 自适应策略：
     *   1. 先计算全局最大幅值（去直流后）
     *   2. 再计算前 blank_samples 段的局部最大幅值
     *   3. 若前段存在超过全局峰值 30% 的信号 -> 仅最小屏蔽（5点）
     *   4. 否则维持原始屏蔽长度
     */
    int blank_samples = rec->sample_rate / 5;  /* 0.2秒 */
    if (blank_samples > n / 4) blank_samples = n / 4;

    /* 全局和前段幅值检测 */
    double global_max_val = 0.0;
    double front_max_val  = 0.0;
    for (int i = 0; i < n; i++) {
        double absv = fabs(tmp1[i]);
        if (absv > global_max_val) global_max_val = absv;
        if (i < blank_samples && absv > front_max_val) front_max_val = absv;
    }

    /* 前段有强信号：只做最小屏蔽 */
    if (global_max_val > 1e-9 && front_max_val > 0.30 * global_max_val) {
        blank_samples = 5;  /* ~14ms @360Hz，仅消除首采样毛刺 */
    }

    for (int i = 0; i < blank_samples && i < n; i++) {
        tmp1[i] = 0.0;
    }

    /* 步骤3: 滑动平均低通滤波 → filtered */
    filter_moving_average(tmp1, rec->filtered, n, FILTER_MA_WINDOW);

    /* 步骤4: 一阶差分 */
    filter_diff1(rec->filtered, tmp1, n);

    /* 步骤5: 平方增强 */
    filter_square(tmp1, tmp2, n);

    /* 步骤6: 积分窗口（宽度约0.1秒） */
    int integ_win = rec->sample_rate / 10;
    if (integ_win < 3) integ_win = 3;
    filter_moving_average(tmp2, rec->enhanced, n, integ_win);

    /* 步骤7: 归一化 */
    filter_normalize(rec->enhanced, n, NULL, NULL);
}
