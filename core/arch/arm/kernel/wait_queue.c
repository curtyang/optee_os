/*
 * Copyright (c) 2015, Linaro Limited
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <types_ext.h>
#include <sm/teesmc.h>
#include <kernel/tz_proc.h>
#include <kernel/wait_queue.h>
#include <kernel/tee_ta_manager.h>
#include <kernel/thread.h>
#include <kernel/tee_rpc.h>
#include <trace.h>

static unsigned wq_spin_lock;


void wq_init(struct wait_queue *wq)
{
	*wq = (struct wait_queue)WAIT_QUEUE_INITIALIZER;
}

static void wq_rpc(uint32_t cmd, int id)
{
	uint32_t ret;
	struct tee_ta_session *sess = NULL;
	struct teesmc32_param params[2];

	DMSG("%s thread %u",
	     cmd == TEE_RPC_WAIT_QUEUE_SLEEP ? "sleep" : "wake ", id);

	tee_ta_get_current_session(&sess);
	if (sess)
		tee_ta_set_current_session(NULL);

	memset(params, 0, sizeof(params));
	params[0].attr = TEESMC_ATTR_TYPE_VALUE_INPUT;
	params[1].attr = TEESMC_ATTR_TYPE_NONE;
	params[0].u.value.a = id;

	ret = thread_rpc_cmd(cmd, 2, params);
	if (ret != TEE_SUCCESS)
		DMSG("%s thread %u ret 0x%x",
		     cmd == TEE_RPC_WAIT_QUEUE_SLEEP ? "sleep" : "wake ", id,
		     ret);

	if (sess)
		tee_ta_set_current_session(sess);
}

static void slist_add_tail(struct wait_queue *wq, struct wait_queue_elem *wqe)
{
	struct wait_queue_elem *wqe_iter;

	/* Add elem to end of wait queue */
	wqe_iter = SLIST_FIRST(wq);
	if (wqe_iter) {
		while (SLIST_NEXT(wqe_iter, link))
			wqe_iter = SLIST_NEXT(wqe_iter, link);
		SLIST_INSERT_AFTER(wqe_iter, wqe, link);
	} else
		SLIST_INSERT_HEAD(wq, wqe, link);

}

void wq_wait_init(struct wait_queue *wq, struct wait_queue_elem *wqe)
{
	uint32_t old_itr_status;

	wqe->handle = thread_get_id();
	wqe->done = false;

	old_itr_status = thread_mask_exceptions(THREAD_EXCP_ALL);
	cpu_spin_lock(&wq_spin_lock);

	slist_add_tail(wq, wqe);

	cpu_spin_unlock(&wq_spin_lock);
	thread_unmask_exceptions(old_itr_status);
}

void wq_wait_final(struct wait_queue *wq, struct wait_queue_elem *wqe)
{
	uint32_t old_itr_status;
	unsigned done;

	do {
		wq_rpc(TEE_RPC_WAIT_QUEUE_SLEEP, wqe->handle);

		old_itr_status = thread_mask_exceptions(THREAD_EXCP_ALL);
		cpu_spin_lock(&wq_spin_lock);

		done = wqe->done;
		if (done)
			SLIST_REMOVE(wq, wqe, wait_queue_elem, link);

		cpu_spin_unlock(&wq_spin_lock);
		thread_unmask_exceptions(old_itr_status);
	} while (!done);
}

void wq_wake_one(struct wait_queue *wq)
{
	uint32_t old_itr_status;
	struct wait_queue_elem *wqe;
	int handle = -1;
	bool do_wakeup = false;

	old_itr_status = thread_mask_exceptions(THREAD_EXCP_ALL);
	cpu_spin_lock(&wq_spin_lock);

	wqe = SLIST_FIRST(wq);
	if (wqe) {
		do_wakeup = !wqe->done;
		wqe->done = true;
		handle = wqe->handle;
	}

	cpu_spin_unlock(&wq_spin_lock);
	thread_unmask_exceptions(old_itr_status);

	if (do_wakeup)
		wq_rpc(TEE_RPC_WAIT_QUEUE_WAKEUP, handle);
}

bool wq_is_empty(struct wait_queue *wq)
{
	uint32_t old_itr_status;
	bool ret;

	old_itr_status = thread_mask_exceptions(THREAD_EXCP_ALL);
	cpu_spin_lock(&wq_spin_lock);

	ret = SLIST_EMPTY(wq);

	cpu_spin_unlock(&wq_spin_lock);
	thread_unmask_exceptions(old_itr_status);

	return ret;
}