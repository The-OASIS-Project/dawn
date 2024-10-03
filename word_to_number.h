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

#ifndef WORD_TO_NUMBER_H
#define WORD_TO_NUMBER_H

/**
 * Converts a textual representation of a number into a double-precision floating-point number.
 * Supports magnitudes from "thousand" to "trillion" and can handle decimal fractions indicated by "point".
 *
 * @param originalWord The string representation of the number (e.g., "two thousand twenty-one point five").
 * @return The numerical value as a double. Returns 0.0 for unrecognizable tokens.
 *
 * Note: This function uses `parseNumericalWord` for converting individual word tokens to numbers,
 * which should be defined elsewhere to handle basic numeric words and teens.
 */
double wordToNumber(char *originalWord);

#endif // WORD_TO_NUMBER_H
