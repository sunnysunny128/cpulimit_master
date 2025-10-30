/**
 *
 * cpulimit - a CPU limiter for Linux
 *
 * Copyright (C) 2005-2012, by:  Angelo Marletta <angelo dot marletta at gmail dot com> 
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 **************************************************************
 *
 * This is a simple program to limit the cpu usage of a process
 * If you modify this code, send me a copy please
 *
 * Get the latest version at: http://github.com/opsengine/cpulimit
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
//#include <sys/sysctl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#ifndef QNX
#include <sys/prctl.h>
#endif

#include "process_group.h"
#include "list.h"
#include "print_debug.h"
#include "idps_engine_health.h"
#include "device_register.h"
#include "safese.h"

//some useful macro
#ifndef MIN
#define MIN(a,b) (((a)<(b))?(a):(b))
#endif
#ifndef MAX
#define MAX(a,b) (((a)>(b))?(a):(b))
#endif

//control time slot in microseconds
//each slot is splitted in a working slice and a sleeping slice
//TODO: make it adaptive, based on the actual system load
#ifndef TIME_SLOT
#define TIME_SLOT 100000
#endif

// #define MAX_PRIORITY -10

/* GLOBAL VARIABLES */

//the "family"
static struct process_group pgroup[6] = {0};
extern idps_health idps_health_list[];
// //pid of cpulimit
// pid_t cpulimit_pid;
// //name of this program (maybe cpulimit...)
// char *program_name;

// //number of cpu
// int NCPU;

// /* CONFIGURATION VARIABLES */

// //verbose mode
int verbose = 0;
// //lazy mode (exits if there is no process)
// int lazy = 0;



//return t1-t2 in microseconds (no overflow checks, so better watch out!)
static inline unsigned long timediff(const struct timeval *t1,const struct timeval *t2)
{
	return (t1->tv_sec - t2->tv_sec) * 1000000 + (t1->tv_usec - t2->tv_usec);
}

static void limit_process(double limit, int include_children, struct process_group *pgroup, int engine_index)
{
	//slice of the slot in which the process is allowed to run
	struct timespec twork;
	//slice of the slot in which the process is stopped
	struct timespec tsleep;
	//when the last twork has started
	struct timeval startwork;
	//when the last twork has finished
	struct timeval endwork;
	//initialization
	memset(&twork, 0, sizeof(struct timespec));
	memset(&tsleep, 0, sizeof(struct timespec));
	memset(&startwork, 0, sizeof(struct timeval));
	memset(&endwork, 0, sizeof(struct timeval));	
	//last working time in microseconds
	unsigned long workingtime = 0;
	//generic list item
	struct list_node *node;
	//counter
	int c = 0;
	pid_t pid= idps_health_list[engine_index].pid;

	//rate at which we are keeping active the processes (range 0-1)
	//1 means that the process are using all the twork slice
	double workingrate = -1;
	int count = 100;
	int first_flag = 1;

	//init_process_group(pgroup, pid, include_children);
	while(1) {
		while(first_flag)		//第一次进入需要获取到pid
		{
			//print_debug(DEBUG_INFO, "try to reload sc pid:%d\n", idps_health_list[1].pid);
			pid = idps_health_list[engine_index].pid;
			if(pid == 0)
			{
				sleep(1);
				continue;
			}else
			{
				first_flag = 0;
				break;
			}
		}
		
		count++;
		if(count % 100 == 0)		//周期更新，防止sc pid重启后未更新
		{
			IDPS_IDLE();
			print_debug(DEBUG_INFO, "try to reload sc pid:%d\n", idps_health_list[engine_index].pid);
			pid = idps_health_list[engine_index].pid;
			if(pid == 0)
			{
				first_flag = 1;
				continue;
			}
		}

		init_process_group(pgroup, pid, include_children);
		// printf("Members in the process group owned by %d: %d\n", pgroup->target_pid, pgroup->proclist->count);

		//update_process_group(pgroup);

		if (pgroup->proclist->count==0) {
			// if (verbose) printf("No more processes.\n");
			usleep(100000);
			close_process_group(pgroup);
			continue;//break;
		}

		//total cpu actual usage (range 0-1)
		//1 means that the processes are using 100% cpu
		double pcpu = -1;

		//estimate how much the controlled processes are using the cpu in the working interval
		for (node = pgroup->proclist->first; node != NULL; node = node->next) {
			struct process *proc = (struct process*)(node->data);
			if (proc->cpu_usage < 0) {
				continue;
			}
			if (pcpu < 0) pcpu = 0;
			pcpu += proc->cpu_usage;
		}

		//adjust work and sleep time slices
		if (pcpu < 0) {
			//it's the 1st cycle, initialize workingrate
			pcpu = limit;
			workingrate = limit;
			twork.tv_nsec = TIME_SLOT * limit * 1000;
		}
		else {
			//adjust workingrate
			workingrate = MIN(workingrate / pcpu * limit, 1);
			twork.tv_nsec = TIME_SLOT * 1000 * workingrate;
		}
		tsleep.tv_nsec = TIME_SLOT * 1000 - twork.tv_nsec;

		// if (verbose) {
		// 	if (c%200==0)
		// 		printf("\n%%CPU\twork quantum\tsleep quantum\tactive rate\tpid\n");
		// 	if (c%10==0 && c>0)
		// 		printf("%0.2lf%%\t%6ld us\t%6ld us\t%0.2lf%%\t\t%0.6ld\n", pcpu*100, twork.tv_nsec/1000, tsleep.tv_nsec/1000, workingrate*100, pid);
		// }

		//resume processes
		node = pgroup->proclist->first;
		while (node != NULL)
		{
			struct list_node *next_node = node->next;
			struct process *proc = (struct process*)(node->data);
			if(pid == proc->pid)
			{
				if (kill(proc->pid,SIGCONT) != 0) {
					//process is dead, remove it from family
					if (verbose) fprintf(stderr, "SIGCONT failed. Process %d dead!\n", proc->pid);
					//remove process from group
					delete_node(pgroup->proclist, node);
					remove_process(pgroup, proc->pid);
				}
			}
			node = next_node;
		}

		//now processes are free to run (same working slice for all)
		gettimeofday(&startwork, NULL);
		nanosleep(&twork, NULL);
		gettimeofday(&endwork, NULL);
		workingtime = timediff(&endwork, &startwork);

		long delay = workingtime - twork.tv_nsec/1000;
		if (c>0 && delay>10000) {
			//delay is too much! signal to user?
			//fprintf(stderr, "%d %ld us\n", c, delay);
		}

		if (tsleep.tv_nsec>0) {
			//stop processes only if tsleep>0
			node = pgroup->proclist->first;
			while (node != NULL)
			{
				struct list_node *next_node = node->next;
				struct process *proc = (struct process*)(node->data);
				if(pid == proc->pid)
				{
					if (kill(proc->pid,SIGSTOP)!=0) {
						//process is dead, remove it from family
						if (verbose) fprintf(stderr, "SIGSTOP failed. Process %d dead!\n", proc->pid);
						//remove process from group
						delete_node(pgroup->proclist, node);
						remove_process(pgroup, proc->pid);
					}
					// print_debug(DEBUG_INFO,"try to SIGSTOP pid = %d, pid =%d\n", proc->pid ,pid);
				}

				node = next_node;
			}
			//now the processes are sleeping
			nanosleep(&tsleep,NULL);
		}
		c++;
		close_process_group(pgroup);
	}
	
}

static void *start_cpu_limit_nidps()
{
	//argument variables
	int perclimit = 0;
	int include_children = 0;
	int i=1;

	//print_debug(DEBUG_INFO, "start sc cpu limit server, max cpu is %d\n", registerInfo.scCpuLimit);
	perclimit = 5;
	double limit = perclimit / 100.0;
	pthread_detach(pthread_self()); //设置线程分离,退出后回收资源
#ifdef QNX
	pthread_setname_np(0, "nidps_cpu_limit");
#else
	prctl(PR_SET_NAME, "nidps_cpu_limit");
#endif

	//control
	limit_process(limit, include_children, (struct process_group *)&pgroup[ENGINE_ETH_IDS_INDEX], ENGINE_ETH_IDS_INDEX);
	return 0;
}

static void *start_cpu_limit_hidps()
{
	//argument variables
	int perclimit = 0;
	int include_children = 0;
	int i=1;

	//print_debug(DEBUG_INFO, "start sc cpu limit server, max cpu is %d\n", registerInfo.scCpuLimit);
	perclimit = 5;
	double limit = perclimit / 100.0;
	pthread_detach(pthread_self()); //设置线程分离,退出后回收资源
#ifdef QNX
	pthread_setname_np(0, "hidps_cpu_limit");
#else
	prctl(PR_SET_NAME, "hidps_cpu_limit");
#endif

	//control
	limit_process(limit, include_children, (struct process_group *)&pgroup[ENGINE_HIDPS_INDEX], ENGINE_HIDPS_INDEX);
	return 0;
}


/* 对外函数，本模块启动 */
int cpu_limitStart(void)
{
    char threadName[32];

    memset(threadName, 0, sizeof(threadName));
    memcpy(threadName, "nidps_cpu_limit", ids_strlen("nidps_cpu_limit"));
    idps_thread_create(threadName, (void*)start_cpu_limit_nidps, NULL, 128);
    memset(threadName, 0, sizeof(threadName));
    memcpy(threadName, "hidps_cpu_limit", ids_strlen("hidps_cpu_limit"));
    idps_thread_create(threadName, (void*)start_cpu_limit_hidps, NULL, 128);

    return 0;
}
