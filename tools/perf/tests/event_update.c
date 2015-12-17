#include <linux/compiler.h>
#include "evlist.h"
#include "evsel.h"
#include "machine.h"
#include "tests.h"
#include "debug.h"

static int process_event_unit(struct perf_tool *tool __maybe_unused,
			      union perf_event *event,
			      struct perf_sample *sample __maybe_unused,
			      struct machine *machine __maybe_unused)
{
	struct event_update_event *ev = (struct event_update_event *) event;

	TEST_ASSERT_VAL("wrong id", ev->id == 123);
	TEST_ASSERT_VAL("wrong id", ev->type == PERF_EVENT_UPDATE__UNIT);
	TEST_ASSERT_VAL("wrong unit", !strcmp(ev->data, "KRAVA"));
	return 0;
}

int test__event_update(int subtest __maybe_unused)
{
	struct perf_evlist *evlist;
	struct perf_evsel *evsel;

	evlist = perf_evlist__new_default();
	TEST_ASSERT_VAL("failed to get evlist", evlist);

	evsel = perf_evlist__first(evlist);

	TEST_ASSERT_VAL("failed to allos ids",
			!perf_evsel__alloc_id(evsel, 1, 1));

	perf_evlist__id_add(evlist, evsel, 0, 0, 123);

	evsel->unit = strdup("KRAVA");

	TEST_ASSERT_VAL("failed to synthesize attr update unit",
			!perf_event__synthesize_event_update_unit(NULL, evsel, process_event_unit));

	return 0;
}
