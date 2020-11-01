#include "main.h"

void BarShmemInit (BarApp_t *app, char *binPath);
void BarShmemSetStrings (const PianoStation_t *curStation, const PianoSong_t *curSong,
		PianoStation_t *stations);
void BarShmemSetTimes (BarApp_t *app);
