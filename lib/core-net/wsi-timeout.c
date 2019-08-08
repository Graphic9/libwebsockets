/*
 * libwebsockets - small server side websockets and web server implementation
 *
 * Copyright (C) 2010-2019 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */

#include "core/private.h"

void
__lws_remove_from_timeout_list(struct lws *wsi)
{
	lws_dll2_remove(&wsi->dll_timeout);
}

void
lws_remove_from_timeout_list(struct lws *wsi)
{
	struct lws_context_per_thread *pt = &wsi->context->pt[(int)wsi->tsi];

	lws_pt_lock(pt, __func__);
	__lws_remove_from_timeout_list(wsi);
	lws_pt_unlock(pt);
}


void
__lws_set_timer_usecs(struct lws *wsi, lws_usec_t us)
{
	struct lws_context_per_thread *pt = &wsi->context->pt[(int)wsi->tsi];

	lws_dll2_remove(&wsi->dll_hrtimer);

	if (us == LWS_SET_TIMER_USEC_CANCEL)
		return;

	wsi->pending_timer = lws_now_usecs() + us;

	/*
	 * we sort the hrtimer list with the earliest timeout first
	 */

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, tp,
				   pt->dll_hrtimer_head.head) {
		struct lws *w = lws_container_of(p, struct lws, dll_hrtimer);

		assert(w->pending_timer); /* shouldn't be on the list otherwise */
		if (w->pending_timer >= wsi->pending_timer) {
			/* drop us in before this guy */
			lws_dll2_add_before(&wsi->dll_hrtimer, &w->dll_hrtimer);

			return;
		}
	} lws_end_foreach_dll_safe(p, tp);

	/*
	 * Either nobody on the list yet to compare him to, or he's the
	 * longest timeout... stick him at the tail end
	 */

	lws_dll2_add_tail(&wsi->dll_hrtimer, &pt->dll_hrtimer_head);
}

LWS_VISIBLE void
lws_set_timer_usecs(struct lws *wsi, lws_usec_t usecs)
{
	__lws_set_timer_usecs(wsi, usecs);
}

/* return 0 if nothing pending, or the number of us before the next event */

lws_usec_t
__lws_hrtimer_service(struct lws_context_per_thread *pt, lws_usec_t t)
{
	struct lws *wsi;

	lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1,
			lws_dll2_get_head(&pt->dll_hrtimer_head)) {
		wsi = lws_container_of(d, struct lws, dll_hrtimer);

		/*
		 * if we met one in the future, we are done, because the list
		 * is sorted by time in the future.
		 */
		if (wsi->pending_timer > t)
			break;

		lws_set_timer_usecs(wsi, LWS_SET_TIMER_USEC_CANCEL);

		/* it's time for the timer to be serviced */

		if (wsi->protocol &&
		    wsi->protocol->callback(wsi, LWS_CALLBACK_TIMER,
					    wsi->user_space, NULL, 0))
			__lws_close_free_wsi(wsi, LWS_CLOSE_STATUS_NOSTATUS,
					     "timer cb errored");
	} lws_end_foreach_dll_safe(d, d1);

	/* return an estimate how many us until next timer hit */

	if (!lws_dll2_get_head(&pt->dll_hrtimer_head))
		return 0; /* there is nothing pending */

	wsi = lws_container_of(lws_dll2_get_head(&pt->dll_hrtimer_head),
			       struct lws, dll_hrtimer);

	t = lws_now_usecs();
	if (wsi->pending_timer <= t) /* in the past */
		return 1;

	return wsi->pending_timer - t; /* at least 1 */
}

void
__lws_set_timeout(struct lws *wsi, enum pending_timeout reason, int secs)
{
	struct lws_context_per_thread *pt = &wsi->context->pt[(int)wsi->tsi];
	time_t now;

	time(&now);

	lwsl_debug("%s: %p: %d secs, reason %d\n", __func__, wsi, secs, reason);

	wsi->pending_timeout_limit = secs;
	wsi->pending_timeout_set = now;
	wsi->pending_timeout = reason;

	lws_dll2_remove(&wsi->dll_timeout);
	if (!reason)
		return;

	lws_dll2_add_head(&wsi->dll_timeout, &pt->dll_timeout_owner);
}

LWS_VISIBLE void
lws_set_timeout(struct lws *wsi, enum pending_timeout reason, int secs)
{
	struct lws_context_per_thread *pt = &wsi->context->pt[(int)wsi->tsi];

	// lwsl_info("%s: %p: %d %d\n", __func__, wsi, reason, secs);

	if (secs == LWS_TO_KILL_SYNC) {
		lws_remove_from_timeout_list(wsi);
		lwsl_debug("synchronously killing %p\n", wsi);
		lws_close_free_wsi(wsi, LWS_CLOSE_STATUS_NOSTATUS,
				   "to sync kill");
		return;
	}

	if (secs == LWS_TO_KILL_ASYNC)
		secs = 0;

	lws_pt_lock(pt, __func__);
	__lws_set_timeout(wsi, reason, secs);
	lws_pt_unlock(pt);
}

/* requires context + vh lock */

int
__lws_timed_callback_remove(struct lws_vhost *vh, struct lws_timed_vh_protocol *p)
{
	lws_start_foreach_llp(struct lws_timed_vh_protocol **, pt,
			      vh->timed_vh_protocol_list) {
		if (*pt == p) {
			*pt = p->next;
			lws_free(p);

			return 0;
		}
	} lws_end_foreach_llp(pt, next);

	return 1;
}


LWS_VISIBLE LWS_EXTERN int
lws_timed_callback_vh_protocol(struct lws_vhost *vh,
			       const struct lws_protocols *prot, int reason,
			       int secs)
{
	struct lws_timed_vh_protocol *p = (struct lws_timed_vh_protocol *)
			lws_malloc(sizeof(*p), "timed_vh");

	if (!p)
		return 1;

	p->tsi_req = lws_pthread_self_to_tsi(vh->context);
	if (p->tsi_req < 0) /* not called from a service thread --> tsi 0 */
		p->tsi_req = 0;

	lws_context_lock(vh->context, __func__); /* context ----------------- */

	p->protocol = prot;
	p->reason = reason;
	p->time = lws_now_secs() + secs;

	lws_vhost_lock(vh); /* vhost ---------------------------------------- */
	p->next = vh->timed_vh_protocol_list;
	vh->timed_vh_protocol_list = p;
	lws_vhost_unlock(vh); /* -------------------------------------- vhost */

	lws_context_unlock(vh->context); /* ------------------------- context */

	return 0;
}