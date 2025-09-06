#ifndef SAAKM_H
#define SAAKM_H

#include <linux/saakm.h>
#include <linux/latencytop.h>
#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/cpuidle.h>
#include <linux/profile.h>
#include <linux/interrupt.h>
#include <linux/mempolicy.h>
#include <linux/migrate.h>
#include <linux/task_work.h>
#include <linux/proc_fs.h>
#include <linux/sort.h>


extern struct list_head saakm_modules;
extern struct list_head saakm_policies;

int saakm_set_policy(char *policies_str);

extern rwlock_t saakm_rwlock;

/* Variables exposed by the sysfs interface */
extern int saakm_fsm_check;
extern int saakm_fsm_log;
extern int saakm_sched_class_log;

#ifdef CONFIG_CGROUP_SAAKM
struct saakm_group {
	struct cgroup_subsys_state css;

	struct saakm_policy *policy;
};

#define saakm_group_of(css) (container_of(css, struct saakm_group, css))
#endif	/* CONFIG_CGROUP_SAAKM */

#endif	/* SAAKM_H */
