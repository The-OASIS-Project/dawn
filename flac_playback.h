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
 * @param play An integer indicating the desired playback state.
 *             Set to 1 to start playback, or 0 to stop playback.
 */
void setMusicPlay(int play);

/**
 * Retrieves the current music playback state.
 *
 * @return An integer representing the playback state.
 *         Returns 1 if playback is active, or 0 if playback is stopped.
 */
int getMusicPlay(void);

/**
 * Plays a FLAC audio file.
 * This function sets up a FLAC decoder and a PulseAudio playback stream to play the specified FLAC audio file.
 * It utilizes callbacks for writing decoded audio to the PulseAudio stream, handling metadata, and managing errors.
 *
 * @param arg A pointer to a PlaybackArgs structure containing playback parameters such as the file name and PulseAudio sink name.
 * @return NULL always. This function does not return a value and is intended to be used with threading.
 *
 * The function performs the following steps:
 * 1. Initializes a PulseAudio playback stream with a specified sample format, channel count, and sample rate.
 * 2. Creates a new FLAC stream decoder and configures it for MD5 checking to verify the integrity of the decoded audio.
 * 3. Initializes the FLAC decoder with the specified file and sets up callbacks for writing audio data, handling metadata, and errors.
 * 4. Starts the decoding process and continues until the end of the stream or an error occurs.
 * 5. Cleans up resources by finishing the decoding process, deleting the decoder, and freeing the PulseAudio stream.
 * 6. If an error occurs during decoding, a callback is triggered to handle the next action, such as playing the next track.
 */
void *playFlacAudio(void *arg);

/**
 * @brief Sets the global music playback volume.
 *
 * This function adjusts the global volume level for music playback across the application.
 * It directly modifies the `global_volume` variable, which affects the volume at which audio is played.
 * The volume level should be specified as a float between 0.0 and 2.0, where 0.0 is complete silence,
 * 1.0 is the maximum volume level, greater than 1.0 is amplification.
 * Values outside this range may lead to undefined behavior.
 *
 * @param val The new volume level as a float. Valid values range from 0.0 to 2.0.
 * @note It's recommended to clamp the value of `val` within the 0.0 to 2.0 range before calling this function
 *       to avoid unexpected behavior.
 */
void setMusicVolume(float val);

#endif // FLAC_PLAYBACK_H

