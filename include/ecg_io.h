/*
 * ecg_io.h — 数据读写与报告生成模块接口
 */

#ifndef ECG_IO_H
#define ECG_IO_H

#include "ecg_types.h"
#include <stdio.h>

/**
 * 从CSV文件读取ECG信号
 * 格式: 首行"# sample_rate=N,label=X"，后续"index,value"
 * @return 0成功，-1失败
 */
int io_load_csv(const char *filename, ECGRecord *rec);

/**
 * 从manifest.txt读取文件列表
 * @param manifest_file  manifest路径
 * @param split          "train"或"test"，NULL则全部读取
 * @param recs           输出记录数组
 * @param max_recs       最大记录数
 * @param data_dir       数据目录前缀
 * @return               实际读取记录数
 */
int io_load_manifest(const char *manifest_file, const char *split,
                     ECGRecord *recs, int max_recs, const char *data_dir);

/**
 * 生成心电分析文本报告（单条记录）
 * @param rec       ECG记录
 * @param metrics   该记录的评价指标
 * @param fp        输出文件指针
 */
void io_write_record_report(const ECGRecord *rec, const EvalMetrics *metrics, FILE *fp);

/**
 * 生成汇总报告（所有测试记录）
 * @param recs      测试记录数组
 * @param n_recs    记录数量
 * @param metrics   汇总评价指标
 * @param params    模型参数
 * @param filename  输出文件路径
 */
void io_write_summary_report(const ECGRecord *recs, int n_recs,
                              const EvalMetrics *metrics,
                              const ModelParams *params,
                              const char *filename);

/**
 * 写运行日志（追加模式）
 */
void io_log(FILE *log_fp, const char *fmt, ...);

/**
 * 将异常片段位置写入CSV文件
 */
void io_write_anomaly_csv(const ECGRecord *rec, const char *out_dir);

#endif /* ECG_IO_H */
