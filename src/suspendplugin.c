/*
 *  Connection Manager plugin
 *
 *  Copyright (C) 2018 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#define CONNMAN_API_SUBJECT_TO_CHANGE
#include <connman/plugin.h>
#include <connman/log.h>
#include <connman/device.h>

#include <mce_display.h>

#include <net/if.h>

#include <stdint.h>

#include <netlink/errno.h>
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>

#include <linux/nl80211.h>

#include <errno.h>

#include <unistd.h>

#include <sys/ioctl.h>

#define QUOTE(x) #x
#define STRINGIFY(x) QUOTE(x)

#define SET_WOWLAN(iface)   do { \
                                if (!wowlan_##iface##_enabled) { \
                                    wowlan_##iface##_enabled = (suspend_set_wowlan(STRINGIFY(iface)) == 0); \
                                } \
                            } while(false)

#define TESTMODE_CMD_ID_SUSPEND 101

#define PRIV_CMD_SIZE 512
typedef struct android_wifi_priv_cmd {
    char buf[PRIV_CMD_SIZE];
    int used_len;
    int total_len;
} android_wifi_priv_cmd;

enum mce_display_events {
    DISPLAY_EVENT_VALID,
    DISPLAY_EVENT_STATE,
    DISPLAY_EVENT_COUNT
};

struct testmode_cmd_hdr {
    uint32_t idx;
    uint32_t buflen;
};

struct testmode_cmd_suspend {
    struct testmode_cmd_hdr header;
    uint8_t suspend;
};

struct multicast_group {
    const char *group;
    int id;
};

static MceDisplay* mce_display = NULL;
static gulong mce_display_event_ids[DISPLAY_EVENT_COUNT];
static gboolean display_was_off = TRUE;

static struct nl_sock *nl_socket = NULL;
static int driver_id = -1;

static gboolean wowlan_wlan0_enabled = FALSE;
static gboolean wowlan_ap0_enabled = FALSE;
static gboolean wowlan_p2p0_enabled = FALSE;

static
int
handle_nl_command_valid(
    struct nl_msg *msg,
    void *arg)
{
    int *ret = arg;
    *ret = 0;
    DBG("%d", *ret);
    return NL_SKIP;
}

static
int
handle_nl_command_error(
    struct sockaddr_nl *nla,
    struct nlmsgerr *err,
    void *arg)
{
    int *ret = arg;
    *ret = err->error;
    connman_error("%s: error: %d", __func__, *ret);
    return NL_SKIP;
}

static
int
handle_nl_command_finished(
    struct nl_msg *msg,
    void *arg)
{
    int *ret = arg;
    *ret = 0;
    DBG("%d", *ret);
    return NL_SKIP;
}

static
int
handle_nl_command_ack(
    struct nl_msg *msg,
    void *arg)
{
    int *ret = arg;
    *ret = 0;
    DBG("%d", *ret);
    return NL_STOP;
}

static
int
handle_nl_seq_check(
    struct nl_msg *msg,
    void *arg)
{
    DBG("");

    return NL_OK;
}

static
int
suspend_plugin_netlink_handler()
{
    struct nl_cb *cb;
    int res = 0;
    int err = 0;

    cb = nl_cb_alloc(NL_CB_VERBOSE);
    if (!cb) {
        connman_error("%s: failed to allocate netlink callbacks", __func__);
        return 1;
    }

    err = 1;
    nl_cb_err(cb, NL_CB_CUSTOM, handle_nl_command_error, &err);
    nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, handle_nl_command_valid, &err);
    nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, handle_nl_command_finished, &err);
    nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, handle_nl_command_ack, &err);
    nl_cb_set(cb, NL_CB_SEQ_CHECK, NL_CB_CUSTOM, handle_nl_seq_check, &err);

    while (err == 1) {
        DBG("waiting until nl testmode command has been processed\n");
        res = nl_recvmsgs(nl_socket, cb);
        if (res < 0) {
            connman_error("nl_recvmsgs failed - wmtWifi %s:%d\n", __func__, res);
            break;
        }
    }

    if (err == 0) {
        DBG("suspend on/off successfully done");
    }

    nl_cb_put(cb);

    return err;
}

static
int
suspend_set_wowlan(
    const char *ifname)
{
    int err = 0;
    struct nl_msg *msg;

    struct nlattr *wowlan_triggers;

    int ifindex = 0;

    ifindex = if_nametoindex(ifname);

    if (ifindex == 0) {
        DBG("iface %s is not active/present (set_wowlan).", ifname);
        return -1;
    }

    DBG("iface %s, setting wowlan.", ifname);

    msg = nlmsg_alloc();

    genlmsg_put(msg, 0, 0, driver_id, 0, 0, NL80211_CMD_SET_WOWLAN, 0);

    nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifindex);

    wowlan_triggers = nla_nest_start(msg, NL80211_ATTR_WOWLAN_TRIGGERS);

    nla_put_flag(msg, NL80211_WOWLAN_TRIG_DISCONNECT);

    nla_nest_end(msg, wowlan_triggers);

    if (nl_send_auto(nl_socket, msg) < 0) {
        connman_error("Failed to send wowlan command.\n");
    } else {
        if (suspend_plugin_netlink_handler() != 0) {
            connman_error("%s: setting wowlan failed %d\n", __func__, err);
        }
    }

    nlmsg_free(msg);
    return err;
}

static
void
suspend_handle_display_on_off_iface(
    const char *ifname,
    int on_off)
{
    // first try the vendor specific TESTMODE command
    struct nl_msg *msg = NULL;
    int ifindex = 0;
    struct testmode_cmd_suspend susp_cmd;

    ifindex = if_nametoindex(ifname);

    if (ifindex == 0) {
        DBG("iface %s is not active/present (handle on_off).", ifname);
        return;
    }

    DBG("iface: %s suspend: %d\n", ifname, (int)on_off ? 0 : 1);

    msg = nlmsg_alloc();

    genlmsg_put(msg, 0, 0, driver_id, 0, 0, NL80211_CMD_TESTMODE, 0);

    memset(&susp_cmd, 0, sizeof susp_cmd);
    susp_cmd.header.idx = TESTMODE_CMD_ID_SUSPEND;
    susp_cmd.header.buflen = 0; // unused
    susp_cmd.suspend = (int)(on_off) ? 0 : 1;

    nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifindex);
    nla_put(
        msg,
        NL80211_ATTR_TESTDATA,
        sizeof susp_cmd,
        (void*)&susp_cmd);

    if (nl_send_auto(nl_socket, msg) < 0) {
        connman_error("Failed to send testmode command.\n");
    } else {
        if (suspend_plugin_netlink_handler() != 0) {
            // the driver returned an error or doesn't support this command
            // could be a driver which uses "SETSUSPENDMODE 1/0" priv cmds
            connman_warn("%s: TESTMODE command failed."
                "Ignore if the kernel is using a gen3 wmtWifi driver.\n",
                __func__);
        }
    }

    nlmsg_free(msg);

    // also send SETSUSPENDMODE private commands for gen3 drivers:
    int cmd_len = 0;
    struct ifreq ifr;
    android_wifi_priv_cmd priv_cmd;

    int ret;
    int ioctl_sock;

    ioctl_sock = socket(PF_INET, SOCK_DGRAM, 0);

    memset(&ifr, 0, sizeof(ifr));
    memset(&priv_cmd, 0, sizeof(priv_cmd));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ);

    cmd_len = snprintf(
        priv_cmd.buf,
        sizeof(priv_cmd.buf),
        "SETSUSPENDMODE %d",
        on_off);

    priv_cmd.used_len = cmd_len + 1;
    priv_cmd.total_len = PRIV_CMD_SIZE;
    ifr.ifr_data = (void*)&priv_cmd;

    ret = ioctl(ioctl_sock, SIOCDEVPRIVATE + 1, &ifr);

    if (ret != 0) {
        connman_warn("%s: SETSUSPENDMODE private command failed: %d,"
            "ignore if the kernel is using a gen2 wmtWifi driver.",
            __func__,
            ret);
    }

    close(ioctl_sock);
}

static
void
suspend_handle_display_on()
{
    DBG("display ON");

    // also enable wowlan for ap0 if it is present.
    SET_WOWLAN(ap0);
    // we don't use p2p0 so it will never be visible but this doesn't hurt
    // either and maybe we need it at one point.
    SET_WOWLAN(p2p0);

    // set wowlan since some kernels require it to be set in order to avoid
    // forced disconnect-on-suspend.
    SET_WOWLAN(wlan0);
    suspend_handle_display_on_off_iface("wlan0", 1);
}

static
void
suspend_handle_display_off()
{
    DBG("display OFF");

    // also enable wowlan for ap0 if it is present.
    SET_WOWLAN(ap0);
    // we don't use p2p0 so it will never be visible but this doesn't hurt
    // either and maybe we need it at one point.
    SET_WOWLAN(p2p0);

    // set wowlan since some kernels require it to be set in order to avoid
    // forced disconnect-on-suspend.
    SET_WOWLAN(wlan0);
    suspend_handle_display_on_off_iface("wlan0", 0);
}

static
gboolean
suspend_display_off(
    MceDisplay *display)
{
    return display->valid && display->state == MCE_DISPLAY_STATE_OFF;
}

static
void
suspend_display_event(
    MceDisplay *display,
    void *data)
{
    if (display->valid) {
        const gboolean display_is_off = suspend_display_off(display);

        if (display_was_off != display_is_off) {
            display_was_off = display_is_off;
            if (display_is_off) {
                suspend_handle_display_off();
            } else {
                suspend_handle_display_on();
            }
        }
    }
}

static
int
suspend_plugin_init()
{
    connman_info("Initializing suspend plugin.");

    nl_socket = nl_socket_alloc();

    genl_connect(nl_socket);

    driver_id = genl_ctrl_resolve(nl_socket, "nl80211");

    mce_display = mce_display_new();
    mce_display_event_ids[DISPLAY_EVENT_VALID] =
        mce_display_add_valid_changed_handler(mce_display,
            suspend_display_event, NULL);
    mce_display_event_ids[DISPLAY_EVENT_STATE] =
        mce_display_add_state_changed_handler(mce_display,
            suspend_display_event, NULL);

    /* Handle the initial state */
    if (mce_display->valid) {
        display_was_off = suspend_display_off(mce_display);
        if (display_was_off) {
            suspend_handle_display_off();
        } else {
            suspend_handle_display_on();
        }
    } else {
        suspend_handle_display_off();
    }

    return 0;
}

static
void
suspend_plugin_exit()
{
    DBG("");

    mce_display_remove_all_handlers(mce_display, mce_display_event_ids);
    mce_display_unref(mce_display);
    mce_display = NULL;

    nl_socket_free(nl_socket);
    nl_socket = NULL;
}

CONNMAN_PLUGIN_DEFINE(suspend, "Suspend plugin for devices with wmtWifi gen2/gen3.",
    CONNMAN_VERSION, CONNMAN_PLUGIN_PRIORITY_DEFAULT, suspend_plugin_init,
    suspend_plugin_exit)

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
