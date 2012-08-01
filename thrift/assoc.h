/**
* Thrift server that supports operations on the 
* Facebook TAO Graph database
* @author Dhruba Borthakur (dhruba@gmail.com)
* Copyright 2012 Facebook
*/
#ifndef THRIFT_LEVELDB_ASSOC_SERVER_H_
#define THRIFT_LEVELDB_ASSOC_SERVER_H_

#include <AssocService.h>
#include <leveldb_types.h>
#include "openhandles.h"
#include "server_options.h"

#include "leveldb/db.h"
#include "leveldb/write_batch.h"
#include "util/testharness.h"

using namespace apache::thrift;
using namespace apache::thrift::protocol;
using namespace apache::thrift::transport;
using namespace apache::thrift::server;

using boost::shared_ptr;

using namespace  ::Tleveldb;

//
// These are the service methods that processes Association Data.

class AssocServiceHandler : virtual public AssocServiceIf {
 public:

  AssocServiceHandler(OpenHandles* openhandles) {
    openhandles_ = openhandles;
  }
  
  int64_t taoAssocPut(const Text& tableName, int64_t assocType, int64_t id1, 
                      int64_t id2, int64_t id1Type, int64_t id2Type, 
                      int64_t timestamp, AssocVisibility visibility, 
                      bool update_count, int64_t dataVersion, const Text& data, 
                      const Text& wormhole_comment) {
    leveldb::DB* db = openhandles_->get(tableName, NULL);
    if (db == NULL) {
      return Code::kNotFound;
    }
    int64_t ret = assocPutInternal(tableName,
                           db, assocType, id1, id2, id1Type, id2Type,
                           timestamp, visibility, update_count, dataVersion,
                           data, wormhole_comment);
    return ret;
  }

  int64_t taoAssocDelete(const Text& tableName, int64_t assocType, int64_t id1, 
                         int64_t id2, AssocVisibility visibility, bool update_count, 
                         const Text& wormhole_comment) {
    printf("taoAssocDelete\n");
  }

  void taoAssocRangeGet(std::vector<TaoAssocGetResult> & _return, 
                        const Text& tableName, int64_t assocType, int64_t id1, 
                        int64_t start_time, int64_t end_time, int64_t offset, 
                        int64_t limit) {
    printf("taoAssocRangeGet\n");
  }

  void taoAssocGet(std::vector<TaoAssocGetResult> & _return, 
                   const Text& tableName, int64_t assocType, int64_t id1, 
                   const std::vector<int64_t> & id2s) {
    leveldb::DB* db = openhandles_->get(tableName, NULL);
    if (db == NULL) {
      throw generate_exception(tableName, Code::kNotFound,
               "Unable to database " ,
               assocType, id1, 0, 0, 0, 0, Tleveldb::UNUSED1);
    }
    taoAssocGetInternal(_return, tableName, db, assocType, id1, id2s);
  }

  int64_t taoAssocCount(const Text& tableName, int64_t assocType, int64_t id1) {
    leveldb::DB* db = openhandles_->get(tableName, NULL);
    if (db == NULL) {
      return Code::kNotFound;
    }
    return taoAssocCountInternal(tableName, db, assocType, id1);
  }

 private:
  OpenHandles* openhandles_;

  //
  // inserts an assoc
  // Returns true if the iinsertion is successful, otherwise return false.
  // 
  bool assocPutInternal(const Text& tableName, leveldb::DB* db,
                      int64_t assocType, int64_t id1, 
                      int64_t id2, int64_t id1Type, int64_t id2Type, 
                      int64_t ts, AssocVisibility vis, 
                      bool update_count, int64_t dataVersion, const Text& data, 
                      const Text& wormhole_comment) {
    leveldb::WriteOptions woptions;
    woptions.sync = true;

    // create the payload for this assoc
    int payloadsize = sizeof(id1Type) + sizeof(id2Type) +
               sizeof(dataVersion) + data.size() + wormhole_comment.size();
    std::string payload;
    payload.reserve(payloadsize);
    payload.resize(payloadsize);
    makePayload(&payload[0], id1Type, id2Type, dataVersion, data, 
                wormhole_comment);
    leveldb::Slice payload_slice(payload);

    // create RowKey with plenty of space at the end to query
    // all columns 'c', 'm', 'p, etc.
    int maxkeysize = sizeof(id1) + sizeof(assocType) +
                       1 +  // 'c', 'p' or 'm'
                       sizeof(ts) +
                       sizeof(id2);
    std::string dummy;
    dummy.reserve(maxkeysize);
    dummy.resize(maxkeysize);
    char* keybuf = &dummy[0];
    int rowkeysize = makeRowKey(keybuf, id1, assocType);
    leveldb::ReadOptions roptions;
    leveldb::Status status;
    std::string value;
    int keysize;

    int64_t count = 0;
    int64_t oldts;
    int8_t  oldvis;
    bool newassoc = false; // is this assoc new or an overwrite

    // make a key for count
    keysize = appendRowKeyForCount(rowkeysize, keybuf);
    leveldb::Slice ckey(keybuf, keysize);

    // Scan 'c'  to get $count if $update_count == true
    if (update_count) {
      status = db->Get(roptions, ckey, &value);
      if (status.IsNotFound()) {
        // nothing to do
      } else if (!status.ok() || (value.size() != sizeof(int64_t))) {
        throw generate_exception(tableName, Code::kNotFound,
                "Unable to find count ", 
                assocType, id1, id2, id1Type, id2Type, ts, vis);
            
      } else {
        extract_int64(&count, (char *)value.c_str());
      }
    }

    // Scan 'm'$id2 to get $ts and $vis
    keysize = appendRowKeyForMeta(rowkeysize, keybuf, id2);
    leveldb::Slice mkey(keybuf, keysize);
    status = db->Get(roptions, mkey, &value);
    if (status.IsNotFound()) {
      newassoc = true;
    } else if (!status.ok() || 
               (value.size() != sizeof(int64_t) + sizeof(int8_t))) {
      throw generate_exception(tableName, Code::kNotFound,
                "Unable to find m$id2 ", 
                assocType, id1, id2, id1Type, id2Type, ts, vis);
    }

    // make the key 'p'$old_ts$id2
    keysize = appendRowKeyForPayload(rowkeysize, keybuf, oldts, id2);
    leveldb::Slice pkey(keybuf, keysize);

    // if ts != oldts, then delete 'p'$old_ts$id2
    if (!newassoc) {
      char* val = (char *)value.c_str();
      extract_int64(&oldts, val);
      extract_int8(&oldvis, val + sizeof(int64_t));

      if (ts != oldts) {
        if (!db->Delete(woptions, pkey).ok()) {
          throw generate_exception(tableName, Code::kNotFound,
                "Unable to delete from p$oldts$id2 ", 
                assocType, id1, id2, id1Type, id2Type, ts, vis);
        }
      }
    }

    // store in m$id2 the value of $ts$vis
    std::string myvalue;
    appendRowKeyForMeta(rowkeysize, keybuf, id2);
    myvalue.reserve(sizeof(int64_t) + sizeof(int8_t));
    myvalue.resize(sizeof(int64_t) + sizeof(int8_t));
    makeTsVisString(&myvalue[0], ts, vis);
    leveldb::Slice sl(myvalue);
    if (!db->Put(woptions, mkey, sl).ok()) {
      // throw exception;
      throw generate_exception(tableName, Code::kNotFound,
               "Unable to put into m$id2", 
                assocType, id1, id2, id1Type, id2Type, ts, vis);
    }

    // store in p$ts$id2 the payload
    appendRowKeyForPayload(rowkeysize, keybuf, ts, id2);
    if (!db->Put(woptions, pkey, payload_slice).ok()) {
      throw generate_exception(tableName, Code::kNotFound,
               "Unable to put into p$ts$id2", 
                assocType, id1, id2, id1Type, id2Type, ts, vis);
    }

    // increment count
    if (update_count && (newassoc || oldvis != VISIBLE)) {
      assert(count >= 0);
      count++;
      appendRowKeyForCount(rowkeysize, keybuf);
      myvalue.reserve(sizeof(int64_t));
      myvalue.resize(sizeof(int64_t));
      makeCountString(&myvalue[0], count);
      leveldb::Slice count_slice(myvalue);
      if (!db->Put(woptions, ckey, count_slice).ok()) {
        throw generate_exception(tableName, Code::kNotFound,
               "Unable to insert into count", 
                assocType, id1, id2, id1Type, id2Type, ts, vis);
      }
    }
    if (update_count) {
      assert(count > 0);
      return count;
    }
    return 0;
  }

  int64_t taoAssocCountInternal(const Text& tableName, leveldb::DB* db,
                                int64_t assocType, int64_t id1) {
    // create key to query
    int maxkeysize = sizeof(id1) + sizeof(assocType) + 1;
    std::string dummy;
    dummy.reserve(maxkeysize);
    dummy.resize(maxkeysize);
    char* keybuf = &dummy[0];
    int rowkeysize = makeRowKey(keybuf, id1, assocType);
    int keysize = appendRowKeyForCount(rowkeysize, keybuf); // column 'c'
    leveldb::Slice ckey(keybuf, keysize);

    // query database to find value
    leveldb::ReadOptions roptions;
    leveldb::Status status;
    std::string value;
    int64_t count;
    status = db->Get(roptions, ckey, &value);

    // parse results retrieved from database
    if (status.IsNotFound()) {
      return 0;              // non existant assoc
    } else if (!status.ok()) {
      throw generate_exception(tableName, Code::kNotFound,
             "Unable to find count ",
             assocType, id1, 0, 0, 0, 0, Tleveldb::UNUSED1);
    }
    if (value.size() != sizeof(int64_t)) {
      printf("expected %lld got %lld\n", sizeof(int64_t), value.size());
      throw generate_exception(tableName, Code::kNotFound,
             "Bad sizes for count ",
             assocType, id1, 0, 0, 0, 0, Tleveldb::UNUSED1);
    }
    extract_int64(&count, (char *)value.c_str());
    return count;
  }

  void taoAssocGetInternal(std::vector<TaoAssocGetResult> & _return, 
                   const Text& tableName, 
                   leveldb::DB* db,
                   int64_t assocType, int64_t id1, 
                   const std::vector<int64_t> & id2s) {
    int64_t ts, id2;
    int8_t  oldvis;
    leveldb::ReadOptions roptions;
    leveldb::Status status;
    std::string value;
    int numout = 0;

    // create max key to query
    int maxkeysize = sizeof(id1) + sizeof(assocType) + 1 + sizeof(ts) +
                     sizeof(id2);
    std::string dummy;
    dummy.reserve(maxkeysize);
    dummy.resize(maxkeysize);

    // create rowkey
    char* keybuf = &dummy[0];
    int rowkeysize = makeRowKey(keybuf, id1, assocType);

    for (int index = 0; index < id2s.size(); index++) {
      int64_t id2 = id2s[index];

      // query column 'm'$id2
      int keysize = appendRowKeyForMeta(rowkeysize, keybuf, id2); 
      leveldb::Slice ckey(keybuf, keysize);
      status = db->Get(roptions, ckey, &value);

      // parse results retrieved from database
      if (status.IsNotFound()) {
        continue;              // non existant assoc
      } else if (!status.ok() ||
                  value.size() != sizeof(int64_t) + sizeof(int8_t)) {
        throw generate_exception(tableName, Code::kNotFound,
               "Unable to find m$id2 ",
               assocType, id1, id2, 0, 0, 0, Tleveldb::UNUSED1);
      }
      extractTsVisString(&ts, &oldvis, &value[0]);
      if(oldvis != AssocVisibility::VISIBLE) {
        continue;
      }
      ASSERT_NE(ts, 0);
      printf("XXX ts = %lld\n", ts);

      // this assoc is visible, scan 'p'$ts$id2 to retrieve payload.
      keysize = appendRowKeyForPayload(rowkeysize, keybuf, ts, id2); 
      leveldb::Slice pkey(keybuf, keysize);
      status = db->Get(roptions, pkey, &value);

      // parse results retrieved from database
      if (status.IsNotFound()) {
        printf("XXX2");
        continue;              // non existant assoc
      } else if (!status.ok()) {
        throw generate_exception(tableName, Code::kNotFound,
               "Unable to find m$id2 ",
               assocType, id1, id2, 0, 0, 0, Tleveldb::UNUSED1);
      }

      // update return values
      _return[numout].id2 = id2;
      _return[numout].data = value;

      // un-encoded from the payload XXX
      _return[numout].id1Type = 0;
      _return[numout].id2Type = 0;
      _return[numout].dataVersion = 0;
      numout++;
    }
  }

  // fill the row key and returns the size of the key
  inline int makeRowKey(char* dest, int64_t id1, int64_t assocType) {
    dest = copy_int64_switch_endian(dest, id1);
    dest = copy_int64_switch_endian(dest, assocType);
    return sizeof(id1) + sizeof(assocType);
  }

  // fill the row key +'c' and returns the size of the key
  inline int appendRowKeyForCount(int rowkeysize, char* dest) {
    dest += rowkeysize;
    *dest = 'c';
    return rowkeysize + 1;
  }

  // fill the row key +'p' + $ts$id2 and returns the size of the key
  inline int appendRowKeyForPayload(int rowkeysize, char* dest,
                                int64_t ts, int64_t id2) {
    dest += rowkeysize;
    *dest++ = 'p';
    dest = copy_int64_switch_endian(dest, ts);
    dest = copy_int64_switch_endian(dest, id2);
    return rowkeysize + sizeof(ts) + sizeof(id2) + 1;
  }
  // fill the row key +'m' + id2 and returns the size of the key
  inline int appendRowKeyForMeta(int rowkeysize, char* dest, 
                                 int64_t id2) {
    dest += rowkeysize;
    *dest++ = 'm';
    dest = copy_int64_switch_endian(dest, id2);
    return rowkeysize + sizeof(id2) + 1;
  }

  //
  // encode id1Type, id2Type, dataversion, etc into the payload
  inline void makePayload(char* dest, int64_t id1Type, int64_t id2Type,
                      int64_t dataVersion, const Text& data, 
                      const Text& wormhole_comment) {
    dest = copy_int64_switch_endian(dest, id1Type);
    dest = copy_int64_switch_endian(dest, id2Type);
    dest = copy_int64_switch_endian(dest, dataVersion);
    memcpy(dest, data.data(), data.size());
    dest += data.size();
    memcpy(dest, wormhole_comment.data(), wormhole_comment.size());
    dest += wormhole_comment.size();
  }

  // fill the timestamp and visibility
  inline void makeTsVisString(char* dest, int64_t ts, int8_t vis) {
    dest = copy_int64_switch_endian(dest, ts);
    *dest = vis;
  }

  // extracts the timestamp and visibility from a byte stream
  inline void extractTsVisString(int64_t* ts, int8_t* vis, char* src) {
    extract_int64(ts, src);
    extract_int8(vis, src + sizeof(*ts));
  }

  // fill the count value
  inline void makeCountString(char* dest, int64_t count) {
    dest = copy_int64_switch_endian(dest, count);
  }

  //
  // Switch endianess of the id and copy it to dest.
  // Returns the updated destination address
  //
  inline char* copy_int64_switch_endian(char* dest, int64_t id) {
    char* src = (char *)&id + sizeof(id) - 1;
    for (int i = 0; i < sizeof(id); i++) {
      *dest++ = *src--;      
    }
    return dest;
  }

  // extracts a int64 type from the char stream. Swaps endianness.
  inline void extract_int64(int64_t* dest, char* src) {
    src += sizeof(int64_t) - 1;
    for (int i = 0; i < sizeof(uint64_t); i++) {
      *dest++ = *src--;      
    }
  }

  // extracts a 1 byte integer from the char stream.
  inline void extract_int8(int8_t* dest, char* src) {
    *dest = *(int8_t *)src;
  }


  // generate an exception message
  LeveldbException generate_exception(const Text& tableName,
                      Code errorCode, const char* message,
                      int64_t assocType, int64_t id1, 
                      int64_t id2, int64_t id1Type, int64_t id2Type, 
                      int64_t ts, AssocVisibility vis) {
    char result[1024];
    sprintf(result, 
            "id1=%d assocType=%d id2=%d id1Type=%d id2Type=%d ts=%d vis=%d ", 
            id1, assocType, id2, id1Type, id2Type, ts, vis);
    fprintf(stderr, "assoc_server error table %s: %s errorCode=%d %s",
            tableName.c_str(), message, errorCode, result);

    LeveldbException e;
    e.errorCode = errorCode;
    e.message = message;
    throw e;
  }

};

#endif // THRIFT_LEVELDB_ASSOC_SERVER_H_