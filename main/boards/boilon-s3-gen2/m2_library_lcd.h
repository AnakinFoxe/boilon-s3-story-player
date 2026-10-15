#pragma once
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/semphr.h>
#include "p1_4_panel_config.h"
#include "p5_1_ui_refresh.h"

namespace boilon::local_library {
#include "m2_library_common_font.inc"
// P5-1: the panel hardware for ui::Refresh, which draws each page as color
// indices into a PSRAM frame and sends only what changed, one DMA strip per
// Poll. No display task, no glyph cache.
class LibraryLcd {
public:
    using Failure = void (*)(const char*);
    using Light = void (*)(bool);
    LibraryLcd(Failure failure, Light light) : failure_(failure), light_(light) {}
    void Initialize() {
        if (!cbin_.Valid())
            failure_("LCD_FONT_FORMAT");
        namespace cal = lcd_cal;
        Check(gpio_set_level(static_cast<gpio_num_t>(cal::kBacklight), 0));
        Check(gpio_set_direction(static_cast<gpio_num_t>(cal::kBacklight), GPIO_MODE_OUTPUT));
        ledc_timer_config_t timer = {};
        timer.speed_mode = LEDC_LOW_SPEED_MODE;
        timer.duty_resolution = LEDC_TIMER_10_BIT;
        timer.timer_num = LEDC_TIMER_0;
        timer.freq_hz = cal::kBacklightHz;
        timer.clk_cfg = LEDC_AUTO_CLK;
        Check(ledc_timer_config(&timer));
        ledc_channel_config_t light = {};
        light.gpio_num = cal::kBacklight;
        light.speed_mode = LEDC_LOW_SPEED_MODE;
        light.channel = LEDC_CHANNEL_0;
        light.timer_sel = LEDC_TIMER_0;
        Check(ledc_channel_config(&light));
        done_ = xSemaphoreCreateBinary();
        auto* strip = static_cast<uint16_t*>(
            heap_caps_malloc(kStripPixels * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        // The index frames are read by the CPU only, so they live in PSRAM.
        auto* front = static_cast<uint8_t*>(heap_caps_malloc(ui::kWidth * ui::kHeight, MALLOC_CAP_SPIRAM));
        auto* back = static_cast<uint8_t*>(heap_caps_malloc(ui::kWidth * ui::kHeight, MALLOC_CAP_SPIRAM));
        if (!done_ || !strip || !front || !back)
            failure_("LCD_MEMORY");
        refresh_.Attach(front, back, strip);
        spi_bus_config_t bus = {};
        bus.mosi_io_num = cal::kMosi;
        bus.sclk_io_num = cal::kSclk;
        bus.miso_io_num = bus.quadwp_io_num = bus.quadhd_io_num = -1;
        bus.max_transfer_sz = kStripPixels * 2;
        Check(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = static_cast<gpio_num_t>(cal::kCs);
        io_config.dc_gpio_num = static_cast<gpio_num_t>(cal::kDc);
        io_config.spi_mode = 3;
        io_config.pclk_hz = cal::kPixelClockHz;
        io_config.trans_queue_depth = 1;
        io_config.lcd_cmd_bits = io_config.lcd_param_bits = 8;
        io_config.on_color_trans_done = OnSent;
        io_config.user_ctx = done_;
        esp_lcd_panel_io_handle_t io = nullptr;
        Check(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io));
        esp_lcd_panel_dev_config_t config = {};
        config.reset_gpio_num = GPIO_NUM_NC;
        config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        config.bits_per_pixel = 16;
        config.vendor_config = const_cast<jd9853_vendor_config_t*>(cal::PanelConfig());
        Check(esp_lcd_new_panel_jd9853(io, &config, &panel_));
        Check(esp_lcd_panel_reset(panel_));
        Check(esp_lcd_panel_init(panel_));
        Check(esp_lcd_panel_invert_color(panel_, false));
        Check(esp_lcd_panel_swap_xy(panel_, false));
        Check(esp_lcd_panel_mirror(panel_, false, false));
        Check(esp_lcd_panel_set_gap(panel_, 0, 0));
    }
    bool Ready(uint64_t now) const { return refresh_.Ready(now); }
    // Draws the page with the current style; Poll sends what changed.
    void Show(const ui::Model& model, uint64_t now) {
        const auto& style = ui::CurrentStyle();
        refresh_.Show(model, style.palette, style.layout, now);
    }
    void Poll(uint64_t now) { refresh_.Poll(now); }

private:
    static constexpr int kStripPixels = ui::kWidth * 8;  // as ui::Refresh sends
    friend class ui::Refresh<LibraryLcd>;
    // The panel port for ui::Refresh.
    bool Done() { return xSemaphoreTake(done_, 0) == pdTRUE; }
    void Send(int x0, int y0, int x1, int y1, const uint16_t* pixels) {
        Check(esp_lcd_panel_draw_bitmap(panel_, x0, y0, x1, y1, pixels));
    }
    void BacklightOn() { light_(true); }
    void Fail(const char* reason) { failure_(reason); }
    static bool OnSent(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* context) {
        BaseType_t wake = pdFALSE;
        xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(context), &wake);
        return wake == pdTRUE;
    }
    void Check(esp_err_t code) {
        if (code != ESP_OK)
            failure_("LCD_IO");
    }
    Failure failure_;
    Light light_;
    const ui::CbinFont cbin_{kCommonFont};
    const ui::Fonts fonts_{ui::Font{&cbin_}, ui::Font{}};
    ui::Refresh<LibraryLcd> refresh_{*this, fonts_};
    SemaphoreHandle_t done_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
};
}  // namespace boilon::local_library
