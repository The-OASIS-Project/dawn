/*
 * Test program for sentence buffering
 */

#include <stdio.h>
#include <string.h>

#include "utils/sentence_buffer.h"

static int sentence_count = 0;

void test_sentence_callback(const char *sentence, void *userdata) {
   sentence_count++;
   printf("[Sentence %d]: %s\n", sentence_count, sentence);
}

int main(void) {
   printf("=== Sentence Buffer Test ===\n\n");

   sentence_buffer_t *buf = sentence_buffer_create(test_sentence_callback, NULL);
   if (!buf) {
      fprintf(stderr, "Failed to create sentence buffer\n");
      return 1;
   }

   // Test 1: Simple sentence with period
   printf("Test 1: Simple sentence\n");
   sentence_buffer_feed(buf, "Hello world. ");
   printf("\n");

   // Test 2: Sentence split across chunks
   printf("Test 2: Split sentence\n");
   sentence_buffer_feed(buf, "This is ");
   sentence_buffer_feed(buf, "a test. ");
   printf("\n");

   // Test 3: Multiple terminators
   printf("Test 3: Multiple terminators\n");
   sentence_buffer_feed(buf, "Question? ");
   sentence_buffer_feed(buf, "Exclamation! ");
   sentence_buffer_feed(buf, "Note: ");
   printf("\n");

   // Test 4: Long chunk with multiple sentences
   printf("Test 4: Multiple sentences in one chunk\n");
   sentence_buffer_feed(buf, "First sentence. Second sentence! Third one? ");
   printf("\n");

   // Test 5: Incomplete sentence that needs flush
   printf("Test 5: Incomplete sentence (should flush)\n");
   sentence_buffer_feed(buf, "Incomplete without terminator");
   sentence_buffer_flush(buf);
   printf("\n");

   // Test 6: Simulate OpenAI-style chunking
   printf("Test 6: OpenAI-style token chunks\n");
   sentence_buffer_feed(buf, "Hello");
   sentence_buffer_feed(buf, "!");
   sentence_buffer_feed(buf, " ");  // This space triggers sentence
   sentence_buffer_feed(buf, "2");
   sentence_buffer_feed(buf, " ");
   sentence_buffer_feed(buf, "+");
   sentence_buffer_feed(buf, " ");
   sentence_buffer_feed(buf, "2");
   sentence_buffer_feed(buf, " ");
   sentence_buffer_feed(buf, "equals");
   sentence_buffer_feed(buf, " ");
   sentence_buffer_feed(buf, "4");
   sentence_buffer_feed(buf, ".");
   sentence_buffer_flush(buf);  // Flush the final sentence
   printf("\n");

   // Test 7: Newlines and whitespace
   printf("Test 7: Newlines\n");
   sentence_buffer_feed(buf, "Hello!\n\n2 + 2 equals 4.");
   sentence_buffer_flush(buf);
   printf("\n");

   sentence_buffer_free(buf);

   printf("=== Test Complete ===\n");
   printf("Total sentences extracted: %d\n", sentence_count);
   printf("Expected: ~13 sentences\n");

   return 0;
}
