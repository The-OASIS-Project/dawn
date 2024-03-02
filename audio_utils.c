#include <math.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Calculates the Root Mean Square (RMS) value of an audio signal.
 * RMS is a statistical measure of the magnitude of a varying quantity and
 * is used here to estimate the power of an audio signal.
 *
 * @param audioBuffer Pointer to the buffer containing 16-bit signed audio samples.
 * @param numSamples The number of samples in the audio buffer.
 * @return The calculated RMS value as a double.
 */
double calculateRMS(const int16_t *audioBuffer, size_t numSamples) {
    double sumOfSquares = 0.0; // Initialize sum of squares to zero.
    for (size_t i = 0; i < numSamples; ++i) {
       // Normalize the sample to the range [-1, 1] using 32768.0 as the divisor.
       // This is because 16-bit signed integers range from -32768 to 32767.
       double normalizedSample = (double)audioBuffer[i] / 32768.0;
       // Accumulate the square of the normalized sample.
       sumOfSquares += normalizedSample * normalizedSample;
    }
    // Calculate the mean of the squares.
    double meanOfSquares = sumOfSquares / numSamples;
    // Return the square root of the mean of squares, which is the RMS value.
    return sqrt(meanOfSquares);
}


