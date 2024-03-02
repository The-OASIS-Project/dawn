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
 * All contributions to this project are agreed to be licensed under the
 * GPLv3 or any later version. Contributions are understood to be
 * any modifications, enhancements, or additions to the project
 * and become the property of the original author Kris Kersey.
 */

/**
 * @file text_to_command_nuevo.c
 * @brief This will process the text to command json file.
 *
 * I've done memeory management differently in the processing of this json file
 * from how I did the main json config file. This was mainly due to how this
 * json is a bit less dynamic or large than the main one.
 *
 * This json memory is static for now with counts for the arrays. I'm sure this
 * will change as I develop it but I wanted to put a not in case I wondered why
 * I did this different.
 *
 * @note The current design of the json file was for human readability over
 *       processing. From a json perspective, I should have put the devices
 *       under each of their actions but that wouldn't have been as easy to add
 *       devices. It's up to the user to give them a valid type. which isn't
 *       too much to ask.
 *
 * @author Kris Kersey
 * @date 2023-08-06
 *
 * @license GPLv3
 */

#include <stdio.h>
#include <string.h>
#include <json-c/json.h>
#include <fnmatch.h>

#include "text_to_command_nuevo.h"

/**
 * @brief Extracts and returns the remaining part of a string after a given substring.
 *
 * This function takes a string and a substring, and returns a pointer to the
 * part of the string that comes after the first occurrence of the substring.
 * If the substring is not found, the function returns NULL.
 *
 * @param input The original string in which to search for the substring.
 * @param substring The substring to search for in the original string.
 *
 * @return A pointer to the remaining part of the string after the first occurrence of the substring.
 *         Returns NULL if the substring is not found.
 *
 * @note The function returns a pointer into the original string, so the caller should not modify
 *       the content unless they are sure of what they are doing.
 */
char* extract_remaining_after_substring(const char* input, const char* substring) {
    char *pos = strstr(input, substring);

    if (pos == NULL) {
        return NULL;  // Substring not found
    }

    return pos + strlen(substring);
}

/*
 * @brief Function to search the second string for the template.
 *
 * This command supports wildcards.
 *
 * @param templateStr String to a wildcard enabled string.
 * @param secondStr String to search through.
 *
 * @return 1 if found, 0 if not found, -1 on error.
 */
int searchString(const char* templateStr, const char* secondStr) {
   if ((templateStr == NULL) || (secondStr == NULL)) {
      return -1;
   }

   char *newTemplate = malloc((strlen(templateStr) + 3) * sizeof(char));
   if (newTemplate == NULL) {
      return -1;
   }

   strcpy(newTemplate, templateStr);
   newTemplate[strlen(templateStr)] = '*';
   newTemplate[strlen(templateStr) + 1] = '\0';

   int found = fnmatch(newTemplate, secondStr, 0);

   free(newTemplate);

   if (found == 0) {
      return 1;
   } else if (found == FNM_NOMATCH) {
      return 0;
   } else {
      printf("Error occurred during wildcard matching.\n");
      return -1;
   }
}

// Function to replace placeholders with given values
char* replaceWithValues(const char* templateStr, const char* deviceName, const char* value) {
   char* modifiedStr = NULL;
   int slDeviceName = 0;
   int slValue = 0;

   if (templateStr == NULL) {
      fprintf(stderr, "replaceWithValues() templateStr cannot be NULL.\n");

      return NULL;
   }

   if (deviceName != NULL) {
      slDeviceName = strlen(deviceName);
   }

   if (value != NULL) {
      slValue = strlen(value);
   }

   modifiedStr = (char*)malloc(strlen(templateStr) + slDeviceName + slValue + 1);   // This isn't perfect but it's safe.
   if (modifiedStr == NULL) {
      fprintf(stderr, "Memory allocation failed.\n");
      return NULL;
   }

   char* modPtr = modifiedStr;

   while (*templateStr) {
      if (*templateStr == '%') {
         const char* placeholder = ++templateStr; // Skip the '%'
         while (*templateStr && *templateStr != '%') {
            templateStr++;
         }

         if (*templateStr == '%') {
            size_t placeholderLen = templateStr - placeholder;
            *templateStr++; // Skip the trailing '%'

            if ((deviceName != NULL) && (strncmp(placeholder, "device_name", placeholderLen) == 0)) {
               strcpy(modPtr, deviceName);
               modPtr += slDeviceName;
            } else if ((value != NULL) && (strncmp(placeholder, "value", placeholderLen) == 0)) {
               strcpy(modPtr, value);
               modPtr += slValue;
            }
         }
      } else {
         *modPtr++ = *templateStr++;
      }
   }

   *modPtr = '\0';
   return modifiedStr;
}

/**
 * @brief Convert the actions data struct into something useful for the commands processor.
 *
 * We need nice strings that can be filtered for in the audio to text section.
 * This should do that.
 */
void convertActionsToCommands(actionType *actions, int *numActions,
                              commandSearchElement *commands, int *numCommands) {
   int i = 0, j = 0, k = 0, m = 0, n = 0;

   const char *thisActionCommand = NULL;
   const char *thisActionWord = NULL;
   const char *thisDevice = NULL;
   const char *thisAlias = NULL;
   const char *thisUnit = NULL;
   const char *thisTopic = NULL;

   char *newActionWordsWildcard = NULL;
   char *newActionWordsRegex = NULL;
   char *newActionCommand = NULL;

   for (i = 0; i < *numActions; i++) {
      for (j = 0; j < actions[i].numSubActions; j++) {
         //printf("\tAction Command: %s\n", actions[i].subActions[j].actionCommand);
         thisActionCommand = actions[i].subActions[j].actionCommand;

         for (k = 0; k < actions[i].subActions[j].numActionWords; k++) {
            //printf("Action Words: %s\n", actions[i].subActions[j].actionWords[k]);
            thisActionWord = actions[i].subActions[j].actionWords[k];

            for (m = 0; m < actions[i].numDevices; m++) {
               //printf("\tDevice Unit: %s\n", actions[i].devices[m].unit);
               thisUnit = actions[i].devices[m].unit;

               //printf("\tDevice Topic: %s\n", actions[i].devices[m].topic);
               thisTopic = actions[i].devices[m].topic;

               //printf("\tDevice Name: %s\n", actions[i].devices[m].name);
               thisDevice = actions[i].devices[m].name;

               newActionWordsWildcard = replaceWithValues(thisActionWord, thisDevice, "*");
               //newActionWordsRegex = replaceWithValues(thisActionWord, thisDevice, "%1024[^\\t\\n]");
               newActionWordsRegex = replaceWithValues(thisActionWord, thisDevice, "%s");
               newActionCommand = replaceWithValues(thisActionCommand, thisDevice, "%s");
               strncpy(commands[*numCommands].actionWordsWildcard, newActionWordsWildcard, MAX_COMMAND_LENGTH);
               strncpy(commands[*numCommands].actionWordsRegex, newActionWordsRegex, MAX_COMMAND_LENGTH);
               strncpy(commands[*numCommands].actionCommand, newActionCommand, MAX_COMMAND_LENGTH);
               strncpy(commands[*numCommands].topic, thisTopic, MAX_WORD_LENGTH);
               (*numCommands)++;
               free(newActionWordsWildcard);
               free(newActionWordsRegex);
               free(newActionCommand);
               if (*numCommands == MAX_COMMANDS) {
                  printf("COMMAND OVERFLOW!!!\n");

                  return;
               }

               for (n = 0; n < actions[i].devices[m].numAliases; n++) {
                  thisAlias = actions[i].devices[m].aliases[n];

                  newActionWordsWildcard = replaceWithValues(thisActionWord, thisAlias, "*");
                  //newActionWordsRegex = replaceWithValues(thisActionWord, thisAlias, "%1024[^\\t\\n]");
                  newActionWordsRegex = replaceWithValues(thisActionWord, thisAlias, "%s");
                  newActionCommand = replaceWithValues(thisActionCommand, thisDevice, "%s");
                  strncpy(commands[*numCommands].actionWordsWildcard, newActionWordsWildcard, MAX_COMMAND_LENGTH);
                  strncpy(commands[*numCommands].actionWordsRegex, newActionWordsRegex, MAX_COMMAND_LENGTH);
                  strncpy(commands[*numCommands].actionCommand, newActionCommand, MAX_COMMAND_LENGTH);
                  strncpy(commands[*numCommands].topic, thisTopic, MAX_WORD_LENGTH);
                  (*numCommands)++;
                  free(newActionWordsWildcard);
                  free(newActionWordsRegex);
                  free(newActionCommand);

                  if (*numCommands == MAX_COMMANDS) {
                     printf("COMMAND OVERFLOW!!!\n");

                     return;
                  }
               }
            }
         }
      }
   }

   printf("Total commands generated: %d\n", *numCommands);
}

/**
 * @brief Debug function to print the parsed data from the struct.
 */
void printParsedData(actionType *actions, int numActions) {
   int i = 0, j = 0, k = 0;

   printf("Data Struct: %d\n", numActions);
   for (i = 0; i < numActions; i++) {
       printf("Action Type: %s\n", actions[i].name);
       printf("Sub-Actions:\n");
       for (j = 0; j < actions[i].numSubActions; j++) {
          printf("\tName: %s\n", actions[i].subActions[j].name);
          printf("\tAction Words:\n");
          for (k = 0; k < actions[i].subActions[j].numActionWords; k++) {
             printf("\t\t%s \n", actions[i].subActions[j].actionWords[k]);
          }
          printf("\tAction Command: %s\n", actions[i].subActions[j].actionCommand);
       }
       printf("Devices:\n");
       for (j = 0; j < actions[i].numDevices; j++) {
          printf("\tName: %s\n", actions[i].devices[j].name);
          printf("\tAliases:\n");
          for (k = 0; k < actions[i].devices[j].numAliases; k++) {
             printf("\t\t%s\n", actions[i].devices[j].aliases[k]);
          }
          printf("\tUnit: %s\n", actions[i].devices[j].unit);
          printf("\tTopic: %s\n", actions[i].devices[j].topic);
       }
       printf("\n");
   }
}

/**
 * @brief Debug function to print the parsed audio device data.
 */
void printParsedAudioDevices(audioDevices *devices, int numDevices) {
   int i = 0, j = 0;

   printf("Audio Devices:\n");
   switch (devices[0].type)
   {
      case AUDIO_DEVICE_CAPTURE:
         printf("\tCapture Devices\n");
         break;
      case AUDIO_DEVICE_PLAYBACK:
         printf("\tPlayback Devices\n");
         break;
      case AUDIO_DEVICE_UNKNOWN:
         printf("\tUnknown Devices: This is bad.\n");
         break;
      default:
         printf("\tI have no idea why I'm here.\n");
   }

   for (i = 0; i < numDevices; i++) {
      printf("\tName: %s\n", devices[i].name);
      printf("\tAliases:\n");
      for (j = 0; j < devices[i].numAliases; j++) {
         printf("\t\t%s\n", devices[i].aliases[j]);
      }
      printf("\tDevice: %s\n", devices[i].device);
   }
}

/**
 * @brief Debug function to print the resulting action commands array.
 */
void printCommands(commandSearchElement *commands, int numCommands) {
   int i = 0;

   for (i = 0; i < numCommands; i++) {
      printf("%d:\t%s\n\t%s\n\t%s\n\t%s\n", i,
             commands[i].actionWordsWildcard, commands[i].actionWordsRegex,
             commands[i].actionCommand, commands[i].topic);
   }
}

/**
 * @brief Parse the passed in json string into the actionType struct.
 *
 * This is the usual long, drawn-out json processing that makes life easy in
 * the long run.
 *
 * @note I don't really like passing all of this in here. It was easiest
 *       given the timeline.
 *
 * @param json Json string to parse.
 *
 * @return 0 on success, 1 on failure.
 */
int parseCommandConfig(char *json, actionType *actions, int *numActions,
                       audioDevices *captureDevices, int *numAudioCaptureDevices,
                       audioDevices *playbackDevices, int *numAudioPlaybackDevices)
{
   struct json_object* parsedJson = NULL;

   struct json_object_iterator it;
   struct json_object_iterator itEnd;
   struct json_object_iterator itSub;
   struct json_object_iterator itSubEnd;

   struct json_object *typeObject = NULL;
   struct json_object *nextTypeObject = NULL;
   struct json_object *actionsObject = NULL;
   struct json_object *nextActionObject = NULL;
   struct json_object *actionWordArrayObject = NULL;
   struct json_object *actionWordObject = NULL;
   struct json_object *actionCommandObject = NULL;

   struct json_object *devicesObject = NULL;
   struct json_object *nextDeviceObject = NULL;
   struct json_object *deviceTypeObject = NULL;
   struct json_object *deviceAliasesArrayObject = NULL;
   struct json_object *deviceAliasObject = NULL;
   struct json_object *deviceUnitObject = NULL;
   struct json_object *deviceTopicObject = NULL;

   struct json_object *audioDevicesObject = NULL;
   struct json_object *nextAudioDeviceObject = NULL;
   struct json_object *audioDeviceTypeObject = NULL;
   struct json_object *audioDeviceAliasesArrayObject = NULL;
   struct json_object *audioDeviceAliasObject = NULL;
   struct json_object *audioDeviceDeviceObject = NULL;

   const char *typeName = NULL;
   const char *actionName = NULL;
   const char *actionWord = NULL;
   const char *actionCommand = NULL;

   const char *deviceName = NULL;
   const char *deviceType = NULL;
   const char *deviceAlias = NULL;
   const char *deviceUnit = NULL;
   const char *deviceTopic = NULL;

   const char *audioDeviceName = NULL;
   const char *audioDeviceType = NULL;
   const char *audioDeviceAlias = NULL;
   const char *audioDeviceDevice = NULL;

   int arrayLength = 0;
   int i = 0;
   int deviceNum = 0;

   printf("Parsing json...");
   parsedJson = json_tokener_parse(json);
   printf("Done.\n");
   if (parsedJson == NULL) {
      fprintf(stderr, "Error parsing json.\n");
      return 1;
   }

   /* TYPES */
   if (!json_object_object_get_ex(parsedJson, "types", &typeObject)) {
      fprintf(stderr, "\"types\" object not found in json.\n");

      return 1;
   }

   it = json_object_iter_begin(typeObject);
   itEnd = json_object_iter_end(typeObject);

   //printf("types:\n");
   /* types loop */
   while (!json_object_iter_equal(&it, &itEnd)) {
      typeName = json_object_iter_peek_name(&it);
      //printf("\t%s\n", typeName);

      strncpy(actions[*numActions].name, typeName, MAX_WORD_LENGTH);  // actionType to struct.

      /* grab that type's object */
      json_object_object_get_ex(typeObject, typeName, &nextTypeObject);

      /* the next object must be actions */
      if (!json_object_object_get_ex(nextTypeObject, "actions", &actionsObject)) {
         fprintf(stderr, "\"actions\" object not found in json.\n");

         break;
      }

      /* now we go through the defined actions */
      itSub = json_object_iter_begin(actionsObject);
      itSubEnd = json_object_iter_end(actionsObject);

      //printf("\t\tactions:\n");
      /* actions loop */
      while (!json_object_iter_equal(&itSub, &itSubEnd)) {
         actionName = json_object_iter_peek_name(&itSub);
         //printf("\t\t\t%s\n", actionName);

         strncpy(actions[*numActions].subActions[actions[*numActions].numSubActions].name, actionName, MAX_WORD_LENGTH); // actionName to struct.

         if (!json_object_object_get_ex(actionsObject, actionName, &nextActionObject)) {
            fprintf(stderr, "\"%s\" object not found in json.\n", actionName);

            break;
         }

         if (!json_object_object_get_ex(nextActionObject, "action_words", &actionWordArrayObject)) {
            fprintf(stderr, "\"action_words\" object not found in json.\n");

            break;
         }

         //printf("\t\t\t\taction_words: ");
         arrayLength = json_object_array_length(actionWordArrayObject);
         for (i = 0; i < arrayLength; i++) {
            actionWordObject = json_object_array_get_idx(actionWordArrayObject, i);
            if (actionWordObject == NULL) {
               fprintf(stderr, "Next action_word string object not found in json.\n");

               break;
            }

            actionWord = json_object_get_string(actionWordObject);
            //printf("\"%s\" ", actionWord);

            strncpy(actions[*numActions].subActions[actions[*numActions].numSubActions].actionWords[actions[*numActions].subActions[actions[*numActions].numSubActions].numActionWords],
                    actionWord, MAX_WORD_LENGTH); // actionWord to struct.

            actions[*numActions].subActions[actions[*numActions].numSubActions].numActionWords++;   // Increment numActionWords to struct.
         }
         //printf("\n");

         if (!json_object_object_get_ex(nextActionObject, "action_command", &actionCommandObject)) {
            fprintf(stderr, "\"action_command\" object not found in json.\n");

            break;
         }

         actionCommand = json_object_get_string(actionCommandObject);
         //printf("\t\t\t\taction_command: \"%s\"\n", actionCommand);

         strncpy(actions[*numActions].subActions[actions[*numActions].numSubActions].actionCommand, actionCommand, MAX_WORD_LENGTH); // actionCommand to struct.

         json_object_iter_next(&itSub);

         actions[*numActions].numSubActions++;  // Increment numSubActions to struct.
      }

      (*numActions)++;  // Increment to the next type.
      if (*numActions > MAX_ACTIONS) {
         fprintf(stderr, "Number of actions processed > max actions supported!\n");
         break;
      }

      json_object_iter_next(&it);
   }

   /* DEVICES */
   if (!json_object_object_get_ex(parsedJson, "devices", &devicesObject)) {
      fprintf(stderr, "\"devices\" object not found in json.\n");

      return 1;
   }

   it = json_object_iter_begin(devicesObject);
   itEnd = json_object_iter_end(devicesObject);

   //printf("devices:\n");
   /* device loop */
   while (!json_object_iter_equal(&it, &itEnd)) {
      deviceName = json_object_iter_peek_name(&it);
      //printf("\t%s\n", deviceName);

      /* grab that device's object */
      json_object_object_get_ex(devicesObject, deviceName, &nextDeviceObject);

      /* the next object must be type */
      if (!json_object_object_get_ex(nextDeviceObject, "type", &deviceTypeObject)) {
         fprintf(stderr, "\"type\" object not found in json.\n");

         break;
      }

      deviceNum = -1;
      deviceType = json_object_get_string(deviceTypeObject);
      //printf("\t\ttype: \"%s\"\n", deviceType);

      for (i = 0; i <= *numActions; i++) {
         //printf("Comparing %s to %s.\n", actions[i].name, deviceType);
         if (strcmp(actions[i].name, deviceType) == 0) {
            deviceNum = i;

            break;
         }
      }

      if (deviceNum == -1) {
         fprintf(stderr, "Could not find device type: %s\n", deviceType);
         break;
      }

      strncpy(actions[deviceNum].devices[actions[deviceNum].numDevices].name, deviceName, MAX_WORD_LENGTH); // deviceName to struct.

      /* the next object must be aliases, strictly not required */
      if (!json_object_object_get_ex(nextDeviceObject, "aliases", &deviceAliasesArrayObject)) {
         fprintf(stderr, "\"aliases\" object not found in json.\n");
      } else {
         //printf("\t\t\taliases: ");
         arrayLength = json_object_array_length(deviceAliasesArrayObject);
         for (i = 0; i < arrayLength; i++) {
            deviceAliasObject = json_object_array_get_idx(deviceAliasesArrayObject, i);
            if (deviceAliasObject == NULL) {
               fprintf(stderr, "Next aliases string object not found in json.\n");

               break;
            }

            deviceAlias = json_object_get_string(deviceAliasObject);
            //printf("\"%s\" ", deviceAlias);

            strncpy(actions[deviceNum].devices[actions[deviceNum].numDevices].aliases[actions[deviceNum].devices[actions[deviceNum].numDevices].numAliases], deviceAlias, MAX_WORD_LENGTH); // deviceAlias to struct.
            actions[deviceNum].devices[actions[deviceNum].numDevices].numAliases++; // Increment numAliases
         }
         //printf("\n");
      }

      /* the next object must be unit, strictly not required */
      if (!json_object_object_get_ex(nextDeviceObject, "unit", &deviceUnitObject)) {
         //fprintf(stderr, "\"unit\" object not found in json.\n");
         actions[deviceNum].devices[actions[deviceNum].numDevices].unit[0] = '\0';
      } else {
         deviceUnit = json_object_get_string(deviceUnitObject);
         //printf("\t\tunit: \"%s\"\n", deviceUnit);

         strncpy(actions[deviceNum].devices[actions[deviceNum].numDevices].unit, deviceUnit, MAX_WORD_LENGTH);
      }

      /* the next object must be topic, required */
      if (!json_object_object_get_ex(nextDeviceObject, "topic", &deviceTopicObject)) {
         fprintf(stderr, "\"topic\" object not found in json.\n");
         break;
      } else {
         deviceTopic = json_object_get_string(deviceTopicObject);
         //printf("\t\ttopic: \"%s\"\n", deviceUnit);

         strncpy(actions[deviceNum].devices[actions[deviceNum].numDevices].topic, deviceTopic, MAX_WORD_LENGTH);
      }

      actions[deviceNum].numDevices++; // Increment number of devices.

      json_object_iter_next(&it);
   }

   /* AUDIO DEVICES */
   if (!json_object_object_get_ex(parsedJson, "audio devices", &audioDevicesObject)) {
      fprintf(stderr, "\"audio devices\" object not found in json.\n");

      return 1;
   }

   it = json_object_iter_begin(audioDevicesObject);
   itEnd = json_object_iter_end(audioDevicesObject);

   printf("audio devices:\n");
   /* audio device loop */
   while (!json_object_iter_equal(&it, &itEnd)) {
      adType adTypeNum = AUDIO_DEVICE_UNKNOWN;
      audioDevices *thisDeviceType = NULL;
      int *thisDeviceCount = NULL;

      audioDeviceName = json_object_iter_peek_name(&it);
      printf("\t%s\n", audioDeviceName);

      /* grab that device's object */
      json_object_object_get_ex(audioDevicesObject, audioDeviceName, &nextAudioDeviceObject);

      /* the next object must be type */
      if (!json_object_object_get_ex(nextAudioDeviceObject, "type", &audioDeviceTypeObject)) {
         fprintf(stderr, "\"type\" object not found in json.\n");

         break;
      }

      audioDeviceType = json_object_get_string(audioDeviceTypeObject);
      printf("\t\ttype: \"%s\"\n", audioDeviceType);

      if (strcmp(AUDIO_DEVICE_CAPTURE_STRING, audioDeviceType) == 0) {
         adTypeNum = AUDIO_DEVICE_CAPTURE;
         thisDeviceType = captureDevices;
         thisDeviceCount = numAudioCaptureDevices;
      } else if (strcmp(AUDIO_DEVICE_PLAYBACK_STRING, audioDeviceType) == 0) {
         adTypeNum = AUDIO_DEVICE_PLAYBACK;
         thisDeviceType = playbackDevices;
         thisDeviceCount = numAudioPlaybackDevices;
      } else {
         fprintf(stderr, "Could not find device type: %s\n", audioDeviceType);
         break;
      }

      thisDeviceType[*thisDeviceCount].type = adTypeNum;
      strncpy(thisDeviceType[*thisDeviceCount].name, audioDeviceName, MAX_WORD_LENGTH);  // audioDeviceName to struct.

      /* the next object must be aliases, not strictly equired */
      if (!json_object_object_get_ex(nextAudioDeviceObject, "aliases", &audioDeviceAliasesArrayObject)) {
         fprintf(stderr, "\"aliases\" object not found in json.\n");
      } else {
         printf("\t\t\taliases: ");
         arrayLength = json_object_array_length(audioDeviceAliasesArrayObject);
         for (i = 0; i < arrayLength; i++) {
            audioDeviceAliasObject = json_object_array_get_idx(audioDeviceAliasesArrayObject, i);
            if (audioDeviceAliasObject == NULL) {
               fprintf(stderr, "Next aliases string object not found in json.\n");

               break;
            }

            audioDeviceAlias = json_object_get_string(audioDeviceAliasObject);
            printf("\"%s\" ", audioDeviceAlias);

            strncpy(thisDeviceType[*thisDeviceCount].aliases[thisDeviceType[*thisDeviceCount].numAliases], audioDeviceAlias, MAX_WORD_LENGTH); // deviceAlias to struct.
            thisDeviceType[*thisDeviceCount].numAliases++; // Increment numAliases
         }
         printf("\n");
      }

      /* the next object must be device, required */
      if (!json_object_object_get_ex(nextAudioDeviceObject, "device", &audioDeviceDeviceObject)) {
         fprintf(stderr, "\"device\" object not found in json.\n");

         break;
      }

      audioDeviceDevice = json_object_get_string(audioDeviceDeviceObject);
      printf("\t\tdevice: \"%s\"\n", audioDeviceDevice);

      strncpy(thisDeviceType[*thisDeviceCount].device, audioDeviceDevice, MAX_WORD_LENGTH);

      (*thisDeviceCount)++; // Increment number of devices.

      json_object_iter_next(&it);
   }

   json_object_put(parsedJson);

   return 0;
}

/**
 * * @brief Initialize all of the action structs' counters.
 */
void initActions(actionType *actions)
{
   int i = 0, j = 0, k = 0;

   for (i = 0; i < MAX_ACTIONS; i++) {
      for (j = 0; j < MAX_SUBACTIONS; j++) {
         actions[i].subActions[j].numActionWords = 0;
      }

      actions[i].numSubActions = 0;

      for (j = 0; j < MAX_DEVICES_PER_ACTION; j++) {
         actions[i].devices[j].numAliases = 0;
      }

      actions[i].numDevices = 0;
   }
}
