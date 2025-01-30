// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "Allocator.h"
#include <bit>
#include "StupidAllocator.h"
#include "BitmapAllocator.h"
#include "AvlAllocator.h"
#include "BtreeAllocator.h"
#include "Btree2Allocator.h"
#include "HybridAllocator.h"
#include "common/debug.h"
#include "common/admin_socket.h"
#include "AllocatorBase.h"

#define dout_subsys ceph_subsys_bluestore
using TOPNSPC::common::cmd_getval;

using std::string;
using std::to_string;

using ceph::bufferlist;
using ceph::Formatter;

AllocatorBase::AllocatorBase(std::string_view name,
                     int64_t _capacity,
                     int64_t _block_size)
  : Allocator(name, _capacity, _block_size)
{}

AllocatorBase::~AllocatorBase()
{}

/*************
* AllocatorBase::FreeStateHistogram
*************/
using std::function;

void AllocatorBase::FreeStateHistogram::record_extent(uint64_t alloc_unit,
                                                  uint64_t off,
                                                  uint64_t len)
{
  size_t idx = myTraits._get_bucket(len);
  ceph_assert(idx < buckets.size());
  ++buckets[idx].total;

  // now calculate the bucket for the chunk after alignment,
  // resulting chunks shorter than alloc_unit are discarded
  auto delta = p2roundup(off, alloc_unit) - off;
  if (len >= delta + alloc_unit) {
    len -= delta;
    idx = myTraits._get_bucket(len);
    ceph_assert(idx < buckets.size());
    ++buckets[idx].aligned;
    buckets[idx].alloc_units += len / alloc_unit;
  }
}
void AllocatorBase::FreeStateHistogram::foreach(
  function<void(uint64_t max_len,
                uint64_t total,
                uint64_t aligned,
                uint64_t unit)> cb)
{
  size_t i = 0;
  for (const auto& b : buckets) {
    cb(myTraits._get_bucket_max(i),
      b.total, b.aligned, b.alloc_units);
    ++i;
  }
}

