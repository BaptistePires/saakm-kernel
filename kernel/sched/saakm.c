// SPDX-License-Identifier: GPL-2.0

#include "asm-generic/rwonce.h"
#include "linux/saakm.h"
#include "linux/compiler.h"
#include "linux/export.h"
#include "linux/jump_label.h"
#include "linux/mutex.h"
#include "linux/sched.h"


#include "linux/sched/smt.h"
#include <linux/lockdep.h>
#include <linux/cpufreq.h>
#include <linux/kgdb.h>
#include <uapi/linux/sched/types.h>
#include <linux/spinlock.h>
#include <linux/percpu-rwsem.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <trace/events/sched.h>
#include <linux/sched/cputime.h>
#include <linux/mmu_context.h>

#include "sched.h"
#include "saakm.h"

/*
 * Allows to bypass fair optimizations that assume that
 * there are no schedclass other than idle after them.
*/
DEFINE_STATIC_KEY_FALSE(__saakm_policy_loaded);
DEFINE_MUTEX(saakm_policy_loaded_mutex);


LIST_HEAD(saakm_policies);
s64 num_saakm_policies;
s64 saakm_policies_id;

rwlock_t saakm_rwlock;

/* Current running task per type */
DEFINE_PER_CPU(struct task_struct *, saakm_current);

struct task_struct *get_saakm_current(int cpu)
{
	return per_cpu(saakm_current, cpu);
}
EXPORT_SYMBOL(get_saakm_current);

void saakm_lock_core(unsigned int id)
{
	raw_spin_lock(&cpu_rq(id)->__lock);
}
EXPORT_SYMBOL(saakm_lock_core);

int saakm_trylock_core(unsigned int id)
{
	return raw_spin_trylock(&cpu_rq(id)->__lock);
}
EXPORT_SYMBOL(saakm_trylock_core);

void saakm_unlock_core(unsigned int id)
{
	raw_spin_unlock(&cpu_rq(id)->__lock);
}
EXPORT_SYMBOL(saakm_unlock_core);

static bool __saakm_policy_exists_nolock(struct saakm_policy *policy)
{
	struct saakm_policy *p;
	bool ret = false;

	list_for_each_entry(p, &saakm_policies, list) {
		if (!strcmp(p->name, policy->name)) {
			ret = true;
			break;
		}
	}

	return ret;
}

static bool saakm_policy_exists(struct saakm_policy *policy)
{
	bool ret;
	unsigned long flags;

	read_lock_irqsave(&saakm_rwlock, flags);
	ret = __saakm_policy_exists_nolock(policy);
	read_unlock_irqrestore(&saakm_rwlock, flags);

	return ret;
}

int saakm_add_policy(struct saakm_policy *policy)
{
	unsigned long flags;
	int ret = 0;

	/* Check if policy exists */
	if (saakm_policy_exists(policy))
		return -EINVAL;

	/* Check if given policy seems correctly setup */
	if (!policy->routines)
		return -EINVAL;

	/* Let's set this up */
	write_lock_irqsave(&saakm_rwlock, flags);
	INIT_LIST_HEAD(&policy->list);
	write_unlock_irqrestore(&saakm_rwlock, flags);

	/* Insert policy to activate it after checking existence again */
	write_lock_irqsave(&saakm_rwlock, flags);
	if (__saakm_policy_exists_nolock(policy)) {
		ret = -EINVAL;
		goto end;
	}
	ret = policy->routines->init(policy);
	if (ret)
		goto end;
	policy->id = saakm_policies_id++;
	list_add_tail(&policy->list, &saakm_policies);

end:
	write_unlock_irqrestore(&saakm_rwlock, flags);

	if (!ret) {
		mutex_lock(&saakm_policy_loaded_mutex);
		static_branch_inc(&__saakm_policy_loaded);
		mutex_unlock(&saakm_policy_loaded_mutex);
	}

	return ret;
}
EXPORT_SYMBOL(saakm_add_policy);

int saakm_remove_policy(struct saakm_policy *policy)
{
	unsigned long flags;
	int ret = 0;

	/* Fail if policy is not inserted */
	write_lock_irqsave(&saakm_rwlock, flags);
	if (!__saakm_policy_exists_nolock(policy)) {
		ret = -EINVAL;
		goto end;
	}

	list_del(&policy->list);

end:
	write_unlock_irqrestore(&saakm_rwlock, flags);

	
	if (!ret) {
		mutex_lock(&saakm_policy_loaded_mutex);
		static_branch_dec(&__saakm_policy_loaded);
		mutex_unlock(&saakm_policy_loaded_mutex);
	}

	return ret;
}
EXPORT_SYMBOL(saakm_remove_policy);

bool saakm_smt_active(void)
{
	return sched_smt_active();
}
EXPORT_SYMBOL(saakm_smt_active);

static void saakm_core_entry(struct saakm_policy *policy, unsigned int core)
{
	struct core_event e = { .target = core };

	WARN(!policy->routines->core_entry,
	     "%s is NULL in policy %s\n", __func__, policy->name);

	policy->routines->core_entry(policy, &e);
}

static void saakm_core_exit(struct saakm_policy *policy, unsigned int core)
{
	struct core_event e = { .target = core };

	WARN(!policy->routines->core_exit,
	     "%s is NULL in policy %s\n", __func__, policy->name);

	policy->routines->core_exit(policy, &e);
}

static enum saakm_core_state saakm_get_core_state(struct saakm_policy *policy,
					       unsigned int core)
{
	struct core_event e = { .target = core };

	WARN(!policy->routines->get_core_state,
	     "%s is NULL in policy %s\n", __func__, policy->name);

	return policy->routines->get_core_state(policy, &e);
}

static int saakm_new_prepare(struct process_event *e)
{
	struct task_struct *p = e->target;
	struct saakm_policy *policy;
	unsigned long flags;

	/*
	 * we acquire this lock to prevent the policy from being removed before
	 * incrementing the refcount
	 */
	read_lock_irqsave(&saakm_rwlock, flags);
	policy = saakm_task_policy(p);
	if (!policy || !try_module_get(policy->kmodule)) {
		read_unlock_irqrestore(&saakm_rwlock, flags);
		return -1;
	}

	WARN(!policy->routines->new_prepare,
	     "%s is NULL in policy %s\n", __func__, policy->name);

	read_unlock_irqrestore(&saakm_rwlock, flags);

	return policy->routines->new_prepare(policy, e);
}

static void saakm_new_place(struct process_event *e)
{
	struct task_struct *p = e->target;
	struct saakm_policy *policy;

	lockdep_assert_held(&task_rq(p)->__lock);

	policy = saakm_task_policy(p);

	WARN(!policy->routines->new_place,
	     "%s is NULL in policy %s\n", __func__, policy->name);

	policy->routines->new_place(policy, e);
}

/* Not used yet. Cause compiling errors. */
// static void saakm_new_end(struct process_event *e)
// {
// 	struct task_struct *p = e->target;
// 	struct saakm_policy *policy;

// 	policy = saakm_task_policy(p);

// 	WARN(!policy->routines->new_end,
// 	     "%s is NULL in policy %s\n", __func__, policy->name);

// 	policy->routines->new_end(policy, e);
// }

static void saakm_tick(struct process_event *e)
{
	struct task_struct *p = e->target;
	struct rq *rq = task_rq(p);
	struct saakm_policy *policy;

	/*
	 * Make sure the rq lock is held, because we will need to call
	 * resched_curr() to schedule another thread.
	 */
	lockdep_assert_held(&rq->__lock);

	policy = saakm_task_policy(p);

	WARN(!policy->routines->tick,
	     "%s is NULL in policy %s\n", __func__, policy->name);

	policy->routines->tick(policy, e);
}

static void saakm_yield(struct process_event *e)
{
	struct task_struct *p = e->target;
	struct rq *rq = task_rq(p);
	struct saakm_policy *policy;

	/*
	 * Make sure the rq lock is held, because we will need to call
	 * resched_curr() to schedule another thread.
	 */
	lockdep_assert_held(&rq->__lock);

	policy = saakm_task_policy(p);

	WARN(!policy->routines->yield,
	     "%s is NULL in policy %s\n", __func__, policy->name);

	policy->routines->yield(policy, e);
}

static void saakm_block(struct process_event *e)
{
	struct task_struct *p = e->target;
	struct rq *rq = task_rq(p);
	struct saakm_policy *policy;

	/*
	 * Make sure the rq lock is held, because we will need to call
	 * resched_curr() to schedule another thread.
	 */
	lockdep_assert_held(&rq->__lock);

	policy = saakm_task_policy(p);

	WARN(!policy->routines->block,
	     "%s is NULL in policy %s\n", __func__, policy->name);

	policy->routines->block(policy, e);
}

static int saakm_unblock_prepare(struct process_event *e)
{
	struct task_struct *p = e->target;
	struct saakm_policy *policy;

	lockdep_assert_held(&p->pi_lock);

	policy = saakm_task_policy(p);

	WARN(!policy->routines->unblock_prepare,
	     "%s is NULL in policy %s\n", __func__, policy->name);

	return policy->routines->unblock_prepare(policy, e);
}

static void saakm_unblock_place(struct process_event *e)
{
	struct task_struct *p = e->target;
	struct saakm_policy *policy;

	lockdep_assert_held(&task_rq(p)->__lock);

	policy = saakm_task_policy(p);

	WARN(!policy->routines->unblock_place,
	     "%s is NULL in policy %s\n", __func__, policy->name);

	policy->routines->unblock_place(policy, e);
}

/* Not used yet. Cause compiling error. */
// static void saakm_unblock_end(struct process_event *e)
// {
// 	struct task_struct *p = e->target;
// 	struct saakm_policy *policy;

// 	lockdep_assert_held(&p->pi_lock);

// 	policy = saakm_task_policy(p);

// 	WARN(!policy->routines->unblock_end,
// 	     "%s is NULL in policy %s\n", __func__, policy->name);

// 	policy->routines->unblock_end(policy, e);
// }

static void saakm_terminate(struct process_event *e)
{
	struct task_struct *p = e->target;
	struct rq *rq = task_rq(p);
	struct saakm_policy *policy;

	lockdep_assert_held(&rq->__lock);

	policy = saakm_task_policy(p);

	WARN(!policy->routines->terminate,
	     "%s is NULL in policy %s\n", __func__, policy->name);

	policy->routines->terminate(policy, e);

	saakm_task_policy(p) = NULL;
	module_put(policy->kmodule);
}

static void saakm_schedule(struct saakm_policy *policy, unsigned int core)
{
	struct rq *rq = cpu_rq(core);

	/* IRQs are apparently disabled. */
	WARN_ON(!irqs_disabled());

	/*
	 * We *must* hold the rq lock here, otherwise we can make a ready task
	 * running while another thread is stealing it.
	 */
	lockdep_assert_held(&rq->__lock);

	WARN(!policy->routines->schedule,
	     "%s is NULL in policy %s\n", __func__, policy->name);

	policy->routines->schedule(policy, core);
}

static void saakm_newly_idle(struct saakm_policy *policy, unsigned int core,
			struct rq_flags *rf)
{
	struct core_event e = { .target = core };
	struct rq *rq = cpu_rq(core);

	WARN(!policy->routines->newly_idle,
	     "%s is NULL in policy %s\n", __func__, policy->name);

	/*
	 * When newly_idle() is called by schedule(), the rq->__lock is
	 * held. However, the handler may want to lock multiple rq->__lock
	 * (idle balancing for example). To allow this, we unpin and
	 * unlock rq->__lock before. We will put everything back to normal
	 * upon returning from the handler.
	 */
	rq_unpin_lock(rq, rf);
	raw_spin_unlock(&rq->__lock);

	policy->routines->newly_idle(policy, &e);

	raw_spin_lock(&rq->__lock);
	rq_repin_lock(rq, rf);
}

static void saakm_enter_idle(struct saakm_policy *policy, unsigned int core)
{
	struct core_event e = { .target = core };

	WARN(!policy->routines->enter_idle,
	     "%s is NULL in policy %s\n", __func__, policy->name);

	policy->routines->enter_idle(policy, &e);
}

static void saakm_exit_idle(struct saakm_policy *policy, unsigned int core)
{
	struct core_event e = { .target = core };

	WARN(!policy->routines->exit_idle,
	     "%s is NULL in policy %s\n", __func__, policy->name);

	policy->routines->exit_idle(policy, &e);
}

static int saakm_balancing_select(void)
{
	unsigned int core = smp_processor_id();
	struct saakm_policy *policy;
	struct core_event e = { .target = core };
	// struct rq_flags rf;
	unsigned long flags;
	int ret = 0;

	// rq_unpin_lock(cpu_rq(core), &rf);
	// TODO : temp fix to see if it works..
	read_lock_irqsave(&saakm_rwlock, flags);
	list_for_each_entry(policy, &saakm_policies, list) {
		if (policy->routines->balancing_select) {
			ret = policy->routines->balancing_select(policy, &e);
			if (ret)
				break;
		}
	}
	read_unlock_irqrestore(&saakm_rwlock, flags);
	// rq_repin_lock(cpu_rq(core), &rf);
	return ret;
}


struct task_struct *saakm_get_task_of(void *proc)
{
	struct sched_saakm_entity *saakm;

	saakm = container_of(proc, struct sched_saakm_entity,
			       policy_metadata);
	return container_of(saakm, struct task_struct, saakm);
}
EXPORT_SYMBOL(saakm_get_task_of);

bool __checkparam_saakm(const struct sched_attr *attr,
			  struct saakm_policy *policy)
{
	if (policy->routines->checkparam_attr)
		return policy->routines->checkparam_attr(attr);
	return true;
}

void __setparam_saakm(struct task_struct *p, const struct sched_attr *attr)
{
	struct saakm_policy *policy = saakm_task_policy(p);

	if (policy->routines->setparam_attr)
		policy->routines->setparam_attr(p, attr);
}

void __getparam_saakm(struct task_struct *p, struct sched_attr *attr)
{
	struct saakm_policy *policy = saakm_task_policy(p);

	if (policy->routines->getparam_attr)
		policy->routines->getparam_attr(p, attr);
}

bool saakm_attr_changed(struct task_struct *p, const struct sched_attr *attr)
{
	struct saakm_policy *policy = saakm_task_policy(p);

	if (policy->routines->attr_changed)
		return policy->routines->attr_changed(p, attr);
	return false;
}

/*
 * Check the validity of an saakm transition
 */
static void check_saakm_transition(struct task_struct *p,
				     enum saakm_state next_state,
				     unsigned int next_cpu)
{
	enum saakm_state prev_state = p->saakm.state;
	unsigned int prev_cpu = task_cpu(p);

	switch (prev_state) {
	case SAAKM_NOT_QUEUED:
	case SAAKM_BLOCKED:
		if (next_state != SAAKM_READY)
			goto wrong_transition;
		goto no_cpu_check;
	case SAAKM_READY:
		if (next_state == SAAKM_MIGRATING)
			goto no_cpu_check;
		if (next_state != SAAKM_RUNNING)
			goto wrong_transition;
		break;
	case SAAKM_RUNNING:
		if (next_state == SAAKM_NOT_QUEUED ||
		    next_state == SAAKM_MIGRATING)
			goto wrong_transition;
		break;
	case SAAKM_READY_TICK:
		if (next_state != SAAKM_READY)
			goto wrong_transition;
		break;
	case SAAKM_MIGRATING:
		if (next_state != SAAKM_READY)
			goto wrong_transition;
		break;
	case SAAKM_TERMINATED:
		goto wrong_transition;
	default:
		goto wrong_transition;
	}

	if (prev_cpu != next_cpu)
		goto wrong_transition;

no_cpu_check:
	return;

wrong_transition:
	pr_warn("[WARN] %s: [pid=%d] Incorrect transition %s[%d] -> %s[%d]\n",
		__func__, p->pid,
		saakm_state_to_str(prev_state), prev_cpu,
		saakm_state_to_str(next_state), next_cpu);

#ifdef CONFIG_SAAKM_PANIC_ON_BAD_TRANSITION
	BUG();
#endif
}

/*
 * Move task p from its current runqueue to the runqueue next_rq with
 * state = next_state
 */
static void change_rq(struct task_struct *p, enum saakm_state next_state,
		      struct saakm_rq *next_rq)
{
	struct saakm_rq *prev_rq = NULL;
	unsigned int prev_cpu, next_cpu;
	enum saakm_state prev_state;

	prev_rq = saakm_task_rq(p);
	prev_cpu = task_cpu(p);
	prev_state = saakm_task_state(p);

	if (prev_rq) {
		lockdep_assert_held(&task_rq(p)->__lock);
		p = saakm_remove_task(prev_rq, p);
		prev_rq->nr_tasks--;
	}

	saakm_task_rq(p) = next_rq;
	saakm_task_state(p) = next_state;

	if (next_rq) {
		next_cpu = next_rq->cpu;
		next_state = next_rq->state;
		lockdep_assert_held(&cpu_rq(next_cpu)->__lock);
		if (saakm_add_task(next_rq, p))
			pr_err("[ERR] %s(pid=%d, cpu=%u) failed. Gonna crash soon...\n",
			       __func__, p->pid, next_cpu);
		next_rq->nr_tasks++;
	}
}

/*
 * Main function handling state change of an saakm task
 */
void change_state(struct task_struct *p, enum saakm_state next_state,
		  unsigned int next_cpu, struct saakm_rq *next_rq)
{
	unsigned int prev_cpu;
	enum saakm_state prev_state;
	struct saakm_rq *prev_rq = NULL;

	/* Safety checks */
	if (!p) {
		pr_err("[ERR] %s: Called with a null process! Exiting...\n",
		       __func__);
		return;
	}

	/* Get current task fields */
	prev_cpu = task_cpu(p);
	prev_state = saakm_task_state(p);
	prev_rq = saakm_task_rq(p);

	/*
	 * Safety checks on parameters, if badly set, use the process'
	 * current values
	 */
	if (next_cpu < 0)
		next_cpu = prev_cpu;
	if (next_state < 0)
		next_state = prev_state;
	if (next_state == SAAKM_RUNNING ||
	    next_state == SAAKM_MIGRATING ||
	    next_state == SAAKM_TERMINATED)
		next_rq = NULL;
	if (next_rq) {
		if (next_cpu != next_rq->cpu ||
		    (next_state != next_rq->state &&
		     next_state != SAAKM_READY_TICK &&
		     next_rq->state != SAAKM_READY)) {
			pr_warn("[WARN] %s: Discrepancy in parameters: next_state = %s; next_cpu = %d, next_rq = [cpu=%d, state=%s]. Using next_rq values\n",
				__func__,
				saakm_state_to_str(next_state),
				next_cpu, next_rq->cpu,
				saakm_state_to_str(next_rq->state));

#ifdef CONFIG_SAAKM_PANIC_ON_BAD_TRANSITION
			BUG();
#endif
			next_cpu = next_rq->cpu;
			next_state = next_rq->state;
		}
	}

	/* If no change, return */
	if (prev_cpu == next_cpu &&
	    prev_state == next_state &&
	    prev_rq == next_rq)
		return;

	if (unlikely(saakm_fsm_log))
		pr_info("[pid=%d] %s[%d] -> %s[%d]\n",
			p->pid, saakm_state_to_str(prev_state), prev_cpu,
			saakm_state_to_str(next_state), next_cpu);

	/* Check transition validity if necessary */
	if (unlikely(saakm_fsm_check))
		check_saakm_transition(p, next_state, next_cpu);

	/* Do the actual rq change */
	change_rq(p, next_state, next_rq);


	/* Now, let's do transition specific handling */

	/* RUNNING -> x */
	if (prev_state == SAAKM_RUNNING)
		per_cpu(saakm_current, prev_cpu) = NULL;

	/* x -> RUNNING */
	if (next_state == SAAKM_RUNNING) {
		if (per_cpu(saakm_current, next_cpu))
			pr_warn("[WARN] putting a task in RUNNING but there is already another task! We preempt it to avoid potential bugs. Should not happen !!!\n");

		per_cpu(saakm_current, next_cpu) = p;
	}

	/* MIGRATING -> READY */
	if (prev_state == SAAKM_MIGRATING) {
		activate_task(cpu_rq(next_cpu), p, 0);
		p->on_rq = TASK_ON_RQ_QUEUED;
	}

	/* READY -> MIGRATING */
	if (next_state == SAAKM_MIGRATING) {
		p->on_rq = TASK_ON_RQ_MIGRATING;
		deactivate_task(cpu_rq(prev_cpu), p, 0);
		set_task_cpu(p, next_cpu);
	}

	/* RUNNING -> READY_TICK */
	if (next_state == SAAKM_READY_TICK)
		resched_curr(cpu_rq(next_cpu));

	/*
	 * Let's check that we have someone RUNNING if not, trigger a resched
	 */
	if (!per_cpu(saakm_current, prev_cpu) &&
	    cpu_rq(prev_cpu)->nr_running &&
	    prev_cpu == next_cpu)
		resched_curr(cpu_rq(prev_cpu));

	if (task_cpu(p) != next_cpu ||
	    (next_rq && task_cpu(p) != next_rq->cpu)) {
		pr_warn("[WARN] Discrepency with task %d (task_cpu()=%d, next_cpu=%d, next_rq->cpu=%d)\n",
			p->pid, task_cpu(p), next_cpu,
			next_rq ? next_rq->cpu : -1);
		dump_stack();
	}
}
EXPORT_SYMBOL(change_state);

/*
 * Return the number of tasks on the cpu (not only in SCHED_SAAKM !!!!)
 */
int count(enum saakm_state state, unsigned int cpu)
{
	if (state == SAAKM_READY || state == SAAKM_READY_TICK)
		return cpu_rq(cpu)->nr_running;
	return -1;
}
EXPORT_SYMBOL(count);

static void enqueue_task_saakm(struct rq *rq,
				 struct task_struct *p,
				 int flags)
{
	struct process_event e = { .target = p, .cpu = smp_processor_id() };
	enum saakm_core_state cstate;

	if (unlikely(saakm_sched_class_log))
		pr_info("In %s [pid=%d, rq=%d]\n",
			__func__, p->pid, rq->cpu);

	/* task has no saakm policy, just increment rq->nr_running */
	if (!saakm_task_policy(p)) {
		pr_warn("[WARN] %s: called on a task with no saakm policy set.\n",
			__func__);
		goto end;
	}

	/*
	 * We are changing attributes of a thread. We don't need to do anything,
	 * just update rq->nr_running/count_ready
	 */
	if (flags & ATTR_CHANGE)
		goto end;

	/*
	 * We are in the middle of a migration. We don't need to do anything,
	 * just update rq->nr_running/count_ready
	 */
	if (task_on_rq_migrating(p) && !(flags & OUSTED))
		goto end;

	/* The thread is switching to SCHED_SAAKM class,
	 * we must:
	 * - initialize its sched_saakm_entity
	 * - call its policy's new_prepare() routine to
	 *   initialize the per-policy metadata, but we ignore
	 *   the return value to avoid inconsistency, since it's
	 *   too late to choose a runqueue.
	 */
	if (flags & SWITCHING_CLASS) {
		saakm_task_state(p) = SAAKM_NOT_QUEUED;
		saakm_task_rq(p) = NULL;

		saakm_new_prepare(&e);
	}

	/*
	 * p->cpu has been set up by saakm_new_prepare(), we now need to do
	 * the actual enqueueing by calling saakm_new_place(), thus going
	 * from SAAKM_NOT_QUEUED to SAAKM_READY.
	 */
	if (saakm_task_state(p) == SAAKM_NOT_QUEUED) {
		/*
		 * If p->nr_cpus_allowed < 2, select_task_rq() is not called on
		 * fork(), and the new_prepare() handler is never called.
		 * We must therefore check if this has been done, and do it
		 * if necessary.
		 * FIXED: in core.c, we always call select_task_rq if in the
		 *        saakm sched class
		 */
		if (!policy_metadata(p))
			saakm_new_prepare(&e);

		/*
		 * If new_prepare() chose an IDLE cpu, we must call the
		 * exit_idle() handler to wake it up on the policy
		 */
		cstate = saakm_get_core_state(saakm_task_policy(p),
						rq->cpu);
		if (cstate == SAAKM_IDLE_CORE)
			saakm_exit_idle(saakm_task_policy(p),
					  rq->cpu);
		saakm_new_place(&e);
		goto end;
	}

	/*
	 * To unblock a task, a thread calls wake_up(), which calls
	 * try_to_wake_up(), which sets the task's state to TASK_WAKING and
	 * then calls enqueue_task(). Therefore, we're only in the presence of
	 * an true unblock() if the state is TASK_WAKING.
	 *
	 * We also check for the OUSTED flag and TASK_ON_RQ_MIGRATING to
	 * simulate a block/unblock pair when a thread is kicked out from its
	 * cpu. It will be placed on a cpu handled by the policy and authorized
	 * for this thread.
	 */
	if (READ_ONCE(p->__state) == TASK_WAKING ||
	    (flags & OUSTED && task_on_rq_migrating(p))) {
		/*
		 * If unblock_prepare() chose an IDLE cpu, we must call the
		 * exit_idle() handler to wake it up on the policy
		 */
		cstate = saakm_get_core_state(saakm_task_policy(p),
						rq->cpu);
		if (cstate == SAAKM_IDLE_CORE)
			saakm_exit_idle(saakm_task_policy(p),
					  rq->cpu);
		saakm_unblock_place(&e);
		goto end;
	}

	/*
	 * If flags is ENQUEUE_RESTORE, it means we are in a quick
	 * dequeue/enqueue, just update nr_running/count_ready
	 */
	if (flags & ENQUEUE_RESTORE)
		goto end;

	pr_warn("[WARN] Uncaught enqueue, CONTEXT: p=[pid=%d, cpu=%d, state=%u, on_cpu=%d, on_rq=%d, saakm=[current_state=%s]]; rq[%d]=%p; flags=%d\n",
		       p->pid, task_cpu(p), READ_ONCE(p->__state), p->on_cpu, p->on_rq,
		       saakm_state_to_str(saakm_task_state(p)),
		       rq->cpu, rq, flags);

end:
	add_nr_running(rq, 1);
	rq->nr_saakm_running++;
}

static void update_curr_saakm(struct rq *rq)
{	s64 delta_exec;

	if (unlikely(saakm_sched_class_log))
		pr_info("In %s [rq=%d]\n", __func__, rq->cpu);

	/*
	 * We now update statistics. Needed to get %CPU working for SaaKM
	 * processes in top, for instance.
	 */
	delta_exec = update_curr_common(rq);
	if (unlikely((s64)delta_exec <= 0))
		return;

}

static bool dequeue_task_saakm(struct rq *rq,
				 struct task_struct *p,
				 int flags)
{
	unsigned int state;
	struct process_event e = { .target = p, .cpu = smp_processor_id(), .flags = flags};


	if (unlikely(saakm_sched_class_log))
		pr_info("In %s [pid=%d, rq=%d, flags=%d]\n",
			__func__, p->pid, rq->cpu, flags);

	update_curr_saakm(rq);

	/* task has no saakm policy, just decrement rq->nr_running */
	if (!saakm_task_policy(p)) {
		pr_warn("[WARN] %s: called on a task with no saakm policy set.\n",
			__func__);
		goto end;
	}

	/*
	 * We are changing attributes of a thread. We don't need to do anything,
	 * just update rq->nr_running/count_ready
	 */
	if (flags & ATTR_CHANGE)
		goto end;

	/*
	 * The task is being dequeued because it's switching to another
	 * scheduling class. In this case too, we should call terminate. We must
	 * also set the saakm policy to NULL to avoid problems if the task
	 * switches back to SCHED_SAAKM class.
	 */
	if (flags & SWITCHING_CLASS) {
		saakm_terminate(&e);
		goto end;
	}

	/*
	 * We are in the middle of a migration. We don't need to do anything,
	 * just update rq->nr_running/count_ready
	 */
	if (task_on_rq_migrating(p) && !(flags & OUSTED))
		goto end;

	/*
	 * The thread doesn't even exist yet according to SaaKM. All we do
	 * is update nr_running/count_ready (at end)
	 */
	if (saakm_task_state(p) == SAAKM_NOT_QUEUED)
		goto end;

	/*
	 * In order to block, one sets the task to either TASK_INTERRUPTIBLE
	 * or TASK_UNINTERRUPTIBLE, and then calls schedule(), which calls
	 * deactivate_task(), which calls dequeue_task(). We are in this
	 * scenario: we're witnessing a true block().
	 *
	 * We also add TASK_STOPPED to make sure the task is removed from the
	 * runqueue when we receive a SIGSTOP signal.
	 *
	 * We add TASK_KILLABLE to make sure that all received signals are
	 * handled correctly.
	 * 
	 * We add TASK_WAKING because of the race between __schedule() and ttwu().
	 * __schedule() can call dequeue_task_saakm(p) while p is being woken
	 * up by another task in ttwu(), thus leading to p->__state = TAKS_WAKING.
	 * We must handle this case as a block/unblock pair.
	 *
	 * We also check for the OUSTED flag and TASK_ON_RQ_MIGRATING to
	 * simulate a block/unblock pair when a thread is kicked out from its
	 * cpu. It will be placed on a cpu handled by the policy and authorized
	 * for this thread.
	 */
	state = READ_ONCE(p->__state);
	if (state & TASK_INTERRUPTIBLE ||
	    state & TASK_UNINTERRUPTIBLE ||
	    state & TASK_STOPPED ||
	    state & TASK_KILLABLE || state & TASK_WAKING ||
	    (flags & OUSTED && task_on_rq_migrating(p))) {
		saakm_block(&e);
		goto end;
	}

	/*
	 * The task is being dequeued because it's dead. This is where we should
	 * call terminate(), not in task_dead_saakm(), because
	 * task_dead_saakm() is called after the task is dequeued and
	 * schedule() is called. Consequently, if we don't remove the task from
	 * the rbtree now, it will be scheduled again while it is not queued,
	 * which will lead to a crash.
	 */
	if (p->flags & PF_EXITING) {
		saakm_terminate(&e);
		goto end;
	}

	/*
	 * If flags is DEQUEUE_SAVE, it means we are in a quick
	 * dequeue/enqueue, just update nr_running/count_ready
	 */
	if (flags & DEQUEUE_SAVE)
		goto end;

	pr_warn("[WARN] Uncaught dequeue, CONTEXT: p=[pid=%d, cpu=%d, state=%u, on_cpu=%d, on_rq=%d, saakm=[current_state=%s]]; rq[%d]=%p; flags=%d\n",
		p->pid, task_cpu(p), READ_ONCE(p->__state), p->on_cpu, p->on_rq,
		saakm_state_to_str(saakm_task_state(p)),
		rq->cpu, rq, flags);

end:
	sub_nr_running(rq, 1);
	rq->nr_saakm_running--;
	return true;
}

static void yield_task_saakm(struct rq *rq)
{
	struct process_event e = { .target = rq->curr,
				   .cpu = smp_processor_id() };
	struct task_struct *p = rq->curr;

	if (unlikely(saakm_sched_class_log))
		pr_info("In %s [rq=%d]\n",
			__func__, rq->cpu);

	/*
	 * The process called yield(). Switch its state to SAAKM_READY,
	 	* schedule() is going to be called very soon.
	 */
	saakm_yield(&e);
	p->saakm.just_yielded = 1;
}

static bool yield_to_task_saakm(struct rq *rq,
				  struct task_struct *p)
{
	if (unlikely(saakm_sched_class_log))
		pr_info("In %s [pid=%d, rq=%d]\n",
			__func__, p->pid, rq->cpu);

	return 0;
}

static void check_preempt_wakeup(struct rq *rq,
				 struct task_struct *p,
				 int wake_flags)
{
	if (unlikely(saakm_sched_class_log))
		pr_info("In %s [pid=%d, rq=%d]\n",
			__func__, p->pid, rq->cpu);
}

static struct task_struct *pick_task_saakm(struct rq *rq)
{
	struct task_struct *next = NULL;
	unsigned long flags;
	struct saakm_policy *policy = NULL;

	read_lock_irqsave(&saakm_rwlock, flags);

	list_for_each_entry(policy, &saakm_policies, list) {
		saakm_schedule(policy, rq->cpu);

		next = per_cpu(saakm_current, rq->cpu);
		if (next)
			break;
	}

	read_unlock_irqrestore(&saakm_rwlock, flags);

	return next;
}

struct task_struct *_pick_next_task_saakm(struct rq *rq, 
						struct task_struct *prev,
						struct rq_flags *rf)
{
	struct task_struct *next;
	struct saakm_policy *policy;
	unsigned long flags;
	enum saakm_core_state cstate;

	/*
	 * If saakm_current is not NULL, it means that pick_next_task() is
	 * called and neither yield(), block() or terminate() was called. This
	 * can happen in __schedule(), if the task is not RUNNABLE
	 * (prev->state != 0) and has a pending signal. The task is therefore
	 * not dequeued in order to handle the pending signals, and still in
	 * saakm_current. For now, we keep the same task as saakm_current,
	 * it will be removed when signals are handled (through a call to
	 * dequeue and the correct saakm event handler).
	 * This might also happen if __schedule() is called with preempt set to
	 * true. This can happen with some syscalls. In this case, we want to
	 * force a preemption, so we're going to simulate a yield().
	 */
	next = per_cpu(saakm_current, rq->cpu);
	if (next) {
		if (READ_ONCE(next->__state) != TASK_RUNNING) {
			/* current has signals pending, leave it running */
			goto end;
		} else {
			/* yield to force preemption */
			struct process_event e = { .target = next };

			saakm_yield(&e);
		}
	}

	next = NULL;

	/* First, we try to pick a task without doing load balancing or anything */
	next = pick_task_saakm(rq);

	/* There is no available tasks, go to idle handling */
	if (!next)
		goto idle;
	else
		goto end;

idle:
	if (!rf)
		return next;

	/* We're idle and we got lock on rq(), do the idle balancing if needed FIX: locks*/
	read_lock_irqsave(&saakm_rwlock, flags);
	list_for_each_entry(policy, &saakm_policies, list) {
		
		/*
		 * Policy has no ready task on this cpu. If cpu is
		 * already idle, try next policy. Else, call the
		 * newly_idle() event and retry once.
		 */
		cstate = saakm_get_core_state(policy, rq->cpu);
		if (cstate == SAAKM_IDLE_CORE)
			continue;

		saakm_newly_idle(policy, rq->cpu, rf);

		saakm_schedule(policy, rq->cpu);
		next = per_cpu(saakm_current, rq->cpu);
		/* if a task is found, schedule it */
		if (next)
			break;
		/* else call enter_idle() handler for this policy/cpu */
		saakm_enter_idle(policy, rq->cpu);
	}
	read_unlock_irqrestore(&saakm_rwlock, flags);
end:
	if (next && next != prev)
		put_prev_set_next_task(rq, prev, next);

	return next;
}

static struct task_struct *__pick_next_task_saakm(struct rq *rq, struct task_struct *prev)
{
	return _pick_next_task_saakm(rq, prev, NULL);
}

static void put_prev_task_saakm(struct rq *rq,
				  struct task_struct *prev,
				  struct task_struct *next)
{
	enum saakm_state state;
	struct process_event e = { .target = prev, .cpu = smp_processor_id() };

	if (unlikely(saakm_sched_class_log))
		pr_info("In %s [pid=%d, rq=%d]\n",
			__func__, prev->pid, rq->cpu);

	/* Safety checks. Use BUG() to fail gracelessly. */
	if (!prev || prev->sched_class != &saakm_sched_class) {
		pr_err("[ERR] %s: At least one precondition not verified [%d %d]\n",
			       __func__, !prev,
			       prev->sched_class != &saakm_sched_class);
		BUG();
	}

	/*
	 * If no policy is set, we are moving out from an saakm policy,
	 * dequeue_task_saakm() already called terminate(). We just remove
	 * prev from saakm_current if necessary. We don't call resched_curr()
	 * because the task will keep the cpu in its new sched_class.
	 */
	if (!prev->saakm.policy) {
		if (per_cpu(saakm_current, task_cpu(prev)) == prev)
			per_cpu(saakm_current, task_cpu(prev)) = NULL;
		return;
	}

	update_curr_saakm(rq);

	state = saakm_task_state(prev);
	switch (state) {
	case SAAKM_RUNNING:
		/*
		 * Case 1: the thread is being preempted. If it's just one of
		 * these quick put_prev_task()/set_next_task() things. Do
		 * nothing. Else, call a yield event (we should have a preempt
		 * event, but since we do not, we just call yield.
		 *
		 * Note: nopreempt is a flag we added.
		 */
		if (!prev->saakm.nopreempt)
			saakm_yield(&e);
		break;
	case SAAKM_READY_TICK:
		/*
		 * Case 2: preemption caused by a transition to READY in tick().
		 * We're just before the preemption of a thread that has just
		 * been moved to the READY queue from tick(). The thread is
		 * still running from the runtime's point of view, but we
		 * already updated its SaaKM metadata, which is why it is
		 * not in the SAAKM_RUNNING state. We can simply change its
		 * state to SAAKM_READY, a context switch that puts the
		 * thread in the runqueue will soon happen.
		 */
		saakm_task_state(prev) = SAAKM_READY;
		break;
	case SAAKM_READY:
		/*
		 * Case 3: if we're already in the READY state, a yield()
		 * event from a call to sched_yield() set us in this state.
		 */
		prev->saakm.just_yielded = 0;
		break;
	case SAAKM_BLOCKED:
	case SAAKM_TERMINATED:
	case SAAKM_MIGRATING:
		/*
		 * Cases 4, 5, 6: Nothing to do.
		 */
		break;
	default:
		/*
		 * Case 7: we're in another state: shouldn't happen.
		 */
		pr_warn("[WARN] %s[pid=%d]: Invalid state %d.\n",
			__func__, prev->pid, state);
	}
}

#ifdef CONFIG_SMP

/* 
 * Balancing is called from prev_balance() right before
 * pick_next_task(). We currently hold rq->__lock.
 * Scan policices to see if we can find a task to run.
 * If prev was not from saakm, 
*/
static int balance_saakm(struct rq *rq, struct task_struct *prev,
			   struct rq_flags *rf)
{
	return saakm_enabled() ? 1 : 0;
}

static int select_task_rq_saakm(struct task_struct *p,
				  int prev_cpu,
				  int wake_flags)
{
	struct process_event e = { .target = p, .cpu = smp_processor_id(), .flags = wake_flags };
	int ret = task_cpu(p);

	if (unlikely(saakm_sched_class_log))
		pr_info("In %s [pid=%d]\n",
			__func__, p->pid);

	/* Safety checks. */
	if (!p || p->sched_class != &saakm_sched_class) {
		pr_warn("[WARN] %s: Preconditions not fulfilled [%d %d]\n",
			__func__, !p,
			p->sched_class != &saakm_sched_class);
		return task_cpu(p);
	}

	/*
	 * If state == SAAKM_NOT_QUEUED, p is a forked process that
	 * will soon be enqueued. We must call new_prepare() event.
	 */
	if (saakm_task_state(p) == SAAKM_NOT_QUEUED) {
		if (!saakm_task_policy(p))
			pr_err("[ERR] %s: p is SAAKM_NOT_QUEUED and policy is NULL. Shouldn't happen\n",
			       __func__);
		saakm_task_rq(p) = NULL;
		ret = saakm_new_prepare(&e);
		if (ret < 0) {
			pr_warn("[WARN] %s: new_prepare failed (pid=%d, policy=%llu), reverting to p->cpu\n",
				__func__, p->pid,
				saakm_task_policy(p)->id);
			ret = task_cpu(p);
		}
	} else if (READ_ONCE(p->__state) == TASK_WAKING) {
		ret = saakm_unblock_prepare(&e);
		/* if migrating on wakeup, remove from previous cpu */
		if (ret >= 0 && ret != task_cpu(p)) {
			struct rq_flags rf;

			rq_lock(task_rq(p), &rf);
			change_rq(p, SAAKM_BLOCKED, NULL);
			rq_unlock(task_rq(p), &rf);
		}
	}

	return ret;
}

static void migrate_task_rq_saakm(struct task_struct *p, int new_cpu)
{
	if (unlikely(saakm_sched_class_log))
		pr_info("In %s, [pid=%d, new_cpu=%d]\n",
			__func__, p->pid, new_cpu);
}

static void rq_online_saakm(struct rq *rq)
{
	struct saakm_policy *policy = NULL;

	if (unlikely(saakm_sched_class_log))
		pr_info("In %s [rq=%d]\n",
			__func__, rq->cpu);

	list_for_each_entry(policy, &saakm_policies, list)
		saakm_core_entry(policy, rq->cpu);
}

static void rq_offline_saakm(struct rq *rq)
{
	struct saakm_policy *policy = NULL;

	if (unlikely(saakm_sched_class_log))
		pr_info("In %s [rq=%d]\n",
			__func__, rq->cpu);

	list_for_each_entry(policy, &saakm_policies, list)
		saakm_core_exit(policy, rq->cpu);
}

static void task_woken_saakm(struct rq *this_rq, struct task_struct *p)
{
	if (unlikely(saakm_sched_class_log))
		pr_info("in %s [pid=%d, rq=%d]\n",
			__func__, p->pid, this_rq->cpu);
}

static void task_dead_saakm(struct task_struct *p)
{
	if (unlikely(saakm_sched_class_log))
		pr_info("In %s [pid=%d]\n",
			__func__, p->pid);

	if (!p || p->sched_class != &saakm_sched_class)
		pr_err("[ERR] %s: exiting because it was called on an invalid process, a non-saakm process, or a process whose metadata was not initialized. [%p %d]",
		       __func__, p,
		       p->sched_class != &saakm_sched_class);

	saakm_task_policy(p) = NULL;
	saakm_task_state(p) = SAAKM_NOT_QUEUED;
}
#endif

static void set_next_task_saakm(struct rq *rq, struct task_struct *p, bool first)
{
	if (unlikely(saakm_sched_class_log))
		pr_info("In %s [rq=%d, pid=%d]\n",
			__func__, rq->cpu, p->pid);

	/* Check that rq->curr is also saakm_current and fix it.
	 * Happens when switching to SCHED_SAAKM: the task is dequeued
	 * from the previous scheduling class queue, then the previous class'
	 * put_prev_task() is called, then the task is enqueued with
	 * enqueue_task_saakm() which removes it from saakm_current and
	 * puts it in READY state.
	 */
	if (!first && per_cpu(saakm_current, rq->cpu) != rq->curr && (rq->curr->sched_class == &saakm_sched_class)) {
		change_state(rq->curr, SAAKM_RUNNING, rq->cpu, NULL);
	}
	/* Update statistics. */
	p->se.exec_start = rq_clock_task(rq);
}

static void task_tick_saakm(struct rq *rq,
			      struct task_struct *curr,
			      int queued)
{
	struct process_event e = { .target = curr, .cpu = smp_processor_id() };

	if (unlikely(saakm_sched_class_log))
		pr_info("In %s [pid=%d, rq=%d]\n",
			__func__, curr->pid, rq->cpu);

	update_curr_saakm(rq);

	/*
	 * In task_tick_saakm, it sometimes happens that rq and curr are on a
	 * different CPU, i.e., rq->cpu and task_cpu(curr) are different. Only
	 * rq's lock is held. This is a bit strange because all calls to
	 * task_tick() in core.c call it with rq and rq->curr. I suspect we are
	 * seeing this because no locks are taken when rq->curr is read, so
	 * it's possible the process was moved before the rq is read.
	 *
	 * What this means is that we may not hold the lock for curr's rq in
	 * tick().  IT DOESN'T MATTER HOWEVER, since we don't allow state
	 * changes in tick() (it seems reasonable).
	 *
	 * FIXME: not the case anymore. State transitions happen in tick().
	 */
	if (rq->cpu != task_cpu(curr)) {
		pr_warn("%s: rq->cpu=%d task_cpu(curr)=%d\n",
			__func__, rq->cpu, task_cpu(curr));
	}

	saakm_tick(&e);
}

static void task_fork_saakm(struct task_struct *p)
{
	if (unlikely(saakm_sched_class_log))
		pr_info("In %s [pid=%d]\n",
			__func__, p->pid);

	saakm_task_state(p) = SAAKM_NOT_QUEUED;
	saakm_task_rq(p) = NULL;
	p->saakm.node_runqueue.__rb_parent_color = 0;
	p->saakm.node_runqueue.rb_right = NULL;
	p->saakm.node_runqueue.rb_left = NULL;
	policy_metadata(p) = NULL;
}

static void prio_changed_saakm(struct rq *rq,
				 struct task_struct *p,
				 int oldprio)
{
	if (unlikely(saakm_sched_class_log))
		pr_info("In %s [pid=%d, rq=%d]\n",
			__func__, p->pid, rq->cpu);
}

static void switched_from_saakm(struct rq *rq, struct task_struct *p)
{
	if (unlikely(saakm_sched_class_log))
		pr_info("In %s [pid=%d, rq=%d]\n",
			__func__, p->pid, rq->cpu);

	/* Task is leaving saakm, let's cleanup everything */
	saakm_task_state(p) = SAAKM_NOT_QUEUED;
	saakm_task_rq(p) = NULL;
	p->saakm.node_runqueue.__rb_parent_color = 0;
	p->saakm.node_runqueue.rb_right = NULL;
	p->saakm.node_runqueue.rb_left = NULL;
	policy_metadata(p) = NULL;
}

static void switched_to_saakm(struct rq *rq, struct task_struct *p)
{
	if (unlikely(saakm_sched_class_log))
		pr_info("In %s [pid=%d, rq=%d]\n",
			__func__, p->pid, rq->cpu);

	if (rq->curr != p) {
		/*
		 * We can safely call resched_curr() here, because the rq lock
		 * is held.
		 */
		lockdep_assert_held(&rq->__lock);
		resched_curr(rq);
	}
}

static void reweight_task_saakm(struct rq *rq, struct task_struct *p,
					const struct load_weight *lw)
{
	struct saakm_policy *policy = saakm_task_policy(p);

	if (unlikely(saakm_sched_class_log))
		pr_info("In %s [pid=%d, rq=%d]\n",
			__func__, p->pid, rq->cpu);
	if (!policy) {
		pr_warn("[WARN] %s: called on a task with no saakm policy set.\n",
			__func__);
		return;
	}

	if (policy->routines->reweight_task)
		policy->routines->reweight_task(policy, p, lw);

}

static unsigned int get_rr_interval_saakm(struct rq *rq,
					    struct task_struct *task)
{
	if (unlikely(saakm_sched_class_log))
		pr_info("In %s [pid=%d, rq=%d]\n",
			__func__, task->pid, rq->cpu);

	return (100 * HZ / 1000);
}

#ifdef CONFIG_FAIR_GROUP_SCHED
static void task_change_group_saakm(struct task_struct *p)
{
	if (unlikely(saakm_sched_class_log))
		pr_info("In %s [pid=%d]\n",
			__func__, p->pid);
}
#endif

static void run_rebalance_domains(void)
{
	saakm_balancing_select();
}

DEFINE_SCHED_CLASS(saakm) = {
	.enqueue_task		= enqueue_task_saakm,
	.dequeue_task		= dequeue_task_saakm,
	.yield_task		= yield_task_saakm,
	.yield_to_task		= yield_to_task_saakm,

	.wakeup_preempt		= check_preempt_wakeup,

	.pick_task		= pick_task_saakm,
	.pick_next_task		= __pick_next_task_saakm,
	.put_prev_task		= put_prev_task_saakm,
	.set_next_task	        = set_next_task_saakm,

#ifdef CONFIG_SMP
	.balance                = balance_saakm,
	.pick_task		= pick_task_saakm,
	.select_task_rq		= select_task_rq_saakm,
	.migrate_task_rq	= migrate_task_rq_saakm,

	.rq_online		= rq_online_saakm,
	.rq_offline		= rq_offline_saakm,

	.task_woken		= task_woken_saakm,
	.task_dead		= task_dead_saakm,
	.set_cpus_allowed	= set_cpus_allowed_common,
#endif


	.task_tick	        = task_tick_saakm,
	.task_fork	        = task_fork_saakm,

	.prio_changed	        = prio_changed_saakm,
	.switched_from	        = switched_from_saakm,
	.switched_to		= switched_to_saakm,
	.reweight_task		= reweight_task_saakm,
	.get_rr_interval	= get_rr_interval_saakm,

	.update_curr		= update_curr_saakm,

#ifdef CONFIG_FAIR_GROUP_SCHED
	.task_change_group	= task_change_group_saakm,
#endif
};

void sched_balance_trigger_saakm(struct rq *rq)
{
	raise_softirq(SCHED_SOFTIRQ_SAAKM);
}

DEFINE_PER_CPU(struct topology_level *, topology_levels);
EXPORT_SYMBOL(topology_levels);

static int create_topology(void)
{
	int cpu;
	struct sched_domain *sd;
	struct topology_level *l, *cur;

	/*
	 * for each CPU, we export the topology to the saakm policies through
	 * the topology_levels per-cpu variable.
	 * To build this, we use the already built sched_domains.
	 */
	rcu_read_lock();
	for_each_possible_cpu(cpu) {
		per_cpu(topology_levels, cpu) = NULL;
		cur = NULL;
		for_each_domain(cpu, sd) {
			l = kzalloc(sizeof(struct topology_level), GFP_KERNEL);
			if (!l)
				return -ENOMEM;
			if (sd->flags & SD_SHARE_CPUCAPACITY)
				l->flags |= DOMAIN_SMT;
			if (sd->flags & SD_CLUSTER)
				l->flags |= DOMAIN_CLUSTER;
			if (sd->flags & SD_SHARE_LLC)
				l->flags |= DOMAIN_CACHE;
			if (sd->flags & SD_NUMA)
				l->flags |= DOMAIN_NUMA;
			cpumask_copy(&l->cores, sched_domain_span(sd));

			/* insert level in per_cpu list (at tail) */
			l->next = NULL;
			if (!per_cpu(topology_levels, cpu))
				per_cpu(topology_levels, cpu) = l;
			else
				cur->next = l;
			cur = l;
		}
	}
	rcu_read_unlock();

	return 0;
}

#ifdef CONFIG_SAAKM_DEBUG_TOPOLOGY
static void print_topology(void)
{
	int cpu;
	struct topology_level *l;

	pr_info("+-----------------------+\n");
	pr_info("|    saakm topology   |\n");
	pr_info("+-----------------------+\n");
	pr_info("  cpu  | SMT | CLUSTER | CACHE | NUMA |   cpulist\n");
	for_each_possible_cpu(cpu) {
		pr_info("-------+-----+-------+-------+------+--------------\n");
		pr_info(" %5d |\n", cpu);
		l = per_cpu(topology_levels, cpu);
		while (l) {
			pr_info("       |  %d  |   %d   |   %d   |   %d  | %*pbl\n",
				l->flags & DOMAIN_SMT ? 1 : 0,
				l->flags & DOMAIN_CLUSTER ? 1 : 0,
				l->flags & DOMAIN_CACHE ? 1 : 0,
				l->flags & DOMAIN_NUMA ? 1 : 0,
				cpumask_pr_args(&l->cores));
			l = l->next;
		}
	}
}
#endif	/* CONFIG_SAAKM_DEBUG_TOPOLOGY */

#ifdef CONFIG_CGROUP_SAAKM
static struct cgroup_subsys_state *
saakm_cgroup_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct saakm_group *ipa_grp;

	ipa_grp = kzalloc(sizeof(struct saakm_group), GFP_KERNEL);
	if (!ipa_grp)
		return ERR_PTR(-ENOMEM);

	return &ipa_grp->css;
}

static void saakm_cgrp_attach(struct cgroup_taskset *tset)
{
	struct task_struct *t = NULL;
	struct cgroup_subsys_state *css = NULL;
	struct sched_attr attr = { .size = sizeof(struct sched_attr),
				   .sched_flags = 0,
				   .sched_nice = 0,
				   .sched_priority = 0
	};

	/* Move each task to the saakm policy */
	cgroup_taskset_for_each(t, css, tset) {
		struct saakm_group *ipa_grp = saakm_group_of(css);

		if (ipa_grp->policy) {
			/* move thread to new saakm policy */
			attr.sched_policy = SCHED_SAAKM;
			attr.sched_ipa_policy = ipa_grp->policy->id;
			attr.sched_ipa_attr_size = 0;
			attr.sched_ipa_attr = NULL;
		} else {
			/* move thread to fair */
			attr.sched_policy = SCHED_NORMAL;
		}
		if (sched_setattr_nocheck(t, &attr)) {
			if (ipa_grp->policy)
				pr_err("task %d could not be moved to saakm policy %llu!\n",
				       t->pid, ipa_grp->policy->id);
			else
				pr_err("task %d could not be moved to fair!\n",
				       t->pid);
		}
	}
}

static void saakm_cgroup_css_free(struct cgroup_subsys_state *css)
{
	struct saakm_group *ipa_grp;

	ipa_grp = saakm_group_of(css);
	kfree(ipa_grp);
}

static s64 saakm_policy_id_read_s64(struct cgroup_subsys_state *css,
				      struct cftype *cft)
{
	struct saakm_group *ipa_grp = saakm_group_of(css);

	if (!ipa_grp->policy)
		return -1;
	return ipa_grp->policy->id;
}

static int saakm_policy_id_write_s64(struct cgroup_subsys_state *css,
				       struct cftype *cft, s64 val)
{
	struct saakm_group *ipa_grp = saakm_group_of(css);
	struct css_task_iter it;
	struct task_struct *t;
	struct saakm_policy *policy, *old_policy = ipa_grp->policy;
	bool found = false;
	unsigned long flags;
	int ret = 0;
	struct sched_attr attr = { .size = sizeof(struct sched_attr),
				   .sched_flags = 0,
				   .sched_nice = 0,
				   .sched_priority = 0
	};

	if (!css->parent)
		return -EPERM;

	/* check boundaries (i.e. policy id is a s64, and -1 is ok here */
	if (val < -1 || val > S64_MAX)
		return -EINVAL;
	if (val != -1) {
		/* if no change, nothing to do */
		if (ipa_grp->policy && val == ipa_grp->policy->id)
			return 0;

		read_lock_irqsave(&saakm_rwlock, flags);
		list_for_each_entry(policy, &saakm_policies, list) {
			if (policy->id == val) {
				found = true;
				break;
			}
		}
		if (found) {
			if (!try_module_get(policy->kmodule))
				ret = -EINVAL;
		} else
			ret = -EINVAL;
		read_unlock_irqrestore(&saakm_rwlock, flags);
		if (ret)
			return ret;

		ipa_grp->policy = policy;
		attr.sched_policy = SCHED_SAAKM;
		attr.sched_ipa_policy = val;
		attr.sched_ipa_attr_size = 0;
		attr.sched_ipa_attr = NULL;
	} else {
		ipa_grp->policy = NULL;
		attr.sched_policy = SCHED_NORMAL;
	}

	if (old_policy)
		module_put(old_policy->kmodule);

	/* Move all tasks in css to their new policy */
	css_task_iter_start(css, 0, &it);
	while ((t = css_task_iter_next(&it))) {
		if (sched_setattr_nocheck(t, &attr)) {
			if (val == -1)
				pr_err("task %d could not be moved to fair!\n",
				       t->pid);
			else
				pr_err("task %d could not be moved to saakm policy %lld!\n",
				       t->pid, val);
		}
	}
	css_task_iter_end(&it);

	return 0;
}

static struct cftype saakm_cgrp_files[] = {
	{
		.name      = "policy_id",
		.read_s64  = saakm_policy_id_read_s64,
		.write_s64 = saakm_policy_id_write_s64,
	},
	{ }    /* terminate */
};

struct cgroup_subsys saakm_cgrp_subsys = {
	.css_alloc      = saakm_cgroup_css_alloc,
	.css_free       = saakm_cgroup_css_free,
	.attach         = saakm_cgrp_attach,
	.legacy_cftypes	= saakm_cgrp_files,
	.dfl_cftypes	= saakm_cgrp_files,
};
#endif	/* CONFIG_CGROUP_SAAKM */

/*
 * Rbtree manipulation
 */
static inline int saakm_add_task_rbtree(struct rb_root *root,
					  struct task_struct *data,
					  int (*cmp_fn)(struct task_struct *,
							struct task_struct *))
{
	struct rb_node **new = &(root->rb_node), *parent = NULL;

	while (*new) {
		struct task_struct *t = container_of(*new, struct task_struct,
						     saakm.node_runqueue);
		int res = cmp_fn(data, t);

		parent = *new;

		/*
		 * We compare with the provided function, but if both threads
		 * are equal, we use the task_struct's address to differenciate.
		 * If the node is already in the rbtree, we stop here.
		 */
		if (res < 0)
			new = &((*new)->rb_left);
		else if (res > 0)
			new = &((*new)->rb_right);
		else if (data < t)
			new = &((*new)->rb_left);
		else if (data > t)
			new = &((*new)->rb_right);
		else
			return -EINVAL;
	}

	rb_link_node(&data->saakm.node_runqueue, parent, new);
	rb_insert_color(&data->saakm.node_runqueue, root);

	return 0;
}

static inline struct task_struct *
saakm_remove_task_rbtree(struct rb_root *root, struct task_struct *data)
{
	rb_erase(&data->saakm.node_runqueue, root);
	memset(&data->saakm.node_runqueue, 0,
	       sizeof(data->saakm.node_runqueue));
	return data;
}

static inline struct task_struct *
saakm_first_task_rbtree(struct rb_root *root)
{
	struct rb_node *first;

	first = rb_first(root);
	if (!first)
		return NULL;

	return container_of(first, struct task_struct, saakm.node_runqueue);
}

/*
 * LIST manipulation
 */
static inline int saakm_add_task_list(struct list_head *head,
					struct task_struct *data,
					int (*cmp_fn)(struct task_struct *,
						      struct task_struct *))
{
	struct task_struct *ts;

	list_for_each_entry(ts, head, saakm.node_list) {
		if (cmp_fn(data, ts) < 0) {
			list_add_tail(&data->saakm.node_list,
				      &ts->saakm.node_list);
			return 0;
		}
	}
	list_add_tail(&data->saakm.node_list, head);
	return 0;
}

static inline struct task_struct *
saakm_remove_task_list(struct list_head *head, struct task_struct *data)
{
	list_del_init(&data->saakm.node_list);

	return data;
}

static inline struct task_struct *
saakm_first_task_list(struct list_head *head)
{
	return list_first_entry_or_null(head, struct task_struct,
					saakm.node_list);
}

/*
 * FIFO manipulation
 */
static inline int saakm_add_task_fifo(struct list_head *head,
					struct task_struct *data,
					int (*cmp_fn)(struct task_struct *,
						      struct task_struct *))
{
	list_add_tail(&data->saakm.node_list, head);
	return 0;
}

/*
 * Generic saakm_rq API
 */
int saakm_add_task(struct saakm_rq *rq, struct task_struct *data)
{
	switch (rq->type) {
	case RBTREE:
		return saakm_add_task_rbtree(&rq->root, data, rq->order_fn);
	case LIST:
		return saakm_add_task_list(&rq->head, data, rq->order_fn);
	case FIFO:
		return saakm_add_task_fifo(&rq->head, data, rq->order_fn);
	default:
		return -EINVAL;
	}
}

struct task_struct *saakm_remove_task(struct saakm_rq *rq,
					struct task_struct *data)
{
	switch (rq->type) {
	case RBTREE:
		return saakm_remove_task_rbtree(&rq->root, data);
	case LIST:
	case FIFO:
		return saakm_remove_task_list(&rq->head, data);
	default:
		return NULL;
	}
}

struct task_struct *saakm_first_task(struct saakm_rq *rq)
{
	switch (rq->type) {
	case RBTREE:
		return saakm_first_task_rbtree(&rq->root);
	case LIST:
	case FIFO:
		return saakm_first_task_list(&rq->head);
	default:
		return NULL;
	}
}
EXPORT_SYMBOL(saakm_first_task);

void init_saakm_rq(struct saakm_rq *rq, enum saakm_rq_type type,
		     unsigned int cpu, enum saakm_state state,
		     int (*order_fn)(struct task_struct *a,
				     struct task_struct *b))
{
	rq->type = type;
	switch (type) {
	case RBTREE:
		rq->root.rb_node = NULL;
		break;
	case LIST:
	case FIFO:
		INIT_LIST_HEAD(&rq->head);
		break;
	}
	rq->cpu = cpu;
	rq->state = state;
	rq->nr_tasks = 0;
	rq->order_fn = order_fn;
}
EXPORT_SYMBOL(init_saakm_rq);

/*
 * procfs interface: located in /proc/saakm
 */
static void *saakm_policies_start(struct seq_file *f, loff_t *pos)
{
	read_lock(&saakm_rwlock);
	return seq_list_start(&saakm_policies, *pos);
}

static void *saakm_policies_next(struct seq_file *f, void *v, loff_t *pos)
{
	return seq_list_next(v, &saakm_policies, pos);
}

static void saakm_policies_stop(struct seq_file *f, void *v)
{
	read_unlock(&saakm_rwlock);
}

static int saakm_policies_show(struct seq_file *f, void *v)
{
	struct saakm_policy *policy = list_entry(v, struct saakm_policy,
						   list);
	seq_printf(f, "%llu %s %d\n",
		   policy->id, policy->name, module_refcount(policy->kmodule));
	return 0;
}

static const struct seq_operations saakm_policies_ops = {
	.start = saakm_policies_start,
	.next  = saakm_policies_next,
	.show  = saakm_policies_show,
	.stop  = saakm_policies_stop
};

static int saakm_policies_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &saakm_policies_ops);
}

static const struct proc_ops saakm_policies_fops = {
	.proc_open    = saakm_policies_open,
	.proc_read    = seq_read,
	.proc_lseek  = seq_lseek,
	.proc_release = seq_release,
};

struct proc_dir_entry *ipa_procdir;
EXPORT_SYMBOL(ipa_procdir);

/*
 * sysfs interface: located at /sys/kernel/saakm/
 */

#define SAAKM_ATTR_RO(_name) \
static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

#define SAAKM_ATTR_RW(_name) \
static struct kobj_attribute _name##_attr = \
	__ATTR(_name, 0644, _name##_show, _name##_store)


/*
 * Check that transitions do not violate the SaaKM finite state machine
 * Prints errors in dmesg if it does
 */
int saakm_fsm_check;
static ssize_t saakm_fsm_check_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", READ_ONCE(saakm_fsm_check));
}
static ssize_t saakm_fsm_check_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	if (kstrtoint(buf, 0, &saakm_fsm_check))
		return -EINVAL;

	return count;
}
SAAKM_ATTR_RW(saakm_fsm_check);

/*
 * Log all transitions in the SaaKM finite state machine
 */
int saakm_fsm_log;
static ssize_t saakm_fsm_log_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", READ_ONCE(saakm_fsm_log));
}
static ssize_t saakm_fsm_log_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	if (kstrtoint(buf, 0, &saakm_fsm_log))
		return -EINVAL;

	return count;
}
SAAKM_ATTR_RW(saakm_fsm_log);

/*
 * Log calls to the saakm scheduling class functions
 */
int saakm_sched_class_log;
static ssize_t saakm_sched_class_log_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *buf)
{
	return sprintf(buf, "%d\n", READ_ONCE(saakm_sched_class_log));
}
static ssize_t saakm_sched_class_log_store(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     const char *buf, size_t count)
{
	if (kstrtoint(buf, 0, &saakm_sched_class_log))
		return -EINVAL;

	return count;
}
SAAKM_ATTR_RW(saakm_sched_class_log);

struct kobject *saakm_kobj;

static struct attribute *saakm_attrs[] = {
	&saakm_fsm_check_attr.attr,
	&saakm_fsm_log_attr.attr,
	&saakm_sched_class_log_attr.attr,
	NULL
};

static struct attribute_group saakm_attr_group = {
	.attrs = saakm_attrs,
};

__init void init_sched_saakm_class(void)
{
	rwlock_init(&saakm_rwlock);
	open_softirq(SCHED_SOFTIRQ_SAAKM, run_rebalance_domains);

	pr_info("sched_class initialized\n");
}

static __init int init_sched_saakm_late(void)
{
	int ret;

	ret = create_topology();
	if (ret) {
		pr_err("create_topology() failed\n");
		goto exit;
	}

#ifdef CONFIG_SAAKM_DEBUG_TOPOLOGY
	print_topology();
#endif

	ipa_procdir = proc_mkdir("saakm", NULL);
	if (!ipa_procdir) {
		pr_err("procfs creation failed\n");
		ret = -ENOMEM;
		goto exit;
	}
	if (!proc_create("policies", 0444, ipa_procdir,
			 &saakm_policies_fops)) {
		pr_err("procfs creation failed\n");
		ret = -ENOMEM;
		goto exit;
	}
	pr_info("/proc/saakm/ directory created\n");

	/* Create /sys/kernel/saakm */
	saakm_kobj = kobject_create_and_add("saakm", kernel_kobj);
	if (!saakm_kobj) {
		ret = -ENOMEM;
		goto exit;
	}
	ret = sysfs_create_group(saakm_kobj, &saakm_attr_group);
	if (ret)
		goto exit;
	pr_info("/sys/kernel/saakm/ directory created\n");

	return 0;

exit:
	return ret;
}
late_initcall(init_sched_saakm_late);
