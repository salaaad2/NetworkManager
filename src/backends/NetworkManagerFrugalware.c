/* NetworkManager -- Network link manager
 *
 * Implementation for the Frugalware Linux distro - http://www.frugalware.org
 *
 * Alex Smith <alex.extreme2@gmail.com>
 *
 * Based on NetworkManagerSlackware.c by Narayan Newton <narayan_newton@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * (C) Copyright 2006 Alex Smith
 */

#include <stdio.h>
#include <sys/types.h>
#include <signal.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>

#include "NetworkManagerSystem.h"
#include "NetworkManagerUtils.h"
#include "nm-device.h"
#include "nm-device-802-3-ethernet.h"
#include "nm-device-802-11-wireless.h"
#include "nm-utils.h"

// Provided by the frugalwareutils package on Frugalware
#include <libfwnetconfig.h>

/*
 * nm_system_init
 *
 * Initializes the distribution-specific system backend
 *
 */
void nm_system_init (void)
{
}

/*
 * nm_system_device_flush_routes
 *
 * Flush all routes associated with a network device
 *
 */
void nm_system_device_flush_routes (NMDevice *dev)
{
	g_return_if_fail (dev != NULL);

	nm_system_device_flush_routes_with_iface (nm_device_get_iface (dev));
}

/*
 * nm_system_device_flush_routes_with_iface
 *
 * Flush all routes associated with a network device
 *
 */
void nm_system_device_flush_routes_with_iface (const char *iface)
{
	char	*buf;

	g_return_if_fail (iface != NULL);

	/* Remove routing table entries */
	buf = g_strdup_printf ("/usr/sbin/ip route flush dev %s", iface);
	nm_spawn_process (buf);
	g_free (buf);
}


/*
 * nm_system_device_flush_addresses
 *
 * Flush all network addresses associated with a network device
 *
 */
void nm_system_device_flush_addresses (NMDevice *dev)
{
	g_return_if_fail (dev != NULL);

	nm_system_device_flush_addresses_with_iface (nm_device_get_iface (dev));
}

/*
 * nm_system_device_flush_addresses_with_iface
 *
 * Flush all network addresses associated with a network device
 *
 */
void nm_system_device_flush_addresses_with_iface (const char *iface)
{
	char	*buf;

	g_return_if_fail (iface != NULL);

	/* Remove all IP addresses for a device */
	buf = g_strdup_printf ("/usr/sbin/ip addr flush dev %s", iface);
	nm_spawn_process (buf);
	g_free (buf);
}

/*
 * nm_system_device_has_active_routes
 *
 * Find out whether the specified device has any routes in the routing
 * table.
 *
 */
gboolean nm_system_device_has_active_routes (NMDevice *dev)
{
	return FALSE;
}


/*
 * nm_system_enable_loopback
 *
 * Bring up the loopback interface
 *
 */
void nm_system_enable_loopback (void)
{
	fwnet_loup ();
}


/*
 * nm_system_kill_all_dhcp_daemons
 *
 * Kill all DHCP daemons currently running, done at startup.
 *
 */
void nm_system_kill_all_dhcp_daemons (void)
{
	nm_spawn_process ("/usr/bin/killall -q dhclient");
}


/*
 * nm_system_update_dns
 *
 * Make glibc/nscd aware of any changes to the resolv.conf file by
 * restarting nscd.
 *
 */
void nm_system_update_dns (void)
{
	/* I'm not running nscd */
}

/*
 * nm_system_restart_mdns_responder
 *
 * Restart the multicast DNS responder so that it knows about new
 * network interfaces and IP addresses.
 *
 */
void nm_system_restart_mdns_responder (void)
{
	/* Check if the daemon was already running - do not start a new instance */
	if (g_file_test("/var/run/avahi-daemon/pid", G_FILE_TEST_EXISTS))
	{
		nm_spawn_process ("/etc/rc.d/rc.avahi-daemon restart");
	}
}


/*
 * nm_system_device_add_ip6_link_address
 *
 * Add a default link-local IPv6 address to a device.
 *
 */
void nm_system_device_add_ip6_link_address (NMDevice *dev)
{
	char *buf;
	struct ether_addr hw_addr;
	unsigned char eui[8];

	if (NM_IS_DEVICE_802_3_ETHERNET (dev))
		nm_device_802_3_ethernet_get_address (NM_DEVICE_802_3_ETHERNET (dev), &hw_addr);
	else if (NM_IS_DEVICE_802_11_WIRELESS (dev))
		nm_device_802_11_wireless_get_address (NM_DEVICE_802_11_WIRELESS (dev), &hw_addr);

	memcpy (eui, &(hw_addr.ether_addr_octet), sizeof (hw_addr.ether_addr_octet));
	memmove (eui+5, eui+3, 3);
	eui[3] = 0xff;
	eui[4] = 0xfe;
	eui[0] ^= 2;

	/* Add the default link-local IPv6 address to a device */
	buf = g_strdup_printf ("/usr/sbin/ip -6 addr add fe80::%x%02x:%x%02x:%x%02x:%x%02x/64 dev %s",
	                       eui[0], eui[1], eui[2], eui[3], eui[4], eui[5],
	                       eui[6], eui[7], nm_device_get_iface (dev));
	nm_spawn_process (buf);
	g_free (buf);
}

/*
 * nm_system_device_add_route_via_device_with_iface
 *
 * Add route to the given device
 *
 */
void nm_system_device_add_route_via_device_with_iface (const char *iface, const char *route)
{
	char	*buf;

	g_return_if_fail (iface != NULL);

	/* Add default gateway */
	buf = g_strdup_printf ("/usr/sbin/ip route add %s dev %s", route, iface);
	nm_spawn_process (buf);
	g_free (buf);
}


/*
 * nm_system_device_replace_default_route
 *
 * Add default route to the given device
 *
 */
void
nm_system_device_replace_default_route (const char *iface,
                                        guint32 gw,
                                        guint32 mss)
{
	nm_generic_device_replace_default_route (iface, gw, mss);
}

 
/*
 * nm_system_flush_loopback_routes
 *
 * Flush all routes associated with the loopback device, because it
 * sometimes gets the first route for ZeroConf/Link-Local traffic.
 *
 */
void nm_system_flush_loopback_routes (void)
{
	nm_system_device_flush_routes_with_iface ("lo");
}

 
/*
 * nm_system_flush_arp_cache
 *
 * Flush all entries in the arp cache.
 *
 */
void nm_system_flush_arp_cache (void)
{
	nm_spawn_process ("/usr/sbin/ip neigh flush all");
}

/*
 * nm_system_activate_nis
 *
 * set up the nis domain and write a yp.conf
 *
 */
void nm_system_activate_nis (NMIP4Config *config)
{
}

/*
 * nm_system_shutdown_nis
 *
 * shutdown ypbind
 *
 */
void nm_system_shutdown_nis (void)
{
}

/*
 * nm_system_set_hostname
 *
 * set the hostname
 *
 */
void nm_system_set_hostname (NMIP4Config *config)
{
}

/*
 * nm_system_should_modify_resolv_conf
 *
 * Can NM update resolv.conf, or is it locked down?
 */
gboolean nm_system_should_modify_resolv_conf (void)
{
	return TRUE;
}

