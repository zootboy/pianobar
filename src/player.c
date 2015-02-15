/*
Copyright (c) 2015
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

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include <piano.h>

#include "player.h"
#include "ui.h"

/*	Initialize player structure; command should be a shell expression
 */
void BarPlayerInit (BarPlayer * const player, const char * const command) {
	assert (player != NULL);
	assert (command != NULL);

	player->stdin = player->stdout = player->stderr = -1;
	player->pid = BAR_NO_PLAYER;
	player->command = command;
}

bool BarPlayerPlay (BarPlayer * const player, const PianoSong_t * const song) {
	assert (player != NULL);
	assert (song != NULL);

	static const char httpPrefix[] = "http://";
	/* only play http uris which do not contain a ' (see execl below) */
	if (song->audioUrl == NULL ||
			strncmp (song->audioUrl, httpPrefix, strlen (httpPrefix)) != 0 ||
			strchr (song->audioUrl, (int) '\'') != NULL) {
		return false;
	}

	int stdinpipe[2], stdoutpipe[2], stderrpipe[2];
	int ret = pipe (stdinpipe);
	assert (ret != -1);
	ret = pipe (stdoutpipe);
	assert (ret != -1);
	ret = pipe (stderrpipe);
	assert (ret != -1);
	const pid_t pid = fork ();
	assert (pid != -1);
	if (pid == 0) {
		/* child */
		/* close write end */
		close (stdinpipe[1]);
		/* close read end */
		close (stdoutpipe[0]);
		close (stderrpipe[0]);
		/* connect stdin to read-end of pipe */
		ret = dup2 (stdinpipe[0], STDIN_FILENO);
		assert (ret != -1);
		/* and stdout to write-end of pipe */
		ret = dup2 (stdoutpipe[1], STDOUT_FILENO);
		assert (ret != -1);
		ret = dup2 (stderrpipe[1], STDERR_FILENO);
		assert (ret != -1);

		char gain[16];
		snprintf (gain, sizeof (gain), "%.2f", song->fileGain);

		setenv ("audioUrl", song->audioUrl, 1);
		setenv ("gain", gain, 1);

		/* XXX: mpv problem: tcgetpgrp is not guarded by isatty, returns -1 */
		ret = execl ("/bin/sh", "/bin/sh", "-c", player->command, (void *) NULL);
		assert (ret != -1);
		return EXIT_FAILURE;
	} else {
		/* parent */
		/* close read end */
		close (stdinpipe[0]);
		/* close write end */
		close (stdoutpipe[1]);
		close (stderrpipe[1]);

		player->stdin = stdinpipe[1];
		player->stdout = stdoutpipe[0];
		player->stderr = stderrpipe[0];
		player->songDuration = song->length;
		player->songPlayed = 0;
		player->pid = pid;
		return true;
	}
}

/*	Read player’s stdout/stderr; should be called regularly as most programs
 *	block if the pipe is full (music stops)
 */
bool BarPlayerIO (BarPlayer * const player, const int fd) {
	assert (player != NULL);
	assert (fd >= 0);

	char buf[1024];
	int ret = read (fd, buf, sizeof (buf)-1);
	if (ret == 0) {
		/* player quit */
		return false;
	}
	assert (ret != -1);
	buf[ret] = '\0';

	/* parse player output */
	char *start = buf, *end = buf;
	while ((end = strchr (start, (int) '\n')) != NULL) {
		assert (end < buf+sizeof (buf));
		*end = '\0';
		float position, duration;
		if (sscanf (start, "%f %f", &position, &duration) == 2) {
			player->songPlayed = position;
			player->songDuration = duration;
		}
		start = end+1;
	}

	return true;
}

/*	Skip current song
 */
void BarPlayerSkip (BarPlayer * const player) {
	assert (player != NULL);
	assert (player->pid != BAR_NO_PLAYER);
	kill (player->pid, SIGTERM);
}

/*	Cleanup zombie and check return status
 */
void BarPlayerCleanup (BarPlayer * const player) {
	int status;
	const pid_t finished = waitpid (player->pid, &status, 0);
	assert (finished != -1);
	assert (finished == player->pid);

	close (player->stdin);
	close (player->stdout);
	close (player->stderr);
	player->stdin = player->stdout = player->stderr = -1;
	player->pid = BAR_NO_PLAYER;

	if (WEXITSTATUS(status) == EXIT_SUCCESS) {
		player->errors = 0;
	} else {
		++player->errors;
	}
}

