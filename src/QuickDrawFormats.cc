#include "QuickDrawFormats.hh"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <exception>
#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Image.hh>
#include <phosg/Strings.hh>
#include <phosg/Time.hh>
#include <stdexcept>
#include <vector>
#include <string>

using namespace std;



Color8::Color8(uint32_t c) : Color8(c >> 16, c >> 8, c) { }

Color8::Color8(uint8_t r, uint8_t g, uint8_t b) : r(r), g(g), b(b) { }



Color::Color(uint16_t r, uint16_t g, uint16_t b) : r(r), g(g), b(b) { }

Color8 Color::as8() const {
  return {
      static_cast<uint8_t>(this->r / 0x101),
      static_cast<uint8_t>(this->g / 0x101),
      static_cast<uint8_t>(this->b / 0x101)};
}

uint64_t Color::to_u64() const {
  return (static_cast<uint64_t>(this->r) << 32) |
         (static_cast<uint64_t>(this->g) << 16) |
         (static_cast<uint64_t>(this->b));
}



Point::Point(int16_t y, int16_t x) : y(y), x(x) { }

bool Point::operator==(const Point& other) const {
  return (this->y == other.y) && (this->x == other.x);
}

bool Point::operator!=(const Point& other) const {
  return !this->operator==(other);
}

string Point::str() const {
  return string_printf("Point(x=%hd, y=%hd)", this->x.load(), this->y.load());
}



Rect::Rect(int16_t y1, int16_t x1, int16_t y2, int16_t x2) : y1(y1), x1(x1), y2(y2), x2(x2) { }

bool Rect::operator==(const Rect& other) const {
  return (this->y1 == other.y1) && (this->x1 == other.x1) &&
         (this->y2 == other.y2) && (this->x2 == other.x2);
}

bool Rect::operator!=(const Rect& other) const {
  return !this->operator==(other);
}

bool Rect::contains(ssize_t x, ssize_t y) const {
  return ((x >= this->x1) && (x < this->x2) &&
          (y >= this->y1) && (y < this->y2));
}

bool Rect::contains(const Rect& other) const {
  return ((other.x1 >= this->x1) && (other.x1 < this->x2) &&
          (other.y1 >= this->y1) && (other.y1 < this->y2) &&
          (other.x2 >= this->x1) && (other.x2 <= this->x2) &&
          (other.y2 >= this->y1) && (other.y2 <= this->y2));
}

ssize_t Rect::width() const {
  return this->x2 - this->x1;
}

ssize_t Rect::height() const {
  return this->y2 - this->y1;
}

bool Rect::is_empty() const {
  return (this->x1 == this->x2) || (this->y1 == this->y2);
}

string Rect::str() const {
  return string_printf("Rect(x1=%hd, y1=%hd, x2=%hd, y2=%hd)",
      this->x1.load(), this->y1.load(), this->x2.load(), this->y2.load());
}



Region::Region(StringReader& r) : rendered(0, 0) {
  size_t start_offset = r.where();

  uint16_t size = r.get_u16b();
  if (size < 0x0A) {
    throw runtime_error("region cannot be smaller than 10 bytes");
  }
  if (size & 1) {
    throw runtime_error("region size is not even");
  }

  this->rect = r.get<Rect>();

  while (r.where() < start_offset + size) {
    int16_t y = r.get_u16b();
    if (y == 0x7FFF) {
      break;
    }
    while (r.where() < start_offset + size) {
      int16_t x = r.get_u16b();
      if (x == 0x7FFF) {
        break;
      }
      this->inversions.emplace(this->signature_for_inversion_point(x, y));
    }
  }

  if (r.where() != start_offset + size) {
    throw runtime_error("region ends before all data is parsed");
  }
}

Region::Region(const Rect& r) : rect(r), rendered(0, 0) { }

string Region::serialize() const {
  vector<Point> points;
  points.reserve(this->inversions.size());
  for (int32_t pt : this->inversions) {
    points.emplace_back(this->inversion_point_for_signature(pt));
  }
  sort(points.begin(), points.end(), [](const Point& a, const Point& b) -> bool {
    if (a.y < b.y) {
      return true;
    } else if (a.y > b.y) {
      return false;
    }
    return a.x < b.x;
  });

  StringWriter w;
  w.put_u16(0); // This will be overwritten at the end
  w.put(this->rect);

  if (!points.empty()) {
    int16_t prev_y = points[0].y;
    w.put_u16b(points[0].y);
    for (const auto& pt : points) {
      if (pt.y != prev_y) {
        w.put_u16b(0x7FFF);
        prev_y = pt.y;
      }
      w.put_u16b(pt.x);
    }
    w.put_u32b(0x7FFF7FFF);
  }

  // Write the size field
  w.pput_u16b(0, w.str().size());

  return w.str();
}

int32_t Region::signature_for_inversion_point(int16_t x, int16_t y) {
  return (static_cast<int32_t>(x) << 16) | y;
}

Point Region::inversion_point_for_signature(int32_t s) {
  return Point(s & 0xFFFF, (s >> 16) & 0xFFFF);
}

bool Region::is_inversion_point(int16_t x, int16_t y) const {
  return this->inversions.count(this->signature_for_inversion_point(x, y));
}

const Image& Region::render() const {
  size_t width = this->rect.width();
  size_t height = this->rect.height();
  if (this->rendered.get_width() != width || this->rendered.get_height() != height) {
    this->rendered = Image(width, height);
    this->rendered.clear(0xFF, 0xFF, 0xFF);

    // TODO: there's probably a lower-time-complexity way to do this
    for (int32_t pt : this->inversions) {
      int16_t x = (pt >> 16) & 0xFFFF;
      int16_t y = pt & 0xFFFF;
      for (size_t yy = y; yy < height; yy++) {
        for (size_t xx = x; xx < width; xx++) {
          uint64_t r;
          this->rendered.read_pixel(xx, yy, &r, nullptr, nullptr);
          this->rendered.write_pixel(xx, yy, r ^ 0xFF, r ^ 0xFF, r ^ 0xFF);
        }
      }
    }
  }

  return this->rendered;
}

bool Region::contains(int16_t x, int16_t y) const {
  if (x < this->rect.x1 || x >= this->rect.x2 ||
      y < this->rect.y1 || y >= this->rect.y2) {
    return false;
  }

  // We could render the region, or we could count the number of inversions that
  // are both above and to the left of the point in question. Rendering is slow
  // the first time, but makes each subsequent contains() call constant-time,
  // whereas counting is linear every time contains() is called. As a heuristic,
  // if the bounds rect area is 1 million pixels or more, we assume that
  // checking inversion points will be faster on average than rendering.
  // Notably, some PICTs have insanely large clip regions defined for packed
  // copy_bits opcodes, even though the PICT's overall bounds rect is fairly
  // small - this heuristic makes those fast to render.
  if (this->rect.width() * this->rect.height() >= 1000000) {
    bool contained = true;
    for (int32_t pt : this->inversions) {
      int16_t pt_x = (pt >> 16) & 0xFFFF;
      int16_t pt_y = pt & 0xFFFF;
      if (pt_x <= x && pt_y <= y) {
        contained = !contained;
      }
    }
    return contained;

  } else {
    this->render();
    uint64_t r;
    this->rendered.read_pixel(x - this->rect.x1, y - this->rect.y1, &r, nullptr, nullptr);
    return (r != 0);
  }
}



Fixed::Fixed() : whole(0), decimal(0) { }

Fixed::Fixed(int16_t whole, uint16_t decimal) : whole(whole), decimal(decimal) { }



Pattern::Pattern(uint64_t pattern) : pattern(pattern) { }

bool Pattern::pixel_at(uint8_t x, uint8_t y) const {
  return (this->rows[y & 7] >> (7 - (x & 7))) & 1;
}



Image decode_monochrome_image(const void* vdata, size_t size, size_t w,
    size_t h, size_t row_bytes) {
  if (row_bytes == 0) {
    if (w & 7) {
      throw runtime_error("width must be a multiple of 8 unless row_bytes is specified");
    }
    row_bytes = w / 8;
  }
  if (size != row_bytes * h) {
    throw runtime_error(string_printf(
        "incorrect data size: expected %zu bytes, got %zu bytes", row_bytes * h, size));
  }
  const uint8_t* data = reinterpret_cast<const uint8_t*>(vdata);

  Image result(w, h);
  for (size_t y = 0; y < h; y++) {
    for (size_t x = 0; x < w; x += 8) {
      uint8_t pixels = data[y * row_bytes + x / 8];
      size_t z_limit = ((x + 8) <= w) ? 8 : w - x;
      for (size_t z = 0; z < z_limit; z++) {
        uint8_t value = (pixels & 0x80) ? 0x00 : 0xFF;
        pixels <<= 1;
        result.write_pixel(x + z, y, value, value, value);
      }
    }
  }

  return result;
}

Image decode_monochrome_image_masked(const void* vdata, size_t size,
    size_t w, size_t h) {
  const uint8_t* image_data = reinterpret_cast<const uint8_t*>(vdata);
  const uint8_t* mask_data = image_data + (w * h / 8);

  if (w & 7) {
    throw runtime_error("width is not a multiple of 8");
  }
  if (size != w * h / 4) {
    throw runtime_error(string_printf(
        "incorrect data size: expected %zu bytes, got %zu bytes", w * h / 4, size));
  }

  Image result(w, h, true);
  for (size_t y = 0; y < h; y++) {
    for (size_t x = 0; x < w; x += 8) {
      uint8_t pixels = image_data[y * w / 8 + x / 8];
      uint8_t mask_pixels = mask_data[y * w / 8 + x / 8];
      for (size_t z = 0; z < 8; z++) {
        uint8_t value = (pixels & 0x80) ? 0x00 : 0xFF;
        uint8_t mask_value = (mask_pixels & 0x80) ? 0xFF : 0x00;
        pixels <<= 1;
        mask_pixels <<= 1;
        result.write_pixel(x + z, y, value, value, value, mask_value);
      }
    }
  }

  return result;
}


const vector<Color8> default_icon_color_table_4bit = {
  0xFFFFFF, 0xFFFF00, 0xFF6600, 0xDD0000, 0xFF0099, 0x330099, 0x0000DD, 0x0099FF,
  0x00BB00, 0x006600, 0x663300, 0x996633, 0xCCCCCC, 0x888888, 0x444444, 0x000000,
};

const vector<Color8> default_icon_color_table_8bit = {
  0xFFFFFF, 0xFFFFCC, 0xFFFF99, 0xFFFF66, 0xFFFF33, 0xFFFF00,
  0xFFCCFF, 0xFFCCCC, 0xFFCC99, 0xFFCC66, 0xFFCC33, 0xFFCC00,
  0xFF99FF, 0xFF99CC, 0xFF9999, 0xFF9966, 0xFF9933, 0xFF9900,
  0xFF66FF, 0xFF66CC, 0xFF6699, 0xFF6666, 0xFF6633, 0xFF6600,
  0xFF33FF, 0xFF33CC, 0xFF3399, 0xFF3366, 0xFF3333, 0xFF3300,
  0xFF00FF, 0xFF00CC, 0xFF0099, 0xFF0066, 0xFF0033, 0xFF0000,
  0xCCFFFF, 0xCCFFCC, 0xCCFF99, 0xCCFF66, 0xCCFF33, 0xCCFF00,
  0xCCCCFF, 0xCCCCCC, 0xCCCC99, 0xCCCC66, 0xCCCC33, 0xCCCC00,
  0xCC99FF, 0xCC99CC, 0xCC9999, 0xCC9966, 0xCC9933, 0xCC9900,
  0xCC66FF, 0xCC66CC, 0xCC6699, 0xCC6666, 0xCC6633, 0xCC6600,
  0xCC33FF, 0xCC33CC, 0xCC3399, 0xCC3366, 0xCC3333, 0xCC3300,
  0xCC00FF, 0xCC00CC, 0xCC0099, 0xCC0066, 0xCC0033, 0xCC0000,
  0x99FFFF, 0x99FFCC, 0x99FF99, 0x99FF66, 0x99FF33, 0x99FF00,
  0x99CCFF, 0x99CCCC, 0x99CC99, 0x99CC66, 0x99CC33, 0x99CC00,
  0x9999FF, 0x9999CC, 0x999999, 0x999966, 0x999933, 0x999900,
  0x9966FF, 0x9966CC, 0x996699, 0x996666, 0x996633, 0x996600,
  0x9933FF, 0x9933CC, 0x993399, 0x993366, 0x993333, 0x993300,
  0x9900FF, 0x9900CC, 0x990099, 0x990066, 0x990033, 0x990000,
  0x66FFFF, 0x66FFCC, 0x66FF99, 0x66FF66, 0x66FF33, 0x66FF00,
  0x66CCFF, 0x66CCCC, 0x66CC99, 0x66CC66, 0x66CC33, 0x66CC00,
  0x6699FF, 0x6699CC, 0x669999, 0x669966, 0x669933, 0x669900,
  0x6666FF, 0x6666CC, 0x666699, 0x666666, 0x666633, 0x666600,
  0x6633FF, 0x6633CC, 0x663399, 0x663366, 0x663333, 0x663300,
  0x6600FF, 0x6600CC, 0x660099, 0x660066, 0x660033, 0x660000,
  0x33FFFF, 0x33FFCC, 0x33FF99, 0x33FF66, 0x33FF33, 0x33FF00,
  0x33CCFF, 0x33CCCC, 0x33CC99, 0x33CC66, 0x33CC33, 0x33CC00,
  0x3399FF, 0x3399CC, 0x339999, 0x339966, 0x339933, 0x339900,
  0x3366FF, 0x3366CC, 0x336699, 0x336666, 0x336633, 0x336600,
  0x3333FF, 0x3333CC, 0x333399, 0x333366, 0x333333, 0x333300,
  0x3300FF, 0x3300CC, 0x330099, 0x330066, 0x330033, 0x330000,
  0x00FFFF, 0x00FFCC, 0x00FF99, 0x00FF66, 0x00FF33, 0x00FF00,
  0x00CCFF, 0x00CCCC, 0x00CC99, 0x00CC66, 0x00CC33, 0x00CC00,
  0x0099FF, 0x0099CC, 0x009999, 0x009966, 0x009933, 0x009900,
  0x0066FF, 0x0066CC, 0x006699, 0x006666, 0x006633, 0x006600,
  0x0033FF, 0x0033CC, 0x003399, 0x003366, 0x003333, 0x003300,
  0x0000FF, 0x0000CC, 0x000099, 0x000066, 0x000033, // Note: no black here

  0xEE0000, 0xDD0000, 0xBB0000, 0xAA0000, 0x880000,
  0x770000, 0x550000, 0x440000, 0x220000, 0x110000,
  0x00EE00, 0x00DD00, 0x00BB00, 0x00AA00, 0x008800,
  0x007700, 0x005500, 0x004400, 0x002200, 0x001100,
  0x0000EE, 0x0000DD, 0x0000BB, 0x0000AA, 0x000088,
  0x000077, 0x000055, 0x000044, 0x000022, 0x000011,
  0xEEEEEE, 0xDDDDDD, 0xBBBBBB, 0xAAAAAA, 0x888888,
  0x777777, 0x555555, 0x444444, 0x222222, 0x111111,
  0x000000,
};

Image decode_4bit_image(
    const void* vdata,
    size_t size,
    size_t w,
    size_t h,
    const std::vector<Color8>* clut) {
  if (w & 1) {
    throw runtime_error("width is not even");
  }
  if (size != w * h / 2) {
    throw runtime_error(string_printf(
        "incorrect data size: expected %zu bytes, got %zu bytes", w * h / 2, size));
  }
  const uint8_t* data = reinterpret_cast<const uint8_t*>(vdata);

  Image result(w, h);
  for (size_t y = 0; y < h; y++) {
    for (size_t x = 0; x < w; x += 2) {
      uint8_t indexes = data[y * w / 2 + x / 2];
      if (clut) {
        const Color8& left_c = clut->at((indexes >> 4) & 0x0F);
        const Color8& right_c = clut->at(indexes & 0x0F);
        result.write_pixel(x, y, left_c.r, left_c.g, left_c.b);
        result.write_pixel(x + 1, y, right_c.r, right_c.g, right_c.b);
      } else {
        uint8_t left_v = (indexes & 0xF0) | ((indexes & 0xF0) >> 4);
        uint8_t right_v = ((indexes & 0x0F) << 4) | (indexes & 0x0F);
        result.write_pixel(x, y, left_v, left_v, left_v);
        result.write_pixel(x + 1, y, right_v, right_v, right_v);
      }
    }
  }

  return result;
}

Image decode_8bit_image(
    const void* vdata,
    size_t size,
    size_t w,
    size_t h,
    const std::vector<Color8>* clut) {
  if (size != w * h) {
    throw runtime_error(string_printf(
        "incorrect data size: expected %zu bytes, got %zu bytes", w * h, size));
  }
  const uint8_t* data = reinterpret_cast<const uint8_t*>(vdata);

  Image result(w, h);
  for (size_t y = 0; y < h; y++) {
    for (size_t x = 0; x < w; x++) {
      if (clut) {
        const Color8& c = clut->at(data[y * w + x]);
        result.write_pixel(x, y, c.r, c.g, c.b);
      } else {
        uint8_t v = data[y * w + x];
        result.write_pixel(x, y, v, v, v);
      }
    }
  }

  return result;
}

uint32_t PixelMapData::lookup_entry(uint16_t pixel_size, size_t row_bytes, size_t x, size_t y) const {
  switch (pixel_size) {
    case 1:
      return (this->data[(y * row_bytes) + (x / 8)] >> (7 - (x & 7))) & 1;
    case 2:
      return (this->data[(y * row_bytes) + (x / 4)] >> (6 - ((x & 3) * 2))) & 3;
    case 4:
      return (this->data[(y * row_bytes) + (x / 2)] >> (4 - ((x & 1) * 4))) & 15;
    case 8:
      return this->data[(y * row_bytes) + x];
    case 16:
      return *reinterpret_cast<const be_uint16_t*>(&this->data[(y * row_bytes) + (x * 2)]);
    case 32:
      return *reinterpret_cast<const be_uint32_t*>(&this->data[(y * row_bytes) + (x * 4)]);
    default:
      throw runtime_error("pixel size is not 1, 2, 4, 8, 16, or 32 bits");
  }
}

size_t PixelMapData::size(uint16_t row_bytes, size_t h) {
  return row_bytes * h;
}

shared_ptr<ColorTable> ColorTable::from_entries(
    const vector<ColorTableEntry>& entries) {
  if (entries.empty()) {
    throw logic_error("cannot construct an empty color table");
  }

  size_t size = sizeof(ColorTable) + entries.size() * sizeof(ColorTableEntry);
  shared_ptr<ColorTable> ret(reinterpret_cast<ColorTable*>(malloc(size)), free);
  ret->seed = 0;
  ret->flags = 0;
  ret->num_entries = entries.size() - 1;
  for (size_t x = 0; x < entries.size(); x++) {
    ret->entries[x] = entries[x];
  }
  return ret;
}

size_t ColorTable::size() const {
  return sizeof(ColorTable) + (this->num_entries + 1) * sizeof(ColorTableEntry);
}

uint32_t ColorTable::get_num_entries() const {
  return this->num_entries + 1;
}

const ColorTableEntry* ColorTable::get_entry(int16_t id) const {
  // It looks like if the highest flag is set (8000) then id is just the
  // index, not the color number, and we should ignore the color_num field
  if (this->flags & 0x8000) {
    if (id <= this->num_entries) {
      return &this->entries[id];
    }
  } else {
    for (int32_t x = 0; x <= this->num_entries; x++) {
      if (this->entries[x].color_num == id) {
        return &this->entries[x];
      }
    }
  }
  return nullptr;
}

Image decode_color_image(const PixelMapHeader& header,
    const PixelMapData& pixel_map, const ColorTable* ctable,
    const PixelMapData* mask_map, size_t mask_row_bytes) {

  // According to apple's docs, pixel_type is 0 for indexed color and 0x0010 for
  // direct color, even for 32-bit images
  if (header.pixel_type != 0 && header.pixel_type != 0x0010) {
    throw runtime_error("unknown pixel type");
  }
  if (header.pixel_type == 0 && !ctable) {
    throw runtime_error("color table must be given for indexed-color image");
  }

  // We only support 3-component direct color images (RGB)
  if (header.pixel_type == 0x0010 && header.component_count != 3) {
    throw runtime_error("unsupported channel count");
  }
  if (header.pixel_type == 0x0010 && header.pixel_size == 0x0010 && header.component_size != 5) {
    throw runtime_error("unsupported 16-bit channel width");
  }
  if (header.pixel_type == 0x0010 && header.pixel_size == 0x0020 && header.component_size != 8) {
    throw runtime_error("unsupported 32-bit channel width");
  }

  size_t width = header.bounds.width();
  size_t height = header.bounds.height();
  Image img(width, height, (mask_map != nullptr));
  for (size_t y = 0; y < height; y++) {
    for (size_t x = 0; x < width; x++) {
      uint32_t color_id = pixel_map.lookup_entry(header.pixel_size,
          header.flags_row_bytes & 0x3FFF, x, y);

      if (header.pixel_type == 0) {
        const auto* e = ctable->get_entry(color_id);
        if (e) {
          uint8_t alpha = 0xFF;
          if (mask_map) {
            alpha = mask_map->lookup_entry(1, mask_row_bytes, x, y) ? 0xFF : 0x00;
          }
          img.write_pixel(x, y, e->c.r >> 8, e->c.g >> 8, e->c.b >> 8, alpha);

        // Some rare pixmaps appear to use 0xFF as black, so we handle that
        // manually here. TODO: figure out if this is the right behavior
        } else if (color_id == static_cast<uint32_t>((1 << header.pixel_size) - 1)) {
          img.write_pixel(x, y, 0, 0, 0, 0xFF);

        } else {
          throw runtime_error(string_printf("color %" PRIX32 " not found in color map", color_id));
        }

      } else if (header.pixel_size == 0x0010 && header.component_size == 5) {
        // xrgb1555. We cheat by filling the lower 3 bits of each channel with
        // the upper 3 bits; this makes white (1F) actually white and black
        // actually black when expanded to 8-bit channels
        uint8_t r = ((color_id >> 7) & 0xF8) | ((color_id >> 12) & 0x07);
        uint8_t g = ((color_id >> 2) & 0xF8) | ((color_id >> 7) & 0x07);
        uint8_t b = ((color_id << 3) & 0xF8) | ((color_id >> 2) & 0x07);
        img.write_pixel(x, y, r, g, b, 0xFF);

      } else if (header.pixel_size == 0x0020 && header.component_size == 8) {
        // xrgb8888
        img.write_pixel(x, y, (color_id >> 16) & 0xFF, (color_id >> 8) & 0xFF,
            color_id & 0xFF, 0xFF);

      } else {
        throw runtime_error("unsupported pixel format");
      }
    }
  }
  return img;
}

Image apply_alpha_from_mask(const Image& img, const Image& mask) {
  if ((img.get_width() != mask.get_width()) || (img.get_height() != mask.get_height())) {
    throw runtime_error("image and mask dimensions are unequal");
  }

  Image ret(img.get_width(), img.get_height(), true);
  for (size_t y = 0; y < img.get_height(); y++) {
    for (size_t x = 0; x < img.get_width(); x++) {
      uint64_t r, g, b, a;
      img.read_pixel(x, y, &r, &g, &b, nullptr);
      mask.read_pixel(x, y, nullptr, nullptr, nullptr, &a);
      ret.write_pixel(x, y, r, g, b, a);
    }
  }
  return ret;
}

vector<Color8> to_color8(const vector<Color>& cs) {
  vector<Color8> ret;
  for (const auto& c : cs) {
    ret.emplace_back(c.as8());
  }
  return ret;
}

vector<Color8> to_color8(const vector<ColorTableEntry>& cs) {
  vector<Color8> ret;
  for (const auto& c : cs) {
    ret.emplace_back(c.c.as8());
  }
  return ret;
}

vector<Color8> to_color8(const vector<PaletteEntry>& cs) {
  vector<Color8> ret;
  for (const auto& c : cs) {
    ret.emplace_back(c.c.as8());
  }
  return ret;
}
