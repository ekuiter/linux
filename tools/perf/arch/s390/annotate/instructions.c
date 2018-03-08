// SPDX-License-Identifier: GPL-2.0
#include <linux/compiler.h>

static int s390_call__parse(struct arch *arch, struct ins_operands *ops,
			    struct map *map)
{
	char *endptr, *tok, *name;
	struct addr_map_symbol target = {
		.map = map,
	};

	tok = strchr(ops->raw, ',');
	if (!tok)
		return -1;

	ops->target.addr = strtoull(tok + 1, &endptr, 16);

	name = strchr(endptr, '<');
	if (name == NULL)
		return -1;

	name++;

	if (arch->objdump.skip_functions_char &&
	    strchr(name, arch->objdump.skip_functions_char))
		return -1;

	tok = strchr(name, '>');
	if (tok == NULL)
		return -1;

	*tok = '\0';
	ops->target.name = strdup(name);
	*tok = '>';

	if (ops->target.name == NULL)
		return -1;
	target.addr = map__objdump_2mem(map, ops->target.addr);

	if (map_groups__find_ams(&target) == 0 &&
	    map__rip_2objdump(target.map, map->map_ip(target.map, target.addr)) == ops->target.addr)
		ops->target.sym = target.sym;

	return 0;
}

static int call__scnprintf(struct ins *ins, char *bf, size_t size,
			   struct ins_operands *ops);

static struct ins_ops s390_call_ops = {
	.parse	   = s390_call__parse,
	.scnprintf = call__scnprintf,
};

static struct ins_ops *s390__associate_ins_ops(struct arch *arch, const char *name)
{
	struct ins_ops *ops = NULL;

	/* catch all kind of jumps */
	if (strchr(name, 'j') ||
	    !strncmp(name, "bct", 3) ||
	    !strncmp(name, "br", 2))
		ops = &jump_ops;
	/* override call/returns */
	if (!strcmp(name, "bras") ||
	    !strcmp(name, "brasl") ||
	    !strcmp(name, "basr"))
		ops = &s390_call_ops;
	if (!strcmp(name, "br"))
		ops = &ret_ops;

	if (ops)
		arch__associate_ins_ops(arch, name, ops);
	return ops;
}

static int s390__cpuid_parse(struct arch *arch, char *cpuid)
{
	unsigned int family;
	char model[16], model_c[16], cpumf_v[16], cpumf_a[16];
	int ret;

	/*
	 * cpuid string format:
	 * "IBM,family,model-capacity,model[,cpum_cf-version,cpum_cf-authorization]"
	 */
	ret = sscanf(cpuid, "%*[^,],%u,%[^,],%[^,],%[^,],%s", &family, model_c,
		     model, cpumf_v, cpumf_a);
	if (ret >= 2) {
		arch->family = family;
		arch->model = 0;
		return 0;
	}

	return -1;
}

static int s390__annotate_init(struct arch *arch, char *cpuid __maybe_unused)
{
	int err = 0;

	if (!arch->initialized) {
		arch->initialized = true;
		arch->associate_instruction_ops = s390__associate_ins_ops;
		if (cpuid)
			err = s390__cpuid_parse(arch, cpuid);
	}

	return err;
}
