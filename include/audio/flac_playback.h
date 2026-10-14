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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 */

#ifndef FLAC_PLAYBACK_H
#define FLAC_PLAYBACK_H

#include <stdint.h>

/**
 * @brief Structure containing arguments for audio file playback.
 *
 * This structure holds the parameters required to initiate audio playback of an audio file
 * using PulseAudio sinks.
 */
typedef struct {
   /**
    * @brief The PulseAudio sink name to play to.
    *
    * Specifies the name of the PulseAudio sink (output device) where the audio will be played.
    * This could be the name of a specific sound card, virtual sink, or any other valid sink
    * recognized by PulseAudio.
    */
   const char *sink_name;

   /**
    * @brief The full path to the audio file to play back.
    *
    * Contains the absolute or relative file path to the audio file that needs to be played.
    * The file should be in a format supported by the playback system (e.g., FLAC).
    */
   char *file_name;

   /**
    * @brief The time in seconds to start the playback.
    *
    * Indicates the starting point of the audio playback within the file, in seconds.
    * Playback will begin from this time offset into the audio file.
    *
    * @note If `start_time` exceeds the length of the audio file, playback may not occur
    * or may result in an error.
    */
   unsigned int start_time;
} PlaybackArgs;

/**
 * Sets the music playback state.
 *
 * Set it to 1 before starting playFlacAudio(): a 0 set while the track is
 * still opening (a Plex download) cancels the open.  Only the track's starter,
 * the music tool, sets 1; anything else that needs music stopped asks the music
 * tool (music_tool_pause()).  Setting 1 clears the saved
 * position, so a pause before the new track plays saves none.
 *
 * @param play An integer indicating the desired playback state.
 *             Set to 1 to start playback, or 0 to stop playback.
 */
void setMusicPlay(int play);

/**
 * Retrieves the current music playback state.
 *
 * @return 1 while playback is wanted: the track is opening (a Plex download) or
 *         playing; 0 once it's stopped or has ended with nothing to follow.
 */
int getMusicPlay(void);

/** Picks and starts the next track (on the playback thread, after its track) */
typedef void (*audio_playback_next_fn)(void);

/**
 * @brief Set who picks the next track when one ends naturally or won't open
 *
 * The music tool registers itself at startup, so this layer never calls up
 * into it (registered rather than a weak symbol: the music tool owns the
 * policy).  NULL (the default) means playback simply stops.
 */
void audio_playback_set_next_fn(audio_playback_next_fn fn);

/**
 * @brief Start a new run of tracks that wouldn't open (no playback thread running)
 *
 * The count carries over from track to track only along auto-advance; call it
 * before a play the user started.
 */
void audio_playback_reset_failed_opens(void);

/**
 * @brief Plays a music library track (FLAC, MP3, Ogg Vorbis, or other supported formats).
 *
 * Opens the track through music_open_track(): a local file inside [paths] music_dir, or a plex:
 * track, downloaded first.  Anything else is refused.  Despite the legacy function name, it
 * supports all formats registered with the audio_decoder subsystem.
 *
 * @param arg A pointer to a PlaybackArgs structure containing playback parameters such as the file
 * name and audio sink name.
 * @return NULL always. This function does not return a value and is intended to be used with
 * threading.
 *
 * The function performs the following steps:
 * 1. Opens the track (downloading a Plex track; a stop cancels the download).
 * 2. Initializes an audio playback stream with the detected sample format.
 * 3. Reads and plays audio samples, applying volume adjustment.
 * 4. Cleans up resources when playback completes or is stopped.
 * 5. At a natural end, or when a track won't open (until the 5th in a row, and not when the
 *    Plex server is unreachable), asks the music tool for the next track.
 *
 * @note The function name is retained for backward compatibility. Use playAudioFile() for new code.
 */
void *playFlacAudio(void *arg);

/**
 * @brief Plays a music library track (wrapper for playFlacAudio with clearer naming).
 *
 * This is the preferred entry point for playing library tracks. It supports all formats
 * registered with the audio_decoder subsystem (FLAC, MP3, Ogg Vorbis, etc.).
 *
 * @param arg A pointer to a PlaybackArgs structure containing playback parameters.
 * @return NULL always. Intended for use with pthread_create().
 *
 * @see playFlacAudio() for implementation details.
 */
static inline void *playAudioFile(void *arg) {
   return playFlacAudio(arg);
}

/**
 * @brief Sets the global music playback volume.
 *
 * This function adjusts the global volume level for music playback across the application.
 * It directly modifies the `global_volume` variable, which affects the volume at which audio is
 * played. The volume level should be specified as a float between 0.0 and 2.0, where 0.0 is
 * complete silence, 1.0 is the maximum volume level, greater than 1.0 is amplification. Values
 * outside this range may lead to undefined behavior.
 *
 * @param val The new volume level as a float. Valid values range from 0.0 to 2.0.
 * @note It's recommended to clamp the value of `val` within the 0.0 to 2.0 range before calling
 * this function to avoid unexpected behavior.
 */
void setMusicVolume(float val);

/**
 * @brief Gets the current global music playback volume.
 *
 * @return The current volume level as a float (0.0 to 2.0).
 */
float getMusicVolume(void);

/**
 * @brief Get current playback position in samples
 *
 * Returns the current playback position measured in samples per channel.
 * This is used for pause/resume functionality - the position is updated
 * continuously during playback and can be read to determine where to
 * resume from after a pause.
 *
 * @return Current position in samples (per channel), or 0 if not playing
 */
uint64_t audio_playback_get_position(void);

/**
 * @brief Get current playback sample rate
 *
 * Returns the sample rate of the currently playing track. This is cached
 * when playback starts, allowing callers to convert sample positions to
 * time without re-opening the audio file.
 *
 * @return Sample rate in Hz, or 0 if not playing
 */
uint32_t audio_playback_get_sample_rate(void);

#endif  // FLAC_PLAYBACK_H
