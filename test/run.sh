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

cp "$test_dir/sample.s" "$temp_dir/input.s"
"$python" "$tool_dir/CPCAsmNopator.py" "$temp_dir/input.s" --update --modify > "$temp_dir/output.raw"
sed 's|^Analysis: .*|Analysis: <input>|' "$temp_dir/output.raw" > "$temp_dir/output.txt"
tr -d '\r' < "$test_dir/reference.txt" > "$temp_dir/reference.normalized.txt"
tr -d '\r' < "$temp_dir/output.txt" > "$temp_dir/output.normalized.txt"

diff -u "$test_dir/expected_output.s" "$temp_dir/input.s"
diff -u "$temp_dir/reference.normalized.txt" "$temp_dir/output.normalized.txt"

"$python" "$tool_dir/CPCAsmNopator.py" "$test_dir/sample.s" --verbose > "$temp_dir/verbose.txt"
"$python" "$tool_dir/CPCAsmNopator.py" "$test_dir/sample.s" --debug > "$temp_dir/debug.txt"
"$python" "$tool_dir/CPCAsmNopator.py" --help > "$temp_dir/help.txt"
grep -Fq 'planned=annotate/correct' "$temp_dir/verbose.txt"
if grep -Fq '| .module test' "$temp_dir/verbose.txt"; then
	exit 1
fi
grep -Fq '| .module test' "$temp_dir/debug.txt"
if grep -Fq -- '--debug' "$temp_dir/help.txt"; then
	exit 1
fi

printf 'CPCAsmNopator regression passed\n'