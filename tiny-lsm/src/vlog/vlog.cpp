#include "vlog/vlog.h"
#include "spdlog/spdlog.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace tiny_lsm {

// Simple CRC32 implementation (polynomial 0xEDB88320)
static uint32_t crc32_compute(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFF;
}

std::shared_ptr<VLog> VLog::open(const std::string &path) {
    // TODO: Lab 6.1 打开或创建 VLog 文件
    // ? 1. 若文件不存在则创建空文件 (std::ofstream)
    // ? 2. 用 FileObj::open(path, false) 打开 (不截断, 保留已有记录)
    // ? 3. 记录 path_ 和 file_
    //open的是文件，目录要用filesysteam的exists判断
    //在底层有参数，当true是会创建并截断，即清空文件
     auto file = FileObj::open(path, !std::filesystem::exists(path) );
    auto vlog = std::make_shared<VLog>();
    vlog->file_ = std::move(file);
    vlog->path_ = path;
    return vlog;
}

uint64_t VLog::append(const std::string &key, const std::string &value) {
    // TODO: Lab 6.1 追加一条 KV 记录到 VLog, 返回记录起始偏移量
    // ? 记录格式: [key_len:uint16][key][val_len:uint32][value][crc32:uint32]
    // ? CRC32 覆盖除自身之外的所有字段
    // ? 注意: 需要加 append_mtx_ 互斥锁
    // ? offset = file_.size() (追加前的文件大小即为本次记录的起始位置)
    std::lock_guard<std::mutex> lock(append_mtx_);
    //返回追加前的文件大小作为新记录的起始偏移
    auto offset = file_.size();

    std::vector<uint8_t> buf;
    //key
    auto key_len = static_cast<uint16_t>(key.size());
    buf.emplace_back(key_len & 0xFF);
    buf.emplace_back((key_len >> 8) & 0xFF);
    buf.insert(buf.end(), key.begin(), key.end());
    //value
    auto val_len = static_cast<uint32_t>(value.size());
    buf.emplace_back(val_len & 0xFF);
    buf.emplace_back((val_len >> 8) & 0xFF);
    buf.emplace_back((val_len >> 16) & 0xFF);
    buf.emplace_back((val_len >> 24) & 0xFF);
    buf.insert(buf.end(), value.begin(), value.end());
    //crc
    auto crc=crc32_compute(buf.data(), buf.size());
    file_.append(buf);
    file_.append_uint32(crc);  
    return offset;
}
std::vector<uint64_t>
VLog::append_batch(const std::vector<std::pair<std::string, std::string>> &kvs) {
  std::lock_guard<std::mutex> lock(append_mtx_);

  std::vector<uint64_t> offsets;
  offsets.reserve(kvs.size());

  uint64_t base = file_.size();
  std::vector<uint8_t> buf;

  for (auto &[key, value] : kvs) {
    offsets.push_back(base + buf.size());

    size_t record_begin = buf.size();

    uint16_t key_len = static_cast<uint16_t>(key.size());
    buf.emplace_back(key_len & 0xff);
    buf.emplace_back((key_len >> 8) & 0xff);
    buf.insert(buf.end(), key.begin(), key.end());

    uint32_t val_len = static_cast<uint32_t>(value.size());
    buf.emplace_back(val_len & 0xff);
    buf.emplace_back((val_len >> 8) & 0xff);
    buf.emplace_back((val_len >> 16) & 0xff);
    buf.emplace_back((val_len >> 24) & 0xff);
    buf.insert(buf.end(), value.begin(), value.end());

    uint32_t crc =
        crc32_compute(buf.data() + record_begin, buf.size() - record_begin);
    buf.emplace_back(crc & 0xff);
    buf.emplace_back((crc >> 8) & 0xff);
    buf.emplace_back((crc >> 16) & 0xff);
    buf.emplace_back((crc >> 24) & 0xff);
  }

  if (!buf.empty() && !file_.append(buf)) {
    throw std::runtime_error("append vlog batch failed");
  }

  return offsets;
}
std::string VLog::read_value(uint64_t offset, uint32_t value_size) {
    // TODO: Lab 6.1 从指定 offset 读取 value
    // ? 记录布局: [key_len:2][key:key_len][val_len:4][value:val_len][crc:4]
    // ? 先读 key_len 以跳过 key, 再定位 value 起始位置
    // ? value 起始 = offset + 2 + key_len + 4
    //key
    auto key_len_buf = file_.read_to_slice(offset,sizeof(uint16_t));
    uint16_t key_len ;
    //memcpy从dest开始复制，在主机中地址从小到大，因此对于小段直接拷贝到key_len的地址就行了
    memcpy(&key_len, key_len_buf.data(), sizeof(uint16_t));
    //value_size已知
    auto value_offset = offset + sizeof(uint16_t) + key_len + sizeof(uint32_t);
    auto value_buf = file_.read_to_slice(value_offset, value_size);
    //crc校验暂时不写
    return std::string(value_buf.begin(), value_buf.end());
    
}

uint64_t VLog::tail_offset() const {
    return static_cast<uint64_t>(file_.size());
}

void VLog::sync() { file_.sync(); }

void VLog::del_vlog() { file_.del_file(); }

} // namespace tiny_lsm
