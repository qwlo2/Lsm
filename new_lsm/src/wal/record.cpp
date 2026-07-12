// src/wal/record.cpp

#include "wal/record.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tiny_lsm {

Record Record::createRecord(uint64_t tranc_id) {
  Record record;
  record.operation_type_ = OperationType::OP_CREATE;
  record.tranc_id_ = tranc_id;
  record.record_len_ = sizeof(uint16_t) + sizeof(uint64_t) + sizeof(uint8_t);
  return record;
}
Record Record::commitRecord(uint64_t tranc_id) {
  Record record;
  record.operation_type_ = OperationType::OP_COMMIT;
  record.tranc_id_ = tranc_id;
  record.record_len_ = sizeof(uint16_t) + sizeof(uint64_t) + sizeof(uint8_t);
  return record;
}
Record Record::rollbackRecord(uint64_t tranc_id) {
  Record record;
  record.operation_type_ = OperationType::OP_ROLLBACK;
  record.tranc_id_ = tranc_id;
  record.record_len_ = sizeof(uint16_t) + sizeof(uint64_t) + sizeof(uint8_t);
  return record;
}
Record Record::putRecord(uint64_t tranc_id, const std::string &key,
                         const std::string &value) {
  Record record;
  record.operation_type_ = OperationType::OP_PUT;
  record.tranc_id_ = tranc_id;
  record.key_ = key;
  record.value_ = value;
  record.record_len_ = sizeof(uint16_t) + sizeof(uint64_t) + sizeof(uint8_t) +
                       sizeof(uint16_t) + key.size() + sizeof(uint16_t) +
                       value.size();
  return record;
}
Record Record::deleteRecord(uint64_t tranc_id, const std::string &key) {
  Record record;
  record.operation_type_ = OperationType::OP_DELETE;
  record.tranc_id_ = tranc_id;
  record.key_ = key;
  record.record_len_ = sizeof(uint16_t) + sizeof(uint64_t) + sizeof(uint8_t) +
                       sizeof(uint16_t) + key.size();
  return record;
}
//在所有的encode后再加crc，这个要做上层实现,先不做
std::vector<uint8_t> Record::encode() const {
     std::vector<uint8_t> record_uin8;
     //record_len
     record_uin8.emplace_back(static_cast<uint8_t>(record_len_));
     record_uin8.emplace_back(static_cast<uint8_t>(record_len_>>8));
     //txn 
     auto n=sizeof(uint64_t);
     for (int i=0;i<n;++i) {
       record_uin8.emplace_back(static_cast<uint8_t>(tranc_id_>>(i*8)));
     }
     //操作类型
      record_uin8.emplace_back(static_cast<uint8_t>(operation_type_));
      //key
    if (operation_type_==OperationType::OP_DELETE) {
        record_uin8.emplace_back(static_cast<uint8_t>(key_.size()));
        record_uin8.emplace_back(static_cast<uint8_t>(key_.size()>>8));
          record_uin8.insert(record_uin8.end(),key_.begin(),key_.end());
          return  record_uin8;
    }
    //key+value
    if (operation_type_==OperationType::OP_PUT) {
          record_uin8.emplace_back(static_cast<uint8_t>(key_.size()));
        record_uin8.emplace_back(static_cast<uint8_t>(key_.size()>>8));
          record_uin8.insert(record_uin8.end(),key_.begin(),key_.end());
         record_uin8.emplace_back(static_cast<uint8_t>(value_.size()));
        record_uin8.emplace_back(static_cast<uint8_t>(value_.size()>>8));
          record_uin8.insert(record_uin8.end(),value_.begin(),value_.end());
    }
    return  record_uin8;
}
//可以之间返回，txn对应的record，上层就不用再处理了
std::vector<Record> Record::decode(const std::vector<uint8_t> &data) {
     uint32_t save_crc;
      auto n=data.size();
   //  memcpy(&save_crc,data.data()+n,sizeof(uint32_t));
     std::vector<Record> record_vec;
     //在wal删除多余文件里没有检查crc，暂时不设计
    //  auto crc32 = [](const std::vector<uint8_t> &data, unsigned int size) {
    //    uint32_t crc = 0xffffffff;
    //    for (int i = 0; i < size; i++) {
    //      crc ^= data[i];
    //      for (int j = 0; j < 8; j++) {
    //        if (crc & 1) {
    //          crc = (crc >> 1) ^ 0xEDB88320;
    //        } else {
    //          crc >>= 1;
    //        }
    //      }
    //    }
    //    return crc ^ 0xffffffff;
    //  };
    //  auto crc=crc32(data,n-sizeof(uint32_t));
    //  if (crc!=save_crc) {
    //   throw  std::runtime_error("encode Redord crc error");
    //  }
     auto record_en=[&](size_t& i){
                //总长度
                
               uint16_t re_len;
               //if (i + sizeof(uint16_t) > n) return ;
               memcpy(&re_len, data.data()+i,sizeof(uint16_t));
               //if (i + re_len > n) return;
               if (re_len<sizeof(uint16_t) + sizeof(uint64_t) + sizeof(uint8_t)) {
                   throw std::runtime_error("encode Redord error re_len<sizeof(uint16_t) + sizeof(uint64_t) + sizeof(uint8_t)");
               }

               Record record;
               record.record_len_=re_len;
               i+=sizeof(uint16_t);
               //txn
               uint64_t txn_id;
              memcpy(&record.tranc_id_, data.data()+i,sizeof(uint64_t));
              i+=sizeof(uint64_t);
              //OperationType
              uint8_t op;
               memcpy(&op, data.data()+i,sizeof(uint8_t));
               record.operation_type_=static_cast<OperationType>(op);
              i+=sizeof(uint8_t);
              //dele
              if (record.getOperationType()==OperationType::OP_DELETE) {
                uint16_t key_len;
                memcpy(&key_len, data.data()+i,sizeof(uint16_t));
                i+=sizeof(uint16_t);
                record.key_.resize(key_len);
                memcpy(record.key_.data(), data.data()+i,key_len);
                i+=key_len;
                return  record;
              }
              // put
              if (record.getOperationType()== OperationType::OP_PUT) {
                uint16_t key_len;
                memcpy(&key_len, data.data()+i,sizeof(uint16_t));
                i+=sizeof(uint16_t);
                record.key_.resize(key_len);
                memcpy(record.key_.data(), data.data()+i,key_len);
                i+=key_len;
                 uint16_t va_len;
                memcpy(&va_len, data.data()+i,sizeof(uint16_t));
                i+=sizeof(uint16_t);
                record.value_.resize(va_len);
                memcpy(record.value_.data(), data.data()+i,va_len);
                i+=va_len;
              }
              return record;
     };
    // n-=sizeof(uint32_t);
     for (size_t i=0;i<n;) {
          record_vec.emplace_back(record_en(i));
     }
     return  record_vec;
}

bool Record::operator==(const Record &other) const {
  if (tranc_id_ != other.tranc_id_ ||
      operation_type_ != other.operation_type_) {
    return false;
  }

  // 不需要 key 和 value 比较的情况
  if (operation_type_ == OperationType::OP_CREATE ||
      operation_type_ == OperationType::OP_COMMIT ||
      operation_type_ == OperationType::OP_ROLLBACK) {
    return true;
  }

  // 需要 key 比较的情况
  if (operation_type_ == OperationType::OP_DELETE) {
    return key_ == other.key_;
  }

  // 需要 key 和 value 比较的情况
  return key_ == other.key_ && value_ == other.value_;
}

bool Record::operator!=(const Record &other) const { return !(*this == other); }
} // namespace tiny_lsm