/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_MEMORY_H_
#define XENIA_MEMORY_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/mmio_handler.h"
#include "xenia/guest_pointers.h"
#include "xenia/memory_coherence.h"
namespace xe {
class ByteStream;
}  // namespace xe

namespace xe {

class Memory;

enum SystemHeapFlag : uint32_t {
  kSystemHeapVirtual = 1 << 0,
  kSystemHeapPhysical = 1 << 1,

  kSystemHeapDefault = kSystemHeapVirtual,
};

enum class HeapType : uint8_t {
  kGuestVirtual,
  kGuestXex,
  kGuestPhysical,
  kHostPhysical,
};

enum MemoryAllocationFlag : uint32_t {
  kMemoryAllocationReserve = 1 << 0,
  kMemoryAllocationCommit = 1 << 1,
};

enum MemoryProtectFlag : uint32_t {
  kMemoryProtectRead = 1 << 0,
  kMemoryProtectWrite = 1 << 1,
  kMemoryProtectNoCache = 1 << 2,
  kMemoryProtectWriteCombine = 1 << 3,

  kMemoryProtectNoAccess = 0,
};

// Write-combine memory is CPU-writable for GPU uploads, so treat it as writable
// alongside the regular write flag.
inline bool IsWritableProtect(uint32_t protect) {
  return (protect & kMemoryProtectWrite) ||
         (protect & kMemoryProtectWriteCombine);
}

inline xe::memory::PageAccess ToPageAccess(uint32_t protect) {
  bool is_writable = IsWritableProtect(protect);

  if ((protect & kMemoryProtectRead) && !is_writable) {
    return xe::memory::PageAccess::kReadOnly;
  } else if ((protect & kMemoryProtectRead) && is_writable) {
    return xe::memory::PageAccess::kReadWrite;
  } else {
    return xe::memory::PageAccess::kNoAccess;
  }
}

// Equivalent to the Win32 MEMORY_BASIC_INFORMATION struct.
struct HeapAllocationInfo {
  // A pointer to the base address of the region of pages.
  uint32_t base_address;
  // A pointer to the base address of a range of pages allocated by the
  // VirtualAlloc function. The page pointed to by the BaseAddress member is
  // contained within this allocation range.
  uint32_t allocation_base;
  // The memory protection option when the region was initially allocated.
  uint32_t allocation_protect;
  // The size specified when the region was initially allocated, in bytes.
  uint32_t allocation_size;
  // The size of the region beginning at the base address in which all pages
  // have identical attributes, in bytes.
  uint32_t region_size;
  // The state of the pages in the region (commit/free/reserve).
  uint32_t state;
  // The access protection of the pages in the region.
  uint32_t protect;
};

// Describes a single page in the page table.
union PageEntry {
  uint64_t qword;
  struct {
    // Base address of the allocated region in 4k pages.
    uint32_t base_address : 20;
    // Total number of pages in the allocated region in 4k pages.
    uint32_t region_page_count : 20;
    // Protection bits specified during region allocation.
    // Composed of bits from MemoryProtectFlag.
    uint32_t allocation_protect : 4;
    // Current protection bits as of the last Protect.
    // Composed of bits from MemoryProtectFlag.
    uint32_t current_protect : 4;
    // Allocation state of the page as a MemoryAllocationFlag bit mask.
    uint32_t state : 2;
    uint32_t reserved : 14;
  };
};

// Heap abstraction for page-based allocation.
class BaseHeap {
 public:
  virtual ~BaseHeap();

  // Offset of the heap in relative to membase, without host_address_offset
  // adjustment.
  uint32_t heap_base() const { return heap_base_; }

  // Length of the heap range.
  uint32_t heap_size() const { return heap_size_; }

  // Size of each page within the heap range in bytes.
  uint32_t page_size() const { return page_size_; }

  // Amount of pages assigned to heap
  uint32_t total_page_count() const { return uint32_t(page_table_.size()); }

  // Sum of unreserved pages in heap
  uint32_t unreserved_page_count() const { return unreserved_page_count_; }

  // Sum of reserved pages in heap
  uint32_t reserved_page_count() const {
    return total_page_count() - unreserved_page_count();
  }

  // Type of specified heap
  HeapType heap_type() const { return heap_type_; }

  // Offset added to the virtual addresses to convert them to host addresses
  // (not including membase).
  uint32_t host_address_offset() const { return host_address_offset_; }

  template <typename T = uint8_t*>
  inline T TranslateRelative(size_t relative_address) const {
    return reinterpret_cast<T>(membase_ + heap_base_ + host_address_offset_ +
                               relative_address);
  }

  // Disposes and decommits all memory and clears the page table.
  virtual void Dispose();

  // Dumps information about all allocations within the heap to the log.
  void DumpMap();

  // Allocates pages with the given properties and allocation strategy.
  // This can reserve and commit the pages as well as set protection modes.
  // This will fail if not enough contiguous pages can be found.
  virtual bool Alloc(uint32_t size, uint32_t alignment,
                     uint32_t allocation_type, uint32_t protect, bool top_down,
                     uint32_t* out_address);

  // Allocates pages at the given address.
  // This can reserve and commit the pages as well as set protection modes.
  // This will fail if the pages are already allocated.
  virtual bool AllocFixed(uint32_t base_address, uint32_t size,
                          uint32_t alignment, uint32_t allocation_type,
                          uint32_t protect);

  // Allocates pages at an address within the given address range.
  // This can reserve and commit the pages as well as set protection modes.
  // This will fail if not enough contiguous pages can be found.
  virtual bool AllocRange(uint32_t low_address, uint32_t high_address,
                          uint32_t size, uint32_t alignment,
                          uint32_t allocation_type, uint32_t protect,
                          bool top_down, uint32_t* out_address);

  virtual bool AllocSystemHeap(uint32_t size, uint32_t alignment,
                               uint32_t allocation_type, uint32_t protect,
                               bool top_down, uint32_t* out_address);
  // Decommits pages in the given range.
  // Partial overlapping pages will also be decommitted.
  virtual bool Decommit(uint32_t address, uint32_t size);

  // Decommits and releases pages in the given range.
  // Partial overlapping pages will also be released.
  virtual bool Release(uint32_t address, uint32_t* out_region_size = nullptr);

  // Modifies the protection mode of pages within the given range.
  virtual bool Protect(uint32_t address, uint32_t size, uint32_t protect,
                       uint32_t* old_protect = nullptr);

  // Queries information about the given region of pages.
  bool QueryRegionInfo(uint32_t base_address, HeapAllocationInfo* out_info);

  // Queries the size of the region containing the given address.
  bool QuerySize(uint32_t address, uint32_t* out_size);

  // Queries the base and size of a region containing the given address.
  bool QueryBaseAndSize(uint32_t* in_out_address, uint32_t* out_size);

  // Queries the current protection mode of the region containing the given
  // address.
  bool QueryProtect(uint32_t address, uint32_t* out_protect);

  // Queries the currently strictest readability and writability for the entire
  // range.
  xe::memory::PageAccess QueryRangeAccess(uint32_t low_address,
                                          uint32_t high_address);

  bool Save(ByteStream* stream);
  bool Restore(ByteStream* stream);

  void Reset();

 protected:
  BaseHeap();

  void Initialize(Memory* memory, uint8_t* membase, HeapType heap_type,
                  uint32_t heap_base, uint32_t heap_size, uint32_t page_size,
                  uint32_t host_address_offset = 0);

  // Rebuilds free_blocks_ by scanning page_table_. Used after Restore.
  void RebuildFreeBlocks();

  // Removes (or splits) the free block covering the given page range.
  void RemoveFreeBlock(uint32_t start_page, uint32_t page_count);

  // Inserts a free block and coalesces with adjacent free blocks.
  void InsertFreeBlock(uint32_t start_page, uint32_t page_count);

  Memory* memory_;
  uint8_t* membase_;
  HeapType heap_type_;
  uint32_t heap_base_;
  uint32_t heap_size_;
  uint32_t page_size_;
  uint32_t page_size_shift_;
  uint32_t host_address_offset_;
  uint32_t unreserved_page_count_;
  xe::global_critical_region global_critical_region_;
  std::vector<PageEntry> page_table_;

  // Auxiliary free block tracker: maps start_page -> count of contiguous free
  // pages. Kept in sync with page_table_ mutations. Not serialized.
  std::map<uint32_t, uint32_t> free_blocks_;
};

// Normal heap allowing allocations from guest virtual address ranges.
class VirtualHeap : public BaseHeap {
 public:
  VirtualHeap();
  ~VirtualHeap() override;

  // Initializes the heap properties and allocates the page table.
  void Initialize(Memory* memory, uint8_t* membase, HeapType heap_type,
                  uint32_t heap_base, uint32_t heap_size, uint32_t page_size);
};

// Where data the GPU wrote waits until the CPU side touches it.
class GpuPullSource {
 public:
  virtual ~GpuPullSource() = default;
  // Blocks until the GPU passed the fence. Any thread. Returns early if the
  // device is lost or shut down.
  virtual void AwaitFence(coherence::Fence fence) = 0;
  // nullptr on failure.
  virtual const uint8_t* MapForRead(uint64_t buffer) = 0;
  virtual void Unmap(uint64_t buffer) = 0;
};

// A heap for ranges of memory that are mapped to physical ranges.
// Physical ranges are used by the audio and graphics subsystems representing
// hardware wired directly to memory in the console.
//
// The physical heap and the behavior of sharing pages with virtual pages is
// implemented by having a 'parent' heap that is used to perform allocation in
// the guest virtual address space 1:1 with the physical address space.
class PhysicalHeap : public BaseHeap {
 public:
  PhysicalHeap();
  ~PhysicalHeap() override;

  // Initializes the heap properties and allocates the page table.
  void Initialize(Memory* memory, uint8_t* membase, HeapType heap_type,
                  uint32_t heap_base, uint32_t heap_size, uint32_t page_size,
                  VirtualHeap* parent_heap);

  bool Alloc(uint32_t size, uint32_t alignment, uint32_t allocation_type,
             uint32_t protect, bool top_down, uint32_t* out_address) override;
  bool AllocFixed(uint32_t base_address, uint32_t size, uint32_t alignment,
                  uint32_t allocation_type, uint32_t protect) override;
  bool AllocRange(uint32_t low_address, uint32_t high_address, uint32_t size,
                  uint32_t alignment, uint32_t allocation_type,
                  uint32_t protect, bool top_down,
                  uint32_t* out_address) override;
  bool AllocSystemHeap(uint32_t size, uint32_t alignment,
                       uint32_t allocation_type, uint32_t protect,
                       bool top_down, uint32_t* out_address) override;
  bool Decommit(uint32_t address, uint32_t size) override;
  bool Release(uint32_t base_address,
               uint32_t* out_region_size = nullptr) override;
  bool Protect(uint32_t address, uint32_t size, uint32_t protect,
               uint32_t* old_protect = nullptr) override;

  void EnableAccessCallbacks(uint32_t physical_address, uint32_t length,
                             bool enable_invalidation_notifications,
                             bool enable_data_providers);
  template <bool enable_invalidation_notifications, bool enable_data_providers>
  XE_NOINLINE void EnableAccessCallbacksInner(const uint32_t system_page_first,
                                              const uint32_t system_page_last)
      XE_RESTRICT;

  // Returns true if any page in the range was watched. With
  // invalidate_unwatched the callbacks are raised even when no watch is armed
  // for a caller that knows the range is about to change rather than one
  // reacting to a fault.
  bool TriggerCallbacks(global_unique_lock_type global_lock_locked_once,
                        uint32_t virtual_address, uint32_t length,
                        bool is_write, bool unwatch_exact_range,
                        bool unprotect = true,
                        bool invalidate_unwatched = false);

  uint32_t GetPhysicalAddress(uint32_t address) const;

  // Sets the host protection of system pages from their guest protection,
  // watches and GPU ownership.
  void UpdateHostProtection(uint32_t system_page_first,
                            uint32_t system_page_last);
  // The same for the system pages of a physical range in this view.
  void UpdateHostProtectionPhysical(uint32_t physical_address, uint32_t length);

  uint32_t SystemPagenumToGuestPagenum(uint32_t num) const {
    return ((num << system_page_shift_) - host_address_offset()) >>
           page_size_shift_;
  }

  // Whether any guest page in the system page is committed in this view. The
  // rest of physical memory is mapped read-write on the host.
  bool SystemPageGuestCommitted(uint32_t system_page_number) const {
    uint32_t offset = host_address_offset();
    uint32_t system_base = system_page_number << system_page_shift_;
    uint32_t system_last = system_base + (system_page_size_ - 1);
    if (system_last < offset) {
      return false;
    }
    uint32_t first =
        system_base > offset ? (system_base - offset) >> page_size_shift_ : 0;
    uint32_t last = (system_last - offset) >> page_size_shift_;
    for (uint32_t i = first; i <= last && i < page_table_.size(); ++i) {
      if (page_table_[i].state & kMemoryAllocationCommit) {
        return true;
      }
    }
    return false;
  }
  uint32_t SystemPagePhysicalAddress(uint32_t system_page_number) const {
    return xe::sat_sub(system_page_number << system_page_shift_,
                       host_address_offset()) +
           GetPhysicalAddress(heap_base_);
  }

  // The most permissive guest access of the guest pages a system page covers.
  // Protection has system page granularity and BaseHeap::Protect resolves a
  // system page the same way, so anything deciding on protection has to agree
  // with it - the host page can be larger than the guest page. Inline, called
  // per page in the arming loop.
  xe::memory::PageAccess SystemPageGuestAccess(
      uint32_t system_page_number) const {
    uint32_t offset = host_address_offset();
    uint32_t system_base = system_page_number << system_page_shift_;
    uint32_t system_last = system_base + (system_page_size_ - 1);
    if (system_last < offset) {
      return xe::memory::PageAccess::kNoAccess;
    }
    uint32_t guest_page_first =
        system_base > offset ? (system_base - offset) >> page_size_shift_ : 0;
    uint32_t guest_page_count = uint32_t(page_table_.size());
    if (guest_page_first >= guest_page_count) {
      return xe::memory::PageAccess::kNoAccess;
    }
    uint32_t guest_page_last = (system_last - offset) >> page_size_shift_;
    if (guest_page_last >= guest_page_count) {
      guest_page_last = guest_page_count - 1;
    }
    xe::memory::PageAccess access = xe::memory::PageAccess::kNoAccess;
    for (uint32_t i = guest_page_first; i <= guest_page_last; ++i) {
      xe::memory::PageAccess page_access =
          ToPageAccess(page_table_[i].current_protect);
      if (page_access == xe::memory::PageAccess::kReadWrite) {
        return xe::memory::PageAccess::kReadWrite;
      }
      if (page_access == xe::memory::PageAccess::kReadOnly) {
        access = xe::memory::PageAccess::kReadOnly;
      }
    }
    return access;
  }

 protected:
  VirtualHeap* parent_heap_;

  uint32_t system_page_size_;
  uint32_t system_page_count_;
  uint32_t system_page_shift_;
  uint32_t padding1_;

  struct SystemPageFlagsBlock {
    // Whether writing to each page should result trigger invalidation
    // callbacks.
    uint64_t notify_on_invalidation = 0;
    // Whether the first access of each page triggers read callbacks. These
    // pages are protected no-access. The watch is one-shot, cleared on access.
    uint64_t notify_on_read = 0;
  };
  // Protected by global_critical_region. Flags for each 64 system pages,
  // interleaved as blocks, so bit scan can be used to quickly extract ranges.
  std::vector<SystemPageFlagsBlock> system_page_flags_;
};

// Models the entire guest memory system on the console.
// This exposes interfaces to both virtual and physical memory and a TLB and
// page table for allocation, mapping, and protection.
//
// The memory is backed by a memory mapped file and is placed at a stable
// fixed address in the host address space (like 0x100000000). This allows
// efficient guest<->host address translations as well as easy sharing of the
// memory across various subsystems.
//
// The guest memory address space is split into several ranges that have varying
// properties such as page sizes, caching strategies, protections, and
// overlap with other ranges. Each range is represented by a BaseHeap of either
// VirtualHeap or PhysicalHeap depending on type. Heaps model the page tables
// and can handle reservation and committing of requested pages.
class Memory {
 public:
  Memory();
  ~Memory();

  // Initializes the memory system.
  // This may fail if the host address space could not be reserved or the
  // mapping to the file system fails.
  bool Initialize();

  // Resets all memory to zero and resets all allocations.
  void Reset();

  // Full file name and path of the memory-mapped file backing all memory.
  const std::filesystem::path& file_name() const { return file_name_; }

  // Base address of virtual memory in the host address space.
  // This is often something like 0x100000000.
  inline uint8_t* virtual_membase() const { return virtual_membase_; }

  // Translates a guest virtual address to a host address that can be accessed
  // as a normal pointer.
  // Note that the contents at the specified host address are big-endian.
  template <typename T = uint8_t*>
  inline T TranslateVirtual(uint32_t guest_address) const {
#if XE_PLATFORM_WIN32 == 1
    uint8_t* host_address = virtual_membase_ + guest_address;
    if (guest_address >= 0xE0000000) {
      host_address += 0x1000;
    }
    return reinterpret_cast<T>(host_address);
#else
    uint8_t* host_address = virtual_membase_ + guest_address;
    const auto heap = LookupHeap(guest_address);
    if (heap) {
      host_address += heap->host_address_offset();
    }
    return reinterpret_cast<T>(host_address);

#endif
  }
  template <typename T>
  inline T* TranslateVirtual(TypedGuestPointer<T> guest_address) {
    return TranslateVirtual<T*>(guest_address.m_ptr);
  }
  template <typename T>
  inline xe::be<T>* TranslateVirtualBE(uint32_t guest_address)
      XE_RESTRICT const {
    static_assert(!std::is_pointer_v<T> &&
                  sizeof(T) > 1);  // maybe assert is_integral?
    return TranslateVirtual<xe::be<T>*>(guest_address);
  }

  // Base address of physical memory in the host address space.
  // This is often something like 0x200000000.
  inline uint8_t* physical_membase() const { return physical_membase_; }

  // Translates a guest physical address to a host address that can be accessed
  // as a normal pointer.
  // Note that the contents at the specified host address are big-endian.
  template <typename T = uint8_t*>
  inline T TranslatePhysical(uint32_t guest_address) const {
    return reinterpret_cast<T>(physical_membase_ +
                               (guest_address & 0x1FFFFFFF));
  }

  // Translates a host address to a guest virtual address.
  // Note that the contents at the returned host address are big-endian.
  uint32_t HostToGuestVirtual(const void* host_address) const;

  // Returns the guest physical address for the guest virtual address, or
  // UINT32_MAX if it can't be obtained.
  uint32_t GetPhysicalAddress(uint32_t address) const;

  // Zeros out a range of memory at the given guest address.
  void Zero(uint32_t address, uint32_t size);

  // Fills a range of guest memory with the given byte value.
  void Fill(uint32_t address, uint32_t size, uint8_t value);

  // Copies a non-overlapping range of guest memory (like a memcpy).
  void Copy(uint32_t dest, uint32_t src, uint32_t size);

  // Searches the given range of guest memory for a run of dword values in
  // big-endian order.
  uint32_t SearchAligned(uint32_t start, uint32_t end, const uint32_t* values,
                         size_t value_count);

  // Defines a memory-mapped IO (MMIO) virtual address range that when accessed
  // will trigger the specified read and write callbacks for dword read/writes.
  bool AddVirtualMappedRange(uint32_t virtual_address, uint32_t mask,
                             uint32_t size, void* context,
                             cpu::MMIOReadCallback read_callback,
                             cpu::MMIOWriteCallback write_callback);

  // Gets the defined MMIO range for the given virtual address, if any.
  cpu::MMIORange* LookupVirtualMappedRange(uint32_t virtual_address);

  // Physical memory access callbacks, two types of them.
  //
  // This is simple per-system-page protection without reference counting or
  // stored ranges. Whenever a watched page is accessed, all callbacks for it
  // are triggered. Also the only way to remove callbacks is to trigger them
  // somehow. Since there are no references from pages to individual callbacks,
  // there's no way to disable only a specific callback for a page. Also
  // callbacks may be triggered spuriously, and handlers should properly ignore
  // pages they don't care about.
  //
  // Once callbacks are triggered for a page, the page is not watched anymore
  // until requested again later. It is, however, unwatched only in one guest
  // view of physical memory (because different views may have different
  // protection for the same memory) - but it's rare when the same memory is
  // used with different guest page sizes, and it's okay to fire a callback more
  // than once.
  //
  // Only accessing the guest virtual memory views of physical memory triggers
  // callbacks - data providers, for instance, must write to the host physical
  // heap directly, otherwise their threads may infinitely await themselves.
  //
  // - Invalidation notifications:
  //
  // Protecting from writing. One-shot callbacks for invalidation of various
  // kinds of physical memory caches (such as the GPU copy of the memory).
  //
  // May be triggered for a single page (in case of a write access violation or
  // when need to synchronize data given by data providers) or for multiple
  // pages (like when memory is released, or explicitly to trigger callbacks
  // when host-side code can't rely on regular access violations, like when
  // accessing a file).
  //
  // Since granularity of callbacks is one single page, an invalidation
  // notification handler must invalidate the all the data stored in the touched
  // pages.
  //
  // Because large ranges (like whole framebuffers) may be written to and
  // exceptions are expensive, it's better to unprotect multiple pages as a
  // result of a write access violation, so the shortest common range returned
  // by all the invalidation callbacks (clamped to a sane range and also not to
  // touch pages with provider callbacks) is unprotected.
  //
  // - Data providers:
  //
  // TODO(Triang3l): Implement data providers - more complicated because they
  // will need to be able to release the global lock.

  // Returns start and length of the smallest physical memory region surrounding
  // the watched region that can be safely unwatched, if it doesn't matter,
  // return (0, UINT32_MAX).
  typedef std::pair<uint32_t, uint32_t> (*PhysicalMemoryInvalidationCallback)(
      void* context_ptr, uint32_t physical_address_start, uint32_t length,
      bool exact_range);
  // Returns a handle for unregistering or for skipping one notification handler
  // while triggering data providers.
  void* RegisterPhysicalMemoryInvalidationCallback(
      PhysicalMemoryInvalidationCallback callback, void* callback_context);
  // Unregisters a physical memory invalidation callback previously added with
  // RegisterPhysicalMemoryInvalidationCallback.
  void UnregisterPhysicalMemoryInvalidationCallback(void* callback_handle);

  // Called on the first CPU access of a page armed as a read watch (via
  // EnablePhysicalMemoryAccessCallbacks with data providers). The page is
  // downgraded and unwatched right after, so it fires once per arm. Must be
  // lightweight and non-blocking. It runs in the fault handler under the global
  // critical region.
  typedef void (*PhysicalMemoryReadCallback)(void* context_ptr,
                                             uint32_t physical_address_start,
                                             uint32_t length);
  void* RegisterPhysicalMemoryReadCallback(PhysicalMemoryReadCallback callback,
                                           void* callback_context);
  void UnregisterPhysicalMemoryReadCallback(void* callback_handle);

  // Enables physical memory access callbacks for the specified memory range,
  // snapped to system page boundaries.
  void EnablePhysicalMemoryAccessCallbacks(
      uint32_t physical_address, uint32_t length,
      bool enable_invalidation_notifications, bool enable_data_providers);

  // Forces triggering of watch callbacks for a virtual address range if pages
  // are watched there and unwatching them. Returns whether any page was
  // watched. Must be called with global critical region locking depth of 1.
  // The invalidation and read callbacks run under that single lock hold.
  bool TriggerPhysicalMemoryCallbacks(
      global_unique_lock_type global_lock_locked_once, uint32_t virtual_address,
      uint32_t length, bool is_write, bool unwatch_exact_range,
      bool unprotect = true);

  // Host code (the GPU command processor, the kernel, the APU) declares its
  // accesses to guest physical memory instead of relying on page faults.
  void PrepareHostRead(uint32_t physical_address, uint32_t length);
  void PrepareHostWrite(uint32_t physical_address, uint32_t length);
  // Notifies the invalidation callbacks of a range host code wrote.
  void FinishHostWrite(uint32_t physical_address, uint32_t length);
  // For host APIs that bypass the page fault handler, such as system calls:
  // a guest buffer within one physical view is prepared and translated through
  // the physical mapping, which is never protected. Other buffers are
  // translated virtually, and a null guest address gives nullptr.
  uint8_t* PrepareHostBuffer(uint32_t guest_address, uint32_t length,
                             bool is_write);
  void FinishHostBufferWrite(uint32_t guest_address, uint32_t length);

  // Data the GPU wrote, pulled into guest memory on the first CPU-side access.
  // Without a source, pending data is dropped. Returns once no pull waits on
  // the old source, which the GPU must let finish.
  void SetGpuPullSource(GpuPullSource* source);
  // Registers a GPU write and protects its pages in every view.
  void RecordGpuWrite(const coherence::GpuWrite& write);
  // Copies what is still pending from a buffer before it's reused or freed.
  void FlushGpuBuffer(uint64_t buffer);
  coherence::Stats coherence_stats() const;
  // Called with every range host code or a pull wrote, so the CPU backend can
  // break lwarx/stwcx. reservations on it, as any write does on hardware.
  using ReservationInvalidator = void (*)(void* context,
                                          uint32_t physical_address,
                                          uint32_t length);
  void SetReservationInvalidator(ReservationInvalidator invalidator,
                                 void* context);

  // Allocates virtual memory from the 'system' heap.
  // System memory is kept separate from game memory but is still accessible
  // using normal guest virtual addresses. Kernel structures and other internal
  // 'system' allocations should come from this heap when possible.
  uint32_t SystemHeapAlloc(uint32_t size, uint32_t alignment = 0x20,
                           uint32_t system_heap_flags = kSystemHeapDefault);

  // Frees memory allocated with SystemHeapAlloc.
  void SystemHeapFree(uint32_t address, uint32_t* out_region_size = nullptr);

  // Gets the heap for the address space containing the given address.
  XE_NOALIAS
  const BaseHeap* LookupHeap(uint32_t address) const;
  XE_NOALIAS
  inline BaseHeap* LookupHeap(uint32_t address) {
    return const_cast<BaseHeap*>(
        const_cast<const Memory*>(this)->LookupHeap(address));
  }

  // Gets the heap with the given properties.
  BaseHeap* LookupHeapByType(bool physical, uint32_t page_size);

  // Gets the physical base heap.
  VirtualHeap* GetPhysicalHeap();

  void GetHeapsPageStatsSummary(const BaseHeap* const* provided_heaps,
                                size_t heaps_count, uint32_t& unreserved_pages,
                                uint32_t& reserved_pages, uint32_t& used_pages,
                                uint32_t& reserved_bytes);

  // Dumps a map of all allocated memory to the log.
  void DumpMap();

  bool Save(ByteStream* stream);
  bool Restore(ByteStream* stream);

  void SetMMIOExceptionRecordingCallback(cpu::MmioAccessRecordCallback callback,
                                         void* context);

 private:
  int MapViews(uint8_t* mapping_base);
  void UnmapViews();

  // Physical address of a buffer within one physical view, or UINT32_MAX.
  uint32_t GetHostBufferPhysicalAddress(uint32_t guest_address,
                                        uint32_t length) const;

  static uint32_t HostToGuestVirtualThunk(const void* context,
                                          const void* host_address);

  bool AccessViolationCallback(global_unique_lock_type global_lock_locked_once,
                               void* host_address, bool is_write);
  static bool AccessViolationCallbackThunk(
      global_unique_lock_type global_lock_locked_once, void* context,
      void* host_address, bool is_write);

  std::filesystem::path file_name_;
  uint32_t system_page_size_ = 0;
  uint32_t system_allocation_granularity_ = 0;
  uint8_t* virtual_membase_ = nullptr;
  uint8_t* physical_membase_ = nullptr;

  xe::memory::FileMappingHandle mapping_ =
      xe::memory::kFileMappingHandleInvalid;
  uint8_t* mapping_base_ = nullptr;
  union {
    struct {
      uint8_t* v00000000;
      uint8_t* v40000000;
      uint8_t* v7F000000;
      uint8_t* v80000000;
      uint8_t* v90000000;
      uint8_t* vA0000000;
      uint8_t* vC0000000;
      uint8_t* vE0000000;
      uint8_t* physical;
    };
    uint8_t* all_views[9];
  } views_ = {{0}};

  std::unique_ptr<cpu::MMIOHandler> mmio_handler_;

  struct {
    VirtualHeap v00000000;
    VirtualHeap v40000000;
    VirtualHeap v80000000;
    VirtualHeap v90000000;

    VirtualHeap physical;
    PhysicalHeap v7F000000;
    PhysicalHeap vA0000000;
    PhysicalHeap vC0000000;
    PhysicalHeap vE0000000;
  } heaps_;

  friend class BaseHeap;

  friend class PhysicalHeap;
  // Pulls GPU-owned data in the physical range, waiting for the GPU with the
  // lock released. Returns whether anything in the range was GPU-owned.
  bool PullGpuOwned(global_unique_lock_type& global_lock,
                    uint32_t physical_address, uint32_t length);
  // Waits with the lock released until the GPU passed pending_fence(), which
  // is evaluated again after each wait, as other threads record meanwhile.
  // Returns a fence at least as new as every one pending_fence() now covers.
  coherence::Fence AwaitPendingGpuWrites(
      global_unique_lock_type& global_lock,
      const std::function<coherence::Fence()>& pending_fence);
  // Never waits: the GPU must have passed passed_fence, which is at least as
  // new as every copy's fence.
  void ApplyCopies(const std::vector<coherence::Copy>& copies,
                   coherence::Fence passed_fence);
  // Counted in the stats when it actually waited.
  void AwaitGpuFence(GpuPullSource* source, coherence::Fence fence);
  void DiscardGpuOwned(uint32_t physical_address, uint32_t length);
  bool IsGpuOwned(uint32_t physical_address, uint32_t length) const;
  void UpdateHostProtection(uint32_t physical_address, uint32_t length);

  xe::global_critical_region global_critical_region_;
  // Guarded by the global critical region.
  coherence::Core coherence_core_;
  GpuPullSource* gpu_pull_source_ = nullptr;
  // Threads in GpuPullSource::AwaitFence, which may still use the old source
  // after it's replaced.
  std::atomic<uint32_t> gpu_pull_waiters_{0};
  std::atomic<uint64_t> coherence_waits_{0};
  std::atomic<uint64_t> coherence_wait_us_{0};
  ReservationInvalidator reservation_invalidator_ = nullptr;
  void* reservation_invalidator_context_ = nullptr;
  std::vector<std::pair<PhysicalMemoryInvalidationCallback, void*>*>
      physical_memory_invalidation_callbacks_;
  std::vector<std::pair<PhysicalMemoryReadCallback, void*>*>
      physical_memory_read_callbacks_;
};

}  // namespace xe

#endif  // XENIA_MEMORY_H_
