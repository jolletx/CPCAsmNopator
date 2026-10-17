#!/usr/bin/env python3
"""Annotate Z80 instruction timing comments and report size totals."""

import argparse
import ast
import csv
import re
import sys
from collections import Counter
from pathlib import Path
from typing import Iterable


STATE = {
    "inside_macro": False,
    "inside_area": "",
    "current_code_size": 0,
    "current_data_size": 0,
}

SDAS_HELPERS = {
    "ignore",
    "data_byte",
    "data_word",
    "data_reserve_bytes",
    "data_reserve_words",
    "data_string",
    "data_string_plus_one",
    "macro_start",
    "repeat_start",
    "repeat_skip_start",
    "block_end",
    "repeat_end",
    "conditional",
}


class TimingEntry:
    def __init__(self, instruction: str, time: str, size: int) -> None:
        self.instruction = instruction
        self.time = time
        self.size = size


class LineIssue:
    def __init__(self, line_number: int, message: str) -> None:
        self.line_number = line_number
        self.message = message


class ProcessedLine:
    def __init__(self, original: str, updated: str, recommended=None, size: int = 0, issue=None, line_number: int = 0) -> None:
        self.original = original
        self.updated = updated
        self.recommended = recommended
        self.size = size
        self.issue = issue
        self.line_number = line_number


class AnalysisResult:
    def __init__(self, lines, issues, code_size: int, data_size: int, recommended_modifications: int,
                 timing_min: int = 0, timing_max: int = 0, unknown_timings=None) -> None:
        self.lines = lines
        self.issues = issues
        self.code_size = code_size
        self.data_size = data_size
        self.recommended_modifications = recommended_modifications
        self.timing_min = timing_min
        self.timing_max = timing_max
        self.unknown_timings = unknown_timings or Counter()


def load_timing_entries(path):
    entries = {}
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle, delimiter=";")
        for row in reader:
            instruction = (row.get("Instruction") or "").strip()
            time = (row.get("Time") or "").strip()
            size = (row.get("Size") or "").strip()
            if not instruction or not time or not size:
                continue
            opcode = instruction.split(None, 1)[0].lower()
            entries.setdefault(opcode, []).append(
                TimingEntry(instruction=instruction, time=time, size=int(size))
            )
    return entries


def load_substitutions(path):
    substitutions = {}
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle, delimiter=";")
        for row in reader:
            key = (row.get("input") or "").strip()
            linked = (row.get("linked") or "").strip()
            if key and linked:
                if key.lower() == "cc":
                    substitutions["_condition_codes"] = linked.lower()
                substitutions.setdefault(key.lower(), key.lower())
                for value in linked.split(","):
                    if value.strip():
                        substitutions.setdefault(value.strip().lower(), key.lower())
    return substitutions


def load_sdas_directives(path):
    directives = {}
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle, delimiter=";")
        if not {"Directive", "Helper"}.issubset(reader.fieldnames or ()):
            raise ValueError(f"SDAS directives table must contain Directive and Helper columns: {path}")
        for row in reader:
            directive = (row.get("Directive") or "").strip().lower()
            helper = (row.get("Helper") or "").strip().lower()
            if not directive or not helper:
                raise ValueError(f"Empty directive or helper in SDAS directives table: {path}")
            if directive in directives:
                raise ValueError(f"Duplicate SDAS directive '{directive}' in {path}")
            if helper not in SDAS_HELPERS:
                raise ValueError(f"Unknown SDAS helper '{helper}' for '{directive}' in {path}")
            directives[directive] = helper
    return directives


def split_instruction(line):
    stripped = line.strip()
    if not stripped or stripped.startswith(";"):
        return None
    if stripped.startswith("."):
        return None
    if stripped.startswith("#"):
        return None
    if stripped.startswith("$"):
        return None

    match = re.match(r"^([A-Za-z_][A-Za-z0-9_\.]*)\s*(.*)$", stripped)
    if not match:
        return None
    return match.group(1).lower(), match.group(2).strip()


def normalize_symbol(symbol, substitutions):
    lowered = symbol.lower()
    if lowered in substitutions:
        return substitutions[lowered]
#    if re.fullmatch(r"[a-z]", lowered):
#        return "k"
    if re.fullmatch(r"(?:ixh|ixl|iyh|iyl)", lowered):
        return "s"
    if lowered in {"ix", "iy"}:
        return "qq"
    if lowered in {"bc", "de", "hl"}:
        return "pp"
    if lowered in {"nz", "z", "nc", "c", "po", "pe", "p", "m"}:
        return "cc"
    if re.fullmatch(r"(?:a|b|c|d|e|h|l)", lowered):
        return "k"
#    if re.fullmatch(r"(?:[a-z]|[a-z][a-z0-9_]*)", lowered):
#        return "k"
    if re.fullmatch(r"(?:[0-9]+|#[0-9a-fA-F]+|\$[0-9a-fA-F]+)", lowered):
        return "n"
    if re.fullmatch(r"[-+]?\d+|#[-+]?0x?[0-9a-fA-F]+|\$[-+]?0x?[0-9a-fA-F]+", lowered):
        return "d"
    if re.fullmatch(r"(?:[a-z]|[a-z][a-z0-9_]*)", lowered):
        return "k"
    return symbol


def normalize_operands(operand_text, substitutions):
    operand_text = operand_text.strip()
    if not operand_text:
        return []
    offset_placeholder = "of" if "of" in substitutions else "d"
    operands = []
    for part in re.split(r"\s*,\s*", operand_text):
        tokens = part.strip()
        if not tokens:
            continue
        simplified = tokens.lower()
        if simplified.startswith("(") and simplified.endswith(")"):
            inside = simplified[1:-1]
            if re.match(r"^(?:ix|iy|qq)\s*[+-]", inside):
                if "+" in inside:
                    left, right = inside.split("+", 1)
                    operands.append(f"({normalize_symbol(left, substitutions)}+{offset_placeholder})")
                else:
                    left, right = inside.split("-", 1)
                    operands.append(f"({normalize_symbol(left, substitutions)}-{offset_placeholder})")
            elif "+" in inside or "-" in inside:
                operands.append("(nn)")
            elif inside in {"ix", "iy"}:
                operands.append(f"({normalize_symbol(inside, substitutions)})")
            else:
                operands.append(f"({normalize_symbol(inside, substitutions)})")
            continue
        operands.append(normalize_symbol(simplified, substitutions))
    return operands


def build_instruction_key(opcode, operand_text, substitutions):
    return opcode.lower(), normalize_operands(operand_text, substitutions)


def match_instruction(opcode, operand_text, entries, substitutions):
    source_operands = split_values(operand_text)
    if opcode.lower() in {"and", "or", "xor", "sub", "cp"} and len(source_operands) == 2 and source_operands[0].lower() == "a":
        operand_text = source_operands[1]
    candidates = entries.get(opcode.lower(), [])
    if not candidates:
        return None

    normalized_operands = normalize_operands(operand_text, substitutions)
    source_operands = split_values(operand_text)
    for candidate in candidates:
        template = candidate.instruction.lower()
        template_parts = template.split(None, 1)
        if len(template_parts) == 1:
            template_operands: list[str] = []
        else:
            template_operands = [
                part.lower() if part.lower() in {"b", "cc", "d", "n", "nn", "of", "bt"}
                else re.sub(r"([+-])k\)$", r"\1d)", normalized)
                for part, normalized in zip(
                    split_values(template_parts[1]),
                    normalize_operands(template_parts[1], substitutions),
                )
            ]
        if len(template_operands) != len(normalized_operands):
            continue
        if len(template_operands) != len(source_operands):
            continue
        raw_template_operands = split_values(template_parts[1]) if len(template_parts) > 1 else []
        if any(
            is_concrete_template_operand(template_operand)
            and template_operand.lower() != source.lower()
            for template_operand, source in zip(raw_template_operands, source_operands)
        ):
            continue
        if all(
            template == actual or symbolic_operand_matches(template, actual, source, substitutions)
            for template, actual, source in zip(template_operands, normalized_operands, source_operands)
        ):
            return candidate
    return None


def decode_macro_instruction(opcode: str, operands: str, entries, substitutions):
    match = re.match(r"^([a-z][a-z0-9]*)(_+)(.+)$", opcode.lower())
    if not match or len(match.group(2)) not in {2, 3}:
        return None

    instruction, separator, encoded_operands = match.groups()
    if len(separator) == 3:
        parts = encoded_operands.split("__")
        decoded_operands = []
        for index, part in enumerate(parts):
            part = part.strip("_")
            if not part:
                return None
            if index == 0:
                part = f"({part})"
            decoded_operands.append(part)
    else:
        decoded_operands = encoded_operands.split("_")
        if any(not part for part in decoded_operands):
            return None

    if operands:
        decoded_operands.extend(split_values(operands))
    decoded_text = ", ".join(decoded_operands)
    if match_instruction(instruction, decoded_text, entries, substitutions):
        return instruction, decoded_text
    return None


def is_concrete_template_operand(operand: str) -> bool:
    value = operand.strip().lower()
    registers = {"a", "b", "c", "e", "h", "l", "i", "r", "bc", "de", "hl", "af", "sp", "ix", "iy", "ixh", "ixl", "iyh", "iyl", "af'"}
    return value in registers or value in {"(bc)", "(de)", "(hl)", "(sp)", "(ix)", "(iy)"}


def symbolic_operand_matches(template: str, actual: str, source: str, substitutions=None) -> bool:
    value = source.strip().lower()
    if template == "cc":
        conditions = (substitutions or {}).get("_condition_codes", "")
        return value in {condition.strip().lower() for condition in conditions.split(",")}
    if template == "bt":
        bit_number = parse_integer(value)
        if bit_number is not None:
            return 0 <= bit_number <= 7
        return bool(re.fullmatch(r"[a-z_.$?][a-z0-9_.$?]*", value))
    if template == "nn" and symbolic_address_expression(value):
        return True
    if template == "n":
        value = re.sub(r"^[<>]#?", "", value)
        if parse_integer(value) is not None:
            return True
        if re.fullmatch(r"[a-z_.$?][a-z0-9_.$?]*", value):
            return value not in {"a", "b", "c", "d", "e", "h", "l", "i", "r", "ix", "iy", "sp", "bc", "de", "hl"}
    if template in {"b", "n", "nn", "d"}:
        if parse_integer(value) is not None:
            return True
        value = value.lstrip("#")
        if re.fullmatch(r"[a-z_.$?][a-z0-9_.$?]*", value):
            return value not in {"a", "b", "c", "d", "e", "h", "l", "i", "r", "ix", "iy", "sp", "bc", "de", "hl"}
    if template == "(nn)" and value.startswith("(") and value.endswith(")"):
        inner = value[1:-1].strip()
        return bool(re.fullmatch(r"[a-z_.$?][a-z0-9_.$?]*", inner)) and inner not in {
            "a", "b", "c", "d", "e", "h", "l", "ix", "iy", "sp", "bc", "de", "hl"
        }
    return False


def symbolic_address_expression(value: str) -> bool:
    expression = value.strip().lower().lstrip("#")
    terms = re.split(r"\s*\+\s*", expression)
    if len(terms) != 2:
        return False

    def is_symbol(term: str) -> bool:
        return bool(re.fullmatch(r"[a-z_.$?][a-z0-9_.$?]*", term)) and term not in {
            "a", "b", "c", "d", "e", "h", "l", "i", "r", "ix", "iy", "sp", "bc", "de", "hl", "af"
        }

    left, right = terms
    return (is_symbol(left) and parse_integer(right) is not None) or (
        parse_integer(left) is not None and is_symbol(right)
    )


def update_timing_comment(line: str, new_time: str, comment_column: int = 0) -> str:
    code, comment = split_comment(line)
    if comment is not None:
        marker = re.match(r";{1,2}", comment)
        if marker:
            body = comment[marker.end():]
            body = re.sub(r"^\s*\[[^\]]+\]\s*", " ", body)
            return f"{code}{marker.group(0)} [{new_time}]{body}"
    padding = max(1, comment_column - len(line.rstrip()))
    return f"{line.rstrip()}{' ' * padding};; [{new_time}]"


def find_timing_comment(line):
    _, comment = split_comment(line)
    match = re.match(r";{1,2}\s*\[([^\]]+)\]", comment or "")
    if match:
        return match.group(1).strip()
    return None


def split_comment(line: str):
    quote = None
    escaped = False
    for index, character in enumerate(line):
        if escaped:
            escaped = False
            continue
        if character == "\\":
            escaped = True
            continue
        if quote:
            if character == quote:
                quote = None
            continue
        if character in {'"', "'"}:
            quote = character
        elif character == ";":
            return line[:index], line[index:]
    return line, None


def split_values(text: str):
    values = []
    start = 0
    quote = None
    escaped = False
    depth = 0
    for index, character in enumerate(text):
        if escaped:
            escaped = False
            continue
        if character == "\\":
            escaped = True
        elif quote:
            if character == quote:
                quote = None
        elif character in {'"', "'"}:
            quote = character
        elif character == "(":
            depth += 1
        elif character == ")":
            depth = max(depth - 1, 0)
        elif character == "," and depth == 0:
            values.append(text[start:index].strip())
            start = index + 1
    tail = text[start:].strip()
    if tail:
        values.append(tail)
    return values


def parse_integer(value: str):
    value = value.strip().lstrip("#")
    try:
        if value.startswith("$"):
            return int(value[1:], 16)
        if value.lower().startswith("0x"):
            return int(value, 16)
        if value[-1:].lower() in {"h", "o", "b"}:
            base = {"h": 16, "o": 8, "b": 2}[value[-1].lower()]
            return int(value[:-1], base)
        return int(value, 10)
    except (ValueError, IndexError):
        return None


def string_size(value: str):
    try:
        decoded = ast.literal_eval(value)
        return len(decoded.encode("latin-1")) if isinstance(decoded, str) else None
    except (SyntaxError, ValueError, UnicodeEncodeError):
        return None


def resolve_symbol_value(expression: str, symbols):
    value = expression.strip().strip("()")
    numeric = parse_integer(value)
    if numeric is not None:
        return numeric
    if re.fullmatch(r"[A-Za-z_.$?][A-Za-z0-9_.$?]*", value):
        return symbols.get(value.lower())
    return None


def find_data_size_comment(line: str):
    _, comment = split_comment(line)
    match = re.match(r";{1,2}\s*\((\d+)\)", comment or "")
    return int(match.group(1)) if match else None


def update_data_size_comment(line: str, size: int, comment_column: int = 0) -> str:
    code, comment = split_comment(line)
    if comment is not None:
        marker = re.match(r";{1,2}", comment)
        if marker:
            body = comment[marker.end():]
            body = re.sub(r"^\s*\(\d+\)\s*", " ", body)
            return f"{code}{marker.group(0)} ({size}){body}"
    padding = max(1, comment_column - len(line.rstrip()))
    return f"{line.rstrip()}{' ' * padding};; ({size})"


def data_directive_size(helper: str, operands: str):
    values = split_values(operands)
    if helper == "data_byte":
        return sum(string_size(value) if string_size(value) is not None else 1 for value in values)
    if helper == "data_word":
        return 2 * len(values)
    if helper == "data_reserve_bytes":
        return parse_integer(values[0]) if values else 0
    if helper == "data_reserve_words":
        count = parse_integer(values[0]) if values else 0
        return count * 2 if count is not None else None
    if helper in {"data_string", "data_string_plus_one"}:
        size = 0
        for value in values:
            value_size = string_size(value)
            if value_size is None:
                return None
            size += value_size
        if helper == "data_string_plus_one":
            size += 1
        return size
    return 0


def split_statement(line: str, known_opcodes):
    code, _ = split_comment(line)
    stripped = code.strip()
    if not stripped or stripped.startswith("#"):
        return None, "", False

    label_match = re.match(r"^([A-Za-z_.$?][A-Za-z0-9_.$?]*)\s*:{1,2}\s*(.*)$", stripped)
    if label_match:
        stripped = label_match.group(2).strip()
        if not stripped:
            return None, "", True
    elif line and not line[0].isspace():
        first = re.match(r"^([^\s]+)(?:\s+(.*))?$", stripped)
        encoded_opcode = re.match(r"^[a-z][a-z0-9]*_{2,3}.", first.group(1), re.IGNORECASE) if first else None
        if first and first.group(1).lower() not in known_opcodes and not first.group(1).startswith(".") and not encoded_opcode:
            remainder = first.group(2) or ""
            if remainder:
                stripped = remainder
            else:
                return None, "", True

    match = re.match(r"^([A-Za-z_.][A-Za-z0-9_.$]*)(?:\s+(.*))?$", stripped)
    if not match:
        return None, "", False
    return match.group(1).lower(), (match.group(2) or "").strip(), False


def analyze(path: Path, update: bool, modify: bool, data_size_comments: bool = False) -> AnalysisResult:
    root = Path(__file__).resolve().parent
    input_dir = root / "input"
    entries = load_timing_entries(input_dir / "Instruction.csv")
    substitutions = load_substitutions(input_dir / "subtitutions.csv")
    sdas_directives = load_sdas_directives(input_dir / "SdasDirectives.csv")
    macro_start_directives = [
        directive for directive, helper in sdas_directives.items() if helper == "macro_start"
    ]
    macro_pattern = re.compile(
        rf"^\s*(?:{'|'.join(re.escape(directive) for directive in macro_start_directives)})"
        r"\s+([A-Za-z_.$?][A-Za-z0-9_.$?]*)",
        re.IGNORECASE,
    ) if macro_start_directives else None
    issues = []
    processed = []
    code_size = 0
    data_size = 0
    recommendations = 0
    timing_min = 0
    timing_max = 0
    unknown_timings = Counter()
    raw_contents = path.read_bytes()
    decoded_lines = []
    byte_offset = 0
    for line_number, raw_line in enumerate(raw_contents.splitlines(keepends=True), 1):
        try:
            decoded_lines.append(raw_line.decode("utf-8"))
        except UnicodeDecodeError as error:
            if raw_line.lstrip().startswith(b";"):
                decoded_lines.append(raw_line.decode("utf-8", errors="replace"))
            else:
                preview = raw_line.rstrip(b"\r\n").decode("utf-8", errors="replace")
                issues.append(LineIssue(
                    line_number,
                    f"Invalid UTF-8 at column {error.start + 1}, byte offset {byte_offset + error.start}: "
                    f"{error.reason}; source line: {preview!r}; raw bytes: {raw_line.rstrip(bytes([10, 13]))!r}",
                ))
                return AnalysisResult([], issues, 0, 0, 0)
        byte_offset += len(raw_line)
    contents = "".join(decoded_lines)
    line_ending = "\r\n" if "\r\n" in contents else "\n"
    lines = contents.splitlines()
    known_opcodes = set(entries)
    known_macros = set()
    local_labels = set()
    symbol_values = {}

    for raw_line in lines:
        code, _ = split_comment(raw_line)
        assignment = re.match(r"^\s*([A-Za-z_.$?][A-Za-z0-9_.$?]*)\s*=\s*(.*?)\s*$", code)
        if assignment:
            value = resolve_symbol_value(assignment.group(2), symbol_values)
            if value is not None:
                symbol_values[assignment.group(1).lower()] = value
        macro_match = macro_pattern.match(code) if macro_pattern else None
        if macro_match:
            known_macros.add(macro_match.group(1).lower())
        label_match = re.match(r"^\s*([A-Za-z_.$?][A-Za-z0-9_.$?]*)\s*:{1,2}", code)
        if label_match:
            local_labels.add(label_match.group(1).lower())

    comment_columns = []
    for raw_line in lines:
        code, comment = split_comment(raw_line)
        if comment and code.strip():
            comment_columns.append(len(code))
    comment_column = Counter(comment_columns).most_common(1)[0][0] if comment_columns else 0

    blocks = []
    macro_timings = {}
    macro_contexts = []
    repeat_multiplier = 1
    for line_number, raw_line in enumerate(lines, 1):
        opcode, operands, label_only = split_statement(raw_line, known_opcodes)
        updated = raw_line
        timing = None
        size = 0

        if opcode is None:
            processed.append(ProcessedLine(raw_line, updated, None, 0, line_number=line_number))
            continue

        directive_helper = sdas_directives.get(opcode, "ignore")
        if directive_helper == "macro_start":
            blocks.append(("macro", repeat_multiplier))
            macro_name = operands.split(None, 1)[0].lower() if operands else ""
            macro_contexts.append({"name": macro_name, "cycles": 0, "size": 0, "instructions": 0, "fixed": bool(macro_name)})
            processed.append(ProcessedLine(raw_line, updated, None, 0, line_number=line_number))
            continue
        if directive_helper in {"repeat_start", "repeat_skip_start"}:
            if directive_helper == "repeat_start":
                count_text = operands.split(None, 1)[0] if operands else ""
                count = resolve_symbol_value(count_text, symbol_values)
                if count is None:
                    issues.append(LineIssue(line_number, f"Cannot determine repeat count '{operands}'"))
                    if macro_contexts:
                        macro_contexts[-1]["fixed"] = False
                    count = 1
                blocks.append(("rept", repeat_multiplier))
                repeat_multiplier *= max(count, 0)
            else:
                if macro_contexts:
                    macro_contexts[-1]["fixed"] = False
                blocks.append(("skip", repeat_multiplier))
            processed.append(ProcessedLine(raw_line, updated, None, 0, line_number=line_number))
            continue
        if directive_helper in {"block_end", "repeat_end"}:
            if blocks and (directive_helper == "block_end" or blocks[-1][0] in {"rept", "skip"}):
                block_type, previous_multiplier = blocks.pop()
                if block_type == "rept":
                    repeat_multiplier = previous_multiplier
                elif block_type == "macro":
                    macro_context = macro_contexts.pop()
                    if macro_context["fixed"] and macro_context["instructions"]:
                        macro_timings[macro_context["name"]] = (
                            str(macro_context["cycles"]),
                            macro_context["size"],
                        )
            processed.append(ProcessedLine(raw_line, updated, None, 0, line_number=line_number))
            continue

        inside_macro = bool(macro_contexts)
        if any(block[0] == "skip" for block in blocks):
            processed.append(ProcessedLine(raw_line, updated, None, 0, line_number=line_number))
            continue

        if opcode.startswith(".") or opcode.startswith("#"):
            if inside_macro:
                if directive_helper.startswith("data_") or directive_helper == "conditional":
                    macro_contexts[-1]["fixed"] = False
                processed.append(ProcessedLine(raw_line, updated, None, 0, line_number=line_number))
                continue
            directive_size = data_directive_size(directive_helper, operands)
            data_planned = False
            data_action = None
            if directive_size is None:
                issues.append(LineIssue(line_number, f"Cannot determine size of {opcode} data"))
            elif directive_size:
                size = directive_size * repeat_multiplier
                data_size += size
                if data_size_comments and directive_helper.startswith("data_"):
                    existing_size = find_data_size_comment(raw_line)
                    if existing_size is None or existing_size != size:
                        data_planned = True
                        data_action = "add size" if existing_size is None else "correct size"
                        recommendations += 1
                        updated = update_data_size_comment(raw_line, size, comment_column)
                        if existing_size is not None and not update:
                            issues.append(LineIssue(line_number, f"Data size mismatch: found ({existing_size}), expected ({size}) (use --update to replace)"))
            processed.append(ProcessedLine(raw_line, updated, None, size, issue=data_action if data_planned else None, line_number=line_number))
            continue

        macro_summary = None
        decoded = decode_macro_instruction(opcode, operands, entries, substitutions)
        if decoded:
            opcode, operands = decoded
        if not decoded and opcode in known_macros:
            macro_summary = macro_timings.get(opcode)
            if macro_summary is None:
                if inside_macro:
                    macro_contexts[-1]["fixed"] = False
                processed.append(ProcessedLine(raw_line, updated, None, 0, line_number=line_number))
                continue
            matched = None
        else:
            matched = match_instruction(opcode, operands, entries, substitutions)
        if matched is None and macro_summary is None:
            if inside_macro:
                macro_contexts[-1]["fixed"] = False
                processed.append(ProcessedLine(raw_line, updated, None, 0, line_number=line_number))
                continue
            unknown_timings[opcode] += 1
            issues.append(LineIssue(line_number, f"Unknown operator or macro '{opcode}'"))
            processed.append(ProcessedLine(raw_line, updated, None, 0, line_number=line_number))
            continue

        if macro_summary is not None:
            timing, size = macro_summary
        else:
            timing = matched.time
            size = matched.size
        existing = find_timing_comment(raw_line)
        planned = existing is None or existing != timing
        action = "annotate" if existing is None else "correct"
        if existing is None:
            recommendations += 1
            updated = update_timing_comment(raw_line, timing, comment_column)
        elif existing != timing:
            recommendations += 1
            updated = update_timing_comment(raw_line, timing, comment_column)
            if not update:
                issues.append(LineIssue(line_number, f"Timing mismatch: found [{existing}], expected [{timing}] (use --update to replace)"))

        if inside_macro:
            macro_context = macro_contexts[-1]
            if re.fullmatch(r"\d+", timing):
                macro_context["cycles"] += int(timing) * repeat_multiplier
                macro_context["size"] += size * repeat_multiplier
                macro_context["instructions"] += repeat_multiplier
            else:
                macro_context["fixed"] = False
            size *= repeat_multiplier
        else:
            code_size += size * repeat_multiplier
            size *= repeat_multiplier
            timing_values = [int(value) for value in timing.split("/") if value.isdigit()]
            if timing_values:
                timing_min += min(timing_values)
                timing_max += max(timing_values)

        if opcode == "call" and not inside_macro:
            targets = split_values(operands)
            target = targets[-1].strip("() ").lower() if targets else ""
            if target and target not in local_labels:
                unknown_timings[target] += 1
                issues.append(LineIssue(line_number, f"Call to unknown timing subroutine '{target}'"))

        processed.append(ProcessedLine(raw_line, updated, timing, size, issue=action if planned else None, line_number=line_number))

    if modify:
        output = line_ending.join(
            item.updated
            if update or (find_timing_comment(item.original) is None and find_data_size_comment(item.original) is None)
            else item.original
            for item in processed
        )
        if contents.endswith(("\n", "\r")):
            output += line_ending
        with path.open("w", encoding="utf-8", newline="") as handle:
            handle.write(output)

    return AnalysisResult(processed, issues, code_size, data_size, recommendations,
                          timing_min, timing_max, unknown_timings)


def format_timing_summary(timing_min: int, timing_max: int, unknown_timings) -> str:
    remainder = []
    for label, count in unknown_timings.items():
        remainder.append(f"{count} x {label}" if count > 1 else label)
    suffix = "".join(f" + {item}" for item in remainder)
    return f"Timings (no loop)  min:{timing_min} NOPs max:{timing_max} NOPs{suffix}"


def print_summary(result: AnalysisResult, path: Path, verbose: bool, no_modify: bool, debug: bool = False) -> None:
    print(f"Analysis: {path}")
    print(f"Recommended modifications: {result.recommended_modifications}")
    print(f"Global code size: {result.code_size} bytes")
    print(f"Global data size: {result.data_size} bytes")
    print(format_timing_summary(result.timing_min, result.timing_max, result.unknown_timings))
    print()

    if result.issues:
        print("Warnings:")
        for issue in result.issues:
            print(f"- Line {issue.line_number}: {issue.message}")
        print()

    if debug or verbose:
        for item in result.lines:
            if not debug and item.issue is None:
                continue
            action = item.issue or "none"
            timing = f"[{item.recommended}]" if item.recommended is not None else "-"
            print(f"Line {item.line_number}: size={item.size} bytes timing={timing} action={action}")
            if item.original != item.updated:
                print(f"Before: {item.original}")
                print(f"After:  {item.updated}")
    elif no_modify:
        for item in result.lines:
            if item.original == item.updated:
                continue
            print(f"Before: {item.original}")
            print(f"After:  {item.updated}")


def parse_args(argv: Iterable[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", help="SDAS source file or directory to analyze")
    parser.add_argument("--update", "-u", action="store_true", help="Replace incorrect timing comments")
    parser.add_argument("--verbose", "-v", action="store_true", help="Print recommended modifications")
    parser.add_argument("--debug", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--modify", "-m", action="store_true", help="Write annotations to the input source file")
    parser.add_argument("--data-size", action="store_true", help="Add or update byte-size comments on data directives")
    return parser.parse_args(list(argv))


def analyze_directory(path: Path, args: argparse.Namespace) -> int:
    source_files = sorted(
        (item for item in path.rglob("*") if item.is_file() and item.suffix.lower() in {".s", ".asm"}),
        key=lambda item: str(item).lower(),
    )
    if not source_files:
        print(f"Error: no .s or .asm source files found in: {path}", file=sys.stderr)
        return 2

    total_recommendations = 0
    total_code_size = 0
    total_data_size = 0
    total_issues = 0
    files_with_issues = 0
    total_timing_min = 0
    total_timing_max = 0
    unknown_timings = Counter()
    for index, source_file in enumerate(source_files, 1):
        relative_path = source_file.relative_to(path)
        print(f"=== File {index}/{len(source_files)}: {relative_path} ===")
        result = analyze(source_file, args.update, args.modify, args.data_size)
        print_summary(result, source_file, args.verbose, not args.modify, args.debug)
        total_recommendations += result.recommended_modifications
        total_code_size += result.code_size
        total_data_size += result.data_size
        total_timing_min += result.timing_min
        total_timing_max += result.timing_max
        unknown_timings.update(result.unknown_timings)
        total_issues += len(result.issues)
        files_with_issues += bool(result.issues)

    print("Directory summary:")
    print(f"Files analyzed: {len(source_files)}")
    print(f"Recommended modifications: {total_recommendations}")
    print(f"Global code size: {total_code_size} bytes")
    print(f"Global data size: {total_data_size} bytes")
    print(format_timing_summary(total_timing_min, total_timing_max, unknown_timings))
    print(f"Warnings: {total_issues} across {files_with_issues} files")
    print(f"Directory status: {'PASS' if not total_issues else 'WARNINGS'}")
    return 0


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else list(argv))
    path = Path(args.file).expanduser()
    if path.is_dir():
        return analyze_directory(path, args)
    if not path.is_file():
        print(f"Error: input file not found: {path}", file=sys.stderr)
        return 2

    result = analyze(path, args.update, args.modify, args.data_size)
    print_summary(result, path, args.verbose, not args.modify, args.debug)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
