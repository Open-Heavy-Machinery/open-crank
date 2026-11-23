/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file oracle.c
 * @brief Acceptance oracle. See oracle.h and docs/acceptance.md.
 *
 * Only the C standard library and open-tick's core types: this file must not
 * know about J1939 or open-crank.
 */

#include <opencrank/oracle.h>

#include <ctype.h>
#include <float.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define HEADER "id,from,until,tolerance_ms,condition,text"
#define MAX_TOLERANCE_MS 3600000UL
#define MAX_OFFSET_MS 4000000000LL

typedef struct {
    OcOracle *o;
    const OtkScenario *scenario;
    const OtkSignalTable *signals;
    size_t lineno;
    bool header_seen;
    char *err;
    size_t err_len;
} Parser;

static bool fail(Parser *p, const char *fmt, ...) {
    if (p->err != NULL && p->err_len > 0U) {
        int used = 0;
        if (p->lineno > 0U) {
            used = snprintf(p->err, p->err_len, "line %lu: ", (unsigned long)p->lineno);
        }
        if (used >= 0 && (size_t)used < p->err_len) {
            va_list ap;
            va_start(ap, fmt);
            (void)vsnprintf(p->err + used, p->err_len - (size_t)used, fmt, ap);
            va_end(ap);
        }
    }
    return false;
}

/* Strips leading and trailing white space in place. */
static char *trim(char *s) {
    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0U && isspace((unsigned char)s[n - 1U])) {
        s[--n] = '\0';
    }
    return s;
}

/* A finite number taking the whole string. */
static bool parse_number(const char *s, double *out) {
    if (*s == '\0') {
        return false;
    }
    char *end = NULL;
    const double v = strtod(s, &end);
    if (end == s || *end != '\0' || !(v >= -DBL_MAX && v <= DBL_MAX)) {
        return false;
    }
    *out = v;
    return true;
}

static bool all_digits(const char *s) {
    if (*s == '\0') {
        return false;
    }
    for (; *s != '\0'; s++) {
        if (!isdigit((unsigned char)*s)) {
            return false;
        }
    }
    return true;
}

/* Scenario event times are nanoseconds; an event applies at the first step at or after it. */
static int64_t ns_to_ms_ceil(OtkNs t) {
    return (int64_t)((t + OTK_NS_PER_MS - 1U) / OTK_NS_PER_MS);
}

/* ------------------------------------------------------------ references */

/*
 * `begin`, `end`, seconds, or `<signal>=<value>`, then an optional `+Nms` or
 * `-Nms`. Sets *event when the reference is a scenario event (`end` too) and
 * copies the reference without its offset to label.
 */
static bool parse_ref(Parser *p, const char *field, const char *what, int64_t *ms, bool *event, int64_t *event_ms,
                      int64_t *offset, char *label, size_t label_len) {
    char buf[OC_ORACLE_MAX_LINE];
    (void)snprintf(buf, sizeof(buf), "%s", field);
    char *base = trim(buf);
    *offset = 0;

    const size_t n = strlen(base);
    if (n > 2U && base[n - 2U] == 'm' && base[n - 1U] == 's') {
        size_t i = n - 2U;
        while (i > 0U && isdigit((unsigned char)base[i - 1U])) {
            i--;
        }
        if (i < n - 2U && i > 0U && (base[i - 1U] == '+' || base[i - 1U] == '-')) {
            const char sign = base[i - 1U];
            base[n - 2U] = '\0';
            if (strlen(&base[i]) > 10U) {
                return fail(p, "offset too large in %s '%s'", what, field);
            }
            const long long v = strtoll(&base[i], NULL, 10);
            if (v > MAX_OFFSET_MS) {
                return fail(p, "offset too large in %s '%s'", what, field);
            }
            *offset = (sign == '-') ? -(int64_t)v : (int64_t)v;
            base[i - 1U] = '\0';
            base = trim(base);
        }
    }
    if (*base == '\0') {
        return fail(p, "empty %s", what);
    }
    (void)snprintf(label, label_len, "%s", base);

    int64_t t = 0;
    *event = false;
    char *eq = strchr(base, '=');
    if (strcmp(base, "begin") == 0) {
        t = 0;
    } else if (strcmp(base, OTK_SIGNAL_END_NAME) == 0) {
        if (!p->scenario->has_end) {
            return fail(p, "%s refers to 'end', but the scenario has no end event", what);
        }
        t = ns_to_ms_ceil(p->scenario->end);
        *event = true;
    } else if (eq != NULL) {
        *eq = '\0';
        const char *name = trim(base);
        const char *value_text = trim(eq + 1);
        OtkSignalId id;
        if (!otk_signal_find(p->signals, name, &id) || id == OTK_SIGNAL_END) {
            return fail(p, "unknown signal '%s' in %s '%s'", name, what, field);
        }
        double value;
        if (!parse_number(value_text, &value)) {
            return fail(p, "bad value '%s' in %s '%s'", value_text, what, field);
        }
        bool found = false;
        for (size_t i = 0; i < p->scenario->n; i++) {
            const OtkEvent *e = &p->scenario->events[i];
            if (e->signal == id && e->value == value) {
                t = ns_to_ms_ceil(e->t);
                found = true;
                break;
            }
        }
        if (!found) {
            return fail(p, "no event in the scenario sets '%s' to %s (%s '%s')", name, value_text, what, field);
        }
        *event = true;
    } else {
        double s;
        if (!parse_number(base, &s) || s < 0.0 || s > OTK_SCENARIO_MAX_TIME_S) {
            return fail(p, "bad reference '%s' in %s", field, what);
        }
        t = (int64_t)(s * 1000.0 + 0.5);
    }
    *event_ms = t;
    *ms = t + *offset;
    if (*ms < 0) {
        return fail(p, "%s '%s' is before t = 0", what, field);
    }
    return true;
}

/* ------------------------------------------------------------ conditions */

static bool parse_term(Parser *p, OcOracleCriterion *c, char *text) {
    char *s = trim(text);
    if (c->n_terms >= OC_ORACLE_MAX_TERMS) {
        return fail(p, "more than %u terms in the condition", (unsigned)OC_ORACLE_MAX_TERMS);
    }
    char *op = strpbrk(s, "<>=!");
    if (op == NULL) {
        return fail(p, "no operator in term '%s'", s);
    }
    OcOracleOp code;
    size_t op_len = 1U;
    const bool eq_next = op[1] == '=';
    switch (*op) {
    case '<':
        code = eq_next ? OC_ORACLE_LE : OC_ORACLE_LT;
        break;
    case '>':
        code = eq_next ? OC_ORACLE_GE : OC_ORACLE_GT;
        break;
    case '=':
        code = OC_ORACLE_EQ;
        break;
    default: /* '!' */
        code = OC_ORACLE_NE;
        break;
    }
    if (eq_next) {
        op_len = 2U;
    } else if (*op == '=' || *op == '!') {
        return fail(p, "bad operator in term '%s'", s);
    } else {
        /* '<' or '>' alone */
    }
    char *rhs_text = trim(op + op_len);
    if (*rhs_text != '\0' && strchr("<>=!", *rhs_text) != NULL) {
        return fail(p, "bad operator in term '%s'", s);
    }
    double rhs;
    if (!parse_number(rhs_text, &rhs)) {
        return fail(p, "bad number '%s' in term '%s'", rhs_text, s);
    }
    *op = '\0';
    const char *name = trim(s);
    if (*name == '\0') {
        return fail(p, "no observable before the operator");
    }
    const OcOracleTable *table = p->o->table;
    size_t index = table->count;
    for (size_t i = 0; i < table->count; i++) {
        if (strcmp(name, table->observables[i].name) == 0) {
            index = i;
            break;
        }
    }
    if (index == table->count) {
        return fail(p, "unknown observable '%s'", name);
    }
    c->terms[c->n_terms].observable = index;
    c->terms[c->n_terms].op = code;
    c->terms[c->n_terms].rhs = rhs;
    c->n_terms++;
    return true;
}

static bool parse_conjunction(Parser *p, OcOracleCriterion *c, char *text) {
    if (*trim(text) == '\0') {
        return fail(p, "empty condition");
    }
    char *s = text;
    for (;;) {
        char *amp = strstr(s, "&&");
        if (amp != NULL) {
            *amp = '\0';
        }
        if (*trim(s) == '\0') {
            return fail(p, "empty term in the condition");
        }
        if (!parse_term(p, c, s)) {
            return false;
        }
        if (amp == NULL) {
            return true;
        }
        s = amp + 2;
    }
}

static bool parse_condition(Parser *p, OcOracleCriterion *c, char *text) {
    char *arrow = strstr(text, "->");
    if (arrow != NULL) {
        if (strstr(arrow + 2, "->") != NULL) {
            return fail(p, "more than one '->' in the condition");
        }
        *arrow = '\0';
        if (!parse_conjunction(p, c, text)) {
            return false;
        }
        c->n_antecedent = c->n_terms;
        return parse_conjunction(p, c, arrow + 2);
    }
    c->n_antecedent = 0U;
    return parse_conjunction(p, c, text);
}

/* ------------------------------------------------------------ lines */

static bool valid_id(const char *s) {
    if (*s == '\0') {
        return false;
    }
    for (; *s != '\0'; s++) {
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '.' && *s != '-') {
            return false;
        }
    }
    return true;
}

static bool parse_criterion(Parser *p, char *line) {
    OcOracle *o = p->o;
    char *field[6];
    field[0] = line;
    for (size_t i = 1; i < 6U; i++) {
        char *comma = strchr(field[i - 1U], ',');
        if (comma == NULL) {
            return fail(p, "expected six fields: " HEADER);
        }
        *comma = '\0';
        field[i] = comma + 1;
    }
    if (o->n >= OC_ORACLE_MAX_CRITERIA) {
        return fail(p, "more than %u criteria", (unsigned)OC_ORACLE_MAX_CRITERIA);
    }
    OcOracleCriterion *c = &o->criteria[o->n];
    memset(c, 0, sizeof(*c));
    c->true_since_ms = -1;

    const char *id = trim(field[0]);
    if (!valid_id(id) || strlen(id) >= OC_ORACLE_MAX_ID) {
        return fail(p, "bad id '%s' (letters, digits, '_', '.', '-'; at most %u characters)", id,
                    (unsigned)(OC_ORACLE_MAX_ID - 1U));
    }
    for (size_t i = 0; i < o->n; i++) {
        if (strcmp(o->criteria[i].id, id) == 0) {
            return fail(p, "duplicate id '%s'", id);
        }
    }
    (void)snprintf(c->id, sizeof(c->id), "%s", id);

    const char *text = trim(field[5]);
    if (*text == '\0') {
        return fail(p, "empty text");
    }
    if (strlen(text) >= OC_ORACLE_MAX_TEXT) {
        return fail(p, "text longer than %u characters", (unsigned)(OC_ORACLE_MAX_TEXT - 1U));
    }
    (void)snprintf(c->text, sizeof(c->text), "%s", text);

    bool from_event = false;
    bool until_event = false;
    int64_t from_offset = 0;
    int64_t until_offset = 0;
    int64_t until_event_ms = 0;
    char until_label[OC_ORACLE_MAX_LABEL];
    if (!parse_ref(p, field[1], "from", &c->from_ms, &from_event, &c->event_ms, &from_offset, c->event,
                   sizeof(c->event)) ||
        !parse_ref(p, field[2], "until", &c->until_ms, &until_event, &until_event_ms, &until_offset, until_label,
                   sizeof(until_label))) {
        return false;
    }
    c->from_event = from_event && from_offset > 0;

    const char *tol = trim(field[3]);
    if (!all_digits(tol) || strlen(tol) > 7U || strtoul(tol, NULL, 10) > MAX_TOLERANCE_MS) {
        return fail(p, "bad tolerance_ms '%s' (whole milliseconds, at most %lu)", tol, MAX_TOLERANCE_MS);
    }
    c->tolerance_ms = (uint32_t)strtoul(tol, NULL, 10);

    if (!parse_condition(p, c, field[4])) {
        return false;
    }

    if (c->from_ms > c->until_ms) {
        return fail(p, "empty window: from %.3f s is after until %.3f s", (double)c->from_ms / 1000.0,
                    (double)c->until_ms / 1000.0);
    }
    if (p->scenario->has_end && c->until_ms > ns_to_ms_ceil(p->scenario->end)) {
        return fail(p, "until %.3f s is after the scenario's end (%.3f s)", (double)c->until_ms / 1000.0,
                    (double)ns_to_ms_ceil(p->scenario->end) / 1000.0);
    }
    o->n++;
    return true;
}

static bool parse_line(Parser *p, char *raw) {
    char *line = trim(raw);
    if (*line == '\0' || *line == '#') {
        return true;
    }
    if (!p->header_seen) {
        if (strcmp(line, HEADER) != 0) {
            return fail(p, "expected the header '" HEADER "'");
        }
        p->header_seen = true;
        return true;
    }
    return parse_criterion(p, line);
}

static bool begin(Parser *p, OcOracle *o, const OcOracleTable *table, const OtkScenario *scenario,
                  const OtkSignalTable *signals, uint32_t step_ms, char *err, size_t err_len) {
    memset(o, 0, sizeof(*o));
    p->o = o;
    p->scenario = scenario;
    p->signals = signals;
    p->lineno = 0U;
    p->header_seen = false;
    p->err = err;
    p->err_len = err_len;
    if (err != NULL && err_len > 0U) {
        err[0] = '\0';
    }
    if (table == NULL || table->observables == NULL || table->count == 0U ||
        table->count > OC_ORACLE_MAX_OBSERVABLES || table->decode == NULL) {
        return fail(p, "bad observable table (1 to %u observables and a decoder)",
                    (unsigned)OC_ORACLE_MAX_OBSERVABLES);
    }
    for (size_t i = 0; i < table->count; i++) {
        if (table->observables[i].name == NULL || table->observables[i].name[0] == '\0') {
            return fail(p, "observable %lu has no name", (unsigned long)i);
        }
    }
    if (scenario == NULL || signals == NULL) {
        return fail(p, "no scenario or signal table");
    }
    if (step_ms == 0U) {
        return fail(p, "step_ms must be at least 1");
    }
    o->table = table;
    o->step_ms = step_ms;
    return true;
}

static bool finish(Parser *p, bool ok) {
    if (ok && !p->header_seen) {
        p->lineno = 0U;
        ok = fail(p, "no header '" HEADER "'");
    } else if (ok && p->o->n == 0U) {
        p->lineno = 0U;
        ok = fail(p, "no criteria");
    } else {
        /* ok, or the error is already written */
    }
    if (!ok) {
        p->o->n = 0U;
    }
    return ok;
}

bool oc_oracle_parse(OcOracle *o, const char *text, const OcOracleTable *table, const OtkScenario *scenario,
                     const OtkSignalTable *signals, uint32_t step_ms, char *err, size_t err_len) {
    Parser p;
    if (!begin(&p, o, table, scenario, signals, step_ms, err, err_len)) {
        o->n = 0U;
        return false;
    }
    bool ok = true;
    const char *s = text;
    while (ok && *s != '\0') {
        p.lineno++;
        const char *nl = strchr(s, '\n');
        const size_t len = (nl != NULL) ? (size_t)(nl - s) : strlen(s);
        char line[OC_ORACLE_MAX_LINE];
        if (len >= sizeof(line)) {
            ok = fail(&p, "longer than %u characters", (unsigned)(OC_ORACLE_MAX_LINE - 1U));
            break;
        }
        memcpy(line, s, len);
        line[len] = '\0';
        ok = parse_line(&p, line);
        s = (nl != NULL) ? nl + 1 : s + len;
    }
    return finish(&p, ok);
}

bool oc_oracle_load(OcOracle *o, const char *path, const OcOracleTable *table, const OtkScenario *scenario,
                    const OtkSignalTable *signals, uint32_t step_ms, char *err, size_t err_len) {
    Parser p;
    if (!begin(&p, o, table, scenario, signals, step_ms, err, err_len)) {
        o->n = 0U;
        return false;
    }
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        (void)fail(&p, "cannot open '%s'", path);
        o->n = 0U;
        return false;
    }
    bool ok = true;
    char line[OC_ORACLE_MAX_LINE + 1U];
    while (ok && fgets(line, (int)sizeof(line), fp) != NULL) {
        p.lineno++;
        const size_t len = strlen(line);
        if (len > 0U && line[len - 1U] != '\n' && !feof(fp)) {
            ok = fail(&p, "longer than %u characters", (unsigned)(OC_ORACLE_MAX_LINE - 1U));
            break;
        }
        ok = parse_line(&p, line);
    }
    if (ok && ferror(fp)) {
        p.lineno = 0U;
        ok = fail(&p, "cannot read '%s'", path);
    }
    fclose(fp);
    return finish(&p, ok);
}

/* ------------------------------------------------------------ grading */

void oc_oracle_frame(OcOracle *o, const OtkCanFrame *f, uint32_t t_ms) {
    if (o->table == NULL) {
        return;
    }
    OcOracleUpdate up[OC_ORACLE_MAX_OBSERVABLES];
    size_t n = o->table->decode(o->table->ctx, f, up, OC_ORACLE_MAX_OBSERVABLES);
    if (n > OC_ORACLE_MAX_OBSERVABLES) {
        n = OC_ORACLE_MAX_OBSERVABLES;
    }
    for (size_t i = 0; i < n; i++) {
        const size_t k = up[i].observable;
        if (k < o->table->count) {
            o->last[k] = up[i].value;
            o->seen[k] = true;
            o->last_ms[k] = t_ms;
        }
    }
}

static bool term_holds(const OcOracle *o, const OcOracleTerm *t) {
    const OcOracleValue v = o->now[t->observable];
    if (v.gap) {
        return false;
    }
    switch (t->op) {
    case OC_ORACLE_EQ:
        return v.value == t->rhs;
    case OC_ORACLE_NE:
        return v.value != t->rhs;
    case OC_ORACLE_LT:
        return v.value < t->rhs;
    case OC_ORACLE_LE:
        return v.value <= t->rhs;
    case OC_ORACLE_GT:
        return v.value > t->rhs;
    case OC_ORACLE_GE:
        return v.value >= t->rhs;
    default:
        return false;
    }
}

static bool all_hold(const OcOracle *o, const OcOracleTerm *terms, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!term_holds(o, &terms[i])) {
            return false;
        }
    }
    return true;
}

static bool condition_holds(const OcOracle *o, const OcOracleCriterion *c) {
    if (c->n_antecedent == 0U) {
        return all_hold(o, c->terms, c->n_terms);
    }
    return !all_hold(o, c->terms, c->n_antecedent) ||
           all_hold(o, &c->terms[c->n_antecedent], c->n_terms - c->n_antecedent);
}

void oc_oracle_step(OcOracle *o, uint32_t t_ms) {
    if (o->table == NULL) {
        return;
    }
    const int64_t t = (int64_t)t_ms;
    for (size_t k = 0; k < o->table->count; k++) {
        const int64_t age = t - (int64_t)o->last_ms[k];
        const bool fresh = o->seen[k] && age <= (int64_t)o->table->observables[k].stale_ms;
        o->now[k] = fresh ? o->last[k] : o->table->observables[k].fallback;
    }

    for (size_t i = 0; i < o->n; i++) {
        OcOracleCriterion *c = &o->criteria[i];
        const bool ok = condition_holds(o, c);

        if (c->from_event && !c->reaction_done && t >= c->event_ms) {
            if (!ok) {
                c->true_since_ms = -1;
            } else if (c->true_since_ms < 0) {
                c->true_since_ms = t;
            } else {
                /* still holding */
            }
            if (t >= c->from_ms) {
                c->reaction_done = true;
                c->reaction_holding = ok;
            }
        }

        if (t < c->from_ms || t > c->until_ms) {
            continue;
        }
        c->evaluated = true;
        c->last_ms = t;
        if (ok) {
            c->false_run = false;
            continue;
        }
        if (!c->false_run) {
            c->false_run = true;
            c->false_since_ms = t;
            memcpy(c->false_values, o->now, sizeof(c->false_values));
        }
        if (!c->failed && (t - c->false_since_ms + (int64_t)o->step_ms) > (int64_t)c->tolerance_ms) {
            c->failed = true;
            c->fail_ms = c->false_since_ms;
            memcpy(c->fail_values, c->false_values, sizeof(c->fail_values));
        }
    }
}

/* The window was evaluated up to its end (the last step inside it is within one step of `until`). */
static bool covered(const OcOracle *o, const OcOracleCriterion *c) {
    return c->evaluated && c->last_ms + (int64_t)o->step_ms > c->until_ms;
}

static bool passed(const OcOracle *o, const OcOracleCriterion *c) {
    return covered(o, c) && !c->failed;
}

static void print_values(const OcOracle *o, const OcOracleCriterion *c, FILE *fp) {
    bool printed[OC_ORACLE_MAX_OBSERVABLES] = { false };
    const char *sep = " ";
    for (size_t i = 0; i < c->n_terms; i++) {
        const size_t k = c->terms[i].observable;
        if (printed[k]) {
            continue;
        }
        printed[k] = true;
        if (c->fail_values[k].gap) {
            fprintf(fp, "%s%s = gap", sep, o->table->observables[k].name);
        } else {
            fprintf(fp, "%s%s = %g", sep, o->table->observables[k].name, c->fail_values[k].value);
        }
        sep = ", ";
    }
}

bool oc_oracle_report(const OcOracle *o, FILE *fp) {
    bool all = true;
    for (size_t i = 0; i < o->n; i++) {
        const OcOracleCriterion *c = &o->criteria[i];
        const bool ok = passed(o, c);
        all = all && ok;
        fprintf(fp, "%s  %s  %s  [%.2f s .. %.2f s]", ok ? "PASS" : "FAIL", c->id, c->text,
                (double)c->from_ms / 1000.0, (double)c->until_ms / 1000.0);
        if (c->from_event) {
            if (!c->reaction_done) {
                fprintf(fp, "  reaction: not measured");
            } else if (c->reaction_holding) {
                fprintf(fp, "  reaction: %lld ms after %s", (long long)(c->true_since_ms - c->event_ms), c->event);
            } else {
                fprintf(fp, "  reaction: not holding at %.2f s", (double)c->from_ms / 1000.0);
            }
        }
        if (c->failed) {
            fprintf(fp, "  first violation at %.2f s:", (double)c->fail_ms / 1000.0);
            print_values(o, c, fp);
        } else if (!c->evaluated) {
            fprintf(fp, "  not evaluated: no step fell inside the window");
        } else if (!covered(o, c)) {
            fprintf(fp, "  not evaluated to the end: the last step was at %.2f s", (double)c->last_ms / 1000.0);
        } else {
            /* passed */
        }
        fputc('\n', fp);
    }
    return all;
}

size_t oc_oracle_count(const OcOracle *o) {
    return o->n;
}

size_t oc_oracle_failures(const OcOracle *o) {
    size_t n = 0U;
    for (size_t i = 0; i < o->n; i++) {
        if (!passed(o, &o->criteria[i])) {
            n++;
        }
    }
    return n;
}

bool oc_oracle_failed(const OcOracle *o, const char *id) {
    for (size_t i = 0; i < o->n; i++) {
        if (strcmp(o->criteria[i].id, id) == 0) {
            return !passed(o, &o->criteria[i]);
        }
    }
    return false;
}
