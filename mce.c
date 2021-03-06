/**
 * @file mce.c
 * Mode Control Entity - main file
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 *
 * mce is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * mce is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mce.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <glib.h>
#include <glib-object.h>		/* g_type_init() */

#include <errno.h>			/* errno, ENOMEM */
#include <fcntl.h>			/* open(), O_RDWR, O_CREAT */
#include <stdio.h>			/* fprintf(), sprintf(),
					 * stdout, stderr
					 */
#include <getopt.h>			/* getopt_long(),
					 * struct options
					 */
#include <signal.h>			/* signal(),
					 * SIGTSTP, SIGTTOU, SIGTTIN,
					 * SIGCHLD, SIGUSR1, SIGHUP,
					 * SIGTERM, SIG_IGN
					 */
#include <stdlib.h>			/* exit(), EXIT_FAILURE, EXIT_SUCCESS */
#include <string.h>			/* strlen() */
#include <unistd.h>			/* close(), lockf(), fork(), chdir(),
					 * getpid(), getppid(), setsid(),
					 * write(), getdtablesize(), dup(),
					 * F_TLOCK
					 */
#include <sys/stat.h>			/* umask() */

#include "mce.h"			/* _(),
					 * setlocale() -- indirect,
					 * bindtextdomain(),
					 * textdomain(),
					 * mainloop,
					 * system_state_pipe,
					 * master_radio_pipe,
					 * call_state_pipe,
					 * call_type_pipe,
					 * submode_pipe,
					 * display_state_pipe,
					 * display_brightness_pipe,
					 * led_brightness_pipe,
					 * led_pattern_activate_pipe,
					 * led_pattern_deactivate_pipe,
					 * key_backlight_pipe,
					 * keypress_pipe,
					 * touchscreen_pipe,
					 * device_inactive_pipe,
					 * lockkey_pipe,
					 * keyboard_slide_pipe,
					 * lid_cover_pipe,
					 * lens_cover_pipe,
					 * proximity_sensor_pipe,
					 * tk_lock_pipe,
					 * charger_state_pipe,
					 * battery_status_pipe,
					 * battery_level_pipe,
					 * inactivity_timeout_pipe,
					 * audio_route_pipe,
					 * usb_cable_pipe,
					 * jack_sense_pipe,
					 * power_saving_mode_pipe,
					 * thermal_state_pipe,
					 * MCE_STATE_UNDEF,
					 * MCE_INVALID_MODE_INT32,
					 * CALL_STATE_NONE,
					 * NORMAL_CALL,
					 * MCE_ALARM_UI_INVALID_INT32,
					 * MCE_NORMAL_SUBMODE,
					 * MCE_DISPLAY_UNDEF,
					 * LOCK_UNDEF,
					 * BATTERY_STATUS_UNDEF,
					 * THERMAL_STATE_UNDEF,
					 * DEFAULT_INACTIVITY_TIMEOUT
					 */

#include "mce-log.h"			/* mce_log_open(), mce_log_close(),
					 * mce_log_set_verbosity(), mce_log(),
					 * LL_*
					 */
#include "mce-conf.h"			/* mce_conf_init(),
					 * mce_conf_exit()
					 */
#include "mce-dbus.h"			/* mce_dbus_init(),
					 * mce_dbus_exit()
					 */
#include "mce-dsme.h"			/* mce_dsme_init(),
					 * mce_dsme_exit()
					 */
#include "mce-gconf.h"			/* mce_gconf_init(),
					 * mce_gconf_exit()
					 */
#include "mce-modules.h"		/* mce_modules_dump_info(),
					 * mce_modules_init(),
					 * mce_modules_exit()
					 */
#include "event-input.h"		/* mce_input_init(),
					 * mce_input_exit()
					 */
#include "event-switches.h"		/* mce_switches_init(),
					 * mce_switches_exit()
					 */
#include "datapipe.h"			/* setup_datapipe(),
					 * free_datapipe()
					 */
#include "modetransition.h"		/* mce_mode_init(),
					 * mce_mode_exit()
					 */

/* "TBD" Modules; eventually this should be handled differently */
#include "tklock.h"			/* mce_tklock_init(),
					 * mce_tklock_exit()
					 */
#include "powerkey.h"			/* mce_powerkey_init(),
					 * mce_powerkey_exit()
					 */
#ifdef ENABLE_WAKELOCKS
# include "libwakelock.h"
#endif

#ifdef ENABLE_SENSORFW
# include "mce-sensorfw.h"
#endif

#include <systemd/sd-daemon.h>

/** Path to the lockfile */
#define MCE_LOCKFILE			"/var/run/mce.pid"
/** Name shown by --help etc. */
#define PRG_NAME			"mce"

extern int optind;			/**< Used by getopt */
extern char *optarg;			/**< Used by getopt */

static const gchar *progname;	/**< Used to store the name of the program */

/** The GMainLoop used by MCE */
static GMainLoop *mainloop = 0;

/** Wrapper for write() for use when we do not care if it works or not
 *
 * Main purpose is to stop static analyzers from nagging us when
 * we really do not care whether the data gets written or not
 *
 * @param fd   file descriptor to write to
 * @param data data to write
 * @param size amount of data to write
 */
static void no_error_check_write(int fd, const void *data, size_t size)
{
	// do the write, then ...
	ssize_t rc = TEMP_FAILURE_RETRY(write(fd, data, size));
	// try to silence static analyzers by doing /something/ with rc
	if( rc == -1 )
		rc = rc;
}

static const char usage_fmt[] =
"Usage: %s [OPTION]...\n"
"Mode Control Entity\n"
"\n"
"  -n, --systemd              notify systemd when started up\n"
"  -d, --daemonflag           run MCE as a daemon\n"
"  -s, --force-syslog         log to syslog even when not daemonized\n"
"  -T, --force-stderr         log to stderr even when daemonized\n"
"  -S, --session              use the session bus instead of the system\n"
"                               bus for D-Bus\n"
"  -M, --show-module-info     show information about loaded modules\n"
"  -D, --debug-mode           run even if dsme fails\n"
"  -q, --quiet                decrease debug message verbosity\n"
"  -v, --verbose              increase debug message verbosity\n"
"  -t, --trace=<what>         enable domain specific debug logging;\n"
"                               supported values: \"wakelocks\"\n"
"  -h, --help                 display this help and exit\n"
"  -V, --version              output version information and exit\n"
"\n"
"Report bugs to <david.weinehall@nokia.com>\n"
;
/**
 * Display usage information
 */
static void usage(void)
{
  fprintf(stdout, usage_fmt, progname);
}

/**
 * Display version information
 */
static void version(void)
{
	fprintf(stdout, _("%s v%s\n%s"),
		progname,
		G_STRINGIFY(PRG_VERSION),
		_("Written by David Weinehall.\n"
		  "\n"
		  "Copyright (C) 2004-2010 Nokia Corporation.  "
		  "All rights reserved.\n"));
}

/**
 * Initialise locale support
 *
 * @param name The program name to output in usage/version information
 * @return 0 on success, non-zero on failure
 */
static gint init_locales(const gchar *const name)
{
	gint status = 0;

#ifdef ENABLE_NLS
	setlocale(LC_ALL, "");

	if ((bindtextdomain(name, LOCALEDIR) == 0) && (errno == ENOMEM)) {
		status = errno;
		goto EXIT;
	}

	if ((textdomain(name) == 0) && (errno == ENOMEM)) {
		status = errno;
		goto EXIT;
	}

EXIT:
	/* In this error-message we don't use _(), since we don't
	 * know where the locales failed, and we probably won't
	 * get a reasonable result if we try to use them.
	 */
	if (status != 0) {
		fprintf(stderr,
			"%s: `%s' failed; %s. Aborting.\n",
			name, "init_locales", g_strerror(status));
	}

	if (errno != ENOMEM)
		errno = 0;
#endif /* ENABLE_NLS */
	progname = name;

	return status;
}

void mce_quit_mainloop(void)
{
#ifdef ENABLE_WAKELOCKS
	/* We are on exit path -> block suspend for good */
	wakelock_block_suspend_until_exit();
#endif

	/* Exit immediately if there is no mainloop to terminate */
	if( !mainloop ) {
		exit(1);
	}

	/* Terminate mainloop */
	g_main_loop_quit(mainloop);
}

#ifdef ENABLE_WAKELOCKS
/** Disable automatic suspend and remove wakelocks mce might hold
 *
 * This function should be called just before mce process terminates
 * so that we do not leave the system in a non-functioning state
 */
static void mce_cleanup_wakelocks(void)
{
	/* We are on exit path -> block suspend for good */
	wakelock_block_suspend_until_exit();

	wakelock_unlock("mce_display_on");
	wakelock_unlock("mce_input_handler");
	wakelock_unlock("mce_cpu_keepalive");
	wakelock_unlock("mce_display_stm");
}
#endif // ENABLE_WAKELOCKS

/** Disable autosuspend then exit via default signal handler
 *
 * @param signr the signal to exit through
 */
static void mce_exit_via_signal(int signr) __attribute__((noreturn));
static void mce_exit_via_signal(int signr)
{
	sigset_t ss;

	sigemptyset(&ss);
	sigaddset(&ss, SIGALRM);

	/* Give us N seconds to exit */
	signal(SIGALRM, SIG_DFL);
	alarm(3);
	sigprocmask(SIG_UNBLOCK, &ss, 0);

#ifdef ENABLE_WAKELOCKS
	/* Cancel auto suspend */
	mce_cleanup_wakelocks();
#endif
	/* Try to exit via default handler */
	signal(signr, SIG_DFL);
	sigaddset(&ss, signr);
	sigprocmask(SIG_UNBLOCK, &ss, 0);
	raise(signr);

	/* Or just abort as the last resort*/
	abort();
}

/** Suspend safe replacement for _exit(1), abort() etc
 */
void mce_abort(void)
{
	mce_exit_via_signal(SIGABRT);
}

static void mce_tx_signal_cb(int sig);

/**
 * Signal handler
 *
 * @param signr Signal type
 */
static void signal_handler(const gint signr)
{
	switch (signr) {
	case SIGUSR1:
		/* We'll probably want some way to communicate with MCE */
		break;

	case SIGHUP:
		/* Possibly for re-reading configuration? */
		break;

	case SIGINT:
	case SIGQUIT:
	case SIGTERM:
		/* Just die if we somehow get here without having a mainloop */
		if( !mainloop ) {
			mce_exit_via_signal(signr);
		}

		/* Terminate mainloop */
		mce_quit_mainloop();
		break;

	case SIGPIPE:
		break;

	default:
		/* Should never happen */
		break;
	}
}

/** Install handlers for signals we need to trap
 */
static void install_signal_handlers(void)
{
	static const int sig[] = {
		SIGUSR1,
		SIGHUP,

		SIGINT,
		SIGQUIT,
		SIGTERM,

#ifdef ENABLE_WAKELOCKS
		SIGABRT,
		SIGILL,
		SIGFPE,
		SIGSEGV,
		SIGPIPE,
		SIGALRM,
		SIGBUS,
		SIGTSTP,
#endif

		-1
	};

	for( size_t i = 0; sig[i] != -1; ++i ) {
		signal(sig[i], mce_tx_signal_cb);
	}
}

/** Pipe used for transferring signals out of signal handler context */
static int signal_pipe[2] = {-1, -1};

/** GIO callback for reading signals from pipe
 *
 * @param channel   io channel for signal pipe
 * @param condition call reason
 * @param data      user data
 *
 * @return TRUE (or aborts on error)
 */
static gboolean mce_rx_signal_cb(GIOChannel *channel,
				 GIOCondition condition, gpointer data)
{
	// we just want the cb ...
	(void)channel; (void)condition; (void)data;

	int sig = 0;
	int got = TEMP_FAILURE_RETRY(read(signal_pipe[0], &sig, sizeof sig));

	if( got != sizeof sig ) {
		mce_abort();
	}

	/* handle the signal */
	signal_handler(sig);

	/* keep the io watch */
	return TRUE;
}

/** Signal handler callback for writing signals to pipe
 *
 * @param sig the signal number to pass to mainloop via pipe
 */
static void mce_tx_signal_cb(int sig)
{
	/* NOTE: this function must be kept async-signal-safe! */

	static volatile int exit_tries = 0;

	static const char msg[] = "\n*** BREAK ***\n";
#ifdef ENABLE_WAKELOCKS
	static const char die[] = "\n*** UNRECOVERABLE FAILURE ***\n";
#endif

	/* FIXME: Should really use sigaction() to avoid having
	 * the default handler active until we manage to restore
	 * our handler here ... */
	signal(sig, mce_tx_signal_cb);

	switch( sig )
	{
	case SIGINT:
	case SIGQUIT:
	case SIGTERM:
		/* Make sure that a stuck or non-existing mainloop does
		 * not stop us from handling at least repeated terminating
		 signals ... */

#ifdef ENABLE_WAKELOCKS
		/* We are on exit path -> block suspend for good */
		wakelock_block_suspend_until_exit();
#endif

		no_error_check_write(STDERR_FILENO, msg, sizeof msg - 1);

		if( !mainloop || ++exit_tries >= 2 ) {
			mce_abort();
		}
		break;

#ifdef ENABLE_WAKELOCKS
	case SIGABRT:
	case SIGILL:
	case SIGFPE:
	case SIGSEGV:
	case SIGALRM:
	case SIGBUS:
		/* Unrecoverable failures can't be handled in the mainloop
		 * Terminate but disable suspend first */
		no_error_check_write(STDERR_FILENO, die, sizeof die - 1);
		mce_exit_via_signal(sig);
		break;

	case SIGTSTP:
		/* Stopping mce could also lead to unrecoverable suspend */
		break;
#endif
	default:
		break;
	}

	/* transfer the signal to mainloop via pipe */
	int did = TEMP_FAILURE_RETRY(write(signal_pipe[1], &sig, sizeof sig));

	if( did != (int)sizeof sig ) {
		mce_abort();
	}
}

/** Create a pipe and io watch for handling signal from glib mainloop
 */
static gboolean mce_init_signal_pipe(void)
{
	int         result  = FALSE;
	GIOChannel *channel = 0;

	if( pipe(signal_pipe) == -1 )
		goto EXIT;

	if( (channel = g_io_channel_unix_new(signal_pipe[0])) == 0 )
		goto EXIT;

	if( !g_io_add_watch(channel, G_IO_IN, mce_rx_signal_cb, 0) )
		goto EXIT;

	result = TRUE;

EXIT:
	if( channel != 0 ) g_io_channel_unref(channel);

	return result;
}

/**
 * Daemonize the program
 *
 * @return TRUE if MCE is started during boot, FALSE otherwise
 */
static gboolean daemonize(void)
{
	gint retries = 0;
	gint i = 0;
	gchar str[10];

	if (getppid() == 1)
		goto EXIT;	/* Already daemonized */

	/* Detach from process group */
	switch (fork()) {
	case -1:
		/* Parent - Failure */
		mce_log(LL_CRIT, "daemonize: fork failed: %s",
			g_strerror(errno));
		mce_log_close();
		exit(EXIT_FAILURE);

	case 0:
		/* Child */
		break;

	default:
		/* Parent -- Success */

		/* One main() one exit() - in this case the parent
		 * must not call atexit handlers etc */
		_exit(EXIT_SUCCESS);
	}

	/* Detach TTY */
	setsid();

	/* Close all file descriptors and redirect stdio to /dev/null */
	if ((i = getdtablesize()) == -1)
		i = 256;

	while (--i >= 0) {
		if (close(i) == -1) {
			if (retries > 10) {
				mce_log(LL_CRIT,
					"close() was interrupted more than "
					"10 times. Exiting.");
				mce_log_close();
				exit(EXIT_FAILURE);
			}

			if (errno == EINTR) {
				mce_log(LL_INFO,
					"close() was interrupted; retrying.");
				errno = 0;
				i++;
				retries++;
			} else if (errno == EBADF) {
				/* Ignore invalid file descriptors */
				errno = 0;
			} else {
				mce_log(LL_CRIT,
					"Failed to close() fd %d; %s. "
					"Exiting.",
					i + 1, g_strerror(errno));
				mce_log_close();
				exit(EXIT_FAILURE);
			}
		} else {
			retries = 0;
		}
	}

	if ((i = open("/dev/null", O_RDWR)) == -1) {
		mce_log(LL_CRIT,
			"Cannot open `/dev/null'; %s. Exiting.",
			g_strerror(errno));
		mce_log_close();
		exit(EXIT_FAILURE);
	}

	if ((dup(i) == -1)) {
		mce_log(LL_CRIT,
			"Failed to dup() `/dev/null'; %s. Exiting.",
			g_strerror(errno));
		mce_log_close();
		exit(EXIT_FAILURE);
	}

	if ((dup(i) == -1)) {
		mce_log(LL_CRIT,
			"Failed to dup() `/dev/null'; %s. Exiting.",
			g_strerror(errno));
		mce_log_close();
		exit(EXIT_FAILURE);
	}

	/* Set umask */
	umask(022);

	/* Set working directory */
	if ((chdir("/tmp") == -1)) {
		mce_log(LL_CRIT,
			"Failed to chdir() to `/tmp'; %s. Exiting.",
			g_strerror(errno));
		mce_log_close();
		exit(EXIT_FAILURE);
	}

	/* Single instance */
	if ((i = open(MCE_LOCKFILE, O_RDWR | O_CREAT, 0640)) == -1) {
		mce_log(LL_CRIT,
			"Cannot open lockfile; %s. Exiting.",
			g_strerror(errno));
		mce_log_close();
		exit(EXIT_FAILURE);
	}

	if (lockf(i, F_TLOCK, 0) == -1) {
		mce_log(LL_CRIT, "Already running. Exiting.");
		mce_log_close();
		exit(EXIT_FAILURE);
	}

	sprintf(str, "%d\n", getpid());
	no_error_check_write(i, str, strlen(str));
	close(i);

	/* Ignore TTY signals */
	signal(SIGTSTP, SIG_IGN);
	signal(SIGTTOU, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);

	/* Ignore child terminate signal */
	signal(SIGCHLD, SIG_IGN);

EXIT:
	return 0;
}

/** Helper for determining how long common prefix two strings have
 *
 * @param str1 non null string
 * @param str2 non null string
 *
 * @return length of common prefix strings share
 */
static size_t common_length(const char *str1, const char *str2)
{
	size_t i;
	for( i = 0; str1[i] && str1[i] == str2[i]; ++i ) {}
	return i;
}

/** Handle --trace=flags options
 *
 * @param flags comma separated list of trace domains
 *
 * @return TRUE on success, FALSE if unknown domains used
 */
static gboolean mce_enable_trace(const char *flags)
{
	static const struct {
		const char *domain;
		void (*callback)(void);
	} lut[] = {
#ifdef ENABLE_WAKELOCKS
		{ "wakelocks", lwl_enable_logging },
#endif
		{ NULL, NULL }
	};

	gboolean  res = TRUE;
	gchar    *tmp = g_strdup(flags);

	gchar    *now, *zen;
	size_t    bi, bn;

	for( now = tmp; now; now = zen ) {
		if( (zen = strchr(now, ',')) )
			*zen++ = 0;

		// initialize to: no match
		bi = bn = 0;

		for( size_t ti = 0; lut[ti].domain; ++ti ) {
			size_t tn = common_length(lut[ti].domain, now);

			// all of flag mathed?
			if( now[tn] )
				continue;

			// better or equal as the previous best?
			if( bn <= tn )
				bi = ti, bn = tn;

			// full match found?
			if( !lut[ti].domain[tn] )
				break;
		}

		// did we find a match?
		if( !bn ) {
			fprintf(stderr, "unknown trace domain: '%s'\n", now);
			res = FALSE;
		}
		else {
			// report if non-full match was used
			if( lut[bi].domain[bn] )
				fprintf(stderr, "trace: %s\n", lut[bi].domain);
			lut[bi].callback();
		}
	}

	g_free(tmp);
	return res;
}

/**
 * Main
 *
 * @param argc Number of command line arguments
 * @param argv Array with command line arguments
 * @return 0 on success, non-zero on failure
 */
int main(int argc, char **argv)
{
	int optc;
	int opt_index;

	int verbosity = LL_DEFAULT;
	int logtype   = MCE_LOG_SYSLOG;

	gint status = EXIT_FAILURE;
	gboolean show_module_info = FALSE;
	gboolean daemonflag = FALSE;
	gboolean systembus = TRUE;
	gboolean debugmode = FALSE;
	gboolean systemd_notify = FALSE;

	const char optline[] = "dsTSMDqvhVt:n";

	struct option const options[] = {
		{ "systemd",          no_argument,       0, 'n' },
		{ "daemonflag",       no_argument,       0, 'd' },
		{ "force-syslog",     no_argument,       0, 's' },
		{ "force-stderr",     no_argument,       0, 'T' },
		{ "session",          no_argument,       0, 'S' },
		{ "show-module-info", no_argument,       0, 'M' },
		{ "debug-mode",       no_argument,       0, 'D' },
		{ "quiet",            no_argument,       0, 'q' },
		{ "verbose",          no_argument,       0, 'v' },
		{ "help",             no_argument,       0, 'h' },
		{ "version",          no_argument,       0, 'V' },
		{ "trace",            required_argument, 0, 't' },
		{ 0, 0, 0, 0 }
        };

	/* Initialise support for locales, and set the program-name */
	if (init_locales(PRG_NAME) != 0)
		goto EXIT;

	/* Parse the command-line options */
	while ((optc = getopt_long(argc, argv, optline,
				   options, &opt_index)) != -1) {
		switch (optc) {
		case 'n':
			systemd_notify = TRUE;
			break;

		case 'd':
			daemonflag = TRUE;
			break;

		case 's':
			logtype = MCE_LOG_SYSLOG;
			break;

		case 'T':
			logtype = MCE_LOG_STDERR;
			break;

		case 'S':
			systembus = FALSE;
			break;

		case 'M':
			show_module_info = TRUE;
			break;

		case 'D':
			debugmode = TRUE;
			break;

		case 'q':
			if (verbosity > LL_NONE)
				verbosity--;
			break;

		case 'v':
			if (verbosity < LL_DEBUG)
				verbosity++;
			break;

		case 'h':
			usage();
			exit(EXIT_SUCCESS);

		case 'V':
			version();
			exit(EXIT_SUCCESS);
		case 't':
			if( !mce_enable_trace(optarg) )
				exit(EXIT_FAILURE);
			break;
		default:
			usage();
			exit(EXIT_FAILURE);
		}
	}

	/* We don't take any non-flag arguments */
	if ((argc - optind) > 0) {
		fprintf(stderr,
			_("%s: Too many arguments\n"
			  "Try: `%s --help' for more information.\n"),
			progname, progname);
		exit(EXIT_FAILURE);
	}

	mce_log_open(PRG_NAME, LOG_DAEMON, logtype);
	mce_log_set_verbosity(verbosity);

#ifdef ENABLE_WAKELOCKS
	/* Since mce enables automatic suspend, we must try to
	 * disable it when mce process exits */
	atexit(mce_cleanup_wakelocks);
#endif

	/* Daemonize if requested */
	if (daemonflag == TRUE)
		daemonize();

	/* Initialise GType system */
	g_type_init();

	/* Register a mainloop */
	mainloop = g_main_loop_new(NULL, FALSE);

	/* Signal handlers can be installed once we have a mainloop */
	if( !mce_init_signal_pipe() ) {
		mce_log(LL_CRIT, "Failed to initialise signal pipe");
		exit(EXIT_FAILURE);
	}
	install_signal_handlers();

	/* Initialise subsystems */

	/* Get configuration options */
	if( !mce_conf_init() ) {
		mce_log(LL_CRIT,
			"Failed to initialise configuration options");
		exit(EXIT_FAILURE);
	}

	/* Initialise D-Bus */
	if (mce_dbus_init(systembus) == FALSE) {
		mce_log(LL_CRIT,
			"Failed to initialise D-Bus");
		mce_log_close();
		exit(EXIT_FAILURE);
	}

	/* Initialise GConf
	 * pre-requisite: g_type_init()
	 */
	if (mce_gconf_init() == FALSE) {
		mce_log(LL_CRIT,
			"Cannot connect to default GConf engine");
		mce_log_close();
		exit(EXIT_FAILURE);
	}

	/* Setup all datapipes */
	setup_datapipe(&system_state_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(MCE_STATE_UNDEF));
	setup_datapipe(&master_radio_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&call_state_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(CALL_STATE_NONE));
	setup_datapipe(&call_type_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(NORMAL_CALL));
	setup_datapipe(&alarm_ui_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(MCE_ALARM_UI_INVALID_INT32));
	setup_datapipe(&submode_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(MCE_NORMAL_SUBMODE));
	setup_datapipe(&display_state_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(MCE_DISPLAY_UNDEF));
	setup_datapipe(&display_state_req_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(MCE_DISPLAY_UNDEF));
	setup_datapipe(&display_brightness_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(3));
	setup_datapipe(&led_brightness_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&led_pattern_activate_pipe, READ_ONLY, FREE_CACHE,
		       0, NULL);
	setup_datapipe(&led_pattern_deactivate_pipe, READ_ONLY, FREE_CACHE,
		       0, NULL);
	setup_datapipe(&key_backlight_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&keypress_pipe, READ_ONLY, FREE_CACHE,
		       sizeof (struct input_event), NULL);
	setup_datapipe(&touchscreen_pipe, READ_ONLY, FREE_CACHE,
		       sizeof (struct input_event), NULL);
	setup_datapipe(&device_inactive_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(FALSE));
	setup_datapipe(&lockkey_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&keyboard_slide_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&lid_cover_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&lens_cover_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&proximity_sensor_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(COVER_OPEN));
	setup_datapipe(&tk_lock_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(LOCK_UNDEF));
	setup_datapipe(&charger_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&battery_status_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(BATTERY_STATUS_UNDEF));
	setup_datapipe(&battery_level_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(100));
	setup_datapipe(&camera_button_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(CAMERA_BUTTON_UNDEF));
	setup_datapipe(&inactivity_timeout_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(DEFAULT_INACTIVITY_TIMEOUT));
	setup_datapipe(&audio_route_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(AUDIO_ROUTE_UNDEF));
	setup_datapipe(&usb_cable_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&jack_sense_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&power_saving_mode_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&thermal_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(THERMAL_STATE_UNDEF));
	setup_datapipe(&heartbeat_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));

	/* Initialise mode management
	 * pre-requisite: mce_gconf_init()
	 * pre-requisite: mce_dbus_init()
	 */
	if (mce_mode_init() == FALSE) {
		goto EXIT;
	}

	/* Initialise DSME
	 * pre-requisite: mce_gconf_init()
	 * pre-requisite: mce_dbus_init()
	 * pre-requisite: mce_mce_init()
	 */
	if (mce_dsme_init(debugmode) == FALSE) {
		if (debugmode == FALSE) {
			mce_log(LL_CRIT, "Cannot connect to DSME");
			goto EXIT;
		}
	}

	/* Initialise powerkey driver */
	if (mce_powerkey_init() == FALSE) {
		goto EXIT;
	}

	/* Initialise /dev/input driver
	 * pre-requisite: g_type_init()
	 */
	if (mce_input_init() == FALSE) {
		goto EXIT;
	}

	/* Initialise switch driver */
	if (mce_switches_init() == FALSE) {
		goto EXIT;
	}

	/* Initialise tklock driver */
	if (mce_tklock_init() == FALSE) {
		goto EXIT;
	}

#ifdef ENABLE_SENSORFW
	if( !mce_sensorfw_init() ) {
		goto EXIT;
	}
#endif

	/* Load all modules */
	if (mce_modules_init() == FALSE) {
		goto EXIT;
	}

	if (show_module_info == TRUE) {
		mce_modules_dump_info();
		goto EXIT;
	}

	/* MCE startup succeeded */
	status = EXIT_SUCCESS;

	/* Tell systemd that we have started up */
	if( systemd_notify ) {
		mce_log(LL_NOTICE, "notifying systemd");
		sd_notify(0, "READY=1");
	}

	/* Run the main loop */
	g_main_loop_run(mainloop);

	/* If we get here, the main loop has terminated;
	 * either because we requested or because of an error
	 */
EXIT:
	/* Unload all modules */
	mce_modules_exit();

	/* Call the exit function for all components */
#ifdef ENABLE_SENSORFW
	mce_sensorfw_quit();
#endif
	mce_tklock_exit();
	mce_switches_exit();
	mce_input_exit();
	mce_powerkey_exit();
	mce_dsme_exit();
	mce_mode_exit();

	/* Free all datapipes */
	free_datapipe(&thermal_state_pipe);
	free_datapipe(&power_saving_mode_pipe);
	free_datapipe(&jack_sense_pipe);
	free_datapipe(&usb_cable_pipe);
	free_datapipe(&audio_route_pipe);
	free_datapipe(&inactivity_timeout_pipe);
	free_datapipe(&battery_level_pipe);
	free_datapipe(&battery_status_pipe);
	free_datapipe(&charger_state_pipe);
	free_datapipe(&tk_lock_pipe);
	free_datapipe(&proximity_sensor_pipe);
	free_datapipe(&lens_cover_pipe);
	free_datapipe(&lid_cover_pipe);
	free_datapipe(&keyboard_slide_pipe);
	free_datapipe(&lockkey_pipe);
	free_datapipe(&device_inactive_pipe);
	free_datapipe(&touchscreen_pipe);
	free_datapipe(&keypress_pipe);
	free_datapipe(&key_backlight_pipe);
	free_datapipe(&led_pattern_deactivate_pipe);
	free_datapipe(&led_pattern_activate_pipe);
	free_datapipe(&led_brightness_pipe);
	free_datapipe(&display_brightness_pipe);
	free_datapipe(&display_state_pipe);
	free_datapipe(&submode_pipe);
	free_datapipe(&alarm_ui_state_pipe);
	free_datapipe(&call_type_pipe);
	free_datapipe(&call_state_pipe);
	free_datapipe(&master_radio_pipe);
	free_datapipe(&system_state_pipe);
	free_datapipe(&heartbeat_pipe);

	/* Call the exit function for all subsystems */
	mce_gconf_exit();
	mce_dbus_exit();
	mce_conf_exit();

	/* If the mainloop is initialised, unreference it */
	if (mainloop != NULL) {
		g_main_loop_unref(mainloop);
		mainloop = 0;
	}

	/* Log a farewell message and close the log */
	mce_log(LL_INFO, "Exiting...");

	/* We do not need to explicitly close the log and doing so
	 * would not allow logging from atexit handlers */
	//mce_log_close();

	return status;
}
