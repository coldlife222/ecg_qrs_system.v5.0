/*
 * import_user.c — 导入用户投放的txt/csv文件，转为独立用户数据集
 *
 * 扫描 data/user/ 下的 .txt/.csv 文件，根据文件名自动判定标签，
 * 转换为CSV格式 → 保持在 data/user/ → 写入独立清单 manifest_user.csv
 * 用户数据与训练集完全隔离，互不污染
 *
 * 标签判定规则（文件名包含即匹配）：
 *   "tachy" → tachycardia
 *   "brady" → bradycardia
 *   "arr"   → arrhythmia
 *   其他     → normal
 *
 * 编译:
 *   gcc -std=c99 -Wall -O3 -o import_user.exe src/import_user.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define MAX_SAMPLES 50000
#define MAX_FILES   200

/* ---- 从文件名判定标签 ---- */
static const char *detect_label(const char *fname) {
    char lower[256];
    int i;
    for (i = 0; fname[i] && i < 254; i++)
        lower[i] = (char)tolower((unsigned char)fname[i]);
    lower[i] = '\0';

    if (strstr(lower, "tachy"))  return "tachycardia";
    if (strstr(lower, "brady"))  return "bradycardia";
    if (strstr(lower, "arr"))    return "arrhythmia";
    return "normal";
}

/* ---- 读取txt（每行一个浮点数） ---- */
static int read_txt(const char *path, double *buf, int max_n) {
    FILE *fp = fopen(path, "r");
    if (!fp) { fprintf(stderr, "  [错误] 无法打开: %s\n", path); return -1; }
    char line[128];
    int n = 0;
    while (n < max_n && fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#') continue;
        buf[n++] = atof(p);
    }
    fclose(fp);
    return n;
}

/* ---- 写入CSV ---- */
static int write_csv(const char *path, const double *buf, int n,
                      const char *label) {
    FILE *fp = fopen(path, "w");
    if (!fp) { fprintf(stderr, "  [错误] 无法写入: %s\n", path); return -1; }
    fprintf(fp, "# sample_rate=360,label=%s\n", label);
    fprintf(fp, "index,value\n");
    for (int i = 0; i < n; i++)
        fprintf(fp, "%d,%.6f\n", i, buf[i]);
    fclose(fp);
    return 0;
}

/* ---- Main ---- */
int main(void) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    printf("\n");
    printf("=============================================\n");
    printf("  用户数据导入工具\n");
    printf("=============================================\n");
    printf("  扫描目录: data/user/\n");
    printf("  输出清单: data/user/manifest_user.csv\n");
    printf("  注：用户数据独立于训练集，互不污染\n\n");

    /* 扫描 data/user/ 下的 .txt 和 .csv 文件 */
    char cmd[512];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd),
        "(dir /b \"data\\user\\*.txt\" 2>NUL & dir /b \"data\\user\\*.csv\" 2>NUL) > data\\user\\_filelist.tmp");
#else
    snprintf(cmd, sizeof(cmd), "ls data/user/*.txt data/user/*.csv 2>/dev/null > data/user/_filelist.tmp");
#endif
    system(cmd);

    FILE *flist = fopen("data/user/_filelist.tmp", "r");
    if (!flist) {
        printf("  data/user/ 中没有 .txt 或 .csv 文件，无需导入。\n");
#ifdef _WIN32
        printf("  按任意键关闭...\n");
        system("pause > nul");
#endif
        printf("\n");
        return 0;
    }

    /* ===== 关键改动：写入 data/user/manifest_user.csv 而非 data/train/manifest_train.csv ===== */
    FILE *mfp = fopen("data/user/manifest_user.csv", "w");
    if (!mfp) {
        fprintf(stderr, "[错误] 无法创建 data/user/manifest_user.csv\n");
        fclose(flist);
        return 1;
    }
    fprintf(mfp, "# split,filename,label\n");

    double *buf = (double *)malloc(MAX_SAMPLES * sizeof(double));
    if (!buf) { fprintf(stderr, "[错误] 内存不足\n"); fclose(flist); fclose(mfp); return 1; }

    char fname[256];
    int  imported = 0;

    /* 找到当前最大编号 */
    int next_id = 0;
    {
        FILE *check = fopen("data/user/manifest_user.csv", "r");
        if (check) {
            char line[256];
            while (fgets(line, sizeof(line), check)) {
                int id;
                if (sscanf(line, "user,user_%d.csv", &id) == 1 && id >= next_id)
                    next_id = id + 1;
            }
            fclose(check);
        }
    }

    while (fgets(fname, sizeof(fname), flist)) {
        /* 去除换行 */
        size_t len = strlen(fname);
        while (len > 0 && (fname[len-1] == '\n' || fname[len-1] == '\r'))
            fname[--len] = '\0';
        if (len == 0) continue;

        /* 跳过非数据文件 */
        if (strstr(fname, "README") || strstr(fname, "readme")) continue;
        char *ext = strrchr(fname, '.');
        if (!ext || (strcmp(ext, ".txt") != 0 && strcmp(ext, ".csv") != 0)) continue;

        /* 判定标签（优先从文件名，CSV则从头部覆盖） */
        char label_buf[32];
        snprintf(label_buf, sizeof(label_buf), "%s", detect_label(fname));
        const char *label = label_buf;

        char src_path[512];
        snprintf(src_path, sizeof(src_path), "data/user/%s", fname);

        /* 检测是否已是格式化CSV */
        int is_csv = 0;
        {
            FILE *test = fopen(src_path, "r");
            if (test) {
                char hdr[128];
                if (fgets(hdr, sizeof(hdr), test) && strstr(hdr, "# sample_rate=")) {
                    is_csv = 1;
                    sscanf(hdr, "# sample_rate=%*d,label=%31s", label_buf);
                }
                fclose(test);
            }
        }

        /* 对于已格式化CSV，直接复用原文件路径；对于txt，转换后写入data/user/ */
        char dst_name[128];
        snprintf(dst_name, sizeof(dst_name), "user_%03d.csv", next_id);

        if (is_csv) {
            /* CSV直接复用（不复制，用原始fname路径） */
            snprintf(dst_name, sizeof(dst_name), "%s", fname);
        } else {
            /* .txt 格式，读数值转换并写入 data/user/ */
            char dst_path[512];
            snprintf(dst_path, sizeof(dst_path), "data/user/%s", dst_name);
            int n = read_txt(src_path, buf, MAX_SAMPLES);
            if (n <= 0) continue;
            if (write_csv(dst_path, buf, n, label) != 0) continue;
        }

        /* ===== 追加到 user 清单（不再写入 train 清单） ===== */
        fprintf(mfp, "user,%s,%s\n", dst_name, label);

        printf("  [%2d] %-40s → %-25s 标签=%s\n",
               ++imported, fname, dst_name, label);
        next_id++;
    }

    fclose(flist);
    fclose(mfp);
    free(buf);

    /* 清理临时文件 */
    remove("data/user/_filelist.tmp");

    printf("\n  导入完成: %d 个文件已加入用户数据集\n", imported);
    printf("  清单文件: data/user/manifest_user.csv\n");
    printf("=============================================\n");
#ifdef _WIN32
    printf("  按任意键关闭...\n");
    system("pause > nul");
#endif
    printf("\n");
    return 0;
}
