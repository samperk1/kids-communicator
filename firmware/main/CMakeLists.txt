# The public build is the credential-free ("light") build and ONLY that build.
# The full family build, with WiFi and Home Assistant, is not published: the
# whole point of this firmware is that WIFI_SSID, WIFI_PASS and any HA token are
# not IN the image at all - not merely unreachable. light_stubs.c answers the
# network API (net.h, ha.h, radio.h, telemetry.h) with honest failures.
#
# 🔑 ask.c is deliberately KEPT. It talks to an optional gateway over ESP-NOW
# and includes only espnow.h / gw_wire.h / applog.h, so the AI path costs no
# credentials. Whether a given handset may USE a gateway is the gateway's allow
# list to decide.
# 🔑 voice.c is kept too: it owns the push-to-talk state machine and is the
# only caller of ask_run(). Its direct-to-HA legs are not in this tree.
set(COMM_NET_SRCS "light_stubs.c" "voice.c")

idf_component_register(
    SRCS "main.c" "display.c" "library.c" "player.c" "ui.c" "battery.c" "faves.c"
         "settings.c" "applog.c" "audio.c" "mic.c" "wt.c" "power.c"
         ${COMM_NET_SRCS}
         "ble.c" "msg.c" "t9.c" "espnow.c" "games.c" "flamingo.c" "tanks.c" "adpcm.c" "fleet.c" "ask.c"
         "lv_font_cards_16.c" "lv_font_cards_26.c" "art.c"
         # 🔑 gps.c is in BOTH builds, and it matters most in the light one:
         # no WiFi means no HTTP Date header and no SNTP, so GPS is the only
         # clock those handsets can ever have.
         "gps.c" "gpslog.c"
         # In BOTH builds: a light-build handset has no WiFi, so USB is the only
         # way anything gets onto its card at all.
         "usbmsc.c"
         # The map draws from the same tiles in both builds — a light-build
         # handset has no WiFi, so an offline vector map is the only map it
         # could ever have.
         "map.c"
         # Flamingo sprites. GENERATED from art/sprites.py via tools/png2lvgl.py
         # — regenerate, never hand-edit. They are `const lv_img_dsc_t`, so they
         # live in flash and cost no RAM; ~6 KB for the whole set.
         "sprites/flamingo1.c" "sprites/flamingo2.c"
         "sprites/tree.c" "sprites/cactus.c" "sprites/meanbird.c" "sprites/rock.c"
         "sprites/pu_life.c" "sprites/pu_shield.c"
         "sprites/pu_lighter.c" "sprites/pu_slow.c"
         "sprites/airplane.c" "sprites/asteroid.c"
         "sprites/nest.c" "sprites/babyflam.c" "sprites/duck.c"
         "sprites/turtle.c" "sprites/cattail.c"
         "sprites/tree_s.c" "sprites/tree_l.c"
         "sprites/cactus_s.c" "sprites/cactus_l.c"
         "sprites/meanbird2.c" "sprites/meanbird3.c" "sprites/biplane.c"
    INCLUDE_DIRS "."
    # 🚨 IDF 6.0 SPLIT THE `driver` UMBRELLA into esp_driver_* components, and
    # `driver` no longer pulls them all in transitively. On 6.0 this surfaced as
    # `fatal error: driver/i2c_master.h: No such file or directory` in display.c
    # and `driver/usb_serial_jtag.h` in battery.c — headers that exist, from a
    # component nobody had required explicitly.
    # ✅ Both components exist in 5.3 as well (the split landed in 5.2, with
    # `driver` kept as a compatibility umbrella), so naming them is correct on
    # BOTH toolchains and does not fork the build.
    # Every driver header this firmware includes, named by its own component
    # rather than left to the umbrella. All eight exist in 5.3 and 6.0 alike.
    #   gpio.h, rtc_io.h    -> esp_driver_gpio
    #   i2c_master.h        -> esp_driver_i2c
    #   i2s_std.h           -> esp_driver_i2s
    #   ledc.h              -> esp_driver_ledc
    #   sdmmc_host.h        -> esp_driver_sdmmc
    #   spi_master.h        -> esp_driver_spi
    #   uart.h              -> esp_driver_uart
    #   usb_serial_jtag.h   -> esp_driver_usb_serial_jtag
    REQUIRES driver
             esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_ledc
             esp_driver_sdmmc esp_driver_spi esp_driver_uart
             esp_driver_usb_serial_jtag
             esp_timer app_update spi_flash sdmmc fatfs esp_adc nvs_flash lvgl__lvgl
             esp_wifi esp_netif esp_event esp_http_client bt mbedtls
             # ⚠️ `usb` (the USB HOST stack) was required only because
             # esp_tinyusb 1.7.x depended on it. esp_tinyusb 2.x does not, and
             # on IDF 6.0 the component is gone from the tree entirely, so
             # naming it fails the build outright.
             espressif__esp_tinyusb)

# The firmware's sources no longer branch on COMM_LIGHT, but keep the symbol
# defined so code carried over from the family tree means the same thing here.
target_compile_definitions(${COMPONENT_LIB} PUBLIC COMM_LIGHT=1)

# 🔑 LVGL'S OBJECT POOL LIVES IN PSRAM. sdkconfig.defaults sets
# CONFIG_LV_MEM_CUSTOM_INCLUDE="lv_psram.h", and lv_mem.c is compiled inside
# the LVGL component, so that component needs this directory on ITS include
# path. See lvgl_hook/lv_psram.h for why.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_include_directories(${lvgl_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/lvgl_hook")
