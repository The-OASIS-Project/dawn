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
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s).
 *
 * The local speaker: a music player on its owner's queue, played through the
 * audio backend (ALSA or PulseAudio), plus the speaker's volume and ducking.
 */

#ifndef MUSIC_SPEAKER_H
#define MUSIC_SPEAKER_H

#include <stdbool.h>

#include "audio/music_player.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Name the device the speaker plays on: after the audio backend starts,
 *        and whenever the playback device is switched (playing music moves to
 *        it).  Without one, or without a backend, there is no speaker.
 */
void music_speaker_set_device(const char *device);

/**
 * @brief The speaker's player, playing @p owner_user_id's queue, referenced:
 *        release it with music_player_release()
 *
 * Created on first use.  When the owner differs from the one the speaker
 * plays for, the old owner's playback stops and a player on the new owner's
 * queue takes its place.  The caller decides who may use the speaker.
 *
 * @return The player, or NULL with no speaker (no device or backend, or
 *         after music_speaker_cleanup())
 */
music_player_t *music_speaker_acquire(int owner_user_id);

/**
 * @brief Pause the speaker (a phone call, or the local mic's spoken cancel),
 *        keeping its place, waiting briefly for its device to close
 *
 * @return true if music was playing and is now paused
 */
bool music_speaker_pause(void);

/** @brief Whether the speaker's device is open (sound is going out); lock-free */
bool music_speaker_is_playing(void);

/** @brief Lower the speaker's music while someone speaks, or restore it */
void music_speaker_set_ducked(bool ducked);

/** @brief Stop the speaker for good (shutdown, before the audio backend goes) */
void music_speaker_cleanup(void);

/** @brief Set the speaker's music volume, 0.0 to 2.0 (above 1.0 amplifies) */
void setMusicVolume(float val);

/** @brief The speaker's music volume, 0.0 to 2.0 (not lowered by ducking) */
float getMusicVolume(void);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_SPEAKER_H */
