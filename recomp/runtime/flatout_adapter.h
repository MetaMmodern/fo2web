#ifndef FLATOUT_ADAPTER_H
#define FLATOUT_ADAPTER_H

#include "winsock.h"
#include <stdio.h>
#include <string.h>

/* NetIdent addresses are stored in network byte order, including on little
 * endian hosts. Copy their bytes instead of shifting the host-order integer. */
static inline void flatout_adapter_ip(char out[16], uint32_t address) {
    unsigned char b[4];
    memcpy(b, &address, sizeof b);
    snprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static inline int flatout_adapter_identity(const NetIdent *id, char ip[16], char mask[16]) {
    if (!id->ip) return 0;
    flatout_adapter_ip(ip, id->ip);
    flatout_adapter_ip(mask, id->mask);
    return 1;
}
#endif
