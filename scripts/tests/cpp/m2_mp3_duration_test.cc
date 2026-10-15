#include "boards/boilon-s3-gen2/m2_mp3_duration.h"

#include <cassert>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using boilon::local_library::Mp3Duration;
using Status = Mp3Duration::Status;

namespace {
using Bytes = std::vector<uint8_t>;

Bytes Frame(unsigned version, unsigned bitrate = 9, unsigned frequency = 0, bool padding = false,
            unsigned mode = 0) {
    const unsigned rates[] = {44100, 48000, 32000};
    const unsigned mpeg1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    const unsigned mpeg2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    const unsigned rate = rates[frequency] / (version == 3 ? 1 : version == 2 ? 2 : 4);
    const size_t size =
        (version == 3 ? 144000 * mpeg1[bitrate] : 72000 * mpeg2[bitrate]) / rate + padding;
    const uint32_t header = 0xffe00000U | (version << 19) | (1U << 17) | (1U << 16) |
                            (bitrate << 12) | (frequency << 10) | (unsigned(padding) << 9) |
                            (mode << 6);
    Bytes result(size, 0);
    for (int i = 0; i < 4; ++i)
        result[i] = static_cast<uint8_t>(header >> (24 - 8 * i));
    return result;
}

void Append(Bytes& result, const Bytes& frame) {
    result.insert(result.end(), frame.begin(), frame.end());
}

struct Reader {
    const Bytes& bytes;
    int calls = 0;
    int headers = 0;
    int fail_at = 0;
    bool operator()(uint64_t offset, uint8_t* out, size_t count) {
        ++calls;
        headers += count == 4;
        assert(offset <= bytes.size() && count <= bytes.size() - offset);
        if (calls == fail_at)
            return false;
        std::copy_n(bytes.data() + offset, count, out);
        return true;
    }
};

uint64_t Scan(const Bytes& bytes, Status expected = Status::kKnown, int fail_at = 0) {
    Mp3Duration scanner(bytes.size());
    Reader reader{bytes, 0, 0, fail_at};
    assert(scanner.State() == Status::kScanning && scanner.DurationMs() == 0);
    int steps = 0;
    while (scanner.State() == Status::kScanning) {
        const int before = reader.headers;
        scanner.Step(std::ref(reader));
        assert(reader.headers - before <= 16);
        assert(++steps <= 100);
        if (scanner.State() != Status::kKnown)
            assert(scanner.DurationMs() == 0);
    }
    assert(scanner.State() == expected);
    const int calls = reader.calls;
    assert(scanner.Step(std::ref(reader)) == expected && reader.calls == calls);
    return scanner.DurationMs();
}

void Versions(bool variable) {
    for (const unsigned version : {3U, 2U, 0U}) {
        Bytes bytes;
        for (unsigned index = 0; index < 37; ++index)
            Append(bytes, Frame(version,
                                variable ? (index % 3 == 0   ? 7
                                            : index % 3 == 1 ? 9
                                                             : 10)
                                         : 9,
                                0, index % 2));
        const unsigned rate = version == 3 ? 44100 : version == 2 ? 22050 : 11025;
        const uint64_t samples = 37 * (version == 3 ? 1152 : 576);
        assert(Scan(bytes) == samples * 1000 / rate);
    }
    // Independent boundary examples for the MPEG1/2/2.5 frame-length factors.
    assert(Frame(3, 9).size() == 417 && Frame(3, 9, 0, true).size() == 418);
    assert(Frame(2, 8).size() == 208 && Frame(2, 8, 0, true).size() == 209);
    assert(Frame(0, 8).size() == 417 && Frame(0, 8, 0, true).size() == 418);
}

void Tags() {
    for (const unsigned version : {2U, 3U, 4U}) {
        for (const bool footer : {false, true}) {
            if (footer && version != 4)
                continue;
            Bytes bytes(10 + 17 + (footer ? 10 : 0), 0);
            bytes[0] = 'I';
            bytes[1] = 'D';
            bytes[2] = '3';
            bytes[3] = version;
            bytes[5] = footer ? 0x10 : 0;
            bytes[9] = 17;
            Append(bytes, Frame(3, 9, 0, true));
            Append(bytes, Frame(3, 10));
            Bytes tail(128, 0);
            tail[0] = 'T';
            tail[1] = 'A';
            tail[2] = 'G';
            Append(bytes, tail);
            assert(Scan(bytes) == 2304ULL * 1000 / 44100);
        }
    }
    Bytes bad_tag(10, 0);
    bad_tag[0] = 'I';
    bad_tag[1] = 'D';
    bad_tag[2] = '3';
    bad_tag[3] = 4;
    for (const int mode : {0, 1, 2, 3}) {
        auto bytes = bad_tag;
        if (mode == 0)
            bytes[6] = 0x80;  // Non-syncsafe tag length.
        if (mode == 1)
            bytes[9] = 100;  // Tag beyond the file.
        if (mode == 2)
            bytes[3] = 1;  // Unsupported version.
        if (mode == 3)
            bytes[4] = 0xff;  // Reserved revision.
        assert(Scan(bytes, Status::kUnknown) == 0);
    }
}

void Invalid() {
    for (const uint32_t mask : {0x00200000U, 0x00060000U, 0x0000f000U, 0x00000c00U}) {
        auto frame = Frame(3);
        uint32_t header = (uint32_t{frame[0]} << 24) | (uint32_t{frame[1]} << 16) |
                          (uint32_t{frame[2]} << 8) | frame[3];
        if (mask == 0x00200000U)
            header &= ~0x00400000U;  // Reserved MPEG version 01.
        if (mask == 0x00060000U)
            header &= ~mask;  // Reserved layer.
        if (mask == 0x0000f000U)
            header |= mask;  // Reserved bitrate index.
        if (mask == 0x00000c00U)
            header |= mask;  // Reserved frequency.
        for (int i = 0; i < 4; ++i)
            frame[i] = static_cast<uint8_t>(header >> (24 - 8 * i));
        assert(Scan(frame, Status::kUnknown) == 0);
    }
    for (int mode = 0; mode < 8; ++mode) {
        auto bytes = Frame(3);
        if (mode == 0)
            bytes.pop_back();  // Truncated final frame.
        if (mode == 1)
            bytes.insert(bytes.end(), {1, 2});  // Trailing junk.
        if (mode == 2)
            bytes[0] = 0;  // Lost synchronization.
        if (mode == 3)
            bytes[2] &= 0x0f;  // Free bitrate is unknown.
        if (mode == 4)
            bytes[3] |= 2;  // Reserved emphasis.
        if (mode == 5)
            Append(bytes, Frame(3, 9, 1));  // Rate changes.
        if (mode == 6)
            Append(bytes, Frame(2));  // MPEG version changes.
        if (mode == 7)
            Append(bytes, Frame(3, 9, 0, false, 3));  // Stereo becomes mono.
        assert(Scan(bytes, Status::kUnknown) == 0);
        auto partial = Frame(3);
        Append(partial, bytes);
        assert(Scan(partial, Status::kUnknown) ==
               0);  // A good prefix never makes a bad file known.
    }
    assert(Scan({}, Status::kUnknown) == 0);
    assert(Scan(Bytes{0xff, 0xfb, 0x90}, Status::kUnknown) == 0);
    Bytes bytes;
    Append(bytes, Frame(3));
    Append(bytes, Frame(3));
    assert(Scan(bytes, Status::kUnknown, 1) == 0);
    assert(Scan(bytes, Status::kUnknown, 3) == 0);
}

void BudgetAndCancel() {
    Bytes bytes;
    for (int i = 0; i < 33; ++i)
        Append(bytes, Frame(3));
    Reader reader{bytes};
    Mp3Duration scanner(bytes.size());
    assert(scanner.Step(std::ref(reader)) == Status::kScanning && reader.headers == 16);
    assert(scanner.DurationMs() == 0);
    const int before = reader.headers;
    assert(scanner.Step(std::ref(reader)) == Status::kScanning && reader.headers - before == 16);
    assert(scanner.DurationMs() == 0);
    assert(scanner.Step(std::ref(reader)) == Status::kKnown && reader.headers == 33);
    assert(scanner.DurationMs() == 33ULL * 1152 * 1000 / 44100);
    Mp3Duration cancelled(bytes.size());
    assert(cancelled.Step(std::ref(reader)) == Status::kScanning);
    cancelled.Cancel();
    const int calls = reader.calls;
    assert(cancelled.State() == Status::kUnknown && cancelled.DurationMs() == 0);
    assert(cancelled.Step(std::ref(reader)) == Status::kUnknown && reader.calls == calls);
    Mp3Duration early(bytes.size());
    early.Cancel();
    assert(early.Step(std::ref(reader)) == Status::kUnknown && reader.calls == calls);
}
void MetadataDuration() {
    for (unsigned version : {3U, 2U, 0U}) {
        for (unsigned mode : {0U, 3U}) {
            Bytes bytes;
            for (int i = 0; i < 100; ++i)
                Append(bytes, Frame(version, 9, 0, false, mode));
            const size_t offset = 4 + (version == 3 ? (mode == 3 ? 17 : 32) : (mode == 3 ? 9 : 17));
            std::memcpy(bytes.data() + offset, "Xing", 4);
            bytes[offset + 7] = 1;
            bytes[offset + 11] = 100;
            Reader reader{bytes};
            const uint64_t expected = 100ULL * (version == 3 ? 1152 : 576) * 1000 /
                                      (version == 3 ? 44100 : version == 2 ? 22050 : 11025);
            assert(Mp3Duration(bytes.size()).HeaderDuration(std::ref(reader)) == expected);
            assert(reader.calls == 3);  // Never read all hundred frames.
            std::memcpy(bytes.data() + offset, "Info", 4);
            assert(Mp3Duration(bytes.size()).HeaderDuration(Reader{bytes}) == expected);
            bytes[offset + 7] = 0;
            assert(Mp3Duration(bytes.size()).HeaderDuration(Reader{bytes}) == 0);
            bytes[offset + 7] = 1;
            bytes[offset + 8] = 0xff;
            assert(Mp3Duration(bytes.size()).HeaderDuration(Reader{bytes}) == 0);
        }
    }
    Bytes bytes = Frame(3);
    assert(Mp3Duration(bytes.size()).HeaderDuration(Reader{bytes}) == 0);
    assert(Mp3Duration(0).HeaderDuration(Reader{bytes}) == 0);
}
}  // namespace

int main(int argc, char** argv) {
    assert(argc >= 2);
    const std::string mode = argv[1];
    if (mode == "cbr")
        Versions(false);
    else if (mode == "vbr")
        Versions(true);
    else if (mode == "tags")
        Tags();
    else if (mode == "invalid")
        Invalid();
    else if (mode == "budget") {
        BudgetAndCancel();
        MetadataDuration();
    } else if (mode == "file") {
        assert(argc == 3);
        std::ifstream file(argv[2], std::ios::binary);
        assert(file);
        const Bytes bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::cout << Scan(bytes) << '\n';
        return 0;
    } else
        return 2;
    std::cout << mode << " PASS (encoding-frame timing only; no PCM decode)\n";
}
