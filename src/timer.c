/* callout queue implementation
 *
 * The mrouted program is covered by the license in the accompanying file
 * named "LICENSE".  Use of the mrouted program represents acceptance of
 * the terms and conditions listed in that file.
 *
 * The mrouted program is COPYRIGHT 1989 by The Board of Trustees of
 * Leland Stanford Junior University.
 */

#include "defs.h"
#include "queue.h"

static int id = 0;
static TAILQ_HEAD(tmr_head,tmr) tl;

struct tmr_head headp;
struct tmr {
	TAILQ_ENTRY(tmr) link;
	int	   	 id;
	cfunc_t          func;	  	/* function to call */
	void	   	 *data;		/* func's data */
	time_t	       	 time;		/* time offset to next event*/
};

static void print_Q(void);


/* Get next free (non-zero) ID
 *
 * ID is a counter that wraps to zero, which is reserved.
 * The range of counters IDs is big so we should be OK.
 */
static int next_id(void)
{
    id++;
    if (id <= 0)
	id = 1;

    return id;
}

void timer_init(void)
{
    TAILQ_INIT(&tl);
}

void timer_free_all(void)
{
    struct tmr *ptr, *tmp;

    TAILQ_FOREACH_SAFE(ptr, &tl, link, tmp) {
	TAILQ_REMOVE(&tl, ptr, link);
	free(ptr);
    }
}


/*
 * elapsed_time seconds have passed; perform all the events that should
 * happen.
 */
void timer_age_queue(time_t elapsed_time)
{
    struct tmr *ptr, *tmp;
    int i = 0;

    TAILQ_FOREACH_SAFE(ptr, &tl, link, tmp) {
	if (ptr->time > elapsed_time) {
	    ptr->time -= elapsed_time;
	    return;
	}

	elapsed_time -= ptr->time;

	IF_DEBUG(DEBUG_TIMEOUT)
	    logit(LOG_DEBUG, 0, "about to call timeout %d (#%d)", ptr->id, i);

	if (ptr->func)
	    ptr->func(ptr->data);

	TAILQ_REMOVE(&tl, ptr, link);
	free(ptr);
	i++;
    }
}

/*
 * Return in how many seconds timer_age_queue() would like to be called.
 * Return -1 if there are no events pending.
 */
int timer_next_delay(void)
{
    struct tmr *ptr = TAILQ_FIRST(&tl);

    IF_DEBUG(DEBUG_TIMEOUT)
	logit(LOG_DEBUG, 0, "%s(): tl: %sempty", __func__, !ptr ? "" : "not ");

    if (!ptr)
	return -1;

    IF_DEBUG(DEBUG_TIMEOUT)
	logit(LOG_DEBUG, 0, "%s(): first timer: %ld sec", __func__, (long)ptr->time);

    if (ptr->time < 0) {
	logit(LOG_WARNING, 0, "%s(): top of queue says %ld", __func__, (long)ptr->time);
	return 0;
    }

    return ptr->time;
}

/*
 * sets the timer
 * @delay:  number of seconds for timeout
 * @action: function to be called on timeout
 * @data:   what to call the timeout function with
 */
int timer_set(time_t delay, cfunc_t action, void *data)
{
    struct tmr *node;
    int i = 0;

    /* create a node */
    node = calloc(1, sizeof(struct tmr));
    if (!node) {
	logit(LOG_ERR, errno, "Failed allocating memory in %s:%s()", __FILE__, __func__);
	return -1;
    }

    node->func = action;
    node->data = data;
    node->time = delay;
    node->id   = next_id();

    /* insert node in the queue */

    /* if the queue is empty, insert the node and return */
    if (TAILQ_EMPTY(&tl)) {
	TAILQ_INSERT_HEAD(&tl, node, link);
    } else {
	struct tmr *ptr;

	/* chase the pointer looking for the right place */
	TAILQ_FOREACH(ptr, &tl, link) {
	    if (delay < ptr->time) {
		/* right place */
		TAILQ_INSERT_BEFORE(ptr, node, link);

		/* adjust current ptr for timer we just added */
		ptr->time -= node->time;

		print_Q();
		IF_DEBUG(DEBUG_TIMEOUT)
		    logit(LOG_DEBUG, 0, "created timeout %d (#%d)", node->id, i);

		return node->id;
	    }

	    /* adjust new node time for each ptr we traverse */
	    delay -= ptr->time;
	    node->time = delay;

	    i++;
	}

	TAILQ_INSERT_TAIL(&tl, node, link);
    }

    print_Q();
    IF_DEBUG(DEBUG_TIMEOUT)
	logit(LOG_DEBUG, 0, "created timeout %d (#%d)", node->id, i);

    return node->id;
}

/* returns the time until the timer is scheduled */
int timer_get(int timer_id)
{
    struct tmr *ptr;
    time_t left = 0;

    if (!timer_id)
	return -1;

    TAILQ_FOREACH(ptr, &tl, link) {
	left += ptr->time;
	if (ptr->id == timer_id)
	    return left;
    }

    return -1;
}

/* clear the associated timer */
void timer_clear(int timer_id)
{
    struct tmr *ptr, *next;
    int i = 0;

    if (!timer_id)
	return;

    /*
     * find the right node, delete it. the subsequent node's time
     * gets bumped up
     */

    print_Q();
    TAILQ_FOREACH(ptr, &tl, link) {
	if (ptr->id == timer_id)
	    break;

	i++;
    }

    if (!ptr) {
	print_Q();
	IF_DEBUG(DEBUG_TIMEOUT)
	    logit(LOG_DEBUG, 0, "failed to delete timer %d (#%d)", timer_id, i);
	return;
    }

    /* Found it, now unlink it from the queue */
    TAILQ_REMOVE(&tl, ptr, link);

    /* increment next node if any */
    next = TAILQ_NEXT(ptr, link);
    if (next)
	next->time += ptr->time;

    if (ptr->data)
	free(ptr->data);

    IF_DEBUG(DEBUG_TIMEOUT)
	logit(LOG_DEBUG, 0, "deleted timer %d (#%d)", ptr->id, i);
    free(ptr);

    print_Q();
}

/*
 * debugging utility
 */
static void print_Q(void)
{
    struct tmr *ptr;

    IF_DEBUG(DEBUG_TIMEOUT) {
	TAILQ_FOREACH(ptr, &tl, link)
	    logit(LOG_DEBUG, 0, "(%d,%ld) ", ptr->id, (long)ptr->time);
    }
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "ellemtel"
 *  c-basic-offset: 4
 * End:
 */
