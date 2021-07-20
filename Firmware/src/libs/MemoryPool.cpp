#include "MemoryPool.h"
#include "OutputStream.h"

#include "FreeRTOS.h" // defines public interface we're implementing here
#include "task.h" // defines public interface we're implementing here

#include <cstring>

#ifdef MEMDEBUG
    #define MDEBUG(...) printf(__VA_ARGS__)
#else
    #define MDEBUG(...)
#endif

class AutoLock
{
public:
    AutoLock(){ vTaskSuspendAll(); }
    ~AutoLock(){ xTaskResumeAll(); }
};

// this catches all usages of delete blah. The object's destructor is called before we get here
// it first checks if the deleted object is part of a pool, and uses free otherwise.
void  operator delete(void* p)
{
    MemoryPool* m = MemoryPool::first;
    while (m)
    {
        if (m->has(p))
        {
            MDEBUG("Pool %p has %p, using dealloc()\n", m, p);
            m->dealloc(p);
            return;
        }
        m = m->next;
    }

    //MDEBUG("no pool has %p, using free()\n", p);
    free(p);
}


#define offset(x) ((uint32_t)(((uint8_t*) x) - ((uint8_t*) this->base)))

typedef struct __attribute__ ((packed))
{
    uint32_t next :31;
    uint32_t used :1;

    uint8_t data[];
} _poolregion;

MemoryPool* MemoryPool::first = NULL;

MemoryPool::MemoryPool(void* _base, uint32_t _size)
{
    // clear it first
    //memset(_base, 0, _size);
    this->base = _base;
    this->size = _size;

    ((_poolregion*) _base)->used = 0;
    ((_poolregion*) _base)->next = _size;

    // insert ourselves into head of LL
    next = first;
    first = this;
}

MemoryPool::~MemoryPool()
{
    MDEBUG("Pool %p destroyed: region %p (%lu)\n", this, base, size);

    // remove ourselves from the LL
    if (first == this)
    {   // special case: we're first
        first = this->next;
        return;
    }

    // otherwise search the LL for the previous pool
    MemoryPool* m = first;
    while (m)
    {
        if (m->next == this)
        {
            m->next = next;
            return;
        }
        m = m->next;
    }
}

void* MemoryPool::alloc(size_t nbytes)
{
    AutoLock lock();

    // nbytes = ceil(nbytes / 4) * 4
    if (nbytes & 3)
        nbytes += 4 - (nbytes & 3);

    // start at the start
    _poolregion* p = ((_poolregion*) base);

    // find the allocation size including our metadata
    uint32_t nsize = nbytes + sizeof(_poolregion);

    MDEBUG("\tallocate %lu bytes (requested %u) from %p\n", nsize, nbytes, base);

    // now we walk the list, looking for a sufficiently large free block
    do {
        MDEBUG("\t\tchecking %p (%s, %lub)\n", p, (p->used?"used":"free"), p->next);
        if ((p->used == 0) && (p->next >= nsize))
        {   // we found a free space that's big enough
            MDEBUG("\t\tFOUND free block at %p (%lu) with %lu bytes\n", p, offset(p), p->next);
            // mark it as used
            p->used = 1;

            // if there's free space at the end of this block
            if (p->next > nsize)
            {
                // q = p->next
                _poolregion* q = (_poolregion*) (((uint8_t*) p) + nsize);

                MDEBUG("\t\twriting header to %p (%lu) (%lu)\n", q, offset(q), p->next - nsize);
                // write a new block header into q
                q->used = 0;
                q->next = p->next - nsize;

                // set our next to point to it
                p->next = nsize;

                // sanity check
                if (offset(q) >= size)
                {
                    // captain, we have a problem!
                    // this can only happen if something has corrupted our heap, since we should simply fail to find a free block if it's full
                    __asm("bkpt #0");
                }
            }

            // then return the data region for the block
            return &p->data;
        }

        // p = p->next
        p = (_poolregion*) (((uint8_t*) p) + p->next);

        // make sure we don't walk off the end
    } while (p < (_poolregion*) (((uint8_t*)base) + size));

    // fell off the end of the region!
    return NULL;
}

void MemoryPool::dealloc(void* d)
{
    AutoLock lock();
    _poolregion* p = (_poolregion*) (((uint8_t*) d) - sizeof(_poolregion));
    p->used = 0;

    MDEBUG("\tdeallocating %p (%lu, %lub)\n", p, offset(p), p->next);

    // combine next block if it's free
    _poolregion* q = (_poolregion*) (((uint8_t*) p) + p->next);
    if(q >= (_poolregion*) (((uint8_t*) base) + size)) {
        // we are beyond the end of the pool, ie last block
        return;
    }

    if (q->used == 0)
    {
        MDEBUG("\t\tCombining with next free region at %p, new size is %d\n", q, p->next + q->next);

        // sanity check
        if (offset(q) > size)
        {
            // captain, we have a problem!
            // this can only happen if something has corrupted our heap, since we should simply fail to find a free block if it's full
            __asm("bkpt #0");
        }

        p->next += q->next;
    }

    // walk the list to find previous block
    q = (_poolregion*) base;
    do {
        // check if q is the previous block
        if ((((uint8_t*) q) + q->next) == (uint8_t*) p) {
            // q is the previous block.
            if (q->used == 0)
            { // if q is free
                MDEBUG("\t\tCombining with previous free region at %p, new size is %d\n", q, p->next + q->next);

                // combine!
                q->next += p->next;

                // sanity check
                if ((offset(p) + p->next) > size)
                {
                    // captain, we have a problem!
                    // this can only happen if something has corrupted our heap, since we should simply fail to find a free block if it's full
                    __asm("bkpt #0");
                }
            }

            // we found previous block, return
            return;
        }

        // return if last block
        if (offset(q) + q->next >= size)
            return;

        // q = q->next
        q = (_poolregion*) (((uint8_t*) q) + q->next);

        // if some idiot deallocates our memory region while we're using it, strange things can happen.
        // avoid an infinite loop in that case, however we'll still leak memory and may corrupt things
        if (q->next == 0)
            return;

        // make sure we don't walk off the end
    } while (q < (_poolregion*) (((uint8_t*) base) + size));
}

void MemoryPool::debug(OutputStream& os)
{
    _poolregion* p = (_poolregion*) base;
    uint32_t tot = 0;
    uint32_t free = 0;
    os.printf("Start: %ub MemoryPool at %p\n", size, p);
    do {
        os.printf("\tChunk at %p (%4lu): %s, %lu bytes\n", p, offset(p), (p->used?"used":"free"), p->next);
        tot += p->next;
        if (p->used == 0)
            free += p->next;
        if ((offset(p) + p->next >= size) || (p->next <= sizeof(_poolregion)))
        {
            os.printf("End: total %lub, free: %lub\n", tot, free);
            return;
        }
        p = (_poolregion*) (((uint8_t*) p) + p->next);
    } while (1);
}

bool MemoryPool::has(void* p)
{
    return ((p >= base) && (p < (void*) (((uint8_t*) base) + size)));
}

uint32_t MemoryPool::available()
{
    uint32_t free = 0;

    _poolregion* p = (_poolregion*) base;

    do {
        if (p->used == 0)
            free += p->next;
        if (offset(p) + p->next >= size)
            return free;
        if (p->next <= sizeof(_poolregion))
            return free;
        p = (_poolregion*) (((uint8_t*) p) + p->next);
    } while (1);
}


extern "C" void *AllocDTCMRAM(size_t size) { return _DTCMRAM->alloc(size); }
extern "C" void DeallocDTCMRAM(void *mem) { _DTCMRAM->dealloc(mem); }
extern "C" void *AllocSRAM_1(size_t size) { return _SRAM_1->alloc(size); }
extern "C" void DeallocSRAM_1(void *mem) { _SRAM_1->dealloc(mem); }
