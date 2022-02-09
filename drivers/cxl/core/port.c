// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2020 Intel Corporation. All rights reserved. */
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <cxlmem.h>
#include <cxl.h>
#include "core.h"

/**
 * DOC: cxl core
 *
 * The CXL core provides a set of interfaces that can be consumed by CXL aware
 * drivers. The interfaces allow for creation, modification, and destruction of
 * regions, memory devices, ports, and decoders. CXL aware drivers must register
 * with the CXL core via these interfaces in order to be able to participate in
 * cross-device interleave coordination. The CXL core also establishes and
 * maintains the bridge to the nvdimm subsystem.
 *
 * CXL core introduces sysfs hierarchy to control the devices that are
 * instantiated by the core.
 */

static DEFINE_IDA(cxl_port_ida);
static DEFINE_XARRAY(cxl_root_buses);

static ssize_t devtype_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "%s\n", dev->type->name);
}
static DEVICE_ATTR_RO(devtype);

static int cxl_device_id(struct device *dev)
{
	if (dev->type == &cxl_nvdimm_bridge_type)
		return CXL_DEVICE_NVDIMM_BRIDGE;
	if (dev->type == &cxl_nvdimm_type)
		return CXL_DEVICE_NVDIMM;
	return 0;
}

static ssize_t modalias_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	return sysfs_emit(buf, CXL_MODALIAS_FMT "\n", cxl_device_id(dev));
}
static DEVICE_ATTR_RO(modalias);

static struct attribute *cxl_base_attributes[] = {
	&dev_attr_devtype.attr,
	&dev_attr_modalias.attr,
	NULL,
};

struct attribute_group cxl_base_attribute_group = {
	.attrs = cxl_base_attributes,
};

static ssize_t start_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct cxl_decoder *cxld = to_cxl_decoder(dev);
	u64 start;

	if (is_root_decoder(dev))
		start = cxld->platform_res.start;
	else
		start = cxld->decoder_range.start;

	return sysfs_emit(buf, "%#llx\n", start);
}
static DEVICE_ATTR_ADMIN_RO(start);

static ssize_t size_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct cxl_decoder *cxld = to_cxl_decoder(dev);
	u64 size;

	if (is_root_decoder(dev))
		size = resource_size(&cxld->platform_res);
	else
		size = range_len(&cxld->decoder_range);

	return sysfs_emit(buf, "%#llx\n", size);
}
static DEVICE_ATTR_RO(size);

#define CXL_DECODER_FLAG_ATTR(name, flag)                            \
static ssize_t name##_show(struct device *dev,                       \
			   struct device_attribute *attr, char *buf) \
{                                                                    \
	struct cxl_decoder *cxld = to_cxl_decoder(dev);              \
                                                                     \
	return sysfs_emit(buf, "%s\n",                               \
			  (cxld->flags & (flag)) ? "1" : "0");       \
}                                                                    \
static DEVICE_ATTR_RO(name)

CXL_DECODER_FLAG_ATTR(cap_pmem, CXL_DECODER_F_PMEM);
CXL_DECODER_FLAG_ATTR(cap_ram, CXL_DECODER_F_RAM);
CXL_DECODER_FLAG_ATTR(cap_type2, CXL_DECODER_F_TYPE2);
CXL_DECODER_FLAG_ATTR(cap_type3, CXL_DECODER_F_TYPE3);
CXL_DECODER_FLAG_ATTR(locked, CXL_DECODER_F_LOCK);

static ssize_t target_type_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct cxl_decoder *cxld = to_cxl_decoder(dev);

	switch (cxld->target_type) {
	case CXL_DECODER_ACCELERATOR:
		return sysfs_emit(buf, "accelerator\n");
	case CXL_DECODER_EXPANDER:
		return sysfs_emit(buf, "expander\n");
	}
	return -ENXIO;
}
static DEVICE_ATTR_RO(target_type);

static ssize_t emit_target_list(struct cxl_decoder *cxld, char *buf)
{
	ssize_t offset = 0;
	int i, rc = 0;

	for (i = 0; i < cxld->interleave_ways; i++) {
		struct cxl_dport *dport = cxld->target[i];
		struct cxl_dport *next = NULL;

		if (!dport)
			break;

		if (i + 1 < cxld->interleave_ways)
			next = cxld->target[i + 1];
		rc = sysfs_emit_at(buf, offset, "%d%s", dport->port_id,
				   next ? "," : "");
		if (rc < 0)
			return rc;
		offset += rc;
	}

	return offset;
}

static ssize_t target_list_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct cxl_decoder *cxld = to_cxl_decoder(dev);
	ssize_t offset;
	unsigned int seq;
	int rc;

	do {
		seq = read_seqbegin(&cxld->target_lock);
		rc = emit_target_list(cxld, buf);
	} while (read_seqretry(&cxld->target_lock, seq));

	if (rc < 0)
		return rc;
	offset = rc;

	rc = sysfs_emit_at(buf, offset, "\n");
	if (rc < 0)
		return rc;

	return offset + rc;
}
static DEVICE_ATTR_RO(target_list);

static struct attribute *cxl_decoder_base_attrs[] = {
	&dev_attr_start.attr,
	&dev_attr_size.attr,
	&dev_attr_locked.attr,
	&dev_attr_target_list.attr,
	NULL,
};

static struct attribute_group cxl_decoder_base_attribute_group = {
	.attrs = cxl_decoder_base_attrs,
};

static struct attribute *cxl_decoder_root_attrs[] = {
	&dev_attr_cap_pmem.attr,
	&dev_attr_cap_ram.attr,
	&dev_attr_cap_type2.attr,
	&dev_attr_cap_type3.attr,
	NULL,
};

static struct attribute_group cxl_decoder_root_attribute_group = {
	.attrs = cxl_decoder_root_attrs,
};

static const struct attribute_group *cxl_decoder_root_attribute_groups[] = {
	&cxl_decoder_root_attribute_group,
	&cxl_decoder_base_attribute_group,
	&cxl_base_attribute_group,
	NULL,
};

static struct attribute *cxl_decoder_switch_attrs[] = {
	&dev_attr_target_type.attr,
	NULL,
};

static struct attribute_group cxl_decoder_switch_attribute_group = {
	.attrs = cxl_decoder_switch_attrs,
};

static const struct attribute_group *cxl_decoder_switch_attribute_groups[] = {
	&cxl_decoder_switch_attribute_group,
	&cxl_decoder_base_attribute_group,
	&cxl_base_attribute_group,
	NULL,
};

static void cxl_decoder_release(struct device *dev)
{
	struct cxl_decoder *cxld = to_cxl_decoder(dev);
	struct cxl_port *port = to_cxl_port(dev->parent);

	ida_free(&port->decoder_ida, cxld->id);
	kfree(cxld);
}

static const struct device_type cxl_decoder_switch_type = {
	.name = "cxl_decoder_switch",
	.release = cxl_decoder_release,
	.groups = cxl_decoder_switch_attribute_groups,
};

static const struct device_type cxl_decoder_root_type = {
	.name = "cxl_decoder_root",
	.release = cxl_decoder_release,
	.groups = cxl_decoder_root_attribute_groups,
};

bool is_root_decoder(struct device *dev)
{
	return dev->type == &cxl_decoder_root_type;
}
EXPORT_SYMBOL_NS_GPL(is_root_decoder, CXL);

bool is_cxl_decoder(struct device *dev)
{
	return dev->type->release == cxl_decoder_release;
}
EXPORT_SYMBOL_NS_GPL(is_cxl_decoder, CXL);

struct cxl_decoder *to_cxl_decoder(struct device *dev)
{
	if (dev_WARN_ONCE(dev, dev->type->release != cxl_decoder_release,
			  "not a cxl_decoder device\n"))
		return NULL;
	return container_of(dev, struct cxl_decoder, dev);
}
EXPORT_SYMBOL_NS_GPL(to_cxl_decoder, CXL);

static void cxl_port_release(struct device *dev)
{
	struct cxl_port *port = to_cxl_port(dev);

	ida_free(&cxl_port_ida, port->id);
	kfree(port);
}

static const struct attribute_group *cxl_port_attribute_groups[] = {
	&cxl_base_attribute_group,
	NULL,
};

static const struct device_type cxl_port_type = {
	.name = "cxl_port",
	.release = cxl_port_release,
	.groups = cxl_port_attribute_groups,
};

bool is_cxl_port(struct device *dev)
{
	return dev->type == &cxl_port_type;
}
EXPORT_SYMBOL_NS_GPL(is_cxl_port, CXL);

struct cxl_port *to_cxl_port(struct device *dev)
{
	if (dev_WARN_ONCE(dev, dev->type != &cxl_port_type,
			  "not a cxl_port device\n"))
		return NULL;
	return container_of(dev, struct cxl_port, dev);
}
EXPORT_SYMBOL_NS_GPL(to_cxl_port, CXL);

static void unregister_port(void *_port)
{
	struct cxl_port *port = _port;

	device_unregister(&port->dev);
}

static void cxl_unlink_uport(void *_port)
{
	struct cxl_port *port = _port;

	sysfs_remove_link(&port->dev.kobj, "uport");
}

static int devm_cxl_link_uport(struct device *host, struct cxl_port *port)
{
	int rc;

	rc = sysfs_create_link(&port->dev.kobj, &port->uport->kobj, "uport");
	if (rc)
		return rc;
	return devm_add_action_or_reset(host, cxl_unlink_uport, port);
}

static struct cxl_port *cxl_port_alloc(struct device *uport,
				       resource_size_t component_reg_phys,
				       struct cxl_port *parent_port)
{
	struct cxl_port *port;
	struct device *dev;
	int rc;

	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port)
		return ERR_PTR(-ENOMEM);

	rc = ida_alloc(&cxl_port_ida, GFP_KERNEL);
	if (rc < 0)
		goto err;
	port->id = rc;

	/*
	 * The top-level cxl_port "cxl_root" does not have a cxl_port as
	 * its parent and it does not have any corresponding component
	 * registers as its decode is described by a fixed platform
	 * description.
	 */
	dev = &port->dev;
	if (parent_port)
		dev->parent = &parent_port->dev;
	else
		dev->parent = uport;

	port->uport = uport;
	port->component_reg_phys = component_reg_phys;
	ida_init(&port->decoder_ida);
	INIT_LIST_HEAD(&port->dports);

	device_initialize(dev);
	device_set_pm_not_required(dev);
	dev->bus = &cxl_bus_type;
	dev->type = &cxl_port_type;

	return port;

err:
	kfree(port);
	return ERR_PTR(rc);
}

/**
 * devm_cxl_add_port - register a cxl_port in CXL memory decode hierarchy
 * @host: host device for devm operations
 * @uport: "physical" device implementing this upstream port
 * @component_reg_phys: (optional) for configurable cxl_port instances
 * @parent_port: next hop up in the CXL memory decode hierarchy
 */
struct cxl_port *devm_cxl_add_port(struct device *host, struct device *uport,
				   resource_size_t component_reg_phys,
				   struct cxl_port *parent_port)
{
	struct cxl_port *port;
	struct device *dev;
	int rc;

	port = cxl_port_alloc(uport, component_reg_phys, parent_port);
	if (IS_ERR(port))
		return port;

	if (parent_port)
		port->depth = parent_port->depth + 1;
	dev = &port->dev;
	if (parent_port)
		rc = dev_set_name(dev, "port%d", port->id);
	else
		rc = dev_set_name(dev, "root%d", port->id);
	if (rc)
		goto err;

	rc = device_add(dev);
	if (rc)
		goto err;

	rc = devm_add_action_or_reset(host, unregister_port, port);
	if (rc)
		return ERR_PTR(rc);

	rc = devm_cxl_link_uport(host, port);
	if (rc)
		return ERR_PTR(rc);

	return port;

err:
	put_device(dev);
	return ERR_PTR(rc);
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_add_port, CXL);

struct pci_bus *cxl_port_to_pci_bus(struct cxl_port *port)
{
	/* There is no pci_bus associated with a CXL platform-root port */
	if (is_cxl_root(port))
		return NULL;

	if (dev_is_pci(port->uport)) {
		struct pci_dev *pdev = to_pci_dev(port->uport);

		return pdev->subordinate;
	}

	return xa_load(&cxl_root_buses, (unsigned long)port->uport);
}
EXPORT_SYMBOL_NS_GPL(cxl_port_to_pci_bus, CXL);

static void unregister_pci_bus(void *uport)
{
	xa_erase(&cxl_root_buses, (unsigned long)uport);
}

int devm_cxl_register_pci_bus(struct device *host, struct device *uport,
			      struct pci_bus *bus)
{
	int rc;

	if (dev_is_pci(uport))
		return -EINVAL;

	rc = xa_insert(&cxl_root_buses, (unsigned long)uport, bus, GFP_KERNEL);
	if (rc)
		return rc;
	return devm_add_action_or_reset(host, unregister_pci_bus, uport);
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_register_pci_bus, CXL);

/* Find a 2nd level CXL port that has a dport that is an ancestor of @match */
static int match_root_child(struct device *dev, const void *match)
{
	const struct device *iter = NULL;
	struct cxl_port *port, *parent;
	struct cxl_dport *dport;

	if (!is_cxl_port(dev))
		return 0;

	port = to_cxl_port(dev);
	if (is_cxl_root(port))
		return 0;

	parent = to_cxl_port(port->dev.parent);
	if (!is_cxl_root(parent))
		return 0;

	cxl_device_lock(&port->dev);
	list_for_each_entry(dport, &port->dports, list) {
		iter = match;
		while (iter) {
			if (iter == dport->dport)
				goto out;
			iter = iter->parent;
		}
	}
out:
	cxl_device_unlock(&port->dev);

	return !!iter;
}

struct cxl_port *find_cxl_root(struct device *dev)
{
	struct device *port_dev;
	struct cxl_port *root;

	port_dev = bus_find_device(&cxl_bus_type, NULL, dev, match_root_child);
	if (!port_dev)
		return NULL;

	root = to_cxl_port(port_dev->parent);
	get_device(&root->dev);
	put_device(port_dev);
	return root;
}
EXPORT_SYMBOL_NS_GPL(find_cxl_root, CXL);

static struct cxl_dport *find_dport(struct cxl_port *port, int id)
{
	struct cxl_dport *dport;

	device_lock_assert(&port->dev);
	list_for_each_entry (dport, &port->dports, list)
		if (dport->port_id == id)
			return dport;
	return NULL;
}

static int add_dport(struct cxl_port *port, struct cxl_dport *new)
{
	struct cxl_dport *dup;

	device_lock_assert(&port->dev);
	dup = find_dport(port, new->port_id);
	if (dup)
		dev_err(&port->dev,
			"unable to add dport%d-%s non-unique port id (%s)\n",
			new->port_id, dev_name(new->dport),
			dev_name(dup->dport));
	else
		list_add_tail(&new->list, &port->dports);

	return dup ? -EEXIST : 0;
}

static void cxl_dport_remove(void *data)
{
	struct cxl_dport *dport = data;
	struct cxl_port *port = dport->port;

	put_device(dport->dport);
	cxl_device_lock(&port->dev);
	list_del(&dport->list);
	cxl_device_unlock(&port->dev);
}

static void cxl_dport_unlink(void *data)
{
	struct cxl_dport *dport = data;
	struct cxl_port *port = dport->port;
	char link_name[CXL_TARGET_STRLEN];

	sprintf(link_name, "dport%d", dport->port_id);
	sysfs_remove_link(&port->dev.kobj, link_name);
}

/**
 * devm_cxl_add_dport - append downstream port data to a cxl_port
 * @host: devm context for allocations
 * @port: the cxl_port that references this dport
 * @dport_dev: firmware or PCI device representing the dport
 * @port_id: identifier for this dport in a decoder's target list
 * @component_reg_phys: optional location of CXL component registers
 *
 * Note that dports are appended to the devm release action's of the
 * either the port's host (for root ports), or the port itself (for
 * switch ports)
 */
struct cxl_dport *devm_cxl_add_dport(struct device *host, struct cxl_port *port,
				     struct device *dport_dev, int port_id,
				     resource_size_t component_reg_phys)
{
	char link_name[CXL_TARGET_STRLEN];
	struct cxl_dport *dport;
	int rc;

	if (!host->driver) {
		dev_WARN_ONCE(&port->dev, 1, "dport:%s bad devm context\n",
			      dev_name(dport_dev));
		return ERR_PTR(-ENXIO);
	}

	if (snprintf(link_name, CXL_TARGET_STRLEN, "dport%d", port_id) >=
	    CXL_TARGET_STRLEN)
		return ERR_PTR(-EINVAL);

	dport = devm_kzalloc(host, sizeof(*dport), GFP_KERNEL);
	if (!dport)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&dport->list);
	dport->dport = dport_dev;
	dport->port_id = port_id;
	dport->component_reg_phys = component_reg_phys;
	dport->port = port;

	rc = add_dport(port, dport);
	if (rc)
		return ERR_PTR(rc);

	get_device(dport_dev);
	rc = devm_add_action_or_reset(host, cxl_dport_remove, dport);
	if (rc)
		return ERR_PTR(rc);

	rc = sysfs_create_link(&port->dev.kobj, &dport_dev->kobj, link_name);
	if (rc)
		return ERR_PTR(rc);

	rc = devm_add_action_or_reset(host, cxl_dport_unlink, dport);
	if (rc)
		return ERR_PTR(rc);

	return dport;
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_add_dport, CXL);

static int decoder_populate_targets(struct cxl_decoder *cxld,
				    struct cxl_port *port, int *target_map)
{
	int i, rc = 0;

	if (!target_map)
		return 0;

	device_lock_assert(&port->dev);

	if (list_empty(&port->dports))
		return -EINVAL;

	write_seqlock(&cxld->target_lock);
	for (i = 0; i < cxld->nr_targets; i++) {
		struct cxl_dport *dport = find_dport(port, target_map[i]);

		if (!dport) {
			rc = -ENXIO;
			break;
		}
		cxld->target[i] = dport;
	}
	write_sequnlock(&cxld->target_lock);

	return rc;
}

/**
 * cxl_decoder_alloc - Allocate a new CXL decoder
 * @port: owning port of this decoder
 * @nr_targets: downstream targets accessible by this decoder. All upstream
 *		ports and root ports must have at least 1 target.
 *
 * A port should contain one or more decoders. Each of those decoders enable
 * some address space for CXL.mem utilization. A decoder is expected to be
 * configured by the caller before registering.
 *
 * Return: A new cxl decoder to be registered by cxl_decoder_add(). The decoder
 *	   is initialized to be a "passthrough" decoder.
 */
static struct cxl_decoder *cxl_decoder_alloc(struct cxl_port *port,
					     unsigned int nr_targets)
{
	struct cxl_decoder *cxld;
	struct device *dev;
	int rc = 0;

	if (nr_targets > CXL_DECODER_MAX_INTERLEAVE || nr_targets == 0)
		return ERR_PTR(-EINVAL);

	cxld = kzalloc(struct_size(cxld, target, nr_targets), GFP_KERNEL);
	if (!cxld)
		return ERR_PTR(-ENOMEM);

	rc = ida_alloc(&port->decoder_ida, GFP_KERNEL);
	if (rc < 0)
		goto err;

	cxld->id = rc;
	cxld->nr_targets = nr_targets;
	seqlock_init(&cxld->target_lock);
	dev = &cxld->dev;
	device_initialize(dev);
	device_set_pm_not_required(dev);
	dev->parent = &port->dev;
	dev->bus = &cxl_bus_type;
	if (is_cxl_root(port))
		cxld->dev.type = &cxl_decoder_root_type;
	else
		cxld->dev.type = &cxl_decoder_switch_type;

	/* Pre initialize an "empty" decoder */
	cxld->interleave_ways = 1;
	cxld->interleave_granularity = PAGE_SIZE;
	cxld->target_type = CXL_DECODER_EXPANDER;
	cxld->platform_res = (struct resource)DEFINE_RES_MEM(0, 0);

	return cxld;
err:
	kfree(cxld);
	return ERR_PTR(rc);
}

/**
 * cxl_root_decoder_alloc - Allocate a root level decoder
 * @port: owning CXL root of this decoder
 * @nr_targets: static number of downstream targets
 *
 * Return: A new cxl decoder to be registered by cxl_decoder_add(). A
 * 'CXL root' decoder is one that decodes from a top-level / static platform
 * firmware description of CXL resources into a CXL standard decode
 * topology.
 */
struct cxl_decoder *cxl_root_decoder_alloc(struct cxl_port *port,
					   unsigned int nr_targets)
{
	if (!is_cxl_root(port))
		return ERR_PTR(-EINVAL);

	return cxl_decoder_alloc(port, nr_targets);
}
EXPORT_SYMBOL_NS_GPL(cxl_root_decoder_alloc, CXL);

/**
 * cxl_switch_decoder_alloc - Allocate a switch level decoder
 * @port: owning CXL switch port of this decoder
 * @nr_targets: max number of dynamically addressable downstream targets
 *
 * Return: A new cxl decoder to be registered by cxl_decoder_add(). A
 * 'switch' decoder is any decoder that can be enumerated by PCIe
 * topology and the HDM Decoder Capability. This includes the decoders
 * that sit between Switch Upstream Ports / Switch Downstream Ports and
 * Host Bridges / Root Ports.
 */
struct cxl_decoder *cxl_switch_decoder_alloc(struct cxl_port *port,
					     unsigned int nr_targets)
{
	if (is_cxl_root(port))
		return ERR_PTR(-EINVAL);

	return cxl_decoder_alloc(port, nr_targets);
}
EXPORT_SYMBOL_NS_GPL(cxl_switch_decoder_alloc, CXL);

/**
 * cxl_decoder_add_locked - Add a decoder with targets
 * @cxld: The cxl decoder allocated by cxl_decoder_alloc()
 * @target_map: A list of downstream ports that this decoder can direct memory
 *              traffic to. These numbers should correspond with the port number
 *              in the PCIe Link Capabilities structure.
 *
 * Certain types of decoders may not have any targets. The main example of this
 * is an endpoint device. A more awkward example is a hostbridge whose root
 * ports get hot added (technically possible, though unlikely).
 *
 * This is the locked variant of cxl_decoder_add().
 *
 * Context: Process context. Expects the device lock of the port that owns the
 *	    @cxld to be held.
 *
 * Return: Negative error code if the decoder wasn't properly configured; else
 *	   returns 0.
 */
int cxl_decoder_add_locked(struct cxl_decoder *cxld, int *target_map)
{
	struct cxl_port *port;
	struct device *dev;
	int rc;

	if (WARN_ON_ONCE(!cxld))
		return -EINVAL;

	if (WARN_ON_ONCE(IS_ERR(cxld)))
		return PTR_ERR(cxld);

	if (cxld->interleave_ways < 1)
		return -EINVAL;

	port = to_cxl_port(cxld->dev.parent);
	rc = decoder_populate_targets(cxld, port, target_map);
	if (rc)
		return rc;

	dev = &cxld->dev;
	rc = dev_set_name(dev, "decoder%d.%d", port->id, cxld->id);
	if (rc)
		return rc;

	/*
	 * Platform decoder resources should show up with a reasonable name. All
	 * other resources are just sub ranges within the main decoder resource.
	 */
	if (is_root_decoder(dev))
		cxld->platform_res.name = dev_name(dev);

	return device_add(dev);
}
EXPORT_SYMBOL_NS_GPL(cxl_decoder_add_locked, CXL);

/**
 * cxl_decoder_add - Add a decoder with targets
 * @cxld: The cxl decoder allocated by cxl_decoder_alloc()
 * @target_map: A list of downstream ports that this decoder can direct memory
 *              traffic to. These numbers should correspond with the port number
 *              in the PCIe Link Capabilities structure.
 *
 * This is the unlocked variant of cxl_decoder_add_locked().
 * See cxl_decoder_add_locked().
 *
 * Context: Process context. Takes and releases the device lock of the port that
 *	    owns the @cxld.
 */
int cxl_decoder_add(struct cxl_decoder *cxld, int *target_map)
{
	struct cxl_port *port;
	int rc;

	if (WARN_ON_ONCE(!cxld))
		return -EINVAL;

	if (WARN_ON_ONCE(IS_ERR(cxld)))
		return PTR_ERR(cxld);

	port = to_cxl_port(cxld->dev.parent);

	cxl_device_lock(&port->dev);
	rc = cxl_decoder_add_locked(cxld, target_map);
	cxl_device_unlock(&port->dev);

	return rc;
}
EXPORT_SYMBOL_NS_GPL(cxl_decoder_add, CXL);

static void cxld_unregister(void *dev)
{
	device_unregister(dev);
}

int cxl_decoder_autoremove(struct device *host, struct cxl_decoder *cxld)
{
	return devm_add_action_or_reset(host, cxld_unregister, &cxld->dev);
}
EXPORT_SYMBOL_NS_GPL(cxl_decoder_autoremove, CXL);

/**
 * __cxl_driver_register - register a driver for the cxl bus
 * @cxl_drv: cxl driver structure to attach
 * @owner: owning module/driver
 * @modname: KBUILD_MODNAME for parent driver
 */
int __cxl_driver_register(struct cxl_driver *cxl_drv, struct module *owner,
			  const char *modname)
{
	if (!cxl_drv->probe) {
		pr_debug("%s ->probe() must be specified\n", modname);
		return -EINVAL;
	}

	if (!cxl_drv->name) {
		pr_debug("%s ->name must be specified\n", modname);
		return -EINVAL;
	}

	if (!cxl_drv->id) {
		pr_debug("%s ->id must be specified\n", modname);
		return -EINVAL;
	}

	cxl_drv->drv.bus = &cxl_bus_type;
	cxl_drv->drv.owner = owner;
	cxl_drv->drv.mod_name = modname;
	cxl_drv->drv.name = cxl_drv->name;

	return driver_register(&cxl_drv->drv);
}
EXPORT_SYMBOL_NS_GPL(__cxl_driver_register, CXL);

void cxl_driver_unregister(struct cxl_driver *cxl_drv)
{
	driver_unregister(&cxl_drv->drv);
}
EXPORT_SYMBOL_NS_GPL(cxl_driver_unregister, CXL);

static int cxl_bus_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	return add_uevent_var(env, "MODALIAS=" CXL_MODALIAS_FMT,
			      cxl_device_id(dev));
}

static int cxl_bus_match(struct device *dev, struct device_driver *drv)
{
	return cxl_device_id(dev) == to_cxl_drv(drv)->id;
}

static int cxl_bus_probe(struct device *dev)
{
	int rc;

	/*
	 * Take the CXL nested lock since the driver core only holds
	 * @dev->mutex and not @dev->lockdep_mutex.
	 */
	cxl_nested_lock(dev);
	rc = to_cxl_drv(dev->driver)->probe(dev);
	cxl_nested_unlock(dev);

	return rc;
}

static void cxl_bus_remove(struct device *dev)
{
	struct cxl_driver *cxl_drv = to_cxl_drv(dev->driver);

	cxl_nested_lock(dev);
	if (cxl_drv->remove)
		cxl_drv->remove(dev);
	cxl_nested_unlock(dev);
}

struct bus_type cxl_bus_type = {
	.name = "cxl",
	.uevent = cxl_bus_uevent,
	.match = cxl_bus_match,
	.probe = cxl_bus_probe,
	.remove = cxl_bus_remove,
};
EXPORT_SYMBOL_NS_GPL(cxl_bus_type, CXL);

static __init int cxl_core_init(void)
{
	int rc;

	cxl_mbox_init();

	rc = cxl_memdev_init();
	if (rc)
		return rc;

	rc = bus_register(&cxl_bus_type);
	if (rc)
		goto err;
	return 0;

err:
	cxl_memdev_exit();
	cxl_mbox_exit();
	return rc;
}

static void cxl_core_exit(void)
{
	bus_unregister(&cxl_bus_type);
	cxl_memdev_exit();
	cxl_mbox_exit();
}

module_init(cxl_core_init);
module_exit(cxl_core_exit);
MODULE_LICENSE("GPL v2");
