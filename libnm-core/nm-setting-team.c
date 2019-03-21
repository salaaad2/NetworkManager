/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 * Copyright 2017 Red Hat, Inc.
 * Copyright 2013 Jiri Pirko <jiri@resnulli.us>
 */

#include "nm-default.h"

#include "nm-setting-team.h"

#include <stdlib.h>

#include "nm-utils.h"
#include "nm-utils-private.h"
#include "nm-connection-private.h"

/**
 * SECTION:nm-setting-team
 * @short_description: Describes connection properties for teams
 *
 * The #NMSettingTeam object is a #NMSetting subclass that describes properties
 * necessary for team connections.
 **/

/*****************************************************************************
 * NMTeamLinkWatch
 *****************************************************************************/

G_DEFINE_BOXED_TYPE (NMTeamLinkWatcher, nm_team_link_watcher,
                     nm_team_link_watcher_dup, nm_team_link_watcher_unref)

enum LinkWatcherTypes {
	LINK_WATCHER_ETHTOOL   = 0,
	LINK_WATCHER_NSNA_PING = 1,
	LINK_WATCHER_ARP_PING  = 2
};

static const char* _link_watcher_name[] = {
	[LINK_WATCHER_ETHTOOL]   = NM_TEAM_LINK_WATCHER_ETHTOOL,
	[LINK_WATCHER_NSNA_PING] = NM_TEAM_LINK_WATCHER_NSNA_PING,
	[LINK_WATCHER_ARP_PING]  = NM_TEAM_LINK_WATCHER_ARP_PING
};

struct NMTeamLinkWatcher {
	guint refcount;

	guint8 type; /* LinkWatcherTypes */

	/*
	 * The union is constructed in order to allow mapping the options of all the
	 * watchers on the arp_ping one: this would allow to manipulate all the watchers
	 * by using the arp_ping struct. See for instance the nm_team_link_watcher_unref()
	 * and nm_team_link_watcher_equal() functions. So, if you need to change the union
	 * be careful.
	 */
	union {
		struct {
			int delay_up;
			int delay_down;
		} ethtool;
		struct {
			int init_wait;
			int interval;
			int missed_max;
			char *target_host;
		} nsna_ping;
		struct {
			int init_wait;
			int interval;
			int missed_max;
			char *target_host;
			char *source_host;
			NMTeamLinkWatcherArpPingFlags flags;
			int vlanid;
		} arp_ping;
	};
};

#define _CHECK_WATCHER_VOID(watcher) \
	G_STMT_START { \
		g_return_if_fail (watcher != NULL); \
		g_return_if_fail (watcher->refcount > 0); \
		g_return_if_fail (watcher->type <= LINK_WATCHER_ARP_PING); \
	} G_STMT_END

#define _CHECK_WATCHER(watcher, err_val) \
	G_STMT_START { \
		g_return_val_if_fail (watcher != NULL, err_val); \
		g_return_val_if_fail (watcher->refcount > 0, err_val); \
		g_return_val_if_fail (watcher->type <= LINK_WATCHER_ARP_PING, err_val); \
	} G_STMT_END

/**
 * nm_team_link_watcher_new_ethtool:
 * @delay_up: delay_up value
 * @delay_down: delay_down value
 * @error: this call never fails, so this var is not used but kept for format
 *   consistency with the link_watcher constructors of other type
 *
 * Creates a new ethtool #NMTeamLinkWatcher object
 *
 * Returns: (transfer full): the new #NMTeamLinkWatcher object
 *
 * Since: 1.12
 **/
NMTeamLinkWatcher *
nm_team_link_watcher_new_ethtool (int delay_up,
                                  int delay_down,
                                  GError **error)
{
	NMTeamLinkWatcher *watcher;
	const char *val_fail = NULL;

	if (delay_up < 0 || !_NM_INT_LE_MAXINT32 (delay_up))
		val_fail = "delay-up";
	if (delay_down < 0 || !_NM_INT_LE_MAXINT32 (delay_down))
		val_fail = "delay-down";
	if (val_fail) {
		g_set_error (error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_FAILED,
		             _("%s is out of range [0, %d]"), val_fail, G_MAXINT32);
		return NULL;
	}

	watcher = g_slice_new0 (NMTeamLinkWatcher);
	watcher->refcount = 1;

	watcher->type = LINK_WATCHER_ETHTOOL;
	watcher->ethtool.delay_up = delay_up;
	watcher->ethtool.delay_down = delay_down;

	return watcher;
}

/**
 * nm_team_link_watcher_new_nsna_ping:
 * @init_wait: init_wait value
 * @interval: interval value
 * @missed_max: missed_max value
 * @target_host: the host name or the ipv6 address that will be used as
 *   target address in the NS packet
 * @error: (out) (allow-none): location to store the error on failure
 *
 * Creates a new nsna_ping #NMTeamLinkWatcher object
 *
 * Returns: (transfer full): the new #NMTeamLinkWatcher object, or %NULL on error
 *
 * Since: 1.12
 **/
NMTeamLinkWatcher *
nm_team_link_watcher_new_nsna_ping (int init_wait,
                                    int interval,
                                    int missed_max,
                                    const char *target_host,
                                    GError **error)
{
	NMTeamLinkWatcher *watcher;
	const char *val_fail = NULL;

	if (!target_host) {
		g_set_error (error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_FAILED,
		             _("Missing target-host in nsna_ping link watcher"));
		return NULL;
	}

	if (strpbrk (target_host, " \\/\t=\"\'")) {
		g_set_error (error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_FAILED,
		             _("target-host '%s' contains invalid characters"), target_host);
		return NULL;
	}

	if (init_wait < 0 || !_NM_INT_LE_MAXINT32 (init_wait))
		val_fail = "init-wait";
	if (interval < 0 || !_NM_INT_LE_MAXINT32 (interval))
		val_fail = "interval";
	if (missed_max < 0 || !_NM_INT_LE_MAXINT32 (missed_max))
		val_fail = "missed-max";
	if (val_fail) {
		g_set_error (error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_FAILED,
		             _("%s is out of range [0, %d]"), val_fail, G_MAXINT32);
		return NULL;
	}

	watcher = g_slice_new0 (NMTeamLinkWatcher);
	watcher->refcount = 1;

	watcher->type = LINK_WATCHER_NSNA_PING;
	watcher->nsna_ping.init_wait = init_wait;
	watcher->nsna_ping.interval = interval;
	watcher->nsna_ping.missed_max = missed_max;
	watcher->nsna_ping.target_host = g_strdup (target_host);

	return watcher;
}

/**
 * nm_team_link_watcher_new_arp_ping:
 * @init_wait: init_wait value
 * @interval: interval value
 * @missed_max: missed_max value
 * @target_host: the host name or the ip address that will be used as destination
 *   address in the arp request
 * @source_host: the host name or the ip address that will be used as source
 *   address in the arp request
 * @flags: the watcher #NMTeamLinkWatcherArpPingFlags
 * @error: (out) (allow-none): location to store the error on failure
 *
 * Creates a new arp_ping #NMTeamLinkWatcher object
 *
 * Returns: (transfer full): the new #NMTeamLinkWatcher object, or %NULL on error
 *
 * Since: 1.12
 **/
NMTeamLinkWatcher *
nm_team_link_watcher_new_arp_ping (int init_wait,
                                   int interval,
                                   int missed_max,
                                   const char *target_host,
                                   const char *source_host,
                                   NMTeamLinkWatcherArpPingFlags flags,
                                   GError **error)
{
	return nm_team_link_watcher_new_arp_ping2 (init_wait,
	                                           interval,
	                                           missed_max,
	                                           -1,
	                                           target_host,
	                                           source_host,
	                                           flags,
	                                           error);
}

/**
 * nm_team_link_watcher_new_arp_ping2:
 * @init_wait: init_wait value
 * @interval: interval value
 * @missed_max: missed_max value
 * @vlanid: vlanid value
 * @target_host: the host name or the ip address that will be used as destination
 *   address in the arp request
 * @source_host: the host name or the ip address that will be used as source
 *   address in the arp request
 * @flags: the watcher #NMTeamLinkWatcherArpPingFlags
 * @error: (out) (allow-none): location to store the error on failure
 *
 * Creates a new arp_ping #NMTeamLinkWatcher object
 *
 * Returns: (transfer full): the new #NMTeamLinkWatcher object, or %NULL on error
 *
 * Since: 1.16
 **/
NMTeamLinkWatcher *
nm_team_link_watcher_new_arp_ping2 (int init_wait,
                                    int interval,
                                    int missed_max,
                                    int vlanid,
                                    const char *target_host,
                                    const char *source_host,
                                    NMTeamLinkWatcherArpPingFlags flags,
                                    GError **error)
{
	NMTeamLinkWatcher *watcher;
	const char *val_fail = NULL;

	if (!target_host || !source_host) {
		g_set_error (error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_FAILED,
		             _("Missing %s in arp_ping link watcher"),
		             target_host ? "source-host" : "target-host");
		return NULL;
	}

	if (strpbrk (target_host, " \\/\t=\"\'")) {
		g_set_error (error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_FAILED,
		             _("target-host '%s' contains invalid characters"), target_host);
		return NULL;
	}

	if (strpbrk (source_host, " \\/\t=\"\'")) {
		g_set_error (error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_FAILED,
		             _("source-host '%s' contains invalid characters"), source_host);
		return NULL;
	}

	else if (init_wait < 0 || !_NM_INT_LE_MAXINT32 (init_wait))
		val_fail = "init-wait";
	else if (interval < 0 || !_NM_INT_LE_MAXINT32 (interval))
		val_fail = "interval";
	else if (missed_max < 0 || !_NM_INT_LE_MAXINT32 (missed_max))
		val_fail = "missed-max";
	if (val_fail) {
		g_set_error (error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_FAILED,
		             _("%s is out of range [0, %d]"), val_fail, G_MAXINT32);
		return NULL;
	}

	if (vlanid < -1 || vlanid > 4094) {
		g_set_error_literal (error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_FAILED,
		                     _("vlanid is out of range [-1, 4094]"));
		return NULL;
	}

	watcher = g_slice_new0 (NMTeamLinkWatcher);
	watcher->refcount = 1;

	watcher->type = LINK_WATCHER_ARP_PING;
	watcher->arp_ping.init_wait = init_wait;
	watcher->arp_ping.interval = interval;
	watcher->arp_ping.missed_max = missed_max;
	watcher->arp_ping.target_host = g_strdup (target_host);
	watcher->arp_ping.source_host = g_strdup (source_host);
	watcher->arp_ping.flags = flags;
	watcher->arp_ping.vlanid = vlanid;

	return watcher;
}

/**
 * nm_team_link_watcher_ref:
 * @watcher: the #NMTeamLinkWatcher
 *
 * Increases the reference count of the object.
 *
 * Since: 1.12
 **/
void
nm_team_link_watcher_ref (NMTeamLinkWatcher *watcher){
	_CHECK_WATCHER_VOID (watcher);

	watcher->refcount++;
}

/**
 * nm_team_link_watcher_unref:
 * @watcher: the #NMTeamLinkWatcher
 *
 * Decreases the reference count of the object.  If the reference count
 * reaches zero, the object will be destroyed.
 *
 * Since: 1.12
 **/
void
nm_team_link_watcher_unref (NMTeamLinkWatcher *watcher)
{
	_CHECK_WATCHER_VOID (watcher);

	watcher->refcount--;
	if (watcher->refcount == 0) {
		g_free (watcher->arp_ping.target_host);
		g_free (watcher->arp_ping.source_host);
		g_slice_free (NMTeamLinkWatcher, watcher);
	}
}

/**
 * nm_team_link_watcher_equal:
 * @watcher: the #NMTeamLinkWatcher
 * @other: the #NMTeamLinkWatcher to compare @watcher to.
 *
 * Determines if two #NMTeamLinkWatcher objects contain the same values
 * in all the properties.
 *
 * Returns: %TRUE if the objects contain the same values, %FALSE if they do not.
 *
 * Since: 1.12
 **/
gboolean
nm_team_link_watcher_equal (NMTeamLinkWatcher *watcher, NMTeamLinkWatcher *other)
{
	_CHECK_WATCHER (watcher, FALSE);
	_CHECK_WATCHER (other, FALSE);

	if (   watcher->type != other->type
	    || !nm_streq0 (watcher->arp_ping.target_host, other->arp_ping.target_host)
	    || !nm_streq0 (watcher->arp_ping.source_host, other->arp_ping.source_host)
	    || watcher->arp_ping.init_wait != other->arp_ping.init_wait
	    || watcher->arp_ping.interval != other->arp_ping.interval
	    || watcher->arp_ping.missed_max != other->arp_ping.missed_max
	    || watcher->arp_ping.vlanid != other->arp_ping.vlanid
	    || watcher->arp_ping.flags != other->arp_ping.flags)
		return FALSE;

	return TRUE;
}

gboolean
_nm_team_link_watchers_equal (GPtrArray *a, GPtrArray *b, gboolean ignore_order)
{
	guint i, j;

	if (a->len != b->len)
		return FALSE;
	if (ignore_order) {
		/* FIXME: comparing this way is O(n^2). Don't do that, instead
		 *        add nm_team_link_watcher_cmp(), sort both lists, and
		 *        compare step by step. */
		for (i = 0; i < a->len; i++) {
			for (j = 0; j < b->len; j++) {
				if (nm_team_link_watcher_equal (a->pdata[i], b->pdata[j]))
					break;
			}
			if (j == b->len)
				return FALSE;
		}
	} else {
		for (i = 0; i < a->len; i++) {
			if (!nm_team_link_watcher_equal (a->pdata[i], b->pdata[i]))
				return FALSE;
		}
	}
	return TRUE;
}

/**
 * nm_team_link_watcher_dup:
 * @watcher: the #NMTeamLinkWatcher
 *
 * Creates a copy of @watcher
 *
 * Returns: (transfer full): a copy of @watcher
 *
 * Since: 1.12
 **/
NMTeamLinkWatcher *
nm_team_link_watcher_dup (NMTeamLinkWatcher *watcher)
{
	_CHECK_WATCHER (watcher, NULL);

	switch (watcher->type) {
	case LINK_WATCHER_ETHTOOL:
		return nm_team_link_watcher_new_ethtool (watcher->ethtool.delay_up,
		                                         watcher->ethtool.delay_down,
		                                         NULL);
		break;
	case LINK_WATCHER_NSNA_PING:
		return nm_team_link_watcher_new_nsna_ping (watcher->nsna_ping.init_wait,
		                                           watcher->nsna_ping.interval,
		                                           watcher->nsna_ping.missed_max,
		                                           watcher->nsna_ping.target_host,
		                                           NULL);
		break;
	case LINK_WATCHER_ARP_PING:
		return nm_team_link_watcher_new_arp_ping2 (watcher->arp_ping.init_wait,
		                                           watcher->arp_ping.interval,
		                                           watcher->arp_ping.missed_max,
		                                           watcher->arp_ping.vlanid,
		                                           watcher->arp_ping.target_host,
		                                           watcher->arp_ping.source_host,
		                                           watcher->arp_ping.flags,
		                                          NULL);
	default:
		g_assert_not_reached ();
		return NULL;
	}
}

/**
 * nm_team_link_watcher_get_name:
 * @watcher: the #NMTeamLinkWatcher
 *
 * Gets the name of the link watcher to be used.
 *
 * Since: 1.12
 **/
const char *
nm_team_link_watcher_get_name (NMTeamLinkWatcher *watcher)
{
	_CHECK_WATCHER (watcher, NULL);

	return _link_watcher_name[watcher->type];
}

/**
 * nm_team_link_watcher_get_delay_up:
 * @watcher: the #NMTeamLinkWatcher
 *
 * Gets the delay_up interval (in milliseconds) that elapses between the link
 * coming up and the runner being notified about it.
 *
 * Since: 1.12
 **/
int
nm_team_link_watcher_get_delay_up (NMTeamLinkWatcher *watcher)
{
	_CHECK_WATCHER (watcher, 0);

	if (watcher->type != LINK_WATCHER_ETHTOOL)
		return -1;
	return watcher->ethtool.delay_up;
}

/**
 * nm_team_link_watcher_get_delay_down:
 * @watcher: the #NMTeamLinkWatcher
 *
 * Gets the delay_down interval (in milliseconds) that elapses between the link
 * going down and the runner being notified about it.
 *
 * Since: 1.12
 **/
int
nm_team_link_watcher_get_delay_down (NMTeamLinkWatcher *watcher)
{
	_CHECK_WATCHER (watcher, 0);

	if (watcher->type != LINK_WATCHER_ETHTOOL)
		return -1;
	return watcher->ethtool.delay_down;
}

/**
 * nm_team_link_watcher_get_init_wait:
 * @watcher: the #NMTeamLinkWatcher
 *
 * Gets the init_wait interval (in milliseconds) that the team slave should
 * wait before sending the first packet to the target host.
 *
 * Since: 1.12
 **/
int
nm_team_link_watcher_get_init_wait (NMTeamLinkWatcher *watcher)
{
	_CHECK_WATCHER (watcher, 0);

	if (!NM_IN_SET (watcher->type,
	                LINK_WATCHER_NSNA_PING,
	                LINK_WATCHER_ARP_PING))
		return -1;
	return watcher->arp_ping.init_wait;
}

/**
 * nm_team_link_watcher_get_interval:
 * @watcher: the #NMTeamLinkWatcher
 *
 * Gets the interval (in milliseconds) that the team slave should wait between
 * sending two check packets to the target host.
 *
 * Since: 1.12
 **/
int
nm_team_link_watcher_get_interval (NMTeamLinkWatcher *watcher)
{
	_CHECK_WATCHER (watcher, 0);

	if (!NM_IN_SET (watcher->type,
	                LINK_WATCHER_NSNA_PING,
	                LINK_WATCHER_ARP_PING))
		return -1;
	return watcher->arp_ping.interval;
}

/**
 * nm_team_link_watcher_get_missed_max:
 * @watcher: the #NMTeamLinkWatcher
 *
 * Gets the number of missed replies after which the link is considered down.
 *
 * Since: 1.12
 **/
int
nm_team_link_watcher_get_missed_max (NMTeamLinkWatcher *watcher)
{
	_CHECK_WATCHER (watcher, 0);

	if (!NM_IN_SET (watcher->type,
	                LINK_WATCHER_NSNA_PING,
	                LINK_WATCHER_ARP_PING))
		return -1;
	return watcher->arp_ping.missed_max;
}

/**
 * nm_team_link_watcher_get_vlanid:
 * @watcher: the #NMTeamLinkWatcher
 *
 * Gets the VLAN tag ID to be used to outgoing link probes
 *
 * Since: 1.16
 **/
int
nm_team_link_watcher_get_vlanid (NMTeamLinkWatcher *watcher)
{
	_CHECK_WATCHER (watcher, -1);

	if (watcher->type != LINK_WATCHER_ARP_PING)
		return -1;
	return watcher->arp_ping.vlanid;
}

/**
 * nm_team_link_watcher_get_target_host:
 * @watcher: the #NMTeamLinkWatcher
 *
 * Gets the host name/ip address to be used as destination for the link probing
 * packets.
 *
 * Since: 1.12
 **/
const char *
nm_team_link_watcher_get_target_host (NMTeamLinkWatcher *watcher)
{
	_CHECK_WATCHER (watcher, NULL);

	return watcher->arp_ping.target_host;
}

/**
 * nm_team_link_watcher_get_source_host:
 * @watcher: the #NMTeamLinkWatcher
 *
 * Gets the ip address to be used as source for the link probing packets.
 *
 * Since: 1.12
 **/
const char *
nm_team_link_watcher_get_source_host (NMTeamLinkWatcher *watcher)
{
	_CHECK_WATCHER (watcher, NULL);

	return watcher->arp_ping.source_host;
}

/**
 * nm_team_link_watcher_get_flags:
 * @watcher: the #NMTeamLinkWatcher
 *
 * Gets the arp ping watcher flags.
 *
 * Since: 1.12
 **/
NMTeamLinkWatcherArpPingFlags
nm_team_link_watcher_get_flags (NMTeamLinkWatcher *watcher)
{
	_CHECK_WATCHER (watcher, 0);

	return watcher->arp_ping.flags;
}

/*****************************************************************************/

NM_GOBJECT_PROPERTIES_DEFINE (NMSettingTeam,
	PROP_CONFIG,
	PROP_NOTIFY_PEERS_COUNT,
	PROP_NOTIFY_PEERS_INTERVAL,
	PROP_MCAST_REJOIN_COUNT,
	PROP_MCAST_REJOIN_INTERVAL,
	PROP_RUNNER,
	PROP_RUNNER_HWADDR_POLICY,
	PROP_RUNNER_TX_HASH,
	PROP_RUNNER_TX_BALANCER,
	PROP_RUNNER_TX_BALANCER_INTERVAL,
	PROP_RUNNER_ACTIVE,
	PROP_RUNNER_FAST_RATE,
	PROP_RUNNER_SYS_PRIO,
	PROP_RUNNER_MIN_PORTS,
	PROP_RUNNER_AGG_SELECT_POLICY,
	PROP_LINK_WATCHERS,
);

static const _NMUtilsTeamPropertyKeys _prop_to_keys[_PROPERTY_ENUMS_LAST] = {
	[PROP_CONFIG] =                      { },
	[PROP_NOTIFY_PEERS_COUNT] =          { .key1 = "notify_peers", .key2 = "count",                                           },
	[PROP_NOTIFY_PEERS_INTERVAL] =       { .key1 = "notify_peers", .key2 = "interval",                                        },
	[PROP_MCAST_REJOIN_COUNT] =          { .key1 = "mcast_rejoin", .key2 = "count",                                           },
	[PROP_MCAST_REJOIN_INTERVAL] =       { .key1 = "mcast_rejoin", .key2 = "interval",                                        },
	[PROP_RUNNER] =                      { .key1 = "runner",       .key2 = "name",                                            .default_str = NM_SETTING_TEAM_RUNNER_DEFAULT, },
	[PROP_RUNNER_HWADDR_POLICY] =        { .key1 = "runner",       .key2 = "hwaddr_policy",                                   },
	[PROP_RUNNER_TX_HASH] =              { .key1 = "runner",       .key2 = "tx_hash",                                         },
	[PROP_RUNNER_TX_BALANCER] =          { .key1 = "runner",       .key2 = "tx_balancer",       .key3 = "name", },
	[PROP_RUNNER_TX_BALANCER_INTERVAL] = { .key1 = "runner",       .key2 = "tx_balancer",       .key3 = "balancing_interval", .default_int = -1 },
	[PROP_RUNNER_ACTIVE] =               { .key1 = "runner",       .key2 = "active",                                          },
	[PROP_RUNNER_FAST_RATE] =            { .key1 = "runner",       .key2 = "fast_rate",                                       },
	[PROP_RUNNER_SYS_PRIO] =             { .key1 = "runner",       .key2 = "sys_prio",                                        .default_int = -1, },
	[PROP_RUNNER_MIN_PORTS] =            { .key1 = "runner",       .key2 = "min_ports",                                       .default_int = -1, },
	[PROP_RUNNER_AGG_SELECT_POLICY] =    { .key1 = "runner",       .key2 = "agg_select_policy",                               },
	[PROP_LINK_WATCHERS] =               { .key1 = "link_watch",                                                              },
};

typedef struct {
	char *config;
	char *runner;
	char *runner_hwaddr_policy;
	char *runner_tx_balancer;
	char *runner_agg_select_policy;
	GPtrArray *runner_tx_hash;
	GPtrArray *link_watchers;
	int notify_peers_count;
	int notify_peers_interval;
	int mcast_rejoin_count;
	int mcast_rejoin_interval;
	int runner_tx_balancer_interval;
	int runner_sys_prio;
	int runner_min_ports;
	bool runner_active;
	bool runner_fast_rate;
} NMSettingTeamPrivate;

G_DEFINE_TYPE (NMSettingTeam, nm_setting_team, NM_TYPE_SETTING)

#define NM_SETTING_TEAM_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_SETTING_TEAM, NMSettingTeamPrivate))

/*****************************************************************************/

/**
 * nm_setting_team_get_config:
 * @setting: the #NMSettingTeam
 *
 * Returns: the #NMSettingTeam:config property of the setting
 **/
const char *
nm_setting_team_get_config (NMSettingTeam *setting)
{
	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), NULL);

	return NM_SETTING_TEAM_GET_PRIVATE (setting)->config;
}

/**
 * nm_setting_team_get_notify_peers_count:
 * @setting: the #NMSettingTeam
 *
 * Returns: the ##NMSettingTeam:notify-peers-count property of the setting
 *
 * Since: 1.12
 **/
int
nm_setting_team_get_notify_peers_count (NMSettingTeam *setting)
{
	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), 0);

	return NM_SETTING_TEAM_GET_PRIVATE (setting)->notify_peers_count;
}

/**
 * nm_setting_team_get_notify_peers_interval:
 * @setting: the #NMSettingTeam
 *
 * Returns: the ##NMSettingTeam:notify-peers-interval property of the setting
 *
 * Since: 1.12
 **/
int
nm_setting_team_get_notify_peers_interval (NMSettingTeam *setting)
{
	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), 0);

	return NM_SETTING_TEAM_GET_PRIVATE (setting)->notify_peers_interval;
}

/**
 * nm_setting_team_get_mcast_rejoin_count:
 * @setting: the #NMSettingTeam
 *
 * Returns: the ##NMSettingTeam:mcast-rejoin-count property of the setting
 *
 * Since: 1.12
 **/
int
nm_setting_team_get_mcast_rejoin_count (NMSettingTeam *setting)
{
	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), 0);

	return NM_SETTING_TEAM_GET_PRIVATE (setting)->mcast_rejoin_count;
}

/**
 * nm_setting_team_get_mcast_rejoin_interval:
 * @setting: the #NMSettingTeam
 *
 * Returns: the ##NMSettingTeam:mcast-rejoin-interval property of the setting
 *
 * Since: 1.12
 **/
int
nm_setting_team_get_mcast_rejoin_interval (NMSettingTeam *setting)
{
	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), 0);

	return NM_SETTING_TEAM_GET_PRIVATE (setting)->mcast_rejoin_interval;
}

/**
 * nm_setting_team_get_runner:
 * @setting: the #NMSettingTeam
 *
 * Returns: the ##NMSettingTeam:runner property of the setting
 *
 * Since: 1.12
 **/
const char *
nm_setting_team_get_runner (NMSettingTeam *setting)
{
	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), NULL);

	return NM_SETTING_TEAM_GET_PRIVATE (setting)->runner;
}

/**
 * nm_setting_team_get_runner_hwaddr_policy:
 * @setting: the #NMSettingTeam
 *
 * Returns: the ##NMSettingTeam:runner-hwaddr-policy property of the setting
 *
 * Since: 1.12
 **/
const char *
nm_setting_team_get_runner_hwaddr_policy (NMSettingTeam *setting)
{
	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), NULL);

	return NM_SETTING_TEAM_GET_PRIVATE (setting)->runner_hwaddr_policy;
}

/**
 * nm_setting_team_get_runner_tx_balancer:
 * @setting: the #NMSettingTeam
 *
 * Returns: the ##NMSettingTeam:runner-tx-balancer property of the setting
 *
 * Since: 1.12
 **/
const char *
nm_setting_team_get_runner_tx_balancer (NMSettingTeam *setting)
{
	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), NULL);

	return NM_SETTING_TEAM_GET_PRIVATE (setting)->runner_tx_balancer;
}

/**
 * nm_setting_team_get_runner_tx_balancer_interval:
 * @setting: the #NMSettingTeam
 *
 * Returns: the ##NMSettingTeam:runner-tx-balancer_interval property of the setting
 *
 * Since: 1.12
 **/
int
nm_setting_team_get_runner_tx_balancer_interval (NMSettingTeam *setting)
{
	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), 0);

	return NM_SETTING_TEAM_GET_PRIVATE (setting)->runner_tx_balancer_interval;
}

/**
 * nm_setting_team_get_runner_active:
 * @setting: the #NMSettingTeam
 *
 * Returns: the ##NMSettingTeam:runner_active property of the setting
 *
 * Since: 1.12
 **/
gboolean
nm_setting_team_get_runner_active (NMSettingTeam *setting)
{
	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), FALSE);

	return NM_SETTING_TEAM_GET_PRIVATE (setting)->runner_active;
}

/**
 * nm_setting_team_get_runner_fast_rate:
 * @setting: the #NMSettingTeam
 *
 * Returns: the ##NMSettingTeam:runner-fast-rate property of the setting
 *
 * Since: 1.12
 **/
gboolean
nm_setting_team_get_runner_fast_rate (NMSettingTeam *setting)
{
	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), FALSE);

	return NM_SETTING_TEAM_GET_PRIVATE (setting)->runner_fast_rate;
}

/**
 * nm_setting_team_get_runner_sys_prio:
 * @setting: the #NMSettingTeam
 *
 * Returns: the ##NMSettingTeam:runner-sys-prio property of the setting
 *
 * Since: 1.12
 **/
int
nm_setting_team_get_runner_sys_prio (NMSettingTeam *setting)
{
	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), 0);

	return NM_SETTING_TEAM_GET_PRIVATE (setting)->runner_sys_prio;
}

/**
 * nm_setting_team_get_runner_min_ports:
 * @setting: the #NMSettingTeam
 *
 * Returns: the ##NMSettingTeam:runner-min-ports property of the setting
 *
 * Since: 1.12
 **/
int
nm_setting_team_get_runner_min_ports (NMSettingTeam *setting)
{
	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), 0);

	return NM_SETTING_TEAM_GET_PRIVATE (setting)->runner_min_ports;
}

/**
 * nm_setting_team_get_runner_agg_select_policy:
 * @setting: the #NMSettingTeam
 *
 * Returns: the ##NMSettingTeam:runner-agg-select-policy property of the setting
 *
 * Since: 1.12
 **/
const char *
nm_setting_team_get_runner_agg_select_policy (NMSettingTeam *setting)
{
	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), NULL);

	return NM_SETTING_TEAM_GET_PRIVATE (setting)->runner_agg_select_policy;
}

/**
 * nm_setting_team_remove_runner_tx_hash_by_value:
 * @setting: the #NMSetetingTeam
 * @txhash: the txhash element to remove
 *
 * Removes the txhash element #txhash
 *
 * Returns: %TRUE if the txhash element was found and removed; %FALSE if it was not.
 *
 * Since: 1.12
 **/
gboolean
nm_setting_team_remove_runner_tx_hash_by_value (NMSettingTeam *setting,
                                               const char *txhash)
{
	NMSettingTeamPrivate *priv = NM_SETTING_TEAM_GET_PRIVATE (setting);
	guint i;

	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), FALSE);
	g_return_val_if_fail (txhash != NULL, FALSE);

	if (priv->runner_tx_hash) {
		for (i = 0; i < priv->runner_tx_hash->len; i++) {
			if (nm_streq (txhash, priv->runner_tx_hash->pdata[i])) {
				g_ptr_array_remove_index (priv->runner_tx_hash, i);
				_notify (setting, PROP_RUNNER_TX_HASH);
				return TRUE;
			}
		}
	}
	return FALSE;
}

/**
 * nm_setting_team_get_num_runner_tx_hash:
 * @setting: the #NMSettingTeam
 *
 * Returns: the number of elements in txhash
 *
 * Since: 1.12
 **/
guint
nm_setting_team_get_num_runner_tx_hash (NMSettingTeam *setting)
{
	NMSettingTeamPrivate *priv = NM_SETTING_TEAM_GET_PRIVATE (setting);

	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), 0);

	return priv->runner_tx_hash ? priv->runner_tx_hash->len : 0;
}

/**
 * nm_setting_team_get_runner_tx_hash
 * @setting: the #NMSettingTeam
 * @idx: index number of the txhash element to return
 *
 * Returns: the txhash element at index @idx
 *
 * Since: 1.12
 **/
const char *
nm_setting_team_get_runner_tx_hash (NMSettingTeam *setting, guint idx)
{
	NMSettingTeamPrivate *priv = NM_SETTING_TEAM_GET_PRIVATE (setting);

	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), NULL);
	g_return_val_if_fail (idx < priv->runner_tx_hash->len, NULL);

	return priv->runner_tx_hash->pdata[idx];
}

/**
 * nm_setting_team_remove_runner_tx_hash:
 * @setting: the #NMSettingTeam
 * @idx: index number of the element to remove from txhash
 *
 * Removes the txhash element at index @idx.
 *
 * Since: 1.12
 **/
void
nm_setting_team_remove_runner_tx_hash (NMSettingTeam *setting, guint idx)
{
	NMSettingTeamPrivate *priv = NM_SETTING_TEAM_GET_PRIVATE (setting);

	g_return_if_fail (NM_IS_SETTING_TEAM (setting));
	g_return_if_fail (idx < priv->runner_tx_hash->len);

	g_ptr_array_remove_index (priv->runner_tx_hash, idx);
	_notify (setting, PROP_RUNNER_TX_HASH);
}

/**
 * nm_setting_team_add_runner_tx_hash:
 * @setting: the #NMSettingTeam
 * @txhash: the element to add to txhash
 *
 * Adds a new txhash element to the setting.
 *
 * Returns: %TRUE if the txhash element was added; %FALSE if the element
 * was already knnown.
 *
 * Since: 1.12
 **/
gboolean
nm_setting_team_add_runner_tx_hash (NMSettingTeam *setting, const char *txhash)
{
	NMSettingTeamPrivate *priv = NM_SETTING_TEAM_GET_PRIVATE (setting);
	guint i;

	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), FALSE);
	g_return_val_if_fail (txhash != NULL, FALSE);
	g_return_val_if_fail (txhash[0] != '\0', FALSE);

	if (!priv->runner_tx_hash)
		priv->runner_tx_hash = g_ptr_array_new_with_free_func (g_free);
	for (i = 0; i < priv->runner_tx_hash->len; i++) {
		if (nm_streq (txhash, priv->runner_tx_hash->pdata[i]))
			return FALSE;
	}

	g_ptr_array_add (priv->runner_tx_hash, g_strdup (txhash));
	_notify (setting, PROP_RUNNER_TX_HASH);
	return TRUE;
}

/**
 * nm_setting_team_get_num_link_watchers:
 * @setting: the #NMSettingTeam
 *
 * Returns: the number of configured link watchers
 *
 * Since: 1.12
 **/
guint
nm_setting_team_get_num_link_watchers (NMSettingTeam *setting)
{
	NMSettingTeamPrivate *priv = NM_SETTING_TEAM_GET_PRIVATE (setting);

	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), 0);

	return priv->link_watchers->len;
}

/**
 * nm_setting_team_get_link_watcher:
 * @setting: the #NMSettingTeam
 * @idx: index number of the link watcher to return
 *
 * Returns: (transfer none): the link watcher at index @idx.
 *
 * Since: 1.12
 **/
NMTeamLinkWatcher *
nm_setting_team_get_link_watcher (NMSettingTeam *setting, guint idx)
{
	NMSettingTeamPrivate *priv = NM_SETTING_TEAM_GET_PRIVATE (setting);

	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), NULL);
	g_return_val_if_fail (idx < priv->link_watchers->len, NULL);

	return priv->link_watchers->pdata[idx];
}

/**
 * nm_setting_team_add_link_watcher:
 * @setting: the #NMSettingTeam
 * @link_watcher: the link watcher to add
 *
 * Appends a new link watcher to the setting.
 *
 * Returns: %TRUE if the link watcher is added; %FALSE if an identical link
 * watcher was already there.
 *
 * Since: 1.12
 **/
gboolean
nm_setting_team_add_link_watcher (NMSettingTeam *setting,
                                  NMTeamLinkWatcher *link_watcher)
{
	NMSettingTeamPrivate *priv = NM_SETTING_TEAM_GET_PRIVATE (setting);
	guint i;

	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), FALSE);
	g_return_val_if_fail (link_watcher != NULL, FALSE);

	for (i = 0; i < priv->link_watchers->len; i++) {
		if (nm_team_link_watcher_equal (priv->link_watchers->pdata[i], link_watcher))
			return FALSE;
	}

	g_ptr_array_add (priv->link_watchers, nm_team_link_watcher_dup (link_watcher));
	_notify (setting, PROP_LINK_WATCHERS);
	return TRUE;
}

/**
 * nm_setting_team_remove_link_watcher:
 * @setting: the #NMSettingTeam
 * @idx: index number of the link watcher to remove
 *
 * Removes the link watcher at index #idx.
 *
 * Since: 1.12
 **/
void
nm_setting_team_remove_link_watcher (NMSettingTeam *setting, guint idx)
{
	NMSettingTeamPrivate *priv = NM_SETTING_TEAM_GET_PRIVATE (setting);

	g_return_if_fail (NM_IS_SETTING_TEAM (setting));
	g_return_if_fail (idx < priv->link_watchers->len);

	g_ptr_array_remove_index (priv->link_watchers, idx);
	_notify (setting, PROP_LINK_WATCHERS);
}

/**
 * nm_setting_team_remove_link_watcher_by_value:
 * @setting: the #NMSettingTeam
 * @link_watcher: the link watcher to remove
 *
 * Removes the link watcher entry matching link_watcher.
 *
 * Returns: %TRUE if the link watcher was found and removed, %FALSE otherwise.
 *
 * Since: 1.12
 **/
gboolean
nm_setting_team_remove_link_watcher_by_value (NMSettingTeam *setting,
                                              NMTeamLinkWatcher *link_watcher)
{
	NMSettingTeamPrivate *priv = NM_SETTING_TEAM_GET_PRIVATE (setting);
	guint i;

	g_return_val_if_fail (NM_IS_SETTING_TEAM (setting), FALSE);

	for (i = 0; i < priv->link_watchers->len; i++) {
		if (nm_team_link_watcher_equal (priv->link_watchers->pdata[i], link_watcher)) {
			g_ptr_array_remove_index (priv->link_watchers, i);
			_notify (setting, PROP_LINK_WATCHERS);
			return TRUE;
		}
	}
	return FALSE;
}

/**
 * nm_setting_team_clear_link_watchers:
 * @setting: the #NMSettingTeam
 *
 * Removes all configured link watchers.
 *
 * Since: 1.12
 **/
void
nm_setting_team_clear_link_watchers (NMSettingTeam *setting) {
	NMSettingTeamPrivate *priv = NM_SETTING_TEAM_GET_PRIVATE (setting);

	g_return_if_fail (NM_IS_SETTING_TEAM (setting));

	if (priv->link_watchers->len != 0) {
		g_ptr_array_set_size (priv->link_watchers, 0);
		_notify (setting, PROP_LINK_WATCHERS);
	}
}

static GVariant *
team_link_watchers_to_dbus (const GValue *prop_value)
{
	return _nm_utils_team_link_watchers_to_variant (g_value_get_boxed (prop_value));
}

static void
team_link_watchers_from_dbus (GVariant   *dbus_value,
                              GValue     *prop_value)
{
	g_value_take_boxed (prop_value, _nm_utils_team_link_watchers_from_variant (dbus_value));
}

static gboolean
verify (NMSetting *setting, NMConnection *connection, GError **error)
{
	NMSettingTeamPrivate *priv = NM_SETTING_TEAM_GET_PRIVATE (setting);
	guint i;

	if (!_nm_connection_verify_required_interface_name (connection, error))
		return FALSE;

	if (priv->config) {
		if (strlen (priv->config) > 1*1024*1024) {
			g_set_error (error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_INVALID_PROPERTY,
			             _("team config exceeds size limit"));
			g_prefix_error (error,
			                "%s.%s: ",
			                NM_SETTING_TEAM_SETTING_NAME,
			                NM_SETTING_TEAM_CONFIG);
			return FALSE;
		}

		if (!nm_utils_is_json_object (priv->config, error)) {
			g_prefix_error (error,
			                "%s.%s: ",
			                NM_SETTING_TEAM_SETTING_NAME,
			                NM_SETTING_TEAM_CONFIG);
			/* We treat an empty string as no config for compatibility. */
			return *priv->config ? FALSE : NM_SETTING_VERIFY_NORMALIZABLE;
		}
	}

	if (   priv->runner
	    && g_ascii_strcasecmp (priv->runner, NM_SETTING_TEAM_RUNNER_BROADCAST)
	    && g_ascii_strcasecmp (priv->runner, NM_SETTING_TEAM_RUNNER_ROUNDROBIN)
	    && g_ascii_strcasecmp (priv->runner, NM_SETTING_TEAM_RUNNER_RANDOM)
	    && g_ascii_strcasecmp (priv->runner, NM_SETTING_TEAM_RUNNER_ACTIVEBACKUP)
	    && g_ascii_strcasecmp (priv->runner, NM_SETTING_TEAM_RUNNER_LOADBALANCE)
	    && g_ascii_strcasecmp (priv->runner, NM_SETTING_TEAM_RUNNER_LACP)) {
		g_set_error (error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_INVALID_SETTING,
		                     _("invalid runner \"%s\""), priv->runner);

		g_prefix_error (error, "%s.%s: ", nm_setting_get_name (setting), NM_SETTING_TEAM_RUNNER);
		return FALSE;
	}

	/* Validate link watchers */
	for (i = 0; i < priv->link_watchers->len; i++) {
		NMTeamLinkWatcher *link_watcher = priv->link_watchers->pdata[i];
		const char *name = nm_team_link_watcher_get_name (link_watcher);

		if (!name) {
			g_set_error (error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_MISSING_SETTING,
			             _("missing link watcher name"));
			g_prefix_error (error, "%s.%s: ", nm_setting_get_name (setting),
			                NM_SETTING_TEAM_LINK_WATCHERS);
			return FALSE;
		}
		if (!NM_IN_STRSET (name,
		                   NM_TEAM_LINK_WATCHER_ETHTOOL,
		                   NM_TEAM_LINK_WATCHER_ARP_PING,
		                   NM_TEAM_LINK_WATCHER_NSNA_PING)) {
			g_set_error (error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_INVALID_SETTING,
			             _("unknown link watcher \"%s\""), name);
			g_prefix_error (error, "%s.%s: ", nm_setting_get_name (setting),
			                NM_SETTING_TEAM_LINK_WATCHERS);
			return FALSE;
		}

		if (NM_IN_STRSET (name,
		                  NM_TEAM_LINK_WATCHER_ARP_PING,
		                  NM_TEAM_LINK_WATCHER_NSNA_PING)
		    && !nm_team_link_watcher_get_target_host (link_watcher)) {
			g_set_error (error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_MISSING_SETTING,
			             _("missing target host"));
			g_prefix_error (error, "%s.%s: ", nm_setting_get_name (setting),
			                NM_SETTING_TEAM_LINK_WATCHERS);
			return FALSE;
		}
		if (nm_streq (name, NM_TEAM_LINK_WATCHER_ARP_PING)
		    && !nm_team_link_watcher_get_source_host (link_watcher)) {
			g_set_error (error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_MISSING_SETTING,
			             _("missing source address"));
			g_prefix_error (error, "%s.%s: ", nm_setting_get_name (setting),
			                NM_SETTING_TEAM_LINK_WATCHERS);
			return FALSE;
		}
	}
	/* NOTE: normalizable/normalizable-errors must appear at the end with decreasing severity.
	 * Take care to properly order statements with priv->config above. */

	return TRUE;
}

static NMTernary
compare_property (const NMSettInfoSetting *sett_info,
                  guint property_idx,
                  NMSetting *setting,
                  NMSetting *other,
                  NMSettingCompareFlags flags)
{
	NMSettingTeamPrivate *a_priv, *b_priv;

	if (nm_streq (sett_info->property_infos[property_idx].name, NM_SETTING_TEAM_LINK_WATCHERS)) {
		if (NM_FLAGS_HAS (flags, NM_SETTING_COMPARE_FLAG_INFERRABLE))
			return NM_TERNARY_DEFAULT;
		if (!other)
			return TRUE;
		a_priv = NM_SETTING_TEAM_GET_PRIVATE (setting);
		b_priv = NM_SETTING_TEAM_GET_PRIVATE (other);
		return _nm_team_link_watchers_equal (a_priv->link_watchers,
		                                     b_priv->link_watchers,
		                                     TRUE);
	}

	if (nm_streq (sett_info->property_infos[property_idx].name, NM_SETTING_TEAM_CONFIG)) {
		if (other) {
			a_priv = NM_SETTING_TEAM_GET_PRIVATE (setting);
			b_priv = NM_SETTING_TEAM_GET_PRIVATE (other);

			if (NM_FLAGS_HAS (flags, NM_SETTING_COMPARE_FLAG_INFERRABLE)) {
				/* If we are trying to match a connection in order to assume it (and thus
				 * @flags contains INFERRABLE), use the "relaxed" matching for team
				 * configuration. Otherwise, for all other purposes (including connection
				 * comparison before an update), resort to the default string comparison. */
				return _nm_utils_team_config_equal (a_priv->config,
				                                    b_priv->config,
				                                    TRUE);
			}

			return nm_streq0 (a_priv->config, b_priv->config);
		}

		return TRUE;
	}

	return NM_SETTING_CLASS (nm_setting_team_parent_class)->compare_property (sett_info,
	                                                                          property_idx,
	                                                                          setting,
	                                                                          other,
	                                                                          flags);
}

static void
_align_team_properties (NMSettingTeam *self)
{
	NMSettingTeamPrivate *priv = NM_SETTING_TEAM_GET_PRIVATE (self);
	GPtrArray *ptrarr;
	char **strv;
	gsize i;

	g_object_freeze_notify (G_OBJECT (self));

	_NM_TEAM_ALIGN_PROP (_nm_team_align_prop_int,    self, priv->config, &priv->notify_peers_count,          PROP_NOTIFY_PEERS_COUNT);
	_NM_TEAM_ALIGN_PROP (_nm_team_align_prop_int,    self, priv->config, &priv->notify_peers_interval,       PROP_NOTIFY_PEERS_INTERVAL);
	_NM_TEAM_ALIGN_PROP (_nm_team_align_prop_int,    self, priv->config, &priv->mcast_rejoin_count,          PROP_MCAST_REJOIN_COUNT);
	_NM_TEAM_ALIGN_PROP (_nm_team_align_prop_int,    self, priv->config, &priv->mcast_rejoin_interval,       PROP_MCAST_REJOIN_INTERVAL);
	_NM_TEAM_ALIGN_PROP (_nm_team_align_prop_int,    self, priv->config, &priv->runner_tx_balancer_interval, PROP_RUNNER_TX_BALANCER_INTERVAL);
	_NM_TEAM_ALIGN_PROP (_nm_team_align_prop_int,    self, priv->config, &priv->runner_sys_prio,             PROP_RUNNER_SYS_PRIO);
	_NM_TEAM_ALIGN_PROP (_nm_team_align_prop_int,    self, priv->config, &priv->runner_min_ports,            PROP_RUNNER_MIN_PORTS);

	_NM_TEAM_ALIGN_PROP (_nm_team_align_prop_bool,   self, priv->config, &priv->runner_active,               PROP_RUNNER_ACTIVE);
	_NM_TEAM_ALIGN_PROP (_nm_team_align_prop_bool,   self, priv->config, &priv->runner_fast_rate,            PROP_RUNNER_FAST_RATE);

	_NM_TEAM_ALIGN_PROP (_nm_team_align_prop_string, self, priv->config, &priv->runner,                      PROP_RUNNER);
	_NM_TEAM_ALIGN_PROP (_nm_team_align_prop_string, self, priv->config, &priv->runner_hwaddr_policy,        PROP_RUNNER_HWADDR_POLICY);
	_NM_TEAM_ALIGN_PROP (_nm_team_align_prop_string, self, priv->config, &priv->runner_tx_balancer,          PROP_RUNNER_TX_BALANCER);
	_NM_TEAM_ALIGN_PROP (_nm_team_align_prop_string, self, priv->config, &priv->runner_agg_select_policy,    PROP_RUNNER_AGG_SELECT_POLICY);

	strv = _nm_utils_json_extract_strv (priv->config, _prop_to_keys[PROP_RUNNER_TX_HASH], FALSE);
	if (_nm_utils_strv_cmp_n ((  priv->runner_tx_hash
	                           ? (const char *const*) priv->runner_tx_hash->pdata
	                           : NULL),
	                          (  priv->runner_tx_hash
	                           ? (gssize) priv->runner_tx_hash->len
	                           : (gssize) -1),
	                          NM_CAST_STRV_CC (strv),
	                          -1) != 0) {
		nm_clear_pointer (&priv->runner_tx_hash, g_ptr_array_unref);
		if (strv) {
			priv->runner_tx_hash = g_ptr_array_new_full (NM_PTRARRAY_LEN (strv), g_free);
			for (i = 0; strv[i]; i++)
				g_ptr_array_add (priv->runner_tx_hash, strv[i]);
			nm_clear_g_free (&strv);
		}
		_notify (self, PROP_RUNNER_TX_HASH);
	}
	nm_clear_pointer (&strv, g_strfreev);

	ptrarr = _nm_utils_json_extract_ptr_array (priv->config, _prop_to_keys[PROP_LINK_WATCHERS], FALSE);
	if (!_nm_team_link_watchers_equal (priv->link_watchers,
	                                   ptrarr,
	                                   FALSE)) {
		g_ptr_array_unref (priv->link_watchers);
		priv->link_watchers = g_steal_pointer (&ptrarr);
		_notify (self, PROP_LINK_WATCHERS);
	} else
		nm_clear_pointer (&ptrarr, g_ptr_array_unref);

	g_object_thaw_notify (G_OBJECT (self));
}

/*****************************************************************************/

static void
get_property (GObject *object, guint prop_id,
              GValue *value, GParamSpec *pspec)
{
	NMSettingTeam *setting = NM_SETTING_TEAM (object);
	NMSettingTeamPrivate *priv = NM_SETTING_TEAM_GET_PRIVATE (setting);

	switch (prop_id) {
	case PROP_CONFIG:
		g_value_set_string (value, nm_setting_team_get_config (setting));
		break;
	case PROP_NOTIFY_PEERS_COUNT:
		g_value_set_int (value, priv->notify_peers_count);
		break;
	case PROP_NOTIFY_PEERS_INTERVAL:
		g_value_set_int (value, priv->notify_peers_interval);
		break;
	case PROP_MCAST_REJOIN_COUNT:
		g_value_set_int (value, priv->mcast_rejoin_count);
		break;
	case PROP_MCAST_REJOIN_INTERVAL:
		g_value_set_int (value, priv->mcast_rejoin_interval);
		break;
	case PROP_RUNNER:
		g_value_set_string (value, nm_setting_team_get_runner (setting));
		break;
	case PROP_RUNNER_HWADDR_POLICY:
		g_value_set_string (value, nm_setting_team_get_runner_hwaddr_policy (setting));
		break;
	case PROP_RUNNER_TX_HASH:
		g_value_take_boxed (value,
		                      priv->runner_tx_hash
		                    ? _nm_utils_ptrarray_to_strv (priv->runner_tx_hash)
		                    : NULL);
		break;
	case PROP_RUNNER_TX_BALANCER:
		g_value_set_string (value, nm_setting_team_get_runner_tx_balancer (setting));
		break;
	case PROP_RUNNER_TX_BALANCER_INTERVAL:
		g_value_set_int (value, priv->runner_tx_balancer_interval);
		break;
	case PROP_RUNNER_ACTIVE:
		g_value_set_boolean (value, nm_setting_team_get_runner_active (setting));
		break;
	case PROP_RUNNER_FAST_RATE:
		g_value_set_boolean (value, nm_setting_team_get_runner_fast_rate (setting));
		break;
	case PROP_RUNNER_SYS_PRIO:
		g_value_set_int (value, priv->runner_sys_prio);
		break;
	case PROP_RUNNER_MIN_PORTS:
		g_value_set_int (value, priv->runner_min_ports);
		break;
	case PROP_RUNNER_AGG_SELECT_POLICY:
		g_value_set_string (value, nm_setting_team_get_runner_agg_select_policy (setting));
		break;
	case PROP_LINK_WATCHERS:
		g_value_take_boxed (value, _nm_utils_copy_array (priv->link_watchers,
		                                                 (NMUtilsCopyFunc) nm_team_link_watcher_dup,
		                                                 (GDestroyNotify) nm_team_link_watcher_unref));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
set_property (GObject *object, guint prop_id,
              const GValue *value, GParamSpec *pspec)
{
	NMSettingTeam *setting = NM_SETTING_TEAM (object);
	NMSettingTeamPrivate *priv = NM_SETTING_TEAM_GET_PRIVATE (object);
	const GValue *align_value = NULL;
	char **strv;
	const char *cstr;

	switch (prop_id) {
	case PROP_CONFIG:
		cstr = g_value_get_string (value);
		if (nm_streq0 (cstr, priv->config))
			return;
		g_free (priv->config);
		priv->config = g_strdup (cstr);
		break;
	case PROP_NOTIFY_PEERS_COUNT:
		if (priv->notify_peers_count == g_value_get_int (value))
			return;
		priv->notify_peers_count = g_value_get_int (value);
		align_value = value;
		break;
	case PROP_NOTIFY_PEERS_INTERVAL:
		if (priv->notify_peers_interval == g_value_get_int (value))
			return;
		priv->notify_peers_interval = g_value_get_int (value);
		align_value = value;
		break;
	case PROP_MCAST_REJOIN_COUNT:
		if (priv->mcast_rejoin_count == g_value_get_int (value))
			return;
		priv->mcast_rejoin_count = g_value_get_int (value);
		align_value = value;
		break;
	case PROP_MCAST_REJOIN_INTERVAL:
		if (priv->mcast_rejoin_interval == g_value_get_int (value))
			return;
		priv->mcast_rejoin_interval = g_value_get_int (value);
		align_value = value;
		break;
	case PROP_RUNNER:
		if (nm_streq0 (priv->runner, g_value_get_string (value)))
			return;
		g_free (priv->runner);
		priv->runner = g_value_dup_string (value);
		align_value = value;
		break;
	case PROP_RUNNER_HWADDR_POLICY:
		if (nm_streq0 (priv->runner_hwaddr_policy, g_value_get_string (value)))
			return;
		g_free (priv->runner_hwaddr_policy);
		priv->runner_hwaddr_policy = g_value_dup_string (value);
		align_value = value;
		break;
	case PROP_RUNNER_TX_HASH:
		if (priv->runner_tx_hash)
			g_ptr_array_unref (priv->runner_tx_hash);
		strv = g_value_get_boxed (value);
		if (strv && strv[0]) {
			priv->runner_tx_hash = _nm_utils_strv_to_ptrarray (strv);
			align_value = value;
		} else
			priv->runner_tx_hash = NULL;
		break;
	case PROP_RUNNER_TX_BALANCER:
		if (nm_streq0 (priv->runner_tx_balancer, g_value_get_string (value)))
			return;
		g_free (priv->runner_tx_balancer);
		priv->runner_tx_balancer = g_value_dup_string (value);
		align_value = value;
		break;
	case PROP_RUNNER_TX_BALANCER_INTERVAL:
		if (priv->runner_tx_balancer_interval == g_value_get_int (value))
			return;
		priv->runner_tx_balancer_interval = g_value_get_int (value);
		align_value = value;
		break;
	case PROP_RUNNER_ACTIVE:
		if (priv->runner_active == g_value_get_boolean (value))
			return;
		priv->runner_active = g_value_get_boolean (value);
		align_value = value;
		break;
	case PROP_RUNNER_FAST_RATE:
		if (priv->runner_fast_rate == g_value_get_boolean (value))
			return;
		priv->runner_fast_rate = g_value_get_boolean (value);
		align_value = value;
		break;
	case PROP_RUNNER_SYS_PRIO:
		if (priv->runner_sys_prio == g_value_get_int (value))
			return;
		priv->runner_sys_prio = g_value_get_int (value);
		align_value = value;
		break;
	case PROP_RUNNER_MIN_PORTS:
		if (priv->runner_min_ports == g_value_get_int (value))
			return;
		priv->runner_min_ports = g_value_get_int (value);
		align_value = value;
		break;
	case PROP_RUNNER_AGG_SELECT_POLICY:
		if (nm_streq0 (priv->runner_agg_select_policy, g_value_get_string (value)))
			return;
		g_free (priv->runner_agg_select_policy);
		priv->runner_agg_select_policy = g_value_dup_string (value);
		align_value = value;
		break;
	case PROP_LINK_WATCHERS:
		g_ptr_array_unref (priv->link_watchers);
		priv->link_watchers = _nm_utils_copy_array (g_value_get_boxed (value),
		                                            (NMUtilsCopyFunc) nm_team_link_watcher_dup,
		                                            (GDestroyNotify) nm_team_link_watcher_unref);
		if (priv->link_watchers->len)
			align_value = value;
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}

	if (prop_id != PROP_CONFIG)
		_nm_utils_json_append_gvalue (&priv->config, _prop_to_keys[prop_id], align_value);

	_align_team_properties (setting);
}

/*****************************************************************************/

static void
nm_setting_team_init (NMSettingTeam *setting)
{
	NMSettingTeamPrivate *priv = NM_SETTING_TEAM_GET_PRIVATE (setting);

	priv->runner = g_strdup (NM_SETTING_TEAM_RUNNER_ROUNDROBIN);
	priv->runner_tx_balancer_interval = -1;
	priv->runner_sys_prio = -1;
	priv->runner_min_ports = -1;
	priv->link_watchers = g_ptr_array_new_with_free_func ((GDestroyNotify) nm_team_link_watcher_unref);
}

/**
 * nm_setting_team_new:
 *
 * Creates a new #NMSettingTeam object with default values.
 *
 * Returns: (transfer full): the new empty #NMSettingTeam object
 **/
NMSetting *
nm_setting_team_new (void)
{
	return (NMSetting *) g_object_new (NM_TYPE_SETTING_TEAM, NULL);
}

static void
finalize (GObject *object)
{
	NMSettingTeamPrivate *priv = NM_SETTING_TEAM_GET_PRIVATE (object);

	g_free (priv->config);
	g_free (priv->runner);
	g_free (priv->runner_hwaddr_policy);
	g_free (priv->runner_tx_balancer);
	g_free (priv->runner_agg_select_policy);
	if (priv->runner_tx_hash)
		g_ptr_array_unref (priv->runner_tx_hash);
	g_ptr_array_unref (priv->link_watchers);

	G_OBJECT_CLASS (nm_setting_team_parent_class)->finalize (object);
}

static void
nm_setting_team_class_init (NMSettingTeamClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	NMSettingClass *setting_class = NM_SETTING_CLASS (klass);
	GArray *properties_override = _nm_sett_info_property_override_create_array ();

	g_type_class_add_private (klass, sizeof (NMSettingTeamPrivate));

	object_class->get_property     = get_property;
	object_class->set_property     = set_property;
	object_class->finalize         = finalize;

	setting_class->compare_property = compare_property;
	setting_class->verify           = verify;

	/**
	 * NMSettingTeam:config:
	 *
	 * The JSON configuration for the team network interface.  The property
	 * should contain raw JSON configuration data suitable for teamd, because
	 * the value is passed directly to teamd. If not specified, the default
	 * configuration is used.  See man teamd.conf for the format details.
	 **/
	/* ---ifcfg-rh---
	 * property: config
	 * variable: TEAM_CONFIG
	 * description: Team configuration in JSON. See man teamd.conf for details.
	 * ---end---
	 */
	obj_properties[PROP_CONFIG] =
	    g_param_spec_string (NM_SETTING_TEAM_CONFIG, "", "",
	                         NULL,
	                         G_PARAM_READWRITE |
	                         NM_SETTING_PARAM_INFERRABLE |
	                         G_PARAM_STATIC_STRINGS);

	/**
	 * NMSettingTeam:notify-peers-count:
	 *
	 * Corresponds to the teamd notify_peers.count.
	 *
	 * Since: 1.12
	 **/
	obj_properties[PROP_NOTIFY_PEERS_COUNT] =
	    g_param_spec_int (NM_SETTING_TEAM_NOTIFY_PEERS_COUNT, "", "",
	                      G_MININT32, G_MAXINT32, 0,
	                      G_PARAM_READWRITE |
	                      G_PARAM_STATIC_STRINGS);

	/**
	 * NMSettingTeam:notify-peers-interval:
	 *
	 * Corresponds to the teamd notify_peers.interval.
	 *
	 * Since: 1.12
	 **/
	obj_properties[PROP_NOTIFY_PEERS_INTERVAL] =
	    g_param_spec_int (NM_SETTING_TEAM_NOTIFY_PEERS_INTERVAL, "", "",
	                      G_MININT32, G_MAXINT32, 0,
	                      G_PARAM_READWRITE |
	                      G_PARAM_STATIC_STRINGS);

	/**
	 * NMSettingTeam:mcast-rejoin-count:
	 *
	 * Corresponds to the teamd mcast_rejoin.count.
	 *
	 * Since: 1.12
	 **/
	obj_properties[PROP_MCAST_REJOIN_COUNT] =
	    g_param_spec_int (NM_SETTING_TEAM_MCAST_REJOIN_COUNT, "", "",
	                      G_MININT32, G_MAXINT32, 0,
	                      G_PARAM_READWRITE |
	                      G_PARAM_STATIC_STRINGS);

	/**
	 * NMSettingTeam:mcast-rejoin-interval:
	 *
	 * Corresponds to the teamd mcast_rejoin.interval.
	 *
	 * Since: 1.12
	 **/
	obj_properties[PROP_MCAST_REJOIN_INTERVAL] =
	    g_param_spec_int (NM_SETTING_TEAM_MCAST_REJOIN_INTERVAL, "", "",
	                      G_MININT32, G_MAXINT32, 0,
	                      G_PARAM_READWRITE |
	                      G_PARAM_STATIC_STRINGS);

	/**
	 * NMSettingTeam:runner:
	 *
	 * Corresponds to the teamd runner.name.
	 * Permitted values are: "roundrobin", "broadcast", "activebackup",
	 * "loadbalance", "lacp", "random".
	 * When setting the runner, all the properties specific to the runner
	 * will be reset to the default value; all the properties specific to
	 * other runners will be set to an empty value (or if not possible to
	 * a default value).
	 *
	 * Since: 1.12
	 **/
	obj_properties[PROP_RUNNER] =
	    g_param_spec_string (NM_SETTING_TEAM_RUNNER, "", "",
	                         NULL,
	                         G_PARAM_READWRITE |
	                         G_PARAM_STATIC_STRINGS);

	/**
	 * NMSettingTeam:runner-hwaddr-policy:
	 *
	 * Corresponds to the teamd runner.hwaddr_policy.
	 *
	 * Since: 1.12
	 **/
	obj_properties[PROP_RUNNER_HWADDR_POLICY] =
	    g_param_spec_string (NM_SETTING_TEAM_RUNNER_HWADDR_POLICY, "", "",
	                         NULL,
	                         G_PARAM_READWRITE |
	                         G_PARAM_STATIC_STRINGS);

	/**
	 * NMSettingTeam:runner-tx-hash:
	 *
	 * Corresponds to the teamd runner.tx_hash.
	 *
	 * Since: 1.12
	 **/
	obj_properties[PROP_RUNNER_TX_HASH] =
	    g_param_spec_boxed (NM_SETTING_TEAM_RUNNER_TX_HASH, "", "",
	                        G_TYPE_STRV,
	                        G_PARAM_READWRITE |
	                        NM_SETTING_PARAM_INFERRABLE |
	                        G_PARAM_STATIC_STRINGS);

	/**
	 * NMSettingTeam:runner-tx-balancer:
	 *
	 * Corresponds to the teamd runner.tx_balancer.name.
	 *
	 * Since: 1.12
	 **/
	obj_properties[PROP_RUNNER_TX_BALANCER] =
	    g_param_spec_string (NM_SETTING_TEAM_RUNNER_TX_BALANCER, "", "",
	                         NULL,
	                         G_PARAM_READWRITE |
	                         G_PARAM_STATIC_STRINGS);

	/**
	 * NMSettingTeam:runner-tx-balancer-interval:
	 *
	 * Corresponds to the teamd runner.tx_balancer.interval.
	 *
	 * Since: 1.12
	 **/
	obj_properties[PROP_RUNNER_TX_BALANCER_INTERVAL] =
	    g_param_spec_int (NM_SETTING_TEAM_RUNNER_TX_BALANCER_INTERVAL, "", "",
	                      G_MININT32, G_MAXINT32, 0,
	                      G_PARAM_READWRITE |
	                      G_PARAM_STATIC_STRINGS);

	/**
	 * NMSettingTeam:runner-active:
	 *
	 * Corresponds to the teamd runner.active.
	 *
	 * Since: 1.12
	 **/
	obj_properties[PROP_RUNNER_ACTIVE] =
	    g_param_spec_boolean (NM_SETTING_TEAM_RUNNER_ACTIVE, "", "",
	                          FALSE,
	                          G_PARAM_READWRITE |
	                          G_PARAM_STATIC_STRINGS);

	/**
	 * NMSettingTeam:runner-fast-rate:
	 *
	 * Corresponds to the teamd runner.fast_rate.
	 *
	 * Since: 1.12
	 **/
	obj_properties[PROP_RUNNER_FAST_RATE] =
	    g_param_spec_boolean (NM_SETTING_TEAM_RUNNER_FAST_RATE, "", "",
	                          FALSE,
	                          G_PARAM_READWRITE |
	                          G_PARAM_STATIC_STRINGS);

	/**
	 * NMSettingTeam:runner-sys-prio:
	 *
	 * Corresponds to the teamd runner.sys_prio.
	 *
	 * Since: 1.12
	 **/
	obj_properties[PROP_RUNNER_SYS_PRIO] =
	    g_param_spec_int (NM_SETTING_TEAM_RUNNER_SYS_PRIO, "", "",
	                      G_MININT32, G_MAXINT32, 0,
	                      G_PARAM_READWRITE |
	                      G_PARAM_STATIC_STRINGS);

	/**
	 * NMSettingTeam:runner-min-ports:
	 *
	 * Corresponds to the teamd runner.min_ports.
	 *
	 * Since: 1.12
	 **/
	obj_properties[PROP_RUNNER_MIN_PORTS] =
	    g_param_spec_int (NM_SETTING_TEAM_RUNNER_MIN_PORTS, "", "",
	                      G_MININT32, G_MAXINT32, 0,
	                      G_PARAM_READWRITE |
	                      G_PARAM_STATIC_STRINGS);

	/**
	 * NMSettingTeam:runner-agg-select-policy:
	 *
	 * Corresponds to the teamd runner.agg_select_policy.
	 *
	 * Since: 1.12
	 **/
	obj_properties[PROP_RUNNER_AGG_SELECT_POLICY] =
	    g_param_spec_string (NM_SETTING_TEAM_RUNNER_AGG_SELECT_POLICY, "", "",
	                         NULL,
	                         G_PARAM_READWRITE |
	                         G_PARAM_STATIC_STRINGS);

	/**
	 * NMSettingTeam:link-watchers: (type GPtrArray(NMTeamLinkWatcher))
	 *
	 * Link watchers configuration for the connection: each link watcher is
	 * defined by a dictionary, whose keys depend upon the selected link
	 * watcher. Available link watchers are 'ethtool', 'nsna_ping' and
	 * 'arp_ping' and it is specified in the dictionary with the key 'name'.
	 * Available keys are:   ethtool: 'delay-up', 'delay-down', 'init-wait';
	 * nsna_ping: 'init-wait', 'interval', 'missed-max', 'target-host';
	 * arp_ping: all the ones in nsna_ping and 'source-host', 'validate-active',
	 * 'validate-inactive', 'send-always'. See teamd.conf man for more details.
	 *
	 * Since: 1.12
	 **/
	obj_properties[PROP_LINK_WATCHERS] =
	    g_param_spec_boxed (NM_SETTING_TEAM_LINK_WATCHERS, "", "",
	                        G_TYPE_PTR_ARRAY,
	                        G_PARAM_READWRITE |
	                        G_PARAM_STATIC_STRINGS);

	_properties_override_add_transform (properties_override,
	                                    obj_properties[PROP_LINK_WATCHERS],
	                                    G_VARIANT_TYPE ("aa{sv}"),
	                                    team_link_watchers_to_dbus,
	                                    team_link_watchers_from_dbus);

	/* ---dbus---
	 * property: interface-name
	 * format: string
	 * description: Deprecated in favor of connection.interface-name, but can
	 *   be used for backward-compatibility with older daemons, to set the
	 *   team's interface name.
	 * ---end---
	 */
	_properties_override_add_dbus_only (properties_override,
	                                    "interface-name",
	                                    G_VARIANT_TYPE_STRING,
	                                    _nm_setting_get_deprecated_virtual_interface_name,
	                                    NULL);

	g_object_class_install_properties (object_class, _PROPERTY_ENUMS_LAST, obj_properties);

	_nm_setting_class_commit_full (setting_class, NM_META_SETTING_TYPE_TEAM,
	                               NULL, properties_override);
}
