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
#include <time.h>
#include <json-c/json.h>
#include <fnmatch.h>

#include "logging.h"
#include "text_to_command_nuevo.h"

char* extract_remaining_after_substring(const char* input, const char* substring) {
    char *pos = strstr(input, substring);

    if (pos == NULL) {
        return NULL;  // Substring not found
    }

    return pos + strlen(substring);
}

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
      LOG_ERROR("Error occurred during wildcard matching.");
      return -1;
   }
}

// Function to replace placeholders with given values
char* replaceWithValues(const char* templateStr, const char* deviceName, const char* value) {
   char* modifiedStr = NULL;
   int slDeviceName = 0;
   int slValue = 0;

   time_t r_time;
   struct tm *l_time = NULL;
   char datetime[16];

   if (templateStr == NULL) {
      LOG_ERROR("replaceWithValues() templateStr cannot be NULL.");

      return NULL;
   }

   if (deviceName != NULL) {
      slDeviceName = strlen(deviceName);
   }

   if (value != NULL) {
      slValue = strlen(value);
   }

   modifiedStr = (char*)malloc(strlen(templateStr) + slDeviceName + slValue +
                               15 /* datetime */ + 1 /* '\0' */); // This isn't perfect but it's safe.
   if (modifiedStr == NULL) {
      LOG_ERROR("Memory allocation failed.");
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
            } else if (strncmp(placeholder, "datetime", placeholderLen) == 0) {
               time(&r_time);
               l_time = localtime(&r_time);
               strftime(datetime, sizeof(datetime), "%Y%m%d_%H%M%S", l_time);

               strcpy(modPtr, datetime);
               modPtr += 15;
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
 * Convert the actions data struct into something useful for the commands processor.
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
         thisActionCommand = actions[i].subActions[j].actionCommand;

         for (k = 0; k < actions[i].subActions[j].numActionWords; k++) {
            thisActionWord = actions[i].subActions[j].actionWords[k];

            for (m = 0; m < actions[i].numDevices; m++) {
               thisUnit = actions[i].devices[m].unit;

               thisTopic = actions[i].devices[m].topic;

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
                  LOG_ERROR("COMMAND OVERFLOW!!!");

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
                     LOG_ERROR("COMMAND OVERFLOW!!!");

                     return;
                  }
               }
            }
         }
      }
   }

   LOG_INFO("Total commands generated: %d", *numCommands);
}

// Debug function to print the parsed data from the struct.
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

/*
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

// Debug function to print the resulting action commands array.
void printCommands(commandSearchElement *commands, int numCommands) {
   int i = 0;

   for (i = 0; i < numCommands; i++) {
      printf("%d:\t%s\n\t%s\n\t%s\n\t%s\n", i,
             commands[i].actionWordsWildcard, commands[i].actionWordsRegex,
             commands[i].actionCommand, commands[i].topic);
   }
}

// Parse the passed in json string into the actionType struct.
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

   LOG_INFO("Parsing json...");
   parsedJson = json_tokener_parse(json);
   if (parsedJson == NULL) {
      LOG_ERROR("Error parsing json.");
      return 1;
   }

   /* TYPES */
   if (!json_object_object_get_ex(parsedJson, "types", &typeObject)) {
      LOG_ERROR("\"types\" object not found in json.");

      return 1;
   }

   it = json_object_iter_begin(typeObject);
   itEnd = json_object_iter_end(typeObject);

   /* types loop */
   while (!json_object_iter_equal(&it, &itEnd)) {
      typeName = json_object_iter_peek_name(&it);

      strncpy(actions[*numActions].name, typeName, MAX_WORD_LENGTH);  // actionType to struct.

      /* grab that type's object */
      json_object_object_get_ex(typeObject, typeName, &nextTypeObject);

      /* the next object must be actions */
      if (!json_object_object_get_ex(nextTypeObject, "actions", &actionsObject)) {
         LOG_ERROR("\"actions\" object not found in json.");

         break;
      }

      /* now we go through the defined actions */
      itSub = json_object_iter_begin(actionsObject);
      itSubEnd = json_object_iter_end(actionsObject);

      /* actions loop */
      while (!json_object_iter_equal(&itSub, &itSubEnd)) {
         actionName = json_object_iter_peek_name(&itSub);

         strncpy(actions[*numActions].subActions[actions[*numActions].numSubActions].name,
                 actionName, MAX_WORD_LENGTH); // actionName to struct.

         if (!json_object_object_get_ex(actionsObject, actionName, &nextActionObject)) {
            LOG_ERROR("\"%s\" object not found in json.", actionName);

            break;
         }

         if (!json_object_object_get_ex(nextActionObject, "action_words", &actionWordArrayObject)) {
            LOG_ERROR("\"action_words\" object not found in json.");

            break;
         }

         arrayLength = json_object_array_length(actionWordArrayObject);
         for (i = 0; i < arrayLength; i++) {
            actionWordObject = json_object_array_get_idx(actionWordArrayObject, i);
            if (actionWordObject == NULL) {
               LOG_ERROR("Next action_word string object not found in json.");

               break;
            }

            actionWord = json_object_get_string(actionWordObject);

            strncpy(actions[*numActions].subActions[actions[*numActions].numSubActions].actionWords[actions[*numActions].subActions[actions[*numActions].numSubActions].numActionWords],
                    actionWord, MAX_WORD_LENGTH); // actionWord to struct.

            actions[*numActions].subActions[actions[*numActions].numSubActions].numActionWords++;   // Increment numActionWords to struct.
         }

         if (!json_object_object_get_ex(nextActionObject, "action_command", &actionCommandObject)) {
            LOG_ERROR("\"action_command\" object not found in json.");

            break;
         }

         actionCommand = json_object_get_string(actionCommandObject);

         strncpy(actions[*numActions].subActions[actions[*numActions].numSubActions].actionCommand, actionCommand, MAX_WORD_LENGTH); // actionCommand to struct.

         json_object_iter_next(&itSub);

         actions[*numActions].numSubActions++;  // Increment numSubActions to struct.
      }

      (*numActions)++;  // Increment to the next type.
      if (*numActions > MAX_ACTIONS) {
         LOG_ERROR("Number of actions processed > max actions supported!");
         break;
      }

      json_object_iter_next(&it);
   }

   /* DEVICES */
   if (!json_object_object_get_ex(parsedJson, "devices", &devicesObject)) {
      LOG_ERROR("\"devices\" object not found in json.\n");

      return 1;
   }

   it = json_object_iter_begin(devicesObject);
   itEnd = json_object_iter_end(devicesObject);

   /* device loop */
   while (!json_object_iter_equal(&it, &itEnd)) {
      deviceName = json_object_iter_peek_name(&it);

      /* grab that device's object */
      json_object_object_get_ex(devicesObject, deviceName, &nextDeviceObject);

      /* the next object must be type */
      if (!json_object_object_get_ex(nextDeviceObject, "type", &deviceTypeObject)) {
         LOG_ERROR("\"type\" object not found in json.");

         break;
      }

      deviceNum = -1;
      deviceType = json_object_get_string(deviceTypeObject);

      for (i = 0; i <= *numActions; i++) {
         if (strcmp(actions[i].name, deviceType) == 0) {
            deviceNum = i;

            break;
         }
      }

      if (deviceNum == -1) {
         LOG_ERROR("Could not find device type: %s", deviceType);
         break;
      }

      strncpy(actions[deviceNum].devices[actions[deviceNum].numDevices].name, deviceName, MAX_WORD_LENGTH); // deviceName to struct.

      /* the next object must be aliases, strictly not required */
      if (!json_object_object_get_ex(nextDeviceObject, "aliases", &deviceAliasesArrayObject)) {
         LOG_WARNING("\"aliases\" object not found in json.");
      } else {
         arrayLength = json_object_array_length(deviceAliasesArrayObject);
         for (i = 0; i < arrayLength; i++) {
            deviceAliasObject = json_object_array_get_idx(deviceAliasesArrayObject, i);
            if (deviceAliasObject == NULL) {
               LOG_WARNING("Next aliases string object not found in json.");

               break;
            }

            deviceAlias = json_object_get_string(deviceAliasObject);

            if (deviceAlias != NULL) {
               strncpy(actions[deviceNum].devices[actions[deviceNum].numDevices].aliases[actions[deviceNum].devices[actions[deviceNum].numDevices].numAliases], deviceAlias, MAX_WORD_LENGTH); // deviceAlias to struct.
               actions[deviceNum].devices[actions[deviceNum].numDevices].numAliases++; // Increment numAliases
            }
         }
      }

      /* the next object must be unit, strictly not required */
      if (!json_object_object_get_ex(nextDeviceObject, "unit", &deviceUnitObject)) {
         actions[deviceNum].devices[actions[deviceNum].numDevices].unit[0] = '\0';
      } else {
         deviceUnit = json_object_get_string(deviceUnitObject);

         strncpy(actions[deviceNum].devices[actions[deviceNum].numDevices].unit, deviceUnit, MAX_WORD_LENGTH);
      }

      /* the next object must be topic, required */
      if (!json_object_object_get_ex(nextDeviceObject, "topic", &deviceTopicObject)) {
         LOG_ERROR("\"topic\" object not found in json.");
         break;
      } else {
         deviceTopic = json_object_get_string(deviceTopicObject);

         strncpy(actions[deviceNum].devices[actions[deviceNum].numDevices].topic, deviceTopic, MAX_WORD_LENGTH);
      }

      actions[deviceNum].numDevices++; // Increment number of devices.

      json_object_iter_next(&it);
   }

   /* AUDIO DEVICES */
   if (!json_object_object_get_ex(parsedJson, "audio devices", &audioDevicesObject)) {
      LOG_ERROR("\"audio devices\" object not found in json.");

      return 1;
   }

   it = json_object_iter_begin(audioDevicesObject);
   itEnd = json_object_iter_end(audioDevicesObject);

   /* audio device loop */
   while (!json_object_iter_equal(&it, &itEnd)) {
      adType adTypeNum = AUDIO_DEVICE_UNKNOWN;
      audioDevices *thisDeviceType = NULL;
      int *thisDeviceCount = NULL;

      audioDeviceName = json_object_iter_peek_name(&it);

      /* grab that device's object */
      json_object_object_get_ex(audioDevicesObject, audioDeviceName, &nextAudioDeviceObject);

      /* the next object must be type */
      if (!json_object_object_get_ex(nextAudioDeviceObject, "type", &audioDeviceTypeObject)) {
         LOG_ERROR("\"type\" object not found in json.");

         break;
      }

      audioDeviceType = json_object_get_string(audioDeviceTypeObject);

      if (strcmp(AUDIO_DEVICE_CAPTURE_STRING, audioDeviceType) == 0) {
         adTypeNum = AUDIO_DEVICE_CAPTURE;
         thisDeviceType = captureDevices;
         thisDeviceCount = numAudioCaptureDevices;
      } else if (strcmp(AUDIO_DEVICE_PLAYBACK_STRING, audioDeviceType) == 0) {
         adTypeNum = AUDIO_DEVICE_PLAYBACK;
         thisDeviceType = playbackDevices;
         thisDeviceCount = numAudioPlaybackDevices;
      } else {
         LOG_ERROR("Could not find device type: %s", audioDeviceType);
         break;
      }

      thisDeviceType[*thisDeviceCount].type = adTypeNum;
      strncpy(thisDeviceType[*thisDeviceCount].name, audioDeviceName, MAX_WORD_LENGTH);  // audioDeviceName to struct.

      /* the next object must be aliases, not strictly equired */
      if (!json_object_object_get_ex(nextAudioDeviceObject, "aliases", &audioDeviceAliasesArrayObject)) {
         LOG_ERROR("\"aliases\" object not found in json.");
      } else {
         arrayLength = json_object_array_length(audioDeviceAliasesArrayObject);
         for (i = 0; i < arrayLength; i++) {
            audioDeviceAliasObject = json_object_array_get_idx(audioDeviceAliasesArrayObject, i);
            if (audioDeviceAliasObject == NULL) {
               LOG_ERROR("Next aliases string object not found in json.");

               break;
            }

            audioDeviceAlias = json_object_get_string(audioDeviceAliasObject);

            strncpy(thisDeviceType[*thisDeviceCount].aliases[thisDeviceType[*thisDeviceCount].numAliases], audioDeviceAlias, MAX_WORD_LENGTH); // deviceAlias to struct.
            thisDeviceType[*thisDeviceCount].numAliases++; // Increment numAliases
         }
      }

      /* the next object must be device, required */
      if (!json_object_object_get_ex(nextAudioDeviceObject, "device", &audioDeviceDeviceObject)) {
         LOG_ERROR("\"device\" object not found in json.");

         break;
      }

      audioDeviceDevice = json_object_get_string(audioDeviceDeviceObject);

      strncpy(thisDeviceType[*thisDeviceCount].device, audioDeviceDevice, MAX_WORD_LENGTH);

      (*thisDeviceCount)++; // Increment number of devices.

      json_object_iter_next(&it);
   }

   json_object_put(parsedJson);

   return 0;
}

// Initialize all of the action structs' counters.
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
