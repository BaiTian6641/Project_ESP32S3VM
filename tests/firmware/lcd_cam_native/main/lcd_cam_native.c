#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_memory_utils.h"
#include "esp_psram.h"
#include "img_converters.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define W 64
#define H 48
#define BYTES (W * H * 2)
static const int data_pins[16] = {4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21};
static uint16_t pixel(unsigned x, unsigned y, unsigned frame)
{
    return (uint16_t)((((x + frame * 3) & 31) << 11) | (((y * 3 + frame) & 63) << 5) | ((x ^ y ^ frame) & 31));
}
static uint32_t hash_bytes(const void *buffer, size_t size)
{
    const uint8_t *p = buffer;
    uint32_t hash = 2166136261u;
    while (size--) hash = (hash ^ *p++) * 16777619u;
    return hash;
}
static void fill(uint16_t *buffer, unsigned frame)
{
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x) buffer[y * W + x] = pixel(x, y, frame);
}
static bool check(const char *operation, esp_err_t result)
{
    printf("LCDCAM API op=%s err=%s code=%ld\n", operation, esp_err_to_name(result), (long)result);
    return result == ESP_OK;
}

#if CONFIG_LCD_CAM_NATIVE_RGB || CONFIG_LCD_CAM_NATIVE_CAMERA
static bool requested_psram_ready(void)
{
#if CONFIG_LCD_CAM_NATIVE_PSRAM
#if CONFIG_SPIRAM
    bool initialized = esp_psram_is_initialized();
    size_t capacity = esp_psram_get_size();
    printf("LCDCAM PSRAM_REQUEST initialized=%u capacity=%u\n",
           initialized, (unsigned)capacity);
    if (!initialized || capacity != 8u * 1024 * 1024) {
        puts("LCDCAM ERROR requested_external_memory_not_backed_8m");
        return false;
    }
#else
    puts("LCDCAM ERROR requested_external_memory_without_spiram");
    return false;
#endif
#endif
    return true;
}
#endif

#if CONFIG_LCD_CAM_NATIVE_I80
static SemaphoreHandle_t completed;
static volatile unsigned callbacks;
static volatile int64_t completion_us[3];
static bool color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *ctx)
{
    (void)io; (void)event; (void)ctx;
    BaseType_t wake = pdFALSE;
    if (callbacks < 3) completion_us[callbacks] = esp_timer_get_time();
    ++callbacks;
    xSemaphoreGiveFromISR(completed, &wake);
    return wake == pdTRUE;
}
static void run_i80(void)
{
    completed = xSemaphoreCreateCounting(4, 0);
    if (!completed) { puts("LCDCAM ERROR semaphore"); return; }
    esp_lcd_i80_bus_handle_t bus;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    esp_lcd_i80_bus_config_t bc = {.dc_gpio_num=2, .wr_gpio_num=1,
        .clk_src=LCD_CLK_SRC_DEFAULT, .bus_width=8, .max_transfer_bytes=BYTES, .dma_burst_size=16};
    for (int i=0;i<8;i++) bc.data_gpio_nums[i]=data_pins[i];
    if (!check("i80_bus", esp_lcd_new_i80_bus(&bc,&bus))) return;
    esp_lcd_panel_io_i80_config_t ic = {.cs_gpio_num=3, .pclk_hz=1000000,
        .trans_queue_depth=4, .on_color_trans_done=color_done,
        .lcd_cmd_bits=8, .lcd_param_bits=8,
        .dc_levels={.dc_idle_level=0,.dc_cmd_level=0,.dc_dummy_level=0,.dc_data_level=1},
        .flags={.swap_color_bytes=1}};
    if (!check("i80_io",esp_lcd_new_panel_io_i80(bus,&ic,&io))) return;
    esp_lcd_panel_dev_config_t pc = {.reset_gpio_num=12,.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB,.bits_per_pixel=16};
    if (!check("st7789",esp_lcd_new_panel_st7789(io,&pc,&panel))) return;
    if (!check("panel_reset",esp_lcd_panel_reset(panel)) || !check("panel_init",esp_lcd_panel_init(panel)) ||
        !check("display_on",esp_lcd_panel_disp_on_off(panel,true))) return;
    uint16_t *buffers[3]={0};
    for (unsigned frame=0;frame<3;frame++) {
        buffers[frame]=esp_lcd_i80_alloc_draw_buffer(io,BYTES,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
        if (!buffers[frame]) { puts("LCDCAM ERROR internalRAM allocation"); goto release; }
        fill(buffers[frame],frame);
        printf("LCDCAM I80_SUBMIT frame=%u width=%d height=%d bytes=%d hash=%08lx wire=rgb565-be callbacks_before=%u time_us=%lld\n",
            frame,W,H,BYTES,(unsigned long)hash_bytes(buffers[frame],BYTES),callbacks,(long long)esp_timer_get_time());
        /* The panel draw establishes CASET/RASET once. Subsequent ordinary
         * panel IO RAMWR transactions reuse that window, avoiding blocking
         * tx_param calls which would otherwise serialize the queued colors. */
        esp_err_t submitted = frame == 0 ?
            esp_lcd_panel_draw_bitmap(panel,0,0,W,H,buffers[frame]) :
            esp_lcd_panel_io_tx_color(io,0x2c,buffers[frame],BYTES);
        if (!check(frame == 0 ? "draw_bitmap" : "queued_ramwr",submitted)) goto release;
    }
    for (unsigned frame=0;frame<3;frame++) {
        if (!xSemaphoreTake(completed,pdMS_TO_TICKS(5000))) {
            printf("LCDCAM ERROR i80_completion_timeout frame=%u callbacks=%u\n",frame,callbacks);
            /* DMA ownership is still outstanding: do not free submitted buffers. */
            return;
        }
        printf("LCDCAM I80_DONE frame=%u callbacks=%u time_us=%lld callback_us=%lld\n",frame,callbacks,(long long)esp_timer_get_time(),(long long)completion_us[frame]);
    }
    check("panel_delete",esp_lcd_panel_del(panel));
    check("io_delete",esp_lcd_panel_io_del(io));
    check("bus_delete",esp_lcd_del_i80_bus(bus));
release:
    /* Normal completion or an API failure; drain queued IO before releasing DMA storage. */
    if (callbacks < 3) { puts("LCDCAM ERROR queued_storage_retained"); return; }
    for (unsigned i=0;i<3;i++) heap_caps_free(buffers[i]);
}
#endif

#if CONFIG_LCD_CAM_NATIVE_RGB
#define RGB_BUS CONFIG_LCD_CAM_NATIVE_RGB_BUS_WIDTH
#define RGB_BITS CONFIG_LCD_CAM_NATIVE_RGB_PIXEL_BITS
#define RGB_PIXEL_BYTES (RGB_BITS / 8)
#define RGB_STRIDE (W * RGB_PIXEL_BYTES)
#define RGB_BYTES (RGB_STRIDE * H)
_Static_assert(RGB_BUS == 8 || RGB_BUS == 16, "RGB bus must be 8 or 16");
_Static_assert(RGB_BITS == 8 || RGB_BITS == 16 || RGB_BITS == 24, "RGB pixel width");
_Static_assert(RGB_BUS == 8 || RGB_BITS != 8, "8-bit pixels require 8-wire bus");
_Static_assert((W * RGB_BITS) % RGB_BUS == 0, "Whole wire cycles per line");
static void rgb_pixel_bytes(uint8_t *out, unsigned x, unsigned y, unsigned frame)
{
#if CONFIG_LCD_CAM_NATIVE_RGB_RGB332
    out[0] = (uint8_t)((((x+frame*3)&7)<<5) | (((y*3+frame)&7)<<2) | ((x^y^frame)&3));
#elif CONFIG_LCD_CAM_NATIVE_RGB_RGB888
    out[0] = (uint8_t)(x+frame*3);
    out[1] = (uint8_t)(y*3+frame);
    out[2] = (uint8_t)(x^y^frame);
#else
    uint16_t value = pixel(x,y,frame);
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
#endif
}
static void rgb_fill(uint8_t *buffer, unsigned frame)
{
    for (unsigned y=0;y<H;++y)
        for (unsigned x=0;x<W;++x)
            rgb_pixel_bytes(buffer+y*RGB_STRIDE+x*RGB_PIXEL_BYTES,x,y,frame);
}
static void rgb_pattern_log(const uint8_t *buffer, unsigned frame)
{
    printf("LCDCAM RGB_PAYLOAD frame=%u bus_width=%d bits_per_pixel=%d width=%d height=%d stride=%d bytes=%d hash=%08lx head=%02x%02x%02x tail=%02x%02x%02x time_us=%lld\n",
        frame,RGB_BUS,RGB_BITS,W,H,RGB_STRIDE,RGB_BYTES,
        (unsigned long)hash_bytes(buffer,RGB_BYTES),buffer[0],buffer[1],buffer[2],
        buffer[RGB_BYTES-3],buffer[RGB_BYTES-2],buffer[RGB_BYTES-1],(long long)esp_timer_get_time());
}
#if CONFIG_LCD_CAM_NATIVE_PSRAM
static bool rgb_external_buffer(const char *name, uint8_t *buffer, size_t bytes)
{
    bool external=buffer && esp_ptr_external_ram(buffer) &&
        esp_ptr_external_ram(buffer+bytes-1);
    size_t capacity=buffer?heap_caps_get_allocated_size(buffer):0;
    printf("LCDCAM RGB_EXTERNAL_BUFFER name=%s alias=%p bytes=%u allocated=%u external=%u mod64=%u\n",
        name,(void*)buffer,(unsigned)bytes,(unsigned)capacity,external,
        (unsigned)((uintptr_t)buffer%64));
    if(!external || capacity<bytes){puts("LCDCAM ERROR rgb_external_buffer");return false;}
    return true;
}
static bool rgb_cache_sync(const char *operation, uint8_t *buffer, bool readback)
{
    int flags=ESP_CACHE_MSYNC_FLAG_TYPE_DATA|ESP_CACHE_MSYNC_FLAG_UNALIGNED;
    flags|=readback?(ESP_CACHE_MSYNC_FLAG_DIR_M2C|ESP_CACHE_MSYNC_FLAG_INVALIDATE):
        ESP_CACHE_MSYNC_FLAG_DIR_C2M;
    if(!check(operation,esp_cache_msync(buffer,RGB_BYTES,flags)))return false;
    printf("LCDCAM RGB_CACHE op=%s alias=%p bytes=%d direction=%s time_us=%lld\n",
        operation,(void*)buffer,RGB_BYTES,readback?"M2C_INV":"C2M_WB",(long long)esp_timer_get_time());
    return true;
}
#endif
#if !CONFIG_LCD_CAM_NATIVE_RGB_BOUNCE
static bool rgb_writeback(const char *operation, uint8_t *buffer)
{
#if CONFIG_LCD_CAM_NATIVE_PSRAM
    return rgb_cache_sync(operation,buffer,false);
#else
    (void)operation;(void)buffer;return true;
#endif
}
#endif
static SemaphoreHandle_t vsync_event;
static portMUX_TYPE rgb_events_lock=portMUX_INITIALIZER_UNLOCKED;
static volatile unsigned vsync_count, reusable_count, draw_count, bounce_count;
static volatile int64_t vsync_us, reusable_us;
static volatile uint32_t bounce_hash=2166136261u, bounce_frame_hash;
static volatile unsigned bounce_frame_bytes, bounce_payload_bytes, bounce_frames;
static volatile unsigned bounce_errors, bounce_isr_count, bounce_external_isr_bytes;
static volatile uintptr_t bounce_buffer_alias[2];
static bool bounce_sequence_valid;
#if CONFIG_LCD_CAM_NATIVE_PSRAM
static const volatile uint8_t *bounce_source;
#endif
#define RGB_BOUNCE_BYTES (W * 4 * RGB_PIXEL_BYTES)
static bool vsync_done(esp_lcd_panel_handle_t panel,const esp_lcd_rgb_panel_event_data_t *e,void *ctx)
{
    (void)panel;(void)e;(void)ctx;BaseType_t wake=pdFALSE;
    portENTER_CRITICAL_ISR(&rgb_events_lock);
    ++vsync_count;vsync_us=esp_timer_get_time();
    portEXIT_CRITICAL_ISR(&rgb_events_lock);
    xSemaphoreGiveFromISR(vsync_event,&wake);return wake==pdTRUE;
}
static bool reusable(esp_lcd_panel_handle_t p,const esp_lcd_rgb_panel_event_data_t *e,void *c)
{
    (void)p;(void)e;(void)c;
    portENTER_CRITICAL_SAFE(&rgb_events_lock);
    ++reusable_count;reusable_us=esp_timer_get_time();
    portEXIT_CRITICAL_SAFE(&rgb_events_lock);return false;
}
static bool drawn(esp_lcd_panel_handle_t p,const esp_lcd_rgb_panel_event_data_t *e,void *c)
{
    (void)p;(void)e;(void)c;
    portENTER_CRITICAL_SAFE(&rgb_events_lock);++draw_count;
    portEXIT_CRITICAL_SAFE(&rgb_events_lock);return false;
}
static bool bounce(esp_lcd_panel_handle_t p,void *buffer,int pos,int bytes,void *ctx)
{
    (void)p;(void)ctx;uint8_t *out=buffer;
    bool in_isr=xPortInIsrContext();
    bool valid=pos>=0 && pos<W*H && bytes>0 && bytes<=RGB_BOUNCE_BYTES &&
        bytes%RGB_PIXEL_BYTES==0;
    size_t offset=valid?(size_t)pos*RGB_PIXEL_BYTES:0;
    valid=valid && (size_t)bytes<=RGB_BYTES-offset;
    if(valid)valid=esp_ptr_internal(out) && esp_ptr_dma_capable(out) &&
        esp_ptr_internal(out+bytes-1) && esp_ptr_dma_capable(out+bytes-1);
#if CONFIG_LCD_CAM_NATIVE_PSRAM
    valid=valid && bounce_source;
#endif
    if(!valid){
        portENTER_CRITICAL_SAFE(&rgb_events_lock);
        ++bounce_errors;++bounce_count;if(in_isr)++bounce_isr_count;
        portEXIT_CRITICAL_SAFE(&rgb_events_lock);return false;
    }
#if CONFIG_LCD_CAM_NATIVE_PSRAM
    /* Source remains immutable. These volatile reads consume actual PSRAM
     * bytes, including the first backing read after the task's M2C invalidate. */
    for(int i=0;i<bytes;++i)out[i]=bounce_source[offset+(size_t)i];
#else
    /* Preserve the original internal-only formula path for every bus/bpp. */
    for(int i=0;i<bytes/RGB_PIXEL_BYTES;i++) {
        unsigned n=(unsigned)(pos+i);
        rgb_pixel_bytes(out+i*RGB_PIXEL_BYTES,n%W,n/W,0);
    }
#endif
    portENTER_CRITICAL_SAFE(&rgb_events_lock);
    uintptr_t alias=(uintptr_t)out;
    if(!bounce_buffer_alias[0])bounce_buffer_alias[0]=alias;
    else if(alias!=bounce_buffer_alias[0]){
        if(!bounce_buffer_alias[1])bounce_buffer_alias[1]=alias;
        else if(alias!=bounce_buffer_alias[1])++bounce_errors;
    }
    if(pos==0){bounce_hash=2166136261u;bounce_frame_bytes=0;bounce_sequence_valid=true;}
    if(offset!=bounce_frame_bytes)bounce_sequence_valid=false;
    uint32_t hash=bounce_hash;
    for(int i=0;i<bytes;++i)hash=(hash^out[i])*16777619u;
    bounce_hash=hash;
    bounce_frame_bytes+=(unsigned)bytes;
    if(bounce_frame_bytes==RGB_BYTES && bounce_sequence_valid){
        bounce_frame_hash=bounce_hash;bounce_payload_bytes=bounce_frame_bytes;++bounce_frames;
    }
    ++bounce_count;
    if(in_isr){
        ++bounce_isr_count;
#if CONFIG_LCD_CAM_NATIVE_PSRAM
        bounce_external_isr_bytes+=(unsigned)bytes;
#endif
    }
    portEXIT_CRITICAL_SAFE(&rgb_events_lock);return false;
}
static bool wait_vsync(unsigned previous)
{
    TickType_t started=xTaskGetTickCount(),timeout=pdMS_TO_TICKS(5000);
    /* Queued semaphore tokens are not proof of a new VSYNC after a request. */
    while(vsync_count==previous){
        TickType_t elapsed=xTaskGetTickCount()-started;
        if(elapsed>=timeout || !xSemaphoreTake(vsync_event,timeout-elapsed)){
            puts("LCDCAM ERROR vsync_timeout");return false;
        }
    }
    portENTER_CRITICAL(&rgb_events_lock);
    unsigned v=vsync_count,r=reusable_count,d=draw_count,b=bounce_count;
    unsigned payload=bounce_payload_bytes,frames=bounce_frames,errors=bounce_errors;
    unsigned isr=bounce_isr_count,external_bytes=bounce_external_isr_bytes;
    uint32_t payload_hash=bounce_frame_hash;
    uintptr_t bb0=bounce_buffer_alias[0],bb1=bounce_buffer_alias[1];
    int64_t v_us=vsync_us,r_us=reusable_us;
    portEXIT_CRITICAL(&rgb_events_lock);
    printf("LCDCAM RGB_VSYNC count=%u reusable=%u draw_copy=%u bounce=%u callback_us=%lld reusable_us=%lld time_us=%lld\n",
        v,r,d,b,(long long)v_us,(long long)r_us,(long long)esp_timer_get_time());
    if(b){
        printf("LCDCAM RGB_BOUNCE_STORAGE bb0=%p bb1=%p distinct=%u callback_count=%u isr_count=%u external_isr_bytes=%u errors=%u time_us=%lld\n",
            (void*)bb0,(void*)bb1,bb0 && bb1 && bb0!=bb1,b,isr,external_bytes,errors,
            (long long)esp_timer_get_time());
    }
    if(payload)
        printf("LCDCAM RGB_BOUNCE_PAYLOAD bus_width=%d bits_per_pixel=%d width=%d height=%d stride=%d bytes=%u hash=%08lx complete_frames=%u time_us=%lld\n",
            RGB_BUS,RGB_BITS,W,H,RGB_STRIDE,payload,(unsigned long)payload_hash,frames,(long long)esp_timer_get_time());
    if(errors){puts("LCDCAM ERROR rgb_bounce_storage_or_bounds");return false;}
    return true;
}
#if CONFIG_LCD_CAM_NATIVE_RGB_DOUBLE
static bool rgb_retention_log(uint8_t *buffer, const uint8_t *retained, uint32_t expected,
    const char *phase)
{
#if CONFIG_LCD_CAM_NATIVE_PSRAM
    /* DMA only reads this immutable buffer; invalidate clean CPU lines before
     * examining the committed backing, never discard a CPU-owned dirty fill. */
    if(!rgb_cache_sync("rgb_old_fb_readback",buffer,true))return false;
#endif
    uint32_t actual=2166136261u;
    unsigned mismatches=0;
    for(unsigned i=0;i<RGB_BYTES;++i){
        uint8_t value=buffer[i];
        actual=(actual^value)*16777619u;
        if(value!=retained[i])++mismatches;
    }
    portENTER_CRITICAL(&rgb_events_lock);
    unsigned v=vsync_count,r=reusable_count;
    int64_t v_us=vsync_us,r_us=reusable_us;
    portEXIT_CRITICAL(&rgb_events_lock);
    printf("LCDCAM RGB_RETENTION phase=%s alias=%p bytes=%d before_hash=%08lx after_hash=%08lx mismatches=%u bytes_equal=%u head=%02x%02x%02x tail=%02x%02x%02x vsync=%u reusable=%u vsync_us=%lld reusable_us=%lld time_us=%lld\n",
        phase,(void*)buffer,RGB_BYTES,(unsigned long)expected,(unsigned long)actual,mismatches,
        mismatches==0,buffer[0],buffer[1],buffer[2],buffer[RGB_BYTES-3],buffer[RGB_BYTES-2],
        buffer[RGB_BYTES-1],v,r,(long long)v_us,(long long)r_us,(long long)esp_timer_get_time());
    if(mismatches || actual!=expected){puts("LCDCAM ERROR rgb_old_framebuffer_changed");return false;}
    return true;
}
#endif
static void run_rgb(void)
{
    if(!requested_psram_ready())return;
    esp_lcd_panel_handle_t panel=NULL;
#if CONFIG_LCD_CAM_NATIVE_RGB_BOUNCE && CONFIG_LCD_CAM_NATIVE_PSRAM
    uint8_t *source=NULL;
#endif
#if CONFIG_LCD_CAM_NATIVE_RGB_DOUBLE
    uint8_t *retained=NULL;
    uint32_t retained_hash=0;
#endif
    vsync_event=xSemaphoreCreateCounting(32,0);
    if(!vsync_event){puts("LCDCAM ERROR semaphore");return;}
    esp_lcd_rgb_panel_config_t c={.clk_src=LCD_CLK_SRC_DEFAULT,.data_width=RGB_BUS,.bits_per_pixel=RGB_BITS,
        .num_fbs=2,.dma_burst_size=16,.hsync_gpio_num=1,.vsync_gpio_num=2,.de_gpio_num=3,
        .pclk_gpio_num=39,.disp_gpio_num=-1,
        .timings={.pclk_hz=1000000,.h_res=W,.v_res=H,.hsync_pulse_width=2,.hsync_back_porch=4,
            .hsync_front_porch=4,.vsync_pulse_width=2,.vsync_back_porch=2,.vsync_front_porch=2}};
    for(int i=0;i<16;++i)c.data_gpio_nums[i]=i<RGB_BUS?data_pins[i]:-1;
#if CONFIG_LCD_CAM_NATIVE_RGB_BOUNCE
    c.num_fbs=0;c.flags.no_fb=1;c.bounce_buffer_size_px=W*4;
#elif CONFIG_LCD_CAM_NATIVE_RGB_DEMAND
    c.num_fbs=1;c.flags.refresh_on_demand=1;
#endif
#if CONFIG_LCD_CAM_NATIVE_PSRAM && !CONFIG_LCD_CAM_NATIVE_RGB_BOUNCE
    c.flags.fb_in_psram=1;
#endif
#if CONFIG_LCD_CAM_NATIVE_RGB_BOUNCE && CONFIG_LCD_CAM_NATIVE_PSRAM
    source=heap_caps_aligned_alloc(64,RGB_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!rgb_external_buffer("bounce_source",source,RGB_BYTES))goto release;
    rgb_fill(source,0);rgb_pattern_log(source,0);
    if(!rgb_cache_sync("rgb_bounce_source_writeback",source,false))goto release;
    bounce_source=source;
#endif
    if(!check("rgb_panel",esp_lcd_new_rgb_panel(&c,&panel)))goto release;
    esp_lcd_rgb_panel_event_callbacks_t cb={.on_vsync=vsync_done,.on_frame_buf_complete=reusable,
        .on_color_trans_done=drawn,.on_bounce_empty=bounce};
    if(!check("rgb_callbacks",esp_lcd_rgb_panel_register_event_callbacks(panel,&cb,NULL)))goto release;
#if !CONFIG_LCD_CAM_NATIVE_RGB_BOUNCE
    uint8_t *fb0=NULL,*fb1=NULL;
    if(c.num_fbs==2) {
        if(!check("rgb_framebuffers",esp_lcd_rgb_panel_get_frame_buffer(panel,2,(void**)&fb0,(void**)&fb1)))goto release;
    } else {
        if(!check("rgb_framebuffer",esp_lcd_rgb_panel_get_frame_buffer(panel,1,(void**)&fb0)))goto release;
    }
    bool distinct=fb0 && fb1 && ((uintptr_t)fb0+RGB_BYTES<=(uintptr_t)fb1 ||
        (uintptr_t)fb1+RGB_BYTES<=(uintptr_t)fb0);
    printf("LCDCAM RGB_FRAMEBUFFERS fb0=%p fb1=%p num_fbs=%u bytes=%d distinct_spans=%u external_requested=%u\n",
        (void*)fb0,(void*)fb1,(unsigned)c.num_fbs,RGB_BYTES,distinct,(unsigned)c.flags.fb_in_psram);
    if(!fb0 || (c.num_fbs==2 && !distinct)){puts("LCDCAM ERROR rgb_framebuffer_aliases");goto release;}
#if CONFIG_LCD_CAM_NATIVE_PSRAM
    if(!rgb_external_buffer("fb0",fb0,RGB_BYTES) ||
        (fb1 && !rgb_external_buffer("fb1",fb1,RGB_BYTES)))goto release;
#endif
    rgb_fill(fb0,0);if(fb1)rgb_fill(fb1,1);
    rgb_pattern_log(fb0,0);if(fb1)rgb_pattern_log(fb1,1);
    if(!rgb_writeback("rgb_fb0_initial_writeback",fb0) ||
        (fb1 && !rgb_writeback("rgb_fb1_initial_writeback",fb1)))goto release;
#if CONFIG_LCD_CAM_NATIVE_RGB_DOUBLE
    retained=heap_caps_malloc(RGB_BYTES,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!retained){puts("LCDCAM ERROR rgb_retention_storage");goto release;}
    memcpy(retained,fb0,RGB_BYTES);retained_hash=hash_bytes(retained,RGB_BYTES);
    if(!rgb_retention_log(fb0,retained,retained_hash,"prepared"))goto release;
#endif
#endif
    if(!check("rgb_reset",esp_lcd_panel_reset(panel)))goto release;
#if CONFIG_LCD_CAM_NATIVE_RGB_BOUNCE && CONFIG_LCD_CAM_NATIVE_PSRAM
    /* No task source reads after invalidation: the ordinary driver's prefill
     * and subsequent real GDMA EOF ISR refills read the committed PSRAM. */
    if(!rgb_cache_sync("rgb_bounce_source_readback",source,true))goto release;
#endif
    unsigned previous_vsync=vsync_count;
    if(!check("rgb_init",esp_lcd_panel_init(panel)))goto release;
#if CONFIG_LCD_CAM_NATIVE_RGB_DEMAND
    for(unsigned i=0;i<3;i++) {
        rgb_fill(fb0,i);rgb_pattern_log(fb0,i);
        if(!rgb_writeback("rgb_demand_writeback",fb0))goto release;
        previous_vsync=vsync_count;
        if(!check("refresh",esp_lcd_rgb_panel_refresh(panel)) || !wait_vsync(previous_vsync))goto release;
    }
    check("restart_demand_expected_invalid_state",esp_lcd_rgb_panel_restart(panel));
#else
    if(!wait_vsync(previous_vsync))goto release;
#if CONFIG_LCD_CAM_NATIVE_RGB_DOUBLE
    if(!rgb_retention_log(fb0,retained,retained_hash,"before_swap"))goto release;
    if(!rgb_writeback("rgb_fb1_before_draw",fb1))goto release;
    portENTER_CRITICAL(&rgb_events_lock);
    unsigned before=reusable_count;
    previous_vsync=vsync_count;
    portEXIT_CRITICAL(&rgb_events_lock);
    printf("LCDCAM RGB_SWAP_REQUEST old=%p next=%p vsync_before=%u reusable_before=%u time_us=%lld\n",
        (void*)fb0,(void*)fb1,previous_vsync,before,(long long)esp_timer_get_time());
    if(!check("rgb_swap",esp_lcd_panel_draw_bitmap(panel,0,0,W,H,fb1)) ||
        !wait_vsync(previous_vsync))goto release;
    for(unsigned i=0;i<3 && reusable_count==before;i++){
        previous_vsync=vsync_count;
        if(!wait_vsync(previous_vsync))goto release;
    }
    if(reusable_count==before){puts("LCDCAM ERROR framebuffer_not_released");goto release;}
    /* S3 EOF callbacks do not identify the old buffer and prefetch may still
     * reference it. Observe events, but never overwrite fb0 before deletion. */
    printf("LCDCAM RGB_OWNERSHIP reusable_events=%u old_retained_until_delete=1 overwrite_permitted=0 time_us=%lld\n",
        reusable_count-before,(long long)esp_timer_get_time());
    if(!rgb_retention_log(fb0,retained,retained_hash,"after_swap_events"))goto release;
#endif
    previous_vsync=vsync_count;
    if(!check("pclk_divider",esp_lcd_rgb_panel_set_pclk(panel,500000)) ||
        !wait_vsync(previous_vsync))goto release;
    previous_vsync=vsync_count;
    if(!wait_vsync(previous_vsync))goto release;
    previous_vsync=vsync_count;
    if(!check("restart",esp_lcd_rgb_panel_restart(panel)) || !wait_vsync(previous_vsync))goto release;
    previous_vsync=vsync_count;
    if(!wait_vsync(previous_vsync))goto release;
#if CONFIG_LCD_CAM_NATIVE_RGB_DOUBLE
    if(!rgb_retention_log(fb0,retained,retained_hash,"before_stop"))goto release;
#endif
#endif
release:
    /* Driver deletion stops LCD/GDMA and owns all framebuffer/bounce frees.
     * A failed delete leaves callback-visible storage and semaphore intact. */
    if(panel && !check("rgb_delete",esp_lcd_panel_del(panel))){
        puts("LCDCAM ERROR rgb_outstanding_storage_retained");return;
    }
    if(panel)printf("LCDCAM RGB_STOPPED driver_deleted=1 time_us=%lld\n",(long long)esp_timer_get_time());
#if CONFIG_LCD_CAM_NATIVE_RGB_BOUNCE && CONFIG_LCD_CAM_NATIVE_PSRAM
    bounce_source=NULL;heap_caps_free(source);
#endif
#if CONFIG_LCD_CAM_NATIVE_RGB_DOUBLE
    heap_caps_free(retained);
#endif
    vSemaphoreDelete(vsync_event);vsync_event=NULL;
}
#endif

#if CONFIG_LCD_CAM_NATIVE_CAMERA
#if CONFIG_LCD_CAM_NATIVE_PSRAM && CONFIG_SPIRAM_MODE_QUAD && CONFIG_SPIRAM_SPEED_40M
#define CAM_NATIVE_EXTERNAL 1
#else
#define CAM_NATIVE_EXTERNAL 0
#endif

#if CAM_NATIVE_EXTERNAL
#define CAM_WIDTH 160u
#define CAM_HEIGHT 120u
#define CAM_RAW_BYTES (CAM_WIDTH * CAM_HEIGHT * 2u)
#define CAM_RGB888_BYTES (CAM_WIDTH * CAM_HEIGHT * 3u)
#define CAM_JPEG_SOURCE_BOUND (20u * 15u * 3u * 300u + 512u)

typedef struct {
    camera_fb_t *fb;
    camera_fb_t snapshot;
    unsigned frame;
    size_t capacity;
    uint32_t hash;
    int64_t timestamp_us;
} cam_owned_t;

static bool cam_external_profile_ready(const camera_config_t *config)
{
#if !CONFIG_CAMERA_PSRAM_DMA || CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE != 32 || CONFIG_CAMERA_DMA_BUFFER_SIZE_MAX != 32768 || CONFIG_CAMERA_CONVERTER_ENABLED
    puts("LCDCAM ERROR camera_external_requires_direct_DMA_cache32_stock_DMA_sizes_no_converter");
    return false;
#endif
    if (config->fb_count != 2 || CONFIG_LCD_CAM_NATIVE_FAULT) {
        puts("LCDCAM ERROR camera_external_requires_two_buffers_no_fault");
        return false;
    }
    if (!esp_camera_get_psram_mode()) {
        puts("LCDCAM ERROR camera_external_direct_DMA_not_enabled");
        return false;
    }
    if (config->pixel_format == PIXFORMAT_JPEG) {
#if !CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE_CUSTOM
        puts("LCDCAM ERROR camera_external_requires_bounded_custom_JPEG_capacity");
        return false;
#else
        if (CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE < CAM_JPEG_SOURCE_BOUND ||
            CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE % 1024 != 0 ||
            CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE > 8u * 1024 * 1024 - 1024 - 64) {
            puts("LCDCAM ERROR camera_external_JPEG_capacity_outside_owned_DMA_bounds");
            return false;
        }
#endif
#if CONFIG_JD_USE_ROM || CONFIG_JD_FORMAT != 0 || CONFIG_JD_FASTDECODE == 2
        puts("LCDCAM ERROR camera_external_requires_software_RGB888_decoder_work3100");
        return false;
#endif
    }
    return true;
}

static bool cam_external_invalidate(const cam_owned_t *owned, const char *phase)
{
    /*
     * Locked cam_hal.c allocates at alignment16, then advances by DMA alignment
     * minus the old address remainder (including a full alignment when zero).
     * Thus fb.buf has an owned prefix of at least16 bytes and a trailing guard
     * of dma_half_buffer_size (raw7680/JPEG1024 in these profiles). With cache32
     * and a 16-aligned payload, rounding reaches at most16 bytes on either side.
     * These are allocation-owned bytes, never neighboring heap metadata.
     */
    uintptr_t payload = (uintptr_t)owned->snapshot.buf;
    uintptr_t start = payload & ~(uintptr_t)31;
    size_t size = (owned->capacity + (payload - start) + 31u) & ~(size_t)31;
    if (!esp_ptr_external_ram((void *)start) ||
        !esp_ptr_external_ram((void *)(start + size - 1))) {
        puts("LCDCAM ERROR camera_external_cache_range_not_PSRAM");
        return false;
    }
    if (!check("camera_cache_M2C_invalidate",
               esp_cache_msync((void *)start, size,
                               ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                               ESP_CACHE_MSYNC_FLAG_INVALIDATE))) {
        return false;
    }
    printf("LCDCAM CAM_CACHE frame=%u phase=%s payload=%p capacity=%u aligned_start=%p aligned_bytes=%u direction=M2C invalidate=1 time_us=%lld\n",
           owned->frame, phase, owned->snapshot.buf, (unsigned)owned->capacity,
           (void *)start, (unsigned)size, (long long)esp_timer_get_time());
    return true;
}

static bool cam_jpeg_shape(const uint8_t *bytes, size_t length)
{
    /* fmt2rgb888 supplies UINT32_MAX as output capacity: bound its SOF first. */
    if (length < 4 || bytes[0] != 0xff || bytes[1] != 0xd8 ||
        bytes[length - 2] != 0xff || bytes[length - 1] != 0xd9) {
        return false;
    }
    bool shape = false;
    size_t pos = 2;
    while (pos < length) {
        if (bytes[pos++] != 0xff) return false;
        while (pos < length && bytes[pos] == 0xff) ++pos;
        if (pos == length) return false;
        unsigned marker = bytes[pos++];
        if (marker == 0 || marker == 1 || (marker >= 0xd0 && marker <= 0xd9) ||
            length - pos < 2) return false;
        size_t segment = ((size_t)bytes[pos] << 8) | bytes[pos + 1];
        if (segment < 2 || segment > length - pos) return false;
        if (marker == 0xc0) {
            if (shape || segment != 17 || bytes[pos + 2] != 8 ||
                (((unsigned)bytes[pos + 3] << 8) | bytes[pos + 4]) != CAM_HEIGHT ||
                (((unsigned)bytes[pos + 5] << 8) | bytes[pos + 6]) != CAM_WIDTH ||
                bytes[pos + 7] != 3) return false;
            shape = true;
        } else if (marker >= 0xc1 && marker <= 0xcf && marker != 0xc4) {
            return false;
        }
        if (marker == 0xda) {
            return shape && segment == 12 && bytes[pos + 2] == 3 &&
                   bytes[pos + 9] == 0 && bytes[pos + 10] == 63 &&
                   bytes[pos + 11] == 0;
        }
        pos += segment;
    }
    return false;
}

static bool cam_external_get(cam_owned_t *owned, unsigned frame,
                             pixformat_t format, uint8_t *decoded)
{
    printf("LCDCAM CAM_GET_BEGIN frame=%u time_us=%lld\n",
           frame, (long long)esp_timer_get_time());
    int64_t start = esp_timer_get_time();
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        printf("LCDCAM CAM_TIMEOUT frame=%u elapsed_us=%lld\n",
               frame, (long long)(esp_timer_get_time() - start));
        return false;
    }
    owned->fb = fb;
    owned->snapshot = *fb;
    owned->frame = frame;
    owned->capacity = format == PIXFORMAT_JPEG ? CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE : CAM_RAW_BYTES;
    owned->timestamp_us = (int64_t)fb->timestamp.tv_sec * 1000000 + fb->timestamp.tv_usec;
    bool valid = fb->buf && esp_ptr_external_ram(fb->buf) &&
                 ((uintptr_t)fb->buf % 16u) == 0 &&
                 fb->width == CAM_WIDTH && fb->height == CAM_HEIGHT &&
                 fb->format == format && fb->len > 0 && fb->len <= owned->capacity &&
                 (format == PIXFORMAT_JPEG || fb->len == CAM_RAW_BYTES);
    printf("LCDCAM CAM_EXTERNAL_BUFFER frame=%u handle=%p payload=%p external=%u len=%u capacity=%u timestamp_us=%lld valid=%u\n",
           frame, (void *)fb, fb->buf, fb->buf && esp_ptr_external_ram(fb->buf),
           (unsigned)fb->len, (unsigned)owned->capacity,
           (long long)owned->timestamp_us, valid);
    if (!valid || !cam_external_invalidate(owned, "acquire")) {
        puts("LCDCAM ERROR camera_external_buffer_contract");
        return false;
    }
    owned->hash = hash_bytes(fb->buf, fb->len);
    if (format == PIXFORMAT_JPEG) {
        valid = cam_jpeg_shape(fb->buf, fb->len) &&
                fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, decoded);
        printf("LCDCAM CAM_DECODE frame=%u source_hash=%08lx decoder=fmt2rgb888 sof=baseline8_3component width=160 height=120 output_type=RGB888 output_bytes=%u decoded=%u",
               frame, (unsigned long)owned->hash, (unsigned)CAM_RGB888_BYTES, valid);
        if (valid) {
            printf(" hash=%08lx head=%02x%02x%02x tail=%02x%02x%02x",
                   (unsigned long)hash_bytes(decoded, CAM_RGB888_BYTES),
                   decoded[0], decoded[1], decoded[2],
                   decoded[CAM_RGB888_BYTES - 3], decoded[CAM_RGB888_BYTES - 2],
                   decoded[CAM_RGB888_BYTES - 1]);
        }
        printf(" time_us=%lld\n", (long long)esp_timer_get_time());
    }
    printf("LCDCAM CAM_FRAME frame=%u width=%u height=%u format=%d len=%u hash=%08lx valid=%d head=%02x%02x tail=%02x%02x time_us=%lld\n",
           frame, (unsigned)fb->width, (unsigned)fb->height, fb->format,
           (unsigned)fb->len, (unsigned long)owned->hash, valid,
           fb->buf[0], fb->len > 1 ? fb->buf[1] : 0,
           fb->len > 1 ? fb->buf[fb->len - 2] : 0, fb->buf[fb->len - 1],
           (long long)esp_timer_get_time());
    if (!valid) puts("LCDCAM ERROR camera_external_JPEG_decode");
    return valid;
}

static bool cam_external_retained(const cam_owned_t *owned, const char *phase)
{
    camera_fb_t *fb = owned->fb;
    const camera_fb_t *snapshot = &owned->snapshot;
    bool metadata = fb->buf == snapshot->buf && fb->len == snapshot->len &&
                    fb->width == snapshot->width && fb->height == snapshot->height &&
                    fb->format == snapshot->format &&
                    fb->timestamp.tv_sec == snapshot->timestamp.tv_sec &&
                    fb->timestamp.tv_usec == snapshot->timestamp.tv_usec;
    if (!metadata || !cam_external_invalidate(owned, phase)) {
        puts("LCDCAM ERROR camera_held_metadata_or_cache_changed");
        return false;
    }
    uint32_t hash = hash_bytes(snapshot->buf, snapshot->len);
    printf("LCDCAM CAM_RETAIN frame=%u phase=%s before_hash=%08lx after_hash=%08lx metadata_same=%u unchanged=%u time_us=%lld\n",
           owned->frame, phase, (unsigned long)owned->hash, (unsigned long)hash,
           metadata, hash == owned->hash, (long long)esp_timer_get_time());
    if (hash != owned->hash) puts("LCDCAM ERROR camera_held_payload_overwritten");
    return hash == owned->hash;
}

static bool cam_external_return(cam_owned_t *owned, int64_t *release_us)
{
    if (!cam_external_retained(owned, "before_return_rearm")) return false;
    *release_us = esp_timer_get_time();
    printf("LCDCAM CAM_RETURN_BEGIN frame=%u handle=%p time_us=%lld\n",
           owned->frame, (void *)owned->fb, (long long)*release_us);
    esp_camera_fb_return(owned->fb);
    owned->fb = NULL;
    printf("LCDCAM CAM_RETURN frame=%u time_us=%lld\n",
           owned->frame, (long long)esp_timer_get_time());
    return true;
}

static void run_camera_external(const camera_config_t *config)
{
    cam_owned_t owned[4] = {0};
    uint8_t *decoded = NULL;
    bool complete = false;
    if (config->pixel_format == PIXFORMAT_JPEG) {
        decoded = heap_caps_malloc(CAM_RGB888_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!decoded) {
            puts("LCDCAM ERROR camera_RGB888_output_alloc");
            goto done;
        }
    }
    if (!cam_external_get(&owned[0], 0, config->pixel_format, decoded) ||
        !cam_external_get(&owned[1], 1, config->pixel_format, decoded)) goto done;
    uintptr_t a = (uintptr_t)owned[0].snapshot.buf;
    uintptr_t b = (uintptr_t)owned[1].snapshot.buf;
    bool distinct = owned[0].fb != owned[1].fb &&
                    (a + owned[0].capacity <= b || b + owned[1].capacity <= a);
    int64_t interval_us = owned[1].timestamp_us - owned[0].timestamp_us;
    printf("LCDCAM CAM_TWO_HELD requested=%u actual=2 distinct_handles_and_payloads=%u interval_us=%lld first_return_not_yet=1 time_us=%lld\n",
           (unsigned)config->fb_count, distinct, (long long)interval_us,
           (long long)esp_timer_get_time());
    if (!distinct || interval_us <= 0) {
        puts("LCDCAM ERROR camera_two_held_ownership_or_actual_interval");
        goto done;
    }
    int64_t hold_us = interval_us;
    if (hold_us < (int64_t)CONFIG_LCD_CAM_NATIVE_SLOW_MS * 1000)
        hold_us = (int64_t)CONFIG_LCD_CAM_NATIVE_SLOW_MS * 1000;
    int64_t hold_start = esp_timer_get_time();
    printf("LCDCAM CAM_HOLD_BEGIN owned=2 interval_us=%lld requested_hold_us=%lld time_us=%lld\n",
           (long long)interval_us, (long long)hold_us, (long long)hold_start);
    vTaskDelay(pdMS_TO_TICKS((hold_us + 999) / 1000) + 1);
    int64_t held_us = esp_timer_get_time() - hold_start;
    printf("LCDCAM CAM_HOLD_END owned=2 elapsed_us=%lld interval_elapsed=%u time_us=%lld\n",
           (long long)held_us, held_us >= interval_us, (long long)esp_timer_get_time());
    if (held_us < interval_us ||
        !cam_external_retained(&owned[0], "two_held_after_interval") ||
        !cam_external_retained(&owned[1], "two_held_after_interval")) goto done;
    for (unsigned frame = 2; frame < 4; ++frame) {
        unsigned returned = frame - 2;
        unsigned still_held = frame - 1;
        camera_fb_t *released_handle = owned[returned].fb;
        uint8_t *released_payload = owned[returned].snapshot.buf;
        int64_t release_us;
        if (!cam_external_return(&owned[returned], &release_us) ||
            !cam_external_get(&owned[frame], frame, config->pixel_format, decoded)) goto done;
        bool resumed = owned[frame].fb == released_handle &&
                       owned[frame].snapshot.buf == released_payload &&
                       owned[frame].timestamp_us >= release_us &&
                       owned[frame].timestamp_us > owned[returned].timestamp_us;
        printf("LCDCAM CAM_REARM returned_frame=%u recovery_frame=%u same_returned_slot=%u timestamp_after_return=%u producer_resumed=%u still_held_frame=%u time_us=%lld\n",
               returned, frame,
               owned[frame].fb == released_handle && owned[frame].snapshot.buf == released_payload,
               owned[frame].timestamp_us >= release_us, resumed, still_held,
               (long long)esp_timer_get_time());
        if (!resumed || !cam_external_retained(&owned[still_held], "other_slot_rearmed")) {
            puts("LCDCAM ERROR camera_return_rearm_recovery");
            goto done;
        }
    }
    int64_t release_us;
    if (!cam_external_return(&owned[2], &release_us) ||
        !cam_external_return(&owned[3], &release_us)) goto done;
    complete = true;
done:
    /* On error, deinit stops the real producer before freeing any held slots. */
    if (!check("camera_external_deinit", esp_camera_deinit())) complete = false;
    heap_caps_free(decoded);
    printf("LCDCAM CAM_EXTERNAL_SUMMARY requested_buffers=2 completed=%u no_fallback=1 qualification=not_claimed\n", complete);
}
#endif

static void fault_task(void *context)
{
    (void)context;ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_LCD_CAM_NATIVE_FAULT_DELAY_MS));
    int pin=CONFIG_LCD_CAM_NATIVE_FAULT==1?14:13;
    int level=CONFIG_LCD_CAM_NATIVE_FAULT==1?1:0;
    gpio_set_level(pin,level);
    printf("LCDCAM CAM_PHYSICAL_FAULT pin=%d level=%d time_us=%lld\n",pin,level,(long long)esp_timer_get_time());
    vTaskDelete(NULL);
}
static void run_camera(void)
{
    camera_config_t c={.pin_pwdn=14,.pin_reset=13,.pin_xclk=15,.pin_sccb_sda=1,.pin_sccb_scl=2,
        .pin_d0=4,.pin_d1=5,.pin_d2=6,.pin_d3=7,.pin_d4=8,.pin_d5=9,.pin_d6=10,.pin_d7=11,
        .pin_vsync=16,.pin_href=17,.pin_pclk=12,.xclk_freq_hz=20000000,
        .ledc_timer=LEDC_TIMER_0,.ledc_channel=LEDC_CHANNEL_0,.pixel_format=PIXFORMAT_RGB565,
        .frame_size=FRAMESIZE_QQVGA,.jpeg_quality=12,.fb_count=CONFIG_LCD_CAM_NATIVE_FB_COUNT,
        .fb_location=CAMERA_FB_IN_DRAM,.grab_mode=CAMERA_GRAB_WHEN_EMPTY};
#if CONFIG_LCD_CAM_NATIVE_CAM_YUV422
    c.pixel_format=PIXFORMAT_YUV422;
#elif CONFIG_LCD_CAM_NATIVE_CAM_JPEG
    c.pixel_format=PIXFORMAT_JPEG;
#endif
#if CONFIG_LCD_CAM_NATIVE_PSRAM
    if (!requested_psram_ready()) return;
    c.fb_location=CAMERA_FB_IN_PSRAM;
#if CAM_NATIVE_EXTERNAL
    if (!cam_external_profile_ready(&c)) return;
    puts("LCDCAM CAM_EXTERNAL_REQUEST profile=quad40 peri=5 direction=IN buffers=2 qualification=not_claimed");
#else
    puts("LCDCAM UNQUALIFIED hardware_PSRAM_fixture native_external_requires_quad40_cache32_direct_DMA");
#endif
#endif
    if(!check("camera_init",esp_camera_init(&c)))return;
    sensor_t *sensor=esp_camera_sensor_get();
    if(!sensor){puts("LCDCAM ERROR missing_sensor");return;}
    printf("LCDCAM CAM_PROBE pid=%04x ver=%02x midh=%02x midl=%02x\n",sensor->id.PID,sensor->id.VER,sensor->id.MIDH,sensor->id.MIDL);
#if CAM_NATIVE_EXTERNAL
    if (!esp_camera_get_psram_mode()) {
        puts("LCDCAM ERROR camera_external_direct_DMA_disabled_after_init");
        check("camera_external_deinit", esp_camera_deinit());
        return;
    }
    run_camera_external(&c);
    return;
#endif
    TaskHandle_t fault=NULL;
    if(CONFIG_LCD_CAM_NATIVE_FAULT && xTaskCreate(fault_task,"physical_fault",2048,NULL,6,&fault)!=pdPASS){puts("LCDCAM ERROR fault_task");return;}
    unsigned received=0,invalid=0;
    for(unsigned frame=0;frame<4;frame++) {
        printf("LCDCAM CAM_GET_BEGIN frame=%u time_us=%lld\n",frame,(long long)esp_timer_get_time());
        if(frame==1 && fault)xTaskNotifyGive(fault);
        int64_t start=esp_timer_get_time();
        camera_fb_t *fb=esp_camera_fb_get();
        if(!fb) {printf("LCDCAM CAM_TIMEOUT frame=%u elapsed_us=%lld\n",frame,(long long)(esp_timer_get_time()-start));continue;}
        ++received;
        bool raw=fb->format!=PIXFORMAT_JPEG;
        bool valid=fb->width==160 && fb->height==120 && fb->format==c.pixel_format;
        if(raw)valid=valid && fb->len==160*120*2;
        else valid=valid && fb->len>=4 && fb->buf[0]==0xff && fb->buf[1]==0xd8 && fb->buf[fb->len-2]==0xff && fb->buf[fb->len-1]==0xd9;
        if(!valid)++invalid;
        printf("LCDCAM CAM_FRAME frame=%u width=%u height=%u format=%d len=%u hash=%08lx valid=%d head=%02x%02x tail=%02x%02x time_us=%lld\n",
            frame,(unsigned)fb->width,(unsigned)fb->height,fb->format,(unsigned)fb->len,
            (unsigned long)hash_bytes(fb->buf,fb->len),valid,fb->len?fb->buf[0]:0,fb->len>1?fb->buf[1]:0,
            fb->len>1?fb->buf[fb->len-2]:0,fb->len?fb->buf[fb->len-1]:0,(long long)esp_timer_get_time());
        if(CONFIG_LCD_CAM_NATIVE_SLOW_MS)vTaskDelay(pdMS_TO_TICKS(CONFIG_LCD_CAM_NATIVE_SLOW_MS));
        esp_camera_fb_return(fb);
        printf("LCDCAM CAM_RETURN frame=%u time_us=%lld\n",frame,(long long)esp_timer_get_time());
    }
    printf("LCDCAM CAM_SUMMARY received=%u invalid=%u buffers=%u slow_ms=%u fault=%u\n",received,invalid,
        CONFIG_LCD_CAM_NATIVE_FB_COUNT,CONFIG_LCD_CAM_NATIVE_SLOW_MS,CONFIG_LCD_CAM_NATIVE_FAULT);
    if(!check("camera_deinit",esp_camera_deinit()))return;
    if(CONFIG_LCD_CAM_NATIVE_FAULT==2){
        gpio_set_level(13,1);
        if(!check("camera_reinit_after_reset",esp_camera_init(&c)))return;
        camera_fb_t *fb=esp_camera_fb_get();
        if(!fb)puts("LCDCAM ERROR reset_recovery_timeout");
        else {
            printf("LCDCAM CAM_RECOVERY width=%u height=%u format=%d len=%u hash=%08lx valid=%d time_us=%lld\n",
                (unsigned)fb->width,(unsigned)fb->height,fb->format,(unsigned)fb->len,
                (unsigned long)hash_bytes(fb->buf,fb->len),
                fb->width==160 && fb->height==120 && fb->format==c.pixel_format && fb->len==38400,
                (long long)esp_timer_get_time());
            esp_camera_fb_return(fb);
            printf("LCDCAM CAM_RECOVERY_RETURN time_us=%lld\n",(long long)esp_timer_get_time());
        }
        check("camera_recovery_deinit",esp_camera_deinit());
    }
}
#endif
void app_main(void)
{
    puts("LCDCAM BEGIN idf=5.5.5 camera=v2.1.3 hash=fnv1a32 no_authored_PASS_claim");
#if CONFIG_LCD_CAM_NATIVE_I80
    run_i80();
#elif CONFIG_LCD_CAM_NATIVE_RGB
    run_rgb();
#else
    run_camera();
#endif
    puts("LCDCAM END inspect_API_errors_and_independent_electrical_capture");
}
