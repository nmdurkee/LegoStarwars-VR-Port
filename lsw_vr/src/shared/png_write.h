#pragma once

// Minimal PNG writer (stored deflate blocks, no compression): enough for
// diagnostic dumps without an image library. Used by the simulation mode and
// the VR host.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace png_write {
inline uint32_t Crc(const uint8_t *data, size_t length, uint32_t crc = 0xffffffffu) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            }
            table[n] = c;
        }
        built = true;
    }
    for (size_t i = 0; i < length; ++i) {
        crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    }
    return crc;
}

inline void Put32(std::vector<uint8_t> &out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

inline void Chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data) {
    Put32(out, static_cast<uint32_t>(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    Put32(out, Crc(out.data() + start, out.size() - start) ^ 0xffffffffu);
}

// RGB rows -> PNG file.
inline bool WritePng(const std::string &path, const std::vector<uint8_t> &rgb, int width, int height) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (width * 3 + 1));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);  // filter: none
        raw.insert(raw.end(), rgb.begin() + static_cast<size_t>(y) * width * 3,
                   rgb.begin() + static_cast<size_t>(y + 1) * width * 3);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t pos = 0; pos < raw.size();) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    Put32(z, (b << 16) | a);
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    std::vector<uint8_t> header;
    Put32(header, width);
    Put32(header, height);
    header.insert(header.end(), {8, 2, 0, 0, 0});  // 8-bit RGB
    Chunk(png, "IHDR", header);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", {});
    FILE *file = nullptr;
    if (fopen_s(&file, path.c_str(), "wb") != 0 || file == nullptr) {
        return false;
    }
    fwrite(png.data(), 1, png.size(), file);
    fclose(file);
    return true;
}
} // namespace png_write
