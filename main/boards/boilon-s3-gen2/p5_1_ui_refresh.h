#pragma once
#include <algorithm>
#include <cstdint>
#include "p5_1_ui_screens.h"

// P5-1: draws each page into an index frame and sends only the bands that
// differ from the frame on the panel, one DMA strip per Poll. The port is the
// panel: bool Done() (the last strip finished, never waits), void Send(x0, y0,
// x1, y1, pixels) (starts one strip), void BacklightOn() and void Fail(reason).
// No ESP-IDF headers: test_p5_1_ui drives it with a fake panel.
namespace boilon::ui {
template <typename Port>
class Refresh {
public:
    // 8 full-width rows per strip: the DMA buffer the panel has always used.
    static constexpr int kStripPixels = kWidth * 8, kMaxBands = 4;
    static constexpr uint64_t kFrameMs = 125, kTimeoutMs = 500;

    Refresh(Port& port, const Fonts& fonts) : port_(port), fonts_(fonts) {}
    // Two kWidth*kHeight index frames and a kStripPixels DMA buffer.
    void Attach(uint8_t* front, uint8_t* back, uint16_t* strip) {
        front_ = front;
        back_ = back;
        strip_ = strip;
    }
    // The previous page is completely on the panel and the next may be drawn.
    bool Ready(uint64_t now) const { return band_ >= bands_ && !waiting_ && now >= next_; }
    void Show(const Model& model, Palette palette, Layout layout, uint64_t now) {
        if (palette != palette_ || !colors_ready_) {
            const uint32_t* rgb = palette == Palette::kPaper ? kPaperRgb : kNightRgb;
            for (int i = 0; i < kColorCount; ++i) {
                const uint16_t value = Rgb565(rgb[i]);
                colors_[i] = static_cast<uint16_t>(value << 8 | value >> 8);  // High byte first.
            }
            palette_ = palette;
            colors_ready_ = true;
            full_ = true;  // Same indices, new colors: resend everything.
        }
        Canvas canvas(back_);
        Draw(canvas, fonts_, model, layout, static_cast<double>(now) / 1000);
        if (full_) {
            list_[0] = {0, kWidth, 0, kHeight};
            bands_ = 1;
            full_ = false;
        } else {
            bands_ = ChangedBands(front_, back_, list_, kMaxBands);
        }
        std::swap(front_, back_);
        band_ = 0;
        row_ = bands_ ? list_[0].y0 : 0;
        next_ = now + kFrameMs;
    }
    void Poll(uint64_t now) {
        if (waiting_) {
            if (!port_.Done()) {
                if (now - submitted_ >= kTimeoutMs)
                    port_.Fail("LCD_DMA_TIMEOUT");  // Never reuse a possibly DMA-owned buffer.
                return;
            }
            waiting_ = false;
            row_ += rows_;
            if (row_ >= list_[band_].y1 && ++band_ < bands_)
                row_ = list_[band_].y0;
            if (band_ >= bands_ && !lit_) {
                port_.BacklightOn();  // After the first complete page.
                lit_ = true;
            }
        }
        if (band_ >= bands_)
            return;
        const Band& band = list_[band_];
        rows_ = std::min(band.y1 - row_, std::max(1, kStripPixels / (band.x1 - band.x0)));
        uint16_t* out = strip_;
        for (int y = row_; y < row_ + rows_; ++y)
            for (int x = band.x0; x < band.x1; ++x)
                *out++ = colors_[front_[y * kWidth + x]];
        port_.Send(band.x0, row_, band.x1, row_ + rows_, strip_);
        submitted_ = now;
        waiting_ = true;
    }

private:
    Port& port_;
    const Fonts& fonts_;
    uint16_t colors_[kColorCount] = {};
    Palette palette_ = kDefaultPalette;
    bool colors_ready_ = false, full_ = true, waiting_ = false, lit_ = false;
    uint8_t *front_ = nullptr, *back_ = nullptr;
    uint16_t* strip_ = nullptr;
    Band list_[kMaxBands];
    int bands_ = 0, band_ = 0, row_ = 0, rows_ = 0;
    uint64_t submitted_ = 0, next_ = 0;
};
}  // namespace boilon::ui
