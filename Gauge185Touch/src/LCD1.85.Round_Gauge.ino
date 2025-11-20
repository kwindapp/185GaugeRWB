/* Minimal LVGL + ESP-NOW RPM display for ESP32-S3 Touch LCD 1.85" round
 * Only essential board functions: LCD + LVGL + I2C / GPIO expander / backlight.
 * Uses printf() instead of Serial so messages appear with vendor logs.
 */

#include "Display_ST77916.h"
#include "LVGL_Driver.h"
#include "PWR_Key.h"

// custom fonts
#include "ui_font_Hollow22.h"
#include "ui_font_Hollow38.h"
#include "ui_font_Hollow85.h"
#include "ui_font_t20.h"

#include "lvgl.h"

#include <WiFi.h>
#include <esp_now.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* --------------------------------------------------------------------------
 *  COLOR SCHEMES – TAP GAUGE TO CYCLE BETWEEN THEM
 *
 *  All colors are standard RGB hex (0xRRGGBB).
 * -------------------------------------------------------------------------- */

typedef struct {
    uint32_t bg;
    uint32_t gauge_lime;      // ticks + outer arc
    uint32_t scale_text;      // numbers 0..8
    uint32_t inner_start;     // inner ring start
    uint32_t inner_end;       // inner ring end
    uint32_t needle;          // needle color
    uint32_t hub_fill;        // center hub fill
    uint32_t hub_border;      // center hub border
    uint32_t rpm_text;        // big numeric RPM in center
    uint32_t brand_text;      // "RAUH Welt BEGRIFF"
} ColorScheme;

// Skin 0 – your current RAUH Welt look (default)
static const ColorScheme color_schemes[] = {
    {
        .bg         = 0x000000,
        .gauge_lime = 0xB7FF00,
        .scale_text = 0x99CF11,
        .inner_start= 0x222222,
        .inner_end  = 0x000000,
        .needle     = 0xFF3B30,
        .hub_fill   = 0x111111,
        .hub_border = 0xB7FF00,
        .rpm_text   = 0x6C7780,
        .brand_text = 0x8F8988
    },
    // Skin 1 – White / red motorsport
    {
        .bg         = 0x101010,
        .gauge_lime = 0xFFFFFF, // white ticks/arc
        .scale_text = 0xFF3B30, // red numbers
        .inner_start= 0x444444,
        .inner_end  = 0x111111,
        .needle     = 0x8F8988, // red needle
        .hub_fill   = 0x000000,
        .hub_border = 0xFFFFFF,
        .rpm_text   = 0xFFFFFF,
        .brand_text = 0x8F8988f
    },
    // Skin 2 – Cyan / gold “retro digital”
    {
        .bg         = 0x000000,
        .gauge_lime = 0x00E0FF, // cyan ticks/arc
        .scale_text = 0x00E0FF,
        .inner_start= 0x003344,
        .inner_end  = 0x000000,
        .needle     = 0xFFC800, // warm gold needle
        .hub_fill   = 0x000000,
        .hub_border = 0x00E0FF,
        .rpm_text   = 0x00E0FF,
        .brand_text = 0x8F8988
    }
};

static int current_scheme = 0;   // starts with your RAUH Welt colors
static lv_timer_t *rpm_timer = NULL;

/* --------------------------------------------------------------------------
 *  ESP-NOW RPM packet definition
 * -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
  uint32_t rpm;
} RpmPacket;

volatile int32_t rpm_value = 0;

/* --------------------------------------------------------------------------
 *  Gauge parameters
 * -------------------------------------------------------------------------- */

// Actual RPM range
static const int32_t RPM_MIN        = 0;
static const int32_t RPM_MAX        = 8000;

// Displayed scale (0..8 → x1000)
static const int32_t SCALE_MIN      = 0;
static const int32_t SCALE_MAX      = 8;

// Geometry
static const int GAUGE_START_DEG = 135;
static const int GAUGE_SWEEP_DEG = 270;

// Needle parameters
static const float   NEEDLE_INNER_RATIO = 0.20f;   // start at 20% of radius
static const float   NEEDLE_OUTER_RATIO = 0.75f;   // end at 75% of radius

static const int     NEEDLE_WIDTH       = 6;
static const lv_opa_t NEEDLE_OPA        = LV_OPA_50;   // 50% transparent

static const int ARC_WIDTH = 14;

/* --------------------------------------------------------------------------
 *  LVGL UI objects
 * -------------------------------------------------------------------------- */

static lv_obj_t  *rpm_meter   = NULL;
static lv_obj_t  *needle_line = NULL;
static lv_point_t needle_points[2];

static lv_obj_t *rpm_label      = NULL;
static lv_obj_t *rpm_unit_label = NULL;

/* Forward declaration so the touch callback can recreate the gauge */
void Lvgl_CreateRpmGauge(void);

/* --------------------------------------------------------------------------
 *  Needle update
 * -------------------------------------------------------------------------- */

static void update_needle(int32_t rpm)
{
    if (!rpm_meter || !needle_line) return;

    if (rpm < RPM_MIN) rpm = RPM_MIN;
    if (rpm > RPM_MAX) rpm = RPM_MAX;

    // convert RPM 0..8000 → scale 0..8
    float scale_val = (float)rpm / 1000.0f;
    if (scale_val < SCALE_MIN) scale_val = SCALE_MIN;
    if (scale_val > SCALE_MAX) scale_val = SCALE_MAX;

    float t = (scale_val - SCALE_MIN) / (float)(SCALE_MAX - SCALE_MIN);
    float angle_deg = GAUGE_START_DEG + t * GAUGE_SWEEP_DEG;
    float angle_rad = angle_deg * (float)M_PI / 180.0f;

    lv_coord_t dw = lv_disp_get_hor_res(NULL);
    lv_coord_t dh = lv_disp_get_ver_res(NULL);
    lv_coord_t cx = dw / 2;
    lv_coord_t cy = dh / 2;

    lv_coord_t meter_radius = lv_obj_get_width(rpm_meter) / 2;
    float outer_r = (float)meter_radius - ARC_WIDTH * 0.5f;
    float inner_r = outer_r * NEEDLE_INNER_RATIO;
    outer_r      *= NEEDLE_OUTER_RATIO;

    needle_points[0].x = cx + cosf(angle_rad) * inner_r;
    needle_points[0].y = cy + sinf(angle_rad) * inner_r;
    needle_points[1].x = cx + cosf(angle_rad) * outer_r;
    needle_points[1].y = cy + sinf(angle_rad) * outer_r;

    lv_line_set_points(needle_line, needle_points, 2);
}

/* --------------------------------------------------------------------------
 *  Timer callback
 * -------------------------------------------------------------------------- */

static void rpm_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    int32_t rpm = rpm_value;
    update_needle(rpm);

    if (rpm_label) {
        char buf[16];
        lv_snprintf(buf, sizeof(buf), "%ld", (long)rpm);
        lv_label_set_text(rpm_label, buf);
    }
}

/* --------------------------------------------------------------------------
 *  Touch callback – tap anywhere to cycle color schemes
 * -------------------------------------------------------------------------- */

static void touch_color_cycle_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    current_scheme++;
    if (current_scheme >= (int)(sizeof(color_schemes) / sizeof(color_schemes[0]))) {
        current_scheme = 0;
    }

    printf("Switching to color scheme %d\r\n", current_scheme);
    Lvgl_CreateRpmGauge();   // rebuild gauge with new colors
}

/* --------------------------------------------------------------------------
 *  Gauge creation
 * -------------------------------------------------------------------------- */

void Lvgl_CreateRpmGauge()
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);

    const ColorScheme *cs = &color_schemes[current_scheme];

    lv_color_t color_bg          = lv_color_hex(cs->bg);
    lv_color_t color_lime        = lv_color_hex(cs->gauge_lime);
    lv_color_t color_scale_text  = lv_color_hex(cs->scale_text);
    lv_color_t color_inner_start = lv_color_hex(cs->inner_start);
    lv_color_t color_inner_end   = lv_color_hex(cs->inner_end);
    lv_color_t color_needle      = lv_color_hex(cs->needle);
    lv_color_t color_hub_fill    = lv_color_hex(cs->hub_fill);
    lv_color_t color_hub_border  = lv_color_hex(cs->hub_border);
    lv_color_t color_rpm_text    = lv_color_hex(cs->rpm_text);
    lv_color_t color_brand_text  = lv_color_hex(cs->brand_text);

    // Background
    lv_obj_set_style_bg_color(scr, color_bg, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Meter
    rpm_meter = lv_meter_create(scr);
    // 390x390 (you set this); if it clips on your display, set to 240,240.
    lv_obj_set_size(rpm_meter, 370, 370);
    lv_obj_center(rpm_meter);

    lv_obj_set_style_bg_opa(rpm_meter, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rpm_meter, 0, 0);

    // Numbers 0–8 use their own color
    lv_obj_set_style_text_font(rpm_meter, &ui_font_Hollow85,
                               LV_PART_TICKS | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(rpm_meter, color_scale_text,
                                LV_PART_TICKS | LV_STATE_DEFAULT);

    lv_meter_scale_t *scale = lv_meter_add_scale(rpm_meter);
    lv_meter_set_scale_range(rpm_meter, scale,
                             SCALE_MIN, SCALE_MAX,
                             GAUGE_SWEEP_DEG, GAUGE_START_DEG);

    // Tick lines use gauge lime color
    lv_meter_set_scale_ticks(rpm_meter, scale, 41, 2, 10, color_lime);
    lv_meter_set_scale_major_ticks(rpm_meter, scale,
                                   5, 4, 20, color_lime, 13);

    // Outer lime arc
    lv_meter_indicator_t *outer_arc =
        lv_meter_add_arc(rpm_meter, scale, ARC_WIDTH, color_lime, 0);
    lv_meter_set_indicator_start_value(rpm_meter, outer_arc, SCALE_MIN);
    lv_meter_set_indicator_end_value(rpm_meter, outer_arc, SCALE_MAX);

    // Dark inner lines / ring for depth
    lv_meter_indicator_t *inner_ring =
        lv_meter_add_scale_lines(rpm_meter, scale,
                                 color_inner_start,
                                 color_inner_end,
                                 false,
                                 0);
    lv_meter_set_indicator_start_value(rpm_meter, inner_ring, SCALE_MIN);
    lv_meter_set_indicator_end_value(rpm_meter, inner_ring, SCALE_MAX);

    // Needle line (center-based, semi-transparent)
    needle_line = lv_line_create(scr);
    lv_obj_remove_style_all(needle_line);
    lv_obj_set_style_line_width(needle_line, NEEDLE_WIDTH, 0);
    lv_obj_set_style_line_color(needle_line, color_needle, 0);
    lv_obj_set_style_line_opa(needle_line, NEEDLE_OPA, 0);

    lv_line_set_points(needle_line, needle_points, 2);

    // Center hub
    lv_obj_t *hub = lv_obj_create(scr);
    lv_obj_remove_style_all(hub);
    lv_obj_set_size(hub, 26, 26);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, color_hub_fill, 0);
    lv_obj_set_style_border_width(hub, 3, 0);
    lv_obj_set_style_border_color(hub, color_hub_border, 0);
    lv_obj_align(hub, LV_ALIGN_CENTER, 0, 0);

    // Center RPM text
    rpm_label = lv_label_create(scr);
    lv_obj_set_style_text_color(rpm_label, color_rpm_text, 0);
    lv_obj_set_style_text_font(rpm_label, &ui_font_Hollow85, 0);
    lv_label_set_text(rpm_label, "0");
    lv_obj_align(rpm_label, LV_ALIGN_CENTER, 0, 60);

    // "RAUH Welt BEGRIFF" label
    rpm_unit_label = lv_label_create(scr);
    lv_obj_set_style_text_color(rpm_unit_label, color_brand_text, 0);
    lv_obj_set_style_text_font(rpm_unit_label, &ui_font_t20, 0);
    lv_label_set_text(rpm_unit_label, "RAUH Welt BEGRIFF");
    lv_obj_align_to(rpm_unit_label, rpm_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 28);

    // Full-screen transparent touch button to change scheme
    lv_obj_t *touch_btn = lv_btn_create(scr);
    lv_obj_remove_style_all(touch_btn);
    lv_obj_set_size(touch_btn, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(touch_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(touch_btn, 0, 0);
    lv_obj_add_event_cb(touch_btn, touch_color_cycle_cb, LV_EVENT_CLICKED, NULL);

    // Ensure needle starts in correct place
    update_needle(rpm_value);

    // Recreate timer (avoid multiple timers stacking)
    if (rpm_timer != NULL) {
        lv_timer_del(rpm_timer);
        rpm_timer = NULL;
    }
    rpm_timer = lv_timer_create(rpm_timer_cb, 100, NULL);
}

/* --------------------------------------------------------------------------
 *  ESP-NOW
 * -------------------------------------------------------------------------- */

void esp_now_recv_cb(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len)
{
    if (len < (int)sizeof(RpmPacket)) return;

    RpmPacket packet;
    memcpy(&packet, incomingData, sizeof(packet));

    if (packet.rpm <= RPM_MAX)
        rpm_value = packet.rpm;
}

void setupEspNow()
{
    WiFi.mode(WIFI_STA);
    delay(100);

    if (esp_now_init() != ESP_OK) {
        printf("ESP-NOW init failed!\r\n");
        return;
    }
    esp_now_register_recv_cb(esp_now_recv_cb);
}

/* --------------------------------------------------------------------------
 *  Arduino setup/loop
 * -------------------------------------------------------------------------- */

void setup() {
  printf("\r\nBooting ESP32-S3 LVGL RPM gauge...\r\n");

  I2C_Init();
  TCA9554PWR_Init(0x00);
  Backlight_Init();

  LCD_Init();
  Lvgl_Init();

  setupEspNow();
  Lvgl_CreateRpmGauge();
}

void loop() {
  Lvgl_Loop();
  vTaskDelay(pdMS_TO_TICKS(5));
}
