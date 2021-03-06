/*
 *   BSD LICENSE
 *
 *   Copyright(c) 2016 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pqos.h>

#include "rdt.h"
#include "cpu.h"

static const struct pqos_cap *m_cap;
static const struct pqos_cpuinfo *m_cpu;
static const struct pqos_capability *m_cap_l2ca;
static const struct pqos_capability *m_cap_l3ca;

/* Print L2 or L3 configuration contained in struct rdt_ca */
static void
print_rdt_ca(struct rdt_ca ca)
{
	if (!(PQOS_CAP_TYPE_L2CA == ca.type || PQOS_CAP_TYPE_L3CA == ca.type) ||
			NULL == ca.u.generic_ptr)
		return;

	if (PQOS_CAP_TYPE_L2CA == ca.type)
		printf("MASK: 0x%llx",
			(unsigned long long) ca.u.l2->ways_mask);
	else if (ca.u.l3->cdp == 1)
		printf("code MASK: 0x%llx, data MASK: 0x%llx",
			(unsigned long long) ca.u.l3->u.s.code_mask,
			(unsigned long long) ca.u.l3->u.s.data_mask);
	else
		printf("MASK: 0x%llx",
			(unsigned long long) ca.u.l3->u.ways_mask);
}

/* Get short string representation of configuration type of struct rdt_ca */
static const char *
get_type_str_rdt_ca(struct rdt_ca ca)
{
	if (!(PQOS_CAP_TYPE_L2CA == ca.type || PQOS_CAP_TYPE_L3CA == ca.type) ||
			NULL == ca.u.generic_ptr)
		return "ERROR!";

	return PQOS_CAP_TYPE_L2CA == ca.type ? "L2" : "L3";
}

/* Validates configuration contained in struct rdt_ca */
static int
is_valid_rdt_ca(struct rdt_ca ca)
{
	if (!(PQOS_CAP_TYPE_L2CA == ca.type || PQOS_CAP_TYPE_L3CA == ca.type) ||
			NULL == ca.u.generic_ptr)
		return 0;

	if (PQOS_CAP_TYPE_L2CA == ca.type)
		return NULL != ca.u.l2 && 0 != ca.u.l2->ways_mask;
	else
		return NULL != ca.u.l3 && ((1 == ca.u.l3->cdp &&
			0 != ca.u.l3->u.s.code_mask &&
			0 != ca.u.l3->u.s.data_mask) ||
			(0 == ca.u.l3->cdp && 0 != ca.u.l3->u.ways_mask));
}

/*
 * Reads COS#0 (default) configuration (L2 or L3) and stores it in
 * struct rdt_ca
 */
static int
get_default_rdt_ca(const unsigned socket, struct rdt_ca ca)
{
	struct pqos_l2ca l2_ca[PQOS_MAX_L3CA_COS];
	struct pqos_l3ca l3_ca[PQOS_MAX_L3CA_COS];
	unsigned num = 0, i = 0;
	int ret = 0;

	if (!(PQOS_CAP_TYPE_L2CA == ca.type || PQOS_CAP_TYPE_L3CA == ca.type) ||
			NULL == ca.u.generic_ptr)
		return PQOS_RETVAL_PARAM;

	if (PQOS_CAP_TYPE_L2CA == ca.type)
		ret = pqos_l2ca_get(socket, PQOS_MAX_L2CA_COS, &num, l2_ca);
	else
		ret = pqos_l3ca_get(socket, PQOS_MAX_L3CA_COS, &num, l3_ca);

	if (PQOS_RETVAL_OK != ret)
		return ret;

	for (i = 0; i < num; i++) {
		if (PQOS_CAP_TYPE_L2CA == ca.type) {
			if (0 == l2_ca[i].class_id) {
				*ca.u.l2 = l2_ca[i];
				break;
			}
		} else {
			if (0 == l3_ca[i].class_id) {
				*ca.u.l3 = l3_ca[i];
				break;
			}
		}
	}

	return PQOS_RETVAL_OK;
}

/* Reads requested COS configuration (L2 or L3) and stores it in struct rdt_ca*/
static int
get_rdt_ca(const unsigned socket, const unsigned max_num_ca, unsigned *num_ca,
		struct rdt_ca ca)
{
	if (!(PQOS_CAP_TYPE_L2CA == ca.type || PQOS_CAP_TYPE_L3CA == ca.type) ||
			NULL == ca.u.generic_ptr)
		return PQOS_RETVAL_PARAM;

	if (PQOS_CAP_TYPE_L2CA == ca.type)
		return pqos_l2ca_get(socket, max_num_ca, num_ca, ca.u.l2);
	else
		return pqos_l3ca_get(socket, max_num_ca, num_ca, ca.u.l3);
}

/* Return number of set bits in bitmask */
static unsigned
bits_count(uint64_t bitmask)
{
	unsigned count = 0;

	for (; bitmask != 0; count++)
		bitmask &= bitmask - 1;

	return count;
}

/* Test if bitmask is contiguous */
static int
is_contiguous(const char *cat_type, const uint64_t bitmask)
{
	/* check if bitmask is contiguous */
	unsigned i = 0, j = 0;
	const unsigned max_idx = (sizeof(bitmask) * CHAR_BIT);

	if (bitmask == 0)
		return 0;

	for (i = 0; i < max_idx; i++) {
		if (((1ULL << i) & bitmask) != 0)
			j++;
		else if (j > 0)
			break;
	}

	if (bits_count(bitmask) != j) {
		printf("CAT: %s mask 0x%llx is not contiguous.\n",
			cat_type, (unsigned long long) bitmask);
		return 0;
	}

	return 1;
}

/* Convert hex string to UINT */
static int
xstr_to_uint(const char *xstr, uint64_t *value)
{
	const char *xstr_start = xstr;
	char *xstr_end = NULL;

	if (NULL == xstr || NULL == value)
		return -EINVAL;

	while (isblank(*xstr_start))
		xstr_start++;

	if (!isxdigit(*xstr_start))
		return -EINVAL;

	errno = 0;
	*value = strtoul(xstr_start, &xstr_end, 16);
	if (errno != 0 || xstr_end == NULL || xstr_end == xstr_start ||
			*value == 0)
		return -EINVAL;

	return xstr_end - xstr;
}

/* Parse CBM masks */
static int
parse_mask_set(const char *cbm, const int force_dual_mask, uint64_t *mask,
		uint64_t *cmask)
{
	int offset = 0;
	const char *cbm_start = cbm;

	/* parse mask_set from start point */
	if (*cbm_start == '(' || force_dual_mask) {
		while (!isxdigit(*cbm_start))
			cbm_start++;

		offset = xstr_to_uint(cbm_start, cmask);
		if (offset < 0)
			goto err;

		cbm_start += offset;

		while (isblank(*cbm_start))
			cbm_start++;

		if (*cbm_start != ',')
			goto err;

		cbm_start++;
	}

	offset = xstr_to_uint(cbm_start, mask);
	if (offset < 0)
		goto err;

	return cbm_start + offset - cbm;

err:
	return -EINVAL;
}

int
parse_l3(const char *l3ca)
{
	unsigned idx = 0;
	const char *end = NULL;

	if (l3ca == NULL)
		return -EINVAL;

	/* Get cbm */
	do {
		uint64_t mask = 0, cmask = 0;
		const char *cbm_start = NULL;
		int offset = 0;
		unsigned cpustr_len = 0;

		while (isblank(*l3ca))
			l3ca++;

		if (*l3ca == '\0')
			return -EINVAL;

		/* record mask_set start point */
		cbm_start = l3ca;

		/* go across a complete bracket */
		if (*cbm_start == '(') {
			l3ca += strcspn(l3ca, ")");
			if (*l3ca++ == '\0')
				return -EINVAL;
		}

		/* scan the separator '@' */
		l3ca += strcspn(l3ca, "@");

		if (*l3ca != '@')
			return -EINVAL;

		l3ca++;

		/* explicit assign cpu_set */
		if (*l3ca == '(') {
			if (NULL == strchr(l3ca, ')'))
				return -EINVAL;
			l3ca++;
			cpustr_len = strcspn(l3ca, ")");
		} else
			cpustr_len = strcspn(l3ca, ",");

		offset = str_to_cpuset(l3ca, cpustr_len,
			&g_cfg.config[idx].cpumask);

		if (offset < 0 || CPU_COUNT(&g_cfg.config[idx].cpumask) == 0)
			return -EINVAL;

		end = l3ca + offset;

		if (*end == ')')
			end++;

		if (*end != ',' && *end != '\0')
			return -EINVAL;

		offset = parse_mask_set(cbm_start, 0, &mask, &cmask);
		if (offset < 0 || 0 == mask)
			return -EINVAL;

		if (is_contiguous("L3", mask) == 0 ||
				(cmask != 0 && is_contiguous("L3", cmask) == 0))
			return -EINVAL;

		if (cmask != 0) {
			g_cfg.config[idx].l3.cdp = 1;
			g_cfg.config[idx].l3.u.s.code_mask = cmask;
			g_cfg.config[idx].l3.u.s.data_mask = mask;
		} else
			g_cfg.config[idx].l3.u.ways_mask = mask;

		l3ca = end + 1;
		idx++;
	} while (*end != '\0' && idx < CPU_SETSIZE);

	g_cfg.config_count = idx;
	if (g_cfg.config_count == 0)
		return -EINVAL;

	return 0;
}

int
parse_reset(const char *cpustr)
{
	unsigned cpustr_len = strlen(cpustr);
	int ret = 0;

	ret = str_to_cpuset(cpustr, cpustr_len, &g_cfg.reset_cpuset);
	return ret > 0 ? 0 : ret;
}

/* parses CBM string and stores in struct rdt_ca*/
static int
str_to_cbm_rdt_ca(const char *param, struct rdt_ca ca)
{
	uint64_t mask = 0, mask2 = 0;
	int ret, force_dual_mask;

	if (!(PQOS_CAP_TYPE_L2CA == ca.type || PQOS_CAP_TYPE_L3CA == ca.type) ||
			NULL == ca.u.generic_ptr || NULL == param)
		return -EINVAL;

	force_dual_mask = (NULL != strchr(param, ','));
	ret = parse_mask_set(param, force_dual_mask, &mask, &mask2);
	if (ret < 0 || 0 == mask || is_contiguous(
			PQOS_CAP_TYPE_L2CA == ca.type ? "L2" : "L3", mask) == 0)
		return -EINVAL;

	if (PQOS_CAP_TYPE_L2CA == ca.type) {
		if (mask2 != 0)
			return -EINVAL;
		ca.u.l2->ways_mask = mask;
	} else {
		if (mask2 != 0 && is_contiguous("L3", mask2) == 0)
			return -EINVAL;
		if (mask2 != 0) {
			ca.u.l3->cdp = 1;
			ca.u.l3->u.s.data_mask = mask;
			ca.u.l3->u.s.code_mask = mask2;
		} else
			ca.u.l3->u.ways_mask = mask;
	}

	return 0;
}

int
parse_rdt(char *rdtstr)
{
	char *group_saveptr = NULL;
	char *group = strtok_r(rdtstr, ";", &group_saveptr);
	unsigned idx = 0;
	int ret;

	if (NULL == rdtstr)
		return -EINVAL;

	while (group != NULL) {
		char *feature_saveptr = NULL;

		/* Min len check, 1 feature + 1 CPU eg. "3(f)@0" */
		if (strlen(group) < 6) {
			printf("Invalid group: \"%s\"\n", group);
			return -EINVAL;
		}

		char *feature = strtok_r(group, ")", &feature_saveptr);

		while (feature != NULL) {
			if (strlen(feature) < 2 ||
					(0 == strspn(feature, "23@") &&
					(2 != strspn(feature, "l23")))) {
				printf("Invalid option: \"%s\"\n", feature);
				return -EINVAL;
			}

			char *param = strstr(feature, "(");

			if (param == NULL) {
				printf("Invalid option: \"%s\"\n", feature);
				return -EINVAL;
			}
			param++;

			if (1 == strspn(feature, "2") ||
					feature == strstr(feature, "l2")) {
				ret = str_to_cbm_rdt_ca(param,
					wrap_l2ca(&g_cfg.config[idx].l2));
				if (ret < 0)
					return ret;
			} else if (1 == strspn(feature, "3") ||
					feature == strstr(feature, "l3")) {
				ret = str_to_cbm_rdt_ca(param,
					wrap_l3ca(&g_cfg.config[idx].l3));
				if (ret < 0)
					return ret;
			} else if (1 == strspn(feature, "@")) {
				ret = str_to_cpuset(param, strlen(param),
					&g_cfg.config[idx].cpumask);

				if (ret <= 0 || CPU_COUNT(
						&g_cfg.config[idx].cpumask) ==
						0)
					return -EINVAL;
			}

			feature = strtok_r(NULL, ")", &feature_saveptr);
		}

		group = strtok_r(NULL, ";", &group_saveptr);
		idx++;
	}

	g_cfg.config_count = idx;
	if (g_cfg.config_count == 0)
		return -EINVAL;

	return 0;
}

/* Check are configured CPU sets overlapping */
static int
check_cpus_overlapping(void)
{
	unsigned i = 0;

	for (i = 0; i < g_cfg.config_count; i++) {
		unsigned j = 0;

		for (j = i + 1; j < g_cfg.config_count; j++) {
			unsigned overlapping = 0;
#ifdef __linux__
			cpu_set_t mask;

			CPU_AND(&mask, &g_cfg.config[i].cpumask,
				&g_cfg.config[j].cpumask);
			overlapping = CPU_COUNT(&mask) != 0;
#endif
#ifdef __FreeBSD__
			overlapping = CPU_OVERLAP(&g_cfg.config[i].cpumask,
				&g_cfg.config[j].cpumask);
#endif
			if (1 == overlapping) {
				printf("CAT: Requested CPUs sets are "
					"overlapping.\n");
				return -EINVAL;
			}
		}
	}

	return 0;
}

/* Check are configured CPUs valid and have no COS associated */
static int
check_cpus(void)
{
	unsigned i = 0;
	int ret = 0;

	for (i = 0; i < g_cfg.config_count; i++) {
		unsigned cpu_id = 0;

		for (cpu_id = 0; cpu_id < CPU_SETSIZE; cpu_id++) {
			unsigned cos_id = 0;

			if (CPU_ISSET(cpu_id, &g_cfg.config[i].cpumask) == 0)
				continue;

			ret = pqos_cpu_check_core(m_cpu, cpu_id);
			if (ret != PQOS_RETVAL_OK) {
				printf("CAT: %u is not a valid logical core id."
					"\n", cpu_id);
				return -ENODEV;
			}

			ret = pqos_alloc_assoc_get(cpu_id, &cos_id);
			if (ret != PQOS_RETVAL_OK) {
				printf("CAT: Failed to read cpu %u COS "
					"association.\n", cpu_id);
				return -EFAULT;
			}

			/*
			 * Check if COS assigned to lcore is different
			 * then default one (#0)
			 */
			if (cos_id != 0) {
				printf("CAT: cpu %u has already associated "
					"COS#%u. Please reset CAT.\n",
					cpu_id, cos_id);
				return -EBUSY;
			}
		}
	}

	return 0;
}

/*
 * Check if CPU supports requested CDP configuration,
 * notify user if it supported but not enabled
 */
static int
check_cdp(void)
{
	unsigned i = 0;

	for (i = 0; i < g_cfg.config_count; i++) {
		if (0 == g_cfg.config[i].l3.cdp ||
				1 == m_cap_l3ca->u.l3ca->cdp_on)
			continue;

		if (m_cap_l3ca->u.l3ca->cdp == 0)
			printf("CAT: CDP requested but not "
				"supported.\n");
		else
			printf("CAT: CDP requested but not enabled. "
				"Please enable CDP.\n");

		return -ENOTSUP;
	}

	return 0;
}

/*
 * Check if configuration (L2CA, L3CA) is supported by the system
 */
static int
check_supported(void)
{
	unsigned i = 0;

	for (i = 0; i < g_cfg.config_count; i++) {
		if (NULL == m_cap_l3ca && is_valid_rdt_ca(
				wrap_l3ca(&g_cfg.config[i].l3))) {
			printf("CAT: L3CA requested but not supported by "
				"system!\n");
			return -ENOTSUP;
		}

		if (NULL == m_cap_l2ca && is_valid_rdt_ca(
				wrap_l2ca(&g_cfg.config[i].l2))) {
			printf("CAT: L2CA requested but not supported by "
				"system!\n");
			return -ENOTSUP;
		}
	}

	return 0;
}

/* Returns negation of max CBM mask for requested type (L2 or L3) */
static uint64_t
get_not_cbm(enum pqos_cap_type type)
{
	if (PQOS_CAP_TYPE_L2CA == type && NULL != m_cap_l2ca)
		return (UINT64_MAX << (m_cap_l2ca->u.l2ca->num_ways));
	else if (PQOS_CAP_TYPE_L3CA == type && NULL != m_cap_l3ca)
		return (UINT64_MAX << (m_cap_l3ca->u.l3ca->num_ways));
	else
		return 0;
}

/* Returns contention mask for requested type (L2 or L3) */
static uint64_t
get_contention_mask(enum pqos_cap_type type)
{
	if (PQOS_CAP_TYPE_L2CA == type && NULL != m_cap_l2ca)
		return m_cap_l2ca->u.l2ca->way_contention;
	else if (PQOS_CAP_TYPE_L3CA == type && NULL != m_cap_l3ca)
		return m_cap_l3ca->u.l3ca->way_contention;
	else
		return 0;
}

/*
 * Returns ORed masks (for L3 CDP config) or single mask of configuration
 * contained in struct rdt_ca
 */
static uint64_t
get_ored_cbm_rdt_ca(struct rdt_ca ca)
{
	if (!(PQOS_CAP_TYPE_L2CA == ca.type || PQOS_CAP_TYPE_L3CA == ca.type) ||
			NULL == ca.u.generic_ptr || 0 == is_valid_rdt_ca(ca))
		return 0;

	if (PQOS_CAP_TYPE_L2CA == ca.type)
		return ca.u.l2->ways_mask;
	else if (ca.u.l3->cdp == 1)
		return ca.u.l3->u.s.code_mask | ca.u.l3->u.s.data_mask;
	else
		return ca.u.l3->u.ways_mask;
}
/*
 * Check are requested CBMs supported by system,
 * warn if CBM overlaps contention mask
 */
static int
check_cbm_len_and_contention_type(enum pqos_cap_type type)
{
	unsigned i = 0;
	struct rdt_ca ca;
	uint64_t mask = 0;

	if (!(PQOS_CAP_TYPE_L2CA == type || PQOS_CAP_TYPE_L3CA == type))
		return -EINVAL;

	const uint64_t not_cbm = get_not_cbm(type);
	const uint64_t contention_cbm = get_contention_mask(type);

	if (0 == not_cbm)
		return -EINVAL;

	for (i = 0; i < g_cfg.config_count; i++) {
		if (PQOS_CAP_TYPE_L2CA == type)
			ca = wrap_l2ca(&g_cfg.config[i].l2);
		else
			ca = wrap_l3ca(&g_cfg.config[i].l3);

		if (!is_valid_rdt_ca(ca))
			continue;

		mask = get_ored_cbm_rdt_ca(ca);

		if ((mask & not_cbm) != 0) {
			printf("CAT: One or more of requested %s CBM "
				"masks (", get_type_str_rdt_ca(ca));
			print_rdt_ca(ca);
			printf(") not supported by system "
				"(too long).\n");
			return -ENOTSUP;
		}

		/* Just a note */
		if ((mask & contention_cbm) != 0) {
			printf("CAT: One or more of requested %s CBM "
				"masks (", get_type_str_rdt_ca(ca));
			print_rdt_ca(ca);
			printf(") overlap CBM contention mask.\n");
		}
	}

	return 0;
}


/*
 * Check are requested CBMs supported by system,
 * warn if CBM overlaps contention mask
 */
static int
check_cbm_len_and_contention(void)
{
	int ret;

	if (NULL != m_cap_l2ca) {
		ret = check_cbm_len_and_contention_type(PQOS_CAP_TYPE_L2CA);
		if (ret < 0)
			return ret;
	}

	if (NULL != m_cap_l3ca) {
		ret = check_cbm_len_and_contention_type(PQOS_CAP_TYPE_L3CA);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/* Get unassigned COS */
static int
get_avail_rdt_ca(const unsigned socket, const unsigned max_num_ca,
		unsigned *num_ca, const unsigned hi_cos_id, struct rdt_ca ca)
{
	unsigned cores[CPU_SETSIZE], cores_count = 0, num = 0;
	unsigned class_id = 0, rdt_ca_class_id = 0, i = 0, j = 0,  k = 0;
	int ret = PQOS_RETVAL_OK;

	if (!(PQOS_CAP_TYPE_L2CA == ca.type || PQOS_CAP_TYPE_L3CA == ca.type) ||
			NULL == ca.u.generic_ptr)
		return PQOS_RETVAL_PARAM;

	ret = get_rdt_ca(socket, max_num_ca, &num, ca);
	if (ret != PQOS_RETVAL_OK) {
		printf("Error retrieving %s configuration!\n",
			PQOS_CAP_TYPE_L2CA == ca.type ? "L2CA" : "L3CA");
		return ret;
	}

	ret = pqos_cpu_get_cores(m_cpu, socket, CPU_SETSIZE, &cores_count,
		&cores[0]);
	if (ret != PQOS_RETVAL_OK) {
		printf("Error retrieving core information!\n");
		return ret;
	}

	/* Get already associated COS and remove them from list */
	for (k = 0; k < cores_count; k++) {
		/* Get COS associated to core */
		ret = pqos_alloc_assoc_get(cores[k], &class_id);
		if (ret != PQOS_RETVAL_OK) {
			printf("Error reading class association!\n");
			return ret;
		}

		/* Remove associated COS from the list */
		for (i = 0; i < num; i++) {
			if (PQOS_CAP_TYPE_L2CA == ca.type)
				rdt_ca_class_id = ca.u.l2[i].class_id;
			else
				rdt_ca_class_id = ca.u.l3[i].class_id;

			if (rdt_ca_class_id == class_id) {
				for (j = i + 1; j < num; j++)
					if (PQOS_CAP_TYPE_L2CA == ca.type)
						ca.u.l2[j-1] = ca.u.l2[j];
					else
						ca.u.l3[j-1] = ca.u.l3[j];

				num--;
				break;
			}
		}
	}

	/* Remove COS with id higher then requested from the list */
	for (i = 0; i < num; i++) {
		if (PQOS_CAP_TYPE_L2CA == ca.type)
			rdt_ca_class_id = ca.u.l2[i].class_id;
		else
			rdt_ca_class_id = ca.u.l3[i].class_id;

		if (rdt_ca_class_id > hi_cos_id) {
			for (j = i + 1; j < num; j++)
				if (PQOS_CAP_TYPE_L2CA == ca.type)
					ca.u.l2[j-1] = ca.u.l2[j];
				else
					ca.u.l3[j-1] = ca.u.l3[j];

			num--;
		}
	}

	*num_ca = num;

	return ret;
}

/* Search for available/unassigned COS to use for passed config */
static int
select_rdt_ca(const unsigned socket, const unsigned num_cos,
		const unsigned hi_cos_id, struct rdt_ca ca)
{
	struct pqos_l2ca avail_l2_ca[PQOS_MAX_L2CA_COS];
	struct pqos_l3ca avail_l3_ca[PQOS_MAX_L3CA_COS];
	unsigned num = 0, i = 0;
	int ret = PQOS_RETVAL_OK;

	if (!(PQOS_CAP_TYPE_L2CA == ca.type || PQOS_CAP_TYPE_L3CA == ca.type) ||
			NULL == ca.u.generic_ptr)
		return PQOS_RETVAL_PARAM;

	if (PQOS_CAP_TYPE_L2CA == ca.type)
		ret = get_avail_rdt_ca(socket, PQOS_MAX_L2CA_COS, &num,
			hi_cos_id, wrap_l2ca(avail_l2_ca));
	else
		ret = get_avail_rdt_ca(socket, PQOS_MAX_L3CA_COS, &num,
			hi_cos_id, wrap_l3ca(avail_l3_ca));

	if (ret != PQOS_RETVAL_OK) {
		printf("Error retrieving list of available %s COS!\n",
			get_type_str_rdt_ca(ca));
		return ret;
	}

	if (num < num_cos) {
		printf("Not enough available COS!\n");
		return PQOS_RETVAL_RESOURCE;
	}

	for (i = 0; i < num_cos; i++)
		if (PQOS_CAP_TYPE_L2CA == ca.type)
			ca.u.l2[i].class_id = avail_l2_ca[i].class_id;
		else
			ca.u.l3[i].class_id = avail_l3_ca[i].class_id;

	return PQOS_RETVAL_OK;
}

static unsigned
get_hi_cos_id(struct pqos_l2ca *l2, struct pqos_l3ca *l3)
{
	unsigned num_l3_cos = 0, num_l2_cos = 0;
	int l2_valid = 0, l3_valid = 0;

	l2_valid = is_valid_rdt_ca(wrap_l2ca(l2));
	l3_valid = is_valid_rdt_ca(wrap_l3ca(l3));

	if (!l2_valid && !l3_valid)
		return 0;

	if (m_cap_l2ca != NULL)
		num_l2_cos = m_cap_l2ca->u.l2ca->num_classes;
	if (m_cap_l3ca != NULL)
		num_l3_cos = m_cap_l3ca->u.l3ca->num_classes;

	if (l2_valid && l3_valid)
		return MIN(num_l2_cos, num_l3_cos);
	else if (l2_valid)
		return num_l2_cos;
	else
		return num_l3_cos;
}

static int
cat_cos_assign(const unsigned socket, const unsigned num_cos,
		struct pqos_l2ca *l2, struct pqos_l3ca *l3)
{
	unsigned i = 0;
	int ret = PQOS_RETVAL_OK;

	if (NULL == l2 && NULL == l3)
		return PQOS_RETVAL_PARAM;

	for (i = 0; i < num_cos; i++) {
		struct pqos_l2ca *l2_cos = NULL;
		struct pqos_l3ca *l3_cos = NULL;

		if (NULL != l2)
			l2_cos = &l2[i];

		if (NULL != l3)
			l3_cos = &l3[i];

		const unsigned hi_cos_id = get_hi_cos_id(l2_cos, l3_cos);

		if (0 == hi_cos_id)
			return PQOS_RETVAL_PARAM;

		if (NULL != m_cap_l3ca && NULL != l3_cos) {
			ret = select_rdt_ca(socket, 1, hi_cos_id,
				wrap_l3ca(l3_cos));
			if (PQOS_RETVAL_OK != ret) {
				printf("Error selecting available L3 COS!\n");
				return ret;
			}

			if (NULL != l2_cos)
				l2_cos->class_id = l3_cos->class_id;
		} else if (NULL != m_cap_l2ca && NULL != l2_cos) {
			ret = select_rdt_ca(socket, 1, hi_cos_id,
				wrap_l2ca(l2_cos));
			if (PQOS_RETVAL_OK != ret) {
				printf("Error selecting available L2 COS!\n");
				return ret;
			}

			if (NULL != l3_cos)
				l3_cos->class_id = l2_cos->class_id;
		} else
			return PQOS_RETVAL_ERROR;
	}

	return PQOS_RETVAL_OK;
}

/*
 * Set L2/L3 configuration, search for available COS on each socket
 * (pqos_l*ca.class_id value is ignored)
 * */
static int
cat_atomic_set(const unsigned num, struct pqos_l2ca *l2ca,
		struct pqos_l3ca *l3ca, cpu_set_t *cpu)
{
	unsigned i = 0, j = 0;
	cpu_set_t cpusets[num][RDT_MAX_SOCKETS];
	int ret = 0;

	memset(cpusets, 0, sizeof(cpusets));

	if (0 == num || NULL == l2ca || NULL == l3ca || NULL == cpu)
		return PQOS_RETVAL_PARAM;

	/*
	 * Make a table with information about cores used
	 * (by each configured class) per socket
	 */
	for (i = 0; i < num; i++) {
		for (j = 0; j < CPU_SETSIZE; j++) {
			unsigned socket = 0;

			if (0 == CPU_ISSET(j, &cpu[i]))
				continue;

			ret = pqos_cpu_get_socketid(m_cpu, j, &socket);
			if (ret != PQOS_RETVAL_OK) {
				printf("Failed to get socket id of "
					"lcore %u!\n", j);
				return ret;
			}
			CPU_SET(j, &cpusets[i][socket]);
		}
	}

	/* Check do we have enough COS available */
	for (i = 0; i < num; i++) {
		for (j = 0; j < RDT_MAX_SOCKETS; j++) {
			if (0 == CPU_COUNT(&cpusets[i][j]))
				continue;

			ret = cat_cos_assign(j, 1, &l2ca[i], &l3ca[i]);
			if (ret != PQOS_RETVAL_OK) {
				printf("Error selecting available COS!\n");
				return ret;
			}
		}
	}

	/* Configure CAT */
	for (i = 0; i < num; i++) {
		for (j = 0; j < RDT_MAX_SOCKETS; j++) {
			unsigned k = 0;

			if (0 == CPU_COUNT(&cpusets[i][j]))
				continue;

			/* Get COS# to be used */
			ret = cat_cos_assign(j, 1, &l2ca[i], &l3ca[i]);
			if (ret != PQOS_RETVAL_OK) {
				printf("Error selecting available COS!\n");
				return ret;
			}

			/* Configure COS */
			if (m_cap_l3ca != NULL && l3ca != NULL) {
				int valid_ca =
					is_valid_rdt_ca(wrap_l3ca(&l3ca[i]));

				if (!valid_ca && 0 != l3ca[i].class_id) {
					/* Set same config as L3 COS#0 */
					ret = get_default_rdt_ca(j,
						wrap_l3ca(&l3ca[i]));
					if (ret != PQOS_RETVAL_OK) {
						printf("Error getting default "
							"L3CA COS!\n");
						return ret;
					}
					valid_ca = 1;
				}

				if (valid_ca) {
					ret = pqos_l3ca_set(j, 1, &l3ca[i]);
					if (ret != PQOS_RETVAL_OK) {
						printf("Error configuring L3CA "
							"COS!\n");
						return ret;
					}
				}
			}


			if (m_cap_l2ca != NULL && l2ca != NULL) {
				int valid_ca =
					is_valid_rdt_ca(wrap_l2ca(&l2ca[i]));

				if (!valid_ca && 0 != l2ca[i].class_id) {
					/* Set same config as L2 COS#0 */
					ret = get_default_rdt_ca(j,
						wrap_l2ca(&l2ca[i]));
					if (ret != PQOS_RETVAL_OK) {
						printf("Error getting default "
							"L2CA COS!\n");
						return ret;
					}
					valid_ca = 1;
				}

				if (valid_ca) {
					ret = pqos_l2ca_set(j, 1, &l2ca[i]);
					if (ret != PQOS_RETVAL_OK) {
						printf("Error configuring L2CA "
							"COS!\n");
						return ret;
					}
				}
			}

			/* Associate COS to cores */
			for (k = 0; k < CPU_SETSIZE; k++) {
				if (0 == CPU_ISSET(k, &cpusets[i][j]))
					continue;

				unsigned class_id = 0;

				if (m_cap_l3ca != NULL && l3ca != NULL)
					class_id = l3ca[i].class_id;
				else if (m_cap_l2ca != NULL && l2ca != NULL)
					class_id = l2ca[i].class_id;
				else
					return PQOS_RETVAL_ERROR;

				ret = pqos_alloc_assoc_set(k, class_id);
				if (ret != PQOS_RETVAL_OK) {
					printf("Error associating cpu %u with "
						"COS %u!\n", k, class_id);
					return ret;
				}
			}
		}
	}

	return PQOS_RETVAL_OK;
}

/* Validate requested L3 CAT configuration */
static int
cat_validate(void)
{
	int ret = 0;

	ret = check_cpus();
	if (ret != 0)
		return ret;

	ret = check_cdp();
	if (ret != 0)
		return ret;

	ret = check_supported();
	if (ret != 0)
		return ret;

	ret = check_cbm_len_and_contention();
	if (ret != 0)
		return ret;

	ret = check_cpus_overlapping();
	if (ret != 0)
		return ret;

	return 0;
}

/*
 * Check is it possible to fulfill requested L3 CAT configuration
 * and then configure system.
 */
int
cat_set(void)
{
	struct pqos_l3ca l3ca[g_cfg.config_count];
	struct pqos_l2ca l2ca[g_cfg.config_count];
	cpu_set_t cpu[g_cfg.config_count];
	unsigned i = 0;
	int ret = 0;

	/* Validate cmd line configuration */
	ret = cat_validate();
	if (ret != 0) {
		printf("CAT: Requested CAT configuration is not valid!\n");
		return ret;
	}

	for (i = 0; i < g_cfg.config_count; i++) {
		l3ca[i] = g_cfg.config[i].l3;
		l2ca[i] = g_cfg.config[i].l2;
		cpu[i] = g_cfg.config[i].cpumask;
	}

	ret =  cat_atomic_set(g_cfg.config_count, l2ca, l3ca, cpu);
	if (ret != PQOS_RETVAL_OK) {
		printf("CAT: Failed to configure CAT!\n");
		ret = -EFAULT;
	}

	return ret;
}

int
cat_reset(void)
{
	unsigned i = 0;
	int ret = 0;

	/* Associate COS#0 to cores */
	for (i = 0; i < CPU_SETSIZE; i++) {
		if (0 == CPU_ISSET(i, &g_cfg.reset_cpuset))
			continue;

		ret = pqos_alloc_assoc_set(i, 0);
		if (ret != PQOS_RETVAL_OK) {
			printf("Error associating COS,"
				"core: %u, COS: 0!\n", i);
			return -EFAULT;
		}
	}

	return 0;
}

void
cat_fini(void)
{
	int ret = 0;

	if (g_cfg.verbose)
		printf("Shutting down PQoS library...\n");

	/* deallocate all the resources */
	ret = pqos_fini();
	if (ret != PQOS_RETVAL_OK && ret != PQOS_RETVAL_INIT)
		printf("Error shutting down PQoS library!\n");

	m_cap = NULL;
	m_cpu = NULL;
	m_cap_l3ca = NULL;
	memset(g_cfg.config, 0, sizeof(g_cfg.config));
	g_cfg.config_count = 0;
}

void
cat_exit(void)
{
	unsigned i = 0;
	int ret = 0;

	/* if lib is not initialized, do nothing */
	if (m_cap == NULL && m_cpu == NULL)
		return;

	if (g_cfg.verbose)
		printf("CAT: Reverting CAT configuration...\n");

	for (i = 0; i < g_cfg.config_count; i++) {
		unsigned j = 0;

		for (j = 0; j < m_cpu->num_cores; j++) {
			unsigned cpu_id = 0;

			cpu_id = m_cpu->cores[j].lcore;
			if (CPU_ISSET(cpu_id, &g_cfg.config[i].cpumask)
					== 0)
				continue;

			ret = pqos_alloc_assoc_set(cpu_id, 0);
			if (ret != PQOS_RETVAL_OK)
				printf("Failed to associate COS 0 to "
					"cpu %u\n", cpu_id);
		}
	}

	cat_fini();
}

/* Signal handler to do clean-up on exit on signal */
static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		printf("\nRDTSET: Signal %d received, preparing to exit...\n",
			signum);

		cat_exit();

		/* exit with the expected status */
		signal(signum, SIG_DFL);
		kill(getpid(), signum);
	}
}

int
cat_init(void)
{
	int ret = 0;
	struct pqos_config cfg;

	if (m_cap != NULL || m_cpu != NULL) {
		printf("CAT: CAT module already initialized!\n");
		return -EEXIST;
	}

	/* PQoS Initialization - Check and initialize CAT capability */
	memset(&cfg, 0, sizeof(cfg));
	cfg.fd_log = STDOUT_FILENO;
	cfg.verbose = 0;
	cfg.cdp_cfg = PQOS_REQUIRE_CDP_ANY;
	ret = pqos_init(&cfg);
	if (ret != PQOS_RETVAL_OK) {
		printf("CAT: Error initializing PQoS library!\n");
		ret = -EFAULT;
		goto err;
	}

	/* Get capability and CPU info pointer */
	ret = pqos_cap_get(&m_cap, &m_cpu);
	if (ret != PQOS_RETVAL_OK || m_cap == NULL || m_cpu == NULL) {
		printf("CAT: Error retrieving PQoS capabilities!\n");
		ret = -EFAULT;
		goto err;
	}

	/* Get L2CA capabilities */
	ret = pqos_cap_get_type(m_cap, PQOS_CAP_TYPE_L2CA, &m_cap_l2ca);
	if (g_cfg.verbose && (ret != PQOS_RETVAL_OK || m_cap_l2ca == NULL))
		printf("CAT: PQOS_CAP_TYPE_L2CA capability not supported!\n");

	/* Get L3CA capabilities */
	ret = pqos_cap_get_type(m_cap, PQOS_CAP_TYPE_L3CA, &m_cap_l3ca);
	if (g_cfg.verbose && (ret != PQOS_RETVAL_OK || m_cap_l3ca == NULL))
		printf("CAT: PQOS_CAP_TYPE_L3CA capability not supported!\n");

	if (m_cap_l3ca == NULL && m_cap_l2ca == NULL) {
		printf("CAT: PQOS_CAP_TYPE_L2CA, PQOS_CAP_TYPE_L3CA "
			"capabilities not supported!\n");
		ret = -EFAULT;
		goto err;
	}

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	ret = atexit(cat_exit);
	if (ret != 0) {
		ret = -EFAULT;
		printf("CAT: Cannot set exit function\n");
		goto err;
	}

	return 0;

err:
	/* deallocate all the resources */
	cat_fini();
	return ret;
}

void
print_cmd_line_rdt_config(void)
{
	char cpustr[CPU_SETSIZE * 3];
	unsigned i;

	if (CPU_COUNT(&g_cfg.reset_cpuset) != 0) {
		cpuset_to_str(cpustr, sizeof(cpustr), &g_cfg.reset_cpuset);
		printf("CAT Reset: CPUs: %s\n", cpustr);
	}

	for (i = 0; i < g_cfg.config_count; i++) {
		struct rdt_ca ca_array[2];
		unsigned j;

		if (CPU_COUNT(&g_cfg.config[i].cpumask) == 0)
			continue;

		cpuset_to_str(cpustr, sizeof(cpustr), &g_cfg.config[i].cpumask);

		ca_array[0] = wrap_l2ca(&g_cfg.config[i].l2);
		ca_array[1] = wrap_l3ca(&g_cfg.config[i].l3);

		for (j = 0; j < DIM(ca_array); j++) {
			if (!is_valid_rdt_ca(ca_array[j]))
				continue;
			printf("%s Allocation: CPUs: %s ",
				get_type_str_rdt_ca(ca_array[j]), cpustr);
			print_rdt_ca(ca_array[j]);
			printf("\n");
		}
	}
}

