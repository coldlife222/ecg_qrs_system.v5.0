# ECG QRS Monitor 项目

## 技术栈
- Web UI (HTML/CSS/JS + Chart.js) 复现专业医疗监控界面
- 左侧导航 + 4 页面: 健康概览/心电记录/健康报告/设置
- 完整 Pan-Tompkins QRS 检测算法 JS 移植版

## 算法验证 (2026-06-12)
- Python 对比测试: test_000~002 (短记录) 100% Precision/Recall/F1
- train_7982 (1440样本, 6 GT峰值): 100% 准确率
- 算法与原始 C 代码功能对等

## 项目结构
```
ECGMonitor/
├── ECGMonitor.csproj      # WinForms 项目 (待编译)
├── Core/                   # C# 算法移植
├── WebUI/                  # Web 应用 (当前可用)
│   ├── index.html
│   ├── styles.css
│   ├── app.js
│   ├── ecg_processor.js
│   ├── server.py
│   └── data/test/          # 测试数据 (41个CSV)
└── test_qrs.py             # 算法验证脚本
```

## 已知限制
- PR间期/QRS时限需P波/T波检测 (原始C项目也未实现)
- WinForms 版本需 Visual Studio/.NET SDK 编译
