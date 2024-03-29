/* Filter driver - lowest layer - disk driver management */

#include "inc.h"

/* Drivers. */
static struct {
  char *label;
  int minor;
  endpoint_t endpt;

  int problem;		/* one of BD_* */
  int error;		/* one of E*, only relevant if problem>0 */
  int retries;
  int kills;
} driver[2];

/* State variables. */
static endpoint_t self_ep;
static asynmsg_t amsgtable[2];

static int size_known = 0;
static u64_t disk_size;

static int problem_stats[BD_LAST] = { 0 };

/*===========================================================================*
 *				driver_open				     *
 *===========================================================================*/
static int driver_open(int which)
{
	/* Perform an open or close operation on the driver. This is
	 * unfinished code: we should never be doing a blocking sendrec() to
	 * the driver.
	 */
	message msg;
	cp_grant_id_t gid;
	struct partition part;
	sector_t sectors;
	int r;

	msg.m_type = DEV_OPEN;
	msg.DEVICE = driver[which].minor;
	msg.IO_ENDPT = self_ep;
	r = sendrec(driver[which].endpt, &msg);

	if (r != OK) {
		/* Should we restart the driver now? */
		printf("Filter: driver_open: sendrec returned %d\n", r);

		return RET_REDO;
	}

	if(msg.m_type != TASK_REPLY || msg.REP_STATUS != OK) {
		printf("Filter: driver_open: sendrec returned %d, %d\n",
			msg.m_type, msg.REP_STATUS);

		return RET_REDO;
	}

	/* Take the opportunity to retrieve the hard disk size. */
	gid = cpf_grant_direct(driver[which].endpt,
		(vir_bytes) &part, sizeof(part), CPF_WRITE);
	if(!GRANT_VALID(gid))
		panic(__FILE__, "invalid grant", gid);

	msg.m_type = DEV_IOCTL_S;
	msg.REQUEST = DIOCGETP;
	msg.DEVICE = driver[which].minor;
	msg.IO_ENDPT = self_ep;
	msg.IO_GRANT = (char *) gid;

	r = sendrec(driver[which].endpt, &msg);

	cpf_revoke(gid);

	if (r != OK || msg.m_type != TASK_REPLY || msg.REP_STATUS != OK) {
		/* Not sure what to do here, either. */
		printf("Filter: ioctl(DIOCGETP) returned (%d, %d)\n", 
			r, msg.m_type);

		return RET_REDO;
	}

	if(!size_known) {
		disk_size = part.size;
		size_known = 1;
		sectors = div64u(disk_size, SECTOR_SIZE);
		if(cmp64(mul64u(sectors, SECTOR_SIZE), disk_size)) {
			printf("Filter: partition too large\n");

			return RET_REDO;
		}
#if DEBUG
		printf("Filter: partition size: 0x%s / %lu sectors\n",
			print64(disk_size), sectors);
#endif
	} else {
		if(cmp64(disk_size, part.size)) {
			printf("Filter: partition size mismatch (%s != %s)\n",
				print64(part.size), print64(disk_size));

			return RET_REDO;
		}
	}

	return OK;
}

/*===========================================================================*
 *				driver_close				     *
 *===========================================================================*/
static int driver_close(int which)
{
	message msg;
	int r;

	msg.m_type = DEV_CLOSE;
	msg.DEVICE = driver[which].minor;
	msg.IO_ENDPT = self_ep;
	r = sendrec(driver[which].endpt, &msg);

	if (r != OK) {
		/* Should we restart the driver now? */
		printf("Filter: driver_close: sendrec returned %d\n", r);

		return RET_REDO;
	}

	if(msg.m_type != TASK_REPLY || msg.REP_STATUS != OK) {
		printf("Filter: driver_close: sendrec returned %d, %d\n",
			msg.m_type, msg.REP_STATUS);

		return RET_REDO;
	}

	return OK;
}

/*===========================================================================*
 *				driver_init				     *
 *===========================================================================*/
void driver_init(void)
{
	/* Initialize the driver layer. */
	int r;

	self_ep = getprocnr();

	memset(driver, 0, sizeof(driver));

	/* Endpoints unknown. */
	driver[DRIVER_MAIN].endpt = NONE;
	driver[DRIVER_BACKUP].endpt = NONE;

	/* Get disk driver's and this proc's endpoint. */
	driver[DRIVER_MAIN].label = MAIN_LABEL;
	driver[DRIVER_MAIN].minor = MAIN_MINOR;

	r = ds_retrieve_label_num(driver[DRIVER_MAIN].label,
		(u32_t *) &driver[DRIVER_MAIN].endpt);
	if (r != OK) {
		printf("Filter: failed to get main disk driver's endpoint: "
			"%d\n", r);
		bad_driver(DRIVER_MAIN, BD_DEAD, EFAULT);
		check_driver(DRIVER_MAIN);
	}
	else if (driver_open(DRIVER_MAIN) != OK) {
		panic(__FILE__, "unhandled driver_open failure", NO_NUM);
	}

	if(USE_MIRROR) {
		driver[DRIVER_BACKUP].label = BACKUP_LABEL;
		driver[DRIVER_BACKUP].minor = BACKUP_MINOR;

		if(!strcmp(driver[DRIVER_MAIN].label,
				driver[DRIVER_BACKUP].label)) {
			panic(__FILE__, "same driver: not tested", NO_NUM);
		}

		r = ds_retrieve_label_num(driver[DRIVER_BACKUP].label,
			(u32_t *) &driver[DRIVER_BACKUP].endpt);
		if (r != OK) {
			printf("Filter: failed to get backup disk driver's "
				"endpoint: %d\n", r);
			bad_driver(DRIVER_BACKUP, BD_DEAD, EFAULT);
			check_driver(DRIVER_BACKUP);
		}
		else if (driver_open(DRIVER_BACKUP) != OK) {
			panic(__FILE__, "unhandled driver_open failure",
				NO_NUM);
		}
	}
}

/*===========================================================================*
 *				driver_shutdown				     *
 *===========================================================================*/
void driver_shutdown(void)
{
	/* Clean up. */

#if DEBUG
	printf("Filter: %u driver deaths, %u protocol errors, "
		"%u data errors\n", problem_stats[BD_DEAD],
		problem_stats[BD_PROTO], problem_stats[BD_DATA]);
#endif

	if(driver_close(DRIVER_MAIN) != OK)
		printf("Filter: DEV_CLOSE failed on shutdown (1)\n");

	if(USE_MIRROR)
		if(driver_close(DRIVER_BACKUP) != OK)
			printf("Filter: DEV_CLOSE failed on shutdown (2)\n");
}

/*===========================================================================*
 *				get_raw_size				     *
 *===========================================================================*/
u64_t get_raw_size(void)
{
	/* Return the size of the raw disks as used by the filter driver.
	 */

	return disk_size;
}

/*===========================================================================*
 *				reset_kills				     *
 *===========================================================================*/
void reset_kills(void)
{
	/* Reset kill and retry statistics. */
	driver[DRIVER_MAIN].kills = 0;
	driver[DRIVER_MAIN].retries = 0;
	driver[DRIVER_BACKUP].kills = 0;
	driver[DRIVER_BACKUP].retries = 0;
}

/*===========================================================================*
 *				bad_driver				     *
 *===========================================================================*/
int bad_driver(int which, int type, int error)
{
	/* A disk driver has died or produced an error. Mark it so that we can
	 * deal with it later, and return RET_REDO to indicate that the
	 * current operation is to be retried. Also store an error code to
	 * return to the user if the situation is unrecoverable.
	 */
	driver[which].problem = type;
	driver[which].error = error;

	return RET_REDO;
}

/*===========================================================================*
 *				new_driver_ep				     *
 *===========================================================================*/
static int new_driver_ep(int which)
{
	/* See if a new driver instance has already been started for the given
	 * driver, by retrieving its entry from DS.
	 */
	int r;
	endpoint_t endpt;

	r = ds_retrieve_label_num(driver[which].label, (u32_t *) &endpt);

	if (r != OK) {
		printf("Filter: DS query for %s failed\n",
			driver[which].label);

		return 0;
	}

	if (endpt == driver[which].endpt) {
#if DEBUG
		printf("Filter: same endpoint for %s\n", driver[which].label);
#endif
		return 0;
	}

#if DEBUG
	printf("Filter: new enpdoint for %s: %d -> %d\n", driver[which].label,
		driver[which].endpt, endpt);
#endif

	driver[which].endpt = endpt;

	return 1;
}

/*===========================================================================*
 *				check_problem				     *
 *===========================================================================*/
static int check_problem(int which, int problem, int retries, int *tell_rs)
{
	/* A problem has occurred with a driver. Update statistics, and decide
	 * what to do. If EAGAIN is returned, the driver should be restarted;
	 * any other result will be passed up.
	 */

#if DEBUG
	printf("Filter: check_driver processing driver %d, problem %d\n",
		which, problem);
#endif

	problem_stats[problem]++;

	if(new_driver_ep(which)) {
#if DEBUG
		printf("Filter: check_problem: noticed a new driver\n");
#endif

		if(driver_open(which) == OK) {
#if DEBUG2
			printf("Filter: open OK -> no recovery\n");
#endif
			return OK;
		} else {
#if DEBUG2
			printf("Filter: open not OK -> recovery\n");
#endif
			problem = BD_PROTO;
			problem_stats[problem]++;
		}
	}

	/* If the driver has died, we always need to restart it. If it has
	 * been giving problems, we first retry the request, up to N times,
	 * after which we kill and restart the driver. We restart the driver
	 * up to M times, after which we remove the driver from the mirror
	 * configuration. If we are not set up to do mirroring, we can only
	 * do one thing, and that is continue to limp along with the bad
	 * driver..
	 */
	switch(problem) {
	case BD_PROTO:
	case BD_DATA:
		driver[which].retries++;

#if DEBUG
		printf("Filter: disk driver %d has had "
			"%d/%d retry attempts, %d/%d kills\n", which, 
			driver[which].retries, NR_RETRIES,
			driver[which].kills, NR_RESTARTS);
#endif

		if (driver[which].retries < NR_RETRIES) {
			if(retries == 1) {
#if DEBUG
				printf("Filter: not restarting; retrying "
					"(retries %d/%d, kills %d/%d)\n",
					driver[which].retries, NR_RETRIES,
					driver[which].kills, NR_RESTARTS);
#endif
				return OK;
			}
#if DEBUG
			printf("Filter: restarting (retries %d/%d, "
				"kills %d/%d, internal retry %d)\n",
				driver[which].retries, NR_RETRIES,
				driver[which].kills, NR_RESTARTS, retries);
#endif
		}

#if DEBUG
		printf("Filter: disk driver %d has reached error "
			"threshold, restarting driver\n", which);
#endif

		*tell_rs = 1;
		break;

	case BD_DEAD:
		/* Can't kill that which is already dead.. */
		*tell_rs = 0;
		break;

	default:
		panic(__FILE__, "invalid problem", problem);
	}

	/* At this point, the driver will be restarted. */
	driver[which].retries = 0;
	driver[which].kills++;

	if (driver[which].kills < NR_RESTARTS)
		return EAGAIN;

	/* We've reached the maximum number of restarts for this driver. */
	if (USE_MIRROR) {
		printf("Filter: kill threshold reached, disabling mirroring\n");

		USE_MIRROR = 0;

		if (which == DRIVER_MAIN) {
			driver[DRIVER_MAIN] = driver[DRIVER_BACKUP];

			/* This is not necessary. */
			strcpy(MAIN_LABEL, BACKUP_LABEL);
			MAIN_MINOR = BACKUP_MINOR;
		}

		driver[DRIVER_BACKUP].endpt = NONE;

		return OK;
	}
	else {
		/* We tried, we really did. But now we give up. Tell the user.
		 */
		printf("Filter: kill threshold reached, returning error\n");

		if (driver[which].error == EAGAIN) return EIO;

		return driver[which].error;
	}
}

/*===========================================================================*
 *				restart_driver				     *
 *===========================================================================*/
static void restart_driver(int which, int tell_rs)
{
	/* Restart the given driver. Block until the new instance is up.
	 */
	message msg;
	endpoint_t endpt;
	int r, w = 0;

	if (tell_rs) {
		/* Tell RS to refresh or restart the driver */
		msg.m_type = RS_REFRESH;
		msg.RS_CMD_ADDR = driver[which].label;
		msg.RS_CMD_LEN = strlen(driver[which].label);

#if DEBUG
		printf("Filter: asking RS to refresh %s..\n",
			driver[which].label);
#endif

		r = sendrec(RS_PROC_NR, &msg);

		if (r != OK || msg.m_type != OK)
			panic(__FILE__, "RS request failed", r);

#if DEBUG
		printf("Filter: RS call succeeded\n");
#endif
	}

	/* Wait until the new driver instance is up, and get its endpoint. */
#if DEBUG
	printf("Filter: endpoint update driver %d; old endpoint %d\n",
		which, driver[which].endpt);
#endif

	do {
		if(w) flt_sleep(1);
		w = 1;

		r = ds_retrieve_label_num(driver[which].label,
			(u32_t *) &endpt);

#if DEBUG2
		if (r != OK)
			printf("Filter: DS request failed (%d)\n", r);
		else if (endpt == driver[which].endpt)
			printf("Filter: DS returned same endpoint\n");
		else
			printf("Filter: DS request OK, new endpoint\n");
#endif
	} while (r != OK || endpt == driver[which].endpt);

	driver[which].endpt = endpt;
}

/*===========================================================================*
 *				check_driver				     *
 *===========================================================================*/
int check_driver(int which)
{
	/* See if the given driver has been troublesome, and if so, deal with
	 * it.
	 */
	int problem, tell_rs;
	int r, retries = 0;

	problem = driver[which].problem;

	if (problem == BD_NONE)
		return OK;

	do {
		if(retries) {
#if DEBUG
			printf("Filter: check_driver: retry number %d\n",
				retries);
#endif
			problem = BD_PROTO;
		}
		retries++;
		driver[which].problem = BD_NONE;

		/* Decide what to do: continue operation, restart the driver,
		 * or return an error.
		 */
		r = check_problem(which, problem, retries, &tell_rs);
		if (r != EAGAIN)
			return r;

		/* Restarting the driver it is. First tell RS (if necessary),
		 * then wait for the new driver instance to come up.
		 */
		restart_driver(which, tell_rs);

		/* Finally, open the device on the new driver */
	} while (driver_open(which) != OK);

#if DEBUG
	printf("Filter: check_driver restarted driver %d, endpoint %d\n",
		which, driver[which].endpt);
#endif

	return OK;
}

/*===========================================================================*
 *				flt_senda				     *
 *===========================================================================*/
static int flt_senda(message *mess, int which)
{
	/* Send a message to one driver. Can only return OK at the moment. */
	int r;
	asynmsg_t *amp;

	/* Fill in the last bits of the message. */
	mess->DEVICE = driver[which].minor;
	mess->IO_ENDPT = self_ep;

	/* Send the message asynchronously. */
	amp = &amsgtable[which];
	amp->dst = driver[which].endpt;
	amp->msg = *mess;
	amp->flags = AMF_VALID;
	r = senda(amsgtable, 2);

	if(r != OK)
		panic(__FILE__, "senda returned error", r);

	return r;
}

/*===========================================================================*
 *				check_senda				     *
 *===========================================================================*/
static int check_senda(int which)
{
	/* Check whether an earlier senda resulted in an error indicating the
	 * message never got delivered. Only in that case can we reliably say
	 * that the driver died. Return BD_DEAD in this case, and BD_PROTO
	 * otherwise.
	 */
	asynmsg_t *amp;

	amp = &amsgtable[which];

	if ((amp->flags & AMF_DONE) &&
		(amp->result == EDEADSRCDST || amp->result == EDSTDIED)) {

		return BD_DEAD;
	}

	return BD_PROTO;
}

/*===========================================================================*
 *				flt_receive				     *
 *===========================================================================*/
static int flt_receive(message *mess, int which)
{
	/* Receive a message from one or either driver, unless a timeout
	 * occurs. Can only return OK or RET_REDO.
	 */
	int r;

	for (;;) {
		r = sef_receive(ANY, mess);
		if(r != OK)
			panic(__FILE__, "sef_receive returned error", r);

		if(mess->m_source == CLOCK && is_notify(mess->m_type)) {
			if (mess->NOTIFY_TIMESTAMP < flt_alarm(-1)) {
#if DEBUG
				printf("Filter: SKIPPING old alarm "
					"notification\n");
#endif
				continue;
			}

#if DEBUG
			printf("Filter: timeout waiting for disk driver %d "
				"reply!\n", which);
#endif

			/* If we're waiting for either driver,
		 	 * both are at fault.
		 	 */
			if (which < 0) {
				bad_driver(DRIVER_MAIN,
					check_senda(DRIVER_MAIN), EFAULT);

				return bad_driver(DRIVER_BACKUP,
					check_senda(DRIVER_BACKUP), EFAULT);
			}

			/* Otherwise, just report the one not replying as dead.
			 */
			return bad_driver(which, check_senda(which), EFAULT);
		}

		if (mess->m_source != driver[DRIVER_MAIN].endpt &&
				mess->m_source != driver[DRIVER_BACKUP].endpt) {
#if DEBUG
			printf("Filter: got STRAY message %d from %d\n",
				mess->m_type, mess->m_source);
#endif

			continue;
		}

		/* We are waiting for a reply from one specific driver. */
		if (which >= 0) {
			/* If the message source is that driver, good. */
			if (mess->m_source == driver[which].endpt)
				break;

			/* This should probably be treated as a real protocol
			 * error. We do not abort any receives (not even paired
			 * receives) except because of timeouts. Getting here
			 * means a driver replied at least the timeout period
			 * later than expected, which should be enough reason
			 * to kill it really. The other explanation is that it
			 * is actually violating the protocol and sending bogus
			 * messages...
			 */
#if DEBUG
			printf("Filter: got UNEXPECTED reply from %d\n",
				mess->m_source);
#endif

			continue;
		}

		/* We got a message from one of the drivers, and we didn't
		 * care which one we wanted to receive from. A-OK.
		 */
		break;
	}

	return OK;
}

/*===========================================================================*
 *				flt_sendrec				     *
 *===========================================================================*/
static int flt_sendrec(message *mess, int which)
{
	int r;

	r = flt_senda(mess, which);
	if(r != OK)
		return r;

	if(check_senda(which) == BD_DEAD) {
		return bad_driver(which, BD_DEAD, EFAULT);
	}

	/* Set alarm. */
	flt_alarm(DRIVER_TIMEOUT);

	r = flt_receive(mess, which);

	/* Clear the alarm. */
	flt_alarm(0);
	return r;
}

/*===========================================================================*
 *				do_sendrec_both				     *
 *===========================================================================*/
static int do_sendrec_both(message *m1, message *m2)
{
	/* If USEE_MIRROR is set, call flt_sendrec() to both drivers.
	 * Otherwise, only call flt_sendrec() to the main driver.
	 * This function will only return either OK or RET_REDO.
	 */
	int r, which = -1;
	message ma, mb;

	/* If the two disks use the same driver, call flt_sendrec() twice
	 * sequentially. Such a setup is not very useful though.
	 */
	if (!strcmp(driver[DRIVER_MAIN].label, driver[DRIVER_BACKUP].label)) {
		if ((r = flt_sendrec(m1, DRIVER_MAIN)) != OK) return r;
		return flt_sendrec(m2, DRIVER_BACKUP);
	}

	/* If the two disks use different drivers, call flt_senda()
	 * twice, and then flt_receive(), and distinguish the return
	 * messages by means of m_source.
	 */
	if ((r = flt_senda(m1, DRIVER_MAIN)) != OK) return r;
	if ((r = flt_senda(m2, DRIVER_BACKUP)) != OK) return r;

	/* Set alarm. */
	flt_alarm(DRIVER_TIMEOUT);

	/* The message received by the 1st flt_receive() may not be
	 * from DRIVER_MAIN.
	 */
	if ((r = flt_receive(&ma, -1)) != OK) {
		flt_alarm(0);
		return r;
	}

	if (ma.m_source == driver[DRIVER_MAIN].endpt) {
		which = DRIVER_BACKUP;
	} else if (ma.m_source == driver[DRIVER_BACKUP].endpt) {
		which = DRIVER_MAIN;
	} else {
		panic(__FILE__, "message from unexpected source",
			ma.m_source);
	}

	r = flt_receive(&mb, which);

	/* Clear the alarm. */
	flt_alarm(0);

	if(r != OK)
		return r;

	if (ma.m_source == driver[DRIVER_MAIN].endpt) {
		*m1 = ma;
		*m2 = mb;
	} else {
		*m1 = mb;
		*m2 = ma;
	}

	return OK;
}

/*===========================================================================*
 *				do_sendrec_one				     *
 *===========================================================================*/
static int do_sendrec_one(message *m1, message *m2)
{
	/* Only talk to the main driver. If something goes wrong, it will
	 * be fixed elsewhere.
	 * This function will only return either OK or RET_REDO.
	 */

    	return flt_sendrec(m1, DRIVER_MAIN);
}

/*===========================================================================*
 *				paired_sendrec				     *
 *===========================================================================*/
static int paired_sendrec(message *m1, message *m2, int both)
{
	/* Sendrec with the disk driver. If the disk driver is down, and was
	 * restarted, redo the request, until the driver works fine, or can't
	 * be restarted again.
	 */
	int r;

#if DEBUG2
	printf("paired_sendrec(%d) - <%d,%x:%x,%d> - %x,%x\n",
		both, m1->m_type, m1->HIGHPOS, m1->POSITION, m1->COUNT,
		m1->IO_GRANT, m2->IO_GRANT);
#endif

	if (both)
		r = do_sendrec_both(m1, m2);
	else
		r = do_sendrec_one(m1, m2);

#if DEBUG2
	if (r != OK)
		printf("paired_sendrec about to return %d\n", r);
#endif

	return r;
}

/*===========================================================================*
 *				single_grant				     *
 *===========================================================================*/
static int single_grant(endpoint_t endpt, vir_bytes buf, int access,
	cp_grant_id_t *gid, iovec_s_t vector[NR_IOREQS], size_t *sizep)
{
	/* Create grants for a vectored request to a single driver.
	 */
	cp_grant_id_t grant;
	size_t size, chunk;
	int count;

	size = *sizep;

	/* Split up the request into chunks, if requested. This makes no
	 * difference at all, except that this works around a weird performance
	 * bug with large DMA PRDs on some machines.
	 */
	if (CHUNK_SIZE > 0) chunk = CHUNK_SIZE;
	else chunk = size;

	/* Fill in the vector, creating a grant for each item. */
	for (count = 0; size > 0 && count < NR_IOREQS; count++) {
		/* The last chunk will contain all the remaining data. */
		if (chunk > size || count == NR_IOREQS - 1)
			chunk = size;

		grant = cpf_grant_direct(endpt, buf, chunk, access);
		if (!GRANT_VALID(grant))
			panic(__FILE__, "invalid grant", grant);

		vector[count].iov_grant = grant;
		vector[count].iov_size = chunk;

		buf += chunk;
		size -= chunk;
	}

	/* Then create a grant for the vector itself. */
	*gid = cpf_grant_direct(endpt, (vir_bytes) vector,
		sizeof(vector[0]) * count, CPF_READ | CPF_WRITE);

	if (!GRANT_VALID(*gid))
		panic(__FILE__, "invalid grant", *gid);

	return count;
}

/*===========================================================================*
 *				paired_grant				     *
 *===========================================================================*/
static int paired_grant(char *buf1, char *buf2, int request,
	cp_grant_id_t *gids, iovec_s_t vectors[2][NR_IOREQS], size_t *sizes,
	int both)
{
	/* Create memory grants, either to one or to both drivers.
	 */
	int count, access;

	count = 0;
	access = (request == FLT_WRITE) ? CPF_READ : CPF_WRITE;

	if(driver[DRIVER_MAIN].endpt > 0) {
		count = single_grant(driver[DRIVER_MAIN].endpt,
			(vir_bytes) buf1, access, &gids[0], vectors[0],
			&sizes[0]);
	}

	if (both) {
		if(driver[DRIVER_BACKUP].endpt > 0) {
			count = single_grant(driver[DRIVER_BACKUP].endpt,
				(vir_bytes) buf2, access, &gids[1],
				vectors[1], &sizes[1]);
		}
	}
        return count;
}

/*===========================================================================*
 *				single_revoke				     *
 *===========================================================================*/
void single_revoke(cp_grant_id_t gid, iovec_s_t vector[NR_IOREQS],
	size_t *sizep, int count)
{
	/* Revoke all grants associated with a request to a single driver.
	 * Modify the given size to reflect the actual I/O performed.
	 */
	int i;

	/* Revoke the grants for all the elements of the vector. */
	for (i = 0; i < count; i++) {
		cpf_revoke(vector[i].iov_grant);
		*sizep -= vector[i].iov_size;
	}

	/* Then revoke the grant for the vector itself. */
	cpf_revoke(gid);
}

/*===========================================================================*
 *				paired_revoke				     *
 *===========================================================================*/
static void paired_revoke(cp_grant_id_t *gids, iovec_s_t vectors[2][NR_IOREQS],
	size_t *sizes, int count, int both)
{
	/* Revoke grants to drivers for a single request.
	 */

	single_revoke(gids[0], vectors[0], &sizes[0], count);

	if (both)
		single_revoke(gids[1], vectors[1], &sizes[1], count);
}

/*===========================================================================*
 *				read_write				     *
 *===========================================================================*/
int read_write(u64_t pos, char *bufa, char *bufb, size_t *sizep, int request)
{
	iovec_s_t vectors[2][NR_IOREQS];
	message m1, m2;
	cp_grant_id_t gids[2];
	size_t sizes[2];
	int r, both, count;

	gids[0] = gids[1] = GRANT_INVALID;
	sizes[0] = sizes[1] = *sizep;

	/* Send two requests only if mirroring is enabled and the given request
	 * is either FLT_READ2 or FLT_WRITE.
	 */
	both = (USE_MIRROR && request != FLT_READ);

	count = paired_grant(bufa, bufb, request, gids, vectors, sizes, both);

	m1.m_type = (request == FLT_WRITE) ? DEV_SCATTER_S : DEV_GATHER_S;
	m1.COUNT = count;
	m1.POSITION = ex64lo(pos);
	m1.HIGHPOS = ex64hi(pos);

	m2 = m1;

	m1.IO_GRANT = (char *) gids[0];
	m2.IO_GRANT = (char *) gids[1];

	r = paired_sendrec(&m1, &m2, both);

	paired_revoke(gids, vectors, sizes, count, both);

	if(r != OK) {
#if DEBUG
		if (r != RET_REDO)
			printf("Filter: paired_sendrec returned %d\n", r);
#endif
		return r;
	}

	if (m1.m_type != TASK_REPLY || m1.REP_STATUS != OK) {
		printf("Filter: unexpected/invalid reply from main driver: "
			"(%x, %d)\n", m1.m_type, m1.REP_STATUS);

		return bad_driver(DRIVER_MAIN, BD_PROTO,
			(m1.m_type == TASK_REPLY) ? m1.REP_STATUS : EFAULT);
	}

	if (sizes[0] != *sizep) {
		printf("Filter: truncated reply from main driver\n");

		/* If the driver returned a value *larger* than we requested,
		 * OR if we did NOT exceed the disk size, then we should
		 * report the driver for acting strangely!
		 */
		if (sizes[0] < 0 || sizes[0] > *sizep ||
			cmp64(add64u(pos, sizes[0]), disk_size) < 0)
			return bad_driver(DRIVER_MAIN, BD_PROTO, EFAULT);

		/* Return the actual size. */
		*sizep = sizes[0];
	}

	if (both) {
		if (m2.m_type != TASK_REPLY || m2.REP_STATUS != OK) {
			printf("Filter: unexpected/invalid reply from "
				"backup driver (%x, %d)\n",
				m2.m_type, m2.REP_STATUS);

			return bad_driver(DRIVER_BACKUP, BD_PROTO,
				m2.m_type == TASK_REPLY ? m2.REP_STATUS :
				EFAULT);
		}
		if (sizes[1] != *sizep) {
			printf("Filter: truncated reply from backup driver\n");

			/* As above */
			if (sizes[1] < 0 || sizes[1] > *sizep ||
				cmp64(add64u(pos, sizes[1]), disk_size) < 0)
				return bad_driver(DRIVER_BACKUP, BD_PROTO,
					EFAULT);

			/* Return the actual size. */
			if (*sizep >= sizes[1])
				*sizep = sizes[1];
		}
	}

	return OK;
}
