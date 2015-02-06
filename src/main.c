/*
Copyright (c) 2008-2013
	Lars-Dominik Braun <lars@6xq.net>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#ifndef __FreeBSD__
#define _POSIX_C_SOURCE 1 /* fileno() */
#define _BSD_SOURCE /* strdup() */
#define _DARWIN_C_SOURCE /* strdup() on OS X */
#endif

/* system includes */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
/* fork () */
#include <unistd.h>
#include <sys/select.h>
#include <time.h>
#include <ctype.h>
/* open () */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
/* tcset/getattr () */
#include <termios.h>
#include <assert.h>
#include <stdbool.h>
#include <limits.h>
#include <signal.h>
/* waitpid () */
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* pandora.com library */
#include <piano.h>

#include "main.h"
#include "terminal.h"
#include "config.h"
#include "ui.h"
#include "ui_dispatch.h"
#include "ui_readline.h"

/*	copy proxy settings to waitress handle
 */
static void BarMainLoadProxy (const BarSettings_t *settings,
		WaitressHandle_t *waith) {
	/* set up proxy (control proxy for non-us citizen or global proxy for poor
	 * firewalled fellows) */
	if (settings->controlProxy != NULL) {
		/* control proxy overrides global proxy */
		if (!WaitressSetProxy (waith, settings->controlProxy)) {
			/* if setting proxy fails, url is invalid */
			BarUiMsg (settings, MSG_ERR, "Control proxy (%s) is invalid!\n",
					 settings->controlProxy);
		}
	} else if (settings->proxy != NULL && strlen (settings->proxy) > 0) {
		if (!WaitressSetProxy (waith, settings->proxy)) {
			/* if setting proxy fails, url is invalid */
			BarUiMsg (settings, MSG_ERR, "Proxy (%s) is invalid!\n",
					 settings->proxy);
		}
	}
}

/*	authenticate user
 */
static bool BarMainLoginUser (BarApp_t *app) {
	PianoReturn_t pRet;
	WaitressReturn_t wRet;
	PianoRequestDataLogin_t reqData;
	bool ret;

	reqData.user = app->settings.username;
	reqData.password = app->settings.password;
	reqData.step = 0;

	BarUiMsg (&app->settings, MSG_INFO, "Login... ");
	ret = BarUiPianoCall (app, PIANO_REQUEST_LOGIN, &reqData, &pRet, &wRet);
	BarUiStartEventCmd (&app->settings, "userlogin", NULL, NULL, NULL, pRet,
			wRet);
	return ret;
}

/*	ask for username/password if none were provided in settings
 */
static bool BarMainGetLoginCredentials (BarSettings_t *settings,
		BarReadlineFds_t *input) {
	bool usernameFromConfig = true;

	if (settings->username == NULL) {
		char nameBuf[100];

		BarUiMsg (settings, MSG_QUESTION, "Email: ");
		BarReadlineStr (nameBuf, sizeof (nameBuf), input, BAR_RL_DEFAULT);
		settings->username = strdup (nameBuf);
		usernameFromConfig = false;
	}

	if (settings->password == NULL) {
		char passBuf[100];

		if (usernameFromConfig) {
			BarUiMsg (settings, MSG_QUESTION, "Email: %s\n", settings->username);
		}

		if (settings->passwordCmd == NULL) {
			BarUiMsg (settings, MSG_QUESTION, "Password: ");
			BarReadlineStr (passBuf, sizeof (passBuf), input, BAR_RL_NOECHO);
			/* write missing newline */
			puts ("");
			settings->password = strdup (passBuf);
		} else {
			pid_t chld;
			int pipeFd[2];

			BarUiMsg (settings, MSG_INFO, "Requesting password from external helper... ");

			if (pipe (pipeFd) == -1) {
				BarUiMsg (settings, MSG_NONE, "Error: %s\n", strerror (errno));
				return false;
			}

			chld = fork ();
			if (chld == 0) {
				/* child */
				close (pipeFd[0]);
				dup2 (pipeFd[1], fileno (stdout));
				execl ("/bin/sh", "/bin/sh", "-c", settings->passwordCmd, (char *) NULL);
				BarUiMsg (settings, MSG_NONE, "Error: %s\n", strerror (errno));
				close (pipeFd[1]);
				exit (1);
			} else if (chld == -1) {
				BarUiMsg (settings, MSG_NONE, "Error: %s\n", strerror (errno));
				return false;
			} else {
				/* parent */
				int status;

				close (pipeFd[1]);
				memset (passBuf, 0, sizeof (passBuf));
				read (pipeFd[0], passBuf, sizeof (passBuf)-1);
				close (pipeFd[0]);

				/* drop trailing newlines */
				ssize_t len = strlen (passBuf)-1;
				while (len >= 0 && passBuf[len] == '\n') {
					passBuf[len] = '\0';
					--len;
				}

				waitpid (chld, &status, 0);
				if (WEXITSTATUS (status) == 0) {
					settings->password = strdup (passBuf);
					BarUiMsg (settings, MSG_NONE, "Ok.\n");
				} else {
					BarUiMsg (settings, MSG_NONE, "Error: Exit status %i.\n", WEXITSTATUS (status));
					return false;
				}
			}
		} /* end else passwordCmd */
	}

	return true;
}

/*	get station list
 */
static bool BarMainGetStations (BarApp_t *app) {
	PianoReturn_t pRet;
	WaitressReturn_t wRet;
	bool ret;

	BarUiMsg (&app->settings, MSG_INFO, "Get stations... ");
	ret = BarUiPianoCall (app, PIANO_REQUEST_GET_STATIONS, NULL, &pRet, &wRet);
	BarUiStartEventCmd (&app->settings, "usergetstations", NULL, NULL,
			app->ph.stations, pRet, wRet);
	return ret;
}

/*	get initial station from autostart setting or user input
 */
static void BarMainGetInitialStation (BarApp_t *app) {
	/* try to get autostart station */
	if (app->settings.autostartStation != NULL) {
		app->curStation = PianoFindStationById (app->ph.stations,
				app->settings.autostartStation);
		if (app->curStation == NULL) {
			BarUiMsg (&app->settings, MSG_ERR,
					"Error: Autostart station not found.\n");
		}
	}
	/* no autostart? ask the user */
	if (app->curStation == NULL) {
		app->curStation = BarUiSelectStation (app, app->ph.stations,
				"Select station: ", NULL, app->settings.autoselect);
	}
	if (app->curStation != NULL) {
		BarUiPrintStation (&app->settings, app->curStation);
	}
}

/*	print song duration
 */
static void BarMainPrintTime (BarApp_t * const app) {
	unsigned int songRemaining;
	char sign;

	if (app->player.songPlayed <= app->player.songDuration) {
		songRemaining = app->player.songDuration - app->player.songPlayed;
		sign = '-';
	} else {
		/* longer than expected */
		songRemaining = app->player.songPlayed - app->player.songDuration;
		sign = '+';
	}
	BarUiMsg (&app->settings, MSG_TIME, "%c%02u:%02u/%02u:%02u\r",
			sign, songRemaining / 60, songRemaining % 60,
			app->player.songDuration / 60,
			app->player.songDuration % 60);
}

#define max(a,b) ((a) > (b) ? (a) : (b))

/*	wait for user input
 */
static void BarMainHandleUserInput (BarApp_t *app) {
	const BarReadlineFds_t * const input = &app->input;
	fd_set set;
	memcpy (&set, &input->set, sizeof (set));
	FD_SET(app->player.stdout, &set);
	FD_SET(app->player.stderr, &set);
	const int maxfd = max (max (input->maxfd, app->player.stdout),
			app->player.stderr) + 1;

	while (true) {
		fd_set tmpset;
		memcpy (&tmpset, &set, sizeof (tmpset));
		int ret = select (maxfd, &tmpset, NULL, NULL, NULL);
		assert (ret > 0);

		if (FD_ISSET(app->player.stdout, &tmpset)) {
			if (!BarPlayerIO (&app->player, app->player.stdout)) {
				break;
			}
			BarMainPrintTime (app);
		} else if (FD_ISSET(app->player.stderr, &tmpset)) {
			if (!BarPlayerIO (&app->player, app->player.stderr)) {
				break;
			}
			BarMainPrintTime (app);
		} else {
			int fd = -1;
			if (FD_ISSET(input->fds[0], &tmpset)) {
				fd = input->fds[0];
			} else if (FD_ISSET(input->fds[1], &tmpset)) {
				fd = input->fds[1];
			}
			char buf = 0;
			ret = read (fd, &buf, sizeof (buf));
			assert (ret != -1);
			const BarKeyShortcutId_t key = BarUiDispatch (app, buf,
					app->curStation, app->playlist, true, BAR_DC_GLOBAL);
			if (key == BAR_KS_COUNT) {
				/* redirect unknown keys to player */
				ret = write (app->player.stdin, &buf, sizeof (buf));
				assert (ret != -1);
			}
		}
	}
}

/*	fetch new playlist
 */
static void BarMainGetPlaylist (BarApp_t *app) {
	PianoReturn_t pRet;
	WaitressReturn_t wRet;
	PianoRequestDataGetPlaylist_t reqData;
	reqData.station = app->curStation;
	reqData.quality = app->settings.audioQuality;

	BarUiMsg (&app->settings, MSG_INFO, "Receiving new playlist... ");
	if (!BarUiPianoCall (app, PIANO_REQUEST_GET_PLAYLIST,
			&reqData, &pRet, &wRet)) {
		app->curStation = NULL;
	} else {
		app->playlist = reqData.retPlaylist;
		if (app->playlist == NULL) {
			BarUiMsg (&app->settings, MSG_INFO, "No tracks left.\n");
			app->curStation = NULL;
		}
	}
	BarUiStartEventCmd (&app->settings, "stationfetchplaylist",
			app->curStation, app->playlist, app->ph.stations, pRet, wRet);
}

/*	start new player
 */
static bool BarMainStartPlayback (BarApp_t *app) {
	assert (app != NULL);

	const PianoSong_t * const curSong = app->playlist;
	assert (curSong != NULL);

	BarUiPrintSong (&app->settings, curSong, app->curStation->isQuickMix ?
			PianoFindStationById (app->ph.stations,
			curSong->stationId) : NULL);

	if (!BarPlayerPlay (&app->player, curSong)) {
		BarUiMsg (&app->settings, MSG_ERR, "Cannot start player.\n");
		return false;
	}

	BarUiStartEventCmd (&app->settings, "songstart", app->curStation,
			curSong, app->ph.stations, PIANO_RET_OK, WAITRESS_RET_OK);

	return true;
}

static void BarMainFinishPlayback (BarApp_t * const app) {
	assert (app != NULL);

	BarPlayerCleanup (&app->player);
	if (app->player.errors >= app->settings.maxPlayerErrors) {
		/* don't continue playback if player reports too many
		 * errors */
		app->curStation = NULL;
	}

	BarUiStartEventCmd (&app->settings, "songfinish", app->curStation,
			app->playlist, app->ph.stations, PIANO_RET_OK,
			WAITRESS_RET_OK);
}

/*	main loop
 */
static void BarMainLoop (BarApp_t *app) {
	if (!BarMainGetLoginCredentials (&app->settings, &app->input)) {
		return;
	}

	BarMainLoadProxy (&app->settings, &app->waith);

	if (!BarMainLoginUser (app)) {
		return;
	}

	if (!BarMainGetStations (app)) {
		return;
	}

	BarMainGetInitialStation (app);

	while (!app->quit) {
		/* start next song */
		if (app->curStation != NULL) {
			/* what's next? */
			if (app->playlist != NULL) {
				PianoSong_t * const histsong = app->playlist;
				app->playlist = PianoListNextP (app->playlist);
				histsong->head.next = NULL;
				BarUiHistoryPrepend (app, histsong);
			}
			if (app->playlist == NULL) {
				BarMainGetPlaylist (app);
			}
			/* song ready to play */
			if (app->playlist != NULL) {
				if (!BarMainStartPlayback (app)) {
					/* hard failure */
					app->curStation = NULL;
				}
			}
		}

		BarMainHandleUserInput (app);

		BarMainFinishPlayback (app);
	}
}

int main (int argc, char **argv) {
	static BarApp_t app;

	memset (&app, 0, sizeof (app));

	/* save terminal attributes, before disabling echoing */
	BarTermInit ();

	/* signals */
	signal (SIGPIPE, SIG_IGN);

	/* init some things */
	gcry_check_version (NULL);
	gcry_control (GCRYCTL_DISABLE_SECMEM, 0);
	gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);
	gnutls_global_init ();

	BarPlayerInit (&app.player);
	BarSettingsInit (&app.settings);
	BarSettingsRead (&app.settings);

	PianoReturn_t pret;
	if ((pret = PianoInit (&app.ph, app.settings.partnerUser,
			app.settings.partnerPassword, app.settings.device,
			app.settings.inkey, app.settings.outkey)) != PIANO_RET_OK) {
		BarUiMsg (&app.settings, MSG_ERR, "Initialization failed:"
				" %s\n", PianoErrorToStr (pret));
		return 0;
	}

	BarUiMsg (&app.settings, MSG_NONE,
			"Welcome to " PACKAGE " (" VERSION ")! ");
	if (app.settings.keys[BAR_KS_HELP] == BAR_KS_DISABLED) {
		BarUiMsg (&app.settings, MSG_NONE, "\n");
	} else {
		BarUiMsg (&app.settings, MSG_NONE,
				"Press %c for a list of commands.\n",
				app.settings.keys[BAR_KS_HELP]);
	}

	if (app.settings.playerCommand == NULL) {
		BarUiMsg (&app.settings, MSG_ERR, "No audio player selected. Please set "
				"player_command in your config file.");
	}

	WaitressInit (&app.waith);
	app.waith.url.host = app.settings.rpcHost;
	app.waith.url.tlsPort = app.settings.rpcTlsPort;
	app.waith.tlsFingerprint = app.settings.tlsFingerprint;

	/* init fds */
	FD_ZERO(&app.input.set);
	app.input.fds[0] = STDIN_FILENO;
	FD_SET(app.input.fds[0], &app.input.set);

	/* open fifo read/write so it won't EOF if nobody writes to it */
	assert (sizeof (app.input.fds) / sizeof (*app.input.fds) >= 2);
	app.input.fds[1] = open (app.settings.fifo, O_RDWR);
	if (app.input.fds[1] != -1) {
		struct stat s;

		/* check for file type, must be fifo */
		fstat (app.input.fds[1], &s);
		if (!S_ISFIFO (s.st_mode)) {
			BarUiMsg (&app.settings, MSG_ERR, "File at %s is not a fifo\n",
					app.settings.fifo);
			close (app.input.fds[1]);
			app.input.fds[1] = -1;
		} else {
			FD_SET(app.input.fds[1], &app.input.set);
			BarUiMsg (&app.settings, MSG_INFO, "Control fifo at %s opened\n",
					app.settings.fifo);
		}
	}
	app.input.maxfd = max (app.input.fds[0], app.input.fds[1]);
	++app.input.maxfd;

	BarMainLoop (&app);

	if (app.input.fds[1] != -1) {
		close (app.input.fds[1]);
	}

	/* write statefile */
	BarSettingsWrite (app.curStation, &app.settings);

	PianoDestroy (&app.ph);
	PianoDestroyPlaylist (app.songHistory);
	PianoDestroyPlaylist (app.playlist);
	WaitressFree (&app.waith);
	gnutls_global_deinit ();
	BarSettingsDestroy (&app.settings);

	/* restore terminal attributes, zsh doesn't need this, bash does... */
	BarTermRestore ();

	return 0;
}

