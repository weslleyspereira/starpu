/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2011-2012,2014-2015,2017                 Inria
 * Copyright (C) 2008-2017                                Université de Bordeaux
 * Copyright (C) 2010                                     Mehdi Juhoor
 * Copyright (C) 2010-2017                                CNRS
 * Copyright (C) 2011                                     Télécom-SudParis
 *
 * StarPU is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * StarPU is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU Lesser General Public License in COPYING.LGPL for more details.
 */

#include <common/config.h>

#include <math.h>
#include <starpu.h>
#include <starpu_profiling.h>
#include <drivers/driver_common/driver_common.h>
#include <common/utils.h>
#include <core/debug.h>
#include <core/workers.h>
#include <core/drivers.h>
#include <drivers/cpu/driver_cpu.h>
#include <core/sched_policy.h>
#include <datawizard/memory_manager.h>
#include <datawizard/malloc.h>
#include <core/simgrid.h>
#include <core/task.h>

#ifdef STARPU_HAVE_HWLOC
#include <hwloc.h>
#ifndef HWLOC_API_VERSION
#define HWLOC_OBJ_PU HWLOC_OBJ_PROC
#endif
#endif

#ifdef STARPU_HAVE_WINDOWS
#include <windows.h>
#endif


/* Actually launch the job on a cpu worker.
 * Handle binding CPUs on cores.
 * In the case of a combined worker WORKER_TASK != J->TASK */

static int execute_job_on_cpu(struct _starpu_job *j, struct starpu_task *worker_task, struct _starpu_worker *cpu_args, int rank, struct starpu_perfmodel_arch* perf_arch)
{
	int is_parallel_task = (j->task_size > 1);
	int profiling = starpu_profiling_status_get();
	struct starpu_task *task = j->task;
	struct starpu_codelet *cl = task->cl;

	STARPU_ASSERT(cl);

	if (is_parallel_task)
	{
		STARPU_PTHREAD_BARRIER_WAIT(&j->before_work_barrier);

		/* In the case of a combined worker, the scheduler needs to know
		 * when each actual worker begins the execution */
		_starpu_sched_pre_exec_hook(worker_task);
	}

	/* Give profiling variable */
	_starpu_driver_start_job(cpu_args, j, perf_arch, rank, profiling);

	/* In case this is a Fork-join parallel task, the worker does not
	 * execute the kernel at all. */
	if ((rank == 0) || (cl->type != STARPU_FORKJOIN))
	{
		_starpu_cl_func_t func = _starpu_task_get_cpu_nth_implementation(cl, j->nimpl);
		if (is_parallel_task && cl->type == STARPU_FORKJOIN)
			/* bind to parallel worker */
			_starpu_bind_thread_on_cpus(_starpu_get_combined_worker_struct(j->combined_workerid));
		STARPU_ASSERT_MSG(func, "when STARPU_CPU is defined in 'where', cpu_func or cpu_funcs has to be defined");
		if (_starpu_get_disable_kernels() <= 0)
		{
			_STARPU_TRACE_START_EXECUTING();
#ifdef STARPU_SIMGRID
			if (cl->flags & STARPU_CODELET_SIMGRID_EXECUTE)
				func(_STARPU_TASK_GET_INTERFACES(task), task->cl_arg);
			else if (cl->flags & STARPU_CODELET_SIMGRID_EXECUTE_AND_INJECT)
			{
				_SIMGRID_TIMER_BEGIN(1);
				func(_STARPU_TASK_GET_INTERFACES(task), task->cl_arg);
				_SIMGRID_TIMER_END;
			}
			else
				_starpu_simgrid_submit_job(cpu_args->workerid, j, perf_arch, NAN, NULL);
#else
			func(_STARPU_TASK_GET_INTERFACES(task), task->cl_arg);
#endif
			_STARPU_TRACE_END_EXECUTING();
		}
		if (is_parallel_task && cl->type == STARPU_FORKJOIN)
			/* rebind to single CPU */
			_starpu_bind_thread_on_cpu(cpu_args->bindid, cpu_args->workerid);
	}

	_starpu_driver_end_job(cpu_args, j, perf_arch, rank, profiling);

	if (is_parallel_task)
	{
		STARPU_PTHREAD_BARRIER_WAIT(&j->after_work_barrier);
#ifdef STARPU_SIMGRID
		if (rank == 0)
		{
			/* Wait for other threads to exit barrier_wait so we
			 * can safely drop the job structure */
			MSG_process_sleep(0.0000001);
			j->after_work_busy_barrier = 0;
		}
#else
		ANNOTATE_HAPPENS_BEFORE(&j->after_work_busy_barrier);
		(void) STARPU_ATOMIC_ADD(&j->after_work_busy_barrier, -1);
		if (rank == 0)
		{
			/* Wait with a busy barrier for other workers to have
			 * finished with the blocking barrier before we can
			 * safely drop the job structure */
			while (j->after_work_busy_barrier > 0)
			{
				STARPU_UYIELD();
				STARPU_SYNCHRONIZE();
			}
			ANNOTATE_HAPPENS_AFTER(&j->after_work_busy_barrier);
		}
#endif
	}

	if (rank == 0)
	{
		_starpu_driver_update_job_feedback(j, cpu_args, perf_arch, profiling);
#ifdef STARPU_OPENMP
		if (!j->continuation)
#endif
		{
			_starpu_push_task_output(j);
		}
	}

	return 0;
}

static size_t _starpu_cpu_get_global_mem_size(int nodeid STARPU_ATTRIBUTE_UNUSED, struct _starpu_machine_config *config STARPU_ATTRIBUTE_UNUSED)
{
	size_t global_mem;
	starpu_ssize_t limit = -1;

#if defined(STARPU_HAVE_HWLOC)
	struct _starpu_machine_topology *topology = &config->topology;

	int nnumas = starpu_memory_nodes_get_numa_count();
	if (nnumas > 1)
	{
		int depth_node = hwloc_get_type_depth(topology->hwtopology, HWLOC_OBJ_NODE);

		if (depth_node == HWLOC_TYPE_DEPTH_UNKNOWN)
		{
#if HWLOC_API_VERSION >= 0x00020000
			global_mem = hwloc_get_root_obj(topology->hwtopology)->total_memory;
#else
			global_mem = hwloc_get_root_obj(topology->hwtopology)->memory.total_memory;
#endif
		}
		else
		{
			char name[32];
			hwloc_obj_t obj = hwloc_get_obj_by_depth(topology->hwtopology, depth_node, nodeid);
#if HWLOC_API_VERSION >= 0x00020000
			global_mem = obj->attr->numanode.local_memory;
#else
			global_mem = obj->memory.local_memory;
#endif
			snprintf(name, sizeof(name), "STARPU_LIMIT_CPU_NUMA_%d_MEM", obj->os_index);
			limit = starpu_get_env_number(name);
		}
	}
	else
	{
		/* Do not limit ourself to a single NUMA node */
#if HWLOC_API_VERSION >= 0x00020000
		global_mem = hwloc_get_root_obj(topology->hwtopology)->total_memory;
#else
		global_mem = hwloc_get_root_obj(topology->hwtopology)->memory.total_memory;
#endif
	}

#else /* STARPU_HAVE_HWLOC */
#ifdef STARPU_DEVEL
#  warning TODO: use sysinfo when available to get global size
#endif
	global_mem = 0;
#endif

	if (limit == -1)
		limit = starpu_get_env_number("STARPU_LIMIT_CPU_MEM");

	if (limit < 0)
		// No limit is defined, we return the global memory size
		return global_mem;
	else if (global_mem && (size_t)limit * 1024*1024 > global_mem)
		// The requested limit is higher than what is available, we return the global memory size
		return global_mem;
	else
		// We limit the memory
		return limit*1024*1024;
}

int _starpu_cpu_driver_init(struct _starpu_worker *cpu_worker)
{
	int devid = cpu_worker->devid;

	_starpu_driver_start(cpu_worker, _STARPU_FUT_CPU_KEY, 1);
	_starpu_memory_manager_set_global_memory_size(cpu_worker->memory_node, _starpu_cpu_get_global_mem_size(cpu_worker->numa_memory_node, cpu_worker->config));
	snprintf(cpu_worker->name, sizeof(cpu_worker->name), "CPU %d", devid);
	snprintf(cpu_worker->short_name, sizeof(cpu_worker->short_name), "CPU %d", devid);
	starpu_pthread_setname(cpu_worker->short_name);

	_STARPU_TRACE_WORKER_INIT_END(cpu_worker->workerid);

	STARPU_PTHREAD_MUTEX_LOCK_SCHED(&cpu_worker->sched_mutex);
	cpu_worker->status = STATUS_UNKNOWN;
	STARPU_PTHREAD_MUTEX_UNLOCK_SCHED(&cpu_worker->sched_mutex);

	/* tell the main thread that we are ready */
	STARPU_PTHREAD_MUTEX_LOCK(&cpu_worker->mutex);
	cpu_worker->worker_is_initialized = 1;
	STARPU_PTHREAD_COND_SIGNAL(&cpu_worker->ready_cond);
	STARPU_PTHREAD_MUTEX_UNLOCK(&cpu_worker->mutex);
	return 0;
}

static int _starpu_cpu_driver_execute_task(struct _starpu_worker *cpu_worker, struct starpu_task *task, struct _starpu_job *j)
{
	int res;

	int rank;
	int is_parallel_task = (j->task_size > 1);

	struct starpu_perfmodel_arch* perf_arch;

	rank = cpu_worker->current_rank;

	/* Get the rank in case it is a parallel task */
	if (is_parallel_task)
	{
		if(j->combined_workerid != -1)
		{
			struct _starpu_combined_worker *combined_worker;
			combined_worker = _starpu_get_combined_worker_struct(j->combined_workerid);
			
			cpu_worker->combined_workerid = j->combined_workerid;
			cpu_worker->worker_size = combined_worker->worker_size;
			perf_arch = &combined_worker->perf_arch;
		}
		else
		{
			struct _starpu_sched_ctx *sched_ctx = _starpu_sched_ctx_get_sched_ctx_for_worker_and_job(cpu_worker, j);
			STARPU_ASSERT_MSG(sched_ctx != NULL, "there should be a worker %d in the ctx of this job \n", cpu_worker->workerid);

			perf_arch = &sched_ctx->perf_arch;
		}
	}
	else
	{
		cpu_worker->combined_workerid = cpu_worker->workerid;
		cpu_worker->worker_size = 1;

		struct _starpu_sched_ctx *sched_ctx = _starpu_sched_ctx_get_sched_ctx_for_worker_and_job(cpu_worker, j);
		if (sched_ctx && !sched_ctx->sched_policy && !sched_ctx->awake_workers && sched_ctx->main_master == cpu_worker->workerid)
			perf_arch = &sched_ctx->perf_arch;
		else
			perf_arch = &cpu_worker->perf_arch;
	}

	_starpu_set_current_task(j->task);
	cpu_worker->current_task = j->task;

	res = execute_job_on_cpu(j, task, cpu_worker, rank, perf_arch);

	_starpu_set_current_task(NULL);
	cpu_worker->current_task = NULL;

	if (res)
	{
		switch (res)
		{
		case -EAGAIN:
			_starpu_push_task_to_workers(task);
			return 0;
		default:
			STARPU_ABORT();
		}
	}

	/* In the case of combined workers, we need to inform the
	 * scheduler each worker's execution is over.
	 * Then we free the workers' task alias */
	if (is_parallel_task)
	{
		_starpu_sched_post_exec_hook(task);
		free(task);
	}

	if (rank == 0)
		_starpu_handle_job_termination(j);
	return 0;
}

int _starpu_cpu_driver_run_once(struct _starpu_worker *cpu_worker)
{
	unsigned memnode = cpu_worker->memory_node;
	int workerid = cpu_worker->workerid;

	int res;

	struct _starpu_job *j;
	struct starpu_task *task = NULL, *pending_task;

	int rank = 0;

#ifdef STARPU_SIMGRID
	starpu_pthread_wait_reset(&cpu_worker->wait);
#endif

	/* Test if async transfers are completed */
	pending_task = cpu_worker->task_transferring;
	if (pending_task != NULL && cpu_worker->nb_buffers_transferred == cpu_worker->nb_buffers_totransfer)
	{
		int ret;
		_STARPU_TRACE_END_PROGRESS(memnode);
		j = _starpu_get_job_associated_to_task(pending_task);

		_starpu_fetch_task_input_tail(pending_task, j, cpu_worker);
		_starpu_set_worker_status(cpu_worker, STATUS_UNKNOWN);
		/* Reset it */
		cpu_worker->task_transferring = NULL;

		ret = _starpu_cpu_driver_execute_task(cpu_worker, pending_task, j);
		_STARPU_TRACE_START_PROGRESS(memnode);
		return ret;
	}

	res = __starpu_datawizard_progress(1, 1);

	if (!pending_task)
		task = _starpu_get_worker_task(cpu_worker, workerid, memnode);

#ifdef STARPU_SIMGRID
 #ifndef STARPU_OPENMP
	if (!res && !task)
		/* No progress, wait */
		starpu_pthread_wait_wait(&cpu_worker->wait);
 #else
  #if SIMGRID_VERSION_MAJOR > 3 || (SIMGRID_VERSION_MAJOR == 3 && SIMGRID_VERSION_MINOR >= 18)
	if (!res && !task)
	{
		/* No progress, wait (but at most 1s for OpenMP support) */
		/* TODO: ideally, make OpenMP wake worker when run_once should return */
		struct timespec abstime;
		_starpu_clock_gettime(&abstime);
		abstime.tv_sec++;
		starpu_pthread_wait_timedwait(&cpu_worker->wait, &abstime);
	}
  #else
	/* Previous simgrid versions don't really permit to use wait_timedwait in C */
	MSG_process_sleep(0.001);
  #endif
 #endif
#endif

	if (!task)
		/* No task or task still pending transfers */
		return 0;

	j = _starpu_get_job_associated_to_task(task);
	/* NOTE: j->task is != task for parallel tasks, which share the same
	 * job. */

	/* can a cpu perform that task ? */
	if (!_STARPU_CPU_MAY_PERFORM(j))
	{
		/* put it and the end of the queue ... XXX */
		_starpu_push_task_to_workers(task);
		return 0;
	}

	_STARPU_TRACE_END_PROGRESS(memnode);
	/* Get the rank in case it is a parallel task */
	if (j->task_size > 1)
	{
		STARPU_PTHREAD_MUTEX_LOCK(&j->sync_mutex);
		rank = j->active_task_alias_count++;
		STARPU_PTHREAD_MUTEX_UNLOCK(&j->sync_mutex);
	}
	else
	{
		rank = 0;
	}
	cpu_worker->current_rank = rank;

#ifdef STARPU_OPENMP
	/* At this point, j->continuation as been cleared as the task is being
	 * woken up, thus we use j->discontinuous instead for the check */
	const unsigned continuation_wake_up = j->discontinuous;
#else
	const unsigned continuation_wake_up = 0;
#endif
	if (rank == 0 && !continuation_wake_up)
	{
		res = _starpu_fetch_task_input(task, j, 1);
		STARPU_ASSERT(res == 0);
	}
	else
	{
		int ret = _starpu_cpu_driver_execute_task(cpu_worker, task, j);
		_STARPU_TRACE_END_PROGRESS(memnode);
		return ret;
	}
	_STARPU_TRACE_END_PROGRESS(memnode);
	return 0;
}

int _starpu_cpu_driver_deinit(struct _starpu_worker *cpu_worker)
{
	_STARPU_TRACE_WORKER_DEINIT_START;

	unsigned memnode = cpu_worker->memory_node;
	_starpu_handle_all_pending_node_data_requests(memnode);

	/* In case there remains some memory that was automatically
	 * allocated by StarPU, we release it now. Note that data
	 * coherency is not maintained anymore at that point ! */
	_starpu_free_all_automatically_allocated_buffers(memnode);

	cpu_worker->worker_is_initialized = 0;
	_STARPU_TRACE_WORKER_DEINIT_END(_STARPU_FUT_CPU_KEY);

	return 0;
}

void *_starpu_cpu_worker(void *arg)
{
	struct _starpu_worker *worker = arg;

	_starpu_cpu_driver_init(worker);
	_STARPU_TRACE_START_PROGRESS(worker->memory_node);
	while (_starpu_machine_is_running())
	{
		_starpu_may_pause();
		_starpu_cpu_driver_run_once(worker);
	}
	_STARPU_TRACE_END_PROGRESS(worker->memory_node);
	_starpu_cpu_driver_deinit(worker);

	return NULL;
}

int _starpu_cpu_driver_run(struct _starpu_worker *worker)
{
	worker->set = NULL;
	worker->worker_is_initialized = 0;
	_starpu_cpu_worker(worker);

	return 0;
}

struct _starpu_driver_ops _starpu_driver_cpu_ops =
{
	.init = _starpu_cpu_driver_init,
	.run = _starpu_cpu_driver_run,
	.run_once = _starpu_cpu_driver_run_once,
	.deinit = _starpu_cpu_driver_deinit
};
