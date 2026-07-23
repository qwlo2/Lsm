#include "block/blockmeta.h"
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace tiny_lsm {
BlockMeta::BlockMeta() : offset(0), first_key(""), last_key("") {}

BlockMeta::BlockMeta(size_t offset, const std::string &first_key,
                     const std::string &last_key)
    : offset(offset), first_key(first_key), last_key(last_key) {}
//metadata以及包含了block——data
void BlockMeta::encode_meta_to_slice(std::vector<BlockMeta> &meta_entries,
                                     std::vector<uint8_t> &metadata) {
        //metadata的前面已经有了block_data
         std::vector<uint8_t> meta_section_data;
          auto encode=[&meta_section_data](BlockMeta& block){
                  for (int i=0;i<4; ++i) {
                      meta_section_data.emplace_back(static_cast<uint8_t>((block.offset>>(i*8))&0xff));
                  }
                  uint16_t f_key_len=static_cast<uint16_t>(block.first_key.size());
                  uint16_t l_key_len=static_cast<uint16_t>(block.last_key.size());

                  meta_section_data.emplace_back(static_cast<uint8_t>(f_key_len&0xff));
                 meta_section_data.emplace_back(static_cast<uint8_t>((f_key_len>>8)&0xff));
                 meta_section_data.insert(meta_section_data.end(),block.first_key.begin(),block.first_key.end());

                 meta_section_data.emplace_back(static_cast<uint8_t>(l_key_len&0xff));
                  meta_section_data.emplace_back(static_cast<uint8_t>((l_key_len>>8)&0xff));
                  meta_section_data.insert(meta_section_data.end(),block.last_key.begin(),block.last_key.end());
          };
          //meta的size
          uint32_t meta_size=static_cast<uint32_t>(meta_entries.size());
          for (int i=0;i<4; ++i) {
            meta_section_data.emplace_back(static_cast<uint8_t>((meta_size>>(i*8))&0xff));
        }
       //每个meta
       for (auto& it : meta_entries) {
             encode(it);
       }
       //hash(crc)的计算
       //实验没有校验 meta_size
       auto crc32=[&meta_section_data](){
                uint32_t crc=0xffffffff;
                for (auto& it:meta_section_data) {
                      crc^=it;
                      for (int i=0;i<8;++i) {
                            if (crc&1) {
                                crc=(crc>>1)^ 0xEDB88320;
                            }
                            else {
                              crc>>=1;
                            }
                      }
                }
                return  crc^0xffffffff;
       };
       auto crc_hash=crc32();
       metadata.insert(metadata.end(),meta_section_data.begin(),meta_section_data.end());
       for (int i=0;i<4; ++i) {
            metadata.emplace_back(static_cast<uint8_t>((crc_hash>>(i*8))&0xff));
        }
}

std::vector<BlockMeta>
BlockMeta::decode_meta_from_slice(const std::vector<uint8_t> &metadata) {
    //检验hash（src）
    //std::hash<std::string_view>{}(
    //  std::string_view(reinterpret_cast<const char *>(data_start), data_len))

    //std::hash<>，创建一个计算哈希的对象，std::string_view字符串视图
      auto crc32=[&metadata](){
                uint32_t crc=0xffffffff;
                 int meta_size=metadata.size();
                for(int i=0;i<meta_size-4;++i) {
                      crc^=metadata[i];
                      for (int i=0;i<8;++i) {
                            if (crc&1) {
                                crc=(crc>>1)^ 0xEDB88320;
                            }
                            else {
                              crc>>=1;
                            }
                      }
                }
                return  crc^0xffffffff;
       };
      uint32_t crc;
      memcpy(&crc,metadata.data()+metadata.size()-4,4);
      int save=crc32();
      if (crc!=save) {
           throw  std::runtime_error("matadata crc校验错误");
      }
      //解码到内存
      //entry 大小
      uint32_t entry_size;
      memcpy(&entry_size,metadata.data(),4);
     //std::vector<BlockMeta> mata_entry(entry_size),这里默认构造了3个，emplace_back会追加
     std::vector<BlockMeta> mata_entry;
     //entry data
           int size=metadata.size(),block_len;
           for (int i=4;i<size-4;i+=block_len) {
                   block_len=0;
             //offset
                  uint32_t off;
                  memcpy(&off,metadata.data()+i+block_len,sizeof(uint32_t));
                  size_t offset=static_cast<size_t>(off);
                  block_len+=sizeof(uint32_t);
            //f_key
                 uint16_t f_key_len;
                 memcpy(&f_key_len,metadata.data()+i+block_len,sizeof(uint16_t));
                 block_len+=sizeof(uint16_t);

                 std::string f_key(
                   reinterpret_cast<const char*>(metadata.data()+i+block_len),
                    f_key_len
                 );
                 //memcpy会直接在地址操作，要注意大小和内存的拥有者
                  //memcpy(&f_key,metadata.data()+i+block_len,f_key_len);
                  block_len+=f_key_len;
            //l_key
              uint16_t l_key_len;
                 memcpy(&l_key_len,metadata.data()+i+block_len,sizeof(uint16_t));
                 block_len+=sizeof(uint16_t);

                 std::string l_key;
                 l_key.resize(l_key_len);
                  memcpy(l_key.data(),metadata.data()+i+block_len,l_key_len);
                  block_len+=l_key_len;
              //entry
              mata_entry.emplace_back(offset,f_key,l_key);
           }
     return  mata_entry;
}
} // namespace tiny_lsm
