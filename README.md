# ECG QRS 波检测与心率异常识别系统 v5.1

基于 Pan-Tompkins 算法的 ECG 信号 QRS 波检测系统，支持心率计算与心律失常分类，纯 C 语言实现，无任何第三方依赖。

## 快速开始

```
双击 ecg_test.exe（或 make run_test）
→ 首次运行：内存合成 8000 条信号 → QRS检测 → 训练 → 评测
→ 后续运行：缓存命中，秒启动
→ 输出：output/model_params.txt + output/summary_report.txt
```

## 功能

- **QRS 波检测**：自适应双阈值 Pan-Tompkins 算法，含二次拟合峰值精确定位
- **心率统计**：平均心率、心率标准差、RR 间期变异系数（RR_CV）
- **心律分类**：正常（normal）、心动过速（tachycardia）、心动过缓（bradycardia）、心律不齐（arrhythmia）、停搏（pause）、混合异常（mixed）
- **批量评测**：对照 Ground Truth 自动计算 Precision / Recall / F1 + 混淆矩阵，AAMI 标准（±150ms 容忍窗口）
- **全类别统计**：训练阶段自动输出 4 类心律的 HR/CV 分布表
- **异常片段定位**：8 拍滑动窗口检测，输出起止采样点索引与局部统计指标
- **用户数据导入**：独立 manifest 管理，不混入训练/测试集

## 项目结构

```
ecg_qrs_system.v5.1/
├── include/                    头文件（8 个）
│   ├── ecg_types.h             公共类型定义与常量
│   ├── ecg_filter.h            信号滤波与增强接口
│   ├── ecg_detector.h          QRS 检测与 RR 分析接口
│   ├── ecg_trainer.h           模型训练与参数持久化接口
│   ├── ecg_inference.h         预测推理与异常片段检测接口
│   ├── ecg_eval.h              评价指标与错误样本分析接口
│   ├── ecg_io.h                数据读写与报告生成接口
│   └── ecg_synth.h             信号合成（内建训练数据生成）
├── src/                        源文件（8 个）
│   ├── ecg_filter.c            滤波流水线（去直流 → MA → 差分 → 平方 → MWI → 归一化）
│   ├── ecg_detector.c          Pan-Tompkins 自适应双阈值 QRS 检测
│   ├── ecg_trainer.c           模型训练（统计学习 + 全类别分析 + K 折交叉验证）
│   ├── ecg_inference.c         贝叶斯 + 模糊规则异常分类（6 类评分向量）
│   ├── ecg_eval.c              评价指标（TP/FP/FN → P/R/F1）+ 错误样本分析
│   ├── ecg_io.c                文件读写、日志、报告生成
│   ├── ecg_synth.c             信号合成（内存生成 → 直接QRS检测 → TrainSum）
│   └── import_user.c           用户自定义数据导入工具
├── data/
│   ├── train/                  训练缓存（首次运行自动生成，~384 KB）
│   ├── test/                   测试集（首次运行自动生成 40 个 CSV）
│   └── user/                   用户自定义数据
├── output/                     运行产物
│   ├── model_params.txt        训练好的模型参数
│   ├── run.log                 带时间戳的运行日志
│   └── summary_report.txt      完整评估报告（含混淆矩阵）
├── main_interactive.c          交互式程序入口
├── main_test.c                 训练 + 测试一体化流水线入口（内建合成）
├── ecg_test.exe                训练+测试程序（一键完成）
├── ecg_interactive.exe         交互式程序
├── import_user.exe             用户数据导入工具
├── run_ecg.bat                 一键运行菜单
├── run_user.bat                用户数据导入 + 训练
└── Makefile                    构建脚本
```

## 使用指南

### 1. 编译（需 MinGW-w64 gcc）

```bash
# 一键编译所有程序
make all

# 或手动编译
gcc -static -std=c99 -Wall -O3 -Iinclude -o ecg_test.exe ^
    main_test.c ^
    src/ecg_filter.c src/ecg_detector.c src/ecg_trainer.c ^
    src/ecg_inference.c src/ecg_eval.c src/ecg_io.c src/ecg_synth.c -lm

gcc -static -std=c99 -Wall -O3 -Iinclude -o ecg_interactive.exe ^
    main_interactive.c ^
    src/ecg_filter.c src/ecg_detector.c src/ecg_trainer.c ^
    src/ecg_inference.c src/ecg_eval.c src/ecg_io.c -lm

gcc -static -std=c99 -Wall -O3 -Iinclude -o import_user.exe ^
    src/import_user.c -lm
```

### 2. 运行训练 + 测试

```
双击 ecg_test.exe（或 run_ecg.bat → 选 [1]）

流程：
  阶段1 → 内存合成 8000 条训练信号
        → QRS 检测 → TrainSum 缓存 → 全类别统计 → 模型训练
  阶段2 → 加载 20 条测试 CSV → 推理 → Ground Truth 对比
        → 输出 QRS 评估指标 + 混淆矩阵
  附加  → 加载用户数据（若存在）→ 纯推理输出
```

### 3. 交互式分析

```
双击 ecg_interactive.exe（或 run_ecg.bat → 选 [3]）

输入采样率（默认 360 Hz）
粘贴 ECG 电压序列，输入 "end" 结束
查看彩色诊断报告（含推理得分）

管道模式: Get-Content your_ecg_data.txt | ecg_interactive.exe
```

### 4. 用户数据

```
1) 将 CSV 放入 data/user/ 目录
   格式: 首行 "# sample_rate=360,label=normal"，后续 "index,value"

2) 双击 run_user.bat（导入 + 训练 + 检测）
   或双击 import_user.exe（仅导入）
```

### 5. 清理

```bash
make clean    # 删除 exe + 数据 + 输出
```

## AI 原理体现

| AI 原理 | 实现方式 | 模块 |
|---------|---------|------|
| **概率计算** | 高斯概率密度 `gaussian_pdf(x, μ, σ)`，计算心率属于正常/异常分布的似然 | `ecg_trainer.c` |
| **迭代优化** | SPKI/NPKI 指数滑动平均在线学习，每检测到 QRS 峰自动更新检测阈值 | `ecg_detector.c` |
| **损失函数 + 超参搜索** | K 折交叉验证以 HR 估计误差（MAD）为损失函数，搜索最优 α 参数 | `ecg_trainer.c` |
| **模糊规则** | 硬规则梯度加权 + 高斯概率软评分 + 混合异常判断 → 取最高分 | `ecg_inference.c` |
| **概率分布学习** | 8000 条训练样本全类别统计 μ_HR/σ_HR/μ_CV，贝叶斯先验 | `ecg_trainer.c` |
| **可解释推理** | 每条记录输出 `inference_scores[6]` 完整得分向量 + 混淆矩阵 | `ecg_inference.c` |

## 训练输出示例

```
[训练] 8000 条训练记录 → 全类别统计:
  类别            样本数   HR均值 HR标准差   HR范围     RR_CV
  normal           2000     79.3     11.7  [56,103]     0.047
  tachycardia      2000    128.3     11.5  [105,151]    0.065
  bradycardia      2000     44.3      5.7  [33,56]      0.031
  arrhythmia       2000     84.0     11.2  [62,106]     0.439

  贝叶斯先验 (正常类): μ=79.3 σ=11.7 CV=0.047
```

## 测试结果

20 个合成 ECG 样本（360 Hz），覆盖 4 类心律：

| 类别 | 样本数 | TP | FP | FN | F1 | 分类 |
|------|--------|----|----|----|-----|------|
| normal | 8 | 25 | 0 | 0 | 1.000 | 8/8 ✓ |
| tachycardia | 4 | 19 | 0 | 0 | 1.000 | 4/4 ✓ |
| bradycardia | 4 | 8 | 0 | 0 | 1.000 | 4/4 ✓ |
| arrhythmia | 4 | 12 | 0 | 0 | 1.000 | 4/4 ✓ |
| **总计** | **20** | **64** | **0** | **0** | **1.000** | **20/20** |

混淆矩阵（4×4）：

| 真实↓ → 预测→ | normal | tachy | brady | arrhy |
|:---------------|:------:|:-----:|:-----:|:-----:|
| normal | 8 | 0 | 0 | 0 |
| tachycardia | 0 | 4 | 0 | 0 |
| bradycardia | 0 | 0 | 4 | 0 |
| arrhythmia | 0 | 0 | 0 | 4 |

- QRS 检测准确率：**100%**（TP=64, FP=0, FN=0）
- 分类准确率：**100%**（20/20）
- 训练集：**8000 样本**（内存合成，384 KB 缓存）
- 模型参数：HR 均值 79.3±11.7 bpm, RR_CV 均值 0.047

## 技术要点

- **采样率**：360 Hz（标准 ECG 采样率）
- **滤波流水线**：去直流 → 5 点移动平均 → 中心差分 → 平方 → 移动窗口积分（MWI）→ 归一化
- **QRS 检测**：自适应双阈值（SPKI/NPKI 指数滑动平均），不应期 200ms，回溯搜索 THRESHOLD2
- **峰值精确定位**：二次抛物线拟合子采样精度
- **异常分类**：硬规则梯度加权 + 高斯概率软评分 + 模糊规则混合决策（6 类得分向量）
- **模型训练**：全类别统计 + K 折交叉验证（5 折），以 HR 估计 MAD 为损失函数搜索最优 α
- **评测标准**：AAMI EC57，±150ms 匹配容忍窗口
- **评测矩阵**：4×4 混淆矩阵 + 各类别精确率/召回率/F1
- **数据隔离**：训练（内存合成）/测试（自动生成 CSV）/用户（手动导入）三集合物理隔离
