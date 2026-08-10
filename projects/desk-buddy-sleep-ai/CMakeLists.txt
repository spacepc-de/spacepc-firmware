cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "../spacepc-airstation/components/esp32_p4_wifi6_touch_lcd_4_3")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "desk-buddy-sleep-ai-v0.2.0")
project(desk_buddy_sleep_ai)
