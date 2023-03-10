#include "MessageQueue.h"
#include "OutputStream.h"

#include <string.h>

static QueueHandle_t queue_handle;

bool create_message_queue()
{
    // create queue for dispatch of lines, can be sent to by several tasks
    queue_handle = xQueueCreate(10, sizeof(comms_msg_t));
    if( queue_handle == 0 ) {
        // Failed to create the queue.
        printf("ERROR: failed to create dispatch queue\n");
       	return false;
    }

    return true;
}

QueueHandle_t get_message_queue()
{
	return queue_handle;
}

int get_message_queue_space()
{
    return uxQueueSpacesAvailable(queue_handle);
}

// can be called by several threads to submit messages to the dispatcher
// the line is copied into the message so can be on the stack
// This call will block until there is room in the queue unless wait is false
// in which case it will will not wait at all
bool send_message_queue(const char *pline, OutputStream *pos, bool wait)
{
    comms_msg_t msg_buffer;
	strcpy(msg_buffer.pline, pline);
	msg_buffer.pos= pos;
    TickType_t waitms = wait ? portMAX_DELAY : 0;
	BaseType_t r= xQueueSend( queue_handle, ( void * )&msg_buffer, waitms);
    return r == pdTRUE;
}

bool send_message_queue(const char *pline, void *pos)
{
	return send_message_queue(pline, (OutputStream*)pos);
}

// Only called by the command thread to receive incoming lines to process
bool receive_message_queue(char **ppline, OutputStream **ppos)
{
    const TickType_t waitms = pdMS_TO_TICKS( 100 );
    static comms_msg_t msg_buffer;
    if( xQueueReceive( queue_handle, &msg_buffer, waitms) ) {
	    *ppline = msg_buffer.pline;
	    *ppos = msg_buffer.pos;

    }else{
    	return false;
    }

    return true;
}
