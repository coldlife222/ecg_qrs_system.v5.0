/*
 * ecg_trainer.h — 模型训练模块接口
 * 负责从训练集学习正常心率分布参数，支持K折交叉验证优化
 */
#ifndef ECG_TRAINER_H
#define ECG_TRAINER_H

#include "ecg_types.h"

/* 高斯概率密度函数（训练和推理共用） */
double gaussian_pdf(double x, double mu, double sigma);

/* 基础训练：从正常样本统计HR分布参数（接受轻量TrainSum，48字节/条） */
void train_model(const TrainSum *train_recs, int n_train, ModelParams *params);

/* K折交叉验证训练：自动搜索最优alpha参数 */
void train_model_cv(const TrainSum *train_recs, int n_train, ModelParams *params,
                     int k_folds);

/* 保存模型参数到文件 */
int save_model_params(const ModelParams *params, const char *filename);

/* 从文件加载模型参数 */
int load_model_params(ModelParams *params, const char *filename);

#endif
