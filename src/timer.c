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

static int id = 0;
static struct timeout_q  *Q = NULL;

struct timeout_q {
	struct timeout_q *next;		/* next event */
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
    Q = NULL;
}

void timer_free_all(void)
{
    struct timeout_q *p;

    while (Q) {
	p = Q;
	Q = Q->next;
	free(p);
    }
}


/*
 * elapsed_time seconds have passed; perform all the events that should
 * happen.
 */
void timer_age_queue(time_t elapsed_time)
{
    struct timeout_q *ptr;
    int i = 0;

    for (ptr = Q; ptr; ptr = Q, i++) {
	if (ptr->time > elapsed_time) {
	    ptr->time -= elapsed_time;
	    return;
	}

	elapsed_time -= ptr->time;
	Q = Q->next;
	IF_DEBUG(DEBUG_TIMEOUT)
	    logit(LOG_DEBUG, 0, "about to call timeout %d (#%d)", ptr->id, i);

	if (ptr->func)
	    ptr->func(ptr->data);
	free(ptr);
    }
}

/*
 * Return in how many seconds timer_age_queue() would like to be called.
 * Return -1 if there are no events pending.
 */
int timer_next_delay(void)
{
    IF_DEBUG(DEBUG_TIMEOUT)
	logit(LOG_DEBUG, 0, "%s(): Q: %p", __func__, Q);

    if (!Q)
	return -1;

    IF_DEBUG(DEBUG_TIMEOUT)
	logit(LOG_DEBUG, 0, "%s(): Q->time: %ld", __func__, (long)Q->time);

    if (Q->time < 0) {
	logit(LOG_WARNING, 0, "%s(): top of queue says %ld", __func__, (long)Q->time);
	return 0;
    }

    return Q->time;
}

/*
 * sets the timer
 * @delay:  number of seconds for timeout
 * @action: function to be called on timeout
 * @data:   what to call the timeout function with
 */
int timer_set(time_t delay, cfunc_t action, void *data)
{
    struct timeout_q *ptr, *node, *prev;
    int i = 0;

    /* create a node */
    node = calloc(1, sizeof(struct timeout_q));
    if (!node) {
	logit(LOG_ERR, errno, "Failed allocating memory in %s:%s()", __FILE__, __func__);
	return -1;
    }

    node->func = action;
    node->data = data;
    node->time = delay;
    node->id   = next_id();

    prev = ptr = Q;

    /* insert node in the queue */

    /* if the queue is empty, insert the node and return */
    if (!Q) {
	Q = node;
    } else {
	/* chase the pointer looking for the right place */
	while (ptr) {

	    if (delay < ptr->time) {
		/* right place */

		node->next = ptr;
		if (ptr == Q)
		    Q = node;
		else
		    prev->next = node;
		ptr->time -= node->time;

		print_Q();
		IF_DEBUG(DEBUG_TIMEOUT)
		    logit(LOG_DEBUG, 0, "created timeout %d (#%d)", node->id, i);

		return node->id;
	    } else  {
		/* keep moving */

		delay -= ptr->time; node->time = delay;
		prev = ptr;
		ptr = ptr->next;
	    }
	    i++;
	}
	prev->next = node;
    }

    print_Q();
    IF_DEBUG(DEBUG_TIMEOUT)
	logit(LOG_DEBUG, 0, "created timeout %d (#%d)", node->id, i);

    return node->id;
}

/* returns the time until the timer is scheduled */
int timer_get(int timer_id)
{
    struct timeout_q *ptr;
    time_t left = 0;

    if (!timer_id)
	return -1;

    for (ptr = Q; ptr; ptr = ptr->next) {
	left += ptr->time;
	if (ptr->id == timer_id)
	    return left;
    }

    return -1;
}

/* clears the associated timer.  Returns 1 if succeeded. */
int timer_clear(int timer_id)
{
    struct timeout_q  *ptr, *prev;
    int i = 0;

    if (!timer_id)
	return 0;

    prev = ptr = Q;

    /*
     * find the right node, delete it. the subsequent node's time
     * gets bumped up
     */

    print_Q();
    while (ptr) {
	if (ptr->id == timer_id) {
	    /* got the right node */

	    /* unlink it from the queue */
	    if (ptr == Q)
		Q = Q->next;
	    else
		prev->next = ptr->next;

	    /* increment next node if any */
	    if (ptr->next != 0)
		(ptr->next)->time += ptr->time;

	    if (ptr->data)
		free(ptr->data);

	    IF_DEBUG(DEBUG_TIMEOUT) {
		logit(LOG_DEBUG, 0, "deleted timer %d (#%d)", ptr->id, i);
	    }
	    free(ptr);

	    print_Q();

	    return 1;
	}
	prev = ptr;
	ptr = ptr->next;
	i++;
    }

    print_Q();
    IF_DEBUG(DEBUG_TIMEOUT) {
	logit(LOG_DEBUG, 0, "failed to delete timer %d (#%d)", timer_id, i);
    }

    return 0;
}

/*
 * debugging utility
 */
static void print_Q(void)
{
    struct timeout_q *ptr;

    IF_DEBUG(DEBUG_TIMEOUT) {
	for (ptr = Q; ptr; ptr = ptr->next)
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
