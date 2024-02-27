#ifndef OPENAI_H
#define OPENAI_H

char *getGptResponse(struct json_object *conversation_history, const char *input_text);

#endif // OPENAI_H
