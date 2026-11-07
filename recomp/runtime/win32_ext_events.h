/* The kernel event objects (win32_ext.c) as WS2_32's WSAEVENT, for
 * winsock.c's waitable/overlapped path. A WSAEVENT is a HANDLE to the same
 * object, so the two APIs share one table. */
#ifndef WIN32_EXT_EVENTS_H
#define WIN32_EXT_EVENTS_H
#include <stdint.h>

uint32_t win32_event_new(void);
int win32_event_set(uint32_t h);
int win32_event_reset(uint32_t h);
int win32_event_close(uint32_t h);
/* first signalled index, or -1 on timeout (ms == 0xFFFFFFFF waits forever) */
int win32_event_wait(const uint32_t *hs, int n, int wait_all, uint32_t ms);

#endif
