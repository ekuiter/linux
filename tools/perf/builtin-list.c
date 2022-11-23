// SPDX-License-Identifier: GPL-2.0
/*
 * builtin-list.c
 *
 * Builtin list command: list all event types
 *
 * Copyright (C) 2009, Thomas Gleixner <tglx@linutronix.de>
 * Copyright (C) 2008-2009, Red Hat Inc, Ingo Molnar <mingo@redhat.com>
 * Copyright (C) 2011, Red Hat Inc, Arnaldo Carvalho de Melo <acme@redhat.com>
 */
#include "builtin.h"

#include "util/print-events.h"
#include "util/pmu.h"
#include "util/pmu-hybrid.h"
#include "util/debug.h"
#include "util/metricgroup.h"
#include "util/string2.h"
#include "util/strlist.h"
#include <subcmd/pager.h>
#include <subcmd/parse-options.h>
#include <stdio.h>

/**
 * struct print_state - State and configuration passed to the default_print
 * functions.
 */
struct print_state {
	/**
	 * @pmu_glob: Optionally restrict PMU and metric matching to PMU or
	 * debugfs subsystem name.
	 */
	char *pmu_glob;
	/** @event_glob: Optional pattern matching glob. */
	char *event_glob;
	/** @name_only: Print event or metric names only. */
	bool name_only;
	/** @desc: Print the event or metric description. */
	bool desc;
	/** @long_desc: Print longer event or metric description. */
	bool long_desc;
	/** @deprecated: Print deprecated events or metrics. */
	bool deprecated;
	/**
	 * @detailed: Print extra information on the perf event such as names
	 * and expressions used internally by events.
	 */
	bool detailed;
	/** @metrics: Controls printing of metric and metric groups. */
	bool metrics;
	/** @metricgroups: Controls printing of metric and metric groups. */
	bool metricgroups;
	/** @last_topic: The last printed event topic. */
	char *last_topic;
	/** @last_metricgroups: The last printed metric group. */
	char *last_metricgroups;
	/** @visited_metrics: Metrics that are printed to avoid duplicates. */
	struct strlist *visited_metrics;
};

static void default_print_start(void *ps)
{
	struct print_state *print_state = ps;

	if (!print_state->name_only && pager_in_use())
		printf("\nList of pre-defined events (to be used in -e or -M):\n\n");
}

static void default_print_end(void *print_state __maybe_unused) {}

static void wordwrap(const char *s, int start, int max, int corr)
{
	int column = start;
	int n;

	while (*s) {
		int wlen = strcspn(s, " \t");

		if (column + wlen >= max && column > start) {
			printf("\n%*s", start, "");
			column = start + corr;
		}
		n = printf("%s%.*s", column > start ? " " : "", wlen, s);
		if (n <= 0)
			break;
		s += wlen;
		column += n;
		s = skip_spaces(s);
	}
}

static void default_print_event(void *ps, const char *pmu_name, const char *topic,
				const char *event_name, const char *event_alias,
				const char *scale_unit __maybe_unused,
				bool deprecated, const char *event_type_desc,
				const char *desc, const char *long_desc,
				const char *encoding_desc,
				const char *metric_name, const char *metric_expr)
{
	struct print_state *print_state = ps;
	int pos;

	if (deprecated && !print_state->deprecated)
		return;

	if (print_state->pmu_glob && pmu_name && !strglobmatch(pmu_name, print_state->pmu_glob))
		return;

	if (print_state->event_glob &&
	    (!event_name || !strglobmatch(event_name, print_state->event_glob)) &&
	    (!event_alias || !strglobmatch(event_alias, print_state->event_glob)) &&
	    (!topic || !strglobmatch_nocase(topic, print_state->event_glob)))
		return;

	if (print_state->name_only) {
		if (event_alias && strlen(event_alias))
			printf("%s ", event_alias);
		else
			printf("%s ", event_name);
		return;
	}

	if (strcmp(print_state->last_topic, topic ?: "")) {
		if (topic)
			printf("\n%s:\n", topic);
		free(print_state->last_topic);
		print_state->last_topic = strdup(topic ?: "");
	}

	if (event_alias && strlen(event_alias))
		pos = printf("  %s OR %s", event_name, event_alias);
	else
		pos = printf("  %s", event_name);

	if (!topic && event_type_desc) {
		for (; pos < 53; pos++)
			putchar(' ');
		printf("[%s]\n", event_type_desc);
	} else
		putchar('\n');

	if (desc && print_state->desc) {
		printf("%*s", 8, "[");
		wordwrap(desc, 8, pager_get_columns(), 0);
		printf("]\n");
	}

	if (long_desc && print_state->long_desc) {
		printf("%*s", 8, "[");
		wordwrap(long_desc, 8, pager_get_columns(), 0);
		printf("]\n");
	}

	if (print_state->detailed && encoding_desc) {
		printf("%*s%s", 8, "", encoding_desc);
		if (metric_name)
			printf(" MetricName: %s", metric_name);
		if (metric_expr)
			printf(" MetricExpr: %s", metric_expr);
		putchar('\n');
	}
}

static void default_print_metric(void *ps,
				const char *group,
				const char *name,
				const char *desc,
				const char *long_desc,
				const char *expr,
				const char *unit __maybe_unused)
{
	struct print_state *print_state = ps;

	if (print_state->event_glob &&
	    (!print_state->metrics || !name || !strglobmatch(name, print_state->event_glob)) &&
	    (!print_state->metricgroups || !group || !strglobmatch(group, print_state->event_glob)))
		return;

	if (!print_state->name_only && !print_state->last_metricgroups) {
		if (print_state->metricgroups) {
			printf("\nMetric Groups:\n");
			if (!print_state->metrics)
				putchar('\n');
		} else {
			printf("\nMetrics:\n\n");
		}
	}
	if (!print_state->last_metricgroups ||
	    strcmp(print_state->last_metricgroups, group ?: "")) {
		if (group && print_state->metricgroups) {
			if (print_state->name_only)
				printf("%s ", group);
			else if (print_state->metrics)
				printf("\n%s:\n", group);
			else
				printf("%s\n", group);
		}
		free(print_state->last_metricgroups);
		print_state->last_metricgroups = strdup(group ?: "");
	}
	if (!print_state->metrics)
		return;

	if (print_state->name_only) {
		if (print_state->metrics &&
		    !strlist__has_entry(print_state->visited_metrics, name)) {
			printf("%s ", name);
			strlist__add(print_state->visited_metrics, name);
		}
		return;
	}
	printf("  %s\n", name);

	if (desc && print_state->desc) {
		printf("%*s", 8, "[");
		wordwrap(desc, 8, pager_get_columns(), 0);
		printf("]\n");
	}
	if (long_desc && print_state->long_desc) {
		printf("%*s", 8, "[");
		wordwrap(long_desc, 8, pager_get_columns(), 0);
		printf("]\n");
	}
	if (expr && print_state->detailed) {
		printf("%*s", 8, "[");
		wordwrap(expr, 8, pager_get_columns(), 0);
		printf("]\n");
	}
}

int cmd_list(int argc, const char **argv)
{
	int i, ret = 0;
	struct print_state ps = {};
	struct print_callbacks print_cb = {
		.print_start = default_print_start,
		.print_end = default_print_end,
		.print_event = default_print_event,
		.print_metric = default_print_metric,
	};
	const char *hybrid_name = NULL;
	const char *unit_name = NULL;
	struct option list_options[] = {
		OPT_BOOLEAN(0, "raw-dump", &ps.name_only, "Dump raw events"),
		OPT_BOOLEAN('d', "desc", &ps.desc,
			    "Print extra event descriptions. --no-desc to not print."),
		OPT_BOOLEAN('v', "long-desc", &ps.long_desc,
			    "Print longer event descriptions."),
		OPT_BOOLEAN(0, "details", &ps.detailed,
			    "Print information on the perf event names and expressions used internally by events."),
		OPT_BOOLEAN(0, "deprecated", &ps.deprecated,
			    "Print deprecated events."),
		OPT_STRING(0, "cputype", &hybrid_name, "hybrid cpu type",
			   "Limit PMU or metric printing to the given hybrid PMU (e.g. core or atom)."),
		OPT_STRING(0, "unit", &unit_name, "PMU name",
			   "Limit PMU or metric printing to the specified PMU."),
		OPT_INCR(0, "debug", &verbose,
			     "Enable debugging output"),
		OPT_END()
	};
	const char * const list_usage[] = {
		"perf list [<options>] [hw|sw|cache|tracepoint|pmu|sdt|metric|metricgroup|event_glob]",
		NULL
	};

	set_option_flag(list_options, 0, "raw-dump", PARSE_OPT_HIDDEN);
	/* Hide hybrid flag for the more generic 'unit' flag. */
	set_option_flag(list_options, 0, "cputype", PARSE_OPT_HIDDEN);

	argc = parse_options(argc, argv, list_options, list_usage,
			     PARSE_OPT_STOP_AT_NON_OPTION);

	setup_pager();

	if (!ps.name_only)
		setup_pager();

	ps.desc = !ps.long_desc;
	ps.last_topic = strdup("");
	assert(ps.last_topic);
	ps.visited_metrics = strlist__new(NULL, NULL);
	assert(ps.visited_metrics);
	if (unit_name)
		ps.pmu_glob = strdup(unit_name);
	else if (hybrid_name) {
		ps.pmu_glob = perf_pmu__hybrid_type_to_pmu(hybrid_name);
		if (!ps.pmu_glob)
			pr_warning("WARNING: hybrid cputype is not supported!\n");
	}

	print_cb.print_start(&ps);

	if (argc == 0) {
		ps.metrics = true;
		ps.metricgroups = true;
		print_events(&print_cb, &ps);
		goto out;
	}

	for (i = 0; i < argc; ++i) {
		char *sep, *s;

		if (strcmp(argv[i], "tracepoint") == 0)
			print_tracepoint_events(&print_cb, &ps);
		else if (strcmp(argv[i], "hw") == 0 ||
			 strcmp(argv[i], "hardware") == 0)
			print_symbol_events(&print_cb, &ps, PERF_TYPE_HARDWARE,
					event_symbols_hw, PERF_COUNT_HW_MAX);
		else if (strcmp(argv[i], "sw") == 0 ||
			 strcmp(argv[i], "software") == 0) {
			print_symbol_events(&print_cb, &ps, PERF_TYPE_SOFTWARE,
					event_symbols_sw, PERF_COUNT_SW_MAX);
			print_tool_events(&print_cb, &ps);
		} else if (strcmp(argv[i], "cache") == 0 ||
			 strcmp(argv[i], "hwcache") == 0)
			print_hwcache_events(&print_cb, &ps);
		else if (strcmp(argv[i], "pmu") == 0)
			print_pmu_events(&print_cb, &ps);
		else if (strcmp(argv[i], "sdt") == 0)
			print_sdt_events(&print_cb, &ps);
		else if (strcmp(argv[i], "metric") == 0 || strcmp(argv[i], "metrics") == 0) {
			ps.metricgroups = false;
			ps.metrics = true;
			metricgroup__print(&print_cb, &ps);
		} else if (strcmp(argv[i], "metricgroup") == 0 ||
			   strcmp(argv[i], "metricgroups") == 0) {
			ps.metricgroups = true;
			ps.metrics = false;
			metricgroup__print(&print_cb, &ps);
		} else if ((sep = strchr(argv[i], ':')) != NULL) {
			int sep_idx;
			char *old_pmu_glob = ps.pmu_glob;

			sep_idx = sep - argv[i];
			s = strdup(argv[i]);
			if (s == NULL) {
				ret = -1;
				goto out;
			}

			s[sep_idx] = '\0';
			ps.pmu_glob = s;
			ps.event_glob = s + sep_idx + 1;
			print_tracepoint_events(&print_cb, &ps);
			print_sdt_events(&print_cb, &ps);
			ps.metrics = true;
			ps.metricgroups = true;
			metricgroup__print(&print_cb, &ps);
			free(s);
			ps.pmu_glob = old_pmu_glob;
		} else {
			if (asprintf(&s, "*%s*", argv[i]) < 0) {
				printf("Critical: Not enough memory! Trying to continue...\n");
				continue;
			}
			ps.event_glob = s;
			print_symbol_events(&print_cb, &ps, PERF_TYPE_HARDWARE,
					event_symbols_hw, PERF_COUNT_HW_MAX);
			print_symbol_events(&print_cb, &ps, PERF_TYPE_SOFTWARE,
					event_symbols_sw, PERF_COUNT_SW_MAX);
			print_tool_events(&print_cb, &ps);
			print_hwcache_events(&print_cb, &ps);
			print_pmu_events(&print_cb, &ps);
			print_tracepoint_events(&print_cb, &ps);
			print_sdt_events(&print_cb, &ps);
			ps.metrics = true;
			ps.metricgroups = true;
			metricgroup__print(&print_cb, &ps);
			free(s);
		}
	}

out:
	print_cb.print_end(&ps);
	free(ps.pmu_glob);
	free(ps.last_topic);
	free(ps.last_metricgroups);
	strlist__delete(ps.visited_metrics);
	return ret;
}
