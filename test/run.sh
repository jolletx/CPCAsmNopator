#!/usr/bin/env bash
set -euo pipefail

tool_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_dir="$tool_dir/test"
temp_dir="$tool_dir/temp"
python="${PYTHON:-python3}"

mkdir -p "$temp_dir"
cp "$test_dir/sample.s" "$temp_dir/no_update.s"
"$python" "$tool_dir/CPCAsmNopator.py" "$temp_dir/no_update.s" --modify > "$temp_dir/no_update.txt"
grep -Fq ';; [3] deliberately incorrect' "$temp_dir/no_update.s"
if grep -Fq ';; (' "$temp_dir/no_update.s"; then
	exit 1
fi
grep -Fq 'Global data size: 30 bytes' "$temp_dir/no_update.txt"

printf '.db 2\n.area _CODE\n.db 1\nnop\n.area _DATA\nnop\n.dw 0\nnop\n' > "$temp_dir/area_sizes.s"
"$python" "$tool_dir/CPCAsmNopator.py" "$temp_dir/area_sizes.s" > "$temp_dir/area_sizes.txt"
grep -Fq 'Global code size: 3 bytes' "$temp_dir/area_sizes.txt"
grep -Fq 'Global data size: 4 bytes' "$temp_dir/area_sizes.txt"

cp "$test_dir/sample.s" "$temp_dir/input.s"
"$python" "$tool_dir/CPCAsmNopator.py" "$temp_dir/input.s" --update --modify --data-size > "$temp_dir/output.raw"
sed 's|^Analysis: .*|Analysis: <input>|' "$temp_dir/output.raw" > "$temp_dir/output.txt"
tr -d '\r' < "$test_dir/reference.txt" > "$temp_dir/reference.normalized.txt"
tr -d '\r' < "$temp_dir/output.txt" > "$temp_dir/output.normalized.txt"

diff -u "$test_dir/expected_output.s" "$temp_dir/input.s"
diff -u "$temp_dir/reference.normalized.txt" "$temp_dir/output.normalized.txt"

"$python" "$tool_dir/CPCAsmNopator.py" "$test_dir/sample.s" --verbose > "$temp_dir/verbose.txt"
"$python" "$tool_dir/CPCAsmNopator.py" "$test_dir/sample.s" --debug --data-size > "$temp_dir/debug.txt"
"$python" "$tool_dir/CPCAsmNopator.py" --help > "$temp_dir/help.txt"
grep -Fq 'action=annotate' "$temp_dir/verbose.txt"
grep -Fq 'Timings (no loop)  min:85 NOPs max:87 NOPs + 2 x mystery + unknown_op' "$temp_dir/verbose.txt"
grep -Fq 'Before:     nop' "$temp_dir/verbose.txt"
grep -Fq 'After:      nop' "$temp_dir/verbose.txt"
grep -Fq -- '--data-size' "$temp_dir/help.txt"
if grep -Fq '.module test' "$temp_dir/verbose.txt"; then
	exit 1
fi
grep -Fq 'Line 1: size=0 bytes timing=- action=none' "$temp_dir/debug.txt"
if grep -Fq '| .module test' "$temp_dir/debug.txt"; then
	exit 1
fi
grep -Fq 'action=correct' "$temp_dir/debug.txt"
grep -Fq 'action=add size' "$temp_dir/debug.txt"
if grep -Fq -- '--debug' "$temp_dir/help.txt"; then
	exit 1
fi

make -C "$tool_dir"
cp "$test_dir/sample.s" "$temp_dir/parity.s"
"$python" "$tool_dir/CPCAsmNopator.py" "$temp_dir/parity.s" --update --modify --data-size > "$temp_dir/python-parity.txt"
cp "$test_dir/sample.s" "$temp_dir/parity.s"
"$tool_dir/bin/CPCAsmNopator" "$temp_dir/parity.s" --update --modify --data-size > "$temp_dir/c-parity.txt"
diff -u "$temp_dir/python-parity.txt" "$temp_dir/c-parity.txt"
diff -u "$test_dir/expected_output.s" "$temp_dir/parity.s"

"$python" "$tool_dir/CPCAsmNopator.py" "$test_dir/sample.s" --verbose > "$temp_dir/python-verbose.txt"
"$tool_dir/bin/CPCAsmNopator" "$test_dir/sample.s" --verbose > "$temp_dir/c-verbose.txt"
diff -u "$temp_dir/python-verbose.txt" "$temp_dir/c-verbose.txt"
"$python" "$tool_dir/CPCAsmNopator.py" "$test_dir/sample.s" --debug --data-size > "$temp_dir/python-debug.txt"
"$tool_dir/bin/CPCAsmNopator" "$test_dir/sample.s" --debug --data-size > "$temp_dir/c-debug.txt"
diff -u "$temp_dir/python-debug.txt" "$temp_dir/c-debug.txt"

printf 'CPCAsmNopator regression passed\n'