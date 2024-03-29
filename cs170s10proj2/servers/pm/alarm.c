/* This file deals with the alarm clock related system calls, eventually
 * passing off the work to the functions in timers.c and check_sig() in
 * signal.c to pass an alarm signal to a process.
 *
 * The entry points into this file are:
 *   do_itimer: perform the ITIMER system call
 *   do_alarm: perform the ALARM system call
 *   set_alarm: tell the timer interface to start or stop a process timer
 *   check_vtimer: check if one of the virtual timers needs to be restarted
 */

#include "pm.h"
#include <signal.h>
#include <sys/time.h>
#include <string.h>
#include <minix/com.h>
#include "mproc.h"
#include "param.h"

#define US 1000000	/* shortcut for microseconds per second */

FORWARD _PROTOTYPE( clock_t ticks_from_timeval, (struct timeval *tv) 	);
FORWARD _PROTOTYPE( void timeval_from_ticks, (struct timeval *tv,
			clock_t ticks)					);
FORWARD _PROTOTYPE( int is_sane_timeval, (struct timeval *tv)		);
FORWARD _PROTOTYPE( void getset_vtimer, (struct mproc *mp, int nwhich,
		struct itimerval *value, struct itimerval *ovalue)	);
FORWARD _PROTOTYPE( void get_realtimer, (struct mproc *mp,
					 struct itimerval *value)	);
FORWARD _PROTOTYPE( void set_realtimer, (struct mproc *mp,
					 struct itimerval *value)	);
FORWARD _PROTOTYPE( void cause_sigalrm, (struct timer *tp)		);

/*===========================================================================*
 *				ticks_from_timeval			     * 
 *===========================================================================*/
PRIVATE clock_t ticks_from_timeval(tv)
struct timeval *tv;
{
  clock_t ticks;

  /* Large delays cause a lot of problems.  First, the alarm system call
   * takes an unsigned seconds count and the library has cast it to an int.
   * That probably works, but on return the library will convert "negative"
   * unsigneds to errors.  Presumably no one checks for these errors, so
   * force this call through.  Second, If unsigned and long have the same
   * size, converting from seconds to ticks can easily overflow.  Finally,
   * the kernel has similar overflow bugs adding ticks.
   *
   * Fixing this requires a lot of ugly casts to fit the wrong interface
   * types and to avoid overflow traps.  ALRM_EXP_TIME has the right type
   * (clock_t) although it is declared as long.  How can variables like
   * this be declared properly without combinatorial explosion of message
   * types?
   */
	
  /* In any case, the following conversion must always round up. */

  ticks = (clock_t) (system_hz * (unsigned long) tv->tv_sec);
  if ( (unsigned long) ticks / system_hz != (unsigned long) tv->tv_sec) {
	ticks = LONG_MAX;
  } else {
	ticks += (clock_t)
		((system_hz * (unsigned long) tv->tv_usec + (US-1)) / US);
  }

  if (ticks < 0) ticks = LONG_MAX;

  return(ticks);
}

/*===========================================================================*
 *				timeval_from_ticks			     * 
 *===========================================================================*/
PRIVATE void timeval_from_ticks(tv, ticks)
struct timeval *tv;
clock_t ticks;
{
  tv->tv_sec = (long) (ticks / system_hz);
  tv->tv_usec = (long) ((ticks % system_hz) * US / system_hz);
}

/*===========================================================================*
 *				is_sane_timeval				     * 
 *===========================================================================*/
PRIVATE int is_sane_timeval(tv)
struct timeval *tv;
{
  /* This imposes a reasonable time value range for setitimer. */
  return (tv->tv_sec >= 0 && tv->tv_sec <= MAX_SECS &&
 	  tv->tv_usec >= 0 && tv->tv_usec < US);
}
 
/*===========================================================================*
 *				do_itimer				     *
 *===========================================================================*/
PUBLIC int do_itimer()
{
  struct itimerval ovalue, value;	/* old and new interval timers */
  int setval, getval;			/* set and/or retrieve the values? */
  int r;

  /* Make sure 'which' is one of the defined timers. */
  if (m_in.which_timer < 0 || m_in.which_timer >= NR_ITIMERS)
  	return(EINVAL);

  /* Determine whether to set and/or return the given timer value, based on
   * which of the new_val and old_val parameters are nonzero. At least one of
   * them must be nonzero.
   */
  setval = (m_in.new_val != NULL);
  getval = (m_in.old_val != NULL);

  if (!setval && !getval) return(EINVAL);

  /* If we're setting a new value, copy the new timer from user space.
   * Also, make sure its fields have sane values.
   */
  if (setval) {
  	r = sys_datacopy(who_e, (vir_bytes) m_in.new_val,
  		PM_PROC_NR, (vir_bytes) &value, (phys_bytes) sizeof(value));
  	if (r != OK) return(r);

  	if (!is_sane_timeval(&value.it_value) ||
  	    !is_sane_timeval(&value.it_interval))
  		return(EINVAL);
  }

  switch (m_in.which_timer) {
  	case ITIMER_REAL :
  		if (getval) get_realtimer(mp, &ovalue);

  		if (setval) set_realtimer(mp, &value);

  		r = OK;
  		break;

  	case ITIMER_VIRTUAL :
  	case ITIMER_PROF :
  		getset_vtimer(mp, m_in.which_timer,
  				(setval) ? &value : NULL,
  				(getval) ? &ovalue : NULL);

  		r = OK;
  		break;

	default:
		panic(__FILE__, "invalid timer type", m_in.which_timer);
  }

  /* If requested, copy the old interval timer to user space. */
  if (r == OK && getval) {
  	r = sys_datacopy(PM_PROC_NR, (vir_bytes) &ovalue,
  		who_e, (vir_bytes) m_in.old_val, (phys_bytes) sizeof(ovalue));
  }

  return(r);
}

/*===========================================================================*
 *				do_alarm				     *
 *===========================================================================*/
PUBLIC int do_alarm()
{
  struct itimerval value, ovalue;
  int remaining;		/* previous time left in seconds */

  /* retrieve the old timer value, in seconds (rounded up) */
  get_realtimer(mp, &ovalue);
  
  remaining = ovalue.it_value.tv_sec;
  if (ovalue.it_value.tv_usec > 0) remaining++;

  /* set the new timer value */
  memset(&value, 0, sizeof(value));
  value.it_value.tv_sec = m_in.seconds;

  set_realtimer(mp, &value);

  /* and return the old timer value */
  return(remaining);
}

/*===========================================================================*
 *				getset_vtimer				     * 
 *===========================================================================*/
PRIVATE void getset_vtimer(rmp, which, value, ovalue)
struct mproc *rmp;
int which;
struct itimerval *value;
struct itimerval *ovalue;
{
  clock_t newticks, *nptr;		/* the new timer value, in ticks */
  clock_t oldticks, *optr;		/* the old ticks value, in ticks */
  int r, num;

  /* The default is to provide sys_vtimer with two null pointers, i.e. to do
   * nothing at all.
   */
  optr = nptr = NULL;

  /* If the old timer value is to be retrieved, have 'optr' point to the
   * location where the old value is to be stored, and copy the interval.
   */
  if (ovalue != NULL) {
  	optr = &oldticks;

  	timeval_from_ticks(&ovalue->it_interval, rmp->mp_interval[which]);
  }

  /* If a new timer value is to be set, store the new timer value and have
   * 'nptr' point to it. Also, store the new interval.
   */
  if (value != NULL) {
  	newticks = ticks_from_timeval(&value->it_value);
  	nptr = &newticks;

  	/* If no timer is set, the interval must be zero. */
  	if (newticks <= 0)
  		rmp->mp_interval[which] = 0;
	else 
		rmp->mp_interval[which] =
			ticks_from_timeval(&value->it_interval);
  }

  /* Find out which kernel timer number to use. */
  switch (which) {
  case ITIMER_VIRTUAL: num = VT_VIRTUAL; break;
  case ITIMER_PROF:    num = VT_PROF;    break;
  default:             panic(__FILE__, "invalid vtimer type", which);
  }

  /* Make the kernel call. If requested, also retrieve and store
   * the old timer value.
   */
  if ((r = sys_vtimer(rmp->mp_endpoint, num, nptr, optr)) != OK)
  	panic(__FILE__, "sys_vtimer failed", r);

  if (ovalue != NULL) {
  	/* If the alarm expired already, we should take into account the
  	 * interval. Return zero only if the interval is zero as well.
  	 */
  	if (oldticks <= 0) oldticks = rmp->mp_interval[which];

	timeval_from_ticks(&ovalue->it_value, oldticks);
  }
}

/*===========================================================================*
 *				check_vtimer				     * 
 *===========================================================================*/
PUBLIC void check_vtimer(proc_nr, sig)
int proc_nr;
int sig;
{
  register struct mproc *rmp;
  int which, num;

  rmp = &mproc[proc_nr];

  /* Translate back the given signal to a timer type and kernel number. */
  switch (sig) {
  case SIGVTALRM: which = ITIMER_VIRTUAL; num = VT_VIRTUAL; break;
  case SIGPROF:   which = ITIMER_PROF;    num = VT_PROF;    break;
  default: panic(__FILE__, "invalid vtimer signal", sig);
  }

  /* If a repetition interval was set for this virtual timer, tell the
   * kernel to set a new timeout for the virtual timer.
   */
  if (rmp->mp_interval[which] > 0)
  	sys_vtimer(rmp->mp_endpoint, num, &rmp->mp_interval[which], NULL);
}

/*===========================================================================*
 *				get_realtimer				     * 
 *===========================================================================*/
PRIVATE void get_realtimer(rmp, value)
struct mproc *rmp;
struct itimerval *value;
{
  clock_t exptime;	/* time at which alarm will expire */
  clock_t uptime;	/* current system time */
  clock_t remaining;	/* time left on alarm */
  int s;

  /* First determine remaining time, in ticks, of previous alarm, if set. */
  if (rmp->mp_flags & ALARM_ON) {
  	if ( (s = getuptime(&uptime)) != OK)
  		panic(__FILE__, "get_realtimer couldn't get uptime", s);
  	exptime = *tmr_exp_time(&rmp->mp_timer);

  	remaining = exptime - uptime;

  	/* If the alarm expired already, we should take into account the
  	 * interval. Return zero only if the interval is zero as well.
  	 */
  	if (remaining <= 0) remaining = rmp->mp_interval[ITIMER_REAL];
  } else {
  	remaining = 0;
  }

  /* Convert the result to a timeval structure. */
  timeval_from_ticks(&value->it_value, remaining);

  /* Similarly convert and store the interval of the timer. */
  timeval_from_ticks(&value->it_interval, rmp->mp_interval[ITIMER_REAL]);
}

/*===========================================================================*
 *				set_realtimer				     * 
 *===========================================================================*/
PRIVATE void set_realtimer(rmp, value)
struct mproc *rmp;
struct itimerval *value;
{
  clock_t ticks;	/* New amount of ticks to the next alarm. */
  clock_t interval;	/* New amount of ticks for the alarm's interval. */

  /* Convert the timeval structures in the 'value' structure to ticks. */
  ticks = ticks_from_timeval(&value->it_value);
  interval = ticks_from_timeval(&value->it_interval);

  /* If no timer is set, the interval must be zero. */
  if (ticks <= 0) interval = 0;

  /* Apply these values. */
  set_alarm(rmp, ticks);
  rmp->mp_interval[ITIMER_REAL] = interval;
}

/*===========================================================================*
 *				set_alarm				     * 
 *===========================================================================*/
PUBLIC void set_alarm(rmp, ticks)
struct mproc *rmp;		/* process that wants the alarm */
clock_t ticks;			/* how many ticks delay before the signal */
{
  if (ticks > 0) {
  	pm_set_timer(&rmp->mp_timer, ticks, cause_sigalrm, rmp->mp_endpoint);
	rmp->mp_flags |=  ALARM_ON;
  } else if (rmp->mp_flags & ALARM_ON) {
  	pm_cancel_timer(&rmp->mp_timer);
  	rmp->mp_flags &= ~ALARM_ON;
  }
}

/*===========================================================================*
 *				cause_sigalrm				     * 
 *===========================================================================*/
PRIVATE void cause_sigalrm(tp)
struct timer *tp;
{
  int proc_nr_n;
  register struct mproc *rmp;

  /* get process from timer */
  if(pm_isokendpt(tmr_arg(tp)->ta_int, &proc_nr_n) != OK) {
  	printf("PM: ignoring timer for invalid endpoint %d\n",
  		tmr_arg(tp)->ta_int);
  	return;
  }

  rmp = &mproc[proc_nr_n];

  if ((rmp->mp_flags & (IN_USE | EXITING)) != IN_USE) return;
  if ((rmp->mp_flags & ALARM_ON) == 0) return;

  /* If an interval is set, set a new timer; otherwise clear the ALARM_ON flag.
   * The set_alarm call will be calling pm_set_timer from within this callback
   * from the pm_expire_timers function. This is safe, but we must not use the
   * "tp" structure below this point anymore. */
  if (rmp->mp_interval[ITIMER_REAL] > 0)
	set_alarm(rmp, rmp->mp_interval[ITIMER_REAL]);
  else rmp->mp_flags &= ~ALARM_ON;

  check_sig(rmp->mp_pid, SIGALRM);
}
