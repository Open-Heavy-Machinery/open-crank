/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file oracle.h
 * @brief Acceptance oracle: grades a run from the frames a device under test
 *        sends, against criteria written as data.
 *
 * The oracle is generic. It knows nothing about J1939 or open-crank: it uses
 * only the C standard library and open-tick's core types (CAN frames,
 * scenarios, signal tables), so that it can move to open-tick. The
 * application gives it a table of observables and a function that decodes a
 * CAN frame into updates of those observables; criteria name observables
 * and scenario events.
 *
 * Criteria file (format: docs/acceptance.md):
 *
 *     # comment lines and blank lines are ignored
 *     id,from,until,tolerance_ms,condition,text
 *     S2,start=1+2000ms,end,0,speed_control == 1,speed control taken within 2 s of start and kept
 *
 * - `from` and `until` bound an inclusive window. Each is `begin` (t = 0),
 *   `end` (the scenario's `end` event), a time in seconds (`5.0`), or
 *   `<signal>=<value>` (the first scenario event that sets that signal to
 *   that value), optionally followed by an offset `+Nms` or `-Nms`.
 * - `condition` is `term (&& term)*`, optionally followed by
 *   `-> term (&& term)*` (implication). A term is `observable op number`,
 *   op one of `== != < <= > >=`. A term on a gap is false.
 * - The criterion fails if, at steps inside its window, the condition is
 *   false for consecutive steps that cover more than `tolerance_ms` (each
 *   step covers one step period). With 0, one false step fails.
 * - `text` is everything after the fifth comma. No quoting, ever.
 *
 * Memory is fixed: no malloc. An OcOracle is large (tens of KiB); keep it
 * off the stack.
 */

#ifndef OPEN_CRANK_ORACLE_H
#define OPEN_CRANK_ORACLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <opentick/can.h>
#include <opentick/scenario.h>
#include <opentick/signals.h>

#define OC_ORACLE_MAX_CRITERIA 64U    /**< most criteria in one file */
#define OC_ORACLE_MAX_OBSERVABLES 16U /**< most observables in one table */
#define OC_ORACLE_MAX_TERMS 8U        /**< most terms in one condition, both sides of `->` together */
#define OC_ORACLE_MAX_ID 16U          /**< longest id, terminator included */
#define OC_ORACLE_MAX_TEXT 256U       /**< longest text, terminator included */
#define OC_ORACLE_MAX_LABEL 64U       /**< longest event reference kept for the report, terminator included */
#define OC_ORACLE_MAX_LINE 512U       /**< longest line of a criteria file, newline included */

/** @brief A number, or a gap (no value). */
typedef struct {
    bool gap;
    double value;
} OcOracleValue;

/** @brief One observable. Its default holds before the first update and once the last update is older than stale_ms. */
typedef struct {
    const char *name;
    uint32_t stale_ms;
    OcOracleValue fallback; /**< the default value */
} OcOracleObservable;

/** @brief A new value for one observable, decoded from a frame. */
typedef struct {
    size_t observable; /**< index in the table */
    OcOracleValue value;
} OcOracleUpdate;

/**
 * @brief Decodes one frame into zero or more updates.
 * @return number of updates written to @p out (at most @p cap)
 */
typedef size_t (*OcOracleDecodeFn)(void *ctx, const OtkCanFrame *f, OcOracleUpdate *out, size_t cap);

/** @brief The application's observables and its frame decoder. */
typedef struct {
    const OcOracleObservable *observables;
    size_t count;            /**< 1..OC_ORACLE_MAX_OBSERVABLES */
    OcOracleDecodeFn decode;
    void *ctx;               /**< passed to decode */
} OcOracleTable;

typedef enum {
    OC_ORACLE_EQ = 0,
    OC_ORACLE_NE,
    OC_ORACLE_LT,
    OC_ORACLE_LE,
    OC_ORACLE_GT,
    OC_ORACLE_GE
} OcOracleOp;

typedef struct {
    size_t observable;
    OcOracleOp op;
    double rhs;
} OcOracleTerm;

/** @brief One criterion, as loaded, and its grading so far. */
typedef struct {
    char id[OC_ORACLE_MAX_ID];
    char text[OC_ORACLE_MAX_TEXT];
    int64_t from_ms;
    int64_t until_ms;
    uint32_t tolerance_ms;
    OcOracleTerm terms[OC_ORACLE_MAX_TERMS];
    size_t n_terms;
    size_t n_antecedent;       /**< terms before `->`; 0 without implication */

    bool from_event;           /**< `from` is an event with a positive offset: reaction time is measured */
    int64_t event_ms;
    char event[OC_ORACLE_MAX_LABEL];

    /* Grading. */
    bool evaluated;            /**< at least one step fell inside the window */
    int64_t last_ms;           /**< last step inside the window */
    bool failed;
    int64_t fail_ms;           /**< first step of the false run that failed */
    OcOracleValue fail_values[OC_ORACLE_MAX_OBSERVABLES];
    bool false_run;
    int64_t false_since_ms;
    OcOracleValue false_values[OC_ORACLE_MAX_OBSERVABLES];
    bool reaction_done;
    bool reaction_holding;     /**< the condition held at `from` */
    int64_t true_since_ms;     /**< start of the current uninterrupted true run since the event, -1 if none */
} OcOracleCriterion;

typedef struct {
    const OcOracleTable *table;
    uint32_t step_ms;
    OcOracleCriterion criteria[OC_ORACLE_MAX_CRITERIA];
    size_t n;

    OcOracleValue last[OC_ORACLE_MAX_OBSERVABLES]; /**< last decoded value */
    bool seen[OC_ORACLE_MAX_OBSERVABLES];
    uint32_t last_ms[OC_ORACLE_MAX_OBSERVABLES];
    OcOracleValue now[OC_ORACLE_MAX_OBSERVABLES];  /**< values at the last step, defaults applied */
} OcOracle;

/**
 * @brief Parses criteria text and resolves its event references.
 *
 * @param table     the application's observables; must outlive the oracle
 * @param scenario  the scenario the run plays
 * @param signals   the signal table the scenario was loaded with
 * @param step_ms   time between two calls of oc_oracle_step(), at least 1
 * @param err       receives a message such as "line 3: unknown observable 'rpm'" on failure
 * @return false on any format error; the oracle then holds no criteria
 */
bool oc_oracle_parse(OcOracle *o, const char *text, const OcOracleTable *table, const OtkScenario *scenario,
                     const OtkSignalTable *signals, uint32_t step_ms, char *err, size_t err_len);

/** @brief Reads a criteria file and parses it as oc_oracle_parse() does. */
bool oc_oracle_load(OcOracle *o, const char *path, const OcOracleTable *table, const OtkScenario *scenario,
                    const OtkSignalTable *signals, uint32_t step_ms, char *err, size_t err_len);

/** @brief Hands the oracle one frame seen on the bus at @p t_ms. */
void oc_oracle_frame(OcOracle *o, const OtkCanFrame *f, uint32_t t_ms);

/** @brief Evaluates every criterion once, at step @p t_ms, after that step's frames. */
void oc_oracle_step(OcOracle *o, uint32_t t_ms);

/**
 * @brief Prints one line per criterion.
 * @return true if every criterion passed
 */
bool oc_oracle_report(const OcOracle *o, FILE *fp);

/** @brief Number of criteria loaded. */
size_t oc_oracle_count(const OcOracle *o);

/** @brief Number of criteria that failed (a window the run never covered to its end counts as failed). */
size_t oc_oracle_failures(const OcOracle *o);

/** @brief True if the criterion @p id exists and failed. */
bool oc_oracle_failed(const OcOracle *o, const char *id);

#endif /* OPEN_CRANK_ORACLE_H */
