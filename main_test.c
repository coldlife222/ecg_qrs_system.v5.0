/*
 * main_test.c — 训练+评估流水线 v5.1（内建信号合成，零训练磁盘开销）
 *
 * 流程：
 *   1. 内存合成8000条训练信号 → QRS检测 → TrainSum → 缓存 _cache.bin(384KB)
 *   2. 全类别统计（normal/tachy/brady/arrhythmia HR/CV分布）
 *   3. 贝叶斯分类器训练（正常样本HR均值/标准差/RR_CV）
 *   4. 保存模型参数
 *   5. 自动生成测试集CSV（若不存在）→ 加载 → 逐条推理+GT评估
 *   6. 输出QRS评估指标 + 4×4混淆矩阵 + 各类别精确/召回/F1
 *   7. 加载用户数据（若存在），纯推理输出
 *
 * 构建（静态链接）：
 *   gcc -static -std=c99 -Wall -O3 -Iinclude -o ecg_test.exe ^
 *       main_test.c src/ecg_filter.c src/ecg_detector.c
 *       src/ecg_trainer.c src/ecg_inference.c
 *       src/ecg_eval.c src/ecg_io.c src/ecg_synth.c -lm
 *
 * 或: make test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <sys/stat.h>
#include "include/ecg_types.h"
#include "include/ecg_filter.h"
#include "include/ecg_detector.h"
#include "include/ecg_trainer.h"
#include "include/ecg_inference.h"
#include "include/ecg_eval.h"
#include "include/ecg_io.h"
#include "include/ecg_synth.h"

/* ANSI 颜色码 */
#define CLR_RESET   "\x1b[0m"
#define CLR_RED     "\x1b[31m"
#define CLR_GREEN   "\x1b[32m"
#define CLR_YELLOW  "\x1b[33m"
#define CLR_CYAN    "\x1b[36m"
#define CLR_WHITE   "\x1b[37m"
#define CLR_BOLD    "\x1b[1m"

#ifdef _WIN32
#include <windows.h>
/* 启用 ANSI 转义 + UTF-8 编码，解决中文乱码 */
static void enable_console(void) {
    SetConsoleOutputCP(CP_UTF8);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}
#else
static void enable_console(void) {}
#endif

#define TOLERANCE_MS      150.0
#define MAX_GT            200
/* 轻量报告条目（只存必要字段，不存完整ECGRecord） */
typedef struct {
    char filename[128];
    char truth[32];
    char pred[32];
    double hr_mean;
    int    n_peaks;
    int    n_anomaly_segments;
} ReportEntry;

/* ============ 辅助函数 ============ */

/* 输出分隔线 */
static void divider(void) {
    printf(CLR_CYAN "============================================================"
           CLR_RESET "\n");
}

/* 输出节标题 */
static void title_s(const char *s) {
    printf(CLR_BOLD CLR_CYAN "==> %s" CLR_RESET "\n", s);
}

/* 标签→混淆矩阵索引 */
static int label_index(const char *label) {
    if (strcmp(label, "normal")      == 0) return 0;
    if (strcmp(label, "tachycardia") == 0) return 1;
    if (strcmp(label, "bradycardia") == 0) return 2;
    if (strcmp(label, "arrhythmia")  == 0) return 3;
    return -1;
}

static const char *index_label(int idx) {
    const char *names[] = {"normal","tachycardia","bradycardia","arrhythmia"};
    return (idx >= 0 && idx < 4) ? names[idx] : "?";
}

/* 打印混淆矩阵 */
static void print_confusion(int cm[4][4]) {
    printf("\n  " CLR_BOLD "混淆矩阵 (4×4):" CLR_RESET "\n");
    printf("                    预测→\n");
    printf("  %-14s %6s %12s %12s %12s %6s\n",
           "真实↓", "normal", "tachy", "brady", "arrhy", "合计");
    printf("  %s\n", "──────────────────────────────────────────────────────────");
    int row_sum[4] = {0}, col_sum[4] = {0}, total_sum = 0;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            row_sum[r] += cm[r][c];
            col_sum[c] += cm[r][c];
        }
        total_sum += row_sum[r];
        printf("  %-14s %6d %12d %12d %12d %6d\n",
               index_label(r), cm[r][0], cm[r][1], cm[r][2], cm[r][3], row_sum[r]);
    }
    printf("  %s\n", "──────────────────────────────────────────────────────────");
    printf("  %-14s %6d %12d %12d %12d %6d\n",
           "合计", col_sum[0], col_sum[1], col_sum[2], col_sum[3], total_sum);

    /* 各类别指标 */
    printf("\n  " CLR_BOLD "各类别指标:" CLR_RESET "\n");
    printf("  %-14s %8s %8s %8s\n", "类别", "精确率", "召回率", "F1");
    printf("  %s\n", "─────────────────────────────────");
    for (int ci = 0; ci < 4; ci++) {
        int tp = cm[ci][ci];
        double prec = col_sum[ci] > 0 ? (double)tp / col_sum[ci] : 0.0;
        double rec  = row_sum[ci] > 0 ? (double)tp / row_sum[ci] : 0.0;
        double f1   = (prec + rec > 1e-9) ? 2 * prec * rec / (prec + rec) : 0.0;
        printf("  %-14s %7.1f%% %7.1f%% %7.3f\n",
               index_label(ci), prec * 100, rec * 100, f1);
    }
}

/* 写混淆矩阵到文件 */
static void fprint_confusion(FILE *fp, int cm[4][4]) {
    fprintf(fp, "\n【混淆矩阵 (4×4)】\n");
    fprintf(fp, "%-14s %6s %12s %12s %12s %6s\n",
            "真实↓", "normal", "tachy", "brady", "arrhy", "合计");
    fprintf(fp, "%s\n", "──────────────────────────────────────────────────────────");
    int row_sum[4] = {0}, col_sum[4] = {0}, total_sum = 0;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            row_sum[r] += cm[r][c];
            col_sum[c] += cm[r][c];
        }
        total_sum += row_sum[r];
        fprintf(fp, "%-14s %6d %12d %12d %12d %6d\n",
                index_label(r), cm[r][0], cm[r][1], cm[r][2], cm[r][3], row_sum[r]);
    }
    fprintf(fp, "%s\n", "──────────────────────────────────────────────────────────");
    fprintf(fp, "%-14s %6d %12d %12d %12d %6d\n",
            "合计", col_sum[0], col_sum[1], col_sum[2], col_sum[3], total_sum);

    fprintf(fp, "\n各类别指标:\n");
    fprintf(fp, "%-14s %8s %8s %8s\n", "类别", "精确率", "召回率", "F1");
    for (int ci = 0; ci < 4; ci++) {
        int tp = cm[ci][ci];
        double prec = col_sum[ci] > 0 ? (double)tp / col_sum[ci] : 0.0;
        double rec  = row_sum[ci] > 0 ? (double)tp / row_sum[ci] : 0.0;
        double f1   = (prec + rec > 1e-9) ? 2 * prec * rec / (prec + rec) : 0.0;
        fprintf(fp, "%-14s %7.1f%% %7.1f%% %7.3f\n",
                index_label(ci), prec * 100, rec * 100, f1);
    }
}

/* ============ 训练阶段 ============ */

/* 缓存结构（二进制格式） */
typedef struct {
    int  magic;     /* 魔数 0x45434753 ("ECGS") */
    int  n_records; /* 记录数 */
    int  version;   /* SYNTH_VERSION，版本变化自动失效 */
    /* 后面紧跟 n_records 个 TrainSum */
} CacheHeader;

/* 写入缓存 */
static void cache_write(const char *path, TrainSum *sums, int n, int version) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    CacheHeader h = { 0x45434753, n, version };
    fwrite(&h, sizeof(h), 1, fp);
    fwrite(sums, sizeof(TrainSum), n, fp);
    fclose(fp);
}

/* 读取缓存，返回记录数，失败返回-1 */
static int cache_read(const char *path, TrainSum *sums, int max_n, int version) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    CacheHeader h;
    if (fread(&h, sizeof(h), 1, fp) != 1 || h.magic != 0x45434753) { fclose(fp); return -1; }
    if (h.version != version || h.n_records > max_n) { fclose(fp); return -1; }
    int nr = (int)fread(sums, sizeof(TrainSum), h.n_records, fp);
    fclose(fp);
    return (nr == h.n_records) ? nr : -1;
}

/* 加载训练集：优先读缓存，首次运行内存合成+QRS检测 */
static ModelParams do_training(FILE *log_fp) {
    ModelParams params;
    memset(&params, 0, sizeof(params));
    params.spki_alpha = 0.125;
    params.npki_alpha = 0.125;
    int n_total = 0;

    io_log(log_fp, "训练阶段: 内建信号合成\n");

    TrainSum *sums = (TrainSum *)calloc(20000, sizeof(TrainSum));
    if (!sums) return params;

    /* 尝试读缓存 */
    int cached = cache_read("data/train/_cache.bin", sums, 20000, SYNTH_VERSION);
    if (cached > 0) {
        n_total = cached;
        printf("  " CLR_GREEN "缓存命中!" CLR_RESET " %d 条记录，跳过信号处理\n", n_total);
        io_log(log_fp, "缓存命中: %d 条\n", n_total);
    } else {
        /* 缓存失效，内存合成 + 直接QRS检测 */
        n_total = synth_generate_trainingset(sums, 20000);

        /* 写缓存 */
        cache_write("data/train/_cache.bin", sums, n_total, SYNTH_VERSION);
        printf("  " CLR_GREEN "已缓存" CLR_RESET " %d 条 (data/train/_cache.bin, %.0f KB)\n",
               n_total, (double)(n_total * sizeof(TrainSum)) / 1000.0);
    }

    printf("  " CLR_GREEN "合计:" CLR_RESET " %d 条训练记录\n", n_total);

    train_model(sums, n_total, &params);
    free(sums);
    return params;
}

/* ============ 测试阶段 ============ */

/* 执行评估：加载全部数据，用训练好的模型推理，评估并输出结果 */
static void do_testing(const char *manifest_path, const char *data_dir,
                        const ModelParams *params, FILE *log_fp) {
    /* 轻量级报告数组（每条仅100字节，520条=52KB） */
    ReportEntry reports[600];
    EvalMetrics per_sample[600];
    int        n_test = 0;

    EvalMetrics total;
    memset(&total, 0, sizeof(total));

    /* 混淆矩阵 */
    int confusion[4][4] = {0};

    /* 用户检测（变量提升至此，供报告生成复用） */
    int u_count = 0, u_idx = 0;

    /* 预分配一个工作记录 */
    ECGRecord *rec = (ECGRecord *)calloc(1, sizeof(ECGRecord));
    if (!rec) {
        fprintf(stderr, CLR_RED "[致命错误] 测试记录内存不足" CLR_RESET "\n");
        return;
    }

    FILE *mfp = fopen(manifest_path, "r");
    if (!mfp) {
        fprintf(stderr, CLR_RED "[致命错误] 无法打开 %s" CLR_RESET "\n", manifest_path);
        free(rec);
        return;
    }

    printf("\n");
    divider();
    title_s("系统测试集");
    divider();

    io_log(log_fp, "测试阶段: %s\n", manifest_path);

    char line[256];

    /* 第一遍：系统测试数据 */
    while (fgets(line, sizeof(line), mfp) && n_test < 600) {
        if (line[0] == '#') continue;

        char fname[128], label[32];
        if (sscanf(line, "%*[^,],%127[^,],%31s", fname, label) != 2)
            continue;

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", data_dir, fname);

        memset(rec, 0, sizeof(ECGRecord));
        if (io_load_csv(full_path, rec) != 0) {
            io_log(log_fp, "跳过测试（加载失败）: %s\n", fname);
            continue;
        }

        /* 对工作记录运行流水线 */
        ModelParams det_params = *params;
        det_params.signal_peak_est = 0;
        det_params.noise_peak_est  = 0;
        det_params.last_qrs_idx    = -1;

        filter_preprocess(rec);
        detect_qrs_pantompkins(rec, &det_params);
        compute_rr_intervals(rec);
        compute_hr_statistics(rec);
        analyze_global_rhythm(rec, params);

        /* 保存报告摘要 */
        ReportEntry *rp = &reports[n_test];
        snprintf(rp->filename, sizeof(rp->filename), "%s", fname);
        snprintf(rp->truth, sizeof(rp->truth), "%s", rec->label_truth);
        snprintf(rp->pred,  sizeof(rp->pred),  "%s", rec->label_pred);
        rp->hr_mean = rec->hr_mean;
        rp->n_peaks = rec->n_peaks;
        rp->n_anomaly_segments = rec->n_anomaly_segments;
        char gt_path[512], base[256];
        snprintf(base, sizeof(base), "%s", fname);
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        snprintf(gt_path, sizeof(gt_path), "%s/%s_gt.csv", data_dir, base);

        int gt_idx[MAX_GT];
        int n_gt = load_ground_truth(gt_path, gt_idx, MAX_GT);

        /* 评估指标 */
        EvalMetrics *m = &per_sample[n_test];
        memset(m, 0, sizeof(*m));
        if (n_gt > 0)
            eval_qrs_detection(rec, gt_idx, n_gt, TOLERANCE_MS, m);
        double p = (m->tp + m->fp > 0) ? (double)m->tp / (m->tp + m->fp) : 0.0;
        double r = (m->tp + m->fn > 0) ? (double)m->tp / (m->tp + m->fn) : 0.0;
        m->precision = p;
        m->recall    = r;
        m->f1        = (p + r > 1e-9) ? 2.0 * p * r / (p + r) : 0.0;

        int class_ok = (strcmp(rec->label_pred, rec->label_truth) == 0);
        eval_accumulate(&total, m, class_ok ? 1 : 0);

        /* 更新混淆矩阵 */
        {
            int ti = label_index(rec->label_truth);
            int pi = label_index(rec->label_pred);
            if (ti >= 0 && pi >= 0) confusion[ti][pi]++;
        }

        /* 逐样本输出 */
        const char *cok = class_ok ? CLR_GREEN "OK(正确)" CLR_RESET : CLR_RED "错误" CLR_RESET;
        printf("  " CLR_WHITE "[%2d]" CLR_RESET " %-35s  "
               "HR(心率)=%6.1f  peaks(R波)=%2d  "
               "真值: " CLR_YELLOW "%-12s" CLR_RESET
               " 预测: " CLR_YELLOW "%-12s" CLR_RESET
               "  |  TP=%-2d FP=%-2d FN=%-2d  "
               "P(精确率)=%.3f R(召回率)=%.3f F1=%.3f  "
               "[%s]\n"
               "                         推理得分: "
               "正%.1f 速%.1f 缓%.1f 不齐%.1f 停%.1f 混%.1f\n",
               n_test + 1, fname,
               rec->hr_mean, rec->n_peaks,
               rec->label_truth, rec->label_pred,
               m->tp, m->fp, m->fn,
               m->precision, m->recall, m->f1,
               cok,
               rec->inference_scores[0], rec->inference_scores[1],
               rec->inference_scores[2], rec->inference_scores[3],
               rec->inference_scores[4], rec->inference_scores[5]);

        io_log(log_fp, "测试[%d] %s: 真值=%s 预测=%s tp=%d fp=%d fn=%d f1=%.3f %s\n",
               n_test + 1, fname,
               rec->label_truth, rec->label_pred,
               m->tp, m->fp, m->fn, m->f1,
               class_ok ? "OK" : "错误");

        /* 分类错误或有漏检/误检时输出详细错误 */
        if (!class_ok || m->fp > 0 || m->fn > 0)
            eval_print_errors(rec, gt_idx, n_gt, TOLERANCE_MS, stdout);

        n_test++;
    }
    fclose(mfp);

    /* ---- 系统测试集汇总 ---- */
    eval_finalize(&total);
    divider();
    eval_print_summary(&total, stdout);

    int det_correct = total.tp;
    int det_total   = total.tp + total.fp + total.fn;
    double det_acc  = det_total > 0 ? 100.0 * det_correct / det_total : 0.0;
    printf("  QRS检测准确率: %.1f%%  (%d/%d)\n",
           det_acc, det_correct, det_total);

    /* 输出混淆矩阵 */
    print_confusion(confusion);
    printf("\n");

    /* ---- 用户数据检测（仅推理，不评估） ---- */
    {
        u_count = 0; u_idx = n_test;

        FILE *ufp = fopen("data/user/manifest_user.csv", "r");
        if (ufp) {
            divider();
            title_s(CLR_YELLOW "用户数据检测" CLR_RESET);
            divider();

            while (fgets(line, sizeof(line), ufp) && u_idx < 600) {
                if (line[0] == '#') continue;
                char fname[128];
                if (sscanf(line, "%*[^,],%127[^,],%*s", fname) != 1) continue;

                char full_path[512];
                snprintf(full_path, sizeof(full_path), "data/user/%s", fname);

                memset(rec, 0, sizeof(ECGRecord));
                if (io_load_csv(full_path, rec) != 0) {
                    fprintf(stderr, "  [跳过] 无法加载: %s\n", fname);
                    continue;
                }

                ModelParams det_params = *params;
                det_params.signal_peak_est = 0;
                det_params.noise_peak_est  = 0;
                det_params.last_qrs_idx    = -1;

                filter_preprocess(rec);
                detect_qrs_pantompkins(rec, &det_params);
                compute_rr_intervals(rec);
                compute_hr_statistics(rec);
                analyze_global_rhythm(rec, params);

                ReportEntry *rp = &reports[u_idx];
                snprintf(rp->filename, sizeof(rp->filename), "%s", fname);
                snprintf(rp->pred, sizeof(rp->pred), "%s", rec->label_pred);
                rp->hr_mean = rec->hr_mean;
                rp->n_peaks = rec->n_peaks;

                /* 纯检测输出：无GT对比，无评估指标 */
                printf("  " CLR_WHITE "[%2d]" CLR_RESET " %-35s  "
                       "HR=%.1f bpm  peaks=%d  预测: "
                       CLR_YELLOW "%-12s" CLR_RESET "\n"
                       "                         推理得分: "
                       "正%.1f 速%.1f 缓%.1f 不齐%.1f 停%.1f 混%.1f\n",
                       u_count + 1, fname, rec->hr_mean, rec->n_peaks,
                       rec->label_pred,
                       rec->inference_scores[0], rec->inference_scores[1],
                       rec->inference_scores[2], rec->inference_scores[3],
                       rec->inference_scores[4], rec->inference_scores[5]);

                u_count++; u_idx++;
            }
            fclose(ufp);
        }

        if (u_count == 0)
            printf("  (无用户数据)\n\n");
    }

    io_log(log_fp, "最终结果: P=%.4f R=%.4f F1=%.4f 分类准确率=%.4f 检测准确率=%.1f%%\n",
           total.precision, total.recall, total.f1,
           total.class_accuracy, det_acc);

    /* 输出轻量级汇总报告 */
    {
        FILE *rfp = fopen("output/summary_report.txt", "w");
        if (rfp) {
            time_t now = time(NULL);
            char tbuf[64];
            strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
            fprintf(rfp, "===== ECG QRS检测与心率异常识别系统 — 报告 =====\n");
            fprintf(rfp, "生成时间: %s\n\n", tbuf);
            fprintf(rfp, "算法: Pan-Tompkins + 贝叶斯分类器\n");
            fprintf(rfp, "模型: HR均值=%.2f HR标准差=%.2f RR_CV=%.4f\n\n",
                    params->trained_hr_mean, params->trained_hr_std,
                    params->trained_rr_cv_mean);
            fprintf(rfp, "%-35s %-12s %-12s %s\n", "文件", "真实标签", "预测标签", "正确");
            fprintf(rfp, "%s\n", "----------------------------------------------");
            for (int i = 0; i < n_test; i++) {
                int ok = (strcmp(reports[i].truth, reports[i].pred) == 0);
                fprintf(rfp, "%-35s %-12s %-12s %s\n",
                        reports[i].filename, reports[i].truth,
                        reports[i].pred, ok ? "Y" : "N");
            }
            fprintf(rfp, "\n");
            eval_print_summary(&total, rfp);
            fprint_confusion(rfp, confusion);

            /* ===== 追加用户检测结果到报告 ===== */
            if (u_count > 0) {
                fprintf(rfp, "\n%s\n", "----------------------------------------------");
                fprintf(rfp, "用户数据检测结果 (%d 条)\n", u_count);
                fprintf(rfp, "%-35s %-12s %8s %6s\n", "文件", "预测标签", "心率(bpm)", "R波数");
                fprintf(rfp, "%s\n", "----------------------------------------------");
                for (int i = n_test; i < u_idx; i++) {
                    fprintf(rfp, "%-35s %-12s %8.1f %6d\n",
                            reports[i].filename, reports[i].pred,
                            reports[i].hr_mean, reports[i].n_peaks);
                }
            }

            fclose(rfp);
            printf("[报告] 已保存: output/summary_report.txt\n");
        }
    }

    free(rec);
}

/* ============ 主函数 ============ */

int main(void) {
    enable_console();

    /* 初始化合成器 PRNG */
    synth_init((unsigned int)time(NULL));

#ifdef _WIN32
    system("mkdir output 2>NUL");
    system("mkdir data\\train 2>NUL");
    system("mkdir data\\test  2>NUL");
#else
    system("mkdir -p output data/train data/test");
#endif

    FILE *log_fp = fopen("output/run.log", "w");
    if (!log_fp) log_fp = stdout;
    io_log(log_fp, "=== ECG 测试流水线启动 v5.1 ===\n");

    printf("\n");
    divider();
    title_s("ECG QRS 检测 - 训练与评估流水线  v5.1");
    printf("  " CLR_GREEN "Pan-Tompkins + 贝叶斯分类器（内建信号合成）" CLR_RESET "\n");
    printf("  训练: 内存合成  |  测试: data/test/  |  用户: data/user/\n");
    printf("  容差: +/- %.0f ms （AAMI标准）\n", TOLERANCE_MS);
    divider();
    printf("\n");

    /* 检查是否需要生成测试集 */
    {
        struct stat st;
        if (stat("data/test/manifest_test.csv", &st) != 0) {
            title_s("自动生成测试样本");
            synth_generate_testset("data/test");
            printf("\n");
        }
    }

    /* 阶段1：训练 */
    divider();
    title_s("阶段1 - 合成训练数据 + 模型训练");
    divider();

    time_t t0 = time(NULL);
    ModelParams trained = do_training(log_fp);

    printf("\n");
    printf("  " CLR_BOLD "训练模型:" CLR_RESET "\n");
    printf("    正常HR均值 : " CLR_GREEN "%.2f" CLR_RESET " bpm\n",
           trained.trained_hr_mean);
    printf("    正常HR标准差: " CLR_GREEN "%.2f" CLR_RESET " bpm\n",
           trained.trained_hr_std);
    printf("    正常RR CV   : " CLR_GREEN "%.4f" CLR_RESET "\n",
           trained.trained_rr_cv_mean);

    save_model_params(&trained, "output/model_params.txt");
    io_log(log_fp, "模型参数已保存至 output/model_params.txt\n");

    time_t t1 = time(NULL);
    printf("\n  训练耗时: %lld 秒\n\n", (long long)(t1 - t0));

    /* 阶段2：测试 */
    printf("\n");
    divider();
    title_s("阶段2 - 测试集评估 (data/test/manifest_test.csv)");
    divider();
    printf("\n");
    do_testing("data/test/manifest_test.csv", "data/test", &trained, log_fp);

    time_t t2 = time(NULL);
    printf("  总耗时: %lld 秒\n", (long long)(t2 - t0));

    divider();
    printf("  " CLR_GREEN "输出文件:" CLR_RESET "\n");
    printf("    output/model_params.txt    - 训练好的模型参数\n");
    printf("    output/summary_report.txt  - 完整评估报告\n");
    printf("    output/run.log             - 带时间戳的运行日志\n");
    divider();
    printf("\n");

    io_log(log_fp, "=== ECG 测试流水线结束 ===\n");

    if (log_fp != stdout) fclose(log_fp);

#ifdef _WIN32
    printf("  按任意键关闭窗口...\n");
    fflush(stdout);
    system("pause > nul");
#endif

    return 0;
}
