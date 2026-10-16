/**
 * @file vm_eval.c
 * @brief Core expression evaluator and rule execution engine.
 *
 * Implements the C-DSL runtime: recursive expression evaluation,
 * simple rule (WHEN/THEN) execution, metric-based scoring rule
 * execution, tri-state reporting, and JSON report serialization.
 *
 * ### Execution flow
 *
 *     cdsl_vm_execute()
 *       ├── execute_metric_rule()   (rule->metrics != NULL)
 *       └── execute_simple_rule()   (rule->when_expr  != NULL)
 *            └── evaluate_condition()
 *                 ├── cdsl_bytecode_execute()  (fast path)
 *                 └── cdsl_eval_expr_internal() (tree-walk fallback)
 *
 * ### Type coercion in binary expressions
 *
 * Comparison: numerics promoted to double before comparison using
 * `fabs(lv - rv) < 1e-9` for EQ.  Strings via `strcmp()`, dates
 * via `time_t` arithmetic.
 *
 * Arithmetic: INT/FLOAT/LONG/BOOL promoted to widest type (FLOAT >
 * LONG > INT).  Division by zero → CDSL_TYPE_VOID.
 *
 * @defgroup eval Expression Evaluator & Rule Execution
 * @{
 */

#include "cdsl/execution.h"
#include "internal.h"
#include "cdsl/cache.h"
#include "cdsl/util/portability.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>

/**
 * @brief Parse a string to int with error detection.
 * @return int value, or default_val on parse failure.
 */
static int
safe_atoi(const char* str, int default_val)
{
	if (!str) {
		return default_val;
	}
	char* end = NULL;
	errno = 0;
	long val = strtol(str, &end, 10);
	if (errno != 0 || end == str || *end != '\0') {
		return default_val;
	}
	return (int)val;
}

/**
 * @brief Fire a trace event if callback is registered.
 */
static void
fire_trace(cdsl_vm_t* vm,
	   cdsl_trace_kind_t kind,
	   const char* rule_name,
	   const char* detail,
	   cdsl_value_t val,
	   int depth)
{
	if (!vm || !vm->trace_cb) {
		return;
	}
	cdsl_trace_event_t ev = {
	    .kind = kind,
	    .rule_name = rule_name,
	    .detail = detail,
	    .value = val,
	    .depth = depth,
	    .timestamp_us = cdsl_get_time_us_internal(),
	};
	vm->trace_cb(&ev, vm->trace_ud);
}

/**
 * @brief Evaluate condition using bytecode if available, else tree-walk.
 */
static cdsl_value_t
evaluate_condition(cdsl_expr_node_t* expr,
		   cdsl_context_t* ctx,
		   cdsl_vm_t* vm,
		   const cdsl_bytecode_t* bc,
		   int debug)
{
	if (bc && bc->code) {
		(void)debug;
		return cdsl_bytecode_execute(vm, bc, ctx);
	}
	return cdsl_eval_expr_internal(expr, ctx, vm, debug, 0);
}

static size_t
json_escape_len(const char* s)
{
	size_t len = 0;
	for (const char* p = s; *p; p++) {
		switch (*p) {
		case '"':
		case '\\':
			len += 2;
			break;
		case '\n':
			len += 2;
			break;
		case '\r':
			len += 2;
			break;
		case '\t':
			len += 2;
			break;
		case '\b':
			len += 2;
			break;
		case '\f':
			len += 2;
			break;
		default:
			if ((unsigned char)*p < 0x20) {
				len += 6;
			} else {
				len++;
			}
			break;
		}
	}
	return len;
}

static size_t
json_escape_buf(char* dst, const char* src, size_t dst_cap)
{
	char* out = dst;
	char* end = dst + dst_cap;
	for (const char* p = src; *p; p++) {
		if (out >= end - 7) {
			break;
		}
		switch (*p) {
		case '"':
			*out++ = '\\';
			*out++ = '"';
			break;
		case '\\':
			*out++ = '\\';
			*out++ = '\\';
			break;
		case '\n':
			*out++ = '\\';
			*out++ = 'n';
			break;
		case '\r':
			*out++ = '\\';
			*out++ = 'r';
			break;
		case '\t':
			*out++ = '\\';
			*out++ = 't';
			break;
		case '\b':
			*out++ = '\\';
			*out++ = 'b';
			break;
		case '\f':
			*out++ = '\\';
			*out++ = 'f';
			break;
		default:
			if ((unsigned char)*p < 0x20) {
				int n = snprintf(out, 7, "\\u%04x", (unsigned char)*p);
				out += n > 0 ? n : 0;
			} else {
				*out++ = *p;
			}
			break;
		}
	}
	if (out < end) {
		*out = '\0';
	} else if (dst_cap > 0) {
		dst[dst_cap - 1] = '\0';
	}
	return (size_t)(out - dst);
}

/**
 * @brief Convert a tri-state rule status to a human-readable string.
 *
 * @param s Status enum value
 * @return Static string: "PASSED", "PARTIALLY PASSED", "FAILED", or "ERROR"
 */
const char*
cdsl_status_str_internal(cdsl_rule_status_t s)
{
	switch (s) {
	case CDSL_STATUS_PASSED:
		return "PASSED";
	case CDSL_STATUS_PARTIALLY_PASSED:
		return "PARTIALLY PASSED";
	case CDSL_STATUS_FAILED:
		return "FAILED";
	default:
		return "ERROR";
	}
}

cdsl_value_t
cdsl_eval_expr_internal(
    cdsl_expr_node_t* expr, cdsl_context_t* ctx, cdsl_vm_t* vm, int debug, int depth)
{
	cdsl_value_t result = {.type = CDSL_TYPE_VOID};
	if (!expr) {
		return result;
	}
	int limit = vm ? vm->max_expr_depth : CDSL_MAX_EXPR_DEPTH;
	if (depth >= limit) {
		if (debug) {
			fprintf(stderr, "[TRACE]   max expr depth (%d) exceeded\n", limit);
		}
		return result;
	}

	/* Instruction quota check for tree-walk evaluator */
	if (vm && vm->instruction_limit > 0) {
		int64_t count = CDSL_ATOMIC_FETCH_ADD(&vm->instruction_count, 1) + 1;
		if (count > vm->instruction_limit) {
			CDSL_ATOMIC_STORE(&vm->error_state, 1);
			return result;
		}
	}

	switch (expr->type) {
	case CDSL_EXPR_INT:
		result.type = CDSL_TYPE_INT;
		result.data.int_val = expr->data.int_val;
		if (debug) {
			fprintf(stderr, "[TRACE]   literal int: %d\n", expr->data.int_val);
		}
		break;
	case CDSL_EXPR_FLOAT:
		result.type = CDSL_TYPE_FLOAT;
		result.data.float_val = expr->data.float_val;
		if (debug) {
			fprintf(stderr, "[TRACE]   literal float: %.2f\n", expr->data.float_val);
		}
		break;
	case CDSL_EXPR_BOOL:
		result.type = CDSL_TYPE_BOOL;
		result.data.bool_val = expr->data.bool_val;
		if (debug) {
			fprintf(stderr,
				"[TRACE]   literal bool: %s\n",
				expr->data.bool_val ? "true" : "false");
		}
		break;
	case CDSL_EXPR_STRING:
		result.type = CDSL_TYPE_STRING;
		result.data.string_val = expr->data.string_val;
		if (debug) {
			fprintf(
			    stderr, "[TRACE]   literal string: \"%s\"\n", expr->data.string_val);
		}
		break;
	case CDSL_EXPR_DATE:
		result.type = CDSL_TYPE_DATE;
		result.data.date_val = expr->data.date_val;
		if (debug) {
			char buf[32];
			struct tm tm_buf;
			CDSL_LOCALTIME_R(&expr->data.date_val, &tm_buf);
			strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
			fprintf(stderr, "[TRACE]   literal date: %s\n", buf);
		}
		break;
	case CDSL_EXPR_LONG:
		result.type = CDSL_TYPE_LONG;
		result.data.long_val = expr->data.long_val;
		if (debug) {
			fprintf(stderr, "[TRACE]   literal long: %ld\n", (long)expr->data.long_val);
		}
		break;
	case CDSL_EXPR_ARRAY: {
		cdsl_arg_node_t* arg = expr->data.array.elements;
		int count = 0;
		while (arg) {
			count++;
			arg = arg->next;
		}
		cdsl_array_t* arr = calloc(1, sizeof(cdsl_array_t));
		if (arr) {
			arr->items = calloc(count > 0 ? count : 1, sizeof(cdsl_value_t));
			if (!arr->items && count > 0) {
				free(arr);
				arr = NULL;
			} else {
				arr->count = count;
				arr->capacity = count;
				arg = expr->data.array.elements;
				for (int i = 0; i < count; i++) {
					arr->items[i] = cdsl_eval_expr_internal(
					    arg->expr, ctx, vm, debug, depth + 1);
					arg = arg->next;
				}
			}
		}
		result.type = CDSL_TYPE_ARRAY;
		result.data.array_val = arr;
		if (debug) {
			fprintf(stderr, "[TRACE]   literal array: [%d elements]\n", count);
		}
		break;
	}
	case CDSL_EXPR_ID: {
		cdsl_context_entry_t* e = cdsl_context_get_entry_internal(ctx, expr->data.id_val);
		if (e) {
			if (debug) {
				fprintf(stderr, "[TRACE]   lookup: %s = ", expr->data.id_val);
				switch (e->value.type) {
				case CDSL_TYPE_INT:
					fprintf(stderr, "%d\n", e->value.data.int_val);
					break;
				case CDSL_TYPE_FLOAT:
					fprintf(stderr, "%.2f\n", e->value.data.float_val);
					break;
				case CDSL_TYPE_BOOL:
					fprintf(stderr,
						"%s\n",
						e->value.data.bool_val ? "true" : "false");
					break;
				case CDSL_TYPE_STRING:
					fprintf(stderr, "\"%s\"\n", e->value.data.string_val);
					break;
				case CDSL_TYPE_DATE: {
					char buf[32];
					struct tm tm_buf;
					CDSL_LOCALTIME_R(&e->value.data.date_val, &tm_buf);
					strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
					fprintf(stderr, "%s\n", buf);
					break;
				}
				case CDSL_TYPE_LONG:
					fprintf(stderr, "%ld\n", (long)e->value.data.long_val);
					break;
				default:
					fprintf(stderr, "?\n");
					break;
				}
			}
			return e->value;
		}
		if (debug) {
			fprintf(stderr, "[TRACE]   lookup: %s = NOT FOUND\n", expr->data.id_val);
		}
		break;
	}
	case CDSL_EXPR_UNARY: {
		cdsl_value_t v =
		    cdsl_eval_expr_internal(expr->data.unary.expr, ctx, vm, debug, depth + 1);
		if (expr->data.unary.op == CDSL_OP_NOT) {
			result.type = CDSL_TYPE_BOOL;
			result.data.bool_val = !v.data.bool_val;
			if (debug) {
				fprintf(stderr,
					"[TRACE]   NOT %s = %s\n",
					v.data.bool_val ? "true" : "false",
					result.data.bool_val ? "true" : "false");
			}
		} else if (expr->data.unary.op == CDSL_OP_NEG) {
			if (v.type == CDSL_TYPE_INT) {
				result.type = CDSL_TYPE_INT;
				result.data.int_val = -v.data.int_val;
			} else if (v.type == CDSL_TYPE_FLOAT) {
				result.type = CDSL_TYPE_FLOAT;
				result.data.float_val = -v.data.float_val;
			} else if (v.type == CDSL_TYPE_LONG) {
				result.type = CDSL_TYPE_LONG;
				result.data.long_val = -v.data.long_val;
			} else {
				result.type = CDSL_TYPE_VOID;
			}
			if (debug) {
				fprintf(stderr, "[TRACE]   NEG\n");
			}
		}
		break;
	}
	case CDSL_EXPR_BINARY: {
		cdsl_op_t op = expr->data.binary.op;
		if (op == CDSL_OP_AND) {
			cdsl_value_t l = cdsl_eval_expr_internal(
			    expr->data.binary.left, ctx, vm, debug, depth + 1);
			if (!l.data.bool_val) {
				result.type = CDSL_TYPE_BOOL;
				result.data.bool_val = 0;
				if (debug) {
					fprintf(stderr, "[TRACE]   AND short-circuit: false\n");
				}
				break;
			}
			cdsl_value_t r = cdsl_eval_expr_internal(
			    expr->data.binary.right, ctx, vm, debug, depth + 1);
			result.type = CDSL_TYPE_BOOL;
			result.data.bool_val = r.data.bool_val;
			if (debug) {
				fprintf(stderr,
					"[TRACE]   AND: true AND %s = %s\n",
					r.data.bool_val ? "true" : "false",
					result.data.bool_val ? "true" : "false");
			}
			break;
		}
		if (op == CDSL_OP_OR) {
			cdsl_value_t l = cdsl_eval_expr_internal(
			    expr->data.binary.left, ctx, vm, debug, depth + 1);
			if (l.data.bool_val) {
				result.type = CDSL_TYPE_BOOL;
				result.data.bool_val = 1;
				if (debug) {
					fprintf(stderr, "[TRACE]   OR short-circuit: true\n");
				}
				break;
			}
			cdsl_value_t r = cdsl_eval_expr_internal(
			    expr->data.binary.right, ctx, vm, debug, depth + 1);
			result.type = CDSL_TYPE_BOOL;
			result.data.bool_val = r.data.bool_val;
			if (debug) {
				fprintf(stderr,
					"[TRACE]   OR: false OR %s = %s\n",
					r.data.bool_val ? "true" : "false",
					result.data.bool_val ? "true" : "false");
			}
			break;
		}
		cdsl_value_t l =
		    cdsl_eval_expr_internal(expr->data.binary.left, ctx, vm, debug, depth + 1);
		cdsl_value_t r =
		    cdsl_eval_expr_internal(expr->data.binary.right, ctx, vm, debug, depth + 1);

		if (l.type == CDSL_TYPE_STRING && r.type == CDSL_TYPE_STRING &&
		    (op == CDSL_OP_EQ || op == CDSL_OP_NE)) {
			result.type = CDSL_TYPE_BOOL;
			int cmp = strcmp(l.data.string_val ? l.data.string_val : "",
					 r.data.string_val ? r.data.string_val : "");
			result.data.bool_val = (op == CDSL_OP_EQ) ? (cmp == 0) : (cmp != 0);
			break;
		}

		/* DATE arithmetic: DATE +/- INT/LONG → DATE, DATE - DATE → INT */
		if (l.type == CDSL_TYPE_DATE &&
		    (r.type == CDSL_TYPE_INT || r.type == CDSL_TYPE_LONG) &&
		    (op == CDSL_OP_ADD || op == CDSL_OP_SUB)) {
			int64_t days =
			    (r.type == CDSL_TYPE_LONG) ? r.data.long_val : (int64_t)r.data.int_val;
			result.type = CDSL_TYPE_DATE;
			result.data.date_val = (op == CDSL_OP_ADD)
						   ? l.data.date_val + (time_t)(days * 86400)
						   : l.data.date_val - (time_t)(days * 86400);
			break;
		}
		if ((l.type == CDSL_TYPE_INT || l.type == CDSL_TYPE_LONG) &&
		    r.type == CDSL_TYPE_DATE && op == CDSL_OP_ADD) {
			int64_t days =
			    (l.type == CDSL_TYPE_LONG) ? l.data.long_val : (int64_t)l.data.int_val;
			result.type = CDSL_TYPE_DATE;
			result.data.date_val = r.data.date_val + (time_t)(days * 86400);
			break;
		}
		if (l.type == CDSL_TYPE_DATE && r.type == CDSL_TYPE_DATE && op == CDSL_OP_SUB) {
			double diff = difftime(l.data.date_val, r.data.date_val);
			result.type = CDSL_TYPE_INT;
			result.data.int_val = (int)(diff / 86400);
			break;
		}

		if (l.type == CDSL_TYPE_DATE && r.type == CDSL_TYPE_DATE) {
			result.type = CDSL_TYPE_BOOL;
			time_t t1 = l.data.date_val;
			time_t t2 = r.data.date_val;
			switch (op) {
			case CDSL_OP_EQ:
				result.data.bool_val = (t1 == t2);
				break;
			case CDSL_OP_NE:
				result.data.bool_val = (t1 != t2);
				break;
			case CDSL_OP_LT:
				result.data.bool_val = (t1 < t2);
				break;
			case CDSL_OP_GT:
				result.data.bool_val = (t1 > t2);
				break;
			case CDSL_OP_LE:
				result.data.bool_val = (t1 <= t2);
				break;
			case CDSL_OP_GE:
				result.data.bool_val = (t1 >= t2);
				break;
			default:
				result.data.bool_val = 0;
				break;
			}
			break;
		}

		/* Coerce operands to double for comparison; also detect LONG for arithmetic */
		int has_float = 0;
		int has_long = 0;
		double lv, rv;
		int64_t llv = 0, lrv = 0;

		if (l.type == CDSL_TYPE_INT) {
			lv = l.data.int_val;
			llv = l.data.int_val;
		} else if (l.type == CDSL_TYPE_FLOAT) {
			lv = l.data.float_val;
			has_float = 1;
		} else if (l.type == CDSL_TYPE_LONG) {
			lv = (double)l.data.long_val;
			llv = l.data.long_val;
			has_long = 1;
		} else if (l.type == CDSL_TYPE_BOOL) {
			lv = l.data.bool_val;
			llv = l.data.bool_val;
		} else {
			result.type = CDSL_TYPE_BOOL;
			result.data.bool_val = 0;
			break;
		}

		if (r.type == CDSL_TYPE_INT) {
			rv = r.data.int_val;
			lrv = r.data.int_val;
		} else if (r.type == CDSL_TYPE_FLOAT) {
			rv = r.data.float_val;
			has_float = 1;
		} else if (r.type == CDSL_TYPE_LONG) {
			rv = (double)r.data.long_val;
			lrv = r.data.long_val;
			has_long = 1;
		} else if (r.type == CDSL_TYPE_BOOL) {
			rv = r.data.bool_val;
			lrv = r.data.bool_val;
		} else {
			result.type = CDSL_TYPE_BOOL;
			result.data.bool_val = 0;
			break;
		}

		result.type = CDSL_TYPE_BOOL;
		switch (op) {
		case CDSL_OP_EQ:
			result.data.bool_val = fabs(lv - rv) < 1e-9;
			break;
		case CDSL_OP_NE:
			result.data.bool_val = fabs(lv - rv) >= 1e-9;
			break;
		case CDSL_OP_LT:
			result.data.bool_val = lv < rv;
			break;
		case CDSL_OP_GT:
			result.data.bool_val = lv > rv;
			break;
		case CDSL_OP_LE:
			result.data.bool_val = lv <= rv;
			break;
		case CDSL_OP_GE:
			result.data.bool_val = lv >= rv;
			break;
		case CDSL_OP_ADD:
		case CDSL_OP_SUB:
		case CDSL_OP_MUL:
			if (has_float) {
				result.type = CDSL_TYPE_FLOAT;
				result.data.float_val = (op == CDSL_OP_ADD)   ? lv + rv
							: (op == CDSL_OP_SUB) ? lv - rv
									      : lv * rv;
			} else if (has_long) {
				result.type = CDSL_TYPE_LONG;
				result.data.long_val = (op == CDSL_OP_ADD)   ? llv + lrv
						       : (op == CDSL_OP_SUB) ? llv - lrv
									     : llv * lrv;
			} else {
				result.type = CDSL_TYPE_INT;
				result.data.int_val = (int)((op == CDSL_OP_ADD)	  ? lv + rv
							    : (op == CDSL_OP_SUB) ? lv - rv
										  : lv * rv);
			}
			break;
		case CDSL_OP_DIV:
			if (rv == 0.0) {
				result.type = CDSL_TYPE_VOID;
				break;
			}
			if (has_float) {
				result.type = CDSL_TYPE_FLOAT;
				result.data.float_val = lv / rv;
			} else if (has_long) {
				result.type = CDSL_TYPE_LONG;
				result.data.long_val = llv / lrv;
			} else {
				result.type = CDSL_TYPE_INT;
				result.data.int_val = (int)(lv / rv);
			}
			break;
			break;
		default:
			break;
		}
		break;
	}
	case CDSL_EXPR_CALL: {
		if (!vm) {
			break;
		}
		for (cdsl_func_entry_t* fn = vm->functions; fn; fn = fn->next) {
			if (strcmp(fn->func_name, expr->data.call.func_name) == 0) {
				if (debug) {
					fprintf(stderr,
						"[TRACE]   calling function: %s()\n",
						expr->data.call.func_name);
				}
				result = fn->cb(
				    expr->data.call.func_name, expr->data.call.args, ctx, vm);
				if (debug) {
					fprintf(stderr, "[TRACE]   function result: ");
					switch (result.type) {
					case CDSL_TYPE_INT:
						fprintf(stderr, "%d\n", result.data.int_val);
						break;
					case CDSL_TYPE_FLOAT:
						fprintf(stderr, "%.2f\n", result.data.float_val);
						break;
					case CDSL_TYPE_BOOL:
						fprintf(stderr,
							"%s\n",
							result.data.bool_val ? "true" : "false");
						break;
					case CDSL_TYPE_STRING:
						fprintf(stderr, "\"%s\"\n", result.data.string_val);
						break;
					default:
						fprintf(stderr, "void\n");
						break;
					}
				}
				break;
			}
		}
		break;
	}
	}
	return result;
}

void
cdsl_trigger_action_internal(cdsl_vm_t* vm, cdsl_action_node_t* action)
{
	if (!vm || !action) {
		return;
	}
	for (cdsl_action_cb_entry_t* cb = vm->callbacks; cb; cb = cb->next) {
		if (strcmp(cb->action_name, action->action_name) == 0) {
			cb->cb(action->action_name, action->args, vm->user_data);
			return;
		}
	}
}

static char*
get_metric_meta(cdsl_meta_item_t* meta, const char* key, const char* def)
{
	char* v = cdsl_meta_get(meta, key);
	return v ? v : (char*)def;
}

static cdsl_rule_report_t*
execute_metric_rule(cdsl_vm_t* vm, const cdsl_rule_t* rule, cdsl_context_t* ctx)
{
	cdsl_rule_report_t* report = calloc(1, sizeof(*report));
	if (!report) {
		return NULL;
	}
	report->rule_name = rule->name ? strdup(rule->name) : strdup("");
	char* desc = cdsl_meta_get(rule->meta_list, "description");
	report->description = desc ? strdup(desc) : strdup("");
	if (!report->rule_name || !report->description) {
		cdsl_report_free(report);
		return NULL;
	}

	int metric_count = 0;
	for (cdsl_metric_node_t* m = rule->metrics; m; m = m->next) {
		metric_count++;
	}
	report->metric_count = metric_count;
	report->metrics = calloc(metric_count, sizeof(cdsl_metric_result_t));
	if (!report->metrics && metric_count > 0) {
		cdsl_report_free(report);
		return NULL;
	}

	int total_max = 0, total_obtained = 0;
	int any_critical_failed = 0;

	int idx = 0;
	for (cdsl_metric_node_t* m = rule->metrics; m; m = m->next, idx++) {
		if (CDSL_ATOMIC_LOAD(&vm->error_state)) {
			break;
		}
		cdsl_metric_result_t* mr = &report->metrics[idx];
		mr->metric_name = strdup(m->name);
		char* mdesc = cdsl_meta_get(m->meta_list, "description");
		mr->description = mdesc ? strdup(mdesc) : strdup("");
		mr->max_weight = safe_atoi(get_metric_meta(m->meta_list, "weight", "0"), 0);
		mr->is_critical =
		    (strcmp(get_metric_meta(m->meta_list, "is_critical", "false"), "true") == 0);

		total_max += mr->max_weight;

		int matched = 0;
		for (cdsl_case_node_t* c = m->case_list; c; c = c->next) {
			if (vm->debug_enabled) {
				fprintf(stderr,
					"[TRACE] eval CASE condition for metric '%s'\n",
					m->name);
			}
			cdsl_value_t cond =
			    cdsl_eval_expr_internal(c->condition, ctx, vm, vm->debug_enabled, 0);
			if (CDSL_ATOMIC_LOAD(&vm->error_state)) {
				break;
			}
			if (cond.type == CDSL_TYPE_BOOL && cond.data.bool_val) {
				if (vm->debug_enabled) {
					fprintf(stderr, "[TRACE]   CASE matched\n");
				}
				cdsl_trigger_action_internal(vm, c->action);
				if (c->action && strcmp(c->action->action_name, "score") == 0 &&
				    c->action->args) {
					cdsl_value_t sv = cdsl_eval_expr_internal(
					    c->action->args->expr, ctx, vm, vm->debug_enabled, 0);
					mr->score_obtained =
					    (sv.type == CDSL_TYPE_INT) ? sv.data.int_val : 0;
					if (CDSL_ATOMIC_LOAD(&vm->error_state)) {
						break;
					}
				} else {
					mr->score_obtained = mr->max_weight;
				}
				if (vm->debug_enabled) {
					fprintf(stderr,
						"[TRACE]   score obtained: %d/%d\n",
						mr->score_obtained,
						mr->max_weight);
				}
				mr->is_passed = (mr->score_obtained > 0);
				matched = 1;
				break;
			}
		}
		if (CDSL_ATOMIC_LOAD(&vm->error_state)) {
			break;
		}

		if (!matched) {
			if (vm->debug_enabled) {
				fprintf(stderr, "[TRACE]   no CASE matched, executing DEFAULT\n");
			}
			cdsl_trigger_action_internal(vm, m->default_action);
			if (!CDSL_ATOMIC_LOAD(&vm->error_state) && m->default_action &&
			    strcmp(m->default_action->action_name, "score") == 0 &&
			    m->default_action->args) {
				cdsl_value_t sv = cdsl_eval_expr_internal(
				    m->default_action->args->expr, ctx, vm, vm->debug_enabled, 0);
				mr->score_obtained =
				    (sv.type == CDSL_TYPE_INT) ? sv.data.int_val : 0;
			} else if (!CDSL_ATOMIC_LOAD(&vm->error_state) && m->default_action &&
				   strcmp(m->default_action->action_name, "fail_metric") == 0) {
				mr->score_obtained = 0;
				if (m->default_action->args && m->default_action->args->next) {
					cdsl_value_t rv = cdsl_eval_expr_internal(
					    m->default_action->args->next->expr,
					    ctx,
					    vm,
					    vm->debug_enabled,
					    0);
					if (!CDSL_ATOMIC_LOAD(&vm->error_state) &&
					    rv.type == CDSL_TYPE_STRING && rv.data.string_val) {
						mr->violation_reason = strdup(rv.data.string_val);
					}
				}
			} else {
				mr->score_obtained = 0;
			}
			mr->is_passed = (mr->score_obtained > 0);
			if (mr->violation_reason == NULL && !mr->is_passed) {
				mr->violation_reason = strdup("default_condition_triggered");
			}
		}

		if (mr->is_critical && !mr->is_passed) {
			any_critical_failed = 1;
		}
		total_obtained += mr->score_obtained;
	}

	report->total_max_score = total_max;
	report->total_obtained_score = total_obtained;

	if (any_critical_failed) {
		report->status = CDSL_STATUS_FAILED;
	} else {
		char* pass_str = cdsl_meta_get(rule->meta_list, "pass_threshold");
		char* partial_str = cdsl_meta_get(rule->meta_list, "partial_threshold");
		int pass_thresh = safe_atoi(pass_str, 80);
		int partial_thresh = safe_atoi(partial_str, 60);

		if (total_obtained >= pass_thresh) {
			report->status = CDSL_STATUS_PASSED;
		} else if (total_obtained >= partial_thresh) {
			report->status = CDSL_STATUS_PARTIALLY_PASSED;
		} else {
			report->status = CDSL_STATUS_FAILED;
		}
	}

	char buf[512];
	switch (report->status) {
	case CDSL_STATUS_PASSED:
		snprintf(buf, sizeof(buf), "PASSED (score: %d/%d)", total_obtained, total_max);
		break;
	case CDSL_STATUS_PARTIALLY_PASSED:
		snprintf(buf,
			 sizeof(buf),
			 "PARTIALLY PASSED (score: %d/%d, needs improvement)",
			 total_obtained,
			 total_max);
		break;
	case CDSL_STATUS_FAILED:
		snprintf(buf,
			 sizeof(buf),
			 "FAILED (score: %d/%d, critical items or threshold not met)",
			 total_obtained,
			 total_max);
		break;
	default:
		snprintf(buf, sizeof(buf), "ERROR");
		break;
	}
	report->decision_summary = strdup(buf);

	return report;
}

static cdsl_rule_report_t*
execute_simple_rule(cdsl_vm_t* vm,
		    const cdsl_rule_t* rule,
		    cdsl_context_t* ctx,
		    const cdsl_bytecode_t* bc)
{
	cdsl_rule_report_t* report = calloc(1, sizeof(*report));
	if (!report) {
		return NULL;
	}
	report->rule_name = strdup(rule->name);
	char* desc = cdsl_meta_get(rule->meta_list, "description");
	report->description = desc ? strdup(desc) : strdup("");
	if (!report->rule_name || !report->description) {
		cdsl_report_free(report);
		return NULL;
	}
	report->metric_count = 1;
	report->metrics = calloc(1, sizeof(cdsl_metric_result_t));
	if (!report->metrics) {
		cdsl_report_free(report);
		return NULL;
	}
	report->metrics[0].metric_name = rule->name ? strdup(rule->name) : strdup("");
	report->metrics[0].description =
	    report->description ? strdup(report->description) : strdup("");
	if (!report->metrics[0].metric_name || !report->metrics[0].description) {
		cdsl_report_free(report);
		return NULL;
	}

	if (vm->debug_enabled) {
		fprintf(stderr, "[TRACE] Evaluating simple rule '%s'\n", rule->name);
	}
	/* Abort if timeout or OOM */
	if (CDSL_ATOMIC_LOAD(&vm->error_state)) {
		cdsl_report_free(report);
		return NULL;
	}
	cdsl_value_t cond = evaluate_condition(rule->when_expr, ctx, vm, bc, vm->debug_enabled);
	if (CDSL_ATOMIC_LOAD(&vm->error_state)) {
		cdsl_report_free(report);
		return NULL;
	}
	fire_trace(vm, CDSL_TRACE_EXPR, rule->name, "WHEN", cond, 0);
	int triggered = (cond.type == CDSL_TYPE_BOOL) ? cond.data.bool_val : 0;
	if (vm->debug_enabled) {
		fprintf(stderr, "[TRACE] WHEN result: %s\n", triggered ? "true" : "false");
	}

	if (triggered) {
		report->metrics[0].score_obtained = 0;
		report->metrics[0].max_weight = 100;
		report->metrics[0].is_passed = 0;
		report->status = CDSL_STATUS_FAILED;
		report->total_max_score = 100;
		report->total_obtained_score = 0;
		cdsl_trigger_action_internal(vm, rule->then_action);
		fire_trace(vm,
			   CDSL_TRACE_ACTION,
			   rule->name,
			   rule->then_action ? rule->then_action->action_name : "?",
			   (cdsl_value_t){.type = CDSL_TYPE_VOID},
			   0);
	} else {
		report->metrics[0].score_obtained = 100;
		report->metrics[0].max_weight = 100;
		report->metrics[0].is_passed = 1;
		report->status = CDSL_STATUS_PASSED;
		report->total_max_score = 100;
		report->total_obtained_score = 100;
	}
	fire_trace(vm,
		   CDSL_TRACE_RULE,
		   rule->name,
		   report->status == CDSL_STATUS_PASSED ? "PASSED" : "FAILED",
		   (cdsl_value_t){.type = CDSL_TYPE_VOID},
		   0);

	char buf[256];
	snprintf(buf,
		 sizeof(buf),
		 "%s (score: %d/100)",
		 triggered ? "FAILED" : "PASSED",
		 report->total_obtained_score);
	report->decision_summary = strdup(buf);
	return report;
}

cdsl_rule_report_t*
cdsl_vm_execute_compiled(cdsl_vm_t* vm, cdsl_compiled_rule_t* compiled, cdsl_context_t* ctx)
{
	if (!vm || !compiled || !compiled->rule || !ctx) {
		return NULL;
	}
	if (compiled->bc.code) {
		return cdsl_bytecode_execute_rule(vm, &compiled->bc, ctx);
	}
	if (compiled->rule->metrics) {
		return execute_metric_rule(vm, compiled->rule, ctx);
	}
	return execute_simple_rule(vm, compiled->rule, ctx, &compiled->bc);
}

cdsl_rule_report_t*
cdsl_vm_execute(cdsl_vm_t* vm, const cdsl_rule_t* rule, cdsl_context_t* ctx)
{
	if (!vm || !rule || !ctx) {
		return NULL;
	}
	double t0 = cdsl_get_time_us_internal();
	cdsl_rule_report_t* rpt;
	/* Reset error state for each execution */
	CDSL_ATOMIC_STORE(&vm->error_state, 0);
	if (rule->metrics) {
		rpt = execute_metric_rule(vm, rule, ctx);
	} else {
		rpt = execute_simple_rule(vm, rule, ctx, NULL);
	}
	/* Check if aborted (instruction quota, timeout, etc.) */
	if (CDSL_ATOMIC_LOAD(&vm->error_state)) {
		if (!rpt) {
			rpt = calloc(1, sizeof(cdsl_rule_report_t));
		}
		if (rpt) {
			rpt->status = CDSL_STATUS_ERROR;
		}
	}
	double elapsed = cdsl_get_time_us_internal() - t0;
	vm->stats.total_executions++;
	vm->stats.total_rules_executed++;
	vm->stats.total_time_us += elapsed;
	if (rpt) {
		vm->stats.total_metrics_evaluated += rpt->metric_count;
	}
	return rpt;
}

void
cdsl_report_free(cdsl_rule_report_t* report)
{
	if (!report) {
		return;
	}
	free(report->rule_name);
	free(report->description);
	free(report->decision_summary);
	if (report->metrics) {
		for (int i = 0; i < report->metric_count; i++) {
			cdsl_metric_result_t* m = &report->metrics[i];
			free(m->metric_name);
			free(m->description);
			free(m->matched_case_expr);
			free(m->violation_reason);
		}
		free(report->metrics);
	}
	free(report);
}

void
cdsl_report_print(const cdsl_rule_report_t* report)
{
	if (!report) {
		printf("No report.\n");
		return;
	}
	printf("\n========================================\n");
	printf("  AUDIT REPORT: %s\n", report->rule_name);
	printf("  %s\n", report->description);
	printf("========================================\n");

	for (int i = 0; i < report->metric_count; i++) {
		cdsl_metric_result_t* m = &report->metrics[i];
		printf("  [%s] %s (weight: %d, score: %d/%d)%s\n",
		       m->is_passed ? "PASS" : "FAIL",
		       m->metric_name,
		       m->max_weight,
		       m->score_obtained,
		       m->max_weight,
		       m->is_critical ? " *" : "");
		if (m->violation_reason) {
			printf("         Reason: %s\n", m->violation_reason);
		}
	}

	printf("----------------------------------------\n");
	printf("  Status:   %s\n", cdsl_status_str_internal(report->status));
	printf("  Score:    %d / %d\n", report->total_obtained_score, report->total_max_score);
	printf("  Summary:  %s\n", report->decision_summary);
	printf("========================================\n\n");
}

char*
cdsl_report_to_json(const cdsl_rule_report_t* report)
{
	if (!report) {
		return strdup("{}");
	}

	size_t cap = 1024;
	char* json = malloc(cap);
	if (!json) {
		return NULL;
	}

	size_t off = 0;

	size_t rn_esc_len = json_escape_len(report->rule_name ? report->rule_name : "");
	size_t desc_esc_len = json_escape_len(report->description ? report->description : "");
	size_t ds_esc_len =
	    json_escape_len(report->decision_summary ? report->decision_summary : "");

	size_t needed = 256 + rn_esc_len + desc_esc_len + ds_esc_len;
	if (needed > cap) {
		cap = needed * 2;
		char* new_json = realloc(json, cap);
		if (!new_json) {
			free(json);
			return NULL;
		}
		json = new_json;
	}

#define JSON_CHECK_CAP(n)                                                                          \
	do {                                                                                       \
		if (off + (n) + 1 > cap) {                                                         \
			cap = (off + (n) + 1) * 2;                                                 \
			char* new_j = realloc(json, cap);                                          \
			if (!new_j) {                                                              \
				free(json);                                                        \
				return NULL;                                                       \
			}                                                                          \
			json = new_j;                                                              \
		}                                                                                  \
	} while (0)

	off += snprintf(json + off, cap - off, "{\"rule_name\":\"");
	off += json_escape_buf(json + off, report->rule_name ? report->rule_name : "", cap - off);
	off += snprintf(json + off, cap - off, "\",\"description\":\"");
	off +=
	    json_escape_buf(json + off, report->description ? report->description : "", cap - off);
	off += snprintf(json + off,
			cap - off,
			"\",\"status\":\"%s\",\"total_max_score\":%d,\"total_obtained_score\":%d,"
			"\"decision_summary\":\"",
			cdsl_status_str_internal(report->status),
			report->total_max_score,
			report->total_obtained_score);
	off += json_escape_buf(
	    json + off, report->decision_summary ? report->decision_summary : "", cap - off);
	off += snprintf(json + off, cap - off, "\",\"metrics\":[");

	for (int i = 0; i < report->metric_count; i++) {
		cdsl_metric_result_t* m = &report->metrics[i];
		size_t mn_esc = json_escape_len(m->metric_name ? m->metric_name : "");
		size_t md_esc = json_escape_len(m->description ? m->description : "");
		size_t vr_esc = m->violation_reason ? json_escape_len(m->violation_reason) : 0;

		JSON_CHECK_CAP(256 + mn_esc + md_esc + vr_esc);

		if (i > 0) {
			off += snprintf(json + off, cap - off, ",");
		}
		off += snprintf(json + off, cap - off, "{\"metric_name\":\"");
		off += json_escape_buf(json + off, m->metric_name ? m->metric_name : "", cap - off);
		off += snprintf(json + off, cap - off, "\",\"description\":\"");
		off += json_escape_buf(json + off, m->description ? m->description : "", cap - off);
		off += snprintf(json + off,
				cap - off,
				"\",\"max_weight\":%d,\"score_obtained\":%d,\"is_critical\":%d,"
				"\"is_passed\":%d",
				m->max_weight,
				m->score_obtained,
				m->is_critical,
				m->is_passed);
		if (m->violation_reason) {
			off += snprintf(json + off, cap - off, ",\"violation_reason\":\"");
			off += json_escape_buf(json + off, m->violation_reason, cap - off);
			off += snprintf(json + off, cap - off, "\"");
		}
		off += snprintf(json + off, cap - off, "}");
	}

	JSON_CHECK_CAP(4);
	snprintf(json + off, cap - off, "]}");

#undef JSON_CHECK_CAP

	return json;
}
/** @} */
