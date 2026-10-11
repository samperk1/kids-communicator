/*
 * The handset functions espnow.c expects, implemented for an ESPHome gateway.
 *
 * 🔑 THIS IS THE EARLIER C GATEWAY'S gw_shim.c (not published), ADAPTED — NOT A NEW FILE. The C
 * gateway and this one must answer espnow.c identically or the two gateways
 * would behave differently on the same air. Keep them in step; the difference is only
 * where the channel and the name come from (YAML here, NVS there).
 *
 * 🚨 THE HANDSET'S POWER MODEL IS INVERTED HERE, ON PURPOSE — and more so than
 * on the C gateway. ESPHome owns the WiFi station completely: it inits it,
 * associates it, and reconnects it. So net_start_radio() must NOT start
 * anything and net_down() must NOT stop anything; both would fight ESPHome's
 * WiFi component. The gateway is mains-powered and must be listening always.
 */

#include <string.h>
#include "esp_log.h"
#include "settings.h"
#include "net.h"

static const char *TAG = "gwshim";

static int  s_channel = 6;
static char s_name[16] = "Gateway";

/* Called by the ESPHome component from its config, before espnow_begin(). */
void gw_shim_set_channel(int ch) { if (ch >= 1 && ch <= 13) s_channel = ch; }
void gw_shim_set_name(const char *n) { if (n && *n) strlcpy(s_name, n, sizeof(s_name)); }

int  settings_espnow_channel(void) { return s_channel; }

/* espnow.c calls this when a peer reports a different channel. Under ESPHome
 * the station's channel is the AP's and cannot be moved, so remember the value
 * for the log and nothing else — see comm_radio.cpp, which is where a mismatch
 * is actually reported. */
void settings_set_espnow_channel(int ch)
{
    if (ch == s_channel) return;
    ESP_LOGW(TAG, "fleet asked for channel %d, we are pinned to the AP's %d", ch, s_channel);
}

const char *settings_name(void)  { return s_name; }
const char *msg_dev_name(void)   { return s_name; }

/* 🚨 ALWAYS 0 (normal 1 Mbps), NEVER LR — a decision, not a stub. A BGNLR
 * station ASSOCIATED TO AN AP negotiates back down to BGN and the rate config
 * is silently ignored (measured). An ESPHome gateway
 * is associated by definition, so LR here would read back correctly and do
 * nothing on the air. */
int settings_wt_range(void) { return 0; }

/* ---- the radio: ESPHome owns it, we borrow it -------------------------- */
bool net_start_radio(void)   { return true; }   /* ESPHome already started it */
void net_radio_hold(void)    { }
void net_radio_release(void) { }

void net_down(void)
{
    /* Deliberately nothing. Honouring it would make the gateway intermittently
     * deaf, a fault that presents as "the handsets sometimes can't reach HA"
     * and gets blamed on range. */
    ESP_LOGD(TAG, "net_down() ignored - ESPHome owns the station and it stays up");
}
