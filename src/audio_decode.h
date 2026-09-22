/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WIIDESK_AUDIO_DECODE_H
#define WIIDESK_AUDIO_DECODE_H
#include <stdint.h>
#include <stddef.h>
struct audio_decoder;
struct audio_info { unsigned rate,channels; int estimated; int64_t frames; };
struct audio_decoder *audio_open(const char *,struct audio_info *,char *,size_t);
/* Output is native-endian signed 16-bit interleaved PCM. */
int audio_read(struct audio_decoder *,int16_t *,unsigned frames);
int64_t audio_seek(struct audio_decoder *,int64_t frame);
void audio_close(struct audio_decoder *);
const char *audio_error(struct audio_decoder *);
#endif
