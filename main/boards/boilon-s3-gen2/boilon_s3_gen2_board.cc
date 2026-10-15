#include "board.h"

// This retained factory preserves the normal board-selection contract without
// making persistent settings or networking reachable from the isolated app.
// m2_library_main.cc deliberately never calls Board::GetInstance().
class BoilonS3Gen2Board final : public Board {
public:
    BoilonS3Gen2Board() noexcept : Board(TransientIdentityTag{}) {}

    std::string GetBoardType() override { return BOARD_TYPE; }
    std::string GetUuid() override { return "boilon-s3-gen2-diagnostic"; }
    Backlight* GetBacklight() override { return nullptr; }
    Led* GetLed() override { return nullptr; }
    AudioCodec* GetAudioCodec() override { return nullptr; }
    bool GetTemperature(float&) override { return false; }
    Display* GetDisplay() override { return nullptr; }
    Camera* GetCamera() override { return nullptr; }
    NetworkInterface* GetNetwork() override { return nullptr; }
    void StartNetwork() override {}
    void SetNetworkEventCallback(NetworkEventCallback) override {}
    const char* GetNetworkStateIcon() override { return ""; }
    bool GetBatteryLevel(int&, bool&, bool&) override { return false; }
    std::string GetSystemInfoJson() override { return "{}"; }
    void SetPowerSaveLevel(PowerSaveLevel) override {}
    std::string GetBoardJson() override { return "{}"; }
    std::string GetDeviceStatusJson() override { return "{}"; }
};

DECLARE_BOARD(BoilonS3Gen2Board);
