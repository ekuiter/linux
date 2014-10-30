/*
 * db-export.c: Support for exporting data suitable for import to a database
 * Copyright (c) 2014, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <errno.h>

#include "evsel.h"
#include "machine.h"
#include "thread.h"
#include "comm.h"
#include "symbol.h"
#include "event.h"
#include "db-export.h"

int db_export__init(struct db_export *dbe)
{
	memset(dbe, 0, sizeof(struct db_export));
	return 0;
}

void db_export__exit(struct db_export *dbe __maybe_unused)
{
}

int db_export__evsel(struct db_export *dbe, struct perf_evsel *evsel)
{
	if (evsel->db_id)
		return 0;

	evsel->db_id = ++dbe->evsel_last_db_id;

	if (dbe->export_evsel)
		return dbe->export_evsel(dbe, evsel);

	return 0;
}

int db_export__machine(struct db_export *dbe, struct machine *machine)
{
	if (machine->db_id)
		return 0;

	machine->db_id = ++dbe->machine_last_db_id;

	if (dbe->export_machine)
		return dbe->export_machine(dbe, machine);

	return 0;
}

int db_export__thread(struct db_export *dbe, struct thread *thread,
		      struct machine *machine, struct comm *comm)
{
	u64 main_thread_db_id = 0;
	int err;

	if (thread->db_id)
		return 0;

	thread->db_id = ++dbe->thread_last_db_id;

	if (thread->pid_ != -1) {
		struct thread *main_thread;

		if (thread->pid_ == thread->tid) {
			main_thread = thread;
		} else {
			main_thread = machine__findnew_thread(machine,
							      thread->pid_,
							      thread->pid_);
			if (!main_thread)
				return -ENOMEM;
			err = db_export__thread(dbe, main_thread, machine,
						comm);
			if (err)
				return err;
			if (comm) {
				err = db_export__comm_thread(dbe, comm, thread);
				if (err)
					return err;
			}
		}
		main_thread_db_id = main_thread->db_id;
	}

	if (dbe->export_thread)
		return dbe->export_thread(dbe, thread, main_thread_db_id,
					  machine);

	return 0;
}

int db_export__comm(struct db_export *dbe, struct comm *comm,
		    struct thread *main_thread)
{
	int err;

	if (comm->db_id)
		return 0;

	comm->db_id = ++dbe->comm_last_db_id;

	if (dbe->export_comm) {
		err = dbe->export_comm(dbe, comm);
		if (err)
			return err;
	}

	return db_export__comm_thread(dbe, comm, main_thread);
}

int db_export__comm_thread(struct db_export *dbe, struct comm *comm,
			   struct thread *thread)
{
	u64 db_id;

	db_id = ++dbe->comm_thread_last_db_id;

	if (dbe->export_comm_thread)
		return dbe->export_comm_thread(dbe, db_id, comm, thread);

	return 0;
}

int db_export__dso(struct db_export *dbe, struct dso *dso,
		   struct machine *machine)
{
	if (dso->db_id)
		return 0;

	dso->db_id = ++dbe->dso_last_db_id;

	if (dbe->export_dso)
		return dbe->export_dso(dbe, dso, machine);

	return 0;
}

int db_export__symbol(struct db_export *dbe, struct symbol *sym,
		      struct dso *dso)
{
	u64 *sym_db_id = symbol__priv(sym);

	if (*sym_db_id)
		return 0;

	*sym_db_id = ++dbe->symbol_last_db_id;

	if (dbe->export_symbol)
		return dbe->export_symbol(dbe, sym, dso);

	return 0;
}

static struct thread *get_main_thread(struct machine *machine, struct thread *thread)
{
	if (thread->pid_ == thread->tid)
		return thread;

	if (thread->pid_ == -1)
		return NULL;

	return machine__find_thread(machine, thread->pid_, thread->pid_);
}

static int db_ids_from_al(struct db_export *dbe, struct addr_location *al,
			  u64 *dso_db_id, u64 *sym_db_id, u64 *offset)
{
	int err;

	if (al->map) {
		struct dso *dso = al->map->dso;

		err = db_export__dso(dbe, dso, al->machine);
		if (err)
			return err;
		*dso_db_id = dso->db_id;

		if (!al->sym) {
			al->sym = symbol__new(al->addr, 0, 0, "unknown");
			if (al->sym)
				symbols__insert(&dso->symbols[al->map->type],
						al->sym);
		}

		if (al->sym) {
			u64 *db_id = symbol__priv(al->sym);

			err = db_export__symbol(dbe, al->sym, dso);
			if (err)
				return err;
			*sym_db_id = *db_id;
			*offset = al->addr - al->sym->start;
		}
	}

	return 0;
}

int db_export__sample(struct db_export *dbe, union perf_event *event,
		      struct perf_sample *sample, struct perf_evsel *evsel,
		      struct thread *thread, struct addr_location *al)
{
	struct export_sample es = {
		.event = event,
		.sample = sample,
		.evsel = evsel,
		.thread = thread,
		.al = al,
	};
	struct thread *main_thread;
	struct comm *comm = NULL;
	int err;

	err = db_export__evsel(dbe, evsel);
	if (err)
		return err;

	err = db_export__machine(dbe, al->machine);
	if (err)
		return err;

	main_thread = get_main_thread(al->machine, thread);
	if (main_thread)
		comm = machine__thread_exec_comm(al->machine, main_thread);

	err = db_export__thread(dbe, thread, al->machine, comm);
	if (err)
		return err;

	if (comm) {
		err = db_export__comm(dbe, comm, main_thread);
		if (err)
			return err;
		es.comm_db_id = comm->db_id;
	}

	es.db_id = ++dbe->sample_last_db_id;

	err = db_ids_from_al(dbe, al, &es.dso_db_id, &es.sym_db_id, &es.offset);
	if (err)
		return err;

	if ((evsel->attr.sample_type & PERF_SAMPLE_ADDR) &&
	    sample_addr_correlates_sym(&evsel->attr)) {
		struct addr_location addr_al;

		perf_event__preprocess_sample_addr(event, sample, thread, &addr_al);
		err = db_ids_from_al(dbe, &addr_al, &es.addr_dso_db_id,
				     &es.addr_sym_db_id, &es.addr_offset);
		if (err)
			return err;
	}

	if (dbe->export_sample)
		return dbe->export_sample(dbe, &es);

	return 0;
}
