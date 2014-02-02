/* Arcan-fe, scriptable front-end engine
 *
 * Arcan-fe is the legal property of its developers, please refer
 * to the COPYRIGHT file distributed with this source distribution.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <errno.h>
#include <math.h>
#include <assert.h>
#include <signal.h>

/* fixed limit of allowed events in queue before old gets overwritten */
#ifndef ARCAN_EVENT_QUEUE_LIM
#define ARCAN_EVENT_QUEUE_LIM 1024
#endif

#include "arcan_math.h"
#include "arcan_general.h"
#include "arcan_video.h"
#include "arcan_audio.h"

#include "arcan_shmpage_if.h"
#include "arcan_event.h"

#include "arcan_framequeue.h"
#include "arcan_frameserver_backend.h"

static int64_t arcan_last_frametime = 0;
static int64_t arcan_tickofset = 0;

typedef struct queue_cell queue_cell;

static arcan_event eventbuf[ARCAN_EVENT_QUEUE_LIM];
static unsigned eventfront = 0, eventback = 0;

static struct arcan_evctx default_evctx = {
	.eventbuf = eventbuf,
	.eventbuf_sz = ARCAN_EVENT_QUEUE_LIM,
	.front = &eventfront,
	.back = &eventback,
	.local = true
};

arcan_evctx* arcan_event_defaultctx(){
	return &default_evctx;
}

/* 
 * If the shmpage integrity is somehow compromised,
 * if semaphore use is out of order etc.
 */
static void pull_killswitch(arcan_evctx* ctx)
{
	arcan_frameserver* ks = (arcan_frameserver*) ctx->synch.killswitch;
	arcan_sem_post(ctx->synch.handle);
	arcan_warning("inconsistency while processing "
		"shmpage events, pulling killswitch.\n");
	arcan_frameserver_free(ks, ks->loop);
	ctx->synch.killswitch = NULL;
}

static unsigned alloc_queuecell(arcan_evctx* ctx)
{
	unsigned rv = *(ctx->back);
	if (ctx->local){
		*(ctx->back) = (*(ctx->back) + 1) % ctx->eventbuf_sz;
	}
/* can't trust queue_sz for external */
	else {
		if (*(ctx->back) > ARCAN_SHMPAGE_QUEUE_SZ)
			pull_killswitch(ctx);
		else {
			*(ctx->back) = (*(ctx->back) + 1) % ARCAN_SHMPAGE_QUEUE_SZ;	
		}	
	}

	return rv;
}

/* check queue for event, ignores mask */
int arcan_event_poll(struct arcan_evctx* ctx, struct arcan_event* dst)
{
	assert(dst);
	if (*ctx->front == *ctx->back)
		return 0;

	if (ctx->local == false){
		if ( *(ctx->front) > ARCAN_SHMPAGE_QUEUE_SZ )
			pull_killswitch(ctx);
		else {
			*dst = ctx->eventbuf[ *(ctx->front) ];
			*(ctx->front) = (*(ctx->front) + 1) % ARCAN_SHMPAGE_QUEUE_SZ;
		}
	}
	else {
		*dst = ctx->eventbuf[ *(ctx->front) ];
		*(ctx->front) = (*(ctx->front) + 1) % ctx->eventbuf_sz;
	}

	return 1;
}

void arcan_event_maskall(arcan_evctx* ctx)
{
	ctx->mask_cat_inp = 0xffffffff;
}

void arcan_event_clearmask(arcan_evctx* ctx)
{
	ctx->mask_cat_inp = 0;
}

void arcan_event_setmask(arcan_evctx* ctx, uint32_t mask)
{
	ctx->mask_cat_inp = mask;
}

/*
 * This is something of a hack, process the entire eventqueue
 * and switch the source element for video/frameserver inputs to
 * a BADID (which won't get propagated when pushing to LUA).
 *
 * The rationale stem from that there can be other events in the
 * queue after an object has died (note; video manipulation 
 * comes from the same (main) thread and new ones for the object
 * in question won't be enqueued during this call, so the 
 * lockless ringbuffer still holds.
 */
void arcan_event_erase_vobj(arcan_evctx* ctx, 
	enum ARCAN_EVENT_CATEGORY category, arcan_vobj_id source)
{
	unsigned elem = *(ctx->front);

/* ignore unsupported categories */
	if ( !(category == EVENT_VIDEO || category == EVENT_FRAMESERVER) )
		return;

	while(elem != *(ctx->back)){

		switch (ctx->eventbuf[elem].category){
		case EVENT_VIDEO: 
			if (ctx->eventbuf[elem].data.video.source == source)
				ctx->eventbuf[elem].data.video.source = ARCAN_EID;
		break;
		case EVENT_FRAMESERVER: 
			if (ctx->eventbuf[elem].data.frameserver.video == source)
				ctx->eventbuf[elem].data.frameserver.video = ARCAN_EID; 
		break;
		}

		elem = (elem + 1) % ctx->eventbuf_sz;
	}
}

static inline int queue_used(arcan_evctx* dq)
{
	int rv = *(dq->front) > *(dq->back) ? dq->eventbuf_sz - 
		*(dq->front) + *(dq->back) : *(dq->back) - *(dq->front);
	return rv;
}

/*
 * enqueue to current context considering input-masking,
 * unless label is set, assign one based on what kind of event it is
 * This function has a similar prototype to the enqueue defined in
 * the interop.h, but a different implementation to support waking up
 * the child, and that blocking behaviors in the main thread is always
 * forbidden. 
 */
int arcan_event_enqueue(arcan_evctx* ctx, const struct arcan_event* const src) 
{
	int ntr = ctx->eventbuf_sz - queue_used(ctx);

/* early-out mask-filter, these are only ever used to silently
 * discard input / output (only operate on head and tail of ringbuffer) */
	if (!src || (src->category & ctx->mask_cat_inp)){
		return ntr; 
	}

/* 
 * Note, we should add panic /warning hooks here as the internal event 
 * subsystem is overloaded, which is a sign of something gone wrong.
 * The recover option would be to silently overwrite one of the lesser
 * important (typically, ANALOG INPUTS or frame counters) in the queue
 */
	if (ntr <= 1)
		return 0;

	unsigned ind = alloc_queuecell(ctx);
	arcan_event* dst = &ctx->eventbuf[ind];
	*dst = *src;
	dst->tickstamp = ctx->c_ticks;

/* 
 * Currently, we just wake the sleeping frameserver up as soon as we get
 * an event (and it's actually sleeping), the better option would be
 * to somehow determine if we'll have more useful events coming in a little
 * while so that we don't get a sleep -> 1 event -> sleep -> 1 event scenario
 * for highly interactive frameservers
 */
	if (!ctx->local){
		int semv;
		if (-1 == arcan_sem_value(ctx->synch.handle, &semv)){
			char buf[128];
			if ( strerror_r(errno, buf, 128) == 0 ){
				buf[128] = 0;
				arcan_warning("broken synchronization (%d:%s)", errno, buf);
			}
			else{
				arcan_warning("broken synchronization (%d)", errno);
			}
		}
		else if (semv == 0){
			arcan_sem_post(ctx->synch.handle);
		}
	}

	return ntr - 1; 
}

void arcan_event_queuetransfer(arcan_evctx* dstqueue, arcan_evctx* srcqueue, 
	enum ARCAN_EVENT_CATEGORY allowed, float saturation, arcan_vobj_id source)
{
	if (!srcqueue || !dstqueue || (srcqueue && !srcqueue->front) 
		|| (srcqueue && !srcqueue->back))
		return;

	saturation = (saturation > 1.0 ? 1.0 : saturation < 0.5 ? 0.5 : saturation);

	while ( srcqueue->front && *srcqueue->front != *srcqueue->back &&
			floor((float)dstqueue->eventbuf_sz * saturation) > queue_used(dstqueue) ){

		arcan_event inev;
		if (arcan_event_poll(srcqueue, &inev) == 0)
			break;
		
/*
 * update / translate to make sure the corresponding frameserver<->lua mapping
 * can be found and tracked 
 */
		if ((inev.category & allowed) > 0 ){
			if (inev.category == EVENT_EXTERNAL)
				inev.data.external.source = source;

			else if (inev.category == EVENT_NET){
				inev.data.network.source = source;
			}

			arcan_event_enqueue(dstqueue, &inev);
		}
	}

}

extern void platform_key_repeat(arcan_evctx* ctx, unsigned rate);
void arcan_event_keyrepeat(arcan_evctx* ctx, unsigned rate)
{
	platform_key_repeat(ctx, rate);
}

int64_t arcan_frametime()
{
	return arcan_last_frametime - arcan_tickofset;
}

/* the main usage case is simply to alternate between process and poll 
 * after a scene has been setup */
extern void platform_event_process(arcan_evctx* ctx);
float arcan_event_process(arcan_evctx* ctx, unsigned* dtick)
{
	static const int rebase_timer_threshold = ARCAN_TIMER_TICK * 1000;

	arcan_last_frametime = arcan_timemillis();
	unsigned delta  = arcan_last_frametime - ctx->c_ticks;

/*
 * compensate for a massive stall, non-monotonic clock
 * or first time initialization 
 */
	if (ctx->c_ticks == 0 || delta == 0 || delta > rebase_timer_threshold){
		ctx->c_ticks = arcan_last_frametime;
		delta = 1;
	}
	
	unsigned nticks = delta / ARCAN_TIMER_TICK;
	float fragment = ((float)(delta % ARCAN_TIMER_TICK) + 0.0001) /
		(float) ARCAN_TIMER_TICK;

	if (nticks){
		arcan_event newevent = {.category = EVENT_TIMER, 
			.kind = 0, 
			.data.timer.pulse_count = nticks
		};

		ctx->c_ticks += nticks * ARCAN_TIMER_TICK;
		arcan_event_enqueue(ctx, &newevent);
	}

	*dtick = nticks;
	platform_event_process(ctx);

	return fragment;
}

arcan_benchdata benchdata = {0};

/* 
 * keep the time tracking separate from the other 
 * timekeeping parts, discard non-monotonic values
 */
void arcan_bench_register_tick(unsigned nticks)
{
	static long long int lasttick = -1;
	if (benchdata.bench_enabled == false)
		return;

	while (nticks--){
		long long int ftime = arcan_timemillis();
		benchdata.tickcount++;

		if (lasttick > 0 && ftime > lasttick){
			unsigned delta = ftime - lasttick;
			benchdata.ticktime[(unsigned)benchdata.tickofs] = delta;
			benchdata.tickofs = (benchdata.tickofs + 1) % 
				(sizeof(benchdata.ticktime) / sizeof(benchdata.ticktime[0]));
		}
		
		lasttick = ftime;
	}
}

void arcan_bench_register_cost(unsigned cost)
{
	benchdata.framecost[(unsigned)benchdata.costofs] = cost;
	if (benchdata.bench_enabled == false)
		return;

	benchdata.costcount++;
	benchdata.costofs = (benchdata.costofs + 1) % 
		(sizeof(benchdata.framecost) / sizeof(benchdata.framecost[0]));
}

void arcan_bench_register_frame()
{
	static long long int lastframe = -1;
	if (benchdata.bench_enabled == false)
		return;

	long long int ftime = arcan_timemillis();
	if (lastframe > 0 && ftime > lastframe){
		unsigned delta = ftime - lastframe;
		benchdata.frametime[(unsigned)benchdata.frameofs] = delta;
		benchdata.framecount++;
		benchdata.frameofs = (benchdata.frameofs + 1) % 
			(sizeof(benchdata.frametime) / sizeof(benchdata.frametime[0]));
		}

	lastframe = ftime;
}

extern void platform_event_deinit(arcan_evctx* ctx);
void arcan_event_deinit(arcan_evctx* ctx)
{
	platform_event_deinit(ctx);
	eventfront = eventback = 0;
}

extern void platform_event_init(arcan_evctx* ctx);
void arcan_event_init(arcan_evctx* ctx)
{
/*
 * non-local (i.e. shmpage resident) event queues has a different 
 * init approach (see frameserver_shmpage.c) 
 */
	if (!ctx->local){
		return;
	}

	platform_event_init(ctx);
 	arcan_tickofset = arcan_timemillis();
}

extern void platform_device_lock(int lockdev, bool lockstate);
void arcan_device_lock(int lockdev, bool lockstate)
{
    platform_device_lock(lockdev, lockstate);
}
