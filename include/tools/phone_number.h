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
 * Phone numbers in one form, for comparing, storing and dialing.  Pure: no
 * DAWN state.  Shared by the phone tool and contact resolution (email too).
 */

#ifndef PHONE_NUMBER_H
#define PHONE_NUMBER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Normalize a phone number for DB matching.
 *
 * LLMs emit phone numbers in many formats ("+1-555-123-4567", "(555) 123-4567",
 * "1 555 123 4567"). Stored rows are E.164 without punctuation ("+15551234567").
 * Strip whitespace/dashes/parens/dots, keep digits and an optional leading '+',
 * prefix a bare 10-digit US number with "+1". Used on both ingestion (inbound
 * SMS sender, outbound contact resolution) and delete (by-number) paths so rows
 * match on any LLM-supplied format.
 *
 * @param in       Raw input (may be NULL/empty → out receives "").
 * @param out      Output buffer.
 * @param out_size Size of out buffer.
 */
void phone_number_normalize(const char *in, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* PHONE_NUMBER_H */
