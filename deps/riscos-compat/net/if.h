/* <net/if.h> plus if_nametoindex() for RISC OS: UnixLib declares it under
 * #if 0 and does not provide it.  Every interface name is unknown, which
 * only affects IPv6 link-local scopes in libssh. */
#ifndef RISCOS_COMPAT_NET_IF_H
#define RISCOS_COMPAT_NET_IF_H
#include_next <net/if.h>
static inline unsigned int if_nametoindex(const char *ifname)
{
    (void)ifname;
    return 0;
}
#endif
