
#ifdef CONFIG_SCHED_TUNE

#ifdef CONFIG_CGROUP_SCHEDTUNE

int schedtune_cpu_boost(int cpu);
int schedtune_task_boost(struct task_struct *tsk);

int schedtune_prefer_idle(struct task_struct *tsk);

void schedtune_exit_task(struct task_struct *tsk);

void schedtune_enqueue_task(struct task_struct *p, int cpu);
void schedtune_dequeue_task(struct task_struct *p, int cpu);

#else /* CONFIG_CGROUP_SCHEDTUNE */

#define schedtune_cpu_boost(cpu)  get_sysctl_sched_cfs_boost()
#define schedtune_task_boost(tsk) get_sysctl_sched_cfs_boost()

#define schedtune_exit_task(task) do { } while (0)

#define schedtune_enqueue_task(task, cpu) do { } while (0)
#define schedtune_dequeue_task(task, cpu) do { } while (0)

#endif /* CONFIG_CGROUP_SCHEDTUNE */

#else /* CONFIG_SCHED_TUNE */

#define schedtune_cpu_boost(cpu)  0
#define schedtune_task_boost(tsk) 0

#define schedtune_exit_task(task) do { } while (0)

#define schedtune_enqueue_task(task, cpu) do { } while (0)
#define schedtune_dequeue_task(task, cpu) do { } while (0)

#endif /* CONFIG_SCHED_TUNE */
