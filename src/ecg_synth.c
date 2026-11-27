/*
 * ecg_synth.c — ECG信号合成模块
 *
 * 训练数据：内存合成 → 直接QRS检测 → 提取TrainSum → 不落盘
 * 测试数据：合成后写CSV（评测需要原始信号+GT）
 *
 * 从 generate_trainingset.c 提取信号合成逻辑，删除 CSV 写入环节
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "ecg_synth.h"
#include "ecg_filter.h"
#include "ecg_detector.h"

/* ========== 常量 ========== */
#define PI           3.14159265358979323846
#define SAMPLE_RATE  360

/* QRS模板: 7采样点，R峰在index=3 */
#define QRS_LEN 7
static const double qrs_template[QRS_LEN] = {
    -0.02, -0.08, 0.0, 1.0, 0.15, -0.05, -0.01
};

/* ========== PRNG (与 generate_trainingset.c 一致) ========== */
static unsigned int rng_state = 0;

static unsigned int rng_rand(void) {
    rng_state = rng_state * 1103515245 + 12345;
    return (rng_state >> 16) & 0x7fff;
}

static double rng_uniform(double lo, double hi) {
    return lo + (hi - lo) * rng_rand() / 32767.0;
}

void synth_init(unsigned int seed) {
    rng_state = seed ? seed : (unsigned int)time(NULL);
}

/* ========== 计算R峰位置 ========== */
static void calc_rr_sequence(int *p, int n_peaks, const int *rr_seq, int n_rr) {
    p[0] = (int)rng_uniform(10, 30);
    for (int k = 1; k < n_peaks; k++)
        p[k] = p[k-1] + ((k-1 < n_rr) ? rr_seq[k-1] : rr_seq[n_rr-1]);
}

/* ========== 生成ECG信号 ========== */
static void generate_ecg_signal(double *sig, int n_samples,
                                 const int *p, int n_peaks, double noise) {
    int i, k;
    for (i = 0; i < n_samples; i++) sig[i] = 0.0;

    /* QRS波 */
    for (k = 0; k < n_peaks; k++) {
        int center = p[k];
        for (int t = -(QRS_LEN/2); t <= QRS_LEN/2; t++) {
            int idx = center + t;
            if (idx >= 0 && idx < n_samples)
                sig[idx] += qrs_template[t + QRS_LEN/2];
        }
    }

    /* T波：R峰后 80ms~200ms */
    for (k = 0; k < n_peaks; k++) {
        int ts = p[k] + (int)(SAMPLE_RATE * 0.08);
        int te = p[k] + (int)(SAMPLE_RATE * 0.20);
        if (te >= n_samples) te = n_samples - 1;
        int len = te - ts;
        if (len < 4) continue;
        for (i = 0; i < len; i++) {
            int idx = ts + i;
            if (idx < n_samples)
                sig[idx] += 0.22 * sin(PI * i / len);
        }
    }

    /* P波：R峰前 180ms~20ms */
    for (k = 0; k < n_peaks; k++) {
        int prev_end = (k > 0) ? p[k-1] + (int)(SAMPLE_RATE * 0.22) : 0;
        int pe = p[k] - (int)(SAMPLE_RATE * 0.02);
        int ps = p[k] - (int)(SAMPLE_RATE * 0.18);
        if (ps < prev_end) ps = prev_end;
        if (ps >= pe) continue;
        int len = pe - ps;
        if (len < 3) continue;
        for (i = 0; i < len; i++) {
            int idx = ps + i;
            if (idx < n_samples)
                sig[idx] += 0.10 * sin(PI * i / len);
        }
    }

    /* 加噪声 */
    for (i = 0; i < n_samples; i++)
        sig[i] += rng_uniform(-noise, noise);
}

/* ========== 裁剪超出边界的峰值 ========== */
static int trim_peaks(int *p, int n_peaks, int n_samples) {
    int margin = QRS_LEN / 2 + 2, valid = 0;
    for (int k = 0; k < n_peaks; k++)
        if (p[k] >= margin && p[k] < n_samples - margin)
            p[valid++] = p[k];
    return valid;
}

/* ========== CSV 写入（仅测试集使用）========== */
static int write_csv_signal(const char *path, const double *sig,
                             int n, int sr, const char *label) {
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "# sample_rate=%d,label=%s\nindex,value\n", sr, label);
    for (int i = 0; i < n; i++)
        fprintf(fp, "%d,%.6f\n", i, sig[i]);
    fclose(fp);
    return 0;
}

static int write_gt_csv(const char *path, const int *p, int n) {
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "peak_index\n");
    for (int i = 0; i < n; i++)
        fprintf(fp, "%d\n", p[i]);
    fclose(fp);
    return 0;
}

/* ========== 训练集生成：内存合成 + QRS检测 → TrainSum ========== */
int synth_generate_trainingset(TrainSum *sums, int max_n) {
    int p[200];
    double sig_buf[10000];   /* 1440 采样点足够 */
    int count = 0;

    /* 分配一个 ECGRecord 复用 */
    ECGRecord *rec = (ECGRecord *)calloc(1, sizeof(ECGRecord));
    if (!rec) return 0;

    /* 检测参数模板（每条样本重置关键字段） */
    ModelParams det_params;
    memset(&det_params, 0, sizeof(det_params));
    det_params.spki_alpha = 0.125;
    det_params.npki_alpha = 0.125;

    printf("  [合成] 生成训练数据中...\n");

    /* 4 类标签 × 各 2000 条 */
    struct { const char *label; int count; double hr_lo; double hr_hi; int arrhythmia; } specs[] = {
        {"normal",       2000, 60.0, 100.0, 0},
        {"tachycardia",  2000, 110.0, 150.0, 0},
        {"bradycardia",  2000, 35.0, 55.0, 0},
        {"arrhythmia",   2000, 60.0, 110.0, 1},
    };

    int n_samples = 1440;  /* 训练信号长度 */
    rec->sample_rate = SAMPLE_RATE;

    for (int class_i = 0; class_i < 4; class_i++) {
        for (int i = 0; i < specs[class_i].count && count < max_n; i++) {
            double hr = rng_uniform(specs[class_i].hr_lo, specs[class_i].hr_hi);
            int arrh = specs[class_i].arrhythmia;

            int rr = (int)(SAMPLE_RATE * 60.0 / hr);
            int n_beats = n_samples / rr;
            int min_beats = (arrh || hr >= 110.0) ? 4 : (hr <= 55.0) ? 2 : 3;
            if (n_beats < min_beats) n_beats = min_beats;
            if (n_beats > 199) n_beats = 199;

            /* 生成 RR 序列 */
            int rr_seq[200];
            if (arrh) {
                for (int b = 0; b < n_beats; b++) {
                    rr_seq[b] = (b % 2 == 0) ? (int)(rr * 1.35) : (int)(rr * 0.50);
                    if (rr_seq[b] < 150) rr_seq[b] = 150;
                }
            } else {
                int jit = (hr >= 110.0) ? 3 : 5;
                for (int b = 0; b < n_beats; b++)
                    rr_seq[b] = rr + (int)rng_uniform(-jit, jit);
            }

            calc_rr_sequence(p, n_beats + 1, rr_seq, n_beats);
            int n_valid = trim_peaks(p, n_beats + 1, n_samples);
            if (n_valid < 2) continue;

            /* 合成信号到缓冲区 */
            generate_ecg_signal(sig_buf, n_samples, p, n_valid,
                                rng_uniform(0.02, 0.06));

            /* 填充 ECGRecord */
            memset(rec, 0, sizeof(ECGRecord));
            rec->n_samples = n_samples;
            rec->sample_rate = SAMPLE_RATE;
            snprintf(rec->label_truth, sizeof(rec->label_truth),
                     "%s", specs[class_i].label);
            memcpy(rec->raw, sig_buf, n_samples * sizeof(double));

            /* 重置检测状态 */
            det_params.signal_peak_est = 0;
            det_params.noise_peak_est  = 0;
            det_params.last_qrs_idx    = -1;

            /* QRS 检测流水线 */
            filter_preprocess(rec);
            detect_qrs_pantompkins(rec, &det_params);
            compute_rr_intervals(rec);
            compute_hr_statistics(rec);

            /* 提取 TrainSum */
            snprintf(sums[count].label, sizeof(sums[count].label),
                     "%s", specs[class_i].label);
            sums[count].hr_mean = rec->hr_mean;
            sums[count].rr_cv   = rec->rr_cv;
            count++;

            if (count % 500 == 0)
                printf("  [合成] 已处理 %4d 条...\n", count);
        }
    }

    free(rec);
    return count;
}

/* ========== 测试集生成：写 CSV 到磁盘 ========== */
static void synth_batch_test(const char *dir, const char *prefix,
                              int n_samples, int dist_n, int dist_t,
                              int dist_b, int dist_a,
                              const char *manifest_path) {
    int p[200];
    double sig_buf[10000];
    char cmd[256];

#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "mkdir \"%s\" 2>NUL", dir);
#else
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", dir);
#endif
    system(cmd);

    FILE *mfp = fopen(manifest_path, "w");
    if (!mfp) return;
    fprintf(mfp, "# split,filename,label\n");

    int count = 0;
    struct { int dist; const char *label; double hr_lo; double hr_hi; int arrh; } specs[] = {
        {dist_n, "normal",       60.0, 100.0, 0},
        {dist_t, "tachycardia",  110.0, 150.0, 0},
        {dist_b, "bradycardia",  35.0, 55.0, 0},
        {dist_a, "arrhythmia",   60.0, 110.0, 1},
    };

    for (int t = 0; t < 4; t++) {
        for (int i = 0; i < specs[t].dist; i++) {
            double hr = rng_uniform(specs[t].hr_lo, specs[t].hr_hi);
            int arr = specs[t].arrh;

            int rr = (int)(SAMPLE_RATE * 60.0 / hr);
            int n_beats = n_samples / rr;
            int min_beats = (arr || hr >= 110.0) ? 4 : (hr <= 55.0) ? 2 : 3;
            if (n_beats < min_beats) n_beats = min_beats;
            if (n_beats > 199) n_beats = 199;

            int rr_seq[200];
            if (arr) {
                for (int b = 0; b < n_beats; b++) {
                    rr_seq[b] = (b % 2 == 0) ? (int)(rr * 1.35) : (int)(rr * 0.50);
                    if (rr_seq[b] < 150) rr_seq[b] = 150;
                }
            } else {
                int jit = (hr >= 110.0) ? 3 : 5;
                for (int b = 0; b < n_beats; b++)
                    rr_seq[b] = rr + (int)rng_uniform(-jit, jit);
            }

            calc_rr_sequence(p, n_beats + 1, rr_seq, n_beats);
            int n_valid = trim_peaks(p, n_beats + 1, n_samples);
            if (n_valid < 2) continue;

            generate_ecg_signal(sig_buf, n_samples, p, n_valid,
                                rng_uniform(0.02, 0.06));

            char fname[64], sp[256], gp[256];
            snprintf(fname, sizeof(fname), "%s_%03d.csv", prefix, count);
            snprintf(sp, sizeof(sp), "%s/%s", dir, fname);
            write_csv_signal(sp, sig_buf, n_samples, SAMPLE_RATE, specs[t].label);

            snprintf(fname, sizeof(fname), "%s_%03d_gt.csv", prefix, count);
            snprintf(gp, sizeof(gp), "%s/%s", dir, fname);
            write_gt_csv(gp, p, n_valid);

            fprintf(mfp, "test,%s_%03d.csv,%s\n", prefix, count, specs[t].label);
            count++;
        }
    }

    fclose(mfp);
    printf("  [测试集] 已生成 %d 个文件到 %s/\n", count * 2, dir);
}

int synth_generate_testset(const char *data_dir) {
    printf("  [合成] 生成测试样本 (20条, 720采样点)...\n");
    synth_batch_test(data_dir, "test", 720, 8, 4, 4, 4,
                     "data/test/manifest_test.csv");
    return 0;
}
