#ifndef __SAAKM_H
#define __SAAKM_H

#include <linux/sched.h>
#include <linux/kref.h>

#include "sched.h"

#define MAX_POLICY_NAME_LEN     32

#ifdef __KERNEL__

enum saakm_core_state { SAAKM_ACTIVE_CORE, SAAKM_IDLE_CORE };

enum saakm_rq_type { RBTREE, LIST, FIFO };

struct saakm_rq {
	enum saakm_rq_type type;
	union {
		struct rb_root root;
		struct list_head head;
	};
	unsigned int cpu;
	enum saakm_state state;
	unsigned int nr_tasks;
	int (*order_fn)(struct task_struct *a, struct task_struct *b);
};

void init_saakm_rq(struct saakm_rq *rq, enum saakm_rq_type type,
		     unsigned int cpu, enum saakm_state state,
		     int (*order_fn)(struct task_struct *a,
				     struct task_struct *b));

int saakm_add_task(struct saakm_rq *rq, struct task_struct *data);
struct task_struct *saakm_remove_task(struct saakm_rq *rq,
					struct task_struct *data);
struct task_struct *saakm_first_task(struct saakm_rq *rq);


struct task_struct *get_saakm_current(int cpu);

extern int nb_topology_levels;
DECLARE_PER_CPU(struct topology_level*, topology_levels);

struct saakm_runtime_metadata;

struct process_event {
	struct task_struct *target;
	int cpu;
	unsigned int flags;
};

struct core_event {
	unsigned int target; // SaaKM core
};

struct saakm_policy {
	struct module *kmodule;
	struct list_head list;
	struct saakm_module_routines *routines;
	void *data;
	s64 id;
	char name[MAX_POLICY_NAME_LEN];
};

struct saakm_module_routines {
	enum saakm_core_state (*get_core_state)(struct saakm_policy *policy,
						  struct core_event *e);

	int (*new_prepare)(struct saakm_policy *policy,
			   struct process_event *e);
	void (*new_place)(struct saakm_policy *policy,
			  struct process_event *e);
	void (*new_end)(struct saakm_policy *policy,
			struct process_event *e);

	void (*tick)(struct saakm_policy *policy, struct process_event *e);
	void (*yield)(struct saakm_policy *policy, struct process_event *e);
	void (*block)(struct saakm_policy *policy, struct process_event *e);

	int (*unblock_prepare)(struct saakm_policy *policy,
			       struct process_event *e);
	void (*unblock_place)(struct saakm_policy *policy,
			      struct process_event *e);
	void (*unblock_end)(struct saakm_policy *policy,
			    struct process_event *e);

	void (*terminate)(struct saakm_policy *policy,
			  struct process_event *e);
	void (*schedule)(struct saakm_policy *policy, unsigned int cpu);

	void (*newly_idle)(struct saakm_policy *policy, struct core_event *e);
	void (*enter_idle)(struct saakm_policy *policy, struct core_event *e);
	void (*exit_idle)(struct saakm_policy *policy, struct core_event *e);

	int (*balancing_select)(struct saakm_policy *policy,
				 struct core_event *e);

	void (*core_entry)(struct saakm_policy *policy, struct core_event *e);
	void (*core_exit)(struct saakm_policy *policy,
			  struct core_event *e);

	bool (*checkparam_attr)(const struct sched_attr *attr);
	void (*setparam_attr)(struct task_struct *p,
			      const struct sched_attr *attr);
	void (*getparam_attr)(struct task_struct *p,
			      struct sched_attr *attr);
	bool (*attr_changed)(struct task_struct *p,
			     const struct sched_attr *attr);
	void (*reweight_task)(struct saakm_policy *policy,
			      struct task_struct *p,
			      const struct load_weight *new_weight);

	int (*init)(struct saakm_policy *policy);
	int (*free_metadata)(struct saakm_policy *policy);

	int (*can_be_default)(struct saakm_policy *policy);
	bool (*attach)(struct saakm_policy *policy, struct task_struct *task,
		       char *command);
};

extern struct proc_dir_entry *ipa_procdir;

/* topology level types, used as flags in struct topology_level */
#define DOMAIN_SMT   0x1      	/* cpus share computing units (simultaneous multi-threading) */
#define DOMAIN_CLUSTER  0x2	/* cpus share LLC tags or L2 cache */
#define DOMAIN_CACHE 0x4	/* cpus share a hardware cache */
#define DOMAIN_NUMA  0x8	/* cpus may be on different NUMA nodes */

struct topology_level {
	cpumask_t cores;
	int flags;
	struct topology_level *next;
};


/* Wake flags. The first three directly map to some SD flag value */
#define SAAKM_WF_EXEC			0x02 /* Wakeup after exec; maps to SD_BALANCE_EXEC */
#define SAAKM_WF_FORK			0x04 /* Wakeup after fork; maps to SD_BALANCE_FORK */
#define SAAKM_WF_TTWU			0x08 /* Wakeup;            maps to SD_BALANCE_WAKE */

#define SAAKM_WF_SYNC			0x10 /* Waker goes to sleep after wakeup */
#define SAAKM_WF_MIGRATED		0x20 /* Internal use, task got migrated */
#define SAAKM_WF_CURRENT_CPU		0x40 /* Prefer to move the wakee to the current CPU. */
#define SAAKM_WF_RQ_SELECTED		0x80 /* ->select_task_rq() was called */

void change_state(struct task_struct *p, enum saakm_state next_state,
		  unsigned int next_cpu, struct saakm_rq *next_rq);
struct task_struct *saakm_first_of_state(enum saakm_state state,
					   unsigned int cpu);

struct task_struct *saakm_get_task_of(void *proc);

int saakm_add_policy(struct saakm_policy *policy);
int saakm_remove_policy(struct saakm_policy *policy);

bool saakm_smt_active(void);

int count(enum saakm_state state, unsigned int cpu);

#define saakm_rq_lock(p)         (&task_rq(p)->lock)
#define policy_metadata(t)         ((t)->saakm.policy_metadata)
#define saakm_task_state(p)      ((p)->saakm.state)
#define saakm_task_rq(p)         ((p)->saakm.rq)
#define saakm_task_policy(p)     ((p)->saakm.policy)

/*
 * Accessors used in policy modules
 */
#define saakm_core(cpu)          (per_cpu(core, (cpu)))
#define saakm_state(cpu)         (per_cpu(state_info, (cpu)))


extern void saakm_lock_core(unsigned int id);
extern int saakm_trylock_core(unsigned int id);
extern void saakm_unlock_core(unsigned int id);

extern int saakm_just_queued(struct task_struct *p);

#endif /* __KERNEL__ */

#endif /* __SAAKM_H */
