/* IPXWrapper - Interface functions
 * Copyright (C) 2011 Daniel Collins <solemnwarning@solemnwarning.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <windows.h>
#include <iphlpapi.h>
#include <utlist.h>
#include <time.h>

#include "interface.h"
#include "common.h"
#include "config.h"

#define INTERFACE_CACHE_TTL 5

static CRITICAL_SECTION interface_cache_cs;

static ipx_interface_t *interface_cache = NULL;
static time_t interface_cache_ctime = 0;

/* Fetch a list of network interfaces available on the system.
 *
 * Returns a linked list of IP_ADAPTER_INFO structures, all allocated within a
 * single memory block beginning at the first node.
*/
IP_ADAPTER_INFO *load_sys_interfaces(void)
{
	IP_ADAPTER_INFO *ifroot = NULL, *ifptr;
	ULONG bufsize = sizeof(IP_ADAPTER_INFO) * 8;
	
	int err = ERROR_BUFFER_OVERFLOW;
	
	while(err == ERROR_BUFFER_OVERFLOW)
	{
		if(!(ifptr = realloc(ifroot, bufsize)))
		{
			log_printf(LOG_ERROR, "Couldn't allocate IP_ADAPTER_INFO structures!");
			break;
		}
		
		ifroot = ifptr;
		
		err = GetAdaptersInfo(ifroot, &bufsize);
		
		if(err == ERROR_NO_DATA)
		{
			log_printf(LOG_WARNING, "No network interfaces detected!");
			break;
		}
		else if(err != ERROR_SUCCESS && err != ERROR_BUFFER_OVERFLOW)
		{
			log_printf(LOG_ERROR, "Error fetching network interfaces: %s", w32_error(err));
			break;
		}
	}
	
	if(err != ERROR_SUCCESS)
	{
		free(ifroot);
		return NULL;
	}
	
	return ifroot;
}

/* Load a list of virtual IPX interfaces. */
ipx_interface_t *load_ipx_interfaces(void)
{
	IP_ADAPTER_INFO *ifroot = load_sys_interfaces(), *ifptr;
	
	addr48_t primary = get_primary_iface();
	
	ipx_interface_t *nics = NULL;
	
	for(ifptr = ifroot; ifptr; ifptr = ifptr->Next)
	{
		addr48_t hwaddr = addr48_in(ifptr->Address);
		
		iface_config_t config = get_iface_config(hwaddr);
		
		if(!config.enabled)
		{
			/* Interface has been disabled, don't add it */
			
			ifptr = ifptr->Next;
			continue;
		}
		
		ipx_interface_t *iface = malloc(sizeof(ipx_interface_t));
		if(!iface)
		{
			log_printf(LOG_ERROR, "Couldn't allocate ipx_interface!");
			
			free_ipx_interface_list(&nics);
			return NULL;
		}
		
		iface->ipaddr = NULL;
		
		IP_ADDR_STRING *ip_ptr = &(ifptr->IpAddressList);
		
		for(; ip_ptr; ip_ptr = ip_ptr->Next)
		{
			uint32_t ipaddr  = inet_addr(ip_ptr->IpAddress.String);
			uint32_t netmask = inet_addr(ip_ptr->IpMask.String);
			
			if(ipaddr == 0)
			{
				/* No IP address.
				 * Because an empty linked list would be silly.
				*/
				
				continue;
			}
			
			ipx_interface_ip_t *addr = malloc(sizeof(ipx_interface_ip_t));
			if(!addr)
			{
				log_printf(LOG_ERROR, "Couldn't allocate ipx_interface_ip!");
				
				free_ipx_interface(iface);
				free_ipx_interface_list(&nics);
				
				continue;
			}
			
			addr->ipaddr  = ipaddr;
			addr->netmask = netmask;
			addr->bcast   = ipaddr | (~netmask);
			
			DL_APPEND(iface->ipaddr, addr);
		}
		
		iface->hwaddr = hwaddr;
		
		iface->ipx_net  = config.netnum;
		iface->ipx_node = config.nodenum;
		
		/* Workaround for buggy versions of Hamachi that don't initialise
		 * the interface hardware address correctly.
		*/
		
		unsigned char hamachi_bug[] = {0x7A, 0x79, 0x00, 0x00, 0x00, 0x00};
		
		if(iface->ipx_node == addr48_in(hamachi_bug) && iface->ipaddr)
		{
			log_printf(LOG_WARNING, "Invalid Hamachi interface detected, correcting node number");
			
			addr32_out(hamachi_bug + 2, iface->ipaddr->ipaddr);
			iface->ipx_node = addr48_in(hamachi_bug);
		}
		
		if(iface->hwaddr == primary)
		{
			/* Primary interface, insert at the start of the list */
			DL_PREPEND(nics, iface);
		}
		else{
			DL_APPEND(nics, iface);
		}
	}
	
	free(ifroot);
	
	return nics;
}

/* Deep copy an ipx_interface structure.
 * Returns NULL on malloc failure.
*/
ipx_interface_t *copy_ipx_interface(const ipx_interface_t *src)
{
	ipx_interface_t *dest = malloc(sizeof(ipx_interface_t));
	if(!dest)
	{
		log_printf(LOG_ERROR, "Cannot allocate ipx_interface!");
		return NULL;
	}
	
	*dest = *src;
	
	dest->ipaddr = NULL;
	dest->prev   = NULL;
	dest->next   = NULL;
	
	ipx_interface_ip_t *ip;
	
	DL_FOREACH(src->ipaddr, ip)
	{
		ipx_interface_ip_t *new_ip = malloc(sizeof(ipx_interface_ip_t));
		if(!new_ip)
		{
			log_printf(LOG_ERROR, "Cannot allocate ipx_interface_ip!");
			
			free_ipx_interface(dest);
			return NULL;
		}
		
		*new_ip = *ip;
		
		DL_APPEND(dest->ipaddr, new_ip);
	}
	
	return dest;
}

/* Free an ipx_interface structure and any memory allocated within. */
void free_ipx_interface(ipx_interface_t *iface)
{
	if(iface == NULL)
	{
		return;
	}
	
	ipx_interface_ip_t *a, *a_tmp;
	
	DL_FOREACH_SAFE(iface->ipaddr, a, a_tmp)
	{
		DL_DELETE(iface->ipaddr, a);
		free(a);
	}
	
	free(iface);
}

/* Deep copy an entire list of ipx_interface structures.
 * Returns NULL on malloc failure.
*/
ipx_interface_t *copy_ipx_interface_list(const ipx_interface_t *src)
{
	ipx_interface_t *dest = NULL;
	
	const ipx_interface_t *s;
	
	DL_FOREACH(src, s)
	{
		ipx_interface_t *d = copy_ipx_interface(s);
		if(!d)
		{
			free_ipx_interface_list(&dest);
			return NULL;
		}
		
		DL_APPEND(dest, d);
	}
	
	return dest;
}

/* Free a list of ipx_interface structures */
void free_ipx_interface_list(ipx_interface_t **list)
{
	ipx_interface_t *iface, *tmp;
	
	DL_FOREACH_SAFE(*list, iface, tmp)
	{
		DL_DELETE(*list, iface);
		free_ipx_interface(iface);
	}
}

/* Initialise the IPX interface cache. */
void ipx_interfaces_init(void)
{
	interface_cache       = NULL;
	interface_cache_ctime = 0;
	
	if(!InitializeCriticalSectionAndSpinCount(&interface_cache_cs, 0x80000000))
	{
		log_printf(LOG_ERROR, "Failed to initialise critical section: %s", w32_error(GetLastError()));
		abort();
	}
}

/* Release any resources used by the IPX interface cache. */
void ipx_interfaces_cleanup(void)
{
	DeleteCriticalSection(&interface_cache_cs);
	
	free_ipx_interface_list(&interface_cache);
}

/* Check the age of the IPX interface cache and reload it if necessary.
 * Ensure you hold interface_cache_cs before calling.
*/
static void renew_interface_cache(void)
{
	if(time(NULL) - interface_cache_ctime > INTERFACE_CACHE_TTL)
	{
		free_ipx_interface_list(&interface_cache);
		
		interface_cache       = load_ipx_interfaces();
		interface_cache_ctime = time(NULL);
	}
}

/* Return a copy of the IPX interface cache. The cache will be reloaded before
 * copying if too old.
*/
ipx_interface_t *get_ipx_interfaces(void)
{
	EnterCriticalSection(&interface_cache_cs);
	
	renew_interface_cache();
	
	ipx_interface_t *copy = copy_ipx_interface_list(interface_cache);
	
	LeaveCriticalSection(&interface_cache_cs);
	
	return copy;
}

/* Search for an IPX interface by address.
 * Returns NULL if the interface doesn't exist or malloc failure.
*/
ipx_interface_t *ipx_interface_by_addr(addr32_t net, addr48_t node)
{
	EnterCriticalSection(&interface_cache_cs);
	
	renew_interface_cache();
	
	ipx_interface_t *iface;
	
	DL_FOREACH(interface_cache, iface)
	{
		if(iface->ipx_net == net && iface->ipx_node == node)
		{
			iface = copy_ipx_interface(iface);
			break;
		}
	}
	
	LeaveCriticalSection(&interface_cache_cs);
	
	return iface;
}

/* Search for an IPX interface by index.
 * Returns NULL if the interface doesn't exist or malloc failure.
*/
ipx_interface_t *ipx_interface_by_index(int index)
{
	EnterCriticalSection(&interface_cache_cs);
	
	renew_interface_cache();
	
	int iface_index = 0;
	ipx_interface_t *iface;
	
	DL_FOREACH(interface_cache, iface)
	{
		if(iface_index++ == index)
		{
			iface = copy_ipx_interface(iface);
			break;
		}
	}
	
	LeaveCriticalSection(&interface_cache_cs);
	
	return iface;
}

/* Returns the number of IPX interfaces. */
int ipx_interface_count(void)
{
	EnterCriticalSection(&interface_cache_cs);
	
	renew_interface_cache();
	
	int count = 0;
	ipx_interface_t *iface;
	
	DL_FOREACH(interface_cache, iface)
	{
		count++;
	}
	
	LeaveCriticalSection(&interface_cache_cs);
	
	return count;
}
