/*
 * ecg_types.h — 全局类型定义与常量
 * ECG QRS检测与心率异常识别系统
 */

#ifndef ECG_TYPES_H
#define ECG_TYPES_H

#include <stddef.h>

/* ===== 系统常量 ===== */
#define ECG_MAX_SAMPLES     50000   /* 最大采样点数 */
#define ECG_MAX_PEAKS       2000    /* 最大R波数量 */
#define ECG_MAX_SEGMENTS    500     /* 最大异常片段数 */
#define ECG_MAX_FILENAME    256
#define ECG_SAMPLE_RATE     360     /* 默认采样率(Hz) */

/* 滤波参数 */
#define FILTER_MA_WINDOW    5       /* 滑动平均窗口(奇数) */
#define FILTER_DIFF_ORDER   2       /* 差分阶数 */

/* Pan-Tompkins阈值参数 */
#define QRS_MIN_RR_MS       300     /* 最小RR间期(ms) = 200bpm上限 */
#define QRS_MAX_RR_MS       2000    /* 最大RR间期(ms) = 30bpm下限 */
#define QRS_THRESHOLD_INIT  0.5     /* 初始峰值阈值(相对最大值) */

/* 心率异常阈值 */
#define HR_TACHY_THRESHOLD  100     /* 心动过速: >100 bpm */
#define HR_BRADY_THRESHOLD  55      /* 心动过缓: <55 bpm（含2bpm容差，避免合成数据边界误判） */
#define HR_ARRHYTHMIA_CV    0.20    /* 节律不齐: RR变异系数>20%（降低灵敏度） */
#define HR_PAUSE_RATIO      1.5     /* 停搏: RR间期>平均*1.5 */
/* 单R波时用首个R波位置推算心率的最大合理延迟(样本数)，超过则判断为心动过缓 */
#define BRADY_SINGLE_PEAK_RATIO 1.2 /* 首个R峰位置 / 期望RR_60bpm > 此值则倾向brady */

/* 异常类型枚举 */
typedef enum {
    ANOMALY_NONE        = 0,
    ANOMALY_TACHY       = 1,   /* 心动过速 */
    ANOMALY_BRADY       = 2,   /* 心动过缓 */
    ANOMALY_ARRHYTHMIA  = 3,   /* 节律不齐 */
    ANOMALY_PAUSE       = 4,   /* 长间歇/停搏 */
    ANOMALY_MIXED       = 5    /* 混合异常 */
} AnomalyType;

/* 异常片段结构 */
typedef struct {
    int    start_idx;          /* 起始采样点 */
    int    end_idx;            /* 终止采样点 */
    AnomalyType type;          /* 异常类型 */
    double rr_mean;            /* 该片段平均RR间期(ms) */
    double rr_cv;              /* 变异系数 */
    double hr_bpm;             /* 该片段平均心率 */
} AnomalySegment;

/* ECG记录结构 */
typedef struct {
    char   filename[ECG_MAX_FILENAME];
    int    sample_rate;
    int    n_samples;
    double raw[ECG_MAX_SAMPLES];       /* 原始信号 */
    double filtered[ECG_MAX_SAMPLES];  /* 滤波后 */
    double enhanced[ECG_MAX_SAMPLES];  /* 差分增强后 */
    
    int    n_peaks;
    int    peak_idx[ECG_MAX_PEAKS];    /* R波位置(采样点索引) */
    double peak_amp[ECG_MAX_PEAKS];    /* R波幅度 */
    
    int    n_rr;
    double rr_ms[ECG_MAX_PEAKS];       /* RR间期序列(ms) */
    
    double hr_mean;             /* 平均心率(bpm) */
    double hr_std;              /* 心率标准差 */
    double rr_mean;             /* 平均RR间期(ms) */
    double rr_cv;               /* 全局变异系数 */
    
    AnomalyType global_type;    /* 全局异常类型 */
    int    n_anomaly_segments;
    AnomalySegment anomaly_segs[ECG_MAX_SEGMENTS];
    
    char   label_truth[32];     /* 真实标签 */
    char   label_pred[32];      /* 预测标签 */
    double inference_scores[6]; /* AI推理得分(6类：正常/过速/过缓/不齐/停搏/混合) */
} ECGRecord;

/* 模型参数结构（自适应阈值参数，体现AI迭代优化） */
typedef struct {
    double signal_peak_est;     /* 信号峰值估计(滑动平均) */
    double noise_peak_est;      /* 噪声峰值估计(滑动平均) */
    double threshold1;          /* 检测阈值1 */
    double threshold2;          /* 检测阈值2（0.5*threshold1） */
    double spki_alpha;          /* 信号峰更新系数 */
    double npki_alpha;          /* 噪声峰更新系数 */
    int    last_qrs_idx;        /* 上一个QRS位置 */
    int    min_rr_samples;      /* 最小RR间期(采样点) */
    /* 训练集统计参数 */
    double trained_hr_mean;     /* 训练集平均心率 */
    double trained_hr_std;      /* 训练集心率标准差 */
    double trained_rr_cv_mean;  /* 训练集平均RR变异系数 */
    int    is_trained;          /* 是否已训练 */
} ModelParams;

/* 轻量训练摘要（48字节/条，仅含训练所需字段） */
typedef struct {
    char   label[32];
    double hr_mean;
    double rr_cv;
} TrainSum;

/* 评价指标结构 */
typedef struct {
    int    tp;          /* 真正例 */
    int    fp;          /* 假正例 */
    int    fn;          /* 假负例 */
    double precision;
    double recall;
    double f1;
    int    total_records;
    int    correct_class;       /* 分类正确数 */
    double class_accuracy;      /* 分类准确率 */
} EvalMetrics;

#endif /* ECG_TYPES_H */
