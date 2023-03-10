#pragma once

#ifdef __cplusplus
#include <cstdint>
#include <cstddef>

class OutputStream;

/*
 * with MUCH thanks to http://www.parashift.com/c++-faq-lite/memory-pools.html
 *
 * test framework at https://gist.github.com/triffid/5563987
 */

class MemoryPool
{
public:
    MemoryPool(void* base, uint32_t size);
    ~MemoryPool();

    void* alloc(size_t);
    void  dealloc(void* p);
    void  debug(OutputStream&);
    bool  has(void*);
    uint32_t available(void);
    uint32_t get_size(void) const { return size; };

    MemoryPool* next;

    static MemoryPool* first;

private:
    void* base;
    uint32_t size;
};

// this overloads "placement new"
inline void* operator new(size_t nbytes, MemoryPool& pool)
{
    return pool.alloc(nbytes);
}

inline void* operator new[](size_t nbytes, MemoryPool& pool)
{
    return pool.alloc(nbytes);
}

// this allows placement new to free memory if the constructor fails
inline void  operator delete(void* p, MemoryPool& pool)
{
    pool.dealloc(p);
}

extern MemoryPool *_DTCMRAM;
extern MemoryPool *_SRAM_1;

extern "C" void *AllocDTCMRAM(size_t size);
extern "C" void DeallocDTCMRAM(void *mem);
extern "C" void *AllocSRAM_1(size_t size);
extern "C" void DeallocSRAM_1(void *mem);

#else
// For c programs
extern void *AllocDTCMRAM(size_t size);
extern void DeallocDTCMRAM(void *mem);
extern void *AllocSRAM_1(size_t size);
extern void DeallocSRAM_1(void *mem);
#endif
