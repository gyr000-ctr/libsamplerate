# 双精度重采样链路 — 验证报告

## 1. 目标与设计约束

在不复制现有重采样路径的前提下，为 libsamplerate 增加一条**全双精度内部链路**，
并满足：

- 核心重采样在双精度下运行：滤波系数、内部延迟线/缓冲、累加全部为 `double`；
- 与单精度路径共用同一套控制逻辑（比率处理、定点索引簿记、状态机）与质量档位
  （`SINC_BEST/MEDIUM/FASTEST`、`ZOH`、`LINEAR`）；
- 公开 API 保持 `float` 不变，导出 ABI 逐符号一致；
- 单精度（默认）路径零回归；
- 两种精度的结果差异可量化，且双精度结果与独立参考一致。

## 2. 实现方式：抽公共层，而非复制路径

新增唯一的精度抽象层 `src/src_precision.h`，定义内部数据平面类型 `SRC_SAMPLE_T`
与三个边界 helper：

| 构件 | 默认构建（float） | `-DLIBSAMPLERATE_DOUBLE_PRECISION=ON`（double） |
|---|---|---|
| `SRC_SAMPLE_T` | `float` | `double` |
| `src_from_api_sample` | 恒等 | float → double 提升 |
| `src_widen_api_sample` | `(double)` 提升（与历史代码逐字等价） | 保持 binary64 |
| `src_store_api_sample` | 直接赋值 | double → float 收敛（唯一的精度收敛点） |
| `src_copy_api_to_internal` | `memcpy` | 逐元素提升 |

各转换器只把“数据平面”的类型改为 `SRC_SAMPLE_T`，**控制逻辑一行未动**：

- `src_sinc.c`：`coeff_t` 与环形缓冲改为 `SRC_SAMPLE_T`（系数表经同一 typedef
  在 double 构建中整体变为双精度常量），定点 `increment_t`、`SHIFT_BITS`、
  比率插值、终止条件全部保持原样；
- `src_linear.c` / `src_zoh.c`：每通道 `last_value` 延迟线改为 `SRC_SAMPLE_T`；
- 构建开关 `LIBSAMPLERATE_DOUBLE_PRECISION`（默认 `OFF`），默认构建行为不变。

提交粒度（每处小改一个提交）：

1. `Add shared precision selection layer (src_precision.h)`
2. `sinc: route coefficients and delay line through precision layer`
3. `linear,zoh: keep last-value state in internal precision`
4. `precision layer: add widen helper, keep float build arithmetic identical`
5. `tests: add dual-precision verification entry point`

## 3. 如何构建两种精度

```sh
cmake -S . -B build-float   -DLIBSAMPLERATE_DOUBLE_PRECISION=OFF   # 默认
cmake -S . -B build-double  -DLIBSAMPLERATE_DOUBLE_PRECISION=ON
cmake --build build-float build-double
```

## 4. 验证入口（全部为仓库自带/新增的 ctest）

新增 `tests/double_precision_test.c` 并注册为 ctest `double_precision_test`。
它通过公开 API（`src_new/src_process/src_reset`，257 帧分片流式输入、
reset 一致性、varispeed 变比率）覆盖全部 5 个转换器 × 5 个比率 × 1/2/3 通道
= 75 个用例，断言输出有限、帧计数精确，并以十六进制浮点转储全部输出样本。
同一测试在两种精度构建下分别运行。

## 5. 验收证据

### 5.1 两套精度测试全绿（仓库自带 ctest 入口）

| 构建 | 结果 |
|---|---|
| float（默认），Debug | 14/14 passed |
| double，Debug | 14/14 passed |
| float，ASan+UBSan | 14/14 passed，无任何诊断 |
| double，ASan+UBSan | 14/14 passed，无任何诊断 |

### 5.2 单精度路径零回归（逐位）

将改动后 float 构建的 175,227 行十六进制浮点输出与 pristine（`0844c20`）
同配置输出逐行比较：

| 优化级别 | 结果 |
|---|---|
| `-O0`（Debug，PIC） | 完全逐位一致 |
| `-O2`（Release 级别，PIC） | 完全逐位一致 |

闸门脚本：`bit_regression_check.py`；基线转储：
`baseline_pristine_0844c20_dump.txt`。此外两个构建的动态库导出符号集合
（23 个）完全相同，ABI 不变。

### 5.3 双精度与独立参考一致

`precision_check.py` 是**独立**的纯 double 参考实现（重新推导 linear、zoh、
sinc 多相算法，并直接解析项目自带系数表），不依赖 C 实现。75/75 用例全部
交叉验证：

- **双精度路径对参考的最大误差：0 个 binary32 ULP（全部用例逐位吻合）**；
- 参考失败用例数：0。

### 5.4 两种精度差异量化

75 个用例的 float-build 与 double-build 输出对比（详见 `precision_report.txt` /
`.json` 与 `precision_chart.html`）：

| 转换器 | max \|double−float\| | max 相对差 | 双精度 vs 参考 | 单精度 vs 参考（ULP） |
|---|---|---|---|---|
| SINC_BEST | 5.96e-08 | 6.85e-05 | 0 ULP | 627 |
| SINC_MEDIUM | 5.96e-08 | 7.14e-04 | 0 ULP | 10,176 |
| SINC_FASTEST | 5.96e-08 | 1.82e-04 | 0 ULP | 2,790 |
| ZOH | 0（逐位相同） | 0 | 0 ULP | 0 |
| LINEAR | 0（逐位相同） | 0 | 0 ULP | 0 |

- 两构建逐位相同的样本占比：82.0%；
- 差异仅出现在 sinc 三档，根因是默认构建把系数与延迟线存为 binary32，
  双精度构建将系数、延迟线、累加全程保持在 binary64；
- ZOH/LINEAR 两种精度输出完全一致（其数据平面在边界即被收敛，内部本就以
  double 计算）。

## 6. output/ 产物清单

| 文件 | 内容 |
|---|---|
| `run_verification.sh` | 非交互端到端验证：双模式配置/构建/ctest + 参考比对 |
| `record_demo.sh` | 分段演示驱动（7 段标题卡 + 段间停顿，全程无键盘输入） |
| `demo.cast` | asciinema 分段录屏（`asciinema play demo.cast` 回放，22s，7 段带标题） |
| `dump_float.txt` / `dump_double.txt` | 两种精度构建的十六进制浮点转储 |
| `precision_check.py` | 独立 double 参考实现与比对器（`python3 -I` 运行） |
| `precision_report.txt` / `.json` | 75 用例完整量化报告 |
| `precision_chart.html` | 自包含交互图表（悬停提示、表格、深色模式） |
| `bit_regression_check.py` | float 路径逐位回归闸门 |
| `baseline_pristine_0844c20_dump.txt` | pristine 基线转储（自包含比对基准） |
| `cmake_*.log` / `build_*.log` / `ctest_*.log` | 端到端运行日志 |

### 录屏分段（`demo.cast`，段间停顿、无键盘输入）

1. 公共精度层与多处小改提交
2. 单精度（默认）路径 ctest 全绿
3. 双精度路径同一套 ctest 全绿
4. 两套构建公开 ABI 符号集合一致
5. 独立 double 参考比对：双精度 0 ULP、差异量化
6. 单精度路径零回归：与 pristine 逐位一致
7. `output/` 产物清单
