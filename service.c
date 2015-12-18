/* Finit service monitor, task starter and generic API for managing svc_t
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

#include "config.h"		/* Generated by configure script */

#include <string.h>
#include <sys/wait.h>
#include <net/if.h>
#include "libite/lite.h"

#include "finit.h"
#include "conf.h"
#include "cond.h"
#include "helpers.h"
#include "private.h"
#include "sig.h"
#include "tty.h"
#include "service.h"
#include "inetd.h"

#define RESPAWN_MAX    10	        /* Prevent endless respawn of faulty services. */

static int    in_teardown = 0, in_dyn_teardown = 0;

#ifndef INETD_DISABLED
static svc_t *find_inetd_svc     (char *path, char *service, char *proto);
#endif

/**
 * service_bootstrap - Start bootstrap services and tasks
 *
 * System startup, runlevel S, where only services, tasks and
 * run commands absolutely essential to bootstrap are located.
 */
void service_bootstrap(void)
{
	_d("Bootstrapping all services in runlevel S from %s", FINIT_CONF);
	service_step_all(SVC_TYPE_RUN | SVC_TYPE_TASK | SVC_TYPE_SERVICE);
}

/**
 * service_enabled - Should the service run?
 * @svc: Pointer to &svc_t object
 *
 * Returns:
 * 1, if the service is allowed to run in the current runlevel and the
 * user has not manually requested that this service should not run. 0
 * otherwise.
 */
int service_enabled(svc_t *svc)
{
	if (!svc ||
	    !svc_in_runlevel(svc, runlevel) ||
	    svc->block != SVC_BLOCK_NONE)
		return 0;

	return 1;
}

/**
 * service_stop_is_done - Have all stopped services been collected?
 *
 * Returns:
 * 1, if all stopped services have been collected. 0 otherwise.
 */
static int service_stop_is_done(void)
{
	svc_t *svc;

	for (svc = svc_iterator(1); svc; svc = svc_iterator(0))
		if (svc->state == SVC_STOPPING_STATE)
			return 0;

	return 1;
}

static int is_norespawn(void)
{
	return  sig_stopped()            ||
		fexist("/mnt/norespawn") ||
		fexist("/tmp/norespawn");
}

/**
 * service_start - Start service
 * @svc: Service to start
 *
 * Returns:
 * 0 if the service was successfully started. Non-zero otherwise. 
 */
static int service_start(svc_t *svc)
{
	int sd = 0;
	pid_t pid;
	sigset_t nmask, omask;

	if (!svc)
		return 1;

	/* Don't try and start service if it doesn't exist. */
	if (!fexist(svc->cmd) && !svc->inetd.cmd) {
		if (verbose) {
			char msg[80];

			snprintf(msg, sizeof(msg), "Service %s does not exist!", svc->cmd);
			print_desc("", msg);
			print_result(1);
		}

		svc->block = SVC_BLOCK_MISSING;
		return 1;
	}

	/* Ignore if finit is SIGSTOP'ed */
	if (is_norespawn())
		return 1;

#ifndef INETD_DISABLED
	if (svc_is_inetd(svc)) {
		char ifname[IF_NAMESIZE] = "UNKNOWN";

		sd = svc->inetd.watcher.fd;

		if (svc->inetd.type == SOCK_STREAM) {
			/* Open new client socket from server socket */
			sd = accept(sd, NULL, NULL);
			if (sd < 0) {
				FLOG_PERROR("Failed accepting inetd service %d/tcp", svc->inetd.port);
				return 1;
			}

			_d("New client socket %d accepted for inetd service %d/tcp", sd, svc->inetd.port);

			/* Find ifname by means of getsockname() and getifaddrs() */
			inetd_stream_peek(sd, ifname);
		} else {           /* SOCK_DGRAM */
			/* Find ifname by means of IP_PKTINFO sockopt --> ifindex + if_indextoname() */
			inetd_dgram_peek(sd, ifname);
		}

		if (!inetd_is_allowed(&svc->inetd, ifname)) {
			FLOG_INFO("Service %s on port %d not allowed from interface %s.",
				  svc->inetd.name, svc->inetd.port, ifname);
			if (svc->inetd.type == SOCK_STREAM)
				close(sd);

			return 1;
		}

		FLOG_INFO("Starting inetd service %s for requst from iface %s ...", svc->inetd.name, ifname);
	} else
#endif
	if (verbose) {
		if (svc_is_daemon(svc))
			print_desc("Starting ", svc->desc);
		else
			print_desc("", svc->desc);
	}

	/* Block sigchild while forking.  */
	sigemptyset(&nmask);
	sigaddset(&nmask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &nmask, &omask);

	pid = fork();
	sigprocmask(SIG_SETMASK, &omask, NULL);
	if (pid == 0) {
		int i = 0;
		int status;
#ifdef ENABLE_STATIC
		int uid = 0; /* XXX: Fix better warning that dropprivs is disabled. */
#else
		int uid = getuser(svc->username);
#endif
		struct sigaction sa;
		char *args[MAX_NUM_SVC_ARGS];

		sigemptyset(&nmask);
		sigaddset(&nmask, SIGCHLD);
		sigprocmask(SIG_UNBLOCK, &nmask, NULL);

		/* Reset signal handlers that were set by the parent process */
		for (i = 1; i < NSIG; i++)
			DFLSIG(sa, i, 0);

		/* Set desired user */
		if (uid >= 0) {
			setuid(uid);

			/* Set default path for regular users */
			if (uid > 0)
				setenv("PATH", _PATH_DEFPATH, 1);
		}

		/* Serve copy of args to process in case it modifies them. */
		for (i = 0; i < (MAX_NUM_SVC_ARGS - 1) && svc->args[i][0] != 0; i++)
			args[i] = svc->args[i];
		args[i] = NULL;

		/* Redirect inetd socket to stdin for service */
		if (svc_is_inetd(svc)) {
			/* sd set previously */
			dup2(sd, STDIN_FILENO);
			close(sd);
			dup2(STDIN_FILENO, STDOUT_FILENO);
			dup2(STDIN_FILENO, STDERR_FILENO);
		} else if (debug) {
			int fd;
			char buf[CMD_SIZE] = "";

			fd = open(CONSOLE, O_WRONLY | O_APPEND);
			if (-1 != fd) {
				dup2(fd, STDOUT_FILENO);
				dup2(fd, STDERR_FILENO);
				close(fd);
			}

			for (i = 0; i < MAX_NUM_SVC_ARGS && args[i]; i++) {
				char arg[MAX_ARG_LEN];

				snprintf(arg, sizeof(arg), "%s ", args[i]);
				if (strlen(arg) < (sizeof(buf) - strlen(buf)))
					strcat(buf, arg);
			}
			_e("Starting %s: %s", svc->cmd, buf);
		}

		sig_unblock();

		if (svc->inetd.cmd)
			status = svc->inetd.cmd(svc->inetd.type);
		else
			status = execv(svc->cmd, args); /* XXX: Maybe use execve() to be able to launch scripts? */

		if (svc_is_inetd(svc)) {
			if (svc->inetd.type == SOCK_STREAM) {
				close(STDIN_FILENO);
				close(STDOUT_FILENO);
				close(STDERR_FILENO);
			}
		}

		exit(status);
	}
	svc->pid = pid;

	if (svc_is_inetd(svc)) {
		if (svc->inetd.type == SOCK_STREAM)
			close(sd);
	} else {
		int result = 0;

		if (SVC_TYPE_RUN == svc->type)
			result = WEXITSTATUS(complete(svc->cmd, pid));

		if (verbose)
			print_result(result);
	}

	return 0;
}

/**
 * service_stop - Stop service
 * @svc: Service to stop
 *
 * Returns:
 * 0 if the service was successfully stopped. Non-zero otherwise. 
 */
static int service_stop(svc_t *svc)
{
	int res = 0;

	if (!svc)
		return 1;

	if (svc->pid <= 1) {
		_d("Bad PID %d for %s, SIGTERM", svc->pid, svc->desc);
		return 1;
	}

	if (SVC_TYPE_SERVICE != svc->type)
		return 0;

	if (runlevel != 1 && verbose)
		print_desc("Stopping ", svc->desc);

	_d("Sending SIGTERM to pid:%d name:%s", svc->pid, pid_get_name(svc->pid, NULL, 0));
	res = kill(svc->pid, SIGTERM);

	if (runlevel != 1 && verbose)
		print_result(res);

	return res;
}

/**
 * service_restart - Restart a service by sending %SIGHUP
 * @svc: Service to reload
 *
 * This function does some basic checks of the runtime state of Finit
 * and a sanity check of the @svc before sending %SIGHUP.
 * 
 * Returns:
 * POSIX OK(0) or non-zero on error.
 */
static int service_restart(svc_t *svc)
{
	int err;

	/* Ignore if finit is SIGSTOP'ed */
	if (is_norespawn())
		return 1;

	if (!svc || !svc->sighup)
		return 1;

	if (svc->pid <= 1) {
		_d("Bad PID %d for %s, SIGHUP", svc->pid, svc->cmd);
		svc->pid = 0;
		return 1;
	}

	if (verbose)
		print_desc("Restarting ", svc->desc);

	_d("Sending SIGHUP to PID %d", svc->pid);
	err = kill(svc->pid, SIGHUP);

	if (verbose)
		print_result(err);
	return err;
}

/**
 * service_reload_dynamic_finish - Finish dynamic service reload
 *
 * Second stage of dynamic reload. Called either directly from first
 * stage if no services had to be stopped, or later from
 * service_monitor once all stopped services have been collected.
 */
static void service_reload_dynamic_finish(void)
{
	in_dyn_teardown = 0;

	_d("All services have been stoppped, calling reconf hooks ...");
	plugin_run_hooks(HOOK_SVC_RECONF);

	_d("Starting services after reconf ...");
	service_step_all(SVC_TYPE_SERVICE);
}

/**
 * service_reload_dynamic - Called on SIGHUP, 'init q' or 'initctl reload'
 *
 * This function is called when Finit has recieved SIGHUP to reload
 * .conf files in /etc/finit.d.  It is responsible for starting,
 * stopping and reloading (forwarding SIGHUP) to processes affected.
 */
void service_reload_dynamic(void)
{
	/* First reload all *.conf in /etc/finit.d/ */
	conf_reload_dynamic();

	/* Then, mark all affected service conditions as in-flux and
	 * let all affected services move to WAITING/HALTED */
	_d("Stopping services services not allowed after reconf ...");
	in_dyn_teardown = 1;
	cond_reload();
	service_step_all(SVC_TYPE_SERVICE);

	/* Need to wait for any services to stop? If so, exit early
	 * and perform second stage from service_monitor later. */
	if (!service_stop_is_done())
		return;

	/* Otherwise, kick all svcs again right away */
	service_reload_dynamic_finish();
}

/**
 * service_runlevel_finish - Finish runlevel change
 *
 * Second stage of runlevel change. Called either directly from first
 * stage if no services had to be stopped, or later from
 * service_monitor once all stopped services have been collected.
 */
static void service_runlevel_finish(void)
{
	/* Prev runlevel services stopped, call hooks before starting new runlevel ... */
	_d("All services have been stoppped, calling runlevel change hooks ...");
	plugin_run_hooks(HOOK_RUNLEVEL_CHANGE);  /* Reconfigure HW/VLANs/etc here */

	_d("Starting services services new to this runlevel ...");
	in_teardown = 0;
	service_step_all(SVC_TYPE_ANY);

	/* Cleanup stale services */
	svc_clean_dynamic(service_unregister);

	if (0 == runlevel) {
		do_shutdown(SIGUSR2);
		return;
	}
	if (6 == runlevel) {
		do_shutdown(SIGUSR1);
		return;
	}

	if (runlevel == 1)
		touch("/etc/nologin");	/* Disable login in single-user mode */
	else
		erase("/etc/nologin");

	/* No TTYs run at bootstrap, they have a delayed start. */
	if (prevlevel > 0)
		tty_runlevel(runlevel);
}

/**
 * service_runlevel - Change to a new runlevel
 * @newlevel: New runlevel to activate
 *
 * Stops all services not in @newlevel and starts, or lets continue to run,
 * those in @newlevel.  Also updates @prevlevel and active @runlevel.
 */
void service_runlevel(int newlevel)
{
	if (runlevel == newlevel)
		return;

	if (newlevel < 0 || newlevel > 9)
		return;

	prevlevel = runlevel;
	runlevel  = newlevel;

	_d("Setting new runlevel --> %d <-- previous %d", runlevel, prevlevel);
	runlevel_set(prevlevel, newlevel);

	/* Make sure to (re)load all *.conf in /etc/finit.d/ */
	conf_reload_dynamic();

	_d("Stopping services services not allowed in new runlevel ...");
	in_teardown = 1;
	service_step_all(SVC_TYPE_ANY);

	/* Need to wait for any services to stop? If so, exit early
	 * and perform second stage from service_monitor later. */
	if (!service_stop_is_done())
		return;

	service_runlevel_finish();
}

/**
 * service_register - Register service, task or run commands
 * @type:     %SVC_TYPE_SERVICE(0), %SVC_TYPE_TASK(1), %SVC_TYPE_RUN(2)
 * @line:     A complete command line with -- separated description text
 * @mtime:    The modification time if service is loaded from /etc/finit.d
 * @username: Optional username to run service as, or %NULL to run as root
 *
 * This function is used to register commands to be run on different
 * system runlevels with optional username.  The @type argument details
 * if it's service to bo monitored/respawned (daemon), a one-shot task
 * or a command that must run in sequence and not in parallell, like
 * service and task commands do.
 *
 * The @line can optionally start with a username, denoted by an @
 * character. Like this:
 *
 *     service @username [!0-6,S] <!EV> /path/to/daemon arg -- Description
 *     task @username [!0-6,S] /path/to/task arg            -- Description
 *     run  @username [!0-6,S] /path/to/cmd arg             -- Description
 *     inetd tcp/ssh nowait [2345] @root:root /sbin/sshd -i -- Description
 *
 * If the username is left out the command is started as root.  The []
 * brackets denote the allowed runlevels, if left out the default for a
 * service is set to [2-5].  Allowed runlevels mimic that of SysV init
 * with the addition of the 'S' runlevel, which is only run once at
 * startup.  It can be seen as the system bootstrap.  If a task or run
 * command is listed in more than the [S] runlevel they will be called
 * when changing runlevel.
 *
 * Services (daemons, not inetd services) also support an optional <!EV>
 * argument.  This is for services that, e.g., require a system gateway
 * or interface to be up before they are started.  Or restarted, or even
 * SIGHUP'ed, when the gateway changes or interfaces come and go.  The
 * special case when a service is declared with <!> means it does not
 * support SIGHUP but must be STOP/START'ed at system reconfiguration.
 *
 * Supported service events are: GW, IFUP[:ifname], IFDN[:ifname], where
 * the interface name (:ifname) is optional.  Actully, the check with a
 * service event declaration is string based, so 'IFUP:ppp' will match
 * any of "IFUP:ppp0" or "IFUP:pppoe1" sent by the netlink.so plugin.
 *
 * For multiple instances of the same command, e.g. multiple DHCP
 * clients, the user must enter an ID, using the :ID syntax.
 *
 *     service :1 /sbin/udhcpc -i eth1
 *     service :2 /sbin/udhcpc -i eth2
 *
 * Without the :ID syntax Finit will overwrite the first service line
 * with the contents of the second.  The :ID must be [1,MAXINT].
 *
 * Returns:
 * POSIX OK(0) on success, or non-zero errno exit status on failure.
 */
int service_register(int type, char *line, time_t mtime, char *username)
{
	int i = 0;
	int id = 1;		/* Default to ID:1 */
#ifndef INETD_DISABLED
	int forking = 0;
#endif
	char *service = NULL, *proto = NULL, *ifaces = NULL;
	char *cmd, *desc, *runlevels = NULL, *cond = NULL;
	svc_t *svc;
	plugin_t *plugin = NULL;

	if (!line) {
		_e("Invalid input argument.");
		return errno = EINVAL;
	}

	desc = strstr(line, "-- ");
	if (desc)
		*desc = 0;

	cmd = strtok(line, " ");
	if (!cmd) {
	incomplete:
		_e("Incomplete service, cannot register.");
		return errno = ENOENT;
	}

	while (cmd) {
		     if (cmd[0] == '@')	/* @username[:group] */
			username = &cmd[1];
		else if (cmd[0] == '[')	/* [runlevels] */
			runlevels = &cmd[0];
		else if (cmd[0] == '<')	/* <[!][ev][,ev..]> */
			cond = &cmd[1];
		else if (cmd[0] == ':')	/* :ID */
			id = atoi(&cmd[1]);
#ifndef INETD_DISABLED
		else if (!strncasecmp(cmd, "nowait", 6))
			forking = 1;
		else if (!strncasecmp(cmd, "wait", 4))
			forking = 0;
#endif
		else if (cmd[0] != '/' && strchr(cmd, '/'))
			service = cmd;   /* inetd service/proto */
		else
			break;

		/* Check if valid command follows... */
		cmd = strtok(NULL, " ");
		if (!cmd)
			goto incomplete;
	}

	/* Example: inetd ssh/tcp@eth0,eth1 or 222/tcp@eth2 */
	if (service) {
		ifaces = strchr(service, '@');
		if (ifaces)
			*ifaces++ = 0;

		proto = strchr(service, '/');
		if (!proto)
			goto incomplete;
		*proto++ = 0;
	}

#ifndef INETD_DISABLED
	/* Find plugin that provides a callback for this inetd service */
	if (type == SVC_TYPE_INETD) {
		if (!strncasecmp(cmd, "internal", 8)) {
			char *ptr, *ps = service;

			/* internal.service */
			ptr = strchr(cmd, '.');
			if (ptr) {
				*ptr++ = 0;
				ps = ptr;
			}

			plugin = plugin_find(ps);
			if (!plugin || !plugin->inetd.cmd) {
				_e("Inetd service %s has no internal plugin, skipping.", service);
				return errno = ENOENT;
			}
		}

		/* Check if known inetd, then add ifnames for filtering only. */
		svc = find_inetd_svc(cmd, service, proto);
		if (svc)
			goto inetd_setup;

		id = svc_next_id(cmd);
	}
#endif

	svc = svc_find(cmd, id);
	if (!svc) {
		_d("Creating new svc for %s id #%d type %d", cmd, id, type);
		svc = svc_new(cmd, id, type);
		if (!svc) {
			_e("Out of memory, cannot register service %s", cmd);
			return errno = ENOMEM;
		}
	}

	/* New, recently modified or unchanged ... used on reload. */
	svc_check_dirty(svc, mtime);

	if (desc)
		strlcpy(svc->desc, desc + 3, sizeof(svc->desc));

	if (username) {
		char *ptr = strchr(username, ':');

		if (ptr) {
			*ptr++ = 0;
			strlcpy(svc->group, ptr, sizeof(svc->group));
		}
		strlcpy(svc->username, username, sizeof(svc->username));
	}

	if (plugin) {
		/* Internal plugin provides this service */
		svc->inetd.cmd = plugin->inetd.cmd;
	} else {
		strlcpy(svc->args[i++], cmd, sizeof(svc->args[0]));
		while ((cmd = strtok(NULL, " ")))
			strlcpy(svc->args[i++], cmd, sizeof(svc->args[0]));
		svc->args[i][0] = 0;

		plugin = plugin_find(svc->cmd);
		if (plugin && plugin->svc.cb) {
			svc->cb           = plugin->svc.cb;
			svc->dynamic      = plugin->svc.dynamic;
			svc->dynamic_stop = plugin->svc.dynamic_stop;
		}
	}

	svc->runlevels = conf_parse_runlevels(runlevels);
	_d("Service %s runlevel 0x%2x", svc->cmd, svc->runlevels);

	if (type == SVC_TYPE_SERVICE)
		conf_parse_cond(svc, cond);

#ifndef INETD_DISABLED
	if (svc_is_inetd(svc)) {
		char *iface, *name = service;

		svc->state = SVC_WAITING_STATE;
		if (svc->inetd.cmd && plugin)
			name = plugin->name;

		if (inetd_new(&svc->inetd, name, service, proto, forking, svc)) {
			_e("Failed registering new inetd service %s.", service);
			inetd_del(&svc->inetd);
			return svc_del(svc);
		}

	inetd_setup:
		if (!ifaces) {
			_d("No specific iface listed for %s, allowing ANY.", service);
			return inetd_allow(&svc->inetd, NULL);
		}

		for (iface = strtok(ifaces, ","); iface; iface = strtok(NULL, ",")) {
			if (iface[0] == '!')
				inetd_deny(&svc->inetd, &iface[1]);
			else
				inetd_allow(&svc->inetd, iface);
		}
	}
#endif

	return 0;
}

void service_unregister(svc_t *svc)
{
	if (svc->state != SVC_HALTED_STATE)
		_e("Failed stopping %s, removing anyway from list of monitored services.", svc->cmd);
	svc_del(svc);
}

/**
 * service_teardown_finish - Complete runlevel change or dynamic reload
 *
 * If any runlevel change or dynamic service reload is in progress and
 * all services that had to be stopped have been collected, run the
 * corresponding second stage.
 */
static void service_teardown_finish(void)
{
	if (!(in_teardown || in_dyn_teardown))
		return;
	
	if (!service_stop_is_done())
		return;

	if (in_teardown)
		service_runlevel_finish();

	if (in_dyn_teardown)
		service_reload_dynamic_finish();
}


void service_monitor(pid_t lost)
{
	svc_t *svc;

	if (fexist(SYNC_SHUTDOWN) || lost <= 1)
		return;

	if (tty_respawn(lost))
		return;

#ifndef INETD_DISABLED
	if (inetd_respawn(lost))
		return;
#endif

	svc = svc_find_by_pid(lost);
	if (!svc) {
		_e("collected unknown PID %d", lost);
		return;
	}

	if (!prevlevel && svc_clean_bootstrap(svc))
		return;

	_d("collected %s(%d)", svc->cmd, lost);

	/* No longer running, update books. */
	svc->pid = 0;
	service_step(svc);

	/* Check if we're still collecting stopped dynamic services */
	service_teardown_finish();
}

void service_step(svc_t *svc)
{
	/* These fields are marked as const in svc_t, only this
	 * function is allowed to modify them */
	svc_state_t *state = (svc_state_t *)&svc->state;
	int *restart_counter = (int *)&svc->restart_counter;

	svc_cmd_t enabled;
	svc_state_t old_state;
	cond_state_t cond;
	char *old_status;
	int err;

restart:
	old_state = *state;
	enabled = service_enabled(svc);

	if (debug)
		old_status = strdup(svc_status(svc));

	switch(*state) {
	case SVC_HALTED_STATE:
		*restart_counter = 0;
		if (enabled)
			*state = SVC_READY_STATE;
		break;

	case SVC_DONE_STATE:
		if (svc_is_changed(svc))
			*state = SVC_HALTED_STATE;
		break;

	case SVC_STOPPING_STATE:
		if (!svc->pid)
			*state = SVC_HALTED_STATE;
		break;

	case SVC_READY_STATE:
		if (!enabled) {
			*state = SVC_HALTED_STATE;
		} else if (cond_get_agg(svc->cond) == COND_ON) {
			if (*restart_counter >= RESPAWN_MAX) {
				_e("%s keeps crashing, not restarting",
				   svc->desc ? : svc->cmd);
				svc->block = SVC_BLOCK_CRASHING;
				*state = SVC_HALTED_STATE;
				break;
			}

			err = service_start(svc);
			if (err || !svc->pid) {
				(*restart_counter)++;
				break;
			}

			svc->dirty = 0;

			switch (svc->type) {
			case SVC_TYPE_SERVICE:
				*state = SVC_RUNNING_STATE;
				break;
			case SVC_TYPE_INETD:
			case SVC_TYPE_TASK:
				*state = SVC_STOPPING_STATE;
				break;
			case SVC_TYPE_RUN:
				*state = SVC_DONE_STATE;
				break;
			default:
				_e("unknown service type %d", svc->type);
			}
		}
		break;

	case SVC_RUNNING_STATE:
		if (!enabled) {
			service_stop(svc);
			*state = SVC_STOPPING_STATE;
			break;
		}

		if (!svc->pid) {
			(*restart_counter)++;
			*state = SVC_READY_STATE;
			break;
		}

		cond = cond_get_agg(svc->cond);

		if (cond == COND_OFF ||
		    (!svc->sighup && (cond < COND_ON || svc_is_changed(svc)))) {
			service_stop(svc);
			*state = SVC_READY_STATE;
			break;
		}

		if (cond == COND_FLUX) {
			kill(svc->pid, SIGSTOP);
			*state = SVC_WAITING_STATE;
			break;
		}

		if (svc_is_changed(svc)) {
			if (svc->sighup) {
				service_restart(svc);
			} else {
				service_stop(svc);
				*state = SVC_READY_STATE;
			}
			svc->dirty = 0;
		}

		break;

	case SVC_WAITING_STATE:
		if (!enabled) {
			kill(svc->pid, SIGCONT);
			service_stop(svc);
			*state = SVC_HALTED_STATE;
			break;
		}

		if (!svc->pid) {
			(*restart_counter)++;
			*state = SVC_READY_STATE;
			break;
		}

		cond = cond_get_agg(svc->cond);
		switch (cond) {
		case COND_ON:
			kill(svc->pid, SIGCONT);
			*state = SVC_RUNNING_STATE;
			break;

		case COND_OFF:
			kill(svc->pid, SIGCONT);
			service_stop(svc);
			*state = SVC_READY_STATE;
			break;

		case COND_FLUX:
			break;
		}
		break;
	}

	if (*state != old_state) {
		if (debug) {
			_d("%-20.20s %s -> %s", svc->cmd,
			   old_status, svc_status(svc));
			free(old_status);
		}
		goto restart;
	}
}

void service_step_all(int types)
{
	svc_t *svc;

	for (svc = svc_iterator(1); svc; svc = svc_iterator(0)) {
		if (!(svc->type & types))
			continue;

		service_step(svc);
	}
}

#ifndef INETD_DISABLED
static svc_t *find_inetd_svc(char *path, char *service, char *proto)
{
	svc_t *svc;

	for (svc = svc_inetd_iterator(1); svc; svc = svc_inetd_iterator(0)) {
		if (strncmp(path, svc->cmd, strlen(svc->cmd)))
			continue;

		if (inetd_match(&svc->inetd, service, proto)) {
			_d("Found a matching inetd svc for %s %s %s", path, service, proto);
			return svc;
		}
	}

	return NULL;
}
#endif

/**
 * Local Variables:
 *  version-control: t
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
