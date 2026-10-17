# SPDX-License-Identifier: MIT
"""Exercise production framebuffer bounds and read-only EFI color selection."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
generic = (root / 'src/system/boot/platform/generic/video_splash.cpp').read_text()
efi = (root / 'src/system/boot/platform/efi/video.cpp').read_text()


def extract(source, name):
    a = source.index('\n' + name + '(') + 1
    return 'void\n' + source[a:source.index('\n}', a) + 2]


fixture = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
using uint8 = uint8_t; using uint32 = uint32_t; using uint64 = uint64_t;
using efi_status = unsigned long;
#define min_c(a,b) std::min<uint64>(a,b)
constexpr efi_status EFI_SUCCESS = 0;
constexpr int EfiBltVideoToBltBuffer = 1;
struct efi_guid { uint32 a; uint16_t b,c; uint8 d[8]; };
struct efi_graphics_output_blt_pixel { uint8 Blue, Green, Red, Reserved; };
struct Info { uint32 HorizontalResolution = 1280, VerticalResolution = 800; } info;
struct Mode { struct Info* Info = &info; } mode;
struct efi_graphics_output_protocol;
efi_status Sample(efi_graphics_output_protocol*, efi_graphics_output_blt_pixel*,
 int, size_t, size_t, size_t, size_t, size_t, size_t, size_t);
struct efi_graphics_output_protocol {
 struct Mode* Mode = &mode;
 decltype(&Sample) Blt = Sample;
} output;
uint32 nvram; size_t nvramSize; efi_status nvramStatus;
uint8 corners[4]; unsigned sampled; bool missingOutput, bltFailed;
uint8 selected, sFirmwareBackground;
efi_guid sGraphicsOutputGuid;
void video_set_splash_background(uint8 shade) { selected = shade; }
struct Runtime {
 efi_status GetVariable(char16_t* name, efi_guid* guid, void*, size_t* size, void* value) {
  assert(std::memcmp(name, u"DefaultBackgroundColor", sizeof(u"DefaultBackgroundColor")) == 0);
  assert(guid->a == 0x4d1ede05 && guid->d[7] == 0x14);
  assert(*size == 4); *size = nvramSize; std::memcpy(value, &nvram, 4);
  return nvramStatus;
 }
} runtime;
struct Boot {
 efi_status LocateProtocol(efi_guid*, void*, void** p) {
  *p = missingOutput ? nullptr : &output; return missingOutput ? 1 : 0;
 }
} boot;
const char* setting; bool hasSettings = true; unsigned unloaded;
void* load_driver_settings(const char* name) {
 assert(std::strcmp(name, "vesa") == 0); return hasSettings ? &selected : nullptr;
}
const char* get_driver_parameter(void*, const char* name, void*, void*) {
 assert(std::strcmp(name, "splash_background_gray") == 0); return setting;
}
void unload_driver_settings(void*) { unloaded++; }
auto* kRuntimeServices = &runtime; auto* kBootServices = &boot;
efi_status Sample(efi_graphics_output_protocol*,
 efi_graphics_output_blt_pixel* pixel, int operation, size_t x, size_t y,
 size_t dx, size_t dy, size_t width, size_t height, size_t delta) {
 assert(operation == EfiBltVideoToBltBuffer && width == 1 && height == 1);
 assert(dx == 0 && dy == 0 && delta == 0 && sampled < 4);
 assert(x == ((sampled & 1) ? 1279 : 0));
 assert(y == ((sampled & 2) ? 799 : 0));
 pixel->Red = pixel->Green = pixel->Blue = corners[sampled++];
 return bltFailed ? 1 : 0;
}
'''
tests = r'''
int main() {
 for (unsigned width = 0; width < 20; width++) {
  for (unsigned height = 0; height < 8; height++) {
   unsigned stride = width * 4 + 12;
   for (unsigned available = 0; available <= stride * height + 1; available++) {
    std::vector<uint8> bytes(available + 32, 0x55);
    fill_gray_background(bytes.data() + 16, available, width, height, stride, 191);
    for (unsigned i = 0; i < bytes.size(); i++) {
     uint8 expected = 0x55;
     if (i >= 16 && i < available + 16) {
      unsigned offset = i - 16, row = offset / stride, col = offset % stride;
      unsigned pixel = col / 4 * 4;
      if (row < height && pixel < width * 4 && row * stride + pixel + 4 <= available)
       expected = col % 4 == 3 ? 0 : 191;
     }
     assert(bytes[i] == expected);
    }
   }
  }
 }
 uint8 image[] = {0,0,0, 255,80,0, 1,1,1, 0,0};
 replace_black_background(image, sizeof(image), 191);
 const uint8 expected[] = {191,191,191, 255,80,0, 1,1,1, 0,0};
 assert(std::memcmp(image, expected, sizeof(image)) == 0);
 uint8 black[] = {0,0,0}; replace_black_background(black, 3, 0);
 assert(black[0] == 0 && black[1] == 0 && black[2] == 0);
 for (const char* invalid : {"", "-1", "256", "99999999999999999999", "191x", " 191"}) {
  setting = invalid; selected = 77; unsigned before = unloaded;
  video_override_splash_background(); assert(selected == 77 && unloaded == before + 1);
 }
 setting = "191"; video_override_splash_background(); assert(selected == 191);
 setting = "0"; video_override_splash_background(); assert(selected == 0);
 setting = "255"; video_override_splash_background(); assert(selected == 255);
 setting = nullptr; selected = 77; video_override_splash_background(); assert(selected == 77);
 hasSettings = false; setting = "191"; video_override_splash_background(); assert(selected == 77);
 nvramSize = 4; nvram = 0xffbfbfbf; nvramStatus = 0;
 video_capture_firmware_background(); assert(selected == 191 && sampled == 0);
 nvram = 0; video_capture_firmware_background(); assert(selected == 0 && sampled == 0);
 nvramStatus = 1; std::fill(corners, corners + 4, 191);
 video_capture_firmware_background(); assert(selected == 191 && sampled == 4);
 selected = 0; sampled = 0; corners[3] = 192;
 video_capture_firmware_background(); assert(selected == 0 && sampled == 4);
 selected = 0; sampled = 0; bltFailed = true;
 video_capture_firmware_background(); assert(selected == 0 && sampled == 1);
 bltFailed = false; sampled = 0; missingOutput = true;
 video_capture_firmware_background(); assert(selected == 0 && sampled == 0);
 missingOutput = false; info.HorizontalResolution = 0;
 video_capture_firmware_background(); assert(sampled == 0);
}
'''
code = fixture + extract(generic, 'fill_gray_background') + '\n' \
    + extract(generic, 'replace_black_background') + '\n' \
    + extract(efi, 'video_capture_firmware_background') + '\n' \
    + extract(efi, 'video_override_splash_background') + tests
with tempfile.TemporaryDirectory() as directory:
    p = Path(directory) / 'background.cpp'
    p.write_text(code)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17',
        '-fsanitize=address,undefined', str(p), '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True)
print('PASS: bounded gray fill, foreground preservation, NVRAM/corners/black/failure')
