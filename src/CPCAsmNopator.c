#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <regex.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char *instruction;
    char *opcode;
    char *operands;
    char *time;
    int size;
} TimingEntry;

typedef struct {
    char *original;
    char *updated;
    char *recommended;
    char *action;
    int size;
    int line_number;
    bool planned;
} ProcessedLine;

typedef struct {
    int line_number;
    char *message;
} LineIssue;

typedef struct {
    char *label;
    int count;
} UnknownTiming;

typedef struct {
    ProcessedLine *lines;
    size_t line_count;
    LineIssue *issues;
    size_t issue_count;
    int code_size;
    int data_size;
    int recommendations;
    int timing_min;
    int timing_max;
    UnknownTiming *unknown_timings;
    size_t unknown_timing_count;
} AnalysisResult;

typedef struct {
    char *name;
    int cycles;
    int size;
    int instructions;
    bool fixed;
} MacroContext;

typedef struct {
    char *name;
    char *time;
    int size;
} MacroTiming;

static TimingEntry *entries;
static size_t entry_count;
static char **known_macros;
static size_t known_macro_count;
static char **labels;
static size_t label_count;
static char **symbol_names;
static int *symbol_values;
static size_t symbol_count;
static MacroTiming *macro_timings;
static size_t macro_timing_count;
static char *condition_codes;

static void *xrealloc(void *ptr, size_t size) {
    void *result = realloc(ptr, size ? size : 1);
    if (!result) {
        perror("realloc");
        exit(2);
    }
    return result;
}

static bool known_opcode(const char *value) {
    for (size_t i = 0; i < entry_count; i++)
        if (!strcasecmp(entries[i].opcode, value)) return true;
    return false;
}

static char *xstrdup(const char *value) {
    char *result = strdup(value ? value : "");
    if (!result) {
        perror("strdup");
        exit(2);
    }
    return result;
}

static char *format_string(const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0) {
        va_end(args);
        return xstrdup("");
    }
    char *result = malloc((size_t)length + 1);
    if (!result) {
        perror("malloc");
        exit(2);
    }
    vsnprintf(result, (size_t)length + 1, format, args);
    va_end(args);
    return result;
}

static char *trim(char *value) {
    while (isspace((unsigned char)*value)) value++;
    char *end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return value;
}

static char *lower_copy(const char *value) {
    char *result = xstrdup(value);
    for (char *p = result; *p; p++) *p = (char)tolower((unsigned char)*p);
    return result;
}

static bool regex_match(const char *pattern, const char *value, regmatch_t *matches,
                        size_t match_count) {
    regex_t regex;
    if (regcomp(&regex, pattern, REG_EXTENDED | REG_ICASE) != 0) return false;
    bool result = regexec(&regex, value, match_count, matches, 0) == 0;
    regfree(&regex);
    return result;
}

static char *regex_group(const char *pattern, const char *value, int group) {
    regmatch_t matches[4];
    if (!regex_match(pattern, value, matches, 4) || matches[group].rm_so < 0) return NULL;
    return strndup(value + matches[group].rm_so,
                   (size_t)(matches[group].rm_eo - matches[group].rm_so));
}

static bool string_in(char **values, size_t count, const char *value) {
    for (size_t i = 0; i < count; i++)
        if (strcasecmp(values[i], value) == 0) return true;
    return false;
}

static void add_string(char ***values, size_t *count, const char *value) {
    if (string_in(*values, *count, value)) return;
    *values = xrealloc(*values, (*count + 1) * sizeof(**values));
    (*values)[(*count)++] = xstrdup(value);
}

static void add_issue(AnalysisResult *result, int line, const char *message) {
    result->issues = xrealloc(result->issues, (result->issue_count + 1) * sizeof(*result->issues));
    result->issues[result->issue_count].line_number = line;
    result->issues[result->issue_count].message = xstrdup(message);
    result->issue_count++;
}

static void add_unknown_timing(AnalysisResult *result, const char *label) {
    for (size_t i = 0; i < result->unknown_timing_count; i++) {
        if (!strcasecmp(result->unknown_timings[i].label, label)) {
            result->unknown_timings[i].count++;
            return;
        }
    }
    result->unknown_timings = xrealloc(result->unknown_timings,
        (result->unknown_timing_count + 1) * sizeof(*result->unknown_timings));
    result->unknown_timings[result->unknown_timing_count++] =
        (UnknownTiming){.label=lower_copy(label), .count=1};
}

static void add_timing_bounds(AnalysisResult *result, const char *timing) {
    char *copy = xstrdup(timing);
    char *save = NULL;
    int minimum = INT_MAX, maximum = INT_MIN;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        char *end = NULL;
        long value = strtol(part, &end, 10);
        if (!*part || !end || *end || value < 0 || value > INT_MAX) continue;
        if ((int)value < minimum) minimum = (int)value;
        if ((int)value > maximum) maximum = (int)value;
    }
    if (minimum != INT_MAX) {
        result->timing_min += minimum;
        result->timing_max += maximum;
    }
    free(copy);
}

static void print_timing_summary(int timing_min, int timing_max,
                                 const UnknownTiming *unknown_timings, size_t count) {
    printf("Timings (no loop)  min:%d NOPs max:%d NOPs", timing_min, timing_max);
    for (size_t i = 0; i < count; i++) {
        if (unknown_timings[i].count > 1)
            printf(" + %d x %s", unknown_timings[i].count, unknown_timings[i].label);
        else
            printf(" + %s", unknown_timings[i].label);
    }
    printf("\n");
}

static char *source_root(const char *argv0) {
    char resolved[PATH_MAX];
    if (!realpath(argv0, resolved)) {
        if (!getcwd(resolved, sizeof(resolved))) return xstrdup(".");
        char *candidate = format_string("%s/%s", resolved, argv0);
        if (!realpath(candidate, resolved)) {
            free(candidate);
            return xstrdup(".");
        }
        free(candidate);
    }
    char *slash = strrchr(resolved, '/');
    if (slash) *slash = '\0';
    char *table = format_string("%s/input/Instruction.csv", resolved);
    if (access(table, R_OK) == 0) {
        free(table);
        return xstrdup(resolved);
    }
    free(table);
    slash = strrchr(resolved, '/');
    if (slash) *slash = '\0';
    return xstrdup(resolved);
}

static void load_tables(const char *root) {
    char *path = format_string("%s/input/Instruction.csv", root);
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "Error: cannot open timing table: %s\n", path);
        exit(2);
    }
    free(path);
    char *line = NULL;
    size_t capacity = 0;
    if (getline(&line, &capacity, file) < 0) {
        fprintf(stderr, "Error: timing table is empty\n");
        exit(2);
    }
    while (getline(&line, &capacity, file) >= 0) {
        char *instruction = strtok(line, ";\r\n");
        char *time = strtok(NULL, ";\r\n");
        char *size_text = strtok(NULL, ";\r\n");
        if (!instruction || !time || !size_text) continue;
        instruction = trim(instruction);
        time = trim(time);
        size_text = trim(size_text);
        if (!*instruction || !*time || !*size_text) continue;
        char *lower = lower_copy(instruction);
        char *space = strpbrk(lower, " \t");
        if (space) *space = '\0';
        entries = xrealloc(entries, (entry_count + 1) * sizeof(*entries));
        entries[entry_count++] = (TimingEntry){
            .instruction = xstrdup(instruction),
            .opcode = lower,
            .operands = xstrdup(strpbrk(instruction, " \t") ?
                                trim(strpbrk(instruction, " \t")) : ""),
            .time = xstrdup(time),
            .size = atoi(size_text)
        };
    }
    free(line);
    fclose(file);

    path = format_string("%s/input/subtitutions.csv", root);
    file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "Error: cannot open substitutions table: %s\n", path);
        exit(2);
    }
    free(path);
    line = NULL;
    capacity = 0;
    if (getline(&line, &capacity, file) < 0) {
        fprintf(stderr, "Error: substitutions table is empty\n");
        exit(2);
    }
    while (getline(&line, &capacity, file) >= 0) {
        char *key = strtok(line, ";\r\n");
        char *linked = strtok(NULL, ";\r\n");
        if (!key || !linked) continue;
        key = trim(key);
        linked = trim(linked);
        if (strcasecmp(key, "cc") == 0) {
            free(condition_codes);
            condition_codes = lower_copy(linked);
        }
    }
    free(line);
    fclose(file);
}

static char *split_comment(const char *line, char **comment) {
    char quote = '\0';
    bool escaped = false;
    for (size_t i = 0; line[i]; i++) {
        char c = line[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (quote) {
            if (c == quote) quote = '\0';
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == ';') {
            *comment = xstrdup(line + i);
            return strndup(line, i);
        }
    }
    *comment = NULL;
    return xstrdup(line);
}

static char **split_values(const char *text, size_t *count) {
    char **values = NULL;
    *count = 0;
    size_t start = 0;
    int depth = 0;
    char quote = '\0';
    bool escaped = false;
    size_t length = strlen(text);
    for (size_t i = 0; i <= length; i++) {
        char c = text[i];
        if (i == length || (c == ',' && depth == 0 && !quote)) {
            char *part = strndup(text + start, i - start);
            char *clean = xstrdup(trim(part));
            free(part);
            if (*clean) {
                values = xrealloc(values, (*count + 1) * sizeof(*values));
                values[(*count)++] = clean;
            } else free(clean);
            start = i + 1;
            continue;
        }
        if (escaped) {
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (quote) {
            if (c == quote) quote = '\0';
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '(') {
            depth++;
        } else if (c == ')' && depth > 0) {
            depth--;
        }
    }
    return values;
}

static bool is_integer(const char *value, long *number) {
    char *copy = xstrdup(trim((char *)value));
    char *p = copy;
    if (*p == '#') p++;
    int base = 10;
    bool suffix = false;
    size_t length = strlen(p);
    if (length && (p[length - 1] == 'h' || p[length - 1] == 'H')) {
        p[length - 1] = '\0'; base = 16; suffix = true;
    } else if (length && (p[length - 1] == 'o' || p[length - 1] == 'O')) {
        p[length - 1] = '\0'; base = 8; suffix = true;
    } else if (length && (p[length - 1] == 'b' || p[length - 1] == 'B')) {
        p[length - 1] = '\0'; base = 2; suffix = true;
    }
    if (*p == '$') { p++; base = 16; }
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; base = 16; }
    errno = 0;
    char *end = NULL;
    long result = strtol(p, &end, base);
    bool ok = *p && end && !*end && errno != ERANGE;
    (void)suffix;
    if (ok && number) *number = result;
    free(copy);
    return ok;
}

static bool is_symbol(const char *value) {
    if (!isalpha((unsigned char)value[0]) && !strchr("_.$?", value[0])) return false;
    for (const char *p = value + 1; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr("_.$?", *p)) return false;
    return true;
}

static char *normalize_symbol(const char *input) {
    char *symbol = lower_copy(trim((char *)input));
    if (!strcmp(symbol, "k") || !strcmp(symbol, "s") || !strcmp(symbol, "pp") ||
        !strcmp(symbol, "qq") || !strcmp(symbol, "of") || !strcmp(symbol, "n") ||
        !strcmp(symbol, "nn") || !strcmp(symbol, "bt") || !strcmp(symbol, "cc")) return symbol;
    if (!strcmp(symbol, "ixh") || !strcmp(symbol, "ixl") ||
        !strcmp(symbol, "iyh") || !strcmp(symbol, "iyl")) return (free(symbol), xstrdup("s"));
    if (!strcmp(symbol, "ix") || !strcmp(symbol, "iy")) return (free(symbol), xstrdup("qq"));
    if (!strcmp(symbol, "bc") || !strcmp(symbol, "de") || !strcmp(symbol, "hl"))
        return (free(symbol), xstrdup("pp"));
    if (!strcmp(symbol, "nz") || !strcmp(symbol, "z") || !strcmp(symbol, "nc") ||
        !strcmp(symbol, "c") || !strcmp(symbol, "po") || !strcmp(symbol, "pe") ||
        !strcmp(symbol, "p") || !strcmp(symbol, "m")) return (free(symbol), xstrdup("cc"));
    if (strlen(symbol) == 1 && strchr("abcdehl", symbol[0])) return (free(symbol), xstrdup("k"));
    if (is_integer(symbol, NULL)) return (free(symbol), xstrdup("n"));
    if (is_symbol(symbol)) {
        if (strlen(symbol) > 1 && strcmp(symbol, "sp") && strcmp(symbol, "af") &&
            strcmp(symbol, "ix") && strcmp(symbol, "iy")) return (free(symbol), xstrdup("k"));
    }
    return symbol;
}

static char *normalize_operand(const char *input) {
    char *value = lower_copy(trim((char *)input));
    size_t length = strlen(value);
    if (length >= 2 && value[0] == '(' && value[length - 1] == ')') {
        char *inside = strndup(value + 1, length - 2);
        char *plus = strchr(inside, '+');
        char *minus = strchr(inside, '-');
        char *result;
        if ((plus || minus) && (!strncmp(inside, "ix", 2) || !strncmp(inside, "iy", 2) ||
                                !strncmp(inside, "qq", 2))) {
            char *sign = plus ? plus : minus;
            *sign = '\0';
            char *left = trim(inside);
            char *norm = normalize_symbol(left);
            result = format_string("(%s%c%s)", norm, plus ? '+' : '-', !strcmp(norm, "qq") ? "of" : "d");
            free(norm);
        } else if (plus || minus) {
            result = xstrdup("(nn)");
        } else {
            char *norm = normalize_symbol(inside);
            result = format_string("(%s)", norm);
            free(norm);
        }
        free(inside);
        free(value);
        return result;
    }
    char *result = normalize_symbol(value);
    free(value);
    return result;
}

static bool concrete_operand(const char *value) {
    static const char *registers[] = {
        "a","b","c","e","h","l","i","r","bc","de","hl","af","sp",
        "ix","iy","ixh","ixl","iyh","iyl","af'","(bc)","(de)","(hl)",
        "(sp)","(ix)","(iy)"
    };
    for (size_t i = 0; i < sizeof(registers) / sizeof(registers[0]); i++)
        if (!strcasecmp(value, registers[i])) return true;
    return false;
}

static bool is_register_name(const char *value) {
    static const char *registers[] = {
        "a","b","c","d","e","h","l","i","r","ix","iy","sp","bc","de","hl","af"
    };
    for (size_t i = 0; i < sizeof(registers) / sizeof(registers[0]); i++)
        if (!strcmp(value, registers[i])) return true;
    return false;
}

static bool symbolic_address(const char *value) {
    char *copy = lower_copy(trim((char *)value));
    if (*copy == '#') memmove(copy, copy + 1, strlen(copy));
    char *plus = strchr(copy, '+');
    if (!plus) { free(copy); return false; }
    *plus = '\0';
    char *left = trim(copy), *right = trim(plus + 1);
    long ignored;
    bool left_sym = is_symbol(left) && !is_register_name(left);
    bool right_sym = is_symbol(right) && !is_register_name(right);
    bool result = (left_sym && is_integer(right, &ignored)) ||
                  (right_sym && is_integer(left, &ignored));
    free(copy);
    return result;
}

static bool symbolic_match(const char *templ, const char *source) {
    if (strcmp(templ, "cc") && strcmp(templ, "bt") && strcmp(templ, "nn") &&
        strcmp(templ, "n") && strcmp(templ, "b") && strcmp(templ, "d") &&
        strcmp(templ, "(nn)")) return false;
    char *value = lower_copy(trim((char *)source));
    long number;
    bool result = false;
    if (!strcmp(templ, "cc")) {
        char *cc = xstrdup(condition_codes);
        char *save = NULL;
        for (char *part = strtok_r(cc, ",", &save); part; part = strtok_r(NULL, ",", &save))
            if (!strcasecmp(trim(part), value)) result = true;
        free(cc);
    } else if (!strcmp(templ, "bt")) {
        result = (is_integer(value, &number) && number >= 0 && number <= 7) || is_symbol(value);
    } else if (!strcmp(templ, "nn") && symbolic_address(value)) {
        result = true;
    } else {
        if (!strcmp(templ, "n") && (*value == '<' || *value == '>')) {
            char *p = value + 1;
            if (*p == '#') p++;
            memmove(value, p, strlen(p) + 1);
        }
        if (is_integer(value, &number)) result = true;
        else {
            if (*templ == 'n' || *templ == 'b' || *templ == 'd') {
                while (*value == '#') memmove(value, value + 1, strlen(value));
            }
            if (is_symbol(value) && !is_register_name(value)) result = true;
            if (!strcmp(templ, "(nn)") && value[0] == '(') {
                size_t len = strlen(value);
                if (len > 2 && value[len - 1] == ')') {
                    value[len - 1] = '\0';
                    result = is_symbol(value + 1) && !is_register_name(value + 1);
                }
            }
        }
    }
    free(value);
    return result;
}

static bool match_instruction(const char *opcode, const char *operand_text, TimingEntry **matched) {
    size_t source_count;
    char **source = split_values(operand_text, &source_count);
    char *operands = xstrdup(operand_text);
    if ((!strcasecmp(opcode, "and") || !strcasecmp(opcode, "or") ||
         !strcasecmp(opcode, "xor") || !strcasecmp(opcode, "sub") ||
         !strcasecmp(opcode, "cp")) && source_count == 2 && !strcasecmp(source[0], "a")) {
        free(operands);
        operands = xstrdup(source[1]);
        free(source[0]); free(source[1]); free(source);
        source = split_values(operands, &source_count);
    }
    char **normalized = calloc(source_count ? source_count : 1, sizeof(*normalized));
    for (size_t i = 0; i < source_count; i++) normalized[i] = normalize_operand(source[i]);

    for (size_t e = 0; e < entry_count; e++) {
        TimingEntry *candidate = &entries[e];
        if (strcasecmp(candidate->opcode, opcode)) continue;
        size_t template_count;
        char **raw_templates = split_values(candidate->operands, &template_count);
        if (template_count != source_count) {
            for (size_t i = 0; i < template_count; i++) free(raw_templates[i]);
            free(raw_templates);
            continue;
        }
        bool good = true;
        for (size_t i = 0; i < template_count; i++) {
            char *templ = lower_copy(raw_templates[i]);
            char *actual = (!strcmp(templ, "b") || !strcmp(templ, "cc") ||
                !strcmp(templ, "d") || !strcmp(templ, "n") || !strcmp(templ, "nn") ||
                !strcmp(templ, "of") || !strcmp(templ, "bt"))
                ? xstrdup(templ) : normalize_operand(raw_templates[i]);
            if (strstr(templ, "+k)") || strstr(templ, "-k)")) {
                char *pos = strrchr(actual, 'k');
                if (pos) *pos = 'd';
            }
            if (concrete_operand(templ) && strcasecmp(templ, source[i])) good = false;
            if (strcasecmp(actual, normalized[i]) &&
                !symbolic_match(templ, source[i]) &&
                !(strstr(templ, "(qq+") && strstr(normalized[i], "(qq+") &&
                  (symbolic_match("d", source[i])))) good = false;
            free(templ);
            free(actual);
            free(raw_templates[i]);
        }
        free(raw_templates);
        if (good) {
            *matched = candidate;
            for (size_t i = 0; i < source_count; i++) free(normalized[i]);
            free(normalized);
            for (size_t i = 0; i < source_count; i++) free(source[i]);
            free(source);
            free(operands);
            return true;
        }
    }
    for (size_t i = 0; i < source_count; i++) { free(normalized[i]); free(source[i]); }
    free(normalized); free(source); free(operands);
    return false;
}

static char *parse_statement(const char *line, bool *label_only) {
    char *comment = NULL;
    char *code = split_comment(line, &comment);
    free(comment);
    char *stripped = trim(code);
    *label_only = false;
    if (!*stripped || *stripped == '#') { free(code); return NULL; }
    char *label = regex_group("^([A-Za-z_.$?][A-Za-z0-9_.$?]*)[ \t]*:{1,2}[ \t]*(.*)$", stripped, 1);
    if (label) {
        char *remainder = regex_group("^([A-Za-z_.$?][A-Za-z0-9_.$?]*)[ \t]*:{1,2}[ \t]*(.*)$", stripped, 2);
        free(label);
        if (!remainder || !*trim(remainder)) {
            free(remainder); free(code); *label_only = true; return NULL;
        }
        memmove(stripped, remainder, strlen(remainder) + 1);
        free(remainder);
    } else if (line[0] && !isspace((unsigned char)line[0])) {
        char *first = strndup(stripped, strcspn(stripped, " \t"));
        char *rest = stripped + strlen(first);
        while (isspace((unsigned char)*rest)) rest++;
        if (!known_opcode(first) && !strchr(first, '.') &&
            !regex_match("^[a-z][a-z0-9]*_{2,3}.", first, NULL, 0)) {
            if (*rest) memmove(stripped, rest, strlen(rest) + 1);
            else { free(first); free(code); *label_only = true; return NULL; }
        }
        free(first);
    }
    char *opcode = regex_group("^([A-Za-z_.][A-Za-z0-9_.$]*)([ \t]+.*)?$", stripped, 1);
    char *operands = regex_group("^[A-Za-z_.][A-Za-z0-9_.$]*[ \t]+(.*)$", stripped, 1);
    free(code);
    if (!opcode) { free(operands); return NULL; }
    char *lower = lower_copy(opcode);
    free(opcode);
    char *result = format_string("%s\n%s", lower, operands ? trim(operands) : "");
    free(lower); free(operands);
    return result;
}

static bool directive(const char *opcode) { return opcode[0] == '.' || opcode[0] == '#'; }

static int parse_symbol(const char *expression, bool *found) {
    char *value = xstrdup(trim((char *)expression));
    size_t len = strlen(value);
    if (len >= 2 && value[0] == '(' && value[len - 1] == ')') {
        memmove(value, value + 1, len - 2);
        value[len - 2] = '\0';
        trim(value);
    }
    long numeric;
    if (is_integer(value, &numeric)) { *found = true; free(value); return (int)numeric; }
    for (size_t i = 0; i < symbol_count; i++) {
        if (!strcasecmp(symbol_names[i], value)) {
            *found = true; free(value); return symbol_values[i];
        }
    }
    *found = false;
    free(value);
    return 0;
}

static int quoted_string_size(const char *value) {
    size_t len = strlen(value);
    if (len < 2 || (value[0] != '"' && value[0] != '\'') || value[len - 1] != value[0]) return -1;
    int size = 0;
    for (size_t i = 1; i + 1 < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c == '\\') {
            if (++i + 1 >= len) return -1;
            c = (unsigned char)value[i];
            if (c == '\n') continue;
            if (c == 'x' || c == 'u' || c == 'U') {
                int digits = c == 'x' ? 2 : c == 'u' ? 4 : 8;
                if (i + (size_t)digits + 1 >= len) return -1;
                uint32_t codepoint = 0;
                for (int j = 1; j <= digits; j++) {
                    unsigned char hex = (unsigned char)value[i + (size_t)j];
                    if (!isxdigit(hex)) return -1;
                    codepoint = codepoint * 16 + (uint32_t)(isdigit(hex) ? hex - '0' :
                        tolower(hex) - 'a' + 10);
                }
                if (codepoint > 255) return -1;
                i += (size_t)digits;
            } else if (c >= '0' && c <= '7') {
                unsigned int codepoint = c - '0';
                int digits = 1;
                while (digits < 3 && i + 1 < len - 1 &&
                       value[i + 1] >= '0' && value[i + 1] <= '7') {
                    codepoint = codepoint * 8 + (unsigned int)(value[++i] - '0');
                    digits++;
                }
                if (codepoint > 255) return -1;
            } else if (!strchr("\\'\"abfnrtv", c)) {
                size++;
            }
        } else if (c >= 0x80) {
            uint32_t codepoint;
            int extra;
            if ((c & 0xe0) == 0xc0) { codepoint = c & 0x1f; extra = 1; }
            else if ((c & 0xf0) == 0xe0) { codepoint = c & 0x0f; extra = 2; }
            else if ((c & 0xf8) == 0xf0) { codepoint = c & 0x07; extra = 3; }
            else return -1;
            if (i + (size_t)extra + 1 >= len) return -1;
            for (int j = 1; j <= extra; j++) {
                unsigned char next = (unsigned char)value[i + (size_t)j];
                if ((next & 0xc0) != 0x80) return -1;
                codepoint = (codepoint << 6) | (next & 0x3f);
            }
            if (codepoint > 255) return -1;
            i += (size_t)extra;
        }
        size++;
    }
    return size;
}

static bool is_data_directive(const char *opcode) {
    static const char *directives[] = {
        ".ascii",".ascis",".asciz",".blkb",".blkw",".byte",".db",".ds",".dw",
        ".fcb",".fcc",".fdb",".rmb",".str",".strs",".strz",".sz",".word"
    };
    for (size_t i = 0; i < sizeof(directives)/sizeof(directives[0]); i++)
        if (!strcmp(opcode, directives[i])) return true;
    return false;
}

static int data_directive_size(const char *opcode, const char *operands, bool *known) {
    size_t count;
    char **values = split_values(operands, &count);
    int size = 0;
    *known = true;
    if (!strcmp(opcode, ".db") || !strcmp(opcode, ".byte") || !strcmp(opcode, ".fcb")) {
        for (size_t i = 0; i < count; i++) {
            int n = quoted_string_size(values[i]);
            size += n >= 0 ? n : 1;
        }
    } else if (!strcmp(opcode, ".dw") || !strcmp(opcode, ".word") || !strcmp(opcode, ".fdb")) {
        size = (int)count * 2;
    } else if (!strcmp(opcode, ".ds") || !strcmp(opcode, ".rmb") || !strcmp(opcode, ".blkb")) {
        if (count) {
            bool found;
            size = parse_symbol(values[0], &found);
            if (!found) *known = false;
        }
    } else if (!strcmp(opcode, ".blkw")) {
        if (count) {
            bool found;
            size = parse_symbol(values[0], &found) * 2;
            if (!found) *known = false;
        }
    } else if (!strcmp(opcode, ".ascii") || !strcmp(opcode, ".ascis") ||
               !strcmp(opcode, ".asciz") || !strcmp(opcode, ".str") ||
               !strcmp(opcode, ".strs") || !strcmp(opcode, ".strz") ||
               !strcmp(opcode, ".fcc") || !strcmp(opcode, ".sz")) {
        for (size_t i = 0; i < count; i++) {
            int n = quoted_string_size(values[i]);
            if (n < 0) *known = false;
            else size += n;
        }
        if (!strcmp(opcode, ".asciz") || !strcmp(opcode, ".strz") || !strcmp(opcode, ".sz")) size++;
    }
    for (size_t i = 0; i < count; i++) free(values[i]);
    free(values);
    return size;
}

static char *comment_value(const char *line, char marker, bool *found) {
    char *comment = NULL;
    char *code = split_comment(line, &comment);
    free(code);
    char pattern[32];
    if (marker == '[') snprintf(pattern, sizeof(pattern), "^;{1,2}[ \t]*\\[([^]]+)\\]");
    else snprintf(pattern, sizeof(pattern), "^;{1,2}[ \t]*\\(([0-9]+)\\)");
    char *value = comment ? regex_group(pattern, comment, 1) : NULL;
    free(comment);
    *found = value != NULL;
    if (value) {
        char *clean = xstrdup(trim(value));
        free(value);
        return clean;
    }
    return NULL;
}

static char *update_comment(const char *line, const char *value, char marker, int column) {
    char *comment = NULL;
    char *code = split_comment(line, &comment);
    if (comment) {
        char *marker_text = regex_group("^(;{1,2})", comment, 1);
        if (marker_text) {
            const char *tail = comment + strlen(marker_text);
            char *body = xstrdup(tail);
            regex_t re;
            if (marker == '[')
                regcomp(&re, "^[ \t]*\\[[^]]+\\][ \t]*", REG_EXTENDED);
            else
                regcomp(&re, "^[ \t]*\\([0-9]+\\)[ \t]*", REG_EXTENDED);
            regmatch_t m;
            if (regexec(&re, body, 1, &m, 0) == 0) {
                memmove(body, body + m.rm_eo, strlen(body + m.rm_eo) + 1);
                if (!*body || *body != ' ') {
                    char *with_space = format_string(" %s", body);
                    free(body);
                    body = with_space;
                }
            }
            regfree(&re);
            char *result = marker == '['
                ? format_string("%s%s [%s]%s", code, marker_text, value, body)
                : format_string("%s%s (%s)%s", code, marker_text, value, body);
            free(marker_text); free(body); free(comment); free(code);
            return result;
        }
    }
    size_t end = strlen(line);
    while (end && isspace((unsigned char)line[end - 1])) end--;
    int padding = column - (int)end;
    if (padding < 1) padding = 1;
    char *result = marker == '['
        ? format_string("%.*s%*s;; [%s]", (int)end, line, padding, "", value)
        : format_string("%.*s%*s;; (%s)", (int)end, line, padding, "", value);
    free(comment); free(code);
    return result;
}

static bool resolve_macro_instruction(char **opcode, char **operands, TimingEntry **matched) {
    regmatch_t m[4];
    if (!regex_match("^([a-z][a-z0-9]*)(_+)(.+)$", *opcode, m, 4)) return false;
    int underscores = (int)(m[2].rm_eo - m[2].rm_so);
    if (underscores != 2 && underscores != 3) return false;
    char *instruction = strndup(*opcode + m[1].rm_so, (size_t)(m[1].rm_eo - m[1].rm_so));
    char *encoded = xstrdup(*opcode + m[2].rm_eo);
    char *decoded = NULL;
    if (underscores == 2) {
        char *save = NULL;
        for (char *part = strtok_r(encoded, "_", &save); part; part = strtok_r(NULL, "_", &save)) {
            decoded = decoded ? format_string("%s, %s", decoded, part) : xstrdup(part);
        }
    } else {
        char *save = NULL;
        int index = 0;
        for (char *part = strtok_r(encoded, "__", &save); part; part = strtok_r(NULL, "__", &save)) {
            char *token = index++ == 0 ? format_string("(%s)", part) : xstrdup(part);
            decoded = decoded ? format_string("%s, %s", decoded, token) : xstrdup(token);
            free(token);
        }
    }
    if (*operands && **operands) {
        decoded = decoded ? format_string("%s, %s", decoded, *operands) : xstrdup(*operands);
    }
    bool found = decoded && match_instruction(instruction, decoded, matched);
    if (found) {
        *opcode = instruction;
        *operands = decoded;
    } else { free(instruction); free(decoded); }
    free(encoded);
    return found;
}

static char *project_relative(const char *path, const char *root) {
    size_t n = strlen(root);
    if (!strncmp(path, root, n) && path[n] == '/') return xstrdup(path + n + 1);
    return xstrdup(path);
}

static char **collect_files(const char *directory, size_t *count) {
    char **result = NULL;
    *count = 0;
    DIR *dir = opendir(directory);
    if (!dir) return NULL;
    struct dirent *item;
    while ((item = readdir(dir))) {
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        char *path = format_string("%s/%s", directory, item->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            size_t child_count;
            char **children = collect_files(path, &child_count);
            for (size_t i = 0; i < child_count; i++) {
                result = xrealloc(result, (*count + 1) * sizeof(*result));
                result[(*count)++] = children[i];
            }
            free(children);
        } else if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            const char *dot = strrchr(item->d_name, '.');
            if (dot && (!strcasecmp(dot, ".s") || !strcasecmp(dot, ".asm"))) {
                result = xrealloc(result, (*count + 1) * sizeof(*result));
                result[(*count)++] = path;
                continue;
            }
        }
        free(path);
    }
    closedir(dir);
    return result;
}

static int compare_paths(const void *a, const void *b) {
    const char *left = *(const char **)a, *right = *(const char **)b;
    char *l = lower_copy(left), *r = lower_copy(right);
    int result = strcmp(l, r);
    free(l); free(r);
    return result;
}

static AnalysisResult analyze_file(const char *path, bool update, bool modify,
                                   bool data_comments, int debug, int verbose) {
    AnalysisResult result = {0};
    FILE *file = fopen(path, "rb");
    if (!file) {
        char *message = format_string("Error: cannot read input file: %s", path);
        fprintf(stderr, "%s\n", message);
        free(message);
        return result;
    }
    char **raw_lines = NULL;
    size_t line_count = 0;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    bool crlf = false;
    bool ended_newline = false;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        if (length >= 2 && line[length - 2] == '\r' && line[length - 1] == '\n') crlf = true;
        ended_newline = length > 0 && line[length - 1] == '\n';
        if (length && line[length - 1] == '\n') line[--length] = '\0';
        if (length && line[length - 1] == '\r') line[--length] = '\0';
        raw_lines = xrealloc(raw_lines, (line_count + 1) * sizeof(*raw_lines));
        raw_lines[line_count++] = xstrdup(line);
    }
    free(line);
    fclose(file);
    const char *newline = crlf ? "\r\n" : "\n";
    known_macro_count = label_count = symbol_count = macro_timing_count = 0;

    for (size_t i = 0; i < line_count; i++) {
        char *comment = NULL;
        char *code = split_comment(raw_lines[i], &comment);
        free(comment);
        char *macro = regex_group("^[ \t]*\\.macro[ \t]+([A-Za-z_.$?][A-Za-z0-9_.$?]*)", code, 1);
        if (macro) { char *low = lower_copy(macro); add_string(&known_macros, &known_macro_count, low); free(low); free(macro); }
        char *label = regex_group("^[ \t]*([A-Za-z_.$?][A-Za-z0-9_.$?]*)[ \t]*:{1,2}", code, 1);
        if (label) { char *low = lower_copy(label); add_string(&labels, &label_count, low); free(low); free(label); }
        char *name = regex_group("^[ \t]*([A-Za-z_.$?][A-Za-z0-9_.$?]*)[ \t]*=", code, 1);
        if (name) {
            char *expr = regex_group("^[ \t]*[A-Za-z_.$?][A-Za-z0-9_.$?]*[ \t]*=[ \t]*(.*?)$", code, 1);
            bool found = false;
            int value = expr ? parse_symbol(expr, &found) : 0;
            if (found) {
                symbol_names = xrealloc(symbol_names, (symbol_count + 1) * sizeof(*symbol_names));
                symbol_values = xrealloc(symbol_values, (symbol_count + 1) * sizeof(*symbol_values));
                symbol_names[symbol_count] = lower_copy(name);
                symbol_values[symbol_count++] = value;
            }
            free(name); free(expr);
        }
        free(code);
    }

    int *comment_columns = calloc(line_count ? line_count : 1, sizeof(*comment_columns));
    size_t column_count = 0;
    for (size_t i = 0; i < line_count; i++) {
        char *comment = NULL;
        char *code = split_comment(raw_lines[i], &comment);
        char *trimmed_code = xstrdup(code);
        if (comment && *trim(trimmed_code)) comment_columns[column_count++] = (int)strlen(code);
        free(trimmed_code);
        free(comment); free(code);
    }
    int comment_column = 0, best = 0;
    for (size_t i = 0; i < column_count; i++) {
        int count = 0;
        for (size_t j = 0; j < column_count; j++) if (comment_columns[i] == comment_columns[j]) count++;
        if (count > best) { best = count; comment_column = comment_columns[i]; }
    }
    free(comment_columns);

    result.lines = calloc(line_count ? line_count : 1, sizeof(*result.lines));
    result.line_count = line_count;
    int repeat = 1;
    typedef struct { char *type; int previous; } Block;
    Block blocks[256];
    int block_count = 0;
    MacroContext macros[256];
    int macro_count = 0;

    for (size_t i = 0; i < line_count; i++) {
        ProcessedLine *item = &result.lines[i];
        item->original = xstrdup(raw_lines[i]);
        item->updated = xstrdup(raw_lines[i]);
        item->line_number = (int)i + 1;
        bool label_only = false;
        char *statement = parse_statement(raw_lines[i], &label_only);
        if (!statement) continue;
        char *separator = strchr(statement, '\n');
        *separator = '\0';
        char *opcode = statement;
        char *operands = separator + 1;

        if (!strcmp(opcode, ".macro")) {
            if (block_count < 256) blocks[block_count++] = (Block){"macro", repeat};
            char *name = strndup(operands, strcspn(operands, " \t"));
            if (macro_count < 256)
                macros[macro_count++] = (MacroContext){.name=lower_copy(name), .fixed=*name != '\0'};
            free(name); free(statement); continue;
        }
        if (!strcmp(opcode, ".rept") || !strcmp(opcode, ".irp") || !strcmp(opcode, ".irpc")) {
            if (!strcmp(opcode, ".rept")) {
                char *count_text = strndup(operands, strcspn(operands, " \t"));
                bool found = false;
                int count = parse_symbol(count_text, &found);
                if (!found) {
                    add_issue(&result, (int)i + 1, format_string("Cannot determine repeat count '%s'", operands));
                    if (macro_count) macros[macro_count - 1].fixed = false;
                    count = 1;
                }
                if (block_count < 256) blocks[block_count++] = (Block){"rept", repeat};
                repeat *= count > 0 ? count : 0;
                free(count_text);
            } else {
                if (macro_count) macros[macro_count - 1].fixed = false;
                if (block_count < 256) blocks[block_count++] = (Block){"skip", repeat};
            }
            free(statement); continue;
        }
        if (!strcmp(opcode, ".endm") || !strcmp(opcode, ".endr")) {
            if (block_count && (!strcmp(opcode, ".endm") ||
                !strcmp(blocks[block_count - 1].type, "rept") || !strcmp(blocks[block_count - 1].type, "skip"))) {
                char *type = blocks[--block_count].type;
                if (!strcmp(type, "rept")) repeat = blocks[block_count].previous;
                else if (!strcmp(type, "macro") && macro_count) {
                    MacroContext context = macros[--macro_count];
                    if (context.fixed && context.instructions) {
                        macro_timings = xrealloc(macro_timings, (macro_timing_count + 1) * sizeof(*macro_timings));
                        macro_timings[macro_timing_count++] = (MacroTiming){
                            .name=context.name, .time=format_string("%d", context.cycles), .size=context.size
                        };
                    } else free(context.name);
                }
            }
            free(statement); continue;
        }
        bool skip = false;
        for (int b = 0; b < block_count; b++) if (!strcmp(blocks[b].type, "skip")) skip = true;
        if (skip) { free(statement); continue; }

        if (directive(opcode)) {
            if (macro_count) {
                if (is_data_directive(opcode) || !strcmp(opcode, ".if") || !strcmp(opcode, ".ifdef") ||
                    !strcmp(opcode, ".ifndef") || !strcmp(opcode, ".else") || !strcmp(opcode, ".endif"))
                    macros[macro_count - 1].fixed = false;
                free(statement); continue;
            }
            bool known = true;
            int bytes = data_directive_size(opcode, operands, &known);
            if (!known) {
                add_issue(&result, (int)i + 1, format_string("Cannot determine size of %s data", opcode));
            } else if (bytes) {
                item->size = bytes * repeat;
                result.data_size += item->size;
                if (data_comments && is_data_directive(opcode)) {
                    bool has_size;
                    char *old = comment_value(raw_lines[i], '(', &has_size);
                    char expected[32]; snprintf(expected, sizeof(expected), "%d", item->size);
                    if (!has_size || strcmp(old, expected)) {
                        item->planned = true;
                        item->action = xstrdup(has_size ? "correct size" : "add size");
                        result.recommendations++;
                        free(item->updated);
                        item->updated = update_comment(raw_lines[i], expected, '(', comment_column);
                        if (has_size && !update)
                            add_issue(&result, (int)i + 1, format_string("Data size mismatch: found (%s), expected (%s) (use --update to replace)", old, expected));
                    }
                    free(old);
                }
            }
            free(statement); continue;
        }

        TimingEntry *matched = NULL;
        bool decoded = resolve_macro_instruction(&opcode, &operands, &matched);
        MacroTiming *macro_summary = NULL;
        if (!decoded && string_in(known_macros, known_macro_count, opcode)) {
            for (size_t m = 0; m < macro_timing_count; m++)
                if (!strcasecmp(macro_timings[m].name, opcode)) macro_summary = &macro_timings[m];
            if (!macro_summary) {
                if (macro_count) macros[macro_count - 1].fixed = false;
                free(statement); continue;
            }
        } else if (!decoded) {
            match_instruction(opcode, operands, &matched);
        }
        if (!matched && !macro_summary) {
            if (macro_count) {
                macros[macro_count - 1].fixed = false;
                free(statement); continue;
            }
            add_unknown_timing(&result, opcode);
            add_issue(&result, (int)i + 1, format_string("Unknown operator or macro '%s'", opcode));
            free(statement); continue;
        }
        const char *timing = macro_summary ? macro_summary->time : matched->time;
        int bytes = macro_summary ? macro_summary->size : matched->size;
        bool has_timing;
        char *old = comment_value(raw_lines[i], '[', &has_timing);
        bool planned = !has_timing || strcmp(old, timing);
        item->planned = planned;
        item->recommended = xstrdup(timing);
        if (planned) item->action = xstrdup(has_timing ? "correct" : "annotate");
        if (planned) {
            result.recommendations++;
            free(item->updated);
            item->updated = update_comment(raw_lines[i], timing, '[', comment_column);
            if (has_timing && !update)
                add_issue(&result, (int)i + 1, format_string("Timing mismatch: found [%s], expected [%s] (use --update to replace)", old, timing));
        }
        free(old);
        if (macro_count) {
            MacroContext *context = &macros[macro_count - 1];
            if (regex_match("^[0-9]+$", timing, NULL, 0)) {
                context->cycles += atoi(timing) * repeat;
                context->size += bytes * repeat;
                context->instructions += repeat;
            } else context->fixed = false;
            item->size = bytes * repeat;
        } else {
            result.code_size += bytes * repeat;
            item->size = bytes * repeat;
            add_timing_bounds(&result, timing);
        }
        if (!strcmp(opcode, "call") && !macro_count) {
            size_t targets_count;
            char **targets = split_values(operands, &targets_count);
            char *target = targets_count ? lower_copy(targets[targets_count - 1]) : xstrdup("");
            size_t n = strlen(target);
            while (n && (target[0] == '(' || target[0] == ')' || isspace((unsigned char)target[0]))) memmove(target, target + 1, strlen(target));
            n = strlen(target);
            while (n && (target[n - 1] == ')' || target[n - 1] == '(' || isspace((unsigned char)target[n - 1]))) target[--n] = '\0';
            if (*target && !string_in(labels, label_count, target)) {
                add_unknown_timing(&result, target);
                add_issue(&result, (int)i + 1, format_string("Call to unknown timing subroutine '%s'", target));
            }
            for (size_t j = 0; j < targets_count; j++) free(targets[j]);
            free(targets); free(target);
        }
        if (decoded) { free(opcode); free(operands); }
        free(statement);
    }

    if (modify) {
        file = fopen(path, "wb");
        if (!file) {
            fprintf(stderr, "Error: cannot write input file: %s\n", path);
        } else {
            for (size_t i = 0; i < line_count; i++) {
                ProcessedLine *item = &result.lines[i];
                bool old_timing, old_data;
                char *t = comment_value(item->original, '[', &old_timing);
                char *d = comment_value(item->original, '(', &old_data);
                free(t); free(d);
                const char *output = (update || (!old_timing && !old_data)) ? item->updated : item->original;
                fputs(output, file);
                if (i + 1 < line_count || (i + 1 == line_count && ended_newline)) fputs(newline, file);
            }
            fclose(file);
        }
    }

    printf("Analysis: %s\n", path);
    printf("Recommended modifications: %d\n", result.recommendations);
    printf("Global code size: %d bytes\n", result.code_size);
    printf("Global data size: %d bytes\n", result.data_size);
    print_timing_summary(result.timing_min, result.timing_max,
                         result.unknown_timings, result.unknown_timing_count);
    printf("\n");
    if (result.issue_count) {
        printf("Warnings:\n");
        for (size_t i = 0; i < result.issue_count; i++)
            printf("- Line %d: %s\n", result.issues[i].line_number, result.issues[i].message);
        printf("\n");
    }
    if (debug || verbose) {
        for (size_t i = 0; i < result.line_count; i++) {
            ProcessedLine *item = &result.lines[i];
            if (!debug && !item->planned) continue;
            printf("Line %d: size=%d bytes timing=%s action=%s\n",
                   item->line_number, item->size,
                   item->recommended ? format_string("[%s]", item->recommended) : "-",
                   item->action ? item->action : "none");
            if (strcmp(item->original, item->updated))
                printf("Before: %s\nAfter:  %s\n", item->original, item->updated);
        }
    } else if (!modify) {
        for (size_t i = 0; i < result.line_count; i++) {
            ProcessedLine *item = &result.lines[i];
            if (strcmp(item->original, item->updated))
                printf("Before: %s\nAfter:  %s\n", item->original, item->updated);
        }
    }
    for (size_t i = 0; i < line_count; i++) free(raw_lines[i]);
    free(raw_lines);
    return result;
}

static void usage(FILE *out, const char *program) {
    (void)program;
    fprintf(out, "usage: CPCAsmNopator.py [-h] [--update] [--verbose] [--modify] [--data-size]\n");
    fprintf(out, "                        file\n\n");
    fprintf(out, "Annotate Z80 instruction timing comments and report size totals.\n\n");
    fprintf(out, "positional arguments:\n  file           SDAS source file or directory to analyze\n\n");
    fprintf(out, "options:\n  -h, --help     show this help message and exit\n");
    fprintf(out, "  --update, -u   Replace incorrect timing comments\n");
    fprintf(out, "  --verbose, -v  Print recommended modifications\n");
    fprintf(out, "  --modify, -m   Write annotations to the input source file\n");
    fprintf(out, "  --data-size    Add or update byte-size comments on data directives\n");
}

int main(int argc, char **argv) {
    char *root = source_root(argv[0]);
    load_tables(root);
    bool update = false, modify = false, data_comments = false;
    int debug = 0, verbose = 0;
    const char *input = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout, argv[0]); free(root); return 0;
        } else if (!strcmp(argv[i], "--update") || !strcmp(argv[i], "-u")) update = true;
        else if (!strcmp(argv[i], "--modify") || !strcmp(argv[i], "-m")) modify = true;
        else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--debug")) debug = 1;
        else if (!strcmp(argv[i], "--data-size")) data_comments = true;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: unrecognized argument: %s\n", argv[i]); free(root); return 2;
        } else if (!input) input = argv[i];
        else {
            fprintf(stderr, "Error: unexpected argument: %s\n", argv[i]); free(root); return 2;
        }
    }
    if (!input) {
        usage(stderr, argv[0]); free(root); return 2;
    }
    struct stat st;
    if (stat(input, &st) != 0) {
        fprintf(stderr, "Error: input file not found: %s\n", input); free(root); return 2;
    }
    int status = 0;
    if (S_ISDIR(st.st_mode)) {
        size_t count;
        char **files = collect_files(input, &count);
        qsort(files, count, sizeof(*files), compare_paths);
        if (!count) {
            fprintf(stderr, "Error: no .s or .asm source files found in: %s\n", input);
            status = 2;
        } else {
            int recommendations = 0, code_size = 0, data_size = 0, issues = 0, files_with_issues = 0;
            int timing_min = 0, timing_max = 0;
            UnknownTiming *unknown = NULL;
            size_t unknown_count = 0;
            for (size_t i = 0; i < count; i++) {
                char *relative = project_relative(files[i], input);
                printf("=== File %zu/%zu: %s ===\n", i + 1, count, relative);
                AnalysisResult result = analyze_file(files[i], update, modify, data_comments, debug, verbose);
                recommendations += result.recommendations;
                code_size += result.code_size; data_size += result.data_size;
                timing_min += result.timing_min;
                timing_max += result.timing_max;
                for (size_t u = 0; u < result.unknown_timing_count; u++) {
                    bool merged = false;
                    for (size_t j = 0; j < unknown_count; j++) {
                        if (!strcasecmp(unknown[j].label, result.unknown_timings[u].label)) {
                            unknown[j].count += result.unknown_timings[u].count;
                            merged = true;
                            break;
                        }
                    }
                    if (!merged) {
                        unknown = xrealloc(unknown, (unknown_count + 1) * sizeof(*unknown));
                        unknown[unknown_count++] = (UnknownTiming){
                            .label=xstrdup(result.unknown_timings[u].label),
                            .count=result.unknown_timings[u].count
                        };
                    }
                }
                issues += (int)result.issue_count;
                if (result.issue_count) files_with_issues++;
                free(relative);
            }
            printf("Directory summary:\nFiles analyzed: %zu\nRecommended modifications: %d\nGlobal code size: %d bytes\nGlobal data size: %d bytes\n",
                   count, recommendations, code_size, data_size);
            print_timing_summary(timing_min, timing_max, unknown, unknown_count);
            printf("Warnings: %d across %d files\nDirectory status: %s\n",
                   issues, files_with_issues,
                   issues ? "WARNINGS" : "PASS");
        }
        for (size_t i = 0; i < count; i++) free(files[i]);
        free(files);
    } else if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "Error: input is not a file: %s\n", input);
        status = 2;
    } else {
        analyze_file(input, update, modify, data_comments, debug, verbose);
    }
    free(root);
    return status;
}
