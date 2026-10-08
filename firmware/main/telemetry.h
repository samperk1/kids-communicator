#pragma once

/* Battery telemetry over WiFi for unattended battery-life runs — see
 * telemetry.c for what this can and cannot actually measure. Reports to
 * sensor.comm_XXXX_battery in Home Assistant, one sensor per board. */
void telemetry_init(void);
