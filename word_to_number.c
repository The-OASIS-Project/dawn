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

#include <stdio.h>
#include <string.h>

#include <stdio.h>
#include <string.h>

typedef struct {
   char *name;
   double multiplier;
} Magnitude;

/**
 * Converts English textual numerical representations into their integer equivalents.
 * Currently supports numbers from 0 to 99, including unit numbers (0-9),
 * teen numbers (10-19), and tens (20, 30, ..., 90).
 *
 * @param token The textual representation of a number (e.g., "one", "twenty", "fourteen").
 * @return The integer value of the number, or 0 if the token does not represent a known number.
 *
 * Note: This function returns 0 both for the textual representation "zero" and for any unrecognized token.
 *       Consider improving error handling to distinguish these cases if necessary.
 */
int parseNumericalWord(const char *token)
{
   char *units[] = {
      "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"
   };

   char *tens[] = {
      "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"
   };

   char *teens[] = {
      "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen",
      "eighteen", "nineteen"
   };

   for (int i = 0; i < 10; i++) {
      if (strcmp(token, units[i]) == 0)
         return i;
      if (strcmp(token, tens[i]) == 0)
         return i * 10;
      if (strcmp(token, teens[i]) == 0)
         return 10 + i;
   }

   return 0;                    // or handle error
}

double wordToNumber(char *originalWord)
{
   char word[1024]; // Temporary buffer for tokenization.
   strncpy(word, originalWord, sizeof(word) - 1); // Ensure null-termination.
   word[sizeof(word) - 1] = '\0';

   Magnitude magnitudes[] = {
      {"thousand", 1000},
      {"million", 1000000},
      {"billion", 1000000000},
      {"trillion", 1000000000000} // Extendable for more magnitudes.
   };

   double result = 0.0; // Accumulator for the result.
   double tempValue = 0.0; // Temporary accumulator for the current numeric segment.
   char *token = strtok(word, " "); // Tokenize the input string by spaces.

   while (token != NULL) {
      if (strcmp(token, "point") == 0) {
         break; // Transition to processing the fractional part.
      } else if (strcmp(token, "hundred") == 0) {
         tempValue *= 100;
      } else {
         int found = 0; // Flag to track if the token matches a magnitude.
         for (int i = 0; i < sizeof(magnitudes) / sizeof(Magnitude); i++) {
            if (strcmp(token, magnitudes[i].name) == 0) {
               result += tempValue * magnitudes[i].multiplier;
               tempValue = 0;
               found = 1;
               break;
            }
         }

         if (!found) { // If not a magnitude, attempt to parse as a number.
            tempValue += parseNumericalWord(token);
         }
      }

      token = strtok(NULL, " ");
   }
   result += tempValue; // Add any remaining value to the result.

   // Process fractional part if "point" was found.
   strncpy(word, originalWord, sizeof(word));
   char *decimalPart = strstr(word, "point");
   if (decimalPart) {
      char *token = strtok(decimalPart + strlen("point"), " ");
      double fractionalValue = 0.0;
      int decimalDigits = 0;
      while (token) {
         fractionalValue = fractionalValue * 10 + parseNumericalWord(token);
         decimalDigits++;
         token = strtok(NULL, " ");
      }
      for (int i = 0; i < decimalDigits; i++) {
         fractionalValue /= 10;
      }
      result += fractionalValue;        // Add the fractional part to the result
   }

   return result;
}

#if 0
int main()
{
   char *testExamples[] = {
      "eighteen", "seven hundred fifty six", "four thousand twenty five",
      "sixty nine thousand three hundred twenty seven",
      "five hundred twelve thousand three hundred forty six",
      "ninety nine thousand nine hundred ninety nine",
      "three hundred seventy five thousand eight hundred sixty two",
      "sixty three thousand four hundred twelve",
      "two hundred fifty thousand",
      "twenty seven thousand four hundred eighty three",
      "six hundred fifty",
      "one hundred ten thousand",
      "three hundred forty five thousand one hundred twenty nine",
      "eight thousand seven hundred",
      "eighty five thousand three hundred twenty one",
      "seventy six thousand nine hundred forty five",
      "four hundred fifty thousand",
      "sixty thousand seven hundred",
      "ninety eight thousand two hundred seventy",
      "forty thousand five hundred",
      "one million eighteen",
      "five hundred million", "eight hundred ninety six thousand",
      "three million four hundred fifty thousand nine hundred",
      "seven hundred forty three thousand two hundred fifty six",
      "sixty million five hundred thousand",
      "ninety eight million seven hundred thousand five hundred forty three",
      "nine hundred ninety nine million nine hundred ninety nine thousand nine hundred ninety nine",
      "two hundred fifty thousand two hundred fifty",
      "one hundred eighty million eight hundred thousand three hundred twenty one",
      "eighty seven million two hundred fifty six thousand four hundred seventy three",
      "four million two hundred twenty two thousand two hundred twenty two",
      "three hundred twenty four million five hundred sixty seven thousand one hundred eighty nine",
      "sixty million four hundred twenty thousand one hundred sixty two",
      "eight hundred thousand five hundred",
      "five hundred seventy three million four hundred fifty thousand twenty one",
      "four million",
      "nine hundred eighty seven million six hundred fifty four thousand three hundred twenty one",
      "one million eight hundred seventy five thousand",
      "three million five hundred",
      "seven hundred sixty four million two hundred three thousand eight hundred ninety one",
      "nine hundred ninety nine billion nine hundred ninety nine million nine hundred ninety nine thousand nine hundred ninety nine",
      "one trillion two hundred thirty four billion five hundred sixty seven million eight hundred ninety thousand one hundred twenty three",
      "five trillion six hundred seventy eight billion nine hundred one million two hundred thirty four thousand five hundred sixty seven",
      "seven trillion eight hundred ninety billion one hundred twenty three million four hundred fifty six thousand seven hundred eighty nine",
      "two trillion three hundred forty five billion six hundred seventy eight million nine hundred twelve thousand three hundred forty five",
      "four trillion five hundred sixty seven billion eight hundred ninety one million two hundred thirty four thousand five hundred sixty seven",
      "six trillion seven hundred eighty nine billion one hundred twenty three million four hundred fifty six thousand seven hundred eighty nine",
      "eight trillion nine hundred one billion two hundred thirty four million five hundred sixty seven thousand eight hundred ninety",
      "three trillion four hundred fifty six billion seven hundred eighty nine million one hundred twenty three thousand four hundred fifty six",
      "five trillion six hundred seventy eight billion nine hundred twelve million three hundred forty five thousand six hundred seventy eight",
      "seven trillion eight hundred ninety one billion two hundred thirty four million five hundred sixty seven thousand eight hundred ninety one",
      "nine trillion one hundred twenty three billion four hundred fifty six million seven hundred eighty nine thousand twelve",
      "three trillion four hundred fifty six billion seven hundred eighty nine million twelve thousand three hundred forty five",
      "five trillion six hundred seventy eight billion nine hundred twelve million three hundred forty five thousand six hundred seventy nine",
      "seven trillion eight hundred ninety one billion two hundred thirty four million five hundred sixty seven thousand eight hundred ninety two",
      "nine trillion twelve billion three hundred forty five million six hundred seventy eight thousand nine hundred twelve",
      "three trillion four hundred fifty six billion seven hundred eighty nine million one hundred twenty three thousand four hundred fifty seven",
      "five trillion six hundred seventy eight billion nine hundred twelve million three hundred forty five thousand six hundred seventy eight",
      "seven trillion eight hundred ninety one billion two hundred thirty four million five hundred sixty seven thousand eight hundred ninety three",
      "nine trillion one hundred twenty three billion four hundred fifty six million seven hundred eighty nine thousand one hundred twenty three",
      "three point one four one five nine",
      "one hundred twenty three point four five six",
      "nine hundred ninety nine billion nine point nine nine nine",
      "seven trillion eight hundred ninety billion point one two three four five six",
      "four million five hundred sixty seven point eight nine one",
      "sixty seven million eight hundred ninety one thousand two hundred thirty four point five six seven eight",
      "eight trillion point nine zero one two",
      "three hundred forty five billion six hundred seventy eight million nine hundred twelve point three four five six",
      "five trillion point six seven eight nine",
      "seven hundred eighty nine million point one two three four",
      "nine trillion twelve billion three hundred forty five million point six seven eight nine",
      "three trillion four hundred fifty six billion point seven eight nine",
      "five trillion six hundred seventy eight billion nine hundred twelve million three point four five six seven eight",
      "eight hundred ninety one billion two hundred thirty four million five hundred sixty seven point eight nine",
      "nine trillion point one two three four five six",
      "three trillion four hundred fifty six billion seven hundred eighty nine million point nine",
      "five trillion six hundred seventy eight billion point nine one two three",
      "seven trillion eight hundred ninety one billion two hundred thirty four million point five six seven eight",
      "nine trillion one hundred twenty three billion four hundred fifty six million seven hundred eighty nine point zero one two three",
      "zero point one eight nine",
      "zero point zero zero one four five"
   };

   double testSolutions[] = {
      18, 756, 4025, 69327, 512346, 99999, 375862, 63412, 250000, 27483,
      650, 110000, 345129, 8700, 85321, 76945, 450000, 60700, 98270, 40500,
      1000018,
      500000000, 896000, 3450900, 743256, 60500000, 98700543, 999999999, 250250,
      180800321, 87256473,
      4222222, 324567189, 60420162, 800500, 573450021, 4000000, 987654321,
      1875000, 3000500, 764203891,
      999999999999,
      1234567890123,
      5678901234567,
      7890123456789,
      2345678912345,
      4567891234567,
      6789123456789,
      8901234567890,
      3456789123456,
      5678912345678,
      7891234567891,
      9123456789012,
      3456789012345,
      5678912345679,
      7891234567892,
      9012345678912,
      3456789123457,
      5678912345678,
      7891234567893,
      9123456789123,
      3.14159,
      123.456,
      999000000009.999,
      7890000000000.123456,
      4000567.891,
      67891234.5678,
      8000000000000.9012,
      345678000912.3456,
      5000000000000.6789,
      789000000.1234,
      9012345000000.6789,
      3456000000000.789,
      5678912000003.45678,
      891234000567.89,
      9000000000000.123456,
      3456789000000.9,
      5678000000000.9123,
      7891234000000.5678,
      9123456789.0123,
      0.189,
      0.00145
   };

   printf("testExamples: %ld\ntestSolutions: %ld\n", sizeof(testExamples), sizeof(testSolutions));

   int totalExamples = sizeof(testExamples) / sizeof(testExamples[0]);
   int pass = 0;
   int fail = 0;

   printf("Testing wordToNumber function with %d examples:\n", totalExamples);

   for (int i = 0; i < totalExamples; i++) {
      char input[1024];
      strcpy(input, testExamples[i]);

      double result = wordToNumber(input);

      if (strcmp(input, "quit") == 0) {
         printf("Exiting the loop early.\n");
         break;
      } else {
         printf
             ("Example %d: \"%s\" / %f (%f): ",
              i + 1, testExamples[i], result, testSolutions[i]);
         if (result == testSolutions[i]) {
            printf("PASS\n");
            pass++;
         } else {
            printf("FAIL\n");
            fail++;
         }
      }
   }

   printf("PASS: %d\nFAIL: %d\n", pass, fail);

   return 0;
}
#endif
