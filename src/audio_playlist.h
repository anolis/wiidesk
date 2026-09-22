/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WIIDESK_AUDIO_PLAYLIST_H
#define WIIDESK_AUDIO_PLAYLIST_H
#include <limits.h>
#define AUDIO_TRACKS 128
struct audio_playlist { int count,ok; char error[200]; char paths[AUDIO_TRACKS][PATH_MAX]; };
void audio_playlist_load(const char *,struct audio_playlist *);
#endif
