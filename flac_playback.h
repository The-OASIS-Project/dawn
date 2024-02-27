#ifndef FLAC_PLAYBACK_H
#define FLAC_PLAYBACK_H

typedef struct {
   char *sink_name;
   char *file_name;
   unsigned int start_time;
} PlaybackArgs;

void setMusicPlay(int play);
int getMusicPlay(void);
void *playFlacAudio(void *arg);

#endif // FLAC_PLAYBACK_H

