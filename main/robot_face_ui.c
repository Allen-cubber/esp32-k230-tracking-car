#include "robot_face_ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define UI_W 128
#define UI_H 160

#define UI_HEX_BG        0x05070D
#define UI_HEX_PANEL     0x0B1020
#define UI_HEX_PANEL_2   0x111827
#define UI_HEX_FACE      0xE5E7EB
#define UI_HEX_TEXT_SUB  0x94A3B8
#define UI_HEX_HAPPY     0x7EE787
#define UI_HEX_ALERT     0xFFB020
#define UI_HEX_DANGER    0xFF5C5C
#define UI_HEX_SEARCH    0x93C5FD

#define UI_COMMAND_DEFAULT_MS 2200
#define UI_ANIM_TIMER_MS       80
#define UI_BLINK_FRAMES         2
#define UI_BLINK_PERIOD_FRAMES 48

typedef enum {
    FACE_VIS_IDLE = 0,
    FACE_VIS_HAPPY,
    FACE_VIS_SEARCH,
    FACE_VIS_ALERT,
    FACE_VIS_DANGER,
} face_visual_t;

typedef struct {
    const char *text;
    face_visual_t visual;
    uint32_t accent_hex;
} ui_profile_t;

typedef struct {
    bool ready;
    bool command_active;
    robot_ui_state_t main_state;
    face_visual_t active_visual;
    uint32_t active_accent_hex;
    uint32_t frame;
    lv_obj_t *canvas;
    lv_obj_t *status_label;
    lv_timer_t *anim_timer;
    lv_timer_t *command_timer;
    char active_text[32];
} face_ui_t;

static face_ui_t s_ui;
static LV_ATTRIBUTE_MEM_ALIGN uint8_t s_canvas_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR(UI_W, UI_H)];

static const ui_profile_t *profile_for_state(robot_ui_state_t state)
{
    static const ui_profile_t profiles[] = {
        [ROBOT_UI_BOOT] = {"STARTING", FACE_VIS_IDLE, UI_HEX_TEXT_SUB},
        [ROBOT_UI_IDLE] = {"READY", FACE_VIS_IDLE, UI_HEX_TEXT_SUB},
        [ROBOT_UI_FOLLOW] = {"FOLLOWING", FACE_VIS_HAPPY, UI_HEX_HAPPY},
        [ROBOT_UI_HAPPY] = {"FOLLOWING", FACE_VIS_HAPPY, UI_HEX_HAPPY},
        [ROBOT_UI_LOST] = {"SEARCHING", FACE_VIS_SEARCH, UI_HEX_SEARCH},
        [ROBOT_UI_WAIT_TRACK] = {"WAIT K230", FACE_VIS_SEARCH, UI_HEX_SEARCH},
        [ROBOT_UI_SEARCH] = {"SEARCHING", FACE_VIS_SEARCH, UI_HEX_SEARCH},
        [ROBOT_UI_OBSTACLE] = {"BLOCKED", FACE_VIS_DANGER, UI_HEX_DANGER},
        [ROBOT_UI_TOO_CLOSE] = {"TOO CLOSE", FACE_VIS_DANGER, UI_HEX_DANGER},
        [ROBOT_UI_TILT_LIMIT] = {"LIMIT", FACE_VIS_ALERT, UI_HEX_ALERT},
        [ROBOT_UI_ALERT] = {"ALERT", FACE_VIS_ALERT, UI_HEX_ALERT},
        [ROBOT_UI_HOLD] = {"HOLD", FACE_VIS_IDLE, UI_HEX_TEXT_SUB},
        [ROBOT_UI_CONFUSED] = {"CONFUSED", FACE_VIS_SEARCH, UI_HEX_SEARCH},
    };

    if (state < 0 || state >= (robot_ui_state_t)(sizeof(profiles) / sizeof(profiles[0]))) {
        return &profiles[ROBOT_UI_IDLE];
    }
    return &profiles[state];
}

static lv_color_t color_hex(uint32_t hex)
{
    return lv_color_hex(hex);
}

static void draw_rect(lv_coord_t x,
                      lv_coord_t y,
                      lv_coord_t w,
                      lv_coord_t h,
                      lv_coord_t radius,
                      uint32_t fill_hex)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = radius;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.bg_color = color_hex(fill_hex);
    dsc.border_opa = LV_OPA_TRANSP;
    lv_canvas_draw_rect(s_ui.canvas, x, y, w, h, &dsc);
}

static void draw_line(const lv_point_t *points,
                      uint32_t point_count,
                      uint32_t color,
                      lv_coord_t width)
{
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color_hex(color);
    dsc.width = width;
    dsc.opa = LV_OPA_COVER;
    dsc.round_start = 1;
    dsc.round_end = 1;
    lv_canvas_draw_line(s_ui.canvas, points, point_count, &dsc);
}

static void draw_arc(lv_coord_t cx,
                     lv_coord_t cy,
                     lv_coord_t r,
                     int32_t start,
                     int32_t end,
                     uint32_t color,
                     lv_coord_t width)
{
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = color_hex(color);
    dsc.width = width;
    dsc.opa = LV_OPA_COVER;
    dsc.rounded = 1;
    lv_canvas_draw_arc(s_ui.canvas, cx, cy, r, start, end, &dsc);
}

static void draw_polygon(const lv_point_t *points,
                         uint32_t point_count,
                         uint32_t color)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.bg_color = color_hex(color);
    dsc.border_opa = LV_OPA_TRANSP;
    lv_canvas_draw_polygon(s_ui.canvas, points, point_count, &dsc);
}

static void draw_pill_eye(lv_coord_t cx,
                          lv_coord_t cy,
                          lv_coord_t w,
                          lv_coord_t h,
                          uint32_t color)
{
    draw_rect(cx - w / 2, cy - h / 2, w, h, h / 2, color);
}

static void draw_star_eye(lv_coord_t cx, lv_coord_t cy, lv_coord_t s, uint32_t color)
{
    lv_point_t star[] = {
        {cx, cy - s},
        {cx + s / 4, cy - s / 4},
        {cx + s, cy},
        {cx + s / 4, cy + s / 4},
        {cx, cy + s},
        {cx - s / 4, cy + s / 4},
        {cx - s, cy},
        {cx - s / 4, cy - s / 4},
    };
    draw_polygon(star, sizeof(star) / sizeof(star[0]), color);

    lv_point_t shine1[] = {{cx - s - 2, cy}, {cx + s + 2, cy}};
    lv_point_t shine2[] = {{cx, cy - s - 2}, {cx, cy + s + 2}};
    draw_line(shine1, 2, color, 2);
    draw_line(shine2, 2, color, 2);
}

static void draw_spiral_eye(lv_coord_t cx, lv_coord_t cy, uint32_t color, uint32_t phase)
{
    int32_t a0 = (int32_t)(phase % 360);
    draw_arc(cx, cy, 19, a0, a0 + 300, color, 3);
    draw_arc(cx, cy, 12, a0 + 45, a0 + 325, color, 3);
    draw_arc(cx, cy, 6, a0 + 100, a0 + 300, color, 3);
}

static void draw_soft_background(void)
{
    lv_canvas_fill_bg(s_ui.canvas, color_hex(UI_HEX_BG), LV_OPA_COVER);
    draw_rect(3, 3, UI_W - 6, UI_H - 6, 8, UI_HEX_PANEL);
    draw_rect(6, 6, UI_W - 12, UI_H - 12, 6, UI_HEX_PANEL_2);
    draw_rect(9, 9, UI_W - 18, UI_H - 18, 4, UI_HEX_BG);
}

static bool blink_closed(void)
{
    uint32_t slot = s_ui.frame % UI_BLINK_PERIOD_FRAMES;
    return slot < UI_BLINK_FRAMES &&
           (s_ui.active_visual == FACE_VIS_IDLE || s_ui.active_visual == FACE_VIS_HAPPY);
}

static void draw_idle_face(void)
{
    const bool closed = blink_closed();
    const lv_coord_t eye_h = closed ? 4 : 25;
    const lv_coord_t eye_w = 31;
    const lv_coord_t bob = (s_ui.frame / 9) % 2;

    draw_pill_eye(42, 65 + bob, eye_w, eye_h, UI_HEX_FACE);
    draw_pill_eye(86, 65 + bob, eye_w, eye_h, UI_HEX_FACE);

    lv_point_t mouth[] = {{50, 104 + bob}, {78, 104 + bob}};
    draw_line(mouth, 2, UI_HEX_TEXT_SUB, 4);
}

static void draw_happy_face(void)
{
    const uint32_t accent = s_ui.active_accent_hex;
    const bool closed = blink_closed();
    const lv_coord_t pulse = (s_ui.frame % 10 < 5) ? 1 : 0;
    const lv_coord_t y = 64 + pulse;

    if (closed) {
        draw_pill_eye(42, y, 34, 4, accent);
        draw_pill_eye(86, y, 34, 4, accent);
    } else {
        draw_star_eye(42, y, 14 + pulse, accent);
        draw_star_eye(86, y, 14 + pulse, accent);
    }

    lv_point_t smile[] = {
        {44, 95},
        {54, 104 + pulse},
        {64, 107 + pulse},
        {74, 104 + pulse},
        {84, 95},
    };
    draw_line(smile, sizeof(smile) / sizeof(smile[0]), accent, 5);
}

static void draw_search_face(void)
{
    const uint32_t phase = s_ui.frame * 18;
    const int8_t look = (int8_t)(((s_ui.frame % 28) < 14) ? (s_ui.frame % 14) : (27 - (s_ui.frame % 28)));
    const lv_coord_t offset = look - 7;

    draw_spiral_eye(42 + offset / 2, 65, UI_HEX_SEARCH, phase);
    draw_spiral_eye(86 + offset / 2, 65, UI_HEX_SEARCH, phase + 140);

    lv_point_t mouth[] = {{50, 103}, {64, 99 + offset / 5}, {78, 104}};
    draw_line(mouth, sizeof(mouth) / sizeof(mouth[0]), UI_HEX_SEARCH, 4);

    lv_coord_t dot_x = 24 + ((s_ui.frame * 4) % 80);
    draw_rect(dot_x, 120, 11, 4, 2, UI_HEX_SEARCH);
}

static void draw_alert_face(uint32_t color)
{
    const int8_t pulse = (s_ui.frame % 8 < 4) ? 0 : 1;

    lv_point_t left_eye[] = {
        {25, 65 - pulse},
        {58, 58 - pulse},
        {61, 70 + pulse},
        {28, 76 + pulse},
    };
    lv_point_t right_eye[] = {
        {70, 58 - pulse},
        {103, 65 - pulse},
        {100, 76 + pulse},
        {67, 70 + pulse},
    };
    draw_polygon(left_eye, sizeof(left_eye) / sizeof(left_eye[0]), color);
    draw_polygon(right_eye, sizeof(right_eye) / sizeof(right_eye[0]), color);

    lv_point_t left_brow[] = {{23, 47}, {60, 57}};
    lv_point_t right_brow[] = {{68, 57}, {105, 47}};
    draw_line(left_brow, 2, color, 5);
    draw_line(right_brow, 2, color, 5);

    lv_point_t mouth[] = {{48, 104 + pulse}, {80, 104 + pulse}};
    draw_line(mouth, 2, color, 5);
}

static void render_face(void)
{
    draw_soft_background();

    switch (s_ui.active_visual) {
    case FACE_VIS_HAPPY:
        draw_happy_face();
        break;
    case FACE_VIS_SEARCH:
        draw_search_face();
        break;
    case FACE_VIS_ALERT:
        draw_alert_face(UI_HEX_ALERT);
        break;
    case FACE_VIS_DANGER:
        draw_alert_face(UI_HEX_DANGER);
        break;
    case FACE_VIS_IDLE:
    default:
        draw_idle_face();
        break;
    }

    lv_obj_invalidate(s_ui.canvas);
}

static void set_status_text(const char *text, uint32_t accent_hex)
{
    const char *safe_text = (text != NULL && text[0] != '\0') ? text : "READY";

    if (strncmp(s_ui.active_text, safe_text, sizeof(s_ui.active_text)) != 0) {
        strlcpy(s_ui.active_text, safe_text, sizeof(s_ui.active_text));
        lv_label_set_text(s_ui.status_label, s_ui.active_text);
    }
    lv_obj_set_style_text_color(s_ui.status_label, color_hex(accent_hex), 0);
}

static void set_active_profile(const ui_profile_t *profile)
{
    s_ui.active_visual = profile->visual;
    s_ui.active_accent_hex = profile->accent_hex;
    set_status_text(profile->text, profile->accent_hex);
    render_face();
}

static void command_timer_cb(lv_timer_t *timer)
{
    if (timer != s_ui.command_timer) {
        return;
    }

    lv_timer_del(timer);
    s_ui.command_timer = NULL;
    s_ui.command_active = false;
    set_active_profile(profile_for_state(s_ui.main_state));
}

static void anim_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!s_ui.ready) {
        return;
    }
    s_ui.frame++;
    render_face();
}

static void init_status_label(lv_obj_t *parent)
{
    s_ui.status_label = lv_label_create(parent);
    lv_obj_set_width(s_ui.status_label, UI_W - 10);
    lv_obj_set_pos(s_ui.status_label, 5, 133);
    lv_obj_set_style_bg_opa(s_ui.status_label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_align(s_ui.status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_ui.status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_ui.status_label, color_hex(UI_HEX_TEXT_SUB), 0);
    lv_label_set_long_mode(s_ui.status_label, LV_LABEL_LONG_DOT);
}

void robot_face_ui_init(lv_obj_t *parent)
{
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.main_state = ROBOT_UI_BOOT;
    s_ui.active_visual = FACE_VIS_IDLE;
    s_ui.active_accent_hex = UI_HEX_TEXT_SUB;

    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(parent, color_hex(UI_HEX_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);

    s_ui.canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_ui.canvas, s_canvas_buf, UI_W, UI_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(s_ui.canvas, 0, 0);
    lv_obj_set_size(s_ui.canvas, UI_W, UI_H);

    init_status_label(parent);

    s_ui.ready = true;
    set_active_profile(profile_for_state(ROBOT_UI_BOOT));

    s_ui.anim_timer = lv_timer_create(anim_timer_cb, UI_ANIM_TIMER_MS, NULL);
}

void robot_face_ui_set_state(robot_ui_state_t state)
{
    if (!s_ui.ready) {
        return;
    }

    s_ui.main_state = state;
    if (s_ui.command_active) {
        return;
    }

    const ui_profile_t *profile = profile_for_state(state);
    if (s_ui.active_visual == profile->visual &&
        s_ui.active_accent_hex == profile->accent_hex &&
        strncmp(s_ui.active_text, profile->text, sizeof(s_ui.active_text)) == 0) {
        return;
    }

    set_active_profile(profile);
}

void robot_face_ui_show_command(robot_ui_cmd_mood_t mood,
                                const char *command_text,
                                uint32_t duration_ms)
{
    if (!s_ui.ready) {
        return;
    }

    static const ui_profile_t command_profiles[] = {
        [ROBOT_UI_CMD_NONE] = {"COMMAND", FACE_VIS_IDLE, UI_HEX_FACE},
        [ROBOT_UI_CMD_HAPPY] = {"COMMAND", FACE_VIS_HAPPY, UI_HEX_HAPPY},
        [ROBOT_UI_CMD_ALERT] = {"COMMAND", FACE_VIS_ALERT, UI_HEX_ALERT},
        [ROBOT_UI_CMD_CONFUSED] = {"COMMAND", FACE_VIS_SEARCH, UI_HEX_SEARCH},
    };

    if (mood < 0 || mood >= (robot_ui_cmd_mood_t)(sizeof(command_profiles) / sizeof(command_profiles[0]))) {
        mood = ROBOT_UI_CMD_NONE;
    }
    if (duration_ms == 0) {
        duration_ms = UI_COMMAND_DEFAULT_MS;
    }

    if (s_ui.command_timer != NULL) {
        lv_timer_del(s_ui.command_timer);
        s_ui.command_timer = NULL;
    }

    ui_profile_t profile = command_profiles[mood];
    if (command_text != NULL && command_text[0] != '\0') {
        profile.text = command_text;
    }

    s_ui.command_active = true;
    set_active_profile(&profile);

    s_ui.command_timer = lv_timer_create(command_timer_cb, duration_ms, NULL);
}

void robot_face_ui_tick(void)
{
    /* Animation is driven by the LVGL timer above. */
}
