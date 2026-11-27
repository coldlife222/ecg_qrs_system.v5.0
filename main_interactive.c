/*
 * main_interactive.c — ECG QRS 检测交互版 v5.1
 *
 * 用法：
 *   交互模式: 直接运行，按提示操作
 *   管道模式: Get-Content data.txt | ecg_interactive.exe
 *
 * 构建命令（静态链接）：
 *   gcc -static -std=c99 -Wall -O3 -Iinclude -o ecg_interactive.exe ^
 *       main_interactive.c ^
 *       src/ecg_filter.c src/ecg_detector.c src/ecg_trainer.c ^
 *       src/ecg_inference.c src/ecg_eval.c src/ecg_io.c -lm
 *
 * 或: make interactive
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ANSI 颜色宏（Windows 10+ cmd/Windows Terminal 支持） */
#define COLOR_RESET   "\x1b[0m"
#define COLOR_RED     "\x1b[31m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_CYAN    "\x1b[36m"
#define COLOR_WHITE   "\x1b[37m"
#define COLOR_BOLD    "\x1b[1m"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define isatty _isatty
#define fileno _fileno

/* 启用 UTF-8 编码 + ANSI 转义，解决中文乱码 */
static void enable_ansi(void) {
    SetConsoleOutputCP(CP_UTF8);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}
#else
#include <unistd.h>
#define enable_ansi()  /* 空操作 */
#endif

#include "include/ecg_types.h"
#include "include/ecg_filter.h"
#include "include/ecg_detector.h"
#include "include/ecg_inference.h"
#include "include/ecg_trainer.h"

/* 最大输入样本数 */
#define MAX_INPUT_SAMPLES 10000

/* 函数声明 */
static int   read_input_data(double *data, int max_count, int is_pipe);
static void  display_results(ECGRecord *rec);
static void  print_title(const char *title);
static void  print_divider(void);
static void  analyze_and_display(ECGRecord *rec, double *input_data,
                                int sample_count, int sample_rate,
                                int session_num, int is_pipe);

/* 模型参数（优先从output/model_params.txt加载） */
static ModelParams model_params;

static void try_load_model(void) {
    if (load_model_params(&model_params, "output/model_params.txt") == 0) {
        if (!model_params.is_trained) {
            model_params.trained_hr_mean = 72.0;
            model_params.trained_hr_std = 8.0;
            model_params.trained_rr_cv_mean = 0.05;
            model_params.is_trained = 1;
        }
        printf(COLOR_GREEN "  [模型] 已加载训练参数 (HR均值=%.1f bpm)\n" COLOR_RESET,
               model_params.trained_hr_mean);
    } else {
        memset(&model_params, 0, sizeof(model_params));
        model_params.trained_hr_mean = 72.0;
        model_params.trained_hr_std = 8.0;
        model_params.trained_rr_cv_mean = 0.05;
        model_params.spki_alpha = 0.125;
        model_params.npki_alpha = 0.125;
        model_params.is_trained = 1;
        printf(COLOR_YELLOW "  [模型] 未找到训练参数，使用默认值\n" COLOR_RESET);
    }
}

/* 保存用户输入数据到独立用户数据集（不混入训练集） */
static void save_to_user_dataset(const double *data, int n, int sample_rate,
                                  const char *label) {
    int next_id = 0;
    FILE *check = fopen("data/user/manifest_user.csv", "r");
    if (check) {
        char line[256];
        while (fgets(line, sizeof(line), check)) {
            int id;
            if (sscanf(line, "user,user_int_%d.csv", &id) == 1 && id >= next_id)
                next_id = id + 1;
        }
        fclose(check);
    }
    char fname[64], fpath[512];
    snprintf(fname, sizeof(fname), "user_int_%03d.csv", next_id);
    snprintf(fpath, sizeof(fpath), "data/user/%s", fname);
#ifdef _WIN32
    system("mkdir data\\user 2>NUL");
#else
    system("mkdir -p data/user");
#endif
    FILE *fp = fopen(fpath, "w");
    if (!fp) return;
    fprintf(fp, "# sample_rate=%d,label=%s\n", sample_rate, label);
    fprintf(fp, "index,value\n");
    for (int i = 0; i < n; i++)
        fprintf(fp, "%d,%.6f\n", i, data[i]);
    fclose(fp);
    FILE *mf = fopen("data/user/manifest_user.csv", "a");
    if (mf) { fprintf(mf, "user,%s,%s\n", fname, label); fclose(mf); }
    printf(COLOR_GREEN "  [保存] %s → 已加入用户数据集 (标签=%s)\n" COLOR_RESET,
           fname, label);
}

/* 输出带颜色的节标题（左对齐，前缀 ==>） */
static void print_title(const char *title) {
    printf(COLOR_CYAN "==> " COLOR_BOLD "%s" COLOR_RESET "\n", title);
}

/* 输出带颜色的分隔线 */
static void print_divider(void) {
    printf(COLOR_CYAN "======================================================" COLOR_RESET "\n");
}

/* 显示分析结果（ANSI彩色美化） */
static void display_results(ECGRecord *rec) {
    /* 根据诊断结果选择颜色 */
    const char *diag_color = COLOR_GREEN;
    if (strcmp(rec->label_pred, "tachycardia") == 0 ||
        strcmp(rec->label_pred, "bradycardia") == 0 ||
        strcmp(rec->label_pred, "arrhythmia")  == 0) {
        diag_color = COLOR_RED;
    }

    printf("\n");
    print_divider();
    print_title("分析结果");
    print_divider();

    printf("  " COLOR_WHITE "R波数量    :" COLOR_RESET "  %-6d\n", rec->n_peaks);

    /* 仅1个R波时显示估算标记 */
    if (rec->n_peaks <= 1 && rec->n_rr == 0 && rec->hr_mean > 0.0) {
        printf("  " COLOR_WHITE "平均心率    :" COLOR_RESET "  %-6.1f  bpm  "
               COLOR_YELLOW "（估算值）" COLOR_RESET "\n", rec->hr_mean);
    } else {
        printf("  " COLOR_WHITE "平均心率    :" COLOR_RESET "  %-6.1f  bpm\n", rec->hr_mean);
    }
    printf("  " COLOR_WHITE "心率标准差  :" COLOR_RESET "  %-6.1f  bpm\n", rec->hr_std);
    printf("  " COLOR_WHITE "平均RR间期  :" COLOR_RESET "  %-6.1f  ms\n",  rec->rr_mean);
    printf("  " COLOR_WHITE "RR变异系数  :" COLOR_RESET "  %-6.3f\n",     rec->rr_cv);
    print_divider();

    /* 诊断结果（高亮显示） */
    printf("  " COLOR_WHITE "诊断结果    :" COLOR_RESET "  %s%s%s\n",
           diag_color, rec->label_pred, COLOR_RESET);

    /* 异常类型 */
    const char *anomaly_str  = "normal";
    const char *anomaly_color = COLOR_GREEN;
    switch (rec->global_type) {
        case ANOMALY_TACHY:      anomaly_str = "tachycardia"; anomaly_color = COLOR_RED;    break;
        case ANOMALY_BRADY:      anomaly_str = "bradycardia"; anomaly_color = COLOR_RED;    break;
        case ANOMALY_ARRHYTHMIA: anomaly_str = "arrhythmia";  anomaly_color = COLOR_YELLOW; break;
        case ANOMALY_PAUSE:      anomaly_str = "pause";       anomaly_color = COLOR_YELLOW; break;
        case ANOMALY_MIXED:      anomaly_str = "mixed";       anomaly_color = COLOR_YELLOW; break;
        default:                   anomaly_str = "normal";      anomaly_color = COLOR_GREEN;  break;
    }
    printf("  " COLOR_WHITE "异常类型    :" COLOR_RESET "  %s%s%s\n",
           anomaly_color, anomaly_str, COLOR_RESET);
    print_divider();

    /* RR间期详情 */
    if (rec->n_rr > 0) {
        printf("  " COLOR_CYAN "RR间期 （ms）:" COLOR_RESET "\n");
        int display_count = (rec->n_rr > 10) ? 10 : rec->n_rr;
        for (int i = 0; i < display_count; i++) {
            printf("      " COLOR_WHITE "[%2d]" COLOR_RESET "  %8.1f  ms\n",
                   i + 1, rec->rr_ms[i]);
        }
        if (rec->n_rr > 10) {
            printf("      ... （" COLOR_YELLOW "%d" COLOR_RESET " 个间期总计）\n", rec->n_rr);
        }
        print_divider();
    }

    /* 检测到的R波位置 */
    if (rec->n_peaks > 0) {
        printf("  " COLOR_CYAN "R波位置 （采样点）:" COLOR_RESET "\n");
        int display_count = (rec->n_peaks > 10) ? 10 : rec->n_peaks;
        for (int i = 0; i < display_count; i++) {
            printf("      " COLOR_WHITE "[%2d]" COLOR_RESET "  采样点 %-6d  幅值 %6.2f\n",
                   i + 1, rec->peak_idx[i], rec->peak_amp[i]);
        }
        if (rec->n_peaks > 10) {
            printf("      ... （" COLOR_YELLOW "%d" COLOR_RESET " 个R波总计）\n", rec->n_peaks);
        }
        print_divider();
    }
    printf("\n");
}

/* 读取用户输入的数值数据（优化版，带彩色提示） */
static int read_input_data(double *data, int max_count, int is_pipe) {
    if (!is_pipe) {
        printf("\n  " COLOR_CYAN "请在下方粘贴或输入数据值:" COLOR_RESET "\n");
        printf("  " COLOR_WHITE "（用空格或换行分隔）" COLOR_RESET "\n");
    }

    int   count  = 0;
    char  buffer[8192];

    while (count < max_count) {
        if (!is_pipe) printf(COLOR_GREEN "> " COLOR_RESET);
        if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
            break;   /* 文件结尾 */
        }

        /* 交互模式：检查退出/结束命令 */
        if (!is_pipe) {
            char *p = buffer;
            while (isspace((unsigned char)*p)) p++;
            if (strncmp(p, "quit", 4) == 0) {
                return -1;   /* 信号：用户想退出 */
            }
            if (strncmp(p, "end", 3) == 0 || strncmp(p, "q", 1) == 0) {
                break;
            }
        }

        /* 解析浮点数 */
        char   *start  = buffer;
        char   *endptr = NULL;
        while (count < max_count) {
            double val = strtod(start, &endptr);
            if (endptr == start) break;
            data[count++] = val;
            start = endptr;
        }

        if (count >= max_count) {
            printf(COLOR_YELLOW "  已达最大样本数 （%d）\n" COLOR_RESET, max_count);
            break;
        }
    }

    return count;
}

/* 执行分析并显示结果（每次循环迭代调用） */
static void analyze_and_display(ECGRecord *rec, double *input_data,
                                int sample_count, int sample_rate,
                                int session_num, int is_pipe) {
    /* 初始化记录 */
    memset(rec, 0, sizeof(ECGRecord));
    rec->sample_rate = sample_rate;
    rec->n_samples  = sample_count;
    memcpy(rec->raw, input_data, sample_count * sizeof(double));
    snprintf(rec->filename, sizeof(rec->filename),
             "interactive_input_%d", session_num);

    /* 预处理：滤波 */
    filter_preprocess(rec);
    if (!is_pipe) {
        printf("  " COLOR_GREEN "预处理完成:" COLOR_RESET
               "  %d 个采样点, %d Hz\n", rec->n_samples, rec->sample_rate);
    }

    /* QRS 检测 */
    model_params.signal_peak_est = 0;
    model_params.noise_peak_est  = 0;
    model_params.last_qrs_idx    = -1;
    detect_qrs_pantompkins(rec, &model_params);

    if (rec->n_peaks == 0) {
        printf("\n  " COLOR_RED "警告: 未检测到R波！" COLOR_RESET "\n");
        printf("  可能原因:\n");
        printf("    1. 数据格式不正确\n");
        printf("    2. 信号质量太差\n");
        printf("    3. R波幅值过小\n");
        return;
    }

    if (!is_pipe) {
        printf("  " COLOR_GREEN "QRS检测完成:" COLOR_RESET
               "  检测到 %d 个R波\n", rec->n_peaks);
    }

    /* 计算RR间期和统计量 */
    compute_rr_intervals(rec);
    compute_hr_statistics(rec);

    /* 异常分析 */
    analyze_global_rhythm(rec, &model_params);
    analyze_anomaly_segments(rec);

    /* 显示结果 */
    display_results(rec);
}

/* 主函数 — 基于循环的交互模式 */
int main(void) {
    enable_ansi();
    try_load_model();

    /* 检测管道/重定向输入（非终端） */
    int is_pipe = !isatty(fileno(stdin));

    ECGRecord *rec = (ECGRecord *)malloc(sizeof(ECGRecord));
    if (!rec) {
        printf("错误: 内存分配失败\n");
        return 1;
    }

    double input_data[MAX_INPUT_SAMPLES];
    int    sample_count;
    int    sample_rate = 360;
    int    session_num = 0;

    /* UI头部信息 */
    if (!is_pipe) {
        printf("\n");
        print_divider();
        print_title("ECG QRS 检测 & 心律失常分析  v5.0");
        printf(COLOR_GREEN "      Pan-Tompkins 算法 + 贝叶斯分类器" COLOR_RESET "\n");
        print_divider();
        printf("\n");
        printf(COLOR_YELLOW "  命令说明:" COLOR_RESET "\n");
        printf("    " COLOR_WHITE "输入采样率 + 数据" COLOR_RESET
               "  -->  分析心电图\n");
        printf("    " COLOR_WHITE "输入 'quit'" COLOR_RESET
               "            -->  退出程序\n");
        printf("    " COLOR_WHITE "输入 'end'" COLOR_RESET
               "             -->  结束数据输入\n");
        printf("\n");
    }

    /* 管道模式：单次运行 */
    if (is_pipe) {
        sample_rate  = 360;
        sample_count = 0;
        int remaining = read_input_data(input_data + sample_count,
                                       MAX_INPUT_SAMPLES - sample_count,
                                       is_pipe);
        sample_count += remaining;

        if (sample_count < 10) {
            printf("错误: 数据点太少（最少需要10个，当前只有 %d 个）\n",
                   sample_count);
            free(rec);
            return 1;
        }

        analyze_and_display(rec, input_data, sample_count,
                            sample_rate, ++session_num, is_pipe);
        save_to_user_dataset(input_data, sample_count, sample_rate,
                          anomaly_name(rec->global_type));
        free(rec);
        return 0;
    }

    /* 交互模式：循环 */
    while (1) {
        printf("\n");
        print_divider();
        char session_title[64];
        snprintf(session_title, sizeof(session_title),
                 COLOR_BOLD "  会话 #%d  " COLOR_RESET, ++session_num);
        printf("  %s\n", session_title);
        print_divider();
        printf("\n");

        /* 提示输入采样率 */
        printf("  " COLOR_WHITE "请输入采样率 （Hz）" COLOR_RESET
               "  [默认360, '" COLOR_YELLOW "quit" COLOR_RESET "' 退出]: ");
        fflush(stdout);

        char rate_buf[32];
        if (fgets(rate_buf, sizeof(rate_buf), stdin) == NULL) {
            printf("\n  " COLOR_GREEN "再见！" COLOR_RESET "\n");
            if (!is_pipe) {
                printf("  按任意键关闭窗口...\n");
                fflush(stdout);
#ifdef _WIN32
                system("pause > nul");
#endif
            }
            break;   /* EOF */
        }

        /* 检查是否退出 */
        char *p = rate_buf;
        while (isspace((unsigned char)*p)) p++;
        if (strncmp(p, "quit", 4) == 0) {
            printf("\n  " COLOR_GREEN "再见！" COLOR_RESET "\n");
            printf("  按任意键关闭窗口...\n");
            fflush(stdout);
#ifdef _WIN32
            system("pause > nul");
#endif
            break;
        }

        int rate_input = atoi(rate_buf);
        if (rate_input > 0 && rate_input <= 10000) {
            sample_rate = rate_input;
        } else {
            sample_rate = 360;
        }
        printf("  采样率: %d Hz\n", sample_rate);

        /* 提示输入ECG数据 */
        printf("\n  " COLOR_YELLOW "请输入ECG数据值" COLOR_RESET
               " （用空格或换行分隔）:\n");
        printf("  " COLOR_WHITE "  输入 '" COLOR_YELLOW "end" COLOR_WHITE "' 完成, "
               "'" COLOR_YELLOW "q" COLOR_WHITE "' 取消, "
               "'" COLOR_YELLOW "quit" COLOR_WHITE "' 退出\n" COLOR_RESET);

        sample_count = 0;
        int remaining = read_input_data(input_data + sample_count,
                                       MAX_INPUT_SAMPLES - sample_count,
                                       is_pipe);

        /* 用户输入期间键入quit */
        if (remaining == -1) {
            printf("\n  " COLOR_GREEN "再见！" COLOR_RESET "\n");
            printf("  按任意键关闭窗口...\n");
            fflush(stdout);
#ifdef _WIN32
            system("pause > nul");
#endif
            break;
        }

        sample_count += remaining;

        if (sample_count < 10) {
            printf("\n  " COLOR_RED "错误:" COLOR_RESET
                   " 数据点太少（最少需要10个，当前只有 %d 个）\n",
                   sample_count);
            printf("  请重试。\n\n");
            continue;
        }

        printf("\n  已读取 %d 个数据点, "
               COLOR_GREEN "开始分析..." COLOR_RESET "\n", sample_count);

        /* 分析与显示 */
        analyze_and_display(rec, input_data, sample_count,
                            sample_rate, session_num, is_pipe);
        save_to_user_dataset(input_data, sample_count, sample_rate,
                          anomaly_name(rec->global_type));

        printf("\n");
        print_divider();
        printf("  准备接收下一组数据。输入新的采样率，"
               "或 '" COLOR_YELLOW "quit" COLOR_RESET "' 退出。\n");
    }

    free(rec);
    return 0;
}
