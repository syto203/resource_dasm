#include "Decoders.hh"

#include <string.h>

#include <map>
#include <phosg/Filesystem.hh>
#include <phosg/Image.hh>
#include <phosg/Strings.hh>
#include <stdexcept>

using namespace std;

Image decode_sssf_image(StringReader& r, const vector<ColorTableEntry>& clut) {
  uint16_t width = r.get_u16b();
  uint16_t height = r.get_u16b();
  r.skip(4); // apparently unused - both PPC and 68K decoders ignore this
  uint32_t data_stream_offset = r.get_u32b();

  StringReader data_r = r.sub(data_stream_offset, r.size() - data_stream_offset);

  string decoded_data;
  size_t target_size = width * height;
  while (decoded_data.size() < target_size) {
    uint8_t num_zeroes = r.get_u8();
    for (size_t x = 0; x < num_zeroes; x++) {
      if (decoded_data.size() >= target_size) {
        throw logic_error("exceeded target size during transparent segment");
      }
      decoded_data.push_back(0x00);
    }
    if (decoded_data.size() >= target_size) {
      break;
    }
    uint8_t num_data_bytes = r.get_u8();
    for (size_t x = 0; x < num_data_bytes; x++) {
      if (decoded_data.size() >= target_size) {
        throw logic_error("exceeded target size during data segment");
      }
      decoded_data.push_back(data_r.get_u8());
    }
  }

  Image ret(width, height, true);
  StringReader decoded_r(decoded_data.data(), decoded_data.size());
  for (size_t y = 0; y < height; y++) {
    for (size_t x = 0; x < width; x++) {
      uint8_t v = decoded_r.get_u8();
      if (v == 0) {
        ret.write_pixel(x, y, 0, 0, 0, 0);
      } else {
        auto c = clut.at(v).c.as8();
        ret.write_pixel(x, y, c.r, c.g, c.b);
      }
    }
  }

  return ret;
}

// sssf <- clut
// 128  <- 1001
// 129  <- 1000

vector<Image> decode_sssf(const string& data, const vector<ColorTableEntry>& clut) {
  StringReader r(data);

  uint32_t num_images = r.get_u32b();
  r.skip(8);

  map<uint32_t, ssize_t> offsets;
  while (offsets.size() < num_images) {
    offsets.emplace(r.get_u32b(), offsets.size());
  }
  offsets.emplace(data.size(), -1);

  vector<Image> ret;
  while (ret.size() < offsets.size() - 1) {
    ret.emplace_back(0, 0);
  }

  for (auto it = offsets.begin(); it != offsets.end(); it++) {
    if (it->second < 0) {
      continue;
    }
    uint32_t end_offset;
    {
      auto end_it = it;
      end_it++;
      end_offset = end_it->first;
    }
    StringReader sub_r = r.sub(it->first, end_offset - it->first);
    ret[it->second] = decode_sssf_image(sub_r, clut);
  }

  return ret;
}
