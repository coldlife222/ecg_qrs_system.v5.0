# ECG QRS波检测与心率异常识别系统 Makefile v5.1
# 全C语言实现，无外部依赖

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c99 -O3 -ffast-math -funroll-loops -Iinclude -finput-charset=UTF-8 -fexec-charset=UTF-8
LDFLAGS = -lm

# 源文件列表
SRC_FILTER    = src/ecg_filter.c
SRC_DETECTOR  = src/ecg_detector.c
SRC_TRAINER   = src/ecg_trainer.c
SRC_INFERENCE = src/ecg_inference.c
SRC_EVAL      = src/ecg_eval.c
SRC_IO        = src/ecg_io.c
SRC_SYNTH     = src/ecg_synth.c

CORE_SRCS = $(SRC_FILTER) $(SRC_DETECTOR) $(SRC_TRAINER) $(SRC_INFERENCE) $(SRC_EVAL) $(SRC_IO) $(SRC_SYNTH)

.PHONY: all clean test interactive run_test import_user

# 默认：编译所有可执行文件
all: test interactive import_user

# -------- 训练+测试一体化程序（内建信号合成）--------
test: ecg_test.exe

ecg_test.exe: main_test.c $(CORE_SRCS)
	$(CC) -static $(CFLAGS) -o $@ main_test.c $(CORE_SRCS) $(LDFLAGS)
	@echo "[OK] ecg_test.exe"

# -------- 交互式程序 --------
interactive: ecg_interactive.exe

ecg_interactive.exe: main_interactive.c $(CORE_SRCS)
	$(CC) -static $(CFLAGS) -o $@ main_interactive.c $(CORE_SRCS) $(LDFLAGS)
	@echo "[OK] ecg_interactive.exe"

# -------- 运行测试（内建合成，一键完成）-------
run_test: ecg_test.exe
	./ecg_test.exe

# -------- 用户数据导入工具 --------
import_user: import_user.exe

import_user.exe: src/import_user.c
	$(CC) -static $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "[OK] import_user.exe"

# -------- 清理 --------
clean:
	rm -f ecg_test.exe ecg_interactive.exe import_user.exe
	rm -rf data/train data/test output
	@echo "[OK] 清理完成"
