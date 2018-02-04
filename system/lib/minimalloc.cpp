/*
 * Simple minimalistic but efficient malloc/free.
 *
 * Assumptions:
 *
 *  - 32-bit system.
 *  - sbrk() is available (and nothing better, it's all we use).
 *  - sbrk() will not be accessed on another thread in parallel to us.
 *
 */
#include <assert.h>
#include <string.h> // for memcpy, memset
#include <unistd.h> // for sbrk()

// Math utilities

static bool isPowerOf2(size_t x) {
  return __builtin_popcount(x) == 1;
}

static size_t upperBoundByPowerOf2(size_t x) {
  if (x == 0) return 1;
  if (isPowerOf2(x)) return x;
  // e.g. 5 is 0..0101, so clz is 29, and we
  // want 8 which is 1 << 3
  return 1 << (32 - __builtin_clz(x));
}

static size_t lowerBoundByPowerOf2(size_t x) {
  if (x == 0) return 1;
  if (isPowerOf2(x)) return x;
  // e.g. 5 is 0..0101, so clz is 29, and we
  // want 4 which is 1 << 2
  return 1 << (31 - __builtin_clz(x));
}

// Constants

// All allocations are aligned to this value.
static const size_t ALIGNMENT = 16;

// Even allocating 1 byte incurs this much actual
// allocation. This is our minimum bin size.
static const size_t MIN_ALLOC = ALIGNMENT;

// How big the metadata is in each region. It is convenient
// that this is identical to the above values.
static const size_t METADATA_SIZE = MIN_ALLOC;

// How big a minimal region is.
static const size_t MIN_REGION_SIZE = METADATA_SIZE + MIN_ALLOC;

// Constant utilities

// Align a pointer, increasing it upwards as necessary
static size_t alignUp(size_t ptr) {
  return (size_t(ptr) + ALIGNMENT - 1) & -ALIGNMENT;
}

static void* alignUpPointer(void* ptr) {
  return (void*)alignUp(size_t(ptr));
}

//
// Data structures
//

struct Region;

// Information memory that is a free list, i.e., may
// be reused.
struct FreeInfo {
  // free lists are doubly-linked lists
  FreeInfo* prev;
  FreeInfo* next;
};

// A contiguous region of memory. Metadata at the beginning describes it,
// after which is the "payload", the sections that user code calling
// malloc can use.
struct Region {
  // The total size of the section of memory this is associated
  // with and contained in.
  // That includes the metadata itself and the payload memory after,
  // which includes the used and unused portions of it.
  size_t totalSize;

  // How many bytes are used out of the payload. If this is 0, the
  // region is free for use (we don't allocate payloads of size 0).
  size_t usedPayload;

  // Each memory area knows its neighbors, as we hope to merge them.
  // If there is no neighbor, NULL.
  Region* prev;
  Region* next;

  // Up to here was the fixed metadata, of size 16. The rest is either
  // the payload, or freelist info.
  union {
    char payload[];
    FreeInfo freeInfo;
  };
};

// Region utilities

static void initRegion(Region* region, size_t totalSize, size_t usedPayload) {
  region->totalSize = totalSize;
  region->usedPayload = usedPayload;
  region->prev = NULL;
  region->next = NULL;
}

static void* getPayload(Region* region) {
  assert(((char*)&region->freeInfo) - ((char*)region) == METADATA_SIZE);
  assert(region->usedPayload);
  return &region->payload;
}

static Region* fromPayload(void* payload) {
  return (Region*)((char*)payload - METADATA_SIZE);
}

static Region* fromFreeInfo(FreeInfo* freeInfo) {
  return (Region*)((char*)freeInfo - METADATA_SIZE);
}

static size_t getMaximumPayloadSize(Region* region) {
  return region->totalSize - METADATA_SIZE;
}

static FreeInfo* getFreeInfo(Region* region) {
  assert(!region->usedPayload);
  return &region->freeInfo;
}

static void* getAfter(Region* region) {
  return ((char*)region) + region->totalSize;
}

// Globals

// TODO: For now we have a single global space for all allocations,
//       but for multithreading etc. we may want to generalize that.

// A freelist (a list of Regions ready for re-use) for all
// power of 2 payload sizes (only the ones from ALIGNMENT
// size and above are relevant, though). The freelist at index
// K contains regions of memory big enough to contain at least
// 2^K bytes.

static const size_t MIN_FREELIST_INDEX = 4;  // 16 == MIN_ALLOC
static const size_t MAX_FREELIST_INDEX = 32; // uint32_t

static FreeInfo* freeLists[MAX_FREELIST_INDEX] = {
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

// The last region of memory. It's important to know the end
// since we may append to it.
static Region* lastRegion = NULL;

// Global utilities

static size_t getFreeListIndex(size_t size) {
  assert(1 << MIN_FREELIST_INDEX == MIN_ALLOC);
  assert(size > 0);
  if (size < MIN_ALLOC) size = MIN_ALLOC;
  // We need a lower bound here, as the list contains things
  // that can contain at least a power of 2.
  size_t ret = lowerBoundByPowerOf2(size);
  assert(MIN_FREELIST_INDEX <= ret && ret < MAX_FREELIST_INDEX);
  return ret;
}

static size_t getMinSizeForFreeListIndex(size_t index) {
  return 1 << index;
}

static void removeFromFreeList(Region* region) {
  assert(region->usedPayload);
  size_t index = getFreeListIndex(getMaximumPayloadSize(region));
  FreeInfo* freeInfo = getFreeInfo(region);
  if (freeLists[index] == freeInfo) {
    freeLists[index] = freeInfo->next;
  }
  if (freeInfo->prev) {
    freeInfo->prev->next = freeInfo->next;
  }
  if (freeInfo->next) {
    freeInfo->next->prev = freeInfo->prev;
  }
}

static void addToFreeList(Region* region) {
  assert(region->usedPayload);
  size_t index = getFreeListIndex(getMaximumPayloadSize(region));
  FreeInfo* freeInfo = getFreeInfo(region);
  FreeInfo* last = freeLists[index];
  freeLists[index] = freeInfo;
  freeInfo->prev = NULL;
  freeInfo->next = last;
}

static void possiblySplitRemainder(Region* region, size_t size) {
  size_t payloadSize = getMaximumPayloadSize(region);
  assert(payloadSize >= size);
  size_t extra = payloadSize - size;
  // We need room for a minimal region, but also must align it.
  if (extra >= MIN_REGION_SIZE + ALIGNMENT) {
    // Worth it, split the region
    // TODO: Consider not doing it, may affect long-term fragmentation.
    Region* split = (Region*)alignUpPointer((char*)getPayload(region) + size);
    size_t totalSplitSize = (char*)split - (char*)region;
    assert(totalSplitSize >= MIN_REGION_SIZE);
    initRegion(split, totalSplitSize, size);
    split->prev = region;
    split->next = region->next;
    region->next = split;
    addToFreeList(split);
  }
}

static void useRegion(Region* region, size_t size) {
  assert(!region->usedPayload);
  region->usedPayload = size;
  // We may not be using all of it, split out a smaller
  // region into a free list if it's large enough.
  possiblySplitRemainder(region, size);
}

static Region* useFreeInfo(FreeInfo* freeInfo) {
  Region* region = fromFreeInfo(freeInfo);
  // This region is no longer free
  removeFromFreeList(region);
  // This region is now in use
  useRegion(region, size);
  return region;
}

// When we free something of size 100, we put it in the
// freelist for items of size 64 and above. Then when something
// needs 64 bytes, we know the things in that list are all
// suitable. However, note that this means that if we then
// try to allocate something of size 100 once more, we will
// look in the freelist for items of size 128 or more (again,
// so we know all items in the list are big enough), which means
// we may not reuse the perfect region we just freed. It's hard
// to do a perfect job on that without a lot more work (memory
// and/or time), so instead, we use a simple heuristic to look
// at the one-lower freelist, which *may* contain something
// big enough for us. We look at just a few elements, but that is
// enough if we are alloating/freeing a lot of such elements
// (since the recent items are there).
// TODO: Consider more optimizations, e.g. slow bubbling of larger
//       items in each freelist towards the root, or even actually
//       keep it sorted by size.
static const size_t SPECULATIVE_FREELIST_TRIES = 3;

static Region* tryFromFreeList(size_t size) {
  size_t index = getFreeListIndex(size);
  // If we *may* find an item in the index one
  // below us, try that briefly in constant time;
  // see comment on algorithm on the declaration of
  // SPECULATIVE_FREELIST_TRIES.
  if (index > MIN_FREELIST_INDEX &&
      size < getMinSizeForFreeListIndex(index)) {
    FreeInfo* freeInfo = freeLists[index - 1];
    size_t tries = 0;
    while (freeInfo && tries < SPECULATIVE_FREELIST_TRIES) {
      Region* region = fromFreeInfo(freeInfo);
      if (getMaximumPayloadSize(region) >= size) {
        // Success, use it
        return useFreeInfo(freeInfo);
      }
      freeInfo = freeInfo->next;
      tries++;
    }
  }
  while (index < MAX_FREELIST_INDEX) {
    FreeInfo* freeInfo = freeLists[index];
    if (freeInfo) {
      // We found one, use it.
      return useFreeInfo(freeInfo);
    }
    // Look in a freelist of larger elements.
    // TODO This does increase the risk of fragmentation, though,
    //      and maybe the iteration adds runtime overhead.
    index++;
  }
  // No luck, no free list.
  return NULL;
}

static Region* newAllocation(size_t size) {
  assert(size > 0);
  size_t sbrkSize = METADATA_SIZE + alignUp(size);
  void* ptr = sbrk(sbrkSize);
  if (ptr == (void*)-1) {
    // sbrk() failed, we failed.
    return NULL;
  }
  // sbrk() results might not be aligned. We assume single-threaded sbrk()
  // access here in order to fix that up
  void* fixedPtr = alignUpPointer(ptr);
  if (ptr != fixedPtr) {
    size_t extra = (char*)fixedPtr - (char*)ptr;
    void* extraPtr = sbrk(extra);
    if (extraPtr == (void*)-1) {
      // sbrk() failed, we failed.
      return NULL;
    }
    // Verify the single-threaded assumption. If this fails, it means
    // we also leak the previous allocation, so we don't even try to
    // handle it.
    assert((char*)extraPtr == (char*)ptr + sbrkSize);
    // We now have a contiguous block of memory from ptr to
    // ptr + sbrkSize + fixedPtr - ptr = fixedPtr + sbrkSize.
    // fixedPtr is aligned and starts a region of the right
    // amount of memory.
  }
  Region* region = (Region*)fixedPtr;
  // Success, we have new memory
  initRegion(region, sbrkSize, size);
  useRegion(region, size);
  // Apply globally, connect it to lastRegion
  if (lastRegion) {
    // If this is adjacent to the previous region, link them.
    if (region == getAfter(lastRegion)) {
      assert(lastRegion->next == NULL);
      lastRegion->next = region;
      region->prev = lastRegion;
    }
  }
  lastRegion = region;
  return region;
}

int mergeIntoExistingFreeRegion(Region *region) {
  int merged = 0;
  Region* prev = region->prev;
  Region* next = region->next;
  if (prev && !prev->usedPayload) {
    // Merge them.
    removeFromFreeList(prev);
    prev->totalSize += region->totalSize;
    prev->next = region->next;
    // We may also be able to merge with the next, keep trying.
    if (next && !next->usedPayload) {
      removeFromFreeList(next);
      prev->totalSize += next->totalSize;
      prev->next = next->next;
    }
    addToFreeList(prev);
    return 1;
  }
  if (next && !next->usedPayload) {
    // Merge them.
    removeFromFreeList(next);
    region->totalSize += next->totalSize;
    region->next = next->next;
    addToFreeList(region);
    return 1;
  }
  return 0;
}

// public API

extern "C" {

void* malloc(size_t size) {
  if (size == 0) return NULL;
  // Look in the freelist first.
  Region* region = tryFromFreeList(size);
  if (!region) {
    // Allocate some new memory otherwise.
    region = newAllocation(size);
    if (!region) {
      // We failed to allocate, sadly.
      return NULL;
    }
  }
  return getPayload(region);
}

void free(void *ptr) {
  if (ptr == NULL) return;
  Region* region = fromPayload(ptr);
  region->usedPayload = 0;
  // Perhaps we can join this to an adjacent free region, unfragmenting?
  if (!mergeIntoExistingFreeRegion(region)) {
    // Otherwise, mark as unused and add to freelist.
    addToFreeList(region);
  }
}

void* calloc(size_t nmemb, size_t size) {
  // TODO If we know no one else is using sbrk(), we can assume that new
  //      memory allocations are zero'd out.
  void* ptr = malloc(size);
  if (!ptr) return NULL;
  memset(ptr, 0, size);
  return ptr;
}

void* realloc(void *ptr, size_t size) {
  if (!ptr) return malloc(size);
  if (!size) {
    free(ptr);
    return NULL;
  }
  Region* region = fromPayload(ptr);
  if (size == region->usedPayload) {
    // Nothing to do.
    return ptr;
  }
  if (size < region->usedPayload) {
    // Shrink it.
    region->usedPayload = size;
    // There might be enough left over to split out now.
    possiblySplitRemainder(region, size);
    return ptr;
  }
  // Grow it. First, maybe we can do simple growth in the current region.
  if (size <= getMaximumPayloadSize(region)) {
    region->usedPayload = size;
    return ptr;
  }
  // Perhaps right after us is free space we can merge to us. We
  // can only do this once, as if there were two free regions after
  // us they would have already been merged.
  Region* next = region->next;
  if (next && !next->usedPayload &&
      size <= getMaximumPayloadSize(region) + next->totalSize) {
    region->totalSize += next->totalSize;
    region->usedPayload = size;
    region->next = next->next;
    removeFromFreeList(next);
    return ptr;
  }
  // Slow path: New allocation, copy to there, free original.
  void* newPtr = malloc(size);
  if (!newPtr) return NULL;
  memcpy(newPtr, getPayload(region), region->usedPayload);
  free(ptr);
  return newPtr;
}

// XXX how about the max size of allocation, half the size of total memory? and 3/4 of total memory? how do those fit in freelists?

} // extern "C"
