/*
 * ecg_synth.h — ECG信号合成模块接口
 * 训练集：内存合成 → QRS检测 → 直接输出 TrainSum 数组
 * 测试集：合成后写入 CSV（评测需原始信号）
 */

#ifndef ECG_SYNTH_H
#define ECG_SYNTH_H

#include "ecg_types.h"

/* 合成器版本号，缓存失效判断依据 */
#define SYNTH_VERSION 20250623

/* 初始化合成器 PRNG */
void synth_init(unsigned int seed);

/* 生成训练集：内存中合成 8000 条信号 → QRS检测 → 提取TrainSum
 * sums:  输出 TrainSum 数组
 * max_n: 最大记录数
 * 返回:  实际生成的记录数 */
int synth_generate_trainingset(TrainSum *sums, int max_n);

/* 生成测试集 CSV（40 个文件：20 信号 + 20 GT）到指定目录
 * 同时生成 manifest_test.csv
 * 返回 0 成功，-1 失败 */
int synth_generate_testset(const char *data_dir);

#endif /* ECG_SYNTH_H */
