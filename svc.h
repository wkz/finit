/* Low-level service primitives and generic API for managing svc_t structures
 *
 * Copyright (c) 2008-2010  Claudio Matsuoka <cmatsuoka@gmail.com>
 * Copyright (c) 2008-2015  Joachim Nilsson <troglobit@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef FINIT_SVC_H_
#define FINIT_SVC_H_

#include <sys/ipc.h>		/* IPC_CREAT */
#include <sys/shm.h>		/* shmat() */
#include <sys/types.h>		/* pid_t */
#include "libite/lite.h"
#include "inetd.h"
#include "helpers.h"

typedef enum {
	SVC_STOP = 0,		/* Disabled */
	SVC_START,		/* Enabled */
	SVC_RELOAD		/* Enabled, needs restart */
} svc_cmd_t;

typedef enum {
	SVC_TYPE_FREE = 0,	/* Free to allocate */
	SVC_TYPE_SERVICE,	/* Monitored, will be respawned */
	SVC_TYPE_TASK,		/* One-shot, runs in parallell */
	SVC_TYPE_RUN,		/* Like task, but wait for completion */
	SVC_TYPE_INETD		/* Classic inetd service */
} svc_type_t;

typedef enum {
	SVC_HALTED_STATE = 0,	/* Not allowed in runlevel, or not enabled. */
	SVC_WAITING_STATE,	/* Waiting for connection (inetd service)   */
	SVC_PAUSED_STATE,	/* Stopped/Paused by user started on reload */
	SVC_CONDHALT_STATE,	/* Not allowed to run atm. event/state lost */
	SVC_RESTART_STATE,	/* Restarting service waiting to be stopped */
	SVC_RELOAD_STATE,	/* Reloading services, after .conf changed  */
	SVC_RUNNING_STATE,	/* Currently running service, see svc->pid  */
} svc_state_t;

#define FINIT_SHM_ID     0x494E4954  /* "INIT", see ascii(7) */
#define MAX_ARG_LEN      64
#define MAX_STR_LEN      64
#define MAX_USER_LEN     16
#define MAX_NUM_FDS      64	     /* Max number of I/O plugins */
#define MAX_NUM_SVC      64	     /* Enough? */
#define MAX_NUM_SVC_ARGS 32

/* Default enable for all services, can be stopped by means
 * of issuing an initctl call. E.g.
 *   initctl <stop|start|restart> service */
typedef struct svc {
	/* Instance specifics */
	int            job, id;	       /* JOB:ID */

	/* Service details */
	pid_t	       pid;
	svc_state_t    state;	       /* Paused, Reloading, Restart, Running, ... */
	svc_type_t     type;
	time_t	       mtime;	       /* Modification time for .conf from /etc/finit.d/ */
	int            dirty;	       /* Set if old mtime != new mtime  => reloaded,
					* or -1 when marked for removal */
	int	       runlevels;
	int            sighup;	       /* This service supports SIGHUP :) */
	char           cond[MAX_ARG_LEN];

	/* Incremented for each restart by service monitor. */
	unsigned int   restart_counter;

	/* For inetd services */
	inetd_t        inetd;

	/* Identity */
	char	       username[MAX_USER_LEN];
	char	       group[MAX_USER_LEN];

	/* Command, arguments and service description */
	char	       cmd[MAX_ARG_LEN];
	char	       args[MAX_NUM_SVC_ARGS][MAX_ARG_LEN];
	char	       desc[MAX_STR_LEN];

	/* For external plugins. If @cb is set, a plugin is loaded.
	 * @dynamic:	  Set by plugins that want dynamic events.
	 * @dynamic_stop: Set by plugins that allow dyn. events to stop it as well.
	 * @private:	  Can be used freely by plugin, e.g., to store "states".
	 */
	int	       dynamic;
	int	       dynamic_stop;
	int	       private;
	svc_cmd_t    (*cb)(struct svc *svc, int event, void *event_arg);
} svc_t;

typedef struct svc_map svc_map_t;

/* Put services array in shm */
static inline svc_t *finit_svc_connect(void)
{
	static void *ptr = (void *)-1;

	if ((void *)-1 == ptr) {
		ptr = shmat(shmget(FINIT_SHM_ID, sizeof(svc_t) * MAX_NUM_SVC,
				   0600 | IPC_CREAT), NULL, 0);
		if ((void *)-1 == ptr)
			return NULL;
	}

	return (svc_t *)ptr;
}

svc_t    *svc_new              (char *cmd, int id, int type);
int	  svc_del	       (svc_t *svc);

svc_t	 *svc_find	       (char *cmd, int id);
svc_t	 *svc_find_by_pid      (pid_t pid);
svc_t	 *svc_find_by_jobid    (int job, int id);
svc_t	 *svc_find_by_nameid   (char *name, int id);

svc_t	 *svc_iterator	       (int first);
svc_t	 *svc_inetd_iterator   (int first);
svc_t	 *svc_dynamic_iterator (int first);
svc_t	 *svc_named_iterator   (int first, char *cmd);
svc_t    *svc_job_iterator     (int first, int job);

void	  svc_foreach	       (void (*cb)(svc_t *));
void	  svc_foreach_dynamic  (void (*cb)(svc_t *));

void	  svc_mark_dynamic     (void);
void	  svc_check_dirty      (svc_t *svc, time_t mtime);
void	  svc_clean_dynamic    (void (*cb)(svc_t *));
int	  svc_clean_bootstrap  (svc_t *svc);

char     *svc_status           (svc_t *svc);
int       svc_next_id          (char *cmd);
int       svc_is_unique        (svc_t *svc);

static inline int svc_in_runlevel(svc_t *svc, int runlevel) { return svc && ISSET(svc->runlevels, runlevel); }

static inline int svc_is_dynamic(svc_t *svc) { return svc &&  0 != svc->mtime; }
static inline int svc_is_removed(svc_t *svc) { return svc && -1 == svc->dirty; }
static inline int svc_is_changed(svc_t *svc) { return svc &&  0 != svc->dirty; }
static inline int svc_is_updated(svc_t *svc) { return svc &&  1 == svc->dirty; }

static inline int svc_is_inetd  (svc_t *svc) { return svc && SVC_TYPE_INETD   == svc->type; }
static inline int svc_is_daemon (svc_t *svc) { return svc && SVC_TYPE_SERVICE == svc->type; }

#endif	/* FINIT_SVC_H_ */

/**
 * Local Variables:
 *  version-control: t
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
