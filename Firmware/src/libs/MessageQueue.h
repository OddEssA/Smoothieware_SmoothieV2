#pragma once

#include "FreeRTOS.h"
#include "queue.h"

#define MAX_LINE_LENGTH 132
#ifdef __cplusplus
class OutputStream;
using comms_msg_t = struct {char pline[MAX_LINE_LENGTH]; OutputStream *pos; };
extern "C" {
bool send_message_queue(const char *pline, OutputStream *pos, bool wait=true);
bool receive_message_queue(char **ppline, OutputStream **ppos);
int get_message_queue_space();
#else
// for c calls
bool send_message_queue(const char *pline, void *pos);
#endif

bool create_message_queue();
QueueHandle_t get_message_queue();

#ifdef __cplusplus
}
#endif
