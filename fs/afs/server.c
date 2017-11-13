/* AFS server record management
 *
 * Copyright (C) 2002, 2007 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/sched.h>
#include <linux/slab.h>
#include "afs_fs.h"
#include "internal.h"

static unsigned afs_server_timeout = 10;	/* server timeout in seconds */

static void afs_inc_servers_outstanding(struct afs_net *net)
{
	atomic_inc(&net->servers_outstanding);
}

static void afs_dec_servers_outstanding(struct afs_net *net)
{
	if (atomic_dec_and_test(&net->servers_outstanding))
		wake_up_atomic_t(&net->servers_outstanding);
}

void afs_server_timer(struct timer_list *timer)
{
	struct afs_net *net = container_of(timer, struct afs_net, server_timer);

	if (!queue_work(afs_wq, &net->server_reaper))
		afs_dec_servers_outstanding(net);
}

/*
 * install a server record in the master tree
 */
static int afs_install_server(struct afs_server *server)
{
	struct afs_server *xserver;
	struct afs_net *net = server->cell->net;
	struct rb_node **pp, *p;
	int ret, diff;

	_enter("%p", server);

	write_lock(&net->servers_lock);

	ret = -EEXIST;
	pp = &net->servers.rb_node;
	p = NULL;
	while (*pp) {
		p = *pp;
		_debug("- consider %p", p);
		xserver = rb_entry(p, struct afs_server, master_rb);
		diff = memcmp(&server->addrs->addrs[0],
			      &xserver->addrs->addrs[0],
			      sizeof(sizeof(server->addrs->addrs[0])));
		if (diff < 0)
			pp = &(*pp)->rb_left;
		else if (diff > 0)
			pp = &(*pp)->rb_right;
		else
			goto error;
	}

	rb_link_node(&server->master_rb, p, pp);
	rb_insert_color(&server->master_rb, &net->servers);
	ret = 0;

error:
	write_unlock(&net->servers_lock);
	return ret;
}

/*
 * allocate a new server record
 */
static struct afs_server *afs_alloc_server(struct afs_cell *cell,
					   const struct sockaddr_rxrpc *addr)
{
	struct afs_server *server;

	_enter("");

	server = kzalloc(sizeof(struct afs_server), GFP_KERNEL);
	if (!server)
		goto enomem;
	server->addrs = kzalloc(sizeof(struct afs_addr_list) +
				sizeof(struct sockaddr_rxrpc),
				GFP_KERNEL);
	if (!server->addrs)
		goto enomem_server;

	atomic_set(&server->usage, 1);
	server->net = cell->net;
	server->cell = cell;

	INIT_LIST_HEAD(&server->link);
	INIT_LIST_HEAD(&server->grave);
	init_rwsem(&server->sem);
	spin_lock_init(&server->fs_lock);
	INIT_LIST_HEAD(&server->cb_interests);
	rwlock_init(&server->cb_break_lock);

	refcount_set(&server->addrs->usage, 1);
	server->addrs->nr_addrs = 1;
	server->addrs->addrs[0] = *addr;
	afs_inc_servers_outstanding(cell->net);

	_leave(" = %p{%d}", server, atomic_read(&server->usage));
	return server;

enomem_server:
	kfree(server);
enomem:
	_leave(" = NULL [nomem]");
	return NULL;
}

/*
 * get an FS-server record for a cell
 */
struct afs_server *afs_lookup_server(struct afs_cell *cell,
				     struct sockaddr_rxrpc *addr)
{
	struct afs_server *server, *candidate;

	_enter("%p,%pIS", cell, &addr->transport);

	/* quick scan of the list to see if we already have the server */
	read_lock(&cell->servers_lock);

	list_for_each_entry(server, &cell->servers, link) {
		if (memcmp(&server->addrs->addrs[0], addr, sizeof(*addr)) == 0)
			goto found_server_quickly;
	}
	read_unlock(&cell->servers_lock);

	candidate = afs_alloc_server(cell, addr);
	if (!candidate) {
		_leave(" = -ENOMEM");
		return ERR_PTR(-ENOMEM);
	}

	write_lock(&cell->servers_lock);

	/* check the cell's server list again */
	list_for_each_entry(server, &cell->servers, link) {
		if (memcmp(&server->addrs->addrs[0], addr, sizeof(*addr)) == 0)
			goto found_server;
	}

	_debug("new");
	server = candidate;
	if (afs_install_server(server) < 0)
		goto server_in_two_cells;

	afs_get_cell(cell);
	list_add_tail(&server->link, &cell->servers);

	write_unlock(&cell->servers_lock);
	_leave(" = %p{%d}", server, atomic_read(&server->usage));
	return server;

	/* found a matching server quickly */
found_server_quickly:
	_debug("found quickly");
	afs_get_server(server);
	read_unlock(&cell->servers_lock);
no_longer_unused:
	if (!list_empty(&server->grave)) {
		spin_lock(&cell->net->server_graveyard_lock);
		list_del_init(&server->grave);
		spin_unlock(&cell->net->server_graveyard_lock);
	}
	_leave(" = %p{%d}", server, atomic_read(&server->usage));
	return server;

	/* found a matching server on the second pass */
found_server:
	_debug("found");
	afs_get_server(server);
	write_unlock(&cell->servers_lock);
	kfree(candidate);
	goto no_longer_unused;

	/* found a server that seems to be in two cells */
server_in_two_cells:
	write_unlock(&cell->servers_lock);
	kfree(candidate);
	afs_dec_servers_outstanding(cell->net);
	printk(KERN_NOTICE "kAFS: Server %pI4 appears to be in two cells\n",
	       addr);
	_leave(" = -EEXIST");
	return ERR_PTR(-EEXIST);
}

/*
 * look up a server by its IP address
 */
struct afs_server *afs_find_server(struct afs_net *net,
				   const struct sockaddr_rxrpc *srx)
{
	struct afs_server *server = NULL;
	struct rb_node *p;
	int diff;

	_enter("{%d,%pIS}", srx->transport.family, &srx->transport);

	read_lock(&net->servers_lock);

	p = net->servers.rb_node;
	while (p) {
		server = rb_entry(p, struct afs_server, master_rb);

		_debug("- consider %p", p);

		diff = memcmp(srx, &server->addrs->addrs[0], sizeof(*srx));
		if (diff < 0) {
			p = p->rb_left;
		} else if (diff > 0) {
			p = p->rb_right;
		} else {
			afs_get_server(server);
			goto found;
		}
	}

	server = NULL;
found:
	read_unlock(&net->servers_lock);
	_leave(" = %p", server);
	return server;
}

static void afs_set_server_timer(struct afs_net *net, time64_t delay)
{
	afs_inc_servers_outstanding(net);
	if (net->live) {
		if (timer_reduce(&net->server_timer, jiffies + delay * HZ))
			afs_dec_servers_outstanding(net);
	} else {
		if (!queue_work(afs_wq, &net->server_reaper))
			afs_dec_servers_outstanding(net);
	}
}

/*
 * destroy a server record
 * - removes from the cell list
 */
void afs_put_server(struct afs_net *net, struct afs_server *server)
{
	if (!server)
		return;

	_enter("%p{%d}", server, atomic_read(&server->usage));

	_debug("PUT SERVER %d", atomic_read(&server->usage));

	ASSERTCMP(atomic_read(&server->usage), >, 0);

	if (likely(!atomic_dec_and_test(&server->usage))) {
		_leave("");
		return;
	}

	spin_lock(&net->server_graveyard_lock);
	if (atomic_read(&server->usage) == 0) {
		list_move_tail(&server->grave, &net->server_graveyard);
		server->time_of_death = ktime_get_real_seconds();
		afs_set_server_timer(net, afs_server_timeout);
	}
	spin_unlock(&net->server_graveyard_lock);
	_leave(" [dead]");
}

/*
 * destroy a dead server
 */
static void afs_destroy_server(struct afs_net *net, struct afs_server *server)
{
	struct afs_addr_list *alist = server->addrs;
	struct afs_addr_cursor ac = {
		.alist	= alist,
		.addr	= &alist->addrs[0],
		.start	= alist->index,
		.index	= alist->index,
		.error	= 0,
	};
	_enter("%p", server);

	afs_fs_give_up_all_callbacks(server, &ac, NULL, false);
	afs_put_cell(net, server->cell);
	afs_put_addrlist(server->addrs);
	kfree(server);
	afs_dec_servers_outstanding(net);
}

/*
 * reap dead server records
 */
void afs_reap_server(struct work_struct *work)
{
	LIST_HEAD(corpses);
	struct afs_server *server;
	struct afs_net *net = container_of(work, struct afs_net, server_reaper);
	unsigned long delay, expiry;
	time64_t now;

	now = ktime_get_real_seconds();
	spin_lock(&net->server_graveyard_lock);

	while (!list_empty(&net->server_graveyard)) {
		server = list_entry(net->server_graveyard.next,
				    struct afs_server, grave);

		/* the queue is ordered most dead first */
		if (net->live) {
			expiry = server->time_of_death + afs_server_timeout;
			if (expiry > now) {
				delay = (expiry - now);
				afs_set_server_timer(net, delay);
				break;
			}
		}

		write_lock(&server->cell->servers_lock);
		write_lock(&net->servers_lock);
		if (atomic_read(&server->usage) > 0) {
			list_del_init(&server->grave);
		} else {
			list_move_tail(&server->grave, &corpses);
			list_del_init(&server->link);
			rb_erase(&server->master_rb, &net->servers);
		}
		write_unlock(&net->servers_lock);
		write_unlock(&server->cell->servers_lock);
	}

	spin_unlock(&net->server_graveyard_lock);

	/* now reap the corpses we've extracted */
	while (!list_empty(&corpses)) {
		server = list_entry(corpses.next, struct afs_server, grave);
		list_del(&server->grave);
		afs_destroy_server(net, server);
	}

	afs_dec_servers_outstanding(net);
}

/*
 * Discard all the server records from a net namespace when it is destroyed or
 * the afs module is removed.
 */
void __net_exit afs_purge_servers(struct afs_net *net)
{
	if (del_timer_sync(&net->server_timer))
		atomic_dec(&net->servers_outstanding);

	afs_inc_servers_outstanding(net);
	if (!queue_work(afs_wq, &net->server_reaper))
		afs_dec_servers_outstanding(net);

	wait_on_atomic_t(&net->servers_outstanding, atomic_t_wait,
			 TASK_UNINTERRUPTIBLE);
}
