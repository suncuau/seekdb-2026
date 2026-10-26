/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/dml/ob_file_table_reader.h"

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cmath>
#include <strings.h>
#include <glob.h>

#include "lib/allocator/page_arena.h"
#include "lib/utility/ob_print_utils.h"

namespace oceanbase
{
using namespace common;
namespace sql
{
namespace
{

const int64_t FILE_MAX_SIZE = 1LL << 30; // 1GB

const int32_t PARQUET_TYPE_BOOLEAN = 0;
const int32_t PARQUET_TYPE_INT32 = 1;
const int32_t PARQUET_TYPE_INT64 = 2;
const int32_t PARQUET_TYPE_INT96 = 3;
const int32_t PARQUET_TYPE_FLOAT = 4;
const int32_t PARQUET_TYPE_DOUBLE = 5;
const int32_t PARQUET_TYPE_BYTE_ARRAY = 6;
const int32_t PARQUET_TYPE_FIXED_LEN_BYTE_ARRAY = 7;

const int32_t PARQUET_PAGE_DATA = 0;
const int32_t PARQUET_PAGE_DICTIONARY = 2;
const int32_t PARQUET_PAGE_DATA_V2 = 3;

const int32_t PARQUET_ENC_PLAIN = 0;
const int32_t PARQUET_ENC_PLAIN_DICTIONARY = 2;
const int32_t PARQUET_ENC_RLE = 3;
const int32_t PARQUET_ENC_RLE_DICTIONARY = 8;

const int32_t PARQUET_CODEC_UNCOMPRESSED = 0;
const int32_t PARQUET_CODEC_SNAPPY = 1;

const int32_t PARQUET_CONVERTED_UTF8 = 0;
const int32_t PARQUET_CONVERTED_DATE = 6;
const int32_t PARQUET_CONVERTED_TIMESTAMP_MILLIS = 9;
const int32_t PARQUET_CONVERTED_TIMESTAMP_MICROS = 10;

const int32_t PARQUET_REP_REQUIRED = 0;
const int32_t PARQUET_REP_OPTIONAL = 1;

inline bool is_digit_char(const char c) { return c >= '0' && c <= '9'; }

inline bool is_space_char(const char c)
{
  return ' ' == c || '\t' == c || '\r' == c;
}

inline int64_t read_le_varint(const char *buf, const int64_t len, int64_t &pos)
{
  uint64_t v = 0;
  int shift = 0;
  while (pos < len && shift <= 63) {
    const uint8_t b = static_cast<uint8_t>(buf[pos++]);
    v |= static_cast<uint64_t>(b & 0x7f) << shift;
    if (0 == (b & 0x80)) {
      break;
    }
    shift += 7;
  }
  return static_cast<int64_t>(v);
}

inline int32_t read_i32(const char *p)
{
  int32_t v = 0;
  MEMCPY(&v, p, 4);
  return v;
}

inline int64_t read_i64(const char *p)
{
  int64_t v = 0;
  MEMCPY(&v, p, 8);
  return v;
}

inline float read_f32(const char *p)
{
  float v = 0;
  MEMCPY(&v, p, 4);
  return v;
}

inline double read_f64(const char *p)
{
  double v = 0;
  MEMCPY(&v, p, 8);
  return v;
}

bool is_bool_str(const ObString &str, bool &value)
{
  bool ok = true;
  if (4 == str.length() && 0 == ::strncasecmp(str.ptr(), "true", 4)) {
    value = true;
  } else if (5 == str.length() && 0 == ::strncasecmp(str.ptr(), "false", 5)) {
    value = false;
  } else {
    ok = false;
  }
  return ok;
}

bool is_int_str(const ObString &str, int64_t &value)
{
  bool ok = true;
  int64_t i = 0;
  int64_t sign = 1;
  uint64_t result = 0;
  int64_t digit_cnt = 0;
  if (i < str.length() && ('+' == str.ptr()[i] || '-' == str.ptr()[i])) {
    sign = ('-' == str.ptr()[i]) ? -1 : 1;
    ++i;
  }
  for (; ok && i < str.length(); ++i) {
    if (!is_digit_char(str.ptr()[i])) {
      ok = false;
    } else {
      result = result * 10 + static_cast<uint64_t>(str.ptr()[i] - '0');
      ++digit_cnt;
    }
  }
  if (0 == digit_cnt || digit_cnt > 18) {
    ok = false;
  }
  if (ok) {
    value = static_cast<int64_t>(result) * sign;
  }
  return ok;
}

bool is_double_str(const ObString &str, double &value)
{
  bool ok = true;
  char buf[64];
  if (0 == str.length() || str.length() >= static_cast<int64_t>(sizeof(buf))) {
    ok = false;
  } else {
    MEMCPY(buf, str.ptr(), str.length());
    buf[str.length()] = '\0';
    char *end = NULL;
    errno = 0;
    const double d = ::strtod(buf, &end);
    if (end != buf + str.length() || 0 != errno || !std::isfinite(d) ||
        is_space_char(buf[0])) {
      ok = false;
    } else {
      value = d;
    }
  }
  return ok;
}

bool is_date_str(const ObString &str)
{
  bool ok = true;
  const char *p = str.ptr();
  if (10 != str.length() || '-' != p[4] || '-' != p[7]) {
    ok = false;
  }
  for (int64_t i = 0; ok && i < 10; ++i) {
    if (4 != i && 7 != i && !is_digit_char(p[i])) {
      ok = false;
    }
  }
  if (ok) {
    const int32_t year = (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
    const int32_t month = (p[5] - '0') * 10 + (p[6] - '0');
    const int32_t day = (p[8] - '0') * 10 + (p[9] - '0');
    if (year < 1 || month < 1 || month > 12 || day < 1 || day > 31) {
      ok = false;
    }
  }
  return ok;
}

bool is_datetime_str(const ObString &str)
{
  bool ok = true;
  const char *p = str.ptr();
  if (str.length() < 19 || '-' != p[4] || '-' != p[7] ||
      (' ' != p[10] && 'T' != p[10]) || ':' != p[13] || ':' != p[16]) {
    ok = false;
  }
  for (int64_t i = 0; ok && i < 19; ++i) {
    if (4 != i && 7 != i && 10 != i && 13 != i && 16 != i && !is_digit_char(p[i])) {
      ok = false;
    }
  }
  if (ok && str.length() > 19 && '.' != p[19]) {
    ok = false;
  }
  if (ok) {
    const int32_t month = (p[5] - '0') * 10 + (p[6] - '0');
    const int32_t day = (p[8] - '0') * 10 + (p[9] - '0');
    const int32_t hour = (p[11] - '0') * 10 + (p[12] - '0');
    const int32_t minute = (p[14] - '0') * 10 + (p[15] - '0');
    const int32_t second = (p[17] - '0') * 10 + (p[18] - '0');
    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 59) {
      ok = false;
    }
  }
  return ok;
}

int snappy_uncompress(const char *src, const int64_t src_len, char *dst, const int64_t dst_len)
{
  int ret = OB_SUCCESS;
  int64_t s = 0;
  int64_t d = 0;
  int64_t need = 0;
  int shift = 0;
  uint64_t ulen = 0;
  while (s < src_len && shift <= 63) {
    const uint8_t b = static_cast<uint8_t>(src[s++]);
    ulen |= static_cast<uint64_t>(b & 0x7f) << shift;
    if (0 == (b & 0x80)) {
      break;
    }
    shift += 7;
  }
  need = static_cast<int64_t>(ulen);
  if (need < 0 || need > dst_len) {
    ret = OB_INVALID_ARGUMENT;
  }
  while (OB_SUCC(ret) && s < src_len) {
    const uint8_t tag = static_cast<uint8_t>(src[s++]);
    const int32_t t = tag & 0x03;
    int64_t n = 0;
    int64_t offset = 0;
    if (0 == t) {
      n = static_cast<int64_t>(tag >> 2) + 1;
      if (s + n > src_len || d + n > dst_len) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        MEMCPY(dst + d, src + s, n);
        s += n;
        d += n;
      }
    } else {
      if (1 == t) {
        n = static_cast<int64_t>((tag >> 2) & 0x07) + 4;
        if (s + 1 > src_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          offset = (static_cast<int64_t>(tag >> 5) << 8) | static_cast<int64_t>(static_cast<uint8_t>(src[s]));
          s += 1;
        }
      } else if (2 == t) {
        n = static_cast<int64_t>(tag >> 2) + 1;
        if (s + 2 > src_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          offset = static_cast<int64_t>(static_cast<uint8_t>(src[s])) |
                   (static_cast<int64_t>(static_cast<uint8_t>(src[s + 1])) << 8);
          s += 2;
        }
      } else {
        n = static_cast<int64_t>(tag >> 2) + 1;
        if (s + 4 > src_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          offset = static_cast<int64_t>(static_cast<uint32_t>(read_i32(src + s)));
          s += 4;
        }
      }
      if (OB_SUCC(ret)) {
        if (offset <= 0 || offset > d || d + n > dst_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          for (int64_t i = 0; i < n; ++i) {
            dst[d + i] = dst[d - offset + i];
          }
          d += n;
        }
      }
    }
  }
  if (OB_SUCC(ret) && d != need) {
    ret = OB_INVALID_ARGUMENT;
  }
  return OB_SUCC(ret) ? static_cast<int>(d) : -1;
}

// decode the RLE/bit-packed hybrid encoding used by parquet definition levels
// and by dictionary indexes
bool rle_bitpacked_decode(const char *buf, const int64_t len, const int32_t bit_width,
                          const int64_t count, uint32_t *out)
{
  bool ok = true;
  int64_t pos = 0;
  int64_t produced = 0;
  if (bit_width < 0 || bit_width > 32) {
    ok = false;
  }
  while (ok && produced < count && pos < len) {
    const uint64_t header = static_cast<uint64_t>(read_le_varint(buf, len, pos));
    if (header & 1) {
      const int64_t groups = static_cast<int64_t>(header >> 1);
      const int64_t total = groups * 8;
      const int64_t bytes = groups * bit_width;
      if (bytes < 0 || pos + bytes > len) {
        ok = false;
        break;
      }
      int64_t bit_pos = 0;
      for (int64_t i = 0; ok && i < total; ++i) {
        uint64_t v = 0;
        for (int32_t b = 0; b < bit_width; ++b) {
          const int64_t byte_idx = pos + (bit_pos >> 3);
          const int32_t bit_idx = static_cast<int32_t>(bit_pos & 7);
          if (byte_idx < len && (static_cast<uint8_t>(buf[byte_idx]) >> bit_idx) & 0x01) {
            v |= static_cast<uint64_t>(1) << b;
          }
          ++bit_pos;
        }
        if (produced < count) {
          out[produced++] = static_cast<uint32_t>(v);
        }
      }
      pos += bytes;
    } else {
      const int64_t run = static_cast<int64_t>(header >> 1);
      const int32_t nbytes = (bit_width + 7) / 8;
      if (pos + nbytes > len) {
        ok = false;
        break;
      }
      uint32_t v = 0;
      for (int32_t i = 0; i < nbytes; ++i) {
        v |= static_cast<uint32_t>(static_cast<uint8_t>(buf[pos + i])) << (8 * i);
      }
      pos += nbytes;
      for (int64_t i = 0; i < run && produced < count; ++i) {
        out[produced++] = v;
      }
    }
  }
  if (ok && produced < count) {
    for (int64_t i = produced; i < count; ++i) {
      out[i] = 0;
    }
  }
  return ok;
}

struct ParquetCompactReader
{
  ParquetCompactReader(const char *buf, const int64_t len) : buf_(buf), len_(len), pos_(0) {}
  bool valid() const { return pos_ >= 0 && pos_ <= len_; }
  void invalidate() { pos_ = len_ + 1; }
  uint8_t read_byte()
  {
    uint8_t b = 0;
    if (pos_ >= 0 && pos_ < len_) {
      b = static_cast<uint8_t>(buf_[pos_++]);
    } else {
      invalidate();
    }
    return b;
  }
  uint64_t read_varint()
  {
    const int64_t v = read_le_varint(buf_, len_, pos_);
    if (!valid()) {
      invalidate();
      return 0;
    }
    return static_cast<uint64_t>(v);
  }
  int64_t read_zigzag()
  {
    const uint64_t v = read_varint();
    return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
  }
  bool read_binary(int32_t &size, const char *&ptr)
  {
    size = 0;
    ptr = NULL;
    const uint64_t n = read_varint();
    if (!valid() || pos_ < 0 || static_cast<uint64_t>(len_ - pos_) < n) {
      invalidate();
      return false;
    }
    size = static_cast<int32_t>(n);
    ptr = buf_ + pos_;
    pos_ += static_cast<int64_t>(n);
    return true;
  }
  bool read_field(uint8_t &type, int16_t &id, int16_t &last_id)
  {
    bool ok = true;
    const uint8_t b = read_byte();
    if (!valid() || 0 == b) {
      ok = false;
    } else {
      type = b & 0x0f;
      const uint8_t delta = b >> 4;
      if (0 != delta) {
        id = static_cast<int16_t>(last_id + delta);
      } else {
        id = static_cast<int16_t>(read_zigzag());
      }
      last_id = id;
      ok = valid();
    }
    return ok;
  }
  bool skip(const uint8_t type)
  {
    bool ok = true;
    switch (type) {
      case 1:
      case 2: {
        break;
      }
      case 3: {
        read_byte();
        break;
      }
      case 4:
      case 5:
      case 6: {
        read_varint();
        break;
      }
      case 7: {
        skip_bytes(8);
        break;
      }
      case 8: {
        int32_t n = 0;
        const char *p = NULL;
        read_binary(n, p);
        break;
      }
      case 9:
      case 10: {
        const uint8_t h = read_byte();
        int32_t n = h >> 4;
        const uint8_t et = h & 0x0f;
        if (15 == n) {
          n = static_cast<int32_t>(read_varint());
        }
        for (int32_t i = 0; ok && i < n; ++i) {
          ok = skip(et);
        }
        break;
      }
      case 11: {
        const int64_t n = static_cast<int64_t>(read_varint());
        if (n > 0 && ok) {
          const uint8_t kv = read_byte();
          const uint8_t kt = kv >> 4;
          const uint8_t vt = kv & 0x0f;
          for (int64_t i = 0; ok && i < n; ++i) {
            ok = skip(kt) && skip(vt);
          }
        }
        break;
      }
      case 12: {
        int16_t last = 0;
        uint8_t t = 0;
        int16_t id = 0;
        while (ok && read_field(t, id, last)) {
          ok = skip(t);
        }
        break;
      }
      default: {
        ok = false;
        break;
      }
    }
    if (!valid()) {
      ok = false;
    }
    return ok;
  }
  void skip_bytes(const int64_t n)
  {
    if (n >= 0 && pos_ >= 0 && pos_ + n <= len_) {
      pos_ += n;
    } else {
      invalidate();
    }
  }
  const char *buf_;
  int64_t len_;
  int64_t pos_;
};

bool read_list_header(ParquetCompactReader &r, uint8_t &elem_type, int32_t &count)
{
  const uint8_t h = r.read_byte();
  elem_type = h & 0x0f;
  count = h >> 4;
  if (15 == count) {
    count = static_cast<int32_t>(r.read_varint());
  }
  return r.valid() && count >= 0;
}

struct ParquetSchemaElement
{
  ParquetSchemaElement()
      : type_(-1), repetition_(PARQUET_REP_REQUIRED), num_children_(0),
        converted_type_(-1), type_length_(0), name_() {}
  int32_t type_;
  int32_t repetition_;
  int32_t num_children_;
  int32_t converted_type_;
  int32_t type_length_;
  ObString name_;
};

struct ParquetChunk
{
  ParquetChunk()
      : col_idx_(-1), physical_type_(-1), codec_(PARQUET_CODEC_UNCOMPRESSED),
        num_values_(0), total_compressed_size_(0), data_page_offset_(0),
        dictionary_page_offset_(-1), row_offset_(0) {}
  int32_t col_idx_;
  int32_t physical_type_;
  int32_t codec_;
  int64_t num_values_;
  int64_t total_compressed_size_;
  int64_t data_page_offset_;
  int64_t dictionary_page_offset_;
  int64_t row_offset_;
};

struct ParquetPageHeader
{
  ParquetPageHeader()
      : type_(0), uncompressed_page_size_(0), compressed_page_size_(0),
        num_values_(0), encoding_(0), num_nulls_(0), num_rows_(0),
        definition_levels_byte_length_(0), repetition_levels_byte_length_(0),
        is_compressed_(true) {}
  int32_t type_;
  int32_t uncompressed_page_size_;
  int32_t compressed_page_size_;
  int32_t num_values_;
  int32_t encoding_;
  int32_t num_nulls_;
  int32_t num_rows_;
  int32_t definition_levels_byte_length_;
  int32_t repetition_levels_byte_length_;
  bool is_compressed_;
};

bool parse_schema_element(ParquetCompactReader &r, ParquetSchemaElement &element)
{
  bool ok = true;
  int16_t last_id = 0;
  uint8_t type = 0;
  int16_t id = 0;
  while (ok && r.read_field(type, id, last_id)) {
    switch (id) {
      case 1: { element.type_ = static_cast<int32_t>(r.read_zigzag()); break; }
      case 2: { element.type_length_ = static_cast<int32_t>(r.read_zigzag()); break; }
      case 3: { element.repetition_ = static_cast<int32_t>(r.read_zigzag()); break; }
      case 4: {
        int32_t n = 0;
        const char *p = NULL;
        if (r.read_binary(n, p) && n > 0) {
          element.name_.assign_ptr(const_cast<char *>(p), n);
        }
        break;
      }
      case 5: { element.num_children_ = static_cast<int32_t>(r.read_zigzag()); break; }
      case 6: { element.converted_type_ = static_cast<int32_t>(r.read_zigzag()); break; }
      default: { ok = r.skip(type); break; }
    }
    ok = ok && r.valid();
  }
  return ok && r.valid();
}

bool parse_column_meta(ParquetCompactReader &r, ParquetChunk &chunk)
{
  bool ok = true;
  int16_t last_id = 0;
  uint8_t type = 0;
  int16_t id = 0;
  while (ok && r.read_field(type, id, last_id)) {
    switch (id) {
      case 1: { chunk.physical_type_ = static_cast<int32_t>(r.read_zigzag()); break; }
      case 2:
      case 3: {
        uint8_t et = 0;
        int32_t n = 0;
        ok = r.valid() && read_list_header(r, et, n);
        for (int32_t i = 0; ok && i < n; ++i) {
          ok = r.skip(et);
        }
        break;
      }
      case 4: { chunk.codec_ = static_cast<int32_t>(r.read_zigzag()); break; }
      case 5: { chunk.num_values_ = r.read_zigzag(); break; }
      case 7: { chunk.total_compressed_size_ = r.read_zigzag(); break; }
      case 9: { chunk.data_page_offset_ = r.read_zigzag(); break; }
      case 11: { chunk.dictionary_page_offset_ = r.read_zigzag(); break; }
      default: { ok = r.skip(type); break; }
    }
    ok = ok && r.valid();
  }
  return ok && r.valid();
}

bool parse_column_chunk(ParquetCompactReader &r, ParquetChunk &chunk)
{
  bool ok = true;
  int16_t last_id = 0;
  uint8_t type = 0;
  int16_t id = 0;
  while (ok && r.read_field(type, id, last_id)) {
    switch (id) {
      case 1: {
        int32_t n = 0;
        const char *p = NULL;
        ok = r.read_binary(n, p);
        break;
      }
      case 3: { ok = parse_column_meta(r, chunk); break; }
      default: { ok = r.skip(type); break; }
    }
    ok = ok && r.valid();
  }
  return ok && r.valid();
}

bool parse_page_header(ParquetCompactReader &r, ParquetPageHeader &header)
{
  bool ok = true;
  int16_t last_id = 0;
  uint8_t type = 0;
  int16_t id = 0;
  while (ok && r.read_field(type, id, last_id)) {
    switch (id) {
      case 1: { header.type_ = static_cast<int32_t>(r.read_zigzag()); break; }
      case 2: { header.uncompressed_page_size_ = static_cast<int32_t>(r.read_zigzag()); break; }
      case 3: { header.compressed_page_size_ = static_cast<int32_t>(r.read_zigzag()); break; }
      case 5: { // DataPageHeader
        int16_t last2 = 0;
        uint8_t t2 = 0;
        int16_t id2 = 0;
        bool ok2 = true;
        while (ok2 && r.read_field(t2, id2, last2)) {
          switch (id2) {
            case 1: { header.num_values_ = static_cast<int32_t>(r.read_zigzag()); break; }
            case 2: { header.encoding_ = static_cast<int32_t>(r.read_zigzag()); break; }
            default: { ok2 = r.skip(t2); break; }
          }
          ok2 = ok2 && r.valid();
        }
        ok = ok2;
        break;
      }
      case 7: { // DictionaryPageHeader
        int16_t last2 = 0;
        uint8_t t2 = 0;
        int16_t id2 = 0;
        bool ok2 = true;
        while (ok2 && r.read_field(t2, id2, last2)) {
          switch (id2) {
            case 1: { header.num_values_ = static_cast<int32_t>(r.read_zigzag()); break; }
            case 2: { header.encoding_ = static_cast<int32_t>(r.read_zigzag()); break; }
            default: { ok2 = r.skip(t2); break; }
          }
          ok2 = ok2 && r.valid();
        }
        ok = ok2;
        break;
      }
      case 8: { // DataPageHeaderV2
        int16_t last2 = 0;
        uint8_t t2 = 0;
        int16_t id2 = 0;
        bool ok2 = true;
        while (ok2 && r.read_field(t2, id2, last2)) {
          switch (id2) {
            case 1: { header.num_values_ = static_cast<int32_t>(r.read_zigzag()); break; }
            case 2: { header.num_nulls_ = static_cast<int32_t>(r.read_zigzag()); break; }
            case 3: { header.num_rows_ = static_cast<int32_t>(r.read_zigzag()); break; }
            case 4: { header.encoding_ = static_cast<int32_t>(r.read_zigzag()); break; }
            case 5: {
              header.definition_levels_byte_length_ = static_cast<int32_t>(r.read_zigzag());
              break;
            }
            case 6: {
              header.repetition_levels_byte_length_ = static_cast<int32_t>(r.read_zigzag());
              break;
            }
            case 7: {
              // a thrift boolean is encoded by the field type, there is no payload
              header.is_compressed_ = (1 == t2);
              break;
            }
            default: { ok2 = r.skip(t2); break; }
          }
          ok2 = ok2 && r.valid();
        }
        ok = ok2;
        break;
      }
      default: { ok = r.skip(type); break; }
    }
    ok = ok && r.valid();
  }
  return ok && r.valid();
}

ObObjType parquet_obj_type(const ParquetSchemaElement &leaf)
{
  ObObjType type = ObVarcharType;
  switch (leaf.type_) {
    case PARQUET_TYPE_BOOLEAN: {
      type = ObTinyIntType;
      break;
    }
    case PARQUET_TYPE_INT32: {
      type = (PARQUET_CONVERTED_DATE == leaf.converted_type_) ? ObDateType : ObIntType;
      break;
    }
    case PARQUET_TYPE_INT64: {
      type = (PARQUET_CONVERTED_TIMESTAMP_MILLIS == leaf.converted_type_ ||
              PARQUET_CONVERTED_TIMESTAMP_MICROS == leaf.converted_type_) ?
             ObDateTimeType : ObIntType;
      break;
    }
    case PARQUET_TYPE_FLOAT:
    case PARQUET_TYPE_DOUBLE: {
      type = ObDoubleType;
      break;
    }
    default: {
      type = ObVarcharType;
      break;
    }
  }
  return type;
}

// decode count PLAIN encoded values and append them to out
bool decode_plain_values(ObIAllocator &alloc, const ObCollationType cs_type,
                         const ParquetSchemaElement &leaf,
                         const char *data, const int64_t len,
                         const int64_t count, ObIArray<ObObjParam> &out)
{
  bool ok = true;
  int64_t pos = 0;
  for (int64_t i = 0; ok && i < count; ++i) {
    ObObjParam obj;
    switch (leaf.type_) {
      case PARQUET_TYPE_BOOLEAN: {
        const int64_t byte_idx = i >> 3;
        if (byte_idx >= len) {
          ok = false;
        } else {
          obj.set_tinyint(((static_cast<uint8_t>(data[byte_idx]) >> (i & 7)) & 0x01) ? 1 : 0);
        }
        break;
      }
      case PARQUET_TYPE_INT32: {
        if (pos + 4 > len) {
          ok = false;
        } else {
          const int32_t v = read_i32(data + pos);
          pos += 4;
          if (PARQUET_CONVERTED_DATE == leaf.converted_type_) {
            obj.set_date(v);
          } else {
            obj.set_int(v);
          }
        }
        break;
      }
      case PARQUET_TYPE_INT64: {
        if (pos + 8 > len) {
          ok = false;
        } else {
          const int64_t v = read_i64(data + pos);
          pos += 8;
          if (PARQUET_CONVERTED_TIMESTAMP_MILLIS == leaf.converted_type_) {
            obj.set_datetime(v * 1000);
          } else if (PARQUET_CONVERTED_TIMESTAMP_MICROS == leaf.converted_type_) {
            obj.set_datetime(v);
          } else {
            obj.set_int(v);
          }
        }
        break;
      }
      case PARQUET_TYPE_FLOAT: {
        if (pos + 4 > len) {
          ok = false;
        } else {
          obj.set_double(static_cast<double>(read_f32(data + pos)));
          pos += 4;
        }
        break;
      }
      case PARQUET_TYPE_DOUBLE: {
        if (pos + 8 > len) {
          ok = false;
        } else {
          obj.set_double(read_f64(data + pos));
          pos += 8;
        }
        break;
      }
      case PARQUET_TYPE_BYTE_ARRAY: {
        if (pos + 4 > len) {
          ok = false;
        } else {
          const int32_t n = read_i32(data + pos);
          pos += 4;
          if (n < 0 || pos + n > len) {
            ok = false;
          } else {
            char *buf = NULL;
            if (n > 0 && OB_ISNULL(buf = static_cast<char *>(alloc.alloc(n)))) {
              ok = false;
            } else {
              if (n > 0) {
                MEMCPY(buf, data + pos, n);
              }
              obj.set_varchar(buf, n);
              obj.set_collation_type(cs_type);
              pos += n;
            }
          }
        }
        break;
      }
      case PARQUET_TYPE_FIXED_LEN_BYTE_ARRAY: {
        const int32_t n = leaf.type_length_;
        char *buf = NULL;
        if (n <= 0 || pos + n > len || OB_ISNULL(buf = static_cast<char *>(alloc.alloc(n)))) {
          ok = false;
        } else {
          MEMCPY(buf, data + pos, n);
          obj.set_varchar(buf, n);
          obj.set_collation_type(cs_type);
          pos += n;
        }
        break;
      }
      default: {
        ok = false;
        break;
      }
    }
    if (ok && OB_SUCCESS != out.push_back(obj)) {
      ok = false;
    }
  }
  return ok;
}

bool decode_chunk(ObIAllocator &alloc,
                  const ObCollationType cs_type,
                  const char *file_buf,
                  const int64_t file_len,
                  const ParquetSchemaElement &leaf,
                  const ParquetChunk &chunk,
                  const int64_t total_rows,
                  const int64_t col_cnt,
                  ObIArray<ObObjParam> &values)
{
  bool ok = true;
  int64_t start = chunk.data_page_offset_;
  if (chunk.dictionary_page_offset_ > 0 && chunk.dictionary_page_offset_ < start) {
    start = chunk.dictionary_page_offset_;
  }
  int64_t end = start + chunk.total_compressed_size_;
  if (start <= 0 || end > file_len - 8 || end <= start || chunk.col_idx_ < 0) {
    ok = false;
  }
  ObArray<ObObjParam> dict;
  ObArray<ObObjParam> page_values;
  int64_t pos = start;
  int64_t produced = 0;
  const bool required = (PARQUET_REP_REQUIRED == leaf.repetition_);
  char *decoded = NULL;
  ObArenaAllocator page_alloc("FileParquetPage");
  while (ok && pos < end && produced < chunk.num_values_) {
    ParquetCompactReader hr(file_buf + pos, end - pos);
    ParquetPageHeader header;
    ok = parse_page_header(hr, header);
    const int64_t header_len = hr.pos_;
    const char *body = file_buf + pos + header_len;
    if (!ok || header_len <= 0 || header.compressed_page_size_ < 0 ||
        pos + header_len + header.compressed_page_size_ > end ||
        header.uncompressed_page_size_ < 0) {
      ok = false;
      break;
    }
    if (PARQUET_PAGE_DICTIONARY == header.type_) {
      const char *data = body;
      int64_t data_len = header.compressed_page_size_;
      if (PARQUET_CODEC_UNCOMPRESSED != chunk.codec_) {
        if (header.uncompressed_page_size_ > FILE_MAX_SIZE) {
          ok = false;
          break;
        }
        decoded = static_cast<char *>(page_alloc.alloc(header.uncompressed_page_size_ + 1));
        if (NULL == decoded) {
          ok = false;
          break;
        }
        const int n = snappy_uncompress(body, header.compressed_page_size_,
                                        decoded, header.uncompressed_page_size_);
        if (n < 0) {
          ok = false;
          break;
        }
        data = decoded;
        data_len = n;
      }
      dict.reuse();
      ok = (PARQUET_ENC_PLAIN == header.encoding_ || PARQUET_ENC_PLAIN_DICTIONARY == header.encoding_) &&
           decode_plain_values(alloc, cs_type, leaf, data, data_len, header.num_values_, dict);
    } else if (PARQUET_PAGE_DATA == header.type_ || PARQUET_PAGE_DATA_V2 == header.type_) {
      const bool is_v2 = (PARQUET_PAGE_DATA_V2 == header.type_);
      const char *data = body;
      int64_t data_len = header.compressed_page_size_;
      if (is_v2) {
        if (header.repetition_levels_byte_length_ < 0 ||
            header.definition_levels_byte_length_ < 0 ||
            header.repetition_levels_byte_length_ + header.definition_levels_byte_length_ > data_len) {
          ok = false;
          break;
        }
        const int64_t level_len = header.repetition_levels_byte_length_ +
                                  header.definition_levels_byte_length_;
        data = body + level_len;
        data_len = header.compressed_page_size_ - level_len;
      }
      const int64_t raw_len = is_v2 ? (header.uncompressed_page_size_ -
                                       header.repetition_levels_byte_length_ -
                                       header.definition_levels_byte_length_)
                                    : header.uncompressed_page_size_;
      if (PARQUET_CODEC_UNCOMPRESSED != chunk.codec_ &&
          !(is_v2 && !header.is_compressed_)) {
        if (raw_len < 0 || raw_len > FILE_MAX_SIZE) {
          ok = false;
          break;
        }
        decoded = static_cast<char *>(page_alloc.alloc(raw_len + 1));
        if (NULL == decoded) {
          ok = false;
          break;
        }
        const int n = snappy_uncompress(data, data_len, decoded, raw_len);
        if (n < 0) {
          ok = false;
          break;
        }
        data = decoded;
        data_len = n;
      }
      uint32_t *def_levels = NULL;
      int64_t non_null_cnt = header.num_values_;
      if (!required) {
        const int64_t cnt = header.num_values_;
        if (cnt < 0) {
          ok = false;
          break;
        }
        if (cnt > 0 && NULL == (def_levels = static_cast<uint32_t *>(
                page_alloc.alloc(cnt * static_cast<int64_t>(sizeof(uint32_t)))))) {
          ok = false;
          break;
        }
        const char *level_data = NULL;
        int64_t level_len = 0;
        const char *value_data = data;
        int64_t value_len = data_len;
        if (is_v2) {
          level_data = body + header.repetition_levels_byte_length_;
          level_len = header.definition_levels_byte_length_;
        } else {
          if (data_len < 4) {
            ok = false;
            break;
          }
          level_len = read_i32(data);
          if (level_len < 0 || 4 + level_len > data_len) {
            ok = false;
            break;
          }
          level_data = data + 4;
          value_data = data + 4 + level_len;
          value_len = data_len - 4 - level_len;
        }
        if (cnt > 0 && !rle_bitpacked_decode(level_data, level_len, 1, cnt, def_levels)) {
          ok = false;
          break;
        }
        non_null_cnt = 0;
        for (int64_t i = 0; i < cnt; ++i) {
          if (1 == def_levels[i]) {
            ++non_null_cnt;
          }
        }
        data = value_data;
        data_len = value_len;
      }
      page_values.reuse();
      if (PARQUET_ENC_RLE_DICTIONARY == header.encoding_ ||
          PARQUET_ENC_PLAIN_DICTIONARY == header.encoding_) {
        if (data_len < 1) {
          ok = false;
          break;
        }
        const int32_t bit_width = static_cast<uint8_t>(data[0]);
        uint32_t *index_buf = NULL;
        if (non_null_cnt > 0 &&
            NULL == (index_buf = static_cast<uint32_t *>(
                page_alloc.alloc(non_null_cnt * static_cast<int64_t>(sizeof(uint32_t)))))) {
          ok = false;
          break;
        }
        if (non_null_cnt > 0 &&
            !rle_bitpacked_decode(data + 1, data_len - 1, bit_width, non_null_cnt, index_buf)) {
          ok = false;
          break;
        }
        for (int64_t i = 0; ok && i < non_null_cnt; ++i) {
          const uint32_t idx = index_buf[i];
          if (idx >= static_cast<uint32_t>(dict.count())) {
            ok = false;
            break;
          }
          if (OB_SUCCESS != page_values.push_back(dict.at(idx))) {
            ok = false;
            break;
          }
        }
      } else if (PARQUET_ENC_PLAIN == header.encoding_) {
        ok = decode_plain_values(alloc, cs_type, leaf, data, data_len, non_null_cnt, page_values);
      } else {
        ok = false;
      }
      if (ok) {
        int64_t value_idx = 0;
        for (int64_t i = 0; ok && i < header.num_values_; ++i) {
          const int64_t row = chunk.row_offset_ + produced;
          if (row >= total_rows) {
            break;
          }
          ObObjParam &slot = values.at(row * col_cnt + chunk.col_idx_);
          if (!required && def_levels != NULL && 1 != def_levels[i]) {
            slot.set_null();
          } else {
            if (value_idx >= page_values.count()) {
              ok = false;
              break;
            }
            slot = page_values.at(value_idx++);
          }
          ++produced;
        }
      }
    } else {
      // index pages are not needed
    }
    pos += header_len + header.compressed_page_size_;
  }
  return ok && produced == chunk.num_values_;
}

bool parse_parquet_footer(const char *buf,
                          const int64_t len,
                          ObIArray<ParquetSchemaElement> &schema,
                          ObIArray<ParquetChunk> &chunks,
                          int64_t &total_rows)
{
  bool ok = true;
  total_rows = 0;
  if (len < 12 || 0 != MEMCMP(buf, "PAR1", 4) || 0 != MEMCMP(buf + len - 4, "PAR1", 4)) {
    ok = false;
  } else {
    const int64_t meta_len = read_i32(buf + len - 8);
    if (meta_len <= 0 || meta_len > len - 12) {
      ok = false;
    } else {
      const int64_t meta_start = len - 8 - meta_len;
      ParquetCompactReader r(buf + meta_start, meta_len);
      int16_t last_id = 0;
      uint8_t type = 0;
      int16_t id = 0;
      int64_t row_offset = 0;
      while (ok && r.read_field(type, id, last_id)) {
        if (2 == id) {
          uint8_t et = 0;
          int32_t n = 0;
          ok = read_list_header(r, et, n);
          for (int32_t i = 0; ok && i < n; ++i) {
            ParquetSchemaElement element;
            ok = (12 == et) && parse_schema_element(r, element);
            if (ok) {
              ok = (OB_SUCCESS == schema.push_back(element));
            }
          }
        } else if (3 == id) {
          total_rows = r.read_zigzag();
        } else if (4 == id) {
          uint8_t et = 0;
          int32_t n = 0;
          ok = read_list_header(r, et, n);
          for (int32_t i = 0; ok && i < n; ++i) {
            int16_t last2 = 0;
            uint8_t t2 = 0;
            int16_t id2 = 0;
            bool ok2 = (12 == et);
            int64_t rg_rows = 0;
            ObArray<ParquetChunk> rg_chunks;
            while (ok2 && r.read_field(t2, id2, last2)) {
              if (1 == id2) {
                uint8_t et2 = 0;
                int32_t n2 = 0;
                ok2 = read_list_header(r, et2, n2);
                for (int32_t j = 0; ok2 && j < n2; ++j) {
                  ParquetChunk chunk;
                  chunk.col_idx_ = j;
                  ok2 = (12 == et2) && parse_column_chunk(r, chunk);
                  if (ok2) {
                    ok2 = (OB_SUCCESS == rg_chunks.push_back(chunk));
                  }
                }
              } else if (3 == id2) {
                rg_rows = r.read_zigzag();
              } else {
                ok2 = r.skip(t2);
              }
              ok2 = ok2 && r.valid();
            }
            for (int32_t j = 0; ok && j < rg_chunks.count(); ++j) {
              ParquetChunk chunk = rg_chunks.at(j);
              chunk.row_offset_ = row_offset;
              ok = (OB_SUCCESS == chunks.push_back(chunk));
            }
            row_offset += rg_rows;
            ok = ok && ok2 && r.valid();
          }
        } else {
          ok = r.skip(type);
        }
        ok = ok && r.valid();
      }
      if (ok) {
        total_rows = row_offset;
      }
    }
  }
  return ok;
}

} // anonymous namespace

int ObFileTableReader::store_str(const ObString &src, ObString &dst)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  dst.reset();
  if (src.empty()) {
    // nothing to copy
  } else if (OB_ISNULL(buf = static_cast<char *>(alloc_.alloc(src.length())))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to allocate memory for file string", K(ret), K(src.length()));
  } else {
    MEMCPY(buf, src.ptr(), src.length());
    dst.assign_ptr(buf, src.length());
  }
  return ret;
}

int ObFileTableReader::make_varchar(const ObString &src, ObObjParam &obj)
{
  int ret = OB_SUCCESS;
  ObString copied;
  if (OB_FAIL(store_str(src, copied))) {
  } else {
    obj.set_varchar(copied);
    obj.set_collation_type(cs_type_);
  }
  return ret;
}

int ObFileTableReader::read_whole_file(const ObString &path, char *&buf, int64_t &len)
{
  int ret = OB_SUCCESS;
  char *path_buf = NULL;
  FILE *fp = NULL;
  int64_t file_size = 0;
  buf = NULL;
  len = 0;
  if (path.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("empty file path", K(ret));
  } else if (OB_ISNULL(path_buf = static_cast<char *>(alloc_.alloc(path.length() + 1)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to allocate memory for file path", K(ret));
  } else {
    MEMCPY(path_buf, path.ptr(), path.length());
    path_buf[path.length()] = '\0';
    if (OB_ISNULL(fp = ::fopen(path_buf, "rb"))) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("failed to open file", K(ret), K(path), K(errno));
    } else if (0 != ::fseek(fp, 0, SEEK_END) || (file_size = ::ftell(fp)) < 0 ||
               0 != ::fseek(fp, 0, SEEK_SET)) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("failed to get file size", K(ret), K(path), K(errno));
    } else if (file_size > FILE_MAX_SIZE) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("file is too large to be read", K(ret), K(file_size));
    } else if (OB_ISNULL(buf = static_cast<char *>(alloc_.alloc(file_size + 1)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate memory for file content", K(ret), K(file_size));
    } else if (file_size > 0 &&
               ::fread(buf, 1, static_cast<size_t>(file_size), fp) !=
                   static_cast<size_t>(file_size)) {
      ret = OB_IO_ERROR;
      LOG_WARN("failed to read file", K(ret), K(path), K(file_size));
    } else {
      buf[file_size] = '\0';
      len = file_size;
    }
    if (NULL != fp) {
      ::fclose(fp);
      fp = NULL;
    }
  }
  return ret;
}

int ObFileTableReader::read(const ObString &path,
                            const ObFileReadOptions &options,
                            ObIArray<ObFileColumnMeta> &columns,
                            ObIArray<ObObjParam> &values,
                            int64_t &row_cnt)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  int64_t len = 0;
  row_cnt = 0;
  columns.reuse();
  values.reuse();
  if (FileType::FILE_GLOB == options.file_type_) {
    if (OB_FAIL(read_glob(path, columns, values, row_cnt))) {
    }
  } else if (FileType::FILE_CSV != options.file_type_ &&
             FileType::FILE_JSON != options.file_type_ &&
             FileType::FILE_PARQUET != options.file_type_) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(read_whole_file(path, buf, len))) {
  } else {
    switch (options.file_type_) {
      case FileType::FILE_CSV: {
        if (OB_FAIL(read_csv(buf, len, options.has_header_, columns, values, row_cnt))) {
        }
        break;
      }
      case FileType::FILE_JSON: {
        if (OB_FAIL(read_json(buf, len, columns, values, row_cnt))) {
        }
        break;
      }
      case FileType::FILE_PARQUET: {
        if (OB_FAIL(read_parquet(buf, len, columns, values, row_cnt))) {
        }
        break;
      }
      default: {
        ret = OB_INVALID_ARGUMENT;
        break;
      }
    }
  }
  return ret;
}

int ObFileTableReader::read_glob(const ObString &path,
                                 ObIArray<ObFileColumnMeta> &columns,
                                 ObIArray<ObObjParam> &values,
                                 int64_t &row_cnt)
{
  int ret = OB_SUCCESS;
  char *pattern = NULL;
  glob_t glob_result;
  MEMSET(&glob_result, 0, sizeof(glob_result));
  ObFileColumnMeta column;
  row_cnt = 0;
  if (path.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(pattern = static_cast<char *>(alloc_.alloc(path.length() + 1)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(pattern, path.ptr(), path.length());
    pattern[path.length()] = '\0';
    const int rc = ::glob(pattern, 0, NULL, &glob_result);
    if (GLOB_NOMATCH == rc) {
      // no file matches the pattern, an empty table is returned
    } else if (0 != rc) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("failed to expand glob pattern", K(ret), K(path), K(rc));
    } else if (OB_FAIL(store_str(ObString::make_string("file"), column.name_))) {
    } else {
      column.type_ = ObVarcharType;
      column.nullable_ = false;
      if (OB_FAIL(columns.push_back(column))) {
      }
      for (size_t i = 0; OB_SUCC(ret) && i < glob_result.gl_pathc; ++i) {
        ObObjParam obj;
        if (OB_FAIL(make_varchar(ObString::make_string(glob_result.gl_pathv[i]), obj))) {
        } else if (OB_FAIL(values.push_back(obj))) {
        } else {
          ++row_cnt;
        }
      }
    }
    ::globfree(&glob_result);
  }
  return ret;
}

int ObFileTableReader::parse_csv_record(const char *buf, const int64_t len, int64_t &pos,
                                        ObIArray<ObFileCell> &fields)
{
  int ret = OB_SUCCESS;
  bool done = (pos >= len);
  fields.reuse();
  while (!done && OB_SUCC(ret)) {
    ObFileCell cell;
    if ('"' == buf[pos]) {
      ++pos;
      const int64_t content_start = pos;
      int64_t content_len = 0;
      bool closed = false;
      while (pos < len) {
        if ('"' == buf[pos]) {
          if (pos + 1 < len && '"' == buf[pos + 1]) {
            ++content_len;
            pos += 2;
          } else {
            ++pos;
            closed = true;
            break;
          }
        } else {
          ++content_len;
          ++pos;
        }
      }
      char *dst = NULL;
      if (OB_ISNULL(dst = static_cast<char *>(alloc_.alloc(content_len + 1)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        int64_t w = 0;
        int64_t p = content_start;
        const int64_t content_end = closed ? pos - 1 : pos;
        while (p < content_end) {
          if ('"' == buf[p] && p + 1 < content_end && '"' == buf[p + 1]) {
            dst[w++] = '"';
            p += 2;
          } else {
            dst[w++] = buf[p++];
          }
        }
        dst[w] = '\0';
        cell.str_.assign_ptr(dst, static_cast<int32_t>(w));
      }
      while (pos < len && ',' != buf[pos] && '\n' != buf[pos]) {
        ++pos;
      }
    } else {
      const int64_t start = pos;
      while (pos < len && ',' != buf[pos] && '\n' != buf[pos]) {
        ++pos;
      }
      int64_t end = pos;
      if (end > start && '\r' == buf[end - 1]) {
        --end;
      }
      if (end == start) {
        cell.is_null_ = true;
      } else {
        cell.str_.assign_ptr(const_cast<char *>(buf + start), static_cast<int32_t>(end - start));
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(fields.push_back(cell))) {
      } else if (pos < len && ',' == buf[pos]) {
        ++pos;
      } else {
        if (pos < len) {
          ++pos;
        }
        done = true;
      }
    }
  }
  return ret;
}

int ObFileTableReader::read_csv(const char *buf,
                                const int64_t len,
                                const bool has_header,
                                ObIArray<ObFileColumnMeta> &columns,
                                ObIArray<ObObjParam> &values,
                                int64_t &row_cnt)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> names;
  ObArray<ObFileCell> cells;
  ObArray<ObFileCell> fields;
  int64_t pos = 0;
  int64_t col_cnt = 0;
  int64_t line_cnt = 0;
  bool first = true;
  while (OB_SUCC(ret) && pos < len) {
    if (OB_FAIL(parse_csv_record(buf, len, pos, fields))) {
    } else if (first) {
      first = false;
      col_cnt = fields.count();
      if (col_cnt <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("empty csv header", K(ret));
      } else if (!has_header) {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
          char name_buf[32];
          const int64_t n = ::snprintf(name_buf, sizeof(name_buf), "column%ld", i);
          ObString name;
          if (OB_FAIL(store_str(ObString(n, name_buf), name))) {
          } else if (OB_FAIL(names.push_back(name))) {
          }
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
          ObFileCell cell;
          if (i < fields.count()) {
            cell = fields.at(i);
          } else {
            cell.is_null_ = true;
          }
          if (OB_FAIL(cells.push_back(cell))) {
          }
        }
        ++line_cnt;
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
          if (OB_FAIL(names.push_back(fields.at(i).str_))) {
          }
        }
      }
    } else {
      ++line_cnt;
      for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
        ObFileCell cell;
        if (i < fields.count()) {
          cell = fields.at(i);
        } else {
          cell.is_null_ = true;
        }
        if (OB_FAIL(cells.push_back(cell))) {
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(build_columns(names, cells, col_cnt, line_cnt, columns, values))) {
    } else {
      row_cnt = line_cnt;
    }
  }
  return ret;
}

namespace
{

int read_json_string(ObIAllocator &alloc, const char *buf, const int64_t len,
                     int64_t &pos, ObString &str)
{
  int ret = OB_SUCCESS;
  if (pos >= len || '"' != buf[pos]) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ++pos;
    char *dst = NULL;
    // the decoded string is never longer than the source
    const int64_t max_len = len - pos;
    if (OB_ISNULL(dst = static_cast<char *>(alloc.alloc(max_len + 1)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      int64_t w = 0;
      bool closed = false;
      while (OB_SUCC(ret) && pos < len && !closed) {
        if ('"' == buf[pos]) {
          ++pos;
          closed = true;
        } else if ('\\' == buf[pos]) {
          ++pos;
          if (pos >= len) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            const char c = buf[pos++];
            if ('n' == c) {
              dst[w++] = '\n';
            } else if ('t' == c) {
              dst[w++] = '\t';
            } else if ('r' == c) {
              dst[w++] = '\r';
            } else if ('b' == c) {
              dst[w++] = '\b';
            } else if ('f' == c) {
              dst[w++] = '\f';
            } else if ('u' == c) {
              uint32_t code = 0;
              bool code_ok = (pos + 4 <= len);
              for (int32_t i = 0; code_ok && i < 4; ++i) {
                const char h = buf[pos + i];
                int32_t v = 0;
                if (h >= '0' && h <= '9') {
                  v = h - '0';
                } else if (h >= 'a' && h <= 'f') {
                  v = h - 'a' + 10;
                } else if (h >= 'A' && h <= 'F') {
                  v = h - 'A' + 10;
                } else {
                  code_ok = false;
                }
                code = code * 16 + static_cast<uint32_t>(v);
              }
              if (!code_ok) {
                ret = OB_INVALID_ARGUMENT;
              } else {
                pos += 4;
                if (code < 0x80) {
                  dst[w++] = static_cast<char>(code);
                } else if (code < 0x800) {
                  dst[w++] = static_cast<char>(0xC0 | (code >> 6));
                  dst[w++] = static_cast<char>(0x80 | (code & 0x3F));
                } else {
                  dst[w++] = static_cast<char>(0xE0 | (code >> 12));
                  dst[w++] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                  dst[w++] = static_cast<char>(0x80 | (code & 0x3F));
                }
              }
            } else {
              dst[w++] = c;
            }
          }
        } else {
          dst[w++] = buf[pos++];
        }
      }
      if (OB_SUCC(ret)) {
        dst[w] = '\0';
        str.assign_ptr(dst, static_cast<int32_t>(w));
      }
    }
  }
  return ret;
}

int read_json_scalar(const char *buf, const int64_t len, int64_t &pos, bool &is_null,
                     int64_t &token_start, int64_t &token_len)
{
  int ret = OB_SUCCESS;
  is_null = false;
  token_start = pos;
  token_len = 0;
  while (pos < len && is_space_char(buf[pos])) {
    ++pos;
  }
  if (pos >= len) {
    ret = OB_INVALID_ARGUMENT;
  } else if ('"' == buf[pos]) {
    int64_t p = pos + 1;
    bool closed = false;
    while (p < len && !closed) {
      if ('\\' == buf[p]) {
        p += 2;
      } else if ('"' == buf[p]) {
        ++p;
        closed = true;
      } else {
        ++p;
      }
    }
    if (!closed) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      token_start = pos;
      token_len = p - pos;
      pos = p;
      is_null = false;
    }
  } else if ('{' == buf[pos] || '[' == buf[pos]) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("nested json value is not supported by file table", K(ret));
  } else {
    while (pos < len && ',' != buf[pos] && '}' != buf[pos] && ']' != buf[pos] &&
           !is_space_char(buf[pos])) {
      ++pos;
    }
    token_len = pos - token_start;
    if (4 == token_len && 0 == ::strncasecmp(buf + token_start, "null", 4)) {
      is_null = true;
    }
  }
  return ret;
}

int read_json_value(ObIAllocator &alloc, const char *buf, const int64_t len,
                    int64_t &pos, ObFileCell &cell)
{
  int ret = OB_SUCCESS;
  int64_t token_start = 0;
  int64_t token_len = 0;
  bool is_null = false;
  while (pos < len && is_space_char(buf[pos])) {
    ++pos;
  }
  if (pos < len && '"' == buf[pos]) {
    ObString str;
    if (OB_FAIL(read_json_string(alloc, buf, len, pos, str))) {
    } else {
      cell.str_ = str;
    }
  } else if (OB_FAIL(read_json_scalar(buf, len, pos, is_null, token_start, token_len))) {
  } else if (is_null) {
    cell.is_null_ = true;
  } else if (token_len <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    char *dst = NULL;
    if (OB_ISNULL(dst = static_cast<char *>(alloc.alloc(token_len)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(dst, buf + token_start, token_len);
      cell.str_.assign_ptr(dst, static_cast<int32_t>(token_len));
    }
  }
  return ret;
}

} // anonymous namespace

int ObFileTableReader::read_json(const char *buf,
                                 const int64_t len,
                                 ObIArray<ObFileColumnMeta> &columns,
                                 ObIArray<ObObjParam> &values,
                                 int64_t &row_cnt)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> names;
  ObArray<ObFileCell> cells;
  ObArray<int64_t> line_starts;
  ObArray<ObString> line_keys;
  ObArray<ObFileCell> line_vals;
  int64_t pos = 0;
  int64_t line_cnt = 0;
  int64_t col_cnt = 0;
  // split the file into non empty lines
  while (OB_SUCC(ret) && pos < len) {
    const int64_t line_start = pos;
    while (pos < len && '\n' != buf[pos]) {
      ++pos;
    }
    if (pos < len) {
      ++pos;
    }
    if (OB_FAIL(line_starts.push_back(line_start))) {
    }
  }
  line_cnt = line_starts.count();
  // pass 1: collect the union of the object keys
  for (int64_t i = 0; OB_SUCC(ret) && i < line_cnt; ++i) {
    int64_t p = line_starts.at(i);
    int64_t end = len;
    for (int64_t j = p; j < len; ++j) {
      if ('\n' == buf[j]) {
        end = j;
        break;
      }
    }
    while (p < end && is_space_char(buf[p])) {
      ++p;
    }
    if (p >= end) {
      continue;
    }
    line_keys.reuse();
    line_vals.reuse();
    if ('{' != buf[p]) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid json line, an object is expected", K(ret));
    } else {
      ++p;
      while (OB_SUCC(ret) && p < end) {
        while (p < end && (is_space_char(buf[p]) || ',' == buf[p])) {
          ++p;
        }
        if (p >= end || '}' == buf[p]) {
          break;
        }
        ObString key;
        ObFileCell cell;
        if ('"' != buf[p]) {
          ret = OB_INVALID_ARGUMENT;
        } else if (OB_FAIL(read_json_string(alloc_, buf, len, p, key))) {
        } else {
          while (p < end && ':' != buf[p]) {
            ++p;
          }
          ++p;
          if (OB_FAIL(read_json_value(alloc_, buf, len, p, cell))) {
          } else {
            bool found = false;
            for (int64_t k = 0; !found && k < names.count(); ++k) {
              if (names.at(k) == key) {
                found = true;
              }
            }
            if (!found && OB_FAIL(names.push_back(key))) {
            }
          }
        }
      }
    }
  }
  col_cnt = names.count();
  // pass 2: fill the cell matrix
  if (OB_SUCC(ret) && col_cnt > 0 && line_cnt > 0) {
    ObFileCell null_cell;
    null_cell.is_null_ = true;
    if (OB_FAIL(cells.reserve(line_cnt * col_cnt))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < line_cnt * col_cnt; ++i) {
      if (OB_FAIL(cells.push_back(null_cell))) {
      }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < line_cnt; ++i) {
    int64_t p = line_starts.at(i);
    int64_t end = len;
    for (int64_t j = p; j < len; ++j) {
      if ('\n' == buf[j]) {
        end = j;
        break;
      }
    }
    while (p < end && is_space_char(buf[p])) {
      ++p;
    }
    if (p >= end) {
      continue;
    }
    if ('{' != buf[p]) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      ++p;
      while (OB_SUCC(ret) && p < end) {
        while (p < end && (is_space_char(buf[p]) || ',' == buf[p])) {
          ++p;
        }
        if (p >= end || '}' == buf[p]) {
          break;
        }
        ObString key;
        ObFileCell cell;
        if ('"' != buf[p]) {
          ret = OB_INVALID_ARGUMENT;
        } else if (OB_FAIL(read_json_string(alloc_, buf, len, p, key))) {
        } else {
          while (p < end && ':' != buf[p]) {
            ++p;
          }
          ++p;
          if (OB_FAIL(read_json_value(alloc_, buf, len, p, cell))) {
          } else {
            int64_t key_idx = -1;
            for (int64_t k = 0; k < names.count(); ++k) {
              if (names.at(k) == key) {
                key_idx = k;
                break;
              }
            }
            if (key_idx >= 0) {
              cells.at(i * col_cnt + key_idx) = cell;
            }
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(build_columns(names, cells, col_cnt, line_cnt, columns, values))) {
    } else {
      row_cnt = line_cnt;
    }
  }
  return ret;
}

int ObFileTableReader::build_columns(const ObIArray<ObString> &names,
                                     const ObIArray<ObFileCell> &cells,
                                     const int64_t col_cnt,
                                     const int64_t row_cnt,
                                     ObIArray<ObFileColumnMeta> &columns,
                                     ObIArray<ObObjParam> &values)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> final_names;
  for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
    const ObString base = i < names.count() ? names.at(i) : ObString();
    ObString final_name;
    int64_t same_cnt = 0;
    for (int64_t j = 0; j < i; ++j) {
      if (j < names.count() && names.at(j) == base) {
        ++same_cnt;
      }
    }
    if (0 == same_cnt) {
      if (OB_FAIL(store_str(base, final_name))) {
      }
    } else {
      char suffix[24];
      const int64_t n = ::snprintf(suffix, sizeof(suffix), "__%ld", same_cnt + 1);
      char *buf = NULL;
      if (OB_ISNULL(buf = static_cast<char *>(alloc_.alloc(base.length() + n)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (base.length() > 0) {
          MEMCPY(buf, base.ptr(), base.length());
        }
        MEMCPY(buf + base.length(), suffix, n);
        final_name.assign_ptr(buf, static_cast<int32_t>(base.length() + n));
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(final_names.push_back(final_name))) {
    }
  }
  if (OB_SUCC(ret) && row_cnt > 0 && col_cnt > 0) {
    ObObjParam null_obj;
    null_obj.set_null();
    if (OB_FAIL(values.reserve(row_cnt * col_cnt))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < row_cnt * col_cnt; ++i) {
      if (OB_FAIL(values.push_back(null_obj))) {
      }
    }
  }
  for (int64_t col = 0; OB_SUCC(ret) && col < col_cnt; ++col) {
    bool all_bool = true;
    bool all_int = true;
    bool all_double = true;
    bool all_date = true;
    bool all_datetime = true;
    bool has_value = false;
    bool has_null = false;
    for (int64_t row = 0; row < row_cnt; ++row) {
      const ObFileCell &cell = cells.at(row * col_cnt + col);
      if (cell.is_null_) {
        has_null = true;
      } else {
        bool b = false;
        int64_t i64 = 0;
        double d = 0;
        has_value = true;
        if (all_bool && !is_bool_str(cell.str_, b)) {
          all_bool = false;
        }
        if (all_int && !is_int_str(cell.str_, i64)) {
          all_int = false;
        }
        if (all_double && !is_double_str(cell.str_, d)) {
          all_double = false;
        }
        if (all_date && !is_date_str(cell.str_)) {
          all_date = false;
        }
        if (all_datetime && !is_datetime_str(cell.str_)) {
          all_datetime = false;
        }
      }
    }
    ObObjType type = ObVarcharType;
    if (!has_value) {
      type = ObVarcharType;
    } else if (all_bool) {
      type = ObTinyIntType;
    } else if (all_int) {
      type = ObIntType;
    } else if (all_double) {
      type = ObDoubleType;
    } else if (all_date) {
      type = ObDateType;
    } else if (all_datetime) {
      type = ObDateTimeType;
    }
    ObFileColumnMeta meta;
    meta.name_ = final_names.at(col);
    meta.type_ = type;
    meta.nullable_ = has_null;
    if (OB_FAIL(columns.push_back(meta))) {
    }
    for (int64_t row = 0; OB_SUCC(ret) && row < row_cnt; ++row) {
      const ObFileCell &cell = cells.at(row * col_cnt + col);
      ObObjParam &obj = values.at(row * col_cnt + col);
      if (cell.is_null_) {
        obj.set_null();
      } else if (ObTinyIntType == type) {
        bool b = false;
        (void)is_bool_str(cell.str_, b);
        obj.set_tinyint(b ? 1 : 0);
      } else if (ObIntType == type) {
        int64_t i64 = 0;
        (void)is_int_str(cell.str_, i64);
        obj.set_int(i64);
      } else if (ObDoubleType == type) {
        double d = 0;
        (void)is_double_str(cell.str_, d);
        obj.set_double(d);
      } else if (ObDateType == type) {
        int32_t date = 0;
        if (OB_FAIL(ObTimeConverter::str_to_date(cell.str_, date))) {
          LOG_WARN("failed to convert string to date", K(ret), K(cell.str_));
        } else {
          obj.set_date(date);
        }
      } else if (ObDateTimeType == type) {
        int64_t datetime = 0;
        ObTimeConvertCtx cvrt_ctx(tz_info_, false);
        if (OB_FAIL(ObTimeConverter::str_to_datetime(cell.str_, cvrt_ctx, datetime))) {
          LOG_WARN("failed to convert string to datetime", K(ret), K(cell.str_));
        } else {
          obj.set_datetime(datetime);
        }
      } else {
        obj.set_varchar(cell.str_);
        obj.set_collation_type(cs_type_);
      }
    }
  }
  return ret;
}

int ObFileTableReader::read_parquet(const char *buf,
                                    const int64_t len,
                                    ObIArray<ObFileColumnMeta> &columns,
                                    ObIArray<ObObjParam> &values,
                                    int64_t &row_cnt)
{
  int ret = OB_SUCCESS;
  ObArray<ParquetSchemaElement> schema;
  ObArray<ParquetChunk> chunks;
  ObArray<int64_t> leaf_indexes;
  int64_t total_rows = 0;
  row_cnt = 0;
  if (!parse_parquet_footer(buf, len, schema, chunks, total_rows)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("failed to parse parquet file", K(ret), K(len));
  }
  // the first schema element is the root, the leaves are the real columns
  for (int64_t i = 0; OB_SUCC(ret) && i < schema.count(); ++i) {
    const ParquetSchemaElement &element = schema.at(i);
    if (0 == element.num_children_ && i > 0) {
      ObFileColumnMeta meta;
      meta.name_ = element.name_;
      meta.type_ = parquet_obj_type(element);
      meta.nullable_ = (PARQUET_REP_OPTIONAL == element.repetition_);
      if (OB_FAIL(columns.push_back(meta))) {
      } else if (OB_FAIL(leaf_indexes.push_back(i))) {
      }
    }
  }
  const int64_t col_cnt = columns.count();
  if (OB_SUCC(ret) && col_cnt <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("no column found in parquet file", K(ret));
  }
  if (OB_SUCC(ret) && total_rows > 0 && col_cnt > 0) {
    ObObjParam null_obj;
    null_obj.set_null();
    if (OB_FAIL(values.reserve(total_rows * col_cnt))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < total_rows * col_cnt; ++i) {
      if (OB_FAIL(values.push_back(null_obj))) {
      }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < chunks.count(); ++i) {
    const ParquetChunk &chunk = chunks.at(i);
    if (chunk.col_idx_ < 0 || chunk.col_idx_ >= col_cnt) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid parquet chunk", K(ret), K(chunk.col_idx_), K(col_cnt));
    } else if (OB_FAIL(ret)) {
    } else {
      const ParquetSchemaElement &leaf = schema.at(leaf_indexes.at(chunk.col_idx_));
      if (!decode_chunk(alloc_, cs_type_, buf, len, leaf, chunk, total_rows, col_cnt, values)) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("failed to decode parquet chunk", K(ret), K(chunk.col_idx_));
      }
    }
  }
  if (OB_SUCC(ret)) {
    row_cnt = total_rows;
  }
  return ret;
}

int ob_file_sql_type_name(const ObObjType type, ObString &name)
{
  const char *str = NULL;
  switch (type) {
    case ObTinyIntType: str = "BOOLEAN"; break;
    case ObIntType: str = "BIGINT"; break;
    case ObDoubleType: str = "DOUBLE"; break;
    case ObDateType: str = "DATE"; break;
    case ObDateTimeType: str = "DATETIME"; break;
    case ObVarcharType: str = "VARCHAR"; break;
    default: str = "VARCHAR"; break;
  }
  name = ObString::make_string(str);
  return OB_SUCCESS;
}

} // namespace sql
} // namespace oceanbase
