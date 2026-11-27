/*
 * ecg_io.c — 数据读写与报告生成实现
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <stdint.h>
#include "ecg_io.h"
#include "ecg_eval.h"

/* 快速浮点解析器：直接从缓冲区解析double，跳过开头空白字符 */
static double fast_atof(const char **p) {
    const char *s = *p;

    /* 跳过空白 */
    while (*s == ' ' || *s == '\t') s++;

    /* 符号处理 */
    double sign = 1.0;
    if (*s == '-') { sign = -1.0; s++; }
    else if (*s == '+') { s++; }

    /* 整数部分 */
    double val = 0.0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10.0 + (double)(*s - '0');
        s++;
    }

    /* 小数部分 */
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') {
            val += (double)(*s - '0') * frac;
            frac *= 0.1;
            s++;
        }
    }

    /* 指数部分（可选） */
    if (*s == 'e' || *s == 'E') {
        s++;
        int exp_sign = 1;
        if (*s == '-') { exp_sign = -1; s++; }
        else if (*s == '+') { s++; }
        int exp_val = 0;
        while (*s >= '0' && *s <= '9') {
            exp_val = exp_val * 10 + (*s - '0');
            s++;
        }
        double pow10 = 1.0;
        if (exp_sign > 0) {
            for (int i = 0; i < exp_val; i++) pow10 *= 10.0;
        } else {
            for (int i = 0; i < exp_val; i++) pow10 *= 0.1;
        }
        val *= pow10;
    }

    *p = s;
    return sign * val;
}

/* -------- CSV数据加载（批量fread + 内存解析） -------- */

int io_load_csv(const char *filename, ECGRecord *rec) {
    /* 静态缓冲区（多线程由调用方通过互斥保护） */
    static char buf[512 * 1024];
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "[错误] 无法打开文件: %s\n", filename);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize <= 0 || fsize >= (long)sizeof(buf)) {
        fprintf(stderr, "[错误] 文件大小异常: %s (%ld 字节)\n", filename, fsize);
        fclose(fp);
        return -1;
    }

    size_t bytes_read = fread(buf, 1, (size_t)fsize, fp);
    fclose(fp);

    if (bytes_read != (size_t)fsize) {
        fprintf(stderr, "[错误] 文件读取不完整: %s\n", filename);
        return -1;
    }
    buf[fsize] = '\0';

    /* 初始化记录 */
    snprintf(rec->filename, sizeof(rec->filename), "%s", filename);
    rec->sample_rate = ECG_SAMPLE_RATE;
    memset(rec->label_truth, 0, sizeof(rec->label_truth));

    /* 逐行解析 */
    const char *p = buf;
    const char *end = buf + fsize;
    int header_skipped = 0;
    int count = 0;

    while (p < end && count < ECG_MAX_SAMPLES) {
        /* 找到当前行结尾 */
        const char *line_end = p;
        while (line_end < end && *line_end != '\n' && *line_end != '\r') line_end++;

        /* 跳过注释行 */
        if (*p == '#') {
            char line[256];
            size_t len = (size_t)(line_end - p);
            if (len >= sizeof(line)) len = sizeof(line) - 1;
            memcpy(line, p, len);
            line[len] = '\0';

            int sr; char label[32];
            if (sscanf(line, "# sample_rate=%d,label=%31s", &sr, label) == 2) {
                rec->sample_rate = sr;
                snprintf(rec->label_truth, sizeof(rec->label_truth), "%s", label);
            }

            /* 跳到下一行 */
            p = line_end;
            while (p < end && (*p == '\n' || *p == '\r')) p++;
            continue;
        }

        /* 空行跳过 */
        if (p == line_end) {
            while (p < end && (*p == '\n' || *p == '\r')) p++;
            continue;
        }

        /* 跳过标题行 "index,value" */
        if (!header_skipped) {
            header_skipped = 1;
            p = line_end;
            while (p < end && (*p == '\n' || *p == '\r')) p++;
            continue;
        }

        /* 解析数据行：跳过第一个数字（索引），解析逗号后的浮点数 */
        const char *q = p;
        /* 跳过索引数字 */
        while (q < line_end && *q != ',') q++;
        if (q < line_end) {
            q++; /* 跳过逗号 */
            /* 用快速解析器解析浮点数 */
            double val = fast_atof(&q);
            rec->raw[count++] = val;
        }

        /* 跳到下一行 */
        p = line_end;
        while (p < end && (*p == '\n' || *p == '\r')) p++;
    }

    rec->n_samples = count;

    if (count == 0) {
        fprintf(stderr, "[错误] 未解析到有效数据: %s\n", filename);
        return -1;
    }
    return 0;
}

/* -------- manifest 加载 -------- */

int io_load_manifest(const char *manifest_file, const char *split,
                     ECGRecord *recs, int max_recs, const char *data_dir) {
    FILE *fp = fopen(manifest_file, "r");
    if (!fp) {
        fprintf(stderr, "[错误] 无法打开manifest: %s\n", manifest_file);
        return 0;
    }

    char line[256];
    int count = 0;

    while (fgets(line, sizeof(line), fp) && count < max_recs) {
        if (line[0] == '#') continue;
        char file_split[16], fname[64], label[32];
        if (sscanf(line, "%15[^,],%63[^,],%31s", file_split, fname, label) != 3) continue;
        if (split && strcmp(file_split, split) != 0) continue;

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", data_dir, fname);

        if (io_load_csv(full_path, &recs[count]) == 0) {
            /* label已由CSV元数据填充，若未填充则从manifest读取 */
            if (strlen(recs[count].label_truth) == 0) {
                snprintf(recs[count].label_truth, sizeof(recs[count].label_truth), "%s", label);
            }
            count++;
        }
    }
    fclose(fp);
    return count;
}

/* -------- 运行日志 -------- */

void io_log(FILE *log_fp, const char *fmt, ...) {
    if (!log_fp) return;
    time_t now = time(NULL);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(log_fp, "[%s] ", tbuf);
    va_list args;
    va_start(args, fmt);
    vfprintf(log_fp, fmt, args);
    va_end(args);
    fflush(log_fp);
}

/* -------- 单记录报告 -------- */

static const char *anomaly_full_name(AnomalyType t) {
    switch (t) {
        case ANOMALY_NONE:       return "正常窦性心律";
        case ANOMALY_TACHY:      return "心动过速";
        case ANOMALY_BRADY:      return "心动过缓";
        case ANOMALY_ARRHYTHMIA: return "节律不齐";
        case ANOMALY_PAUSE:      return "长间歇/停搏";
        case ANOMALY_MIXED:      return "混合异常";
        default:                 return "未知";
    }
}

void io_write_record_report(const ECGRecord *rec, const EvalMetrics *metrics, FILE *fp) {
    fprintf(fp, "\n┌─────────────────────────────────────────────────────────\n");
    fprintf(fp, "│ 文件: %s\n", rec->filename);
    fprintf(fp, "│ 采样率: %d Hz  |  采样点数: %d  |  时长: %.1f 秒\n",
            rec->sample_rate, rec->n_samples,
            (double)rec->n_samples / rec->sample_rate);
    fprintf(fp, "├─────────── QRS检测结果 ──────────────────────────────────\n");
    fprintf(fp, "│ 检测到R波: %d 个\n", rec->n_peaks);
    fprintf(fp, "│ 平均RR间期: %.1f ms  |  RR变异系数: %.4f\n",
            rec->rr_mean, rec->rr_cv);
    fprintf(fp, "│ 平均心率: %.1f bpm  |  心率标准差: %.1f bpm\n",
            rec->hr_mean, rec->hr_std);
    fprintf(fp, "├─────────── 异常分析结果 ─────────────────────────────────\n");
    fprintf(fp, "│ 真实标签: %-20s  预测结果: %s\n",
            rec->label_truth, rec->label_pred);
    fprintf(fp, "│ 全局节律类型: %s\n", anomaly_full_name(rec->global_type));
    fprintf(fp, "│ 异常片段数量: %d\n", rec->n_anomaly_segments);
    for (int i = 0; i < rec->n_anomaly_segments; i++) {
        const AnomalySegment *seg = &rec->anomaly_segs[i];
        fprintf(fp, "│   片段%d: [%d ~ %d] (%.2fs~%.2fs)  类型=%s  HR=%.1f bpm\n",
                i + 1,
                seg->start_idx, seg->end_idx,
                (double)seg->start_idx / rec->sample_rate,
                (double)seg->end_idx   / rec->sample_rate,
                anomaly_full_name(seg->type),
                seg->hr_bpm);
    }
    if (rec->n_anomaly_segments == 0) fprintf(fp, "│   无异常片段\n");
    fprintf(fp, "├─────────── 检测评价指标 ─────────────────────────────────\n");
    fprintf(fp, "│ TP=%d  FP=%d  FN=%d\n", metrics->tp, metrics->fp, metrics->fn);
    fprintf(fp, "│ Precision=%.4f  Recall=%.4f  F1=%.4f\n",
            metrics->precision, metrics->recall, metrics->f1);
    fprintf(fp, "└─────────────────────────────────────────────────────────\n");
}

/* -------- 汇总报告 -------- */

void io_write_summary_report(const ECGRecord *recs, int n_recs,
                              const EvalMetrics *metrics,
                              const ModelParams *params,
                              const char *filename) {
    FILE *fp = fopen(filename, "w");
    if (!fp) { perror("io_write_summary_report"); return; }

    time_t now = time(NULL);
    char tbuf[64];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));

    fprintf(fp, "=========================================================\n");
    fprintf(fp, "       ECG QRS波检测与心率异常识别系统 — 分析报告\n");
    fprintf(fp, "=========================================================\n");
    fprintf(fp, "生成时间: %s\n\n", tbuf);

    fprintf(fp, "【一、系统概述】\n");
    fprintf(fp, "  算法: Pan-Tompkins自适应双阈值QRS检测\n");
    fprintf(fp, "  AI原理: 贝叶斯概率推理 + 模糊规则 + 迭代参数优化\n");
    fprintf(fp, "  预处理: 去直流 → 滑动平均低通 → 差分 → 平方 → 积分窗口\n\n");

    fprintf(fp, "【二、模型参数（训练结果）】\n");
    if (params->is_trained) {
        fprintf(fp, "  正常HR均值:     %.2f bpm\n", params->trained_hr_mean);
        fprintf(fp, "  正常HR标准差:   %.2f bpm\n", params->trained_hr_std);
        fprintf(fp, "  正常RR变异系数: %.4f\n",     params->trained_rr_cv_mean);
        fprintf(fp, "  信号峰估计:     %.4f\n",     params->signal_peak_est);
        fprintf(fp, "  噪声峰估计:     %.4f\n",     params->noise_peak_est);
        fprintf(fp, "  SPKI更新系数:   %.3f\n",     params->spki_alpha);
        fprintf(fp, "  NPKI更新系数:   %.3f\n",     params->npki_alpha);
    } else {
        fprintf(fp, "  （未训练，使用默认参数）\n");
    }
    fprintf(fp, "\n");

    fprintf(fp, "【三、测试结果汇总】\n");
    fprintf(fp, "  测试记录数: %d\n", n_recs);
    fprintf(fp, "\n  %-30s %-12s %-12s %-8s\n",
            "文件", "真实标签", "预测标签", "是否正确");
    fprintf(fp, "  %s\n", "──────────────────────────────────────────────────────");
    for (int i = 0; i < n_recs; i++) {
        int correct = (strcmp(recs[i].label_truth, recs[i].label_pred) == 0);
        fprintf(fp, "  %-30s %-12s %-12s  %s\n",
                recs[i].filename,
                recs[i].label_truth,
                recs[i].label_pred,
                correct ? "✓" : "✗");
    }
    fprintf(fp, "\n");

    fprintf(fp, "【四、评价指标】\n");
    eval_print_summary(metrics, fp);
    fprintf(fp, "\n");

    fprintf(fp, "【五、异常片段位置汇总】\n");
    for (int i = 0; i < n_recs; i++) {
        if (recs[i].n_anomaly_segments > 0) {
            fprintf(fp, "  文件: %s\n", recs[i].filename);
            for (int j = 0; j < recs[i].n_anomaly_segments; j++) {
                const AnomalySegment *seg = &recs[i].anomaly_segs[j];
                fprintf(fp, "    [%d~%d] %.2fs~%.2fs  %s  HR=%.1f\n",
                        seg->start_idx, seg->end_idx,
                        (double)seg->start_idx / recs[i].sample_rate,
                        (double)seg->end_idx   / recs[i].sample_rate,
                        anomaly_full_name(seg->type),
                        seg->hr_bpm);
            }
        }
    }

    fprintf(fp, "\n=========================================================\n");
    fprintf(fp, "  本报告由C语言ECG分析系统自动生成\n");
    fprintf(fp, "=========================================================\n");
    fclose(fp);
    printf("[报告] 已保存: %s\n", filename);
}

/* -------- 异常片段CSV输出 -------- */

void io_write_anomaly_csv(const ECGRecord *rec, const char *out_dir) {
    /* 从filename提取基名 */
    const char *base = rec->filename;
    const char *p = strrchr(base, '/');
    if (!p) p = strrchr(base, '\\');
    if (p) base = p + 1;

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/anomaly_%s", out_dir, base);

    FILE *fp = fopen(outpath, "w");
    if (!fp) { perror("io_write_anomaly_csv"); return; }
    fprintf(fp, "# anomaly_segments for %s\n", rec->filename);
    fprintf(fp, "start_idx,end_idx,start_sec,end_sec,type,hr_bpm,rr_cv\n");
    for (int i = 0; i < rec->n_anomaly_segments; i++) {
        const AnomalySegment *seg = &rec->anomaly_segs[i];
        fprintf(fp, "%d,%d,%.4f,%.4f,%d,%.2f,%.4f\n",
                seg->start_idx, seg->end_idx,
                (double)seg->start_idx / rec->sample_rate,
                (double)seg->end_idx   / rec->sample_rate,
                (int)seg->type,
                seg->hr_bpm,
                seg->rr_cv);
    }
    fclose(fp);
}
