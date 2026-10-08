/* SPDX-License-Identifier: Apache-2.0
 * Real IDF6.1 drivers. Every receiver uses a physical pad in the public graph.
 * ISR times below are guest esp_timer microseconds, not exact edge timestamps.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "driver/mcpwm_prelude.h"
#include "driver/sdm.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#include "soc/system_reg.h"
#include "soc/soc.h"

#include "hal/sdm_caps.h"
#if CONFIG_PULSE_NATIVE_CONNECTED
#define WIRING "connected"
#define COUNTER_CONNECTED true
#define CAPTURE_CONNECTED true
#elif CONFIG_PULSE_NATIVE_DISCONNECTED
#define WIRING "disconnected"
#define COUNTER_CONNECTED false
#define CAPTURE_CONNECTED false
#else
#define WIRING "wrong-wire"
#define COUNTER_CONNECTED false
#define CAPTURE_CONNECTED true
#endif

static unsigned failures;
#define EVENTS 2048
struct event { int kind; int value; int64_t callback_us; };
static struct event events[EVENTS];
static volatile unsigned event_count;
static volatile bool event_overflow;
static volatile unsigned fade_events, pcnt_events, capture_events, stop_events, brake_events;
static volatile int64_t fade_us;
static void record(int kind, int value)
{
    unsigned n = event_count;
    if (n < EVENTS) {
        events[n] = (struct event){kind, value, esp_timer_get_time()};
        event_count = n + 1;
    } else { event_overflow = true; }
}
static void check(const char *phase, const char *name, bool ok)
{
    printf("PULSE_CHECK phase=%s name=%s result=%s time_us=%" PRId64 "\n",
           phase, name, ok ? "PASS" : "FAIL", esp_timer_get_time());
    failures += !ok;
    fflush(stdout);
}
static void status(const char *phase, const char *name, esp_err_t result)
{
    printf("PULSE_STATUS phase=%s actual=%s operation=%s\n", phase, esp_err_to_name(result), name);
    check(phase, "api_ok", result == ESP_OK);
    ESP_ERROR_CHECK(result);
}
#define OK(p, expr) status(p, #expr, (expr))
static void phase(const char *name)
{
    printf("PULSE_PHASE name=%s time_us=%" PRId64 "\n", name, esp_timer_get_time());
    fflush(stdout);
}
static void wait_ms(unsigned ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static void dump_events(const char *name)
{
    unsigned n = event_count;
    for (unsigned i = 0; i < n; i++) {
        printf("PULSE_CALLBACK phase=%s index=%u kind=%d value=%d callback_us=%" PRId64 "\n",
               name, i, events[i].kind, events[i].value, events[i].callback_us);
    }
    check(name, "bounded_event_storage", !event_overflow);
    event_count = 0;
    event_overflow = false;
}
static bool fade_cb(const ledc_cb_param_t *e, void *ctx)
{
    (void)ctx;
    fade_us = esp_timer_get_time(); fade_events++; record(1, e->duty); return false;
}
static bool count_cb(pcnt_unit_handle_t unit, const pcnt_watch_event_data_t *e, void *ctx)
{
    (void)unit; (void)ctx; pcnt_events++; record(2, e->watch_point_value); return false;
}
static pcnt_unit_handle_t counter;
static pcnt_channel_handle_t edge_channel;
static int read_count(const char *name)
{
    int value;
    OK(name, pcnt_unit_get_count(counter, &value));
    printf("PULSE_COUNT phase=%s value=%d time_us=%" PRId64 " events=%u\n", name, value,
           esp_timer_get_time(), pcnt_events);
    return value;
}
static void open_counter(void)
{
    pcnt_unit_config_t unit = {.low_limit = -100, .high_limit = 100, .flags.accum_count = true};
    pcnt_chan_config_t input = {.edge_gpio_num = 5, .level_gpio_num = 15};
    OK("counter_setup", pcnt_new_unit(&unit, &counter));
    OK("counter_setup", pcnt_new_channel(counter, &input, &edge_channel));
    OK("counter_setup", pcnt_channel_set_edge_action(edge_channel, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                                    PCNT_CHANNEL_EDGE_ACTION_HOLD));
    OK("counter_setup", pcnt_channel_set_level_action(edge_channel, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                     PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    pcnt_glitch_filter_config_t filter = {.max_glitch_ns = 1000};
    OK("counter_setup", pcnt_unit_set_glitch_filter(counter, &filter));
    OK("counter_setup", pcnt_unit_add_watch_point(counter, 10));
    OK("counter_setup", pcnt_unit_add_watch_point(counter, -10));
    OK("counter_setup", pcnt_unit_add_watch_point(counter, 100));
    OK("counter_setup", pcnt_unit_add_watch_point(counter, -100));
    pcnt_event_callbacks_t cbs = {.on_reach = count_cb};
    OK("counter_setup", pcnt_unit_register_event_callbacks(counter, &cbs, NULL));
    OK("counter_setup", pcnt_unit_enable(counter));
    OK("counter_setup", pcnt_unit_clear_count(counter));
    OK("counter_setup", pcnt_unit_start(counter));
}
static void counter_window(const char *name, bool positive)
{
    phase(name);
    OK(name, pcnt_unit_clear_count(counter));
    int64_t start = esp_timer_get_time();
    wait_ms(150);
    int value = read_count(name);
    printf("PULSE_WINDOW phase=%s start_us=%" PRId64 " end_us=%" PRId64 "\n", name, start, esp_timer_get_time());
    check(name, "physical_count_sign", COUNTER_CONNECTED ? (positive ? value > 100 : value < -100) : value == 0);
}
static void __attribute__((unused)) ledc_pcnt(void)
{
    OK("setup", gpio_set_direction(14, GPIO_MODE_OUTPUT));
    OK("setup", gpio_set_level(14, 1));
    OK("setup", gpio_set_direction(3, GPIO_MODE_OUTPUT));
    OK("setup", gpio_set_level(3, 0));
    open_counter();
    ledc_timer_config_t timer = {.speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT, .freq_hz = 1000, .clk_cfg = LEDC_USE_APB_CLK};
    ledc_channel_config_t channel = {.gpio_num = 4, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 256, .hpoint = 0};
    OK("ledc_setup", ledc_timer_config(&timer));
    OK("ledc_setup", ledc_channel_config(&channel));
    OK("fade_setup", ledc_fade_func_install(0));
    ledc_cbs_t cbs = {.fade_cb = fade_cb};
    OK("fade_setup", ledc_cb_register(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, &cbs, NULL));
    counter_window("ledc_apb_count", true);
    check("ledc_apb_count", "watch_irq", COUNTER_CONNECTED ? pcnt_events > 0 : pcnt_events == 0);
    OK("counter_direction", gpio_set_level(14, 0));
    counter_window("pcnt_negative_direction", false);
    OK("counter_direction", gpio_set_level(14, 1));
    phase("pcnt_gate");
    int gate_count = read_count("pcnt_gate");
    REG_CLR_BIT(SYSTEM_PERIP_CLK_EN0_REG, SYSTEM_PCNT_CLK_EN);
    wait_ms(20);
    REG_SET_BIT(SYSTEM_PERIP_CLK_EN0_REG, SYSTEM_PCNT_CLK_EN);
    check("pcnt_gate", "gated_counter_holds", gate_count == read_count("pcnt_gate"));
    phase("pcnt_stop");
    OK("pcnt_stop", pcnt_unit_stop(counter));
    int stopped = read_count("pcnt_stop"); wait_ms(20);
    check("pcnt_stop", "stopped_holds_count", stopped == read_count("pcnt_stop"));
    OK("pcnt_clear", pcnt_unit_clear_count(counter));
    check("pcnt_clear", "reset_zero", read_count("pcnt_clear") == 0);
    OK("pcnt_resume", pcnt_unit_start(counter));
    phase("ledc_fade");
    fade_events = 0;
    int64_t submit = esp_timer_get_time();
    OK("ledc_fade", ledc_set_fade_with_step(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 768, 64, 2));
    OK("ledc_fade", ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_NO_WAIT));
    check("ledc_fade", "not_synchronous_callback", fade_events == 0);
    wait_ms(60);
    check("ledc_fade", "single_fade_irq", fade_events == 1);
    check("ledc_fade", "fade_not_early", fade_us - submit >= 16000);
    check("ledc_fade", "target_duty", ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0) == 768);
    printf("PULSE_FADE submit_us=%" PRId64 " callback_us=%" PRId64 " events=%u\n", submit, fade_us, fade_events);
    phase("ledc_pause");
    OK("ledc_pause", ledc_timer_pause(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0));
    stopped = read_count("ledc_pause"); wait_ms(20);
    check("ledc_pause", "no_edges_while_paused", stopped == read_count("ledc_pause"));
    OK("ledc_resume", ledc_timer_resume(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0));
    phase("ledc_gate");
    REG_CLR_BIT(SYSTEM_PERIP_CLK_EN0_REG, SYSTEM_LEDC_CLK_EN);
    stopped = read_count("ledc_gate"); wait_ms(20);
    check("ledc_gate", "no_edges_while_gated", stopped == read_count("ledc_gate"));
    REG_SET_BIT(SYSTEM_PERIP_CLK_EN0_REG, SYSTEM_LEDC_CLK_EN);
    counter_window("ledc_gate_resume", true);
    timer.clk_cfg = LEDC_USE_XTAL_CLK;
    OK("ledc_xtal", ledc_timer_config(&timer));
    counter_window("ledc_xtal_count", true);
    OK("ledc_stop", ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0));
    stopped = read_count("ledc_stop"); wait_ms(20);
    check("ledc_stop", "no_edges_after_stop", stopped == read_count("ledc_stop"));
    phase("ledc_reset");
    REG_SET_BIT(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_LEDC_RST);
    REG_CLR_BIT(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_LEDC_RST);
    timer.clk_cfg = LEDC_USE_APB_CLK;
    OK("ledc_reset", ledc_timer_config(&timer));
    OK("ledc_reset", ledc_channel_config(&channel));
    counter_window("ledc_reset_reconfigured", true);
    OK("cleanup", ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0));
    ledc_fade_func_uninstall();
    OK("cleanup", pcnt_unit_stop(counter));
    OK("cleanup", pcnt_unit_disable(counter));
    OK("cleanup", pcnt_del_channel(edge_channel));
    OK("cleanup", pcnt_del_unit(counter));
    dump_events("ledc_pcnt");
}

static bool cap_cb(mcpwm_cap_channel_handle_t ch, const mcpwm_capture_event_data_t *e, void *ctx)
{
    (void)ch; (void)ctx; capture_events++; record(e->cap_edge == MCPWM_CAP_EDGE_POS ? 3 : 4, e->cap_value); return false;
}
static bool timer_empty(mcpwm_timer_handle_t t, const mcpwm_timer_event_data_t *e, void *ctx)
{ (void)t; (void)ctx; record(5, e->count_value); return false; }
static bool timer_stop(mcpwm_timer_handle_t t, const mcpwm_timer_event_data_t *e, void *ctx)
{ (void)t; (void)ctx; stop_events++; record(6, e->count_value); return false; }
static bool compare_cb(mcpwm_cmpr_handle_t c, const mcpwm_compare_event_data_t *e, void *ctx)
{ (void)c; (void)ctx; record(7, e->compare_ticks); return false; }
static bool brake_cb(mcpwm_oper_handle_t o, const mcpwm_brake_event_data_t *e, void *ctx)
{ (void)o; (void)e; (void)ctx; brake_events++; record(8, 0); return false; }
static bool fault_enter(mcpwm_fault_handle_t f, const mcpwm_fault_event_data_t *e, void *ctx)
{ (void)f; (void)e; (void)ctx; record(9, 1); return false; }
static bool fault_exit(mcpwm_fault_handle_t f, const mcpwm_fault_event_data_t *e, void *ctx)
{ (void)f; (void)e; (void)ctx; record(9, 0); return false; }
static void __attribute__((unused)) mcpwm_case(mcpwm_timer_count_mode_t mode, const char *name)
{
    phase(name);
    mcpwm_timer_handle_t timer;
    mcpwm_oper_handle_t oper;
    mcpwm_cmpr_handle_t cmp;
    mcpwm_gen_handle_t a, b;
    mcpwm_cap_timer_handle_t ct;
    mcpwm_cap_channel_handle_t cap;
    mcpwm_sync_handle_t sync;
    mcpwm_fault_handle_t fault;
    mcpwm_timer_config_t tc = {.group_id = 0, .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000, .period_ticks = 1000, .count_mode = mode};
    mcpwm_operator_config_t oc = {.group_id = 0};
    mcpwm_comparator_config_t cc = {.flags.update_cmp_on_tez = true};
    mcpwm_generator_config_t gc = {.gen_gpio_num = 6};
    OK(name, mcpwm_new_timer(&tc, &timer));
    OK(name, mcpwm_new_operator(&oc, &oper));
    OK(name, mcpwm_operator_connect_timer(oper, timer));
    OK(name, mcpwm_new_comparator(oper, &cc, &cmp));
    OK(name, mcpwm_comparator_set_compare_value(cmp, 250));
    OK(name, mcpwm_new_generator(oper, &gc, &a));
    gc.gen_gpio_num = 7;
    OK(name, mcpwm_new_generator(oper, &gc, &b));
    mcpwm_timer_direction_t dir = mode == MCPWM_TIMER_COUNT_MODE_DOWN ? MCPWM_TIMER_DIRECTION_DOWN : MCPWM_TIMER_DIRECTION_UP;
    mcpwm_timer_event_t start = mode == MCPWM_TIMER_COUNT_MODE_DOWN ? MCPWM_TIMER_EVENT_FULL : MCPWM_TIMER_EVENT_EMPTY;
    OK(name, mcpwm_generator_set_action_on_timer_event(a, MCPWM_GEN_TIMER_EVENT_ACTION(dir, start, MCPWM_GEN_ACTION_HIGH)));
    OK(name, mcpwm_generator_set_action_on_compare_event(a, MCPWM_GEN_COMPARE_EVENT_ACTION(dir, cmp, MCPWM_GEN_ACTION_LOW)));
    if (mode == MCPWM_TIMER_COUNT_MODE_UP_DOWN) {
        OK(name, mcpwm_generator_set_action_on_compare_event(a, MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_DOWN, cmp, MCPWM_GEN_ACTION_HIGH)));
    }
    mcpwm_dead_time_config_t dt = {.posedge_delay_ticks = 10};
    OK(name, mcpwm_generator_set_dead_time(a, a, &dt));
    dt = (mcpwm_dead_time_config_t){.negedge_delay_ticks = 10, .flags.invert_output = true};
    OK(name, mcpwm_generator_set_dead_time(a, b, &dt));
    mcpwm_timer_event_callbacks_t tcb = {.on_empty = timer_empty, .on_stop = timer_stop};
    OK(name, mcpwm_timer_register_event_callbacks(timer, &tcb, NULL));
    mcpwm_comparator_event_callbacks_t ccb = {.on_reach = compare_cb};
    OK(name, mcpwm_comparator_register_event_callbacks(cmp, &ccb, NULL));
    mcpwm_capture_timer_config_t ctc = {.group_id = 0, .clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT};
    mcpwm_capture_channel_config_t capc = {.gpio_num = 8, .prescale = 1, .flags.pos_edge = true, .flags.neg_edge = true};
    OK(name, mcpwm_new_capture_timer(&ctc, &ct));
    uint32_t cap_hz;
    OK(name, mcpwm_capture_timer_get_resolution(ct, &cap_hz));
    printf("PULSE_CAPTURE_CONFIG phase=%s resolution_hz=%" PRIu32 "\n", name, cap_hz);
    OK(name, mcpwm_new_capture_channel(ct, &capc, &cap));
    mcpwm_capture_event_callbacks_t capcb = {.on_cap = cap_cb};
    OK(name, mcpwm_capture_channel_register_event_callbacks(cap, &capcb, NULL));
    OK(name, mcpwm_capture_timer_enable(ct));
    OK(name, mcpwm_capture_timer_start(ct));
    OK(name, mcpwm_capture_channel_enable(cap));
    OK(name, gpio_set_direction(12, GPIO_MODE_OUTPUT));
    OK(name, gpio_set_direction(13, GPIO_MODE_OUTPUT));
    OK(name, gpio_set_level(12, 0));
    OK(name, gpio_set_level(13, 0));
    mcpwm_gpio_sync_src_config_t sc = {.group_id = 0, .gpio_num = 9};
    OK(name, mcpwm_new_gpio_sync_src(&sc, &sync));
    mcpwm_timer_sync_phase_config_t sp = {.sync_src = sync, .count_value = 100, .direction = dir};
    OK(name, mcpwm_timer_set_phase_on_sync(timer, &sp));
    mcpwm_gpio_fault_config_t fc = {.group_id = 0, .gpio_num = 10, .flags.active_level = true};
    OK(name, mcpwm_new_gpio_fault(&fc, &fault));
    mcpwm_fault_event_callbacks_t fcb = {.on_fault_enter = fault_enter, .on_fault_exit = fault_exit};
    OK(name, mcpwm_fault_register_event_callbacks(fault, &fcb, NULL));
    mcpwm_operator_event_callbacks_t ocb = {.on_brake_ost = brake_cb, .on_brake_cbc = brake_cb};
    OK(name, mcpwm_operator_register_event_callbacks(oper, &ocb, NULL));
    mcpwm_brake_config_t bc = {.fault = fault, .brake_mode = MCPWM_OPER_BRAKE_MODE_OST};
    OK(name, mcpwm_operator_set_brake_on_fault(oper, &bc));
    OK(name, mcpwm_generator_set_action_on_brake_event(a, MCPWM_GEN_BRAKE_EVENT_ACTION(dir, MCPWM_OPER_BRAKE_MODE_OST, MCPWM_GEN_ACTION_LOW)));
    capture_events = stop_events = brake_events = 0;
    OK(name, mcpwm_timer_enable(timer));
    OK(name, mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP));
    wait_ms(20);
    check(name, "physical_capture", CAPTURE_CONNECTED ? capture_events > 10 : capture_events == 0);
    OK("mcpwm_compare_update", mcpwm_comparator_set_compare_value(cmp, 400));
    wait_ms(5);
    phase("mcpwm_gpio_sync");
    printf("PULSE_SYNC submit_us=%" PRId64 " target_count=100\n", esp_timer_get_time());
    OK(name, gpio_set_level(12, 1)); wait_ms(2); OK(name, gpio_set_level(12, 0));
    phase("mcpwm_fault_ost");
    OK(name, gpio_set_level(13, 1)); wait_ms(5);
    check(name, "physical_fault_brake_irq", CAPTURE_CONNECTED ? brake_events > 0 : brake_events == 0);
    OK(name, gpio_set_level(13, 0)); wait_ms(2);
    OK(name, mcpwm_operator_recover_from_fault(oper, fault));
    wait_ms(5);
    phase("mcpwm_fault_cbc");
    bc.brake_mode = MCPWM_OPER_BRAKE_MODE_CBC;
    bc.flags.cbc_recover_on_tez = true;
    OK(name, mcpwm_operator_set_brake_on_fault(oper, &bc));
    OK(name, mcpwm_generator_set_action_on_brake_event(a, MCPWM_GEN_BRAKE_EVENT_ACTION(dir, MCPWM_OPER_BRAKE_MODE_CBC, MCPWM_GEN_ACTION_LOW)));
    unsigned brakes_before = brake_events;
    OK(name, gpio_set_level(13, 1)); wait_ms(3);
    OK(name, gpio_set_level(13, 0)); wait_ms(3);
    check(name, "cbc_physical_fault_irq", CAPTURE_CONNECTED ? brake_events > brakes_before : brake_events == brakes_before);
    unsigned capture_before = capture_events;
    wait_ms(4);
    check(name, "cbc_recovers_capture", CAPTURE_CONNECTED ? capture_events > capture_before : capture_events == capture_before);
    phase("mcpwm_gate");
    REG_CLR_BIT(SYSTEM_PERIP_CLK_EN0_REG, SYSTEM_PWM0_CLK_EN);
    /* Gate publication itself can create a real resolved falling edge.
     * Drain that boundary callback before measuring a held-clock interval. */
    wait_ms(1);
    capture_before = capture_events;
    wait_ms(5);
    check(name, "no_capture_when_gated", capture_events == capture_before);
    REG_SET_BIT(SYSTEM_PERIP_CLK_EN0_REG, SYSTEM_PWM0_CLK_EN);
    wait_ms(5);
    phase("mcpwm_stop");
    OK(name, mcpwm_timer_start_stop(timer, MCPWM_TIMER_STOP_EMPTY)); wait_ms(5);
    check(name, "stop_irq", stop_events == 1);
    unsigned captures = capture_events; wait_ms(5);
    check(name, "no_capture_after_stop", captures == capture_events);
    OK(name, mcpwm_timer_disable(timer));
    OK(name, mcpwm_capture_channel_disable(cap));
    OK(name, mcpwm_capture_timer_stop(ct));
    OK(name, mcpwm_capture_timer_disable(ct));
    OK(name, mcpwm_del_capture_channel(cap));
    OK(name, mcpwm_del_capture_timer(ct));
    OK(name, mcpwm_del_sync_src(sync));
    OK(name, mcpwm_del_generator(b));
    OK(name, mcpwm_del_generator(a));
    OK(name, mcpwm_del_comparator(cmp));
    OK(name, mcpwm_del_operator(oper));
    OK(name, mcpwm_del_fault(fault));
    OK(name, mcpwm_del_timer(timer));
    dump_events(name);
    phase("mcpwm_reset");
    REG_SET_BIT(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_PWM0_RST);
    REG_CLR_BIT(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_PWM0_RST);
}
static void __attribute__((unused)) sdm_cases(void)
{
    phase("sdm_allocation");
    sdm_channel_handle_t channels[SDM_CAPS_GET(CHANS_PER_INST)] = {0};
    static const gpio_num_t pads[SDM_CAPS_GET(CHANS_PER_INST)] =
        {11, 16, 17, 18, 21, 38, 39, 40};
    for (unsigned i = 0; i < SDM_CAPS_GET(CHANS_PER_INST); i++) {
        sdm_config_t config = {.gpio_num = pads[i],
            .clk_src = SDM_CLK_SRC_DEFAULT, .sample_rate_hz = 312500};
        OK("sdm_allocation", sdm_new_channel(&config, &channels[i]));
        printf("PULSE_SDM_CHANNEL index=%u gpio=%d\n", i, config.gpio_num);
    }
    sdm_channel_handle_t extra = NULL;
    sdm_config_t config = {.gpio_num = 41, .clk_src = SDM_CLK_SRC_DEFAULT, .sample_rate_hz = 312500};
    esp_err_t err = sdm_new_channel(&config, &extra);
    check("sdm_exhaustion", "no_free_channel", err == ESP_ERR_NOT_FOUND && extra == NULL);
    if (extra) { OK("cleanup", sdm_del_channel(extra)); }
    OK("sdm_enable", sdm_channel_enable(channels[0]));
    const int densities[] = {-128, -90, 0, 90, 127};
#if CONFIG_PULSE_NATIVE_ADC_PROVIDER_READY
    adc_oneshot_unit_handle_t adc;
    adc_oneshot_unit_init_cfg_t ac = {.unit_id = ADC_UNIT_1};
    adc_oneshot_chan_cfg_t cc = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    OK("sdm_adc", adc_oneshot_new_unit(&ac, &adc));
    OK("sdm_adc", adc_oneshot_config_channel(adc, ADC_CHANNEL_0, &cc));
#else
    printf("PULSE_UNAVAILABLE stage=sdm_adc reason=adc_provider_not_declared_ready\n");
#endif
    for (unsigned i = 0; i < sizeof(densities) / sizeof(densities[0]); i++) {
        phase("sdm_density");
        int64_t submit = esp_timer_get_time();
        OK("sdm_density", sdm_channel_set_pulse_density(channels[0], densities[i]));
        wait_ms(30);
        printf("PULSE_SDM density=%d sample_rate_hz=312500 submit_us=%" PRId64 " end_us=%" PRId64 "\n",
               densities[i], submit, esp_timer_get_time());
#if CONFIG_PULSE_NATIVE_ADC_PROVIDER_READY
        for (unsigned n = 0; n < 16; n++) {
            int raw;
            int64_t before = esp_timer_get_time();
            OK("sdm_adc", adc_oneshot_read(adc, ADC_CHANNEL_0, &raw));
            printf("PULSE_ADC density=%d index=%u raw=%d before_us=%" PRId64 " after_us=%" PRId64 "\n",
                   densities[i], n, raw, before, esp_timer_get_time());
            wait_ms(1);
        }
#endif
    }
#if CONFIG_PULSE_NATIVE_ADC_PROVIDER_READY
    OK("cleanup", adc_oneshot_del_unit(adc));
#endif
    phase("sdm_api_disable");
    OK("sdm_api_disable", sdm_channel_disable(channels[0]));
    printf("PULSE_LIMIT sdm_channel_disable_changes_driver_state_not_hardware_clock\n");
    wait_ms(10);
    OK("sdm_resume", sdm_channel_enable(channels[0]));
    OK("sdm_resume", sdm_channel_set_pulse_density(channels[0], 0));
    wait_ms(10);
    OK("cleanup", sdm_channel_disable(channels[0]));
    for (unsigned i = 0; i < SDM_CAPS_GET(CHANS_PER_INST); i++) { OK("cleanup", sdm_del_channel(channels[i])); }
    OK("sdm_reallocation", sdm_new_channel(&config, &extra));
    OK("cleanup", sdm_del_channel(extra));
    printf("PULSE_UNAVAILABLE stage=sdm_silicon_sequence reason=second_order_sequence_reference_not_qualified\n");
}
void app_main(void)
{
    printf("PULSE_BOOT wiring=%s idf=6.1 timestamp_unit=guest_us\n", WIRING);
    printf("PULSE_LIMIT guest_ISR_times_are_not_electrical_edge_timestamps\n");
#if CONFIG_PULSE_NATIVE_ALL || CONFIG_PULSE_NATIVE_LEDC_PCNT
    ledc_pcnt();
#endif
#if CONFIG_PULSE_NATIVE_ALL || CONFIG_PULSE_NATIVE_MCPWM
    mcpwm_case(MCPWM_TIMER_COUNT_MODE_UP, "mcpwm_up");
    mcpwm_case(MCPWM_TIMER_COUNT_MODE_DOWN, "mcpwm_down");
    mcpwm_case(MCPWM_TIMER_COUNT_MODE_UP_DOWN, "mcpwm_up_down");
#endif
#if CONFIG_PULSE_NATIVE_ALL || CONFIG_PULSE_NATIVE_SDM
    sdm_cases();
#endif
    printf("PULSE_DONE wiring=%s failures=%u time_us=%" PRId64 "\n", WIRING, failures, esp_timer_get_time());
    fflush(stdout);
    for (;;) { wait_ms(1000); }
}
