#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <errno.h>
#include "ipc.h"
#include "ui.h"

#define SHMEM_LEN ( \
	4 +   /* ipc_version;  Identifies the expected structure of the IPC block */ \
	4 +   /* playback_status;  Playing / paused */ \
	128 + /* station_name[128]; */ \
	128 + /* current_song_artist[128]; */ \
	128 + /* current_song_album[128]; */ \
	128 + /* current_song_title[128]; */ \
	128 + /* current_song_station[128]; */ \
	512 + /* current_song_coverart[512]; */ \
	4 +   /* current_song_duration; */ \
	4 +   /* current_song_played; */ \
	128 + /* next_song_artist[128]; */ \
	128 + /* next_song_album[128]; */ \
	128 + /* next_song_title[128]; */ \
	128 + /* next_song_station[128]; */ \
	512   /* next_song_coverart[512]; */ \
	)

#define PBIPC_VERSION 0x00000001
#define PBIPC_PLAYBACK_PAUSED 0
#define PBIPC_PLAYBACK_PLAYING 1

struct shmem_ptrs {
	uint32_t *ipc_version;
	uint32_t *playback_status;
	char *station_name;
	char *current_song_artist;
	char *current_song_album;
	char *current_song_title;
	char *current_song_station;
	char *current_song_coverart;
	uint32_t *current_song_duration;
	uint32_t *current_song_played;
	char *next_song_artist;
	char *next_song_album;
	char *next_song_title;
	char *next_song_station;
	char *next_song_coverart;
};

static struct shmem_ptrs sp;

static uint8_t *shmem = NULL;
int shmid = 0;

void BarShmemInit (BarApp_t *app, char *binPath) {
	key_t key = ftok(binPath, 1);
	printf("Key: %d\n", key);
	shmid = shmget(key, SHMEM_LEN, 0644|IPC_CREAT);
	if (shmid < 0) {
		if (errno == EEXIST) {
			struct shmid_ds buf;
			if(shmctl(shmid, IPC_STAT, &buf) < 0) {
				BarUiMsg (&app->settings, MSG_ERR, "Shared mem info get failed.\n");
				return;
			}
			if (buf.shm_nattch > 0) {
				BarUiMsg (&app->settings, MSG_ERR, "Shared mem already attached by another process.\n");
				return;
			} else { /* No one is attached to the old shm segment. */
				shmctl(shmid, IPC_RMID, NULL);
				shmid = shmget(key, SHMEM_LEN, 0644|IPC_CREAT);
				if (shmid < 0) {
					BarUiMsg (&app->settings, MSG_ERR, "Shared mem recreation failed.\n");
					return;
				}
			}
		} else {
			BarUiMsg (&app->settings, MSG_ERR, "Shared mem creation failed.\n");
			return;
		}
	}

	shmem = (uint8_t *)shmat (shmid, NULL, 0);
	if (shmem == (void *) -1) {
		BarUiMsg (&app->settings, MSG_ERR, "Shared mem shmat failed.\n");
		shmem = NULL;
	}

	sp.ipc_version = (uint32_t *)(shmem);
	sp.playback_status = (uint32_t *)(((void *)sp.ipc_version) + 4);
	sp.station_name = (char *)(((void *)sp.playback_status) + 4);
	sp.current_song_artist = (char *)(((void *)sp.station_name) + 128);
	sp.current_song_album = (char *)(((void *)sp.current_song_artist) + 128);
	sp.current_song_title = (char *)(((void *)sp.current_song_album) + 128);
	sp.current_song_station = (char *)(((void *)sp.current_song_title) + 128);
	sp.current_song_coverart = (char *)(((void *)sp.current_song_station) + 128);
	sp.current_song_duration = (uint32_t *)(((void *)sp.current_song_coverart) + 512);
	sp.current_song_played = (uint32_t *)(((void *)sp.current_song_duration) + 4);
	sp.next_song_artist = (char *)(((void *)sp.current_song_played) + 4);
	sp.next_song_album = (char *)(((void *)sp.next_song_artist) + 128);
	sp.next_song_title = (char *)(((void *)sp.next_song_album) + 128);
	sp.next_song_station = (char *)(((void *)sp.next_song_title) + 128);
	sp.next_song_coverart = (char *)(((void *)sp.next_song_station) + 128);


	*sp.ipc_version = PBIPC_VERSION;
	*sp.playback_status = PBIPC_PLAYBACK_PAUSED;
	sp.station_name[0] = '\0';
	sp.current_song_artist[0] = '\0';
	sp.current_song_album[0] = '\0';
	sp.current_song_title[0] = '\0';
	sp.current_song_station[0] = '\0';
	sp.current_song_coverart[0] = '\0';
	*sp.current_song_duration = 0;
	*sp.current_song_played = 0;
	sp.next_song_artist[0] = '\0';
	sp.next_song_album[0] = '\0';
	sp.next_song_title[0] = '\0';
	sp.next_song_station[0] = '\0';
	sp.next_song_coverart[0] = '\0';

	BarUiMsg (&app->settings, MSG_INFO, "Shared mem initialized.\n");
}

static void ShmemTouch (void) {
	struct shmid_ds buf;
	shmctl (shmid, IPC_STAT, &buf);
	shmctl (shmid, IPC_SET, &buf);
}

void BarShmemSetStrings (const PianoStation_t *curStation, const PianoSong_t *curSong,
		PianoStation_t *stations) {
	if (shmem == NULL) return;
	if (curStation == NULL) return;
	if (curSong == NULL) return;
	if (stations == NULL) return;

	strncpy (sp.station_name, curStation->name, 127);
	sp.station_name[127] = '\0';
	strncpy (sp.current_song_artist, curSong->artist, 127);
	sp.current_song_artist[127] = '\0';
	strncpy (sp.current_song_album, curSong->album, 127);
	sp.current_song_album[127] = '\0';
	strncpy (sp.current_song_title, curSong->title, 127);
	sp.current_song_title[127] = '\0';
	strncpy (sp.current_song_coverart, curSong->coverArt, 511);
	sp.current_song_coverart[511] = '\0';

	if (curStation->isQuickMix) {
		PianoStation_t *songStation = PianoFindStationById (stations, curSong->stationId);
		strncpy (sp.current_song_station, songStation->name, 127);
		sp.current_song_station[127] = '\0';
	} else {
		strncpy (sp.station_name, curStation->name, 127);
		sp.station_name[127] = '\0';
		sp.current_song_station[0] = '\0';
	}

	PianoSong_t * const nextSong = PianoListNextP (curSong);
	if (nextSong != NULL) {
		if (curStation->isQuickMix) {
			PianoStation_t *ss = PianoFindStationById (stations, nextSong->stationId);
			strncpy (sp.next_song_station, ss->name, 127);
			sp.next_song_station[127] = '\0';
		} else {
			sp.next_song_station[0] = '\0';
		}

		strncpy (sp.next_song_artist, nextSong->artist, 127);
		sp.next_song_artist[127] = '\0';
		strncpy (sp.next_song_album, nextSong->album, 127);
		sp.next_song_album[127] = '\0';
		strncpy (sp.next_song_title, nextSong->title, 127);
		sp.next_song_title[127] = '\0';
		strncpy (sp.next_song_coverart, nextSong->coverArt, 511);
		sp.next_song_coverart[511] = '\0';
	} else {
		sp.next_song_artist[0] = '\0';
		sp.next_song_album[0] = '\0';
		sp.next_song_title[0] = '\0';
		sp.next_song_station[0] = '\0';
		sp.next_song_coverart[0] = '\0';
	}

	ShmemTouch ();
}

void BarShmemSetTimes (const unsigned int songDuration, const unsigned int songPlayed) {
	if (shmem == NULL) return;

	*sp.current_song_duration = songDuration;
	*sp.current_song_played = songPlayed;

	ShmemTouch ();
}
