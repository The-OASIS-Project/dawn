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

#ifndef MIC_PASSTHROUGH_H
#define MIC_PASSTHROUGH_H

/**
 * @brief Captures audio from an input device and plays it back through an output device in real-time.
 *
 * This function runs in a separate thread and continuously captures audio from a specified input device,
 * playing it back through a specified output device, effectively amplifying the captured sound.
 * The implementation depends on the compile-time option and uses either ALSA or PulseAudio for audio handling.
 *
 * @param arg Unused parameter, included for compatibility with pthreads' start routine signature.
 * @return NULL Always returns NULL, indicating the thread has completed execution.
 *
 * @details
 * **Implementation Details Based on Compile-Time Option:**
 *
 * - **ALSA Implementation (when `ALSA_DEVICE` is defined):**
 *   - Uses ALSA's API to open PCM devices for capture and playback.
 *   - Handles audio data transfer between these devices.
 *   - **Prerequisites:**
 *     - ALSA (Advanced Linux Sound Architecture) must be supported and configured on the system.
 *     - Proper ALSA devices must be available and specified by the user or configuration.
 *   - **Notes:**
 *     - Error handling is incorporated to address issues with device initialization and audio data transfer.
 *     - The global `running` variable controls the main loop. Use `setStopVA` to request thread termination.
 *
 * - **PulseAudio Implementation (when `ALSA_DEVICE` is not defined):**
 *   - Uses PulseAudio for both audio capture and playback.
 *   - Initializes PulseAudio streams for both input and output using the specified device names.
 *   - **Prerequisites:**
 *     - PulseAudio must be supported and properly configured on the system.
 *     - The specified audio capture (microphone) and playback (speakers) devices must be available.
 *   - **Notes:**
 *     - The global `running` variable controls the loop execution, enabling external control to start or stop the voice amplification.
 *     - Proper error handling is implemented to manage issues during audio capture and playback initialization and operation.
 *     - Resource management ensures that PulseAudio streams are freed appropriately before thread termination.
 *
 * **General Notes:**
 * - This function is intended to be run in a separate thread.
 * - Execution continues until `setStopVA` is called to set the global `running` flag to `0`.
 * - Error handling is implemented to manage issues during audio capture and playback initialization and operation.
 */
void* voiceAmplificationThread(void* arg);

/**
 * @brief Signals the voice amplification thread to stop execution.
 *
 * This function sets the global `running` flag to `0`, which the `voiceAmplificationThread` function monitors.
 * When the flag is set to `0`, the voice amplification thread will exit its main loop and terminate gracefully.
 *
 * @note
 * - This function should be called when you need to stop the voice amplification, such as during application shutdown
 *   or when disabling the voice amplification feature.
 * - Ensure that the `voiceAmplificationThread` is properly checking the `running` flag in its loop.
 * - The function is thread-safe if `running` is declared as `volatile sig_atomic_t` or appropriate synchronization is used.
 *
 * @see voiceAmplificationThread()
 */
void setStopVA(void);

#endif // MIC_PASSTHROUGH_H

