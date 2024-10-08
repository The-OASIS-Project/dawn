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

#ifndef AUDIO_UTILS_H
#define AUDIO_UTILS_H

/**
 * Calculates the Root Mean Square (RMS) value of an audio signal.
 * RMS is a statistical measure of the magnitude of a varying quantity and
 * is used here to estimate the power of an audio signal.
 *
 * @param audioBuffer Pointer to the buffer containing 16-bit signed audio samples.
 * @param numSamples The number of samples in the audio buffer.
 * @return The calculated RMS value as a double.
 */
double calculateRMS(const int16_t *audioBuffer, size_t numSamples);

#endif // AUDIO_UTILS_H

