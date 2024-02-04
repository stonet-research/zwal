/*
 * MIT License
Copyright (c) 2021 - current
Authors:  Animesh Trivedi
This code is part of the Storage System Course at VU Amsterdam
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
 */

#include <getopt.h>
#include <unistd.h>

// #ifdef NDEBUG
// #undef NDEBUG
//#endif
#include <cassert>
#include <cstdio>
#include <iostream>
#include <memory>

#include "rocksdb/convenience.h"
#include "rocksdb/db.h"
#include "rocksdb/file_system.h"

#include "db/column_family.h"
#include "db/db_impl/db_impl.h"
#include "db/log_writer.h"
#include "db/version_set.h"

#include "file/filename.h"
#include "file/random_access_file_reader.h"
#include "file/read_write_util.h"
#include "file/writable_file_writer.h"

#include "test_util/sync_point.h"

#include "rocksdb/transaction_log.h"
#include "rocksdb/cache.h"
#include "rocksdb/file_system.h"
#include "rocksdb/write_batch.h"
#include "rocksdb/write_buffer_manager.h"
#include "util/string_util.h"

#include "logging/logging.h"


namespace ROCKSDB_NAMESPACE {

using namespace std;

struct MyRocksContext {
  std::string uri;
  rocksdb::Options options;
  rocksdb::DB *db;
  rocksdb::ConfigOptions config_options;
  std::shared_ptr<rocksdb::Env> env_guard;
};

static std::string genrate_random_string(const int len) {
  std::string str;
  static const char alphanum_char[] =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz@!#$%^&*("
      ")";
  str.reserve(len);
  for (int i = 0; i < len; ++i) {
    str += alphanum_char[rand() % (sizeof(alphanum_char))];
  }
  return str;
}

static int fill_up_map(std::map<std::string, std::string> &testmap, int entries,
                       int ksize, int vsize) {
  int count = 0;
  while (testmap.size() != (size_t)entries) {
    // the problem is that with small key sizes, we might run out of unique keys
    // to insert, hence we append the count at the end to make them unique and
    // then dynamically adjust the value size to control the total bytes of data
    // inserted in the database
    assert(count < entries);
    std::string key = genrate_random_string(ksize);
    key = key.append(to_string(count));  // this makes it unique
    auto x =
        testmap.insert({key, genrate_random_string((ksize + vsize) - key.size())
                                 .append(to_string(count))});
    // this would fail if there were not unique keys
    assert(x.second);
    (void)x;
    count++;
  }
  std::cout << "a testmap is filled with " << entries << " of max_size "
            << (ksize + vsize) << " each \n";
  return 0;
}

static int allocate_myrocks_context(struct MyRocksContext *&ctx) {
  ctx = new MyRocksContext[1];
  assert(ctx != nullptr);
  ctx->config_options.env = rocksdb::Env::Default();
  ctx->db = nullptr;
  return 0;
}

static void destroy_myrocks_context(struct MyRocksContext *&ctx) {
  assert(ctx != nullptr);
  if (ctx->db != nullptr) {
    //rocksdb::Status s = ctx->db->Close();
    assert(s.ok());
    delete ctx->db;
  }
  // free ctx
  delete[] ctx;
}

int open_zns_rocksdb(struct MyRocksContext *context, std::string db_path) {
  rocksdb::Status s;
  s = rocksdb::DB::Open(context->options, db_path, &context->db);
  if (!s.ok()) {
    return -1;
  }
  return 0;
}

// posix takes:  posix://.*"
// s2fs-rocksdb takes takes: s2fs:.*://.*
int open_rocksdb(struct MyRocksContext *context, const std::string delimiter) {
  std::string db_path = context->uri.substr(
      context->uri.find(delimiter) + delimiter.length(), context->uri.size());
  std::string uri_ext = context->uri.substr(0, context->uri.find(delimiter));
  rocksdb::Status s;
  cout << "Opening database at " << context->uri << " with uri " << uri_ext
       << " and the db_path as " << db_path << std::endl;
  if (uri_ext.compare("zns") == 0) {
    int rc = open_zns_rocksdb(context, db_path);
    if (rc != 0) {
      return rc;
    }
    cout << "## Database opened at " << context->uri << " db name is "
         << context->db->GetName() << "\n";
    return rc;
  }
  s = rocksdb::Env::CreateFromUri(context->config_options, "", context->uri,
                                  &(context->options.env), &context->env_guard);
  if (!s.ok()) {
    fprintf(stderr, "Create Env from uri failed, status %s \n",
            s.ToString().c_str());
    return -1;
  }

  // std::cout << "Environment from URI " << context->uri << " for FS "
  //           << context->options.env->GetFileSystem()->Name() << " \n";
  // s = rocksdb::DB::Open(context->options, db_path, &context->db);
  // if (!s.ok()) {
  //   fprintf(stderr, "DB opening failed at %s due to %s \n\n",
  //           context->uri.c_str(), s.ToString().c_str());
  //   return -1;
  // }
  // cout << "## Database opened at " << context->uri << " db name is "
  //      << context->db->GetName() << " , attached FS is --> "
  //      << context->db->GetFileSystem()->Name() << "<-- \n";
  return 0;
}


// if the changes are not being picked up then you need to delete and reinstall
// the file rm librocksdb.a; cp ../../../storage/rocksdb/librocksdb.a .
int run_test(int argc, char **argv) {
  (void)argc;
  (void)argv;
  srand((unsigned)time(nullptr) * getpid());
  printf("============================================================== \n");
  std::cout << "Welcome to milestone 4/5, which is about integration into "
               "RocksDB (also: congratulations for bearing with us so far!)\n";
  printf("============================================================== \n");
  const std::string delimiter = "://";
  //int ksize = 16, vsize = 3980, entires = (1024 * 1024) / 64;
  int ksize = 16, vsize = 4041, entires = (1024 * 1024) / 64;
  std::map<std::string, std::string> testdata;
  rocksdb::Status s;
  int ret;
  bool roverify = false, deleteall = false, single = false;

  MyRocksContext *ctx_test = nullptr;
  ret = allocate_myrocks_context(ctx_test);
  assert(ret == 0 && ctx_test != nullptr);


  ctx_test->uri = "zenfs://dev:nvme2n1";
  ctx_test->options.create_if_missing = true;
  ctx_test->options.write_buffer_size = 1024ULL*1024ULL*1024ULL*2ULL;

  printf("============================================================== \n");
  std::cout << "test_uri " << ctx_test->uri << "\n";
  std::cout << "entries " << entires << ", each entry ksize " << ksize
            << " bytes, vsize " << vsize
            << " bytes, readonly verify : " << roverify
            << " deleteall : " << deleteall << " single : " << single << "\n";
  printf("============================================================== \n");

  ret = open_rocksdb(ctx_test, delimiter);
  assert(ret == 0);
  assert(ctx_test->db != nullptr);

  // Init
  ctx_test->options.env->CreateDirIfMissing("wal_test");
  const std::string wal_name = "wal_test/0001.log";
  const FileOptions file_opts = FileOptions();
  
{
  std::unique_ptr<rocksdb::FSWritableFile> file;
  ctx_test->options.env->GetFileSystem()->NewWritableFile(wal_name, FileOptions(), &file,
                                                   nullptr);

  std::unique_ptr<WritableFileWriter> file_writer(
      new WritableFileWriter(std::move(file), wal_name, file_opts));
  log::Writer writer(std::move(file_writer), 1, 0);


  std::cout << "Preparing the map to insert values for " << entires
      << " entries, max size " << (ksize + vsize) << "\n";
  // init the dataset
  ret = fill_up_map(testdata, entires, ksize, vsize);
  assert(ret == 0);
  // if not just ro verify is set then that means we need to insert values
  // we have data, lets write it out to the test DB and then just compare and
  // read with the test and shadow dbs
  for (auto it = testdata.begin(); it != testdata.end(); ++it) {

    WriteBatch batch;
    batch.Put( (*it).first, (*it).second);
    WriteBatchInternal::SetSequence(&batch, 10);
    writer.AddRecord(WriteBatchInternal::Contents(&batch));
  }

 // file->Close(IOOptions(), nullptr);
}

{
    std::unique_ptr<FSSequentialFile> file;
    s = ctx_test->options.env->GetFileSystem()->NewSequentialFile(
        wal_name, ctx_test->options.env->GetFileSystem()->OptimizeForLogRead(FileOptions()), &file, nullptr);
    std::unique_ptr<SequentialFileReader> file_reader(
        new SequentialFileReader(std::move(file), wal_name, nullptr)); 

  struct LogReporter : public log::Reader::Reporter {
    Env* env;
    Logger* info_log;
    const char* fname;

    Status* status;
    bool ignore_error;  // true if db_options_.paranoid_checks==false
    void Corruption(size_t /*bytes*/, const Status& s) override {
      if (this->status->ok()) {
        // only keep the first error
        *this->status = s;
      }
    }
  };


  LogReporter reporter;
  reporter.env = ctx_test->options.env;
  reporter.info_log = ctx_test->options.info_log.get();
  reporter.fname = wal_name.c_str();
  reporter.status = &s;
  reporter.ignore_error = true;
  log::Reader reader(ctx_test->options.info_log, std::move(file_reader), &reporter,
                     true /*checksum*/, 1);

  std::string scratch;
  Slice record;                    

  SequenceNumber sequence;

  uint64_t read = 0;
  while (reader.ReadRecord(&record, &scratch)) {
    if (record.size() < WriteBatchInternal::kHeader) {
      reporter.Corruption(record.size(),
                          Status::Corruption("log record too small"));
      // TODO read record's till the first no corrupt entry?
      printf("Corruption\n");
      break;
    } else {
      WriteBatch batch;
      // We can overwrite an existing non-OK Status since it'd only reach here
      // with `paranoid_checks == false`.
      s = WriteBatchInternal::SetContents(&batch, record);
      if (s.ok()) {
        sequence = WriteBatchInternal::Sequence(&batch);
      }
    }
    read++;
  }
    printf("Read number %lu %lu\n", read, sequence);
}


  std::cout << "All values inserted, number of entries " << entires
    << ", expected size stores would be "
    << ((ksize + vsize) * entires) << " bytes \n";
  // shadow DB must be a posix db so we know it works
  // lets get an iterator
 

// will close the db - for now shadow context is created unconditionally, but
// can be moved in the single mode
destroy_myrocks_context(ctx_test);
cout << "database(s) closed, test is done OK " << endl;
return ret;
}

}

int main(int argc, char **argv) {
  return ROCKSDB_NAMESPACE::run_test(argc, argv);
} 