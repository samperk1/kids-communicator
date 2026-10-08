cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# NOTE: deliberately NO `set(COMPONENTS main)`. The Espressif examples ship that
# line and it trims esp_psram out of the build, which makes CONFIG_SPIRAM
# impossible to enable no matter what sdkconfig.defaults says. That cost an
# evening on 2026-08-12. Do not re-add it.

project(communicator)
