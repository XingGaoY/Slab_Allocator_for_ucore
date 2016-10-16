[TOC]

# Paper Review: The Slab Allocator: An Object-Caching Kernel Allocator.

---
[TOC]

### Basic Ideas

The Slab allocator is based on an approach that through caching objects in memory, Slab can reduce the cost of frequent allocating same object models. The idea is to preserve the invariant portion of the object's initial state to eliminate the time to destroy and recreate. For example, lock and condition variables may be heavily used in multithread programs.

As the paper suggested, the cost of constructing an object can be significantly higher than the cost of allocating memory for it.

So, the basic design is straightforward:

**Allocate:**

```
if(there's an object in the cache)
  take it(no need to reconstruct);
else{
  allocate memory;
  construct the object.
}
```

**Free:**

`return to cache(no need to destruct);`

**Reclaim memory from cache:**

```
take some objects from the cache;
destroy the objects;
free the underlying memory;
```

### Why Central Allocator

From my point of view, things may get easier to put Object caching in subsystem even in user's system. 

There are several advantages to put object caching in central allocator:

* Users don't know when to return memory from the cache when system wants memory back. They don't have the information of the overall memory needs of the system.
* It is easier to monitor and debug to get the insight into the cache.

### Object Cache Interface

Here, the paper mentioned to observations:

1. Descriptions of objects\(name size, alignment, constructor and destructor\) belong in the **clients**, not in the central allocator.
2. Memory management policies belong in the centrl allocator, not in the clients.  

* Hence, from the former principle, object cache creation must be client-driven and must include a full specification of objects, and the function returns an opaque descriptor for accessing the cache.

* And from the latter one, the clients should need just two simple functions to allocate and free objects.

  The object got from the cache is already in its constructed state. `flags` is either `KM_SLEEP` or `KM_NOSLEEP`, to indicate whether it is acceptable to wait for memory if none is currently available.

* The object will still be in its constructed state, after it is returned to the cache.

* Finally, if a cache is no longer needed, it can be destroyed by the client, and reclaim all the associated resources.

**The functions above does not have to be built with compile-time knowledge of its clients as most custom allocators do, nor does it have to keep guessing as in the adaptive-fit methods. Rather, the object-cache interface allows clients to specify the allocation services they need on the fly.**

### Slab Allocator Implementation

#### Caches

Each cache has a front and back end:
![cache](https://raw.github.com/XingGaoY/Slab_Allocator_for_ucore/master/Review%20and%20Summary/img/cache.png)

* The front end is the public interface to the allocator, which moves objects in and out
* The back end manages the flow of real memory through the cache. ** Note that all back-end activity is triggered solely by memory presure **  
  Each cache maintains its own statistics - total allocations, number of allocated and freed buffers, etc. They indicate which part of system consume the most memory and help to indentify memory leaks.  
  They also indicate the activity level in various subsystems, to the extent that allocator traffic is an accurate approximation.

#### Slabs

The slab is the primary unit in the allocator. The allocator acquires or reclaims a entire slab at once. A slab consists of one or more pages of virtually contiguous memory carved up into equal-size chunks, with a reference count indicating how many of those chunks have been allocated.

* Reclaiming unused memory is trival.
* Allocating and freeing memory are fast, constant-time operations.
* Severe external fragmentation\(unused bffers on the freelist\) is unlikely.
* Internal fragmentation\(per-buffer wasted space\) is minimal.

  ##### Slab Layout - Logical

* The data structure `kmem_slab` maintains the slab's linkage in the cache, its reference count and its list of free buffers.

* In turn, each buffer in the slab is managed by a `kmem_bufctl` structure thar holds the freelist linkage, buffer address and a back-pointer to the controlling slab.
  A slab may look like this:
  ![slab](https://raw.github.com/XingGaoY/Slab_Allocator_for_ucore/master/Review%20and%20Summary/img/slab.png)

  ##### Slab Layout for Small Objects

  For objects smaller than 1\/8 page, a slab is built by allocating a page, placing the slab data at the end and dividing the rest into equal-size buffers:
  ![page](https://raw.github.com/XingGaoY/Slab_Allocator_for_ucore/master/Review%20and%20Summary/img/slab_page.png)
  Each buffer serves as its own bufctl while on the freelist. Only the linkeage is actually needed, since everything else is computable.

  The freelist linkage resides at the end of the buffer.

  ##### Slab Layout for Large Objects

  The scheme above is not efficient for large objects. In other words, it is more a partial page allocator.

  ##### Freelist Management

  **Each cache maintains a circular, doubly-linked list of all its ****~~slabs~~**. The slab list is partially sorted, empty slabs comes the first, followed by the partial slabs, and finally the complete slabs.

  The cache's freelist pointer points to its first non-empty slab.

  Each slab has its own freelist of available buffers.

  When the allocator reclaims al slab, it just unlinks the slab.

  ##### Reclaiming Memory

  When `kmem_cache_free` sees that the slab reference count is zero, it moves the slab to the tail of the freelist where all the complete slabs reside.

  When more memory is needed, the system asks the allocator to liberate as much memory as it can. **The allocator obliges, but retains a 15 second working set of recently-used slabs to prevent thrashing**.


