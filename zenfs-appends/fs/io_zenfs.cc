// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2019-present, Western Digital Corporation
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#if !defined(ROCKSDB_LITE) && !defined(OS_WIN)

#include "io_zenfs.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libzbd/zbd.h>
#include <linux/blkzoned.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <liburing.h>

#include "rocksdb/env.h"
#include "util/coding.h"

#include <cmath>


namespace ROCKSDB_NAMESPACE {

ZoneExtent::ZoneExtent(uint64_t start, uint64_t length, Zone* zone)
    : start_(start), length_(length), zone_(zone) {}

Status ZoneExtent::DecodeFrom(Slice* input) {
  if (input->size() != (sizeof(start_) + sizeof(length_)))
    return Status::Corruption("ZoneExtent", "Error: length missmatch");

  GetFixed64(input, &start_);
  GetFixed64(input, &length_);
  return Status::OK();
}

void ZoneExtent::EncodeTo(std::string* output) {
  PutFixed64(output, start_);
  PutFixed64(output, length_);
}

void ZoneExtent::EncodeJson(std::ostream& json_stream) {
  json_stream << "{";
  json_stream << "\"start\":" << start_ << ",";
  json_stream << "\"length\":" << length_;
  json_stream << "}";
}

enum ZoneFileTag : uint32_t {
  kFileID = 1,
  kFileNameDeprecated = 2,
  kFileSize = 3,
  kWriteLifeTimeHint = 4,
  kExtent = 5,
  kModificationTime = 6,
  kActiveExtentStart = 7,
  kIsSparse = 8,
  kLinkedFilename = 9,
  // APPEND-DOC, we need an additional tag to mark WAL appends
  kWALSeq = 10,
};

void ZoneFile::EncodeTo(std::string* output, uint32_t extent_start) {
  PutFixed32(output, kFileID);
  PutFixed64(output, file_id_);

  PutFixed32(output, kFileSize);
  PutFixed64(output, file_size_);

  PutFixed32(output, kWriteLifeTimeHint);
  PutFixed32(output, (uint32_t)lifetime_);

  for (uint32_t i = extent_start; i < extents_.size(); i++) {
    std::string extent_str;

    PutFixed32(output, kExtent);
    extents_[i]->EncodeTo(&extent_str);
    PutLengthPrefixedSlice(output, Slice(extent_str));
  }

  // APPEND-DOC, WALs are encoded with a WAL tag, and the sequence number
  if (is_wal_) {
    // APPEND-DOC, uncomment to debug the encoding
    // printf("WAL %s put WALSeq %lu\n", linkfiles_[0].data(), 
    //   wal_seq_.load(std::memory_order_consume));
    PutFixed32(output, kWALSeq);
    PutFixed64(output, wal_seq_.load(std::memory_order_consume));
  }

  PutFixed32(output, kModificationTime);
  PutFixed64(output, (uint64_t)m_time_);

  /* We store the current extent start - if there is a crash
   * we know that this file wrote the data starting from
   * active extent start up to the zone write pointer.
   * We don't need to store the active zone as we can look it up
   * from extent_start_ */
  PutFixed32(output, kActiveExtentStart);
  PutFixed64(output, extent_start_);

  if (is_sparse_) {
    PutFixed32(output, kIsSparse);
  }

  for (uint32_t i = 0; i < linkfiles_.size(); i++) {
    PutFixed32(output, kLinkedFilename);
    PutLengthPrefixedSlice(output, Slice(linkfiles_[i]));
  }
}

void ZoneFile::EncodeJson(std::ostream& json_stream) {
  json_stream << "{";
  json_stream << "\"id\":" << file_id_ << ",";
  json_stream << "\"size\":" << file_size_ << ",";
  json_stream << "\"hint\":" << lifetime_ << ",";
  json_stream << "\"filename\":[";

  bool first_element = true;
  for (const auto& name : GetLinkFiles()) {
    if (first_element) {
      first_element = false;
    } else {
      json_stream << ",";
    }
    json_stream << "\"" << name << "\"";
  }
  json_stream << "],";

  json_stream << "\"extents\":[";

  first_element = true;
  for (ZoneExtent* extent : extents_) {
    if (first_element) {
      first_element = false;
    } else {
      json_stream << ",";
    }
    extent->EncodeJson(json_stream);
  }
  json_stream << "]}";
}

Status ZoneFile::DecodeFrom(Slice* input) {
  uint32_t tag = 0;
  bool is_wal = false;
  uint64_t pad_sz = 0;
  uint32_t align = 0;

  GetFixed32(input, &tag);
  if (tag != kFileID || !GetFixed64(input, &file_id_))
    return Status::Corruption("ZoneFile", "File ID missing");

  while (true) {
    Slice slice;
    ZoneExtent* extent;
    Status s;
    // APPEND-DOC, we will use a once_log to store the WAL
    SZD::SZDOnceLog *wal_pos;

    if (!GetFixed32(input, &tag)) break;

    switch (tag) {
      case kFileSize:
        if (!GetFixed64(input, &file_size_))
          return Status::Corruption("ZoneFile", "Missing file size");
        break;
      case kWriteLifeTimeHint:
        uint32_t lt;
        if (!GetFixed32(input, &lt))
          return Status::Corruption("ZoneFile", "Missing life time hint");
        lifetime_ = (Env::WriteLifeTimeHint)lt;
        break;
      case kExtent:
        extent = new ZoneExtent(0, 0, nullptr);
        GetLengthPrefixedSlice(input, &slice);
        s = extent->DecodeFrom(&slice);
        if (!s.ok()) {
          delete extent;
          return s;
        }
        extent->zone_ = zbd_->GetIOZone(extent->start_);
        if (!extent->zone_)
          return Status::Corruption("ZoneFile", "Invalid zone extent");
        extent->zone_->used_capacity_ += extent->length_;

        align = extent->length_ % 4096;
        if (align) {
          pad_sz += 4096 - align;
        }

        extents_.push_back(extent);

        // APPEND-DOC, Along with the extents, we need to decode the once log.
        if (wal_ == nullptr) {
          wal_pos = zbd_->GetWAL(extent->start_);
          if (wal_pos != nullptr) {
            // printf("WAL reinited at %lu\n", extent->zone_->GetZoneNr());
            wal_ = wal_pos;
          }
        }

        break;
      case kModificationTime:
        uint64_t ct;
        if (!GetFixed64(input, &ct))
          return Status::Corruption("ZoneFile", "Missing creation time");
        m_time_ = (time_t)ct;
        break;
      case kActiveExtentStart:
        uint64_t es;
        if (!GetFixed64(input, &es))
          return Status::Corruption("ZoneFile", "Active extent start");
        extent_start_ = es;
        break;
      case kIsSparse:
        is_sparse_ = true;
        break;
      case kLinkedFilename:
        if (!GetLengthPrefixedSlice(input, &slice))
          return Status::Corruption("ZoneFile", "LinkFilename missing");

        if (slice.ToString().length() == 0)
          return Status::Corruption("ZoneFile", "Zero length Linkfilename");
        linkfiles_.push_back(slice.ToString());
        break;
// APPEND-DOC, WAL-entry
      case kWALSeq:
        uint64_t wal_seq;
        is_wal = true;
        if (!GetFixed64(input, &wal_seq))
          return Status::Corruption("ZoneFile", "Missing sequence number");
        // printf("Recovered wal sequence number %lu\n", wal_seq);
        wal_seq_.store(wal_seq, std::memory_order_acquire);
        break;
      default:
        return Status::Corruption("ZoneFile", "Unexpected tag");
    }
  }

  if (is_wal) {
  #ifdef WAL_BARRIERS
    append_bytes_since_last_barrier_ = (file_size_ + pad_sz) % (WAL_BARRIER_SIZE_IN_KB * KiB);
  #endif

    if (wal_) {
      uint64_t wal_size = (wal_->GetWriteHead() - wal_->GetWriteTail()) * 4096;
      printf("WAL size %lu \n", wal_size);
    }

    uint64_t ext = extents_.size() ? (extents_[0]->start_ / zbd_->GetZoneSize()) : 0xdeadbeef;
  #ifdef WAL_BARRIERS
    printf("Last write (recover) %lu: %lu %lu \n", ext, append_bytes_since_last_barrier_, pad_sz);
  #endif
  }

  MetadataSynced();
  return Status::OK();
}

Status ZoneFile::MergeUpdate(std::shared_ptr<ZoneFile> update, bool replace) {
  if (file_id_ != update->GetID())
    return Status::Corruption("ZoneFile update", "ID missmatch");

  SetFileSize(update->GetFileSize());
  SetWriteLifeTimeHint(update->GetWriteLifeTimeHint());
  SetFileModificationTime(update->GetFileModificationTime());

  // APPEND-DOC, update sequence number if it has increased in the merge
  uint64_t wal_seq = wal_seq_.load(std::memory_order_consume);
  if (update->GetWALSeq() > wal_seq) {
    wal_seq_.store(update->GetWALSeq(), std::memory_order_acquire);
    // printf("WAL seq %s updated to %lu \n", GetFilename().c_str(), wal_seq_.load(std::memory_order_consume));
  }

  if (replace) {
    // printf("Replacing is a hack that should be illegal\n");
    ClearExtents();
  }

  std::vector<ZoneExtent*> update_extents = update->GetExtents();
  for (long unsigned int i = 0; i < update_extents.size(); i++) {
    ZoneExtent* extent = update_extents[i];
    Zone* zone = extent->zone_;
    zone->used_capacity_ += extent->length_;
    extents_.push_back(new ZoneExtent(extent->start_, extent->length_, zone));
  }
  extent_start_ = update->GetExtentStart();
  is_sparse_ = update->IsSparse();
  MetadataSynced();

  linkfiles_.clear();
  for (const auto& name : update->GetLinkFiles()) linkfiles_.push_back(name);

  return Status::OK();
}

ZoneFile::ZoneFile(ZonedBlockDevice* zbd, uint64_t file_id,
                   MetadataWriter* metadata_writer)
    : zbd_(zbd),
      active_zone_(NULL),
      extent_start_(NO_EXTENT),
      extent_filepos_(0),
      lifetime_(Env::WLTH_NOT_SET),
      io_type_(IOType::kUnknown),
      file_size_(0),
      file_id_(file_id),
      nr_synced_extents_(0),
      m_time_(0),
      metadata_writer_(metadata_writer) {}

std::string ZoneFile::GetFilename() { return linkfiles_[0]; }
time_t ZoneFile::GetFileModificationTime() { return m_time_; }

inline bool ends_with(std::string const& value, std::string const& ending) {
  if (ending.size() > value.size()) return false;
  return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}

uint64_t ZoneFile::GetFileSize() { return file_size_; }
void ZoneFile::SetFileSize(uint64_t sz) { file_size_ = sz; }
void ZoneFile::SetFileModificationTime(time_t mt) { m_time_ = mt; }
void ZoneFile::SetIOType(IOType io_type) { 
  io_type_ = io_type; 
  // APPEND-DOC, try to immediately set the file to WAL (even before extension is set)
  is_wal_ = io_type_ == IOType::kWAL && ends_with(GetFilename(), ".log");
  // printf("%s isWAL %d\n", GetFilename().data(), is_wal_);
}

ZoneFile::~ZoneFile() { 
  // APPEND-DOC, write diagnostics (I know that this is an anti-pattern)
#ifdef MEASURE_WAL_LAT
  if (wal_read_time_count_ > 0) {
    printf("Wal recovery, calls:%lu sum_time:%lu average:%f standard_dev:%f copy_sum:%lu seq_read_sum:%lu sort_sum:%lu decode:%lu\n", 
      wal_read_time_count_,
      wal_read_time_sum_,
      static_cast<double>(wal_read_time_sum_) / static_cast<double>(wal_read_time_count_),
      std::sqrt(static_cast<double>(wal_read_time_sum_squared_ * wal_read_time_count_ -
                                                   wal_read_time_sum_ * wal_read_time_sum_) /
                               static_cast<double>(wal_read_time_count_ * wal_read_time_count_)),
      wal_copy_time_sum_,
      wal_seq_read_time_sum_,
      wal_sort_time_sum_,
      wal_decode_time_sum_
    );
  }
#endif

  // APPEND-DOC, sync all WAL business
  if (wal_) {
    uint64_t ext = extents_.size() ? (extents_[0]->start_ / zbd_->GetZoneSize()) : 0xdeadbeef;
  #ifdef WAL_BARRIERS
    printf("Last write (close) %lu: %lu %lu \n",ext, append_bytes_since_last_barrier_, wal_syncs_);
    if (wal_writes_>0) printf("Correct Nameless syncs:%lu writes:%lu ratio:%f\n", wal_syncs_, wal_writes_, (double)wal_writes_
      / (double)wal_syncs_);
  #endif  
    // An anti-pattern to circumentvent weirdness when sync is not called before deletion.
    zbd_->AppendSync(wal_);
    wal_->Sync();
    delete wal_;
  } 
  ClearExtents(); 
#ifdef WAL_BARRIERS
  //if (wal_) {
  //    printf("DELETE WAL \n"); fflush(stdout);
  //}
  if (loaded_wal_chunks_.wal_entries_.size() > 0) {
    for (auto w : loaded_wal_chunks_.wal_entries_) {
      delete w.second;
    }
  }
  loaded_wal_chunks_.wal_entries_.clear();
  //if (wal_) {
      //printf("DELETED WAL \n"); fflush(stdout);
  //}
#else
  if (wal_entries_.size() > 0) {
    for (auto w : wal_entries_) {
      delete w.second;
    }
  }
  wal_entries_.clear();
#endif
}

// APPEND-DOC, Reset the WAL zones (needed as they are now in different zonesets than files)
IOStatus ZoneFile::ResetWALZones() {
  IOStatus s = IOStatus::OK();
  Zone* z = nullptr;
  if (extents_.size() > 0 && !wal_) {
    z = extents_[0]->zone_;
    while (!z->Acquire())
      ;
    // printf("WAL Resetting zones from %lu to %lu \n", z->GetZoneNr(), z->GetZoneNr() + 3);
    s = zbd_->OpenWALZone(&wal_, z);
    if (!s.ok()) return s;  
  } 

  if (wal_) {
    s = wal_->ResetAll() == SZD::SZDStatus::Success 
      ? IOStatus::OK() 
      : IOStatus::IOError("WAL sync error");
    if (s.ok()) {
      s = zbd_->ReleaseUnusedWALZones();
    }
  }

  if (z != nullptr) {
    z->Release();
  }

  return s;
}

void ZoneFile::ClearExtents() {
  for (auto e = std::begin(extents_); e != std::end(extents_); ++e) {
    Zone* zone = (*e)->zone_;

    assert(zone && zone->used_capacity_ >= (*e)->length_);
    zone->used_capacity_ -= (*e)->length_;
    delete *e;
  }
  extents_.clear();
}

IOStatus ZoneFile::CloseActiveZone() {
  IOStatus s = IOStatus::OK();
  // APPEND-DOC, force sync the WAL appends
  if (is_wal_ && wal_ != NULL) {
    s = wal_->Sync() == SZD::SZDStatus::Success 
      ? IOStatus::OK() 
      : IOStatus::IOError("Failed syncing WAL");
  }

  if (active_zone_) {
    bool full = active_zone_->IsFull();
    s = active_zone_->Close();
    ReleaseActiveZone();
    if (!s.ok()) {
      return s;
    }
    zbd_->PutOpenIOZoneToken();
    if (full) {
      zbd_->PutActiveIOZoneToken();
    }
  }
  return s;
}

void ZoneFile::AcquireWRLock() {
  open_for_wr_mtx_.lock();
  open_for_wr_ = true;
}

bool ZoneFile::TryAcquireWRLock() {
  if (!open_for_wr_mtx_.try_lock()) return false;
  open_for_wr_ = true;
  return true;
}

void ZoneFile::ReleaseWRLock() {
  assert(open_for_wr_);
  open_for_wr_ = false;
  open_for_wr_mtx_.unlock();
}

bool ZoneFile::IsOpenForWR() { return open_for_wr_; }

IOStatus ZoneFile::CloseWR() {
  IOStatus s;
  /* Mark up the file as being closed */
  extent_start_ = NO_EXTENT;
  s = PersistMetadata();
  if (!s.ok()) return s;
  ReleaseWRLock();
  return CloseActiveZone();
}

IOStatus ZoneFile::PersistMetadata() {
  assert(metadata_writer_ != NULL);
  return metadata_writer_->Persist(this);
}

// APPEND-DOC, modified to support both WAL and file
ZoneExtent* ZoneFile::GetExtent(uint64_t file_offset, uint64_t* dev_offset) {
  for (unsigned int i = 0; i < extents_.size(); i++) {
    if (file_offset < extents_[i]->length_) {
      *dev_offset = extents_[i]->start_ + file_offset;
        // printf("  Picked Extend id:%u offset:%lu (mark)\n", i, file_offset);
      return extents_[i];
    } else {
      file_offset -= extents_[i]->length_;
    }
  }
  return NULL;
}

// APPEND-DOC, get WALextent
ZoneExtent* ZoneFile::GetWALExtent(uint64_t file_offset, uint64_t* dev_offset, 
  uint64_t* index) {
  for (unsigned int i = *index; i < extents_.size(); i++) {
    if (file_offset < extents_[i]->length_) {
      *dev_offset = file_offset;
      *index = i;
      // printf("  Picked WAL Extend id:%u (seq%lu) offset:%lu start(%lu) (mark)\n", i, 
      // wal_entries_[i].first,
      // file_offset, extents_[i]->start_);
      // printf("Get WAL extent id %lu\n", i);
      return extents_[i];
    } else {
      file_offset -= extents_[i]->length_;
    }
  }
  printf("Extent could not be found?\n");
  return NULL;
}

IOStatus ZoneFile::InvalidateCache(uint64_t pos, uint64_t size) {
  ReadLock lck(this);
  uint64_t offset = pos;
  uint64_t left = size;
  IOStatus s = IOStatus::OK();

  if (left == 0) {
    left = GetFileSize();
  }

  while (left) {
    uint64_t dev_offset;
    ZoneExtent* extent = GetExtent(offset, &dev_offset);

    if (!extent) {
      s = IOStatus::IOError("Extent not found while invalidating cache");
      break;
    }

    uint64_t extent_end = extent->start_ + extent->length_;
    uint64_t invalidate_size = std::min(left, extent_end - dev_offset);

    s = zbd_->InvalidateCache(dev_offset, invalidate_size);
    if (!s.ok()) break;

    left -= invalidate_size;
    offset += invalidate_size;
  }

  return s;
}

static uint64_t shift_block_size(uint64_t blocksize) {
  switch (blocksize) {
    case 512: return 9ULL; break;
    default: break;
  }
  return 12ULL;
}

#ifdef MEASURE_WAL_LAT
static uint64_t get_timespan(const struct timespec& begin, struct timespec& end) {
  uint64_t time_passed = 0; 
  if (end.tv_nsec < begin.tv_nsec) {
    time_passed += 1000'000'000 - (begin.tv_nsec - end.tv_nsec);
    end.tv_sec -= 1; 
  } else {
    time_passed += end.tv_nsec - begin.tv_nsec;
  }
  time_passed += 1000'000'000 * (end.tv_sec - begin.tv_sec);
  return time_passed;
}
#endif

IOStatus ZoneFile::RecoverWALChunk(uint64_t begin, uint64_t end, std::vector<std::pair<uint64_t, std::string*>>*  wal_entries) {
  IOStatus s = IOStatus::OK();
  char* ptr;
  size_t str_size = end - begin;

#ifdef MEASURE_WAL_LAT
	struct timespec tp_begin_read_io;
	struct timespec tp_end_read_io;
	struct timespec tp_begin_chunk;
	struct timespec tp_end_chunk;
	struct timespec tp_begin_sort;
	struct timespec tp_end_sort;
#endif

  //printf("RecoverWALChunk, IN %lu OUT %lu   %lu %lu\n", begin, end, wal_->GetWriteTail() << shift_block_size(GetBlockSize()), 
  //wal_->GetWriteHead() << shift_block_size(GetBlockSize()));
  //fflush(stdout);

  // Cleanup leaks
  {
    if (wal_entries->size() > 0) {
      for (auto w : *wal_entries) {
        delete w.second;
      }
    }
    wal_entries->clear();
  }
#ifdef MEASURE_WAL_LAT
  clock_gettime(CLOCK_MONOTONIC, &tp_begin_read_io);
#endif  
  // Read from storage
  {
    // printf("ALLOC SIZE %lu\n", str_size);
    ptr = new char[str_size+1];
    // printf("WAL %lu %lu %lu %lu %lu\n", 
    //   wal_->GetWriteTail(), 
    //   begin >> shift_block_size(GetBlockSize()), 
    //   str_size, 
    //   wal_->GetWriteHead(), 
    //   wal_->GetWriteTail() + (str_size + GetBlockSize()-1) / GetBlockSize());
    s = wal_->Read(begin >> shift_block_size(GetBlockSize()), ptr, str_size, true) == SZD::SZDStatus::Success 
      ? IOStatus::OK() 
      : IOStatus::IOError("Error WAL read I/O");
    if (!s.ok()) return s;
  }
#ifdef MEASURE_WAL_LAT
  clock_gettime(CLOCK_MONOTONIC, &tp_end_read_io);
  wal_seq_read_time_sum_ += get_timespan(tp_begin_read_io, tp_end_read_io);
  clock_gettime(CLOCK_MONOTONIC, &tp_begin_chunk);
#endif
  // Read entries
  {
    uint64_t r = 0, br = 0;
    uint64_t max_seq = 0;
    const uint64_t header_sz = ZoneFile::SPARSE_HEADER_SIZE +  ZoneFile::SPARSE_WAL_HEADER_SIZE;

    // Read entries one-by-one
    while (r < str_size && br < str_size) {
      // Decode extent header
      uint64_t size = DecodeFixed64(ptr);
      uint64_t seqn = DecodeFixed64(ptr + sizeof(uint64_t));

      // We reached into the padding zone
      if (!size) {
        break;
      }

      // Increment sequence number in batch
      if (seqn > max_seq)
        max_seq = seqn;

      // Padding region
      if (seqn == 0 && begin > wal_->GetWriteTail() << shift_block_size(GetBlockSize()))
        break;

      //printf("SEQN %lu\n", seqn);

      std::string* dat = new std::string;
      // IMPORTANT: We skip the header! We do not NEED the information in the buffer
      dat->assign(ptr + header_sz, size);
      wal_entries->push_back(std::make_pair(seqn, dat));

      // printf("Decoded WAL entry %lu - %lu %lu %lu\n", br, r, size, seqn);

      // Move to the next extent
      br += size;
      r += size + header_sz;
      ptr += size + header_sz;
    }

    // Corruption
    if (r > str_size) {
      s = IOStatus::Corruption("Overshooting WAL\n");
      return s;
    }
  }
#ifdef MEASURE_WAL_LAT
  clock_gettime(CLOCK_MONOTONIC, &tp_end_chunk);
  wal_decode_time_sum_ += get_timespan(tp_begin_chunk, tp_end_chunk);
  clock_gettime(CLOCK_MONOTONIC, &tp_begin_sort);
#endif
  // Sort entries
  {
    std::sort(
      wal_entries->begin(), wal_entries->end(),
      [](std::pair<uint64_t, std::string*> a,
        std::pair<uint64_t, std::string*> b) { return a.first < b.first; });
  }
  // delete[] ptr;
#ifdef MEASURE_WAL_LAT
    clock_gettime(CLOCK_MONOTONIC, &tp_end_sort);
    wal_sort_time_sum_ += get_timespan(tp_begin_sort, tp_end_sort);
#endif

  return s;
}


#ifndef WAL_BARRIERS
IOStatus ZoneFile::RecoverEntireWAL() {
  IOStatus s = IOStatus::OK();
  // Ensure that we can recover and that there is something to recover in the first place.
  if (extents_.size() == 0) {
    return s;
  }
  if (!wal_) {
    s = zbd_->OpenWALZone(&wal_, extents_[0]->zone_);
    if (!s.ok()) return s;
  }

  //printf("Opened WAL...\n");
  fflush(stdout);

  // Recover chunk
  s = RecoverWALChunk(
     wal_->GetWriteTail() << shift_block_size(GetBlockSize()),
     wal_->GetWriteHead() << shift_block_size(GetBlockSize()), 
     &wal_entries_
  );
  if (!s.ok()) return s;

  // Yes +1. The sequence number is pointing to where the new entry will be stored.
  // if (wal_seq_.load(std::memory_order_consume) != max_seq + 1 || max_seq + 1 != wal_entries_.size()) {
  //   return IOStatus::Corruption("Incorrect sequence numbers in WAL\n");
  // }
  return s;
}
#endif

IOStatus ZoneFile::TryRecoverWAL(uint64_t offset) {
  IOStatus s = IOStatus::OK();
#ifdef WAL_BARRIERS
  // printf("In chunk start:%lu end:%lu jump%lu offset:%lu \n", loaded_wal_chunks_.start_,
  // loaded_wal_chunks_.end_, loaded_wal_chunks_.jump_, offset);
  // fflush(stdout);

  // Still in range
  if (loaded_wal_chunks_.wal_entries_.size() && loaded_wal_chunks_.end_ > offset) {
    return s;
  }

  // Ensure WAL is ready
  if (!wal_) {
    s = zbd_->OpenWALZone(&wal_, extents_[0]->zone_);
    if (!s.ok()) return s;
  }

  // Stupid RocksDB thinks this is ok.
  offset = std::min(offset, file_size_);

  // Repeatedly load the next chunk if needed
  do {
    uint64_t jump = loaded_wal_chunks_.wal_entries_.size();

    // Calculate the next chunk
    // uint64_t chunk_id =  offset / (WAL_BARRIER_SIZE_IN_KB * KiB);
    uint64_t lba_in = (wal_->GetWriteTail() << shift_block_size(GetBlockSize())) 
      + chunk_id_ * ((WAL_BARRIER_SIZE_IN_KB * KiB));
    uint64_t lba_out = (wal_->GetWriteTail() << shift_block_size(GetBlockSize())) 
      + (chunk_id_+1) * ((WAL_BARRIER_SIZE_IN_KB * KiB));
    // Safety first
    lba_in = std::min(lba_in, wal_->GetWriteHead() << shift_block_size(GetBlockSize()));
    lba_out = std::min(lba_out, wal_->GetWriteHead() << shift_block_size(GetBlockSize()));
    // printf("LBA %lu - %lu\n", lba_in, lba_out); fflush(stdout);
    if (lba_in >= lba_out) {
      // printf("in >= out\n");fflush(stdout);
      break;
    }

    // Load next chunk
    s = RecoverWALChunk(lba_in, lba_out, &(loaded_wal_chunks_.wal_entries_));
    if (!s.ok()) {
      // printf("Errored \n");fflush(stdout);
      return s;
    }

    // Get complete size
    uint64_t extent_size = 0;
    for (const auto& extent : loaded_wal_chunks_.wal_entries_) {
      extent_size += extent.second->size();
    }

    // Update chunk info
    loaded_wal_chunks_.start_ = loaded_wal_chunks_.end_;
    loaded_wal_chunks_.end_ =  loaded_wal_chunks_.start_ + extent_size;
    loaded_wal_chunks_.jump_ += jump;

    //printf("Out chunk:%lu start:%lu end:%lu jump%lu offset:%lu \n", chunk_id_, loaded_wal_chunks_.start_,
    //  loaded_wal_chunks_.end_, loaded_wal_chunks_.jump_, offset); fflush(stdout);

    // Update chunk_id
    chunk_id_++;
  } while (loaded_wal_chunks_.wal_entries_.size() && loaded_wal_chunks_.end_ < offset);

#else
  //APPEND-DOC, We need to first load the WAL
  if (wal_entries_.size() == 0) {
    s = RecoverEntireWAL();
    if (!s.ok()) return s;
  }
  (void)offset;
#endif
  return s;
};


// APPEND-DOC, a WAL read is very different. Here we read whether it is a WAL or not
IOStatus ZoneFile::WALPositionedRead(uint64_t offset, size_t n, Slice* result,
                                  char* scratch, bool direct) {
                              
#ifdef MEASURE_WAL_LAT
  struct timespec tp_begin_wal_read;
  struct timespec tp_end_wal_read;

  struct timespec tp_begin_copy;
  struct timespec tp_end_copy; 
#endif
 ZenFSMetricsLatencyGuard guard(zbd_->GetMetrics(), ZENFS_READ_LATENCY,
                                 Env::Default());
  zbd_->GetMetrics()->ReportQPS(ZENFS_READ_QPS, 1);

  ReadLock lck(this);

  IOStatus s;
  bool iswal = ends_with(linkfiles_[0], ".log");
  char* ptr;
  uint64_t r_off = 0;
  size_t r_sz;
  ssize_t r = 0;
  size_t read = 0;
  ZoneExtent* extent;
  uint64_t extent_end;
  uint64_t extend_id = 0;

  // Read OOB?
  if (offset >= file_size_) {
    *result = Slice(scratch, 0);
    if (iswal) printf("  PositionedRead (OOB) %lu %lu\n", offset, n);
    return IOStatus::OK();
  }

  if (iswal) {
    // printf("Get WAL %lu %lu\n", offset, n); fflush(stdout);
#ifdef MEASURE_WAL_LAT
    clock_gettime(CLOCK_MONOTONIC, &tp_begin_wal_read);
#endif
    s = TryRecoverWAL(offset);
    if (!s.ok())  {
      //printf("ERROR\n"); fflush(stdout);
      return s;
    }
    // APPEND-DOC, Hack the extend_id
    extend_id = reader_offset_index_;
  }

  // APPEND-DOC, get extent
  extent = iswal 
    ? GetWALExtent(r_off, &r_off, &extend_id) 
    : GetExtent(offset, &r_off);
  // With the WAL we are working inside a larger buffer, not a single read
  if (iswal) r_off = reader_offset_;
  
  /* read start beyond end of (synced) file data*/
  if (!extent) {
    //printf("FIRST ISSUE\n"); fflush(stdout);
    *result = Slice(scratch, 0);
    return s;
  }
  extent_end = iswal
    ? extent->length_
    : extent->start_ + extent->length_;

  /* Limit read size to end of file */
  if ((offset + n) > file_size_)
    r_sz = file_size_ - offset;
  else
    r_sz = n;

  ptr = scratch;

  while (read != r_sz) {
    size_t pread_sz = r_sz - read;

    if ((pread_sz + r_off) > extent_end) pread_sz = extent_end - r_off;

    /* We may get some unaligned direct reads due to non-aligned extent lengths,
     * so increase read request size to be aligned to next blocksize boundary.
     */
    bool aligned = (pread_sz % zbd_->GetBlockSize() == 0);

    size_t bytes_to_align = 0;
    if (direct && !aligned) {
      bytes_to_align = zbd_->GetBlockSize() - (pread_sz % zbd_->GetBlockSize());
      pread_sz += bytes_to_align;
      aligned = true;
    }

    // APPEND-DOC, copy into mem.
    if (iswal) {
#ifdef MEASURE_WAL_LAT
    clock_gettime(CLOCK_MONOTONIC, &tp_begin_copy);
#endif
#ifdef WAL_BARRIERS
      // printf("Reading chunksize:%lu extid:%lu jump:%lu offs:%lu sz:%lu\n", 
      //   loaded_wal_chunks_.wal_entries_.size(), extend_id, loaded_wal_chunks_.jump_, r_off, pread_sz); fflush(stdout);
      // for (int i = 0; i < pread_sz; i++) {
      //   printf("%c", loaded_wal_chunks_.wal_entries_[extend_id-loaded_wal_chunks_.jump_].second->data() + r_off + i);
      // }
      // printf("\n");
      // fflush(stdout);
      memcpy(ptr, loaded_wal_chunks_.wal_entries_[extend_id-loaded_wal_chunks_.jump_].second->data() + r_off, pread_sz);
#else
      // for (size_t i = 0; i < pread_sz; i++) {
      //   printf("%c", wal_entries_[extend_id].second->data()[r_off + i]);
      // }
      // printf("\n");
      memcpy(ptr, wal_entries_[extend_id].second->data() + r_off, pread_sz);
#endif
#ifdef MEASURE_WAL_LAT
      clock_gettime(CLOCK_MONOTONIC, &tp_end_copy);
      wal_copy_time_sum_ +=  get_timespan(tp_begin_copy, tp_end_copy);
#endif
      r = pread_sz;
    } else {
      r = zbd_->Read(ptr, r_off, pread_sz, direct && aligned);
    }
    // Read error
    if (r <= 0) break;

    /* Verify and update the the bytes read count (if read size was incremented,
     * for alignment purposes).
     */
    if ((size_t)r <= pread_sz - bytes_to_align)
      pread_sz = (size_t)r;
    else
      pread_sz -= bytes_to_align;

    ptr += pread_sz;
    read += pread_sz;
    r_off += pread_sz;
    // printf("%lu %lu %lu\n", read, pread_sz, r_sz); fflush(stdout);

    // Get next extent
    if (read != r_sz && r_off == extent_end) {
      // We might need to load the next extent from storage
      if (iswal){
        s = TryRecoverWAL(offset+read);
        if (!s.ok())  {
          printf("ERROR\n"); fflush(stdout);
          return s;
        }
      }
      // Get pointers
      extent = iswal 
        ? GetWALExtent(r_off, &r_off, &extend_id)  
        : GetExtent(offset + read, &r_off);

      if (!extent) {
          //printf("SECOND ISSUE\n"); fflush(stdout);
          /* read beyond end of (synced) file data */
          break;
        }
      r_off = iswal ? 0 : extent->start_;
      extent_end = iswal ? extent->length_ : extent->start_ + extent->length_;
    }
  }

  if (iswal && r_off == extent_end) {
      // We might need to load the next extent from storage
      if (iswal){
        s = TryRecoverWAL(offset+read);
        if (!s.ok())  {
          printf("ERROR\n"); fflush(stdout);
          return s;
        }
      }
      // Get pointers
      extent = iswal 
        ? GetWALExtent(r_off, &r_off, &extend_id)  
        : GetExtent(offset + read, &r_off);

      if (extent) {
        r_off = iswal ? 0 : extent->start_;
        extent_end = iswal ? extent->length_ : extent->start_ + extent->length_;
      } else {
        //printf("THIRD ISSUE %lu %lu\n", r_off, extent_end); fflush(stdout);
      }
  } 

  if (r < 0) {
    s = IOStatus::IOError("pread error\n");
    printf("READ ERROR\n"); fflush(stdout);
    read = 0;
  }

  *result = Slice((char*)scratch, read);

  if (iswal) {
      // printf("Read %lu %lu %lu %lu %d\n", offset, n, read, file_size_, s.ok()); fflush(stdout);
#ifdef MEASURE_WAL_LAT
    clock_gettime(CLOCK_MONOTONIC, &tp_end_wal_read);
    uint64_t time_passed =  get_timespan(tp_begin_wal_read, tp_end_wal_read);
    wal_read_time_count_++;
    wal_read_time_sum_  += time_passed;
    wal_read_time_sum_squared_ += time_passed * time_passed;
#endif
    reader_offset_index_ = extend_id;
    reader_offset_ = r_off;
  }
  last_read = offset + read;

  return s;
}


// APPEND-DOC, old file read implementation
IOStatus ZoneFile::NormalPositionedRead(uint64_t offset, size_t n, Slice* result,
                                  char* scratch, bool direct) {
   ZenFSMetricsLatencyGuard guard(zbd_->GetMetrics(), ZENFS_READ_LATENCY,
                                 Env::Default());
  zbd_->GetMetrics()->ReportQPS(ZENFS_READ_QPS, 1);

  ReadLock lck(this);

  IOStatus s;
  char* ptr;
  uint64_t r_off;
  size_t r_sz;
  ssize_t r = 0;
  size_t read = 0;
  ZoneExtent* extent;
  uint64_t extent_end;

  if (offset >= file_size_) {
    *result = Slice(scratch, 0);
    return IOStatus::OK();
  }

  r_off = 0;
  extent = GetExtent(offset, &r_off);
  if (!extent) {
    /* read start beyond end of (synced) file data*/
    *result = Slice(scratch, 0);
    return s;
  }
  extent_end = is_wal_
    ? extent->length_
    : extent->start_ + extent->length_;

  /* Limit read size to end of file */
  if ((offset + n) > file_size_)
    r_sz = file_size_ - offset;
  else
    r_sz = n;

  ptr = scratch;

  while (read != r_sz) {
    size_t pread_sz = r_sz - read;

    if ((pread_sz + r_off) > extent_end) pread_sz = extent_end - r_off;

    /* We may get some unaligned direct reads due to non-aligned extent lengths,
     * so increase read request size to be aligned to next blocksize boundary.
     */
    bool aligned = (pread_sz % zbd_->GetBlockSize() == 0);

    size_t bytes_to_align = 0;
    if (direct && !aligned) {
      bytes_to_align = zbd_->GetBlockSize() - (pread_sz % zbd_->GetBlockSize());
      pread_sz += bytes_to_align;
      aligned = true;
    }

    r = zbd_->Read(ptr, r_off, pread_sz, direct && aligned);    
    if (r <= 0) break;

    /* Verify and update the the bytes read count (if read size was incremented,
     * for alignment purposes).
     */
    if ((size_t)r <= pread_sz - bytes_to_align)
      pread_sz = (size_t)r;
    else
      pread_sz -= bytes_to_align;

    ptr += pread_sz;
    read += pread_sz;
    r_off += pread_sz;

    if (read != r_sz && r_off == extent_end) {
      extent = GetExtent(offset + read, &r_off);
      if (!extent) {
          break;
        }
      r_off = extent->start_;
      extent_end = extent->start_ + extent->length_;
    }
  }

  if (r < 0) {
    s = IOStatus::IOError("pread error\n");
    read = 0;
  }

  *result = Slice((char*)scratch, read);
  return s;
}

IOStatus ZoneFile::PositionedRead(uint64_t offset, size_t n, Slice* result,
                                  char* scratch, bool direct) {
  IOStatus s = WALPositionedRead(offset, n, result, scratch, direct);
  return s;
}

void ZoneFile::PushExtent() {
  uint64_t length;

  assert(file_size_ >= extent_filepos_);

  if (!active_zone_) return;

  length = file_size_ - extent_filepos_;
  if (length == 0) return;

  assert(length <= (active_zone_->wp_ - extent_start_));
  extents_.push_back(new ZoneExtent(extent_start_, length, active_zone_));

  active_zone_->used_capacity_ += length;
  extent_start_ = active_zone_->wp_;
  extent_filepos_ = file_size_;
}

IOStatus ZoneFile::AllocateNewZone() {
  IOStatus s;
  Zone* zone;

  // APPEND-DOC, Use a WAL zone for a WAL
  if (is_wal_) {
      Zone *z = nullptr;
      for (auto e : extents_) {
        if (z == nullptr) {
          z = e->zone_;
        } else if (e->zone_->GetZoneNr() > z->GetZoneNr()) {
          z = e->zone_;
        }
      }
      //  APPEND-loG crossing a zone is a barrier (but annoying to fix)
      // append_bytes_since_last_barrier_ = 0;
      s = zbd_->AllocateWALZone(&zone, &wal_, z);
    if (!s.ok()) return s;
  } else {
    s = zbd_->AllocateIOZone(lifetime_, io_type_, &zone);
  }

  if (!s.ok()) return s;
  if (!zone) {
    return IOStatus::NoSpace("Zone allocation failure\n");
  }
  SetActiveZone(zone);
  extent_start_ = active_zone_->wp_;
  extent_filepos_ = file_size_;

  /* Persist metadata so we can recover the active extent using
     the zone write pointer in case there is a crash before syncing */
  return PersistMetadata();
}

/* Byte-aligned writes without a sparse header */
IOStatus ZoneFile::BufferedAppend(char* buffer, uint32_t data_size) {
  uint32_t left = data_size;
  uint32_t wr_size;
  uint32_t block_sz = GetBlockSize();
  IOStatus s;

  if (active_zone_ == NULL) {
    s = AllocateNewZone();
    if (!s.ok()) return s;
  }

  while (left) {
    wr_size = left;

    if (wr_size > active_zone_->capacity_) wr_size = active_zone_->capacity_;
    // APPEND_LOG, Prevent cross-barrier write
  #ifdef WAL_BARRIERS
    if (is_wal_ && wr_size > (WAL_BARRIER_SIZE_IN_KB * KiB - append_bytes_since_last_barrier_)) 
      wr_size = WAL_BARRIER_SIZE_IN_KB * KiB - append_bytes_since_last_barrier_;
  #endif
    /* Pad to the next block boundary if needed */
    uint32_t align = wr_size % block_sz;
    uint32_t pad_sz = 0;

    if (align) pad_sz = block_sz - align;

    /* the buffer size s aligned on block size, so this is ok*/
    if (pad_sz) memset(buffer + wr_size, 0x0, pad_sz);

    uint64_t extent_length = wr_size;

    // APPEND-DOC, write to WAL with a zone append, to a file with a write (Append is write in ZenFS...)
    if (is_wal_) {
      // APPEND-DOC do the sync (first)
    #ifdef WAL_BARRIERS
      if (append_bytes_since_last_barrier_ >= WAL_BARRIER_SIZE_IN_KB * KiB) {
        wal_->Sync();
        append_bytes_since_last_barrier_ = 0;
        wal_syncs_++;
      }
      wal_writes_++;
    #endif
      s = active_zone_->ZoneAppend(buffer, wr_size + pad_sz, wal_);
    #ifdef WAL_BARRIERS
      append_bytes_since_last_barrier_ += (wr_size + pad_sz);
    #endif
      if (!s.ok()) return s;
    } else {
      s = active_zone_->Append(buffer, wr_size + pad_sz);
      if (!s.ok()) return s;
    }

    extents_.push_back(
        new ZoneExtent(extent_start_, extent_length, active_zone_));

    extent_start_ = active_zone_->wp_;
    active_zone_->used_capacity_ += extent_length;
    file_size_ += extent_length;
    left -= extent_length;

    if (active_zone_->capacity_ == 0) {
      s = CloseActiveZone();
      if (!s.ok()) {
        return s;
      }
      if (left) {
        memmove((void*)(buffer), (void*)(buffer + wr_size), left);
      }
      s = AllocateNewZone();
      if (!s.ok()) return s;
    }
  }

  return IOStatus::OK();
}

/* Byte-aligned, sparse writes with inline metadata
   the caller reserves 8 bytes of data for a size header */
IOStatus ZoneFile::SparseAppend(char* sparse_buffer, uint32_t data_size) {
  uint32_t left = data_size;
  uint32_t wr_size;
  // TODO: fix APPEND-DOC, only works with 4KiB for now
  uint32_t block_sz = 4096; // GetBlockSize();
  IOStatus s;

  if (active_zone_ == NULL) {
    s = AllocateNewZone();
    if (!s.ok()) return s;
  }

  while (left) {
    // APPEND-DOC, append has more data in nwals
    uint64_t header_size = ZoneFile::SPARSE_HEADER_SIZE + 
      (is_wal_ * ZoneFile::SPARSE_WAL_HEADER_SIZE);

    // APPEND-DOC, write to WAL with a zone append, wal_->Sync()
  #ifdef WAL_BARRIERS
    if (is_wal_ && append_bytes_since_last_barrier_ >= WAL_BARRIER_SIZE_IN_KB * KiB) 
    {
      // printf("Synced barrier because %lu >= %lu\n", append_bytes_since_last_barrier_, WAL_BARRIER_SIZE_IN_KB * KiB);
      s = WALSync();
      if (!s.ok()) return s;
      // printf("Synced WAL\n");
      wal_syncs_++;
      append_bytes_since_last_barrier_ = 0;
    }
    wal_writes_++;
#endif

    wr_size = left + header_size;
    if (wr_size > active_zone_->capacity_) wr_size = active_zone_->capacity_;
    // APPEND_LOG, Prevent cross-barrier write
    #ifdef WAL_BARRIERS
    if (is_wal_ && wr_size > (WAL_BARRIER_SIZE_IN_KB * KiB - append_bytes_since_last_barrier_)) 
      wr_size = WAL_BARRIER_SIZE_IN_KB * KiB - append_bytes_since_last_barrier_;
    // printf("Writing size: %u \n", wr_size);
    #endif
    /* Pad to the next block boundary if needed */
    uint32_t align = wr_size % block_sz;
    uint32_t pad_sz = 0;

    if (align) pad_sz = block_sz - align;

    /* the sparse buffer has block_sz extra bytes tail allocated for padding, so
     * this is safe */
    if (pad_sz) memset(sparse_buffer + wr_size, 0x0, pad_sz);

    uint64_t extent_length = wr_size - header_size;
    EncodeFixed64(sparse_buffer, extent_length);
    // printf("Write (%lu %lu %lu %lu\n", wal_seq_.load(std::memory_order_acquire), extent_length, file_size_, 
    //   (wal_->GetWriteHead() - wal_->GetWriteTail()) * 512 );
    EncodeFixed64(sparse_buffer + sizeof(uint64_t), wal_seq_++);

    // APPEND-DOC, write to WAL with a zone append, to a file with a write (Append is write in ZenFS...)
    if (is_wal_) {
      s = active_zone_->ZoneAppend(sparse_buffer, wr_size + pad_sz, wal_);
      if (!s.ok()) return s;
    #ifdef WAL_BARRIERS
      append_bytes_since_last_barrier_ += wr_size + pad_sz;
      // printf("Append before barrier because %lu <= %lu\n", append_bytes_since_last_barrier_, WAL_BARRIER_SIZE_IN_KB * KiB);
    #endif   
    } else {
      s = active_zone_->Append(sparse_buffer, wr_size + pad_sz);
      if (!s.ok()) return s;
    }

    // APPEND-DOC, variable header size
    extents_.push_back(
        new ZoneExtent(extent_start_ + header_size,
                       extent_length, active_zone_));

    extent_start_ = active_zone_->wp_;
    active_zone_->used_capacity_ += extent_length;
    file_size_ += extent_length;
    left -= extent_length;

    if (active_zone_->capacity_ == 0) {
      s = CloseActiveZone();
      if (!s.ok()) {
        return s;
      }
      if (left) {
        // APPEND-DOC, vartiable headersize
        memmove((void*)(sparse_buffer + header_size),
                (void*)(sparse_buffer + wr_size), left);
      }
      s = AllocateNewZone();
      if (!s.ok()) return s;
    }
  }

  return IOStatus::OK();
}

/* Assumes that data and size are block aligned */
IOStatus ZoneFile::Append(void* data, int data_size) {
  uint32_t left = data_size;
  uint32_t wr_size, offset = 0;
  IOStatus s = IOStatus::OK();

  if (!active_zone_) {
    s = AllocateNewZone();
    if (!s.ok()) return s;
  }

  while (left) {
    if (active_zone_->capacity_ == 0) {
      PushExtent();

      s = CloseActiveZone();
      if (!s.ok()) {
        return s;
      }

      s = AllocateNewZone();
      if (!s.ok()) return s;
    }

    wr_size = left;
    if (wr_size > active_zone_->capacity_) wr_size = active_zone_->capacity_;

    s = active_zone_->Append((char*)data + offset, wr_size);
    if (!s.ok()) return s;

    file_size_ += wr_size;
    left -= wr_size;
    offset += wr_size;
  }

  return IOStatus::OK();
}

IOStatus ZoneFile::RecoverSparseExtents(uint64_t start, uint64_t end,
                                        Zone* zone) {
  /* Sparse writes, we need to recover each individual segment */
  IOStatus s;
  uint32_t block_sz = GetBlockSize();
  uint64_t next_extent_start = start;
  char* buffer;
  int recovered_segments = 0;
  int ret;

  // APPEND-DOC, keep seq nr on one thread (no need for load)
  uint64_t wal_seq_rec = wal_seq_.load(std::memory_order_consume);

  ret = posix_memalign((void**)&buffer, sysconf(_SC_PAGESIZE), block_sz);
  if (ret) {
    return IOStatus::IOError("Out of memory while recovering");
  }

  while (next_extent_start < end) {
    // APPEND-DOC, WAL has different header size
    uint64_t header_size = ZoneFile::SPARSE_HEADER_SIZE + 
      (is_wal_ * ZoneFile::SPARSE_WAL_HEADER_SIZE);
    uint64_t extent_length;

    ret = zbd_->Read(buffer, next_extent_start, block_sz, false);
    if (ret != (int)block_sz) {
      s = IOStatus::IOError("Unexpected read error while recovering");
      break;
    }

    extent_length = DecodeFixed64(buffer);
    if (extent_length == 0) {
      s = IOStatus::IOError("Unexpected extent length while recovering");
      break;
    }
    // APPEND-DOC, Decode WAL meta, update sequence number (in tmp var) if needed
    if (ends_with(GetFilename(), ".log")) {
      uint64_t wal_seq_rec_tmp = DecodeFixed64(buffer + sizeof(uint64_t));
      if (wal_seq_rec_tmp > wal_seq_rec) {
        wal_seq_rec = wal_seq_rec_tmp;
      }
    }
    recovered_segments++;

    zone->used_capacity_ += extent_length;
    // APPEND-DOC, different size for WAL and file
    extents_.push_back(new ZoneExtent(next_extent_start + header_size,
                                      extent_length, zone));
    uint64_t extent_blocks = (extent_length + header_size) / block_sz;
    if ((extent_length + header_size) % block_sz) {
      extent_blocks++;
    }
    next_extent_start += extent_blocks * block_sz;
  }

  // APPEND-DOC, update sequence number from tmp var
  // printf("WAL Recovered with sequence number %lu\n", wal_seq_rec);
  wal_seq_.store(wal_seq_rec, std::memory_order_release);

  free(buffer);
  return s;
}

IOStatus ZoneFile::Recover() {
  /* If there is no active extent, the file was either closed gracefully
     or there were no writes prior to a crash. All good.*/
  if (!HasActiveExtent()) return IOStatus::OK();

  /* Figure out which zone we were writing to */
  Zone* zone = zbd_->GetIOZone(extent_start_);

  if (zone == nullptr) {
    return IOStatus::IOError(
        "Could not find zone for extent start while recovering");
  }

  if (zone->wp_ < extent_start_) {
    return IOStatus::IOError("Zone wp is smaller than active extent start");
  }

  /* How much data do we need to recover? */
  uint64_t to_recover = zone->wp_ - extent_start_;

  /* Do we actually have any data to recover? */
  if (to_recover == 0) {
    /* Mark up the file as having no missing extents */
    extent_start_ = NO_EXTENT;
    return IOStatus::OK();
  }

  /* Is the data sparse or was it writted direct? */
  if (is_sparse_) {
    IOStatus s = RecoverSparseExtents(extent_start_, zone->wp_, zone);
    if (!s.ok()) return s;
  } else {
    /* For non-sparse files, the data is contigous and we can recover directly
       any missing data using the WP */
    zone->used_capacity_ += to_recover;
    extents_.push_back(new ZoneExtent(extent_start_, to_recover, zone));
  }

  /* Mark up the file as having no missing extents */
  extent_start_ = NO_EXTENT;

  /* Recalculate file size */
  file_size_ = 0;
  for (uint32_t i = 0; i < extents_.size(); i++) {
    file_size_ += extents_[i]->length_;
  }

  // Retrieve barrier number
  // if (ends_with(linkfiles_[0], ".log")) {
  //}

  return IOStatus::OK();
}

void ZoneFile::ReplaceExtentList(std::vector<ZoneExtent*> new_list) {
  assert(IsOpenForWR() && new_list.size() > 0);
  assert(new_list.size() == extents_.size());

  WriteLock lck(this);
  extents_ = new_list;
}

void ZoneFile::AddLinkName(const std::string& linkf) {
  linkfiles_.push_back(linkf);
}

IOStatus ZoneFile::RenameLink(const std::string& src, const std::string& dest) {
  auto itr = std::find(linkfiles_.begin(), linkfiles_.end(), src);
  if (itr != linkfiles_.end()) {
    linkfiles_.erase(itr);
    linkfiles_.push_back(dest);
  } else {
    return IOStatus::IOError("RenameLink: Failed to find the linked file");
  }
  return IOStatus::OK();
}

IOStatus ZoneFile::RemoveLinkName(const std::string& linkf) {
  assert(GetNrLinks());
  auto itr = std::find(linkfiles_.begin(), linkfiles_.end(), linkf);
  if (itr != linkfiles_.end()) {
    linkfiles_.erase(itr);
  } else {
    return IOStatus::IOError("RemoveLinkInfo: Failed to find the link file");
  }
  return IOStatus::OK();
}

IOStatus ZoneFile::SetWriteLifeTimeHint(Env::WriteLifeTimeHint lifetime) {
  lifetime_ = lifetime;
  return IOStatus::OK();
}

void ZoneFile::ReleaseActiveZone() {
  assert(active_zone_ != nullptr);
  bool ok = active_zone_->Release();
  assert(ok);
  (void)ok;
  active_zone_ = nullptr;
}

void ZoneFile::SetActiveZone(Zone* zone) {
  assert(active_zone_ == nullptr);
  assert(zone->IsBusy());
  active_zone_ = zone;
}

ZonedWritableFile::ZonedWritableFile(ZonedBlockDevice* zbd, bool _buffered,
                                     std::shared_ptr<ZoneFile> zoneFile) {
  assert(zoneFile->IsOpenForWR());
  wp = zoneFile->GetFileSize();

  buffered = _buffered;
  block_sz = zbd->GetBlockSize();
  zoneFile_ = zoneFile;
  buffer_pos = 0;
  sparse_buffer = nullptr;
  buffer = nullptr;

  if (buffered) {
    if (zoneFile->IsSparse()) {
      size_t sparse_buffer_sz;

      // APPEND-DOC, we made the buffersize definable
      sparse_buffer_sz =
          SPARSE_BUFFER_SIZE_IN_KB * KiB + block_sz; /* one extra block size for padding */
      int ret = posix_memalign((void**)&sparse_buffer, sysconf(_SC_PAGESIZE),
                               sparse_buffer_sz);

      if (ret) sparse_buffer = nullptr;

      assert(sparse_buffer != nullptr);

      // APPEND-DOC, WALs need more space
      uint64_t header_size = ZoneFile::SPARSE_HEADER_SIZE + 
      (zoneFile->IsWAL() * ZoneFile::SPARSE_WAL_HEADER_SIZE);

      buffer_sz = sparse_buffer_sz - header_size - block_sz;
      buffer = sparse_buffer + header_size;
    } else {
      buffer_sz = 1024 * 1024;
      int ret =
          posix_memalign((void**)&buffer, sysconf(_SC_PAGESIZE), buffer_sz);

      if (ret) buffer = nullptr;
      assert(buffer != nullptr);
    }
  }

  open = true;
}

ZonedWritableFile::~ZonedWritableFile() {
  IOStatus s = CloseInternal();
  if (buffered) {
    if (sparse_buffer != nullptr) {
      free(sparse_buffer);
    } else {
      free(buffer);
    }
  }

  if (!s.ok()) {
    zoneFile_->GetZbd()->SetZoneDeferredStatus(s);
  }
}

MetadataWriter::~MetadataWriter() {}

IOStatus ZonedWritableFile::Truncate(uint64_t size,
                                     const IOOptions& /*options*/,
                                     IODebugContext* /*dbg*/) {
  zoneFile_->SetFileSize(size);
  return IOStatus::OK();
}

IOStatus ZonedWritableFile::DataSync() {
  // APPEND-DOC, sync on datasync
  if (zoneFile_->IsWAL()) {
    zoneFile_->WALSync();
  }

  if (buffered) {
    IOStatus s;
    buffer_mtx_.lock();
    /* Flushing the buffer will result in a new extent added to the list*/
    s = FlushBuffer();
    buffer_mtx_.unlock();
    if (!s.ok()) {
      return s;
    }

    /* We need to persist the new extent, if the file is not sparse,
     * as we can't use the active zone WP, which is block-aligned, to recover
     * the file size */
    if (!zoneFile_->IsSparse()) return zoneFile_->PersistMetadata();
  } else {
    /* For direct writes, there is no buffer to flush, we just need to push
       an extent for the latest written data */
    zoneFile_->PushExtent();
  }

  return IOStatus::OK();
}

IOStatus ZonedWritableFile::Fsync(const IOOptions& /*options*/,
                                  IODebugContext* /*dbg*/) {
  // APPEND-DOC, Not needed to sync on fsync
  if (zoneFile_->IsWAL()) {
  }

  IOStatus s;
  ZenFSMetricsLatencyGuard guard(zoneFile_->GetZBDMetrics(),
                                 zoneFile_->GetIOType() == IOType::kWAL
                                     ? ZENFS_WAL_SYNC_LATENCY
                                     : ZENFS_NON_WAL_SYNC_LATENCY,
                                 Env::Default());
  zoneFile_->GetZBDMetrics()->ReportQPS(ZENFS_SYNC_QPS, 1);

  s = DataSync();
  if (!s.ok()) return s;

  /* As we've already synced the metadata in DataSync, no need to do it again */
  if (buffered && !zoneFile_->IsSparse()) return IOStatus::OK();

  return zoneFile_->PersistMetadata();
}

IOStatus ZonedWritableFile::Sync(const IOOptions& /*options*/,
                                 IODebugContext* /*dbg*/) {
  return DataSync();
}

IOStatus ZonedWritableFile::Flush(const IOOptions& /*options*/,
                                  IODebugContext* /*dbg*/) {
  return IOStatus::OK();
}

IOStatus ZonedWritableFile::RangeSync(uint64_t offset, uint64_t nbytes,
                                      const IOOptions& /*options*/,
                                      IODebugContext* /*dbg*/) {
  if (wp < offset + nbytes) return DataSync();

  return IOStatus::OK();
}

IOStatus ZonedWritableFile::Close(const IOOptions& /*options*/,
                                  IODebugContext* /*dbg*/) {
  return CloseInternal();
}

IOStatus ZonedWritableFile::CloseInternal() {
  if (!open) {
    return IOStatus::OK();
  }

  IOStatus s = DataSync();
  if (!s.ok()) return s;

  s = zoneFile_->CloseWR();
  if (!s.ok()) return s;

  open = false;
  return s;
}

IOStatus ZonedWritableFile::FlushBuffer() {
  IOStatus s;

  if (buffer_pos == 0) return IOStatus::OK();

  if (zoneFile_->IsSparse()) {
    s = zoneFile_->SparseAppend(sparse_buffer, buffer_pos);
  } else {
    s = zoneFile_->BufferedAppend(buffer, buffer_pos);
  }

  if (!s.ok()) {
    return s;
  }

  wp += buffer_pos;
  buffer_pos = 0;

  return IOStatus::OK();
}

IOStatus ZonedWritableFile::BufferedWrite(const Slice& slice) {
  uint32_t data_left = slice.size();
  char* data = (char*)slice.data();
  IOStatus s;

  while (data_left) {
    uint32_t buffer_left = buffer_sz - buffer_pos;
    uint32_t to_buffer;

    if (!buffer_left) {
      s = FlushBuffer();
      if (!s.ok()) return s;
      buffer_left = buffer_sz;
    } 

    to_buffer = data_left;
    if (to_buffer > buffer_left) {
      to_buffer = buffer_left;
    }

    memcpy(buffer + buffer_pos, data, to_buffer);
    buffer_pos += to_buffer;
    data_left -= to_buffer;
    data += to_buffer;
  }

  return IOStatus::OK();
}

IOStatus ZonedWritableFile::Append(const Slice& data,
                                   const IOOptions& /*options*/,
                                   IODebugContext* /*dbg*/) {
  IOStatus s;
  ZenFSMetricsLatencyGuard guard(zoneFile_->GetZBDMetrics(),
                                 zoneFile_->GetIOType() == IOType::kWAL
                                     ? ZENFS_WAL_WRITE_LATENCY
                                     : ZENFS_NON_WAL_WRITE_LATENCY,
                                 Env::Default());
  zoneFile_->GetZBDMetrics()->ReportQPS(ZENFS_WRITE_QPS, 1);
  zoneFile_->GetZBDMetrics()->ReportThroughput(ZENFS_WRITE_THROUGHPUT,
                                               data.size());

  if (buffered) {
    buffer_mtx_.lock();
    s = BufferedWrite(data);
    buffer_mtx_.unlock();
  } else {
    s = zoneFile_->Append((void*)data.data(), data.size());
    if (s.ok()) wp += data.size();
  }

  return s;
}

IOStatus ZonedWritableFile::PositionedAppend(const Slice& data, uint64_t offset,
                                             const IOOptions& /*options*/,
                                             IODebugContext* /*dbg*/) {
  IOStatus s;
  ZenFSMetricsLatencyGuard guard(zoneFile_->GetZBDMetrics(),
                                 zoneFile_->GetIOType() == IOType::kWAL
                                     ? ZENFS_WAL_WRITE_LATENCY
                                     : ZENFS_NON_WAL_WRITE_LATENCY,
                                 Env::Default());
  zoneFile_->GetZBDMetrics()->ReportQPS(ZENFS_WRITE_QPS, 1);
  zoneFile_->GetZBDMetrics()->ReportThroughput(ZENFS_WRITE_THROUGHPUT,
                                               data.size());

  if (offset != wp) {
    assert(false);
    return IOStatus::IOError("positioned append not at write pointer");
  }

  if (buffered) {
    buffer_mtx_.lock();
    s = BufferedWrite(data);
    buffer_mtx_.unlock();
  } else {
    s = zoneFile_->Append((void*)data.data(), data.size());
    if (s.ok()) wp += data.size();
  }

  return s;
}

void ZonedWritableFile::SetWriteLifeTimeHint(Env::WriteLifeTimeHint hint) {
  zoneFile_->SetWriteLifeTimeHint(hint);
}

IOStatus ZonedSequentialFile::Read(size_t n, const IOOptions& /*options*/,
                                   Slice* result, char* scratch,
                                   IODebugContext* /*dbg*/) {
  IOStatus s;

  s = zoneFile_->PositionedRead(rp, n, result, scratch, direct_);
  if (s.ok()) rp += result->size();

  return s;
}

IOStatus ZonedSequentialFile::Skip(uint64_t n) {
  if (rp + n >= zoneFile_->GetFileSize())
    return IOStatus::InvalidArgument("Skip beyond end of file");
  rp += n;
  return IOStatus::OK();
}

IOStatus ZonedSequentialFile::PositionedRead(uint64_t offset, size_t n,
                                             const IOOptions& /*options*/,
                                             Slice* result, char* scratch,
                                             IODebugContext* /*dbg*/) {
  return zoneFile_->PositionedRead(offset, n, result, scratch, direct_);
}

IOStatus ZonedRandomAccessFile::Read(uint64_t offset, size_t n,
                                     const IOOptions& /*options*/,
                                     Slice* result, char* scratch,
                                     IODebugContext* /*dbg*/) const {
  return zoneFile_->PositionedRead(offset, n, result, scratch, direct_);
}

// APPEND-DOC, method to force sync the WAL
IOStatus ZoneFile::WALSync() {
  if (wal_) {
    zbd_->AppendSync(wal_);
    return wal_->Sync() == SZD::SZDStatus::Success 
      ? IOStatus::OK()
      : IOStatus::IOError("Error WAL sync"); 
  }
  return IOStatus::OK();
}

IOStatus ZoneFile::MigrateData(uint64_t offset, uint32_t length,
                               Zone* target_zone) {
  uint32_t step = 128 << 10;
  uint32_t read_sz = step;
  int block_sz = zbd_->GetBlockSize();

  assert(offset % block_sz == 0);
  if (offset % block_sz != 0) {
    return IOStatus::IOError("MigrateData offset is not aligned!\n");
  }

  char* buf;
  int ret = posix_memalign((void**)&buf, block_sz, step);
  if (ret) {
    return IOStatus::IOError("failed allocating alignment write buffer\n");
  }

  int pad_sz = 0;
  while (length > 0) {
    read_sz = length > read_sz ? read_sz : length;
    pad_sz = read_sz % block_sz == 0 ? 0 : (block_sz - (read_sz % block_sz));

    int r = zbd_->Read(buf, offset, read_sz + pad_sz, true);
    if (r < 0) {
      free(buf);
      return IOStatus::IOError(strerror(errno));
    }
    target_zone->Append(buf, r);
    length -= read_sz;
    offset += r;
  }

  free(buf);

                      //  printf("  Migrate data done\n");
  return IOStatus::OK();
}

}  // namespace ROCKSDB_NAMESPACE

#endif  // !defined(ROCKSDB_LITE) && !defined(OS_WIN)
