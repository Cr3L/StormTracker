#ifndef LV_CONF_H
#define LV_CONF_H

/* Match the device's renderer, fonts and fixed allocator for host previews. */
#define LV_COLOR_DEPTH 16
#define LV_MEM_SIZE (16 * 1024U)
#define LV_USE_OS LV_OS_NONE
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_USE_DRAW_SW 1

/* A failed host assertion must return a failing process, not wait forever. */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1
#define LV_ASSERT_HANDLER_INCLUDE <stdlib.h>
#define LV_ASSERT_HANDLER abort();

#endif
