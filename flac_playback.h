/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * All contributions to this project are agreed to be licensed under the
 * GPLv3 or any later version. Contributions are understood to be
 * any modifications, enhancements, or additions to the project
 * and become the property of the original author Kris Kersey.
 */

#ifndef FLAC_PLAYBACK_H
#define FLAC_PLAYBACK_H

typedef struct {
   const char *sink_name;
   char *file_name;
   unsigned int start_time;
} PlaybackArgs;

void setMusicPlay(int play);
int getMusicPlay(void);
void *playFlacAudio(void *arg);
void setMusicVolume(float val);

#endif // FLAC_PLAYBACK_H

