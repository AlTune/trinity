/*
 * Each process that gets forked runs this code.
 */

#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/prctl.h>

#include "trinity.h"
#include "syscall.h"
#include "shm.h"

static struct rlimit oldrlimit;

static void disable_coredumps()
{
	struct rlimit limit;

	if (debug == TRUE) {
		(void)signal(SIGSEGV, SIG_DFL);
		return;
	}

	getrlimit(RLIMIT_CORE, &oldrlimit);

	limit.rlim_cur = 0;
	limit.rlim_max = oldrlimit.rlim_max;
	if (setrlimit(RLIMIT_CORE, &limit) != 0)
		perror( "setrlimit(RLIMIT_CORE)" );
}

static void reenable_coredumps()
{
	if (debug == TRUE)
		return;

	prctl(PR_SET_DUMPABLE, TRUE);

	if (setrlimit(RLIMIT_CORE, &oldrlimit) != 0) {
		printf("Error restoring rlimits to cur:%d max:%d (%s)\n",
			(unsigned int) oldrlimit.rlim_cur,
			(unsigned int) oldrlimit.rlim_max,
			strerror(errno));
	}
}
static void set_make_it_fail()
{
	static char failed = FALSE;
	int fd;
	const char *buf = "1";

	/* If we failed last time, don't bother trying in future. */
	if (failed == TRUE)
		return;

	fd = open("/proc/self/make-it-fail", O_WRONLY);
	if (fd == -1)
		return;

	if (write(fd, buf, 1) == -1) {
		if (errno != EPERM)
			printf("writing to /proc/self/make-it-fail failed! (%s)\n", strerror(errno));
		else
			failed = TRUE;
	}
	close(fd);
}

/*
 * We call this occasionally to set some FPU state, in the hopes that we
 * might tickle some weird FPU/scheduler related bugs
 */
static void use_fpu(void)
{
	double x = 0;
	asm volatile("":"+m" (x));
	x += 1;
	asm volatile("":"+m" (x));
}

void init_child(void)
{
	int i;

	i = find_pid_slot(getpid());
	shm->total_syscalls[i] = 0;

	set_make_it_fail();
	if (rand() % 100 < 50)
		use_fpu();
}

int child_process(void)
{
	cpu_set_t set;
	pid_t pid = getpid();
	int ret;
	unsigned int syscallnr;
	unsigned int cpu;
	unsigned int childno = find_pid_slot(pid);

	disable_coredumps();

	for (cpu = 0; cpu < shm->nr_childs; cpu++) {
		if (shm->pids[cpu] == pid)
			break;
	}

	if (sched_getaffinity(pid, sizeof(set), &set) == 0) {
		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		sched_setaffinity(getpid(), sizeof(set), &set);
		output("bound child %d to cpu %d\n", pid, cpu);
	}

	init_child();

	sigsetjmp(ret_jump, 1);

	ret = 0;

	while (shm->exit_now == FALSE) {

		while (shm->regenerating == TRUE)
			sleep(1);

		if (biarch == TRUE) {

			if ((use_64bit == TRUE) && (use_32bit == TRUE)) {
				/*
				 * 10% possibility of a 32bit syscall
				 */
				shm->do32bit = FALSE;
//				if (rand() % 100 < 10)
//					shm->do32bit = TRUE;
			}

			if (validate_syscall_table_32() == FALSE)
				use_32bit = FALSE;

			if (validate_syscall_table_64() == FALSE)
				use_64bit = FALSE;

			if (shm->do32bit == FALSE) {
				syscalls = syscalls_64bit;
				max_nr_syscalls = max_nr_64bit_syscalls;
			} else {
				syscalls = syscalls_32bit;
				max_nr_syscalls = max_nr_32bit_syscalls;
			}
		}

		if (count_enabled_syscalls() == 0) {
			output("[%d] No more syscalls enabled. Exiting\n", getpid());
			shm->exit_now = TRUE;
		}

retry:
		if (shm->exit_now == TRUE)
			goto out;

		syscallnr = rand() % max_nr_syscalls;

		if (syscalls[syscallnr].entry->num_args == 0)
			goto retry;

		if (!(syscalls[syscallnr].entry->flags & ACTIVE))
			goto retry;

		if (syscalls[syscallnr].entry->flags & AVOID_SYSCALL)
			goto retry;

		if (syscalls[syscallnr].entry->flags & NI_SYSCALL)
			goto retry;

		/* if we get here, syscallnr is finally valid */

		shm->syscallno[childno] = syscallnr;

		if (syscallcount) {
			if (shm->execcount >= syscallcount) {
				output("[%d] shm->execcount (%d) >= syscallcount (%d)\n", getpid(), shm->execcount,syscallcount);
				shm->exit_now = TRUE;
			}

			if (shm->execcount == syscallcount)
				printf("[%d] Reached maximum syscall count %ld\n", pid, shm->execcount);
		}

		ret = mkcall(childno);
	}


out:
	reenable_coredumps();

	return ret;
}
