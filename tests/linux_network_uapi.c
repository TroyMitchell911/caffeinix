#include <stddef.h>

#include <linux_uapi.h>

#include <net/if.h>
#include <linux/if_arp.h>
#include <linux/sockios.h>

_Static_assert(sizeof(struct linux_ifreq) == sizeof(struct ifreq),
	       "ifreq size");
_Static_assert(offsetof(struct linux_ifreq, data) ==
	       offsetof(struct ifreq, ifr_ifru), "ifreq union offset");
_Static_assert(sizeof(struct linux_ifconf) == sizeof(struct ifconf),
	       "ifconf size");
_Static_assert(offsetof(struct linux_ifconf, buffer) ==
	       offsetof(struct ifconf, ifc_ifcu), "ifconf buffer offset");
_Static_assert(LINUX_SIOCGIFNAME == SIOCGIFNAME,
	       "SIOCGIFNAME value");
_Static_assert(LINUX_SIOCGIFCONF == SIOCGIFCONF,
	       "SIOCGIFCONF value");
_Static_assert(LINUX_SIOCGIFFLAGS == SIOCGIFFLAGS,
	       "SIOCGIFFLAGS value");
_Static_assert(LINUX_SIOCGIFADDR == SIOCGIFADDR,
	       "SIOCGIFADDR value");
_Static_assert(LINUX_SIOCGIFBRDADDR == SIOCGIFBRDADDR,
	       "SIOCGIFBRDADDR value");
_Static_assert(LINUX_SIOCGIFNETMASK == SIOCGIFNETMASK,
	       "SIOCGIFNETMASK value");
_Static_assert(LINUX_SIOCGIFMETRIC == SIOCGIFMETRIC,
	       "SIOCGIFMETRIC value");
_Static_assert(LINUX_SIOCGIFMTU == SIOCGIFMTU,
	       "SIOCGIFMTU value");
_Static_assert(LINUX_SIOCGIFHWADDR == SIOCGIFHWADDR,
	       "SIOCGIFHWADDR value");
_Static_assert(LINUX_SIOCGIFINDEX == SIOCGIFINDEX,
	       "SIOCGIFINDEX value");
_Static_assert(LINUX_IFF_UP == IFF_UP, "IFF_UP value");
_Static_assert(LINUX_IFF_BROADCAST == IFF_BROADCAST,
	       "IFF_BROADCAST value");
_Static_assert(LINUX_IFF_LOOPBACK == IFF_LOOPBACK,
	       "IFF_LOOPBACK value");
_Static_assert(LINUX_IFF_RUNNING == IFF_RUNNING,
	       "IFF_RUNNING value");
_Static_assert(LINUX_ARPHRD_ETHER == ARPHRD_ETHER,
	       "ARPHRD_ETHER value");
_Static_assert(LINUX_ARPHRD_LOOPBACK == ARPHRD_LOOPBACK,
	       "ARPHRD_LOOPBACK value");
