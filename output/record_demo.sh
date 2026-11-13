#!/usr/bin/env bash
#
# Segmented, fully non-interactive demo driver for the dual-precision work.
# Each segment prints a numbered title banner, pauses, then runs a real
# command against the already-built trees / artifacts under output/.
# Recorded with:
#   asciinema rec output/demo.cast --overwrite -c "bash output/record_demo.sh"
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
OUT="$ROOT/output"
TOTAL=7

pause () { sleep 1.5 ; }

title ()
{
	pause
	echo
	printf '==============================================================\n'
	printf '  [%d/%d] %s\n' "$1" "$TOTAL" "$2"
	printf '==============================================================\n'
}

# ---------------------------------------------------------------------------
title 1 "公共精度层与多处小改提交 (shared layer, small commits)"
git log --oneline -5
echo
echo "--- 精度抽象层 src/src_precision.h 关键定义 ---"
grep -n "typedef.*SRC_SAMPLE_T\|src_store_api_sample\|src_widen_api_sample\|src_copy_api_to_internal" src/src_precision.h
echo
echo "--- 构建开关 (默认 OFF, float 路径行为不变) ---"
grep -n "LIBSAMPLERATE_DOUBLE_PRECISION" CMakeLists.txt

# ---------------------------------------------------------------------------
title 2 "单精度(默认)路径: 仓库自带 ctest 全绿"
ctest --test-dir build-verify-float

# ---------------------------------------------------------------------------
title 3 "双精度路径: 同一套 ctest 全绿 (控制逻辑/质量档位共用)"
ctest --test-dir build-verify-double

# ---------------------------------------------------------------------------
title 4 "两套构建公开 ABI 符号集合一致"
nm build-verify-float/src/libsamplerate.a | grep ' T ' | awk '{print $3}' | grep '^src_' | sort > /tmp/abi_float.txt
nm build-verify-double/src/libsamplerate.a | grep ' T ' | awk '{print $3}' | grep '^src_' | sort > /tmp/abi_double.txt
if diff -u /tmp/abi_float.txt /tmp/abi_double.txt ; then
	echo ">> 公开 src_* 符号完全一致: $(wc -l < /tmp/abi_float.txt) 个, ABI 不变"
fi

# ---------------------------------------------------------------------------
title 5 "独立 double 参考比对: 双精度 0 ULP, 差异量化"
echo "转储精度标记:"
head -1 "$OUT/dump_float.txt"
head -1 "$OUT/dump_double.txt"
echo
python3 -I "$OUT/precision_check.py" \
	"$OUT/dump_float.txt" "$OUT/dump_double.txt" "$ROOT/src" \
	"$OUT/precision_report.json" "$OUT/precision_report.txt" >/dev/null
echo ">> 完整报告见 output/precision_report.txt, 摘要如下:"
head -12 "$OUT/precision_report.txt"

# ---------------------------------------------------------------------------
title 6 "单精度路径零回归: 与 pristine(0844c20) 逐位一致"
python3 -I "$OUT/bit_regression_check.py" \
	"$OUT/baseline_pristine_0844c20_dump.txt" "$OUT/dump_float.txt"

# ---------------------------------------------------------------------------
title 7 "output/ 产物清单 (全部非交互生成)"
ls -la "$OUT"
echo
echo ">> 量化报告: output/precision_report.txt"
echo ">> 交互图表: output/precision_chart.html"
echo ">> 验证说明: output/VERIFICATION.md"
pause
echo
echo "全部演示完成, 无需任何键盘输入。"
