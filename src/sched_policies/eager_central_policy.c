/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010-2016  Université de Bordeaux
 * Copyright (C) 2010-2013  CNRS
 * Copyright (C) 2011  INRIA
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

/*
 *	This is just the trivial policy where every worker use the same
 *	JOB QUEUE.
 */

#include <starpu_scheduler.h>
#include <sched_policies/fifo_queues.h>
#include <common/thread.h>
#include <starpu_bitmap.h>

struct _starpu_eager_center_policy_data
{
	struct _starpu_fifo_taskq *fifo;
	starpu_pthread_mutex_t policy_mutex;
	struct starpu_bitmap *waiters;
};

static void initialize_eager_center_policy(unsigned sched_ctx_id)
{
	struct _starpu_eager_center_policy_data *data = (struct _starpu_eager_center_policy_data*)malloc(sizeof(struct _starpu_eager_center_policy_data));

	_STARPU_DISP("Warning: you are running the default eager scheduler, which is not a very smart scheduler. Make sure to read the StarPU documentation about adding performance models in order to be able to use the dmda or dmdas scheduler instead.\n");

	/* there is only a single queue in that trivial design */
	data->fifo =  _starpu_create_fifo();
	data->waiters = starpu_bitmap_create();

	 /* Tell helgrind that it's fine to check for empty fifo in
	  * pop_task_eager_policy without actual mutex (it's just an integer)
	  */
	STARPU_HG_DISABLE_CHECKING(data->fifo->ntasks);

	starpu_sched_ctx_set_policy_data(sched_ctx_id, (void*)data);
	STARPU_PTHREAD_MUTEX_INIT(&data->policy_mutex, NULL);
}

static void deinitialize_eager_center_policy(unsigned sched_ctx_id)
{
	struct _starpu_eager_center_policy_data *data = (struct _starpu_eager_center_policy_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
	struct _starpu_fifo_taskq *fifo = data->fifo;

	STARPU_ASSERT(starpu_task_list_empty(&fifo->taskq));

	/* deallocate the job queue */
	_starpu_destroy_fifo(fifo);
	starpu_bitmap_destroy(data->waiters);

	STARPU_PTHREAD_MUTEX_DESTROY(&data->policy_mutex);
	free(data);
}

static int push_task_eager_policy(struct starpu_task *task)
{
	unsigned sched_ctx_id = task->sched_ctx;
	struct _starpu_eager_center_policy_data *data = (struct _starpu_eager_center_policy_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	STARPU_PTHREAD_MUTEX_LOCK(&data->policy_mutex);
	starpu_task_list_push_back(&data->fifo->taskq,task);
	data->fifo->ntasks++;
	data->fifo->nprocessed++;

	starpu_push_task_end(task);

	/*if there are no tasks block */
	/* wake people waiting for a task */
	unsigned worker = 0;
	struct starpu_worker_collection *workers = starpu_sched_ctx_get_worker_collection(sched_ctx_id);
	
	struct starpu_sched_ctx_iterator it;
#ifndef STARPU_NON_BLOCKING_DRIVERS
	char dowake[STARPU_NMAXWORKERS] = { 0 };
#endif

	workers->init_iterator_for_parallel_tasks(workers, &it, task);
	while(workers->has_next(workers, &it))
	{
		worker = workers->get_next(workers, &it);

#ifdef STARPU_NON_BLOCKING_DRIVERS
		if (!starpu_bitmap_get(data->waiters, worker))
			/* This worker is not waiting for a task */
			continue;
#endif

		if (starpu_worker_can_execute_task_first_impl(worker, task, NULL))
		{
			/* It can execute this one, tell him! */
#ifdef STARPU_NON_BLOCKING_DRIVERS
			starpu_bitmap_unset(data->waiters, worker);
			/* We really woke at least somebody, no need to wake somebody else */
			break;
#else
			dowake[worker] = 1;
#endif
		}
	}
	/* Let the task free */
	STARPU_PTHREAD_MUTEX_UNLOCK(&data->policy_mutex);

#if !defined(STARPU_NON_BLOCKING_DRIVERS) || defined(STARPU_SIMGRID)
	/* Now that we have a list of potential workers, try to wake one */

	workers->init_iterator_for_parallel_tasks(workers, &it, task);
	while(workers->has_next(workers, &it))
	{
		worker = workers->get_next(workers, &it);
		if (dowake[worker])
			if (starpu_wake_worker(worker))
				break; // wake up a single worker
	}
#endif

	starpu_sched_ctx_list_task_counters_increment_all(task, sched_ctx_id);

	return 0;
}

static struct starpu_task *pop_every_task_eager_policy(unsigned sched_ctx_id)
{
	struct _starpu_eager_center_policy_data *data = (struct _starpu_eager_center_policy_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
	int workerid = starpu_worker_get_id();;
	STARPU_PTHREAD_MUTEX_LOCK(&data->policy_mutex);
	struct starpu_task* task = _starpu_fifo_pop_every_task(data->fifo, workerid);
	STARPU_PTHREAD_MUTEX_UNLOCK(&data->policy_mutex);

	starpu_sched_ctx_list_task_counters_reset_all(task, sched_ctx_id);

	return task;
}

static struct starpu_task *pop_task_eager_policy(unsigned sched_ctx_id)
{
	struct starpu_task *chosen_task = NULL;
	unsigned workerid = starpu_worker_get_id();
	struct _starpu_eager_center_policy_data *data = (struct _starpu_eager_center_policy_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	/* block until some event happens */
	/* Here helgrind would shout that this is unprotected, this is just an
	 * integer access, and we hold the sched mutex, so we can not miss any
	 * wake up. */
	if (!STARPU_RUNNING_ON_VALGRIND && _starpu_fifo_empty(data->fifo))
		return NULL;

#ifdef STARPU_NON_BLOCKING_DRIVERS
	if (!STARPU_RUNNING_ON_VALGRIND && starpu_bitmap_get(data->waiters, workerid))
		/* Nobody woke us, avoid bothering the mutex */
		return NULL;
#endif

	STARPU_PTHREAD_MUTEX_LOCK(&data->policy_mutex);

	chosen_task = _starpu_fifo_pop_task(data->fifo, workerid);
	if (!chosen_task)
		/* Tell pushers that we are waiting for tasks for us */
		starpu_bitmap_set(data->waiters, workerid);

	STARPU_PTHREAD_MUTEX_UNLOCK(&data->policy_mutex);

	if(chosen_task)
	{
		starpu_sched_ctx_list_task_counters_decrement_all(chosen_task, sched_ctx_id);

		unsigned child_sched_ctx = starpu_sched_ctx_worker_is_master_for_child_ctx(workerid, sched_ctx_id);
		if(child_sched_ctx != STARPU_NMAX_SCHED_CTXS)
		{
			starpu_sched_ctx_move_task_to_ctx(chosen_task, child_sched_ctx, 1);
			starpu_sched_ctx_revert_task_counters(sched_ctx_id, chosen_task->flops);
			return NULL;
		}
	}


	return chosen_task;
}

static void eager_add_workers(unsigned sched_ctx_id, int *workerids, unsigned nworkers)
{

	int workerid;
	unsigned i;
	for (i = 0; i < nworkers; i++)
	{
		workerid = workerids[i];
		int curr_workerid = starpu_worker_get_id();
		if(workerid != curr_workerid)
			starpu_wake_worker(workerid);

		starpu_sched_ctx_worker_shares_tasks_lists(workerid, sched_ctx_id);
	}
}

struct starpu_sched_policy _starpu_sched_eager_policy =
{
	.init_sched = initialize_eager_center_policy,
	.deinit_sched = deinitialize_eager_center_policy,
	.add_workers = eager_add_workers,
	.remove_workers = NULL,
	.push_task = push_task_eager_policy,
	.pop_task = pop_task_eager_policy,
	.pre_exec_hook = NULL,
	.post_exec_hook = NULL,
	.pop_every_task = pop_every_task_eager_policy,
	.policy_name = "eager",
	.policy_description = "eager policy with a central queue",
#ifdef STARPU_HAVE_HWLOC
	.worker_type = STARPU_WORKER_TREE,
#else
	.worker_type = STARPU_WORKER_LIST,
#endif
};
