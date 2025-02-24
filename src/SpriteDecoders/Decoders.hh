#include <stdint.h>

#include <phosg/Image.hh>
#include <string>
#include <vector>
#include <unordered_map>

#include "../QuickDrawFormats.hh"

// Ambrosia-btSP-HrSp.cc
Image decode_btSP(const std::string& data, const std::vector<ColorTableEntry>& clut);
Image decode_HrSp(const std::string& data, const std::vector<ColorTableEntry>& clut);

// DarkCastle-DC2.cc
Image decode_DC2(const std::string& data);

// DarkCastle-PPCT-PSCR.cc
Image decode_PPCT(const std::string& data);
Image decode_PSCR(const std::string& data, bool is_v2);

// Factory-1img-4img-8img.cc
Image decode_1img(const std::string& data);
Image decode_4img(const std::string& data, const std::vector<ColorTableEntry>& pltt);
Image decode_8img(const std::string& data, const std::vector<ColorTableEntry>& pltt);

// Greebles-GSIF.cc
Image decode_GSIF(const std::string& data, const std::vector<ColorTableEntry>& pltt);

// Lemmings-PrinceOfPersia-SHPD.cc
enum class SHPDVersion {
  LEMMINGS_V1 = 0,
  LEMMINGS_V2,
  PRINCE_OF_PERSIA,
};
struct DecodedSHPDImage {
  int16_t origin_x;
  int16_t origin_y;
  Image image;
};
std::unordered_map<std::string, DecodedSHPDImage> decode_SHPD_collection(
    const std::string& resource_fork_contents,
    const std::string& data_fork_contents,
    const std::vector<ColorTableEntry>& clut,
    SHPDVersion version);

// PrinceOfPersia2-SHAP.cc
Image decode_SHAP(const std::string& data, const std::vector<ColorTableEntry>& ctbl);

// SimCity2000-SPRT.cc
std::vector<Image> decode_SPRT(const std::string& data, const std::vector<ColorTableEntry>& pltt);

// StepOnIt-sssf.cc
std::vector<Image> decode_sssf(const std::string& data, const std::vector<ColorTableEntry>& clut);

// SwampGas-PPic.cc
std::vector<Image> decode_PPic(const std::string& data, const std::vector<ColorTableEntry>& clut);

// TheZone-Spri.cc
Image decode_Spri(const std::string& data, const std::vector<ColorTableEntry>& clut);
