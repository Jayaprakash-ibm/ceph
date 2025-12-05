// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_OSD_BLUESTORE_BLUESTORE_OBJECTS_H
#define CEPH_OSD_BLUESTORE_BLUESTORE_OBJECTS_H

#include <atomic>
#include <bit>
#include <mutex>
#include <condition_variable>
#include <memory_resource>
#include <new>

#include "bluestore_types.h"
#include "BlueStore.h"

class BitMapMemoryResource : public std::pmr::memory_resource {
  std::pmr::vector<void*> bases;
  std::pmr::vector<uint8_t> bitmaps;
  std::pmr::vector<int> seg;
  size_t seg_n;

  int slots;
  size_t slot_size;
  size_t chunk_size;
  size_t alignment;
  mempool::pool_index_t pool;

public:
  BitMapMemoryResource(size_t slot_sz,
    int slots,
    size_t align,
    mempool::pool_index_t p)
    : bases(std::pmr::get_default_resource()),
      bitmaps(std::pmr::get_default_resource()),
      seg(std::pmr::get_default_resource()),
      seg_n(0),
      slots(slots),
      slot_size(slot_sz),
      chunk_size(slot_sz * slots),
      alignment(align),
      pool(p)
  {}

  ~BitMapMemoryResource() override {
    for (size_t i = 0; i < bases.size(); ++i) {
      if (!bases[i]) continue;
      ::operator delete(bases[i], std::align_val_t(alignment));
      mempool::get_pool(
        mempool::pool_index_t(mempool::mempool_bluestore_cache_other)).
          adjust_count(-(slots), (int)(chunk_size));
    }
  }

protected:
  void* do_allocate(size_t, size_t) override {
    int idx = find_free();
    if (idx == -1) idx = create_and_insert_chunk();

    uint8_t free_mask = static_cast<uint8_t>(~bitmaps[idx]);
    unsigned b = static_cast<unsigned>(__builtin_ctz(free_mask));
    bitmaps[idx] |= static_cast<uint8_t>(1u << b);
    update_seg(idx);

    return static_cast<char*>(bases[idx]) + b * slot_size;
  }

  void do_deallocate(void* p, size_t, size_t) override {
    if (!p) return;
    int idx = locate_chunk(p);
    if (idx < 0) return;

    char* base = static_cast<char*>(bases[idx]);
    std::ptrdiff_t off_signed = static_cast<char*>(p) - base;
    size_t off = static_cast<size_t>(off_signed);
    int slot = off / slot_size;

    bitmaps[idx] &= static_cast<uint8_t>(~(1u << slot));
    update_seg(idx);

    if (bitmaps[idx] == 0) [[unlikely]] {
      ::operator delete(bases[idx], std::align_val_t(alignment));
      bases.erase(bases.begin() + idx);
      bitmaps.erase(bitmaps.begin() + idx);
      mempool::get_pool(
        mempool::pool_index_t(mempool::mempool_bluestore_cache_other)).
          adjust_count(-slots, -(int)(chunk_size));
      rebuild_seg();
    }
  }

  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }

private:
  int create_and_insert_chunk() {
    void* m = ::operator new(chunk_size, std::align_val_t(alignment));
    if (!m) throw std::bad_alloc();

    uint8_t bm = 0;
    auto it = std::lower_bound(bases.begin(), bases.end(), m);
    size_t pos = it - bases.begin();
    bases.insert(bases.begin() + pos, m);
    bitmaps.insert(bitmaps.begin() + pos, bm);
    mempool::get_pool(
      mempool::pool_index_t(mempool::mempool_bluestore_cache_other)).
        adjust_count((int)slots, (int)(chunk_size));

    rebuild_seg();
    return static_cast<int>(pos);
  }

  int locate_chunk(void* p) {
    if (bases.empty()) return -1;
    auto it = std::lower_bound(bases.begin(), bases.end(), p);
    size_t idx;
    if (it == bases.begin()) idx = 0;
    else if (it == bases.end()) idx = bases.size() - 1;
    else if (*it == p) idx = it - bases.begin();
    else idx = (it - bases.begin()) - 1;

    char* base = static_cast<char*>(bases[idx]);
    if (p < base || static_cast<char*>(p) >= base + chunk_size) return -1;
    return static_cast<int>(idx);
  }

  void rebuild_seg() {
    size_t n = bases.size();
    size_t sz = 1;
    while (sz < n) sz <<= 1;
    seg.assign(2 * sz, 0);
    seg_n = sz;
    for (size_t i = 0; i < n; ++i) {
      seg[sz + i] = static_cast<int>(slots - std::popcount(bitmaps[i]));
    }
    for (size_t i = n; i < sz; ++i) {
      seg[sz + i] = 0;
    }
    for (size_t i = sz; i-- > 1;) {
      seg[i] = seg[i<<1] + seg[(i<<1)|1];
    }
  }

  void update_seg(size_t idx) {
    if (seg_n == 0) { rebuild_seg(); return; }
    size_t p = seg_n + idx;
    seg[p] = static_cast<int>(slots - std::popcount(bitmaps[idx]));
    for (p >>= 1; p >= 1; p >>= 1) {
      seg[p] = seg[p<<1] + seg[(p<<1)|1];
      if (p == 1) break;
    }
  }

  int find_free() {
    if (seg_n == 0 || seg[1] == 0) return -1;
    size_t p = 1;
    while (p < seg_n) {
      if (seg[p<<1] > 0) p = p<<1;
      else p = (p<<1)|1;
    }
    int idx = static_cast<int>(p - seg_n);
    return (idx >= 0 && static_cast<size_t>(idx) < bases.size()) ? idx : -1;
  }
};


namespace bluestore {

  /// in-memory blob metadata and associated cached buffers (if any)
  struct Blob {
    MEMPOOL_CLASS_HELPERS();

    std::atomic_int nref = {0};     ///< reference count
    int16_t id = -1;                ///< id, for spanning blobs only, >= 0
    int16_t last_encoded_id = -1;   ///< (ephemeral) used during encoding only
    bluestore::Onode* onode;

    void set_shared_blob(BlueStore::SharedBlobRef sb);
    Blob(bluestore::Onode* onode) : onode(onode) {}
  private:
    BlueStore::SharedBlobRef shared_blob;      ///< shared blob state (if any)
    mutable bluestore_blob_t blob;  ///< decoded blob metadata
    /// refs from this shard.  ephemeral if id<0, persisted if spanning.
    bluestore_blob_use_tracker_t used_in_blob;

  public:

    friend void intrusive_ptr_add_ref(Blob *b) { b->get(); }
    friend void intrusive_ptr_release(Blob *b) { b->put(); }

    void dump(ceph::Formatter* f) const;
    friend std::ostream& operator<<(std::ostream& out, const Blob &b);
    struct printer : public BlueStore::printer {
      const Blob& blob;
      uint16_t mode;
      printer(const Blob& blob, uint16_t mode)
      :blob(blob), mode(mode) {}
    };
    friend std::ostream& operator<<(std::ostream& out, const printer &p);
    printer print(uint16_t mode) const {
      return printer(*this, mode);
    }
    const bluestore_blob_use_tracker_t& get_blob_use_tracker() const {
      return used_in_blob;
    }
    bluestore_blob_use_tracker_t& dirty_blob_use_tracker() {
      return used_in_blob;
    }

    const BlueStore::SharedBlobRef& get_shared_blob() const {
      return shared_blob;
    }

    BlueStore::SharedBlobRef& get_dirty_shared_blob() {
      return shared_blob;
    }

    bool is_referenced() const {
      return used_in_blob.is_not_empty();
    }
    uint32_t get_referenced_bytes() const {
      return used_in_blob.get_referenced_bytes();
    }

    bool is_spanning() const {
      return id >= 0;
    }

    bool can_split() {
      // splitting a BufferSpace writing list is too hard; don't try.
      return used_in_blob.can_split() &&
             get_blob().can_split();
    }

    bool can_merge_blob(const Blob* other, uint32_t& blob_end) const;
    uint32_t merge_blob(CephContext* cct, Blob* blob_to_dissolve);

    bool can_split_at(uint32_t blob_offset) const {
      return used_in_blob.can_split_at(blob_offset) &&
             get_blob().can_split_at(blob_offset);
    }

    bool can_reuse_blob(uint32_t min_alloc_size,
			uint32_t target_blob_size,
			uint32_t b_offset,
			uint32_t *length0);

    void dup(Blob& o) {
      o.set_shared_blob(shared_blob);
      o.blob = blob;
    }
    void add_tail(uint32_t new_blob_size, uint32_t min_release_size);
    void dup(const Blob& from, bool copy_used_in_blob);
    void copy_from(CephContext* cct, const Blob& from,
		   uint32_t min_release_size, uint32_t start, uint32_t len);
    void copy_extents(CephContext* cct, const Blob& from, uint32_t start,
		      uint32_t pre_len, uint32_t main_len, uint32_t post_len);
    void copy_extents_over_empty(CephContext* cct, const Blob& from, uint32_t start, uint32_t len);

    inline const bluestore_blob_t& get_blob() const {
      return blob;
    }
    inline bluestore_blob_t& dirty_blob() {
      return blob;
    }

    /// get logical references
    void get_ref(BlueStore::Collection *coll, uint32_t offset, uint32_t length);
    /// put logical references, and get back any released extents
    bool put_ref(BlueStore::Collection *coll, uint32_t offset, uint32_t length,
		 PExtentVector *r);
    uint32_t put_ref_accumulate(
      BlueStore::Collection *coll,
      uint32_t offset,
      uint32_t length,
      PExtentVector *released_disk);
    /// split the blob
    void split(BlueStore::Collection *coll, uint32_t blob_offset, Blob *o);

    void maybe_prune_tail();

    void get() {
      ++nref;
    }
    void put();
    bool is_shared_loaded() const;
    BlueStore::BufferCacheShard* get_cache();
    uint64_t get_sbid() const;

    ~Blob();

    void bound_encode(
      size_t& p,
      uint64_t struct_v,
      uint64_t sbid,
      bool include_ref_map) const {
      denc(blob, p, struct_v);
      if (blob.is_shared()) {
        denc(sbid, p);
      }
      if (include_ref_map) {
	used_in_blob.bound_encode(p);
      }
    }
    void encode(
      ceph::buffer::list::contiguous_appender& p,
      uint64_t struct_v,
      uint64_t sbid,
      bool include_ref_map) const {
      denc(blob, p, struct_v);
      if (blob.is_shared()) {
        denc(sbid, p);
      }
      if (include_ref_map) {
	used_in_blob.encode(p);
      }
    }
    void decode(
      ceph::buffer::ptr::const_iterator& p,
      uint64_t struct_v,
      uint64_t* sbid,
      bool include_ref_map,
      BlueStore::Collection *coll);
  };

  /// an in-memory object
  struct Onode {
    MEMPOOL_CLASS_HELPERS();

    std::atomic_int nref = 0;      ///< reference count
    std::atomic_int pin_nref = 0;  ///< reference count replica to track pinning
    BlueStore::Collection *c;
    ghobject_t oid;

    /// key under PREFIX_OBJ where we are stored
    mempool::bluestore_cache_meta::string key;

    boost::intrusive::list_member_hook<> lru_item;

    bluestore_onode_t onode;  ///< metadata stored as value in kv store
    bool exists;              ///< true if object logically exists
    bool cached;              ///< Onode is logically in the cache
                              /// (it can be pinned and hence physically out
                              /// of it at the moment though)
    uint16_t prev_spanning_cnt = 0; /// spanning blobs count
    BlueStore::BufferSpace bc;             ///< buffer cache

    // track txc's that have not been committed to kv store (and whose
    // effects cannot be read via the kvdb read methods)
    std::atomic<int> flushing_count = {0};
    std::atomic<int> waiting_count = {0};
    /// protect flush_txns
    ceph::mutex flush_lock = ceph::make_mutex("BlueStore::Onode::flush_lock");
    ceph::condition_variable flush_cond;   ///< wait here for uncommitted txns
    std::shared_ptr<int64_t> cache_age_bin;  ///< cache age bin

    BitMapMemoryResource extent_mem_resource;
    std::pmr::polymorphic_allocator<BlueStore::Extent> LocalExtentAllocator;
    bool fast_deletion = false;
    BlueStore::ExtentMap extent_map;

    Onode(BlueStore::Collection *c, const ghobject_t& o,
	  const mempool::bluestore_cache_meta::string& k);
    Onode(CephContext* cct);

    ~Onode();

    static void decode_raw(
      BlueStore::Onode* on,
      const bufferlist& v,
      BlueStore::ExtentMap::ExtentDecoder& dencoder,
      bool use_onode_segmentation);

    static Onode* create_decode(
      BlueStore::CollectionRef c,
      const ghobject_t& oid,
      const std::string& key,
      const ceph::buffer::list& v,
      bool allow_empty,
      bool use_onode_segmentation);

    void dump(ceph::Formatter* f) const;

    void flush();
    void get();
    void put();

    inline bool is_cached() const {
      return cached;
    }
    inline void set_cached() {
      ceph_assert(!cached);
      cached = true;
    }
    inline void clear_cached() {
      ceph_assert(cached);
      cached = false;
    }

    BlueStore::BlobRef new_blob();

    inline BlueStore::Extent* get_new_extent() {
      BlueStore::Extent* ne = LocalExtentAllocator.allocate(1);
      std::construct_at(ne);
      return ne;
    }

    inline BlueStore::Extent* get_new_extent(uint32_t lo) {
      BlueStore::Extent* ne = LocalExtentAllocator.allocate(1);
      std::construct_at(ne, lo);
      return ne;
    }
    
    inline BlueStore::Extent* get_new_extent(uint32_t lo,
      uint32_t o,
      uint32_t l,
      BlueStore::BlobRef& b)
    {
      BlueStore::Extent* ne = LocalExtentAllocator.allocate(1);
      std::construct_at(ne, lo, o, l, b);
      return ne;
    }

    static const std::string& calc_omap_prefix(uint8_t flags);
    static void calc_omap_header(uint8_t flags, const Onode* o,
      std::string* out);
    static void calc_omap_key(uint8_t flags, const Onode* o,
      const std::string& key, std::string* out);
    static void calc_omap_tail(uint8_t flags, const Onode* o,
      std::string* out);

    const std::string& get_omap_prefix() {
      return calc_omap_prefix(onode.flags);
    }
    void get_omap_header(std::string* out) {
      calc_omap_header(onode.flags, this, out);
    }
    void get_omap_key(const std::string& key, std::string* out) {
      calc_omap_key(onode.flags, this, key, out);
    }
    void get_omap_tail(std::string* out) {
      calc_omap_tail(onode.flags, this, out);
    }

    void rewrite_omap_key(const std::string& old, std::string *out);
    size_t calc_userkey_offset_in_omap_key() const;
    void decode_omap_key(const std::string& key, std::string *user_key);

    void finish_write(BlueStore::TransContext* txc, uint32_t offset, uint32_t length);

    struct printer : public BlueStore::printer {
      const Onode &onode;
      uint16_t mode;
      uint32_t from = 0;
      uint32_t end = BlueStore::OBJECT_MAX_SIZE;
      printer(const Onode &onode, uint16_t mode) : onode(onode), mode(mode) {}
      printer(const Onode &onode, uint16_t mode, uint32_t from, uint32_t end)
          : onode(onode), mode(mode), from(from), end(end) {}
    };
    friend std::ostream &operator<<(std::ostream &out, const printer &p);
    printer print(uint16_t mode) const { return printer(*this, mode); }
    printer print(uint16_t mode, uint32_t from, uint32_t end) const {
      return printer(*this, mode, from, end);
    }
  };

  static inline void intrusive_ptr_add_ref(bluestore::Onode *o) {
    o->get();
  }
  static inline void intrusive_ptr_release(bluestore::Onode *o) {
    o->put();
  }  

}

#endif
