#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/api.h"
#include "lwip/sockets.h"

#include "main.h"
#include "OutputStream.h"
#include "RingBuffer.h"
#include "MessageQueue.h"

#include "FreeRTOS.h"
#include "timers.h"
#include "queue.h"

#include <set>

#if !(LWIP_SOCKET && LWIP_SOCKET_SELECT)
#error LWIP_SOCKET_SELECT and  LWIP_SOCKET needed
#endif
// #if LWIP_NETCONN_FULLDUPLEX != 1
// #error LWIP_NETCONN_FULLDUPLEX is required for this to work
// #endif

#define MAX_SERV 3
#define BUFSIZE 256
#define MAGIC 0x6013D852
struct shell_state_t {
    int socket;
    struct sockaddr_storage cliaddr;
    socklen_t clilen;
    OutputStream *os;
    QueueHandle_t tx_queue;
    char line[132];
    size_t cnt;
    bool discard;
    bool need_write;
    uint32_t magic;
};
using shell_t = struct shell_state_t;
static std::set<shell_t*> shells;

using tx_msg_t = struct{char *buf; uint16_t size:16; uint16_t off:16; };

// Stores OutputStreams that need to be deleted when done
static RingBuffer<OutputStream*, MAX_SERV*2> gc;

// callback from command thread to write data to the socket
// lwip_write should not be called from a different thread
// so we use a queue to get the shell thread to do the write
static int write_back(shell_t *p_shell, const char *rbuf, size_t len)
{
    if(p_shell->magic != MAGIC) {
        // we probably went away
        printf("shell: write_back: ERROR magic was bad\n");
        return 0;
    }

    char *p= new char[len];
    if(p == nullptr) {
        printf("shell: out of memory in write_back\n");
        return 0;
    }
    memcpy(p, rbuf, len);
    tx_msg_t msg {p, (uint16_t)len, 0};
    if(xQueueSend(p_shell->tx_queue, (void *)&msg, portMAX_DELAY) != pdTRUE) {
        delete [] p;
        return 0;
    }

    p_shell->need_write= true;
    return len;
}

/**************************************************************
 * Close the socket and remove this shell_t from the list.
 **************************************************************/
static void close_shell(shell_t *p_shell)
{
    p_shell->magic= 0; // safety
    // if we delete the OutputStream now and command thread is still outputting stuff we will crash
    // it needs to stick around until the command has completed
    if(p_shell->os->is_done()) {
        printf("shell: releasing output stream: %p\n", p_shell->os);
        delete p_shell->os;
    }else{
        printf("shell: delaying releasing output stream: %p\n", p_shell->os);
        p_shell->os->set_closed();
        gc.push_back(p_shell->os);
    }
    // before deleting the queue free up any buffers in it
    while(uxQueueMessagesWaiting(p_shell->tx_queue) > 0) {
        tx_msg_t msg;
        if(xQueueReceive(p_shell->tx_queue, &msg, 0) == pdTRUE) {
            delete [] msg.buf;
        }
    }
    vQueueDelete(p_shell->tx_queue);

    printf("shell: closing shell connection: %d\n", p_shell->socket);
    lwip_close(p_shell->socket);

    // Free shell
    if(shells.erase(p_shell) != 1) {
        printf("shell: erasing shell not found\n");
    }
    mem_free(p_shell);
}

// This will delete any OutputStreams that are done
// we need to do this so that we don't crash when an OutputStream is deleted before it is done
static void os_garbage_collector( TimerHandle_t xTimer )
{
    while(!gc.empty()) {
        // we only check the oldest, presuming it will be done before newer ones
        OutputStream *os= gc.peek_front();
        if(os->is_done()) {
            os= gc.pop_front();
            delete os;
            printf("shell: releasing output stream: %p\n", os);
        } else {
            // if this is not done then we presume the newer ones aren't either
            break;
        }
    }
}

// process any write requests coming from other threads
// return false if the shell closed
static bool process_writes(shell_t *p_shell)
{
    tx_msg_t msg;
    if(xQueuePeek(p_shell->tx_queue, &msg, 0) == pdTRUE) {
        int n;
        if((n=lwip_write(p_shell->socket, msg.buf+msg.off, msg.size-msg.off)) < 0) {
            printf("shell: error writing\n");
            close_shell(p_shell);
            return false;
        }
        if(n == (msg.size-msg.off)) {
            // all written, so remove from queue
            delete [] msg.buf;
            xQueueReceive(p_shell->tx_queue, &msg, 0);
        }else{
            // can't write anymore at the moment try again next time
            msg.off += n;
        }
    }
    return true;
}

static void shell_thread(void *arg)
{
    LWIP_UNUSED_ARG(arg);
    int listenfd;
    struct sockaddr_in shell_saddr;
    fd_set readset;
    fd_set writeset;
    int i, maxfdp1;

    printf("Network: Shell thread started\n");

    // OutputStream garbage collector timer
    TimerHandle_t timer_handle= xTimerCreate("osgarbage", pdMS_TO_TICKS(1000), pdTRUE, nullptr, os_garbage_collector);
    if( xTimerStart( timer_handle, 1000 ) != pdPASS ) {
        printf("shell_thread: ERROR: Failed to start the timer\n");
    }

    // does not do anything unless LWIP_NETCONN_SEM_PER_THREAD==1
    lwip_socket_thread_init();

    memset(&shell_saddr, 0, sizeof (shell_saddr));

    /* First acquire our socket for listening for connections */
    listenfd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    shell_saddr.sin_family = AF_INET;
    shell_saddr.sin_addr.s_addr = PP_HTONL(INADDR_ANY);
    shell_saddr.sin_port = lwip_htons(23); /* telnet server port */

    LWIP_ASSERT("shell_thread: Socket create failed.", listenfd >= 0);

    if (lwip_bind(listenfd, (struct sockaddr *) &shell_saddr, sizeof (shell_saddr)) == -1) {
        LWIP_ASSERT("shell_thread: Socket bind failed.", 0);
    }

    /* Put socket into listening mode */
    if (lwip_listen(listenfd, MAX_SERV) == -1) {
        LWIP_ASSERT("shell_thread: Listen failed.", 0);
    }

    struct timeval timeout;
    timeout.tv_sec= 0;
    timeout.tv_usec= 10000; // 10ms

    /* Wait forever for network input: This could be connections or data */
    for (;;) {
        maxfdp1 = listenfd + 1;

        /* Determine what sockets need to be in readset */
        FD_ZERO(&readset);
        FD_ZERO(&writeset);
        FD_SET(listenfd, &readset);
        for(auto p_shell : shells) {
            if (maxfdp1 < p_shell->socket + 1) {
                maxfdp1 = p_shell->socket + 1;
            }
            FD_SET(p_shell->socket, &readset);
            if(p_shell->need_write){
                FD_SET(p_shell->socket, &writeset);
            }
        }

        // Wait for data or a new connection, we have a timeout so we can check if we need to wait on write
        i = lwip_select(maxfdp1, &readset, &writeset, 0, &timeout);

        if (i == 0) {
            continue;
        }

        /* At least one descriptor is ready */
        if (FD_ISSET(listenfd, &readset)) {
            /* We have a new connection request */
            /* create a new control block */
            shell_t *p_shell = (shell_t *) mem_malloc(sizeof(shell_t));
            if(p_shell != nullptr) {
                p_shell->socket = lwip_accept(listenfd, (struct sockaddr *) &p_shell->cliaddr, &p_shell->clilen);
                if (p_shell->socket < 0) {
                    mem_free(p_shell);
                    printf("shell: accept socket error: %d\n", errno);

                } else {
                    // add shell state to our set of shells
                    shells.insert(p_shell);

                    printf("shell: accepted shell connection: %d\n", p_shell->socket);

                    // initialise command buffer state
                    p_shell->need_write= false;
                    p_shell->cnt = 0;
                    p_shell->discard = false;
                    // setup tx queue so we keep reads and writes in the same thread due to lwip limitations
                    p_shell->tx_queue= xQueueCreate(4, sizeof(tx_msg_t));
                    if(p_shell->tx_queue == 0) {
                        // Failed to create the queue.
                        printf("shell: failed to create tx_queue - out of memory\n");
                        close_shell(p_shell);

                    } else {
                        p_shell->os = new OutputStream([p_shell](const char *ibuf, size_t ilen) { return write_back(p_shell, ibuf, ilen); });

                        //output_streams.push_back(p_shell->os);
                        p_shell->magic= MAGIC;
                        lwip_write(p_shell->socket, "Welcome to the Smoothie Shell\n", 30);
                    }
                }

            } else {
                /* No memory to accept connection. Just accept and then close */
                int sock;
                struct sockaddr cliaddr;
                socklen_t clilen;

                sock = lwip_accept(listenfd, &cliaddr, &clilen);
                if (sock >= 0) {
                    lwip_close(sock);
                }
                printf("shell: out of memory on listen\n");
            }
        }

        // Go through list of connected clients and process write requests
        // we do this first to avoid deadlock when a read request tries to write
        // can still deadlock though if there is a lot to write (eg cat file)
        for(auto p_shell : shells) {
            if (FD_ISSET(p_shell->socket, &writeset)) {
                // request to write data
                if(process_writes(p_shell)) {
                    if(uxQueueMessagesWaiting(p_shell->tx_queue) == 0) {
                        // if nothing left to write don't select on write ready anymore
                        p_shell->need_write= false;
                    }

                }else {
                    break;
                }
            }
        }

        // check for read requests
        for (auto p_shell : shells) {
            if (FD_ISSET(p_shell->socket, &readset)) {
                char buf[BUFSIZE];
                // This socket is ready for reading.
                int n = lwip_read(p_shell->socket, buf, BUFSIZE);
                if (n > 0) {
                    if(strncmp(buf, "quit\n", 5) == 0 || strncmp(buf, "quit\r\n", 6) == 0) {
                        lwip_write(p_shell->socket, "Goodbye!\n", 9);
                        close_shell(p_shell);
                        break;
                    }
                    // this could block which would then also block any output that the command thread needs to make
                    // so tell it to not wait, and if it returns false it means it gave up waiting
                    // so process writes while waiting
                    if(!process_command_buffer(n, buf, p_shell->os, p_shell->line, p_shell->cnt, p_shell->discard, false)) {
                        // and keep trying to resubmit, this will yield for about 100ms
                        while(!send_message_queue(p_shell->line, p_shell->os, false)) {
                            // process any writes
                            if(!process_writes(p_shell)) {
                                p_shell= nullptr;
                                // the shell closed here we may lose the last command sent
                                break;
                            }
                        }
                        if(p_shell == nullptr) break;
                    }

                } else {
                    close_shell(p_shell);
                    break;
                }
            }
        }
    }
    lwip_socket_thread_cleanup();
}

void shell_init(void)
{
    sys_thread_new("shell_thread", shell_thread, NULL, 350, DEFAULT_THREAD_PRIO);
}
