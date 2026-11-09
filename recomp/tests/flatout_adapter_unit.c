#include "../runtime/flatout_adapter.h"
#include <assert.h>

int main(void) {
    NetIdent id = {0};
    char ip[16], mask[16];
    assert(!flatout_adapter_identity(&id, ip, mask));
    const unsigned char addr[] = {10, 77, 2, 2};
    const unsigned char subnet[] = {255, 255, 224, 0};
    memcpy(&id.ip, addr, sizeof addr);
    memcpy(&id.mask, subnet, sizeof subnet);
    assert(flatout_adapter_identity(&id, ip, mask));
    assert(!strcmp(ip, "10.77.2.2"));
    assert(!strcmp(mask, "255.255.224.0"));
    const unsigned char changed[] = {10, 77, 1, 2};
    memcpy(&id.ip, changed, sizeof changed);
    assert(flatout_adapter_identity(&id, ip, mask));
    assert(!strcmp(ip, "10.77.1.2"));
    return 0;
}
