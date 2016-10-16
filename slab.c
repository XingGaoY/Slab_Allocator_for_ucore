#include <types.h>
#include <list.h>
#include <spinlock.h>
#include <kio.h>
#include <string.h>
#include <mmu.h>
#include <sem.h>
#include <cache.h>
#include <sync.h>
#include <compiler.h>

#define info 0;

#define CACHE_NAMELEN 10;       //TODO object name length
#define BYTE_PER_WORD sizeof(void*);
#define MAX_ORDER 11;           //different with the def in buddy_pmm.c, consistent with linux4.8, TODO TEST: maybe result the same.
#define MAX_GFP_ORDER 5;        //max size of pages 2^~ in a slab
#define CFLGS_OFF_SLAB          //TODO def flags
#define SLAB_LIMIT      0xFFFFFFFEL	// the max value of obj number
#define BUFCTL_END 				//TODO end marker of a slabbuf_ctl

typedef uint32_t kmem_bufctl_t;

//global vals, calculate when slab init
uint32_t offslab_limit;    
uint32_t slab_break_gfp_order;
semaphore_t cache_chain_sem;
/*
------------------------
Two main type of object in slab;
* kmem_cache_s: defines a cache desciptor to manage some slabs of a same object;
* slab_s: manage the objects in a slab;
------------------------
*/
typedef struct kmem_cache_s{
    list_entry_t slabs_full;
    list_entry_t slabs_partial;
    //struct list_entry slabs_free;
    size_t objsize;         //The size of each object in the slab
    size_t flags;           //determines how the allocator behaves
    size_t num;             //num of objects contained i each slab
    spinlock_s spinlock;

    //&gfp parameters
    uint32_t gfporder;      //indicates the 2^order pages size of the slab
    uint32_t gfpflags;      //flags for calling buddy system

    //color scheme
    size_t colour;	    //number of different offset can use
    uint32_t colour_off;	    //multiple to offset each object in the slab
    uint32_t colour_next;   //the next colour line to use, wrap back to 0 when reach colour
    uint32_t dflags;	    //flags modified during its lifetime							
	uint32_t growing;		//indicate it is growing to make it less likely to be eliminated

    kmem_cache_t *slabp_cache;  //if slab_desp is off_slab, it will be saved here;

    //constructor and destructor, may be NULL
    void(*ctor)(void*, kmem_cache_t *,uint32_t);
    void(*dtor)(void*, kmem_cache_t *,uint32_t);

    char name[CACHE_NAMELEN];   //human readabl object name
    
    list_entry_t next;     //the next cache in the cache chain;
}kmem_cache_t;

typedef struct slab_s{
    list_entry_t    list;     //the linked list the slab belongs to, slab_full or slabs_partial;
    uint32_t 	    colouroff;//color offset from the base addr of the 1st obj in the slab
    void            *s_mem;   //the starting address of the first object within the slab.
    uint32_t        inuse;    //the number of active objects in the slab
    kmem_bufctl_t   free;     //the first free object in the slab.
}slab_t;

typedef struct cache_sizes{
	size_t 		cs_size;
	kmem_cache_t   *cs_cachep;
	//kmem_cache_t   *cs_dmacachep
}cache_sizes_t;
//get the beginning of the kmem_bufctl_t array
#define slab_bufctl(slabp)					 \
	((kmem_bufctl_t *)(((slab_t*)slabp)+1))
// get the slab address according to the link element (see list.h)
#define le2cache(le, member)                 \
    to_struct((le), kmem_cache_t, member)
//set the info in page, which cache or page it belongs to 
#define SET_PAGE_CACHE(page, cachep)                                                \
    do {                                                                            \
        struct Page *__page = (struct Page *)(page);                                \
        kmem_cache_t **__cachepp = (kmem_cache_t **)&(__page->page_link.next);      \
        *__cachepp = (kmem_cache_t *)(cachep);                                      \
    } while (0)

#define SET_PAGE_SLAB(page, slabp)                                                  \
    do {                                                                            \
        struct Page *__page = (struct Page *)(page);                                \
        slab_t **__cachepp = (slab_t **)&(__page->page_link.prev);                  \
        *__cachepp = (slab_t *)(slabp);                                             \
    } while (0)


//the cache of the cache(really awesome, how could i forget this)
static kmem_cache_t cache_cache = {
slabs_full:     list_init(cache_cache.slabs_full),
slab_partial:   list_init(cache_cache.slabs_partial),
//slab_free:    list_init(cache_cache.slabs_free),
objsize:        sizeof(kmem_cache_t),
flags:          SLAB_NO_REAP,
spinlock:       {0},
name:           "kmem_cache"
};

static cache_sizes_t cache_sizes[] = {
	{    32, NULL},
	{    64, NULL},
	{   128, NULL},
	{   256, NULL},
	{   512, NULL},
	{  1024, NULL},
	{  2048, NULL},
	{  4096, NULL},
	{  8192, NULL},
	{ 16384, NULL},
	{ 32768, NULL},
	{ 65536, NULL},
	{131072, NULL},
	{     0, NULL}
};


void kmem_cache_init_objs(kmem_cache_t *cachep, slab_t *slabp, uint32_t ctor_flags);


/*   
-----------------------------------------------------------
			Section on CACHE
-----------------------------------------------------------
*/


//XXX cannot figure out what the original code did,roundup seems a waste of time
/*kmem_cache_estimate--Determine how many obj will be put in a slab, and the space left unused.
 *@gfporder: indicate the order of the pages in a slab
 *@size: size of the object
 *@flags: flags of the cache
 *@left_over: the number of bytes left in the slab, meant to be returned.
 *@num: the number of obj can be put in the slab, meant to be returned.
 */
static void kmem_cache_estimate(uint32_t gfporder, size_t size,
        int flags, size_t *left_over, uit32_t *num)
{
	int i=0;

	size_t wastage = PAGE_SIZE<<gfporder;
	size_t base = 0;
	size_t extra = 0;
	
	if(!(flags & CFLGS_OFF_SLAB)){
		base = sizeof(slab_t);
		extra = sizeof(kmem_bufctl_t);
	}
	//obj_num is the num of objs, solve the equation $i*size + base + i*extra <= wastage$
	while(i*size + L1_CACHE_ALIGN(base+i*extra) <= wastage)
		i++;
	if(i > 0)
		i--;
	if(i > SLAB_LIMIT)
		i = SLAB_LIMIT;

	*num = i;
	wastage -= i*size;
	wastage -= L1_CACHE_ALIGN(base+i*extra);
	*left_over = wastage;
}

void kmem_cache_free(kmem_cache_t *cachep, void *objp);
kmem_cache_t * kmem_find_general_cachep(size_t size, int gfpflags);
//Not defined in the original virsion, for caches are already organized as an array. 
/*kmem_cache_create--create a new cache, and check some para
 *@name: human readable name of a object(cache)
 *@size: the size of the object
 *@flags: cache flags
 *@ctor& dtor: constructor and destructor of the objects, can be left NULL
 */
kmem_cache_t * kmem_cache_create(const char* name, size_t size, 
        size_t offset, size_t flags,
        void (*ctor)(void*, kmem_cache_t *, uint32_t),
        void (*dtor)(void*, kmem_cache_t *, uint32_t))
{
    const char* bug_location = KERN_ERR "kmem_create: ";
    size_t left_over, align, slab_size;
    
    kmem_cache_t *cachep = NULL;    //TODO
    
    /*check usage bugs
     */
    if((!name)||
       ((strlen(name) >= CACHE_NAMELEN -1)) ||
       //((in_interrupt()||
       (size < BYTES_PER_WORD) ||
       (size > (1<<MAX_ORDER)*PGSIZE) ||
       (dtor && !ctor) ||
       (offset < 0 || offset > size))
        BUG(bug_location,"bad parameter usage");
    
    cachep = (kmem_cache_t *)kmem_cache_alloc(&cache_cache, SLAB_KERNEL);
    //simply allocate a new cache TODO kmem_cache_alloc 
    if(!cachep)
        goto opps;
    memset(cachep, 0, sizeof(kmem_cache_t));    //TODO shouldbe put in kmem_cache_alloc?
    
    //aligns the object ro the wordsize
    if(size & (BYTES_PER_WORD-1)){
        size += (BYTES_PER_WORD-1);
        size &= ~(BYTES_PER_WORD-1);
#if info
        kprintf("SLAB CACHE CREATE: Forcing Alignment--%s\n", name);
#endif
    }


    //determine the value of align
    align = BYTES_PER_WORD;
    //if requested aligns the obj to the L1 CPU cache
    if(flags & SLAB_HWCACHE_ALIGN)
	align = L1_CACHE_BYTES;
    //keep slab_t offslab when too large object
    if(size >= (PGSIZE>>3))
        flags |= CFLGS_OFF_SLAB;
    //TODO Comments needed here
	if(flags & SLAB_HWCACHE_ALIGN){
		while (size < align / 2)
	    	align /= 2;
		size = (size+align-1)&(~(align-1));
    }

    //calculate num of objects in a slab and adjust the size of a slab
    do{
        uint32_t break_flag = 0;
cal_wastage:
        kmem_cache_estimate(cachep->gfporder, size, flags, &left_over, &cachep->num);
        //set if the num of objs fitting on the slab exceeds the num that can be kept in the slab when offslab descrp are used
        if(break_flag)
            break;
        //the order size of pages in a slab must not be exceeded
        if(cachep->gfporder >= MAX_GFP_ORDER)
            break;
        //TODO not understand
        if(!cachep->num)
            goto next;
        //if offslab, but num of obj 
        if(flags & CFLGS_OFF_SLAB && cachep->num > offslab_limit){
            cachep->gfporder--;
            break_flag++;
            goto cal_wastage;
        }
        //TODO nt understand
        if(cachep->gfporder >= slab_break_gfp_order)
            break;
        //make sure slab left_over is less than 1/8 of a slab
        if((left_over*8) <= (PGSIZE<<cachep->gfporder))
            break;
        //if internal fragment is too high, increase slab size and recalculate
next:
        cachep->gfporder++;
    }while(1);

    //after recalculate, if the object isstill not fit, error
    if(!cachep->num){
        kprintf("kmem_cache_create: could not create cache %s.\n", name);
        kmem_cache_free(&cache_cache, cachep);
        cachep = NULL;
        goto opps;
    }
    //calculate slab_size, align to L1 cache
    slab_size = L1_CACHE_ALIGN(cachep->num * sizeof(kmem_bufctl_t) + sizeof(slab_t));
    //if enough space left for slab descriptor, move it into slab and update flags and slab leftover
    if(flags & CFLGS_OFF_SLAB && left_over >= slab_size){
        flags &= ~CFLGS_OFF_SLAB;
        left_over -= slab_size;
    }
	
	//calculate colour offsets
	offset += (align-1);
	offset &= ~(align -1);
	if(!offset)
		offset = L1_CACHE_BYTES;
	cachep->colour_off = offset;
	cachep->colour = left_over/offset;

    cachep->flags = flags;
    cachep->gfpflags = 0;
    spinlock_init(&cache->spinlock);
    cachep->objsize = size;

    list_init(&cachep->slabs_full);
    list_init(&cachep->slabs_partial);

    //if off slab, slab will be put in a slab cache
    if(flags & CFLGS_OFF_SLAB)
        cachep->slabp_cache = kmem_find_general_cachep(slab_size, 0);

    cachep->ctor = ctor;
    cachep->dtor = dtor;
    strcpy(cachep->nme, name);
    
    //loop the list to ensure no existed cache has the same name with it
    down(&cache_chain_sem);
    {
        struct list_entry_t *p;
        p = cache_chain->next;
        while(p != cache_chain){
            if(!strcmp(le2cache(p, next)->name,name))
                BUG(bug_location, "Cache cannot be created--same cache name");
        }
    }

    list_add(&cache_chain, &cachep->next);
    up(&cache_chain_sem);
opps:
    return cachep;
}

/*-------------------------------------------------------------------------
 *			      Section on slab
 */------------------------------------------------------------------------

//Function the same.
//The original codes need less input, so some address mapping is essential
/*kmem_cache_slabmgmt: either allocate space to keep the slab descriptor **off cache**
 * or reserve enough space at the beginning of the slab for the descriptor and bufctl **in the cache**
 * @cachep: the cache which slab allocated to
 * @objp: points to the **beginning of the slab**
 * @local_flags: flags for cache
 */
static inline slab_t *kmem_cache_slabmgmt(kmem_cache_t *cachep, void *objp
		uint32_t colour_off, int local_flags)
{
	slab_t *slabp;

	//
	if(OFF_SLAB(cachep)){
		slabp = kmem_cache_alloc(cachep->slabp_cache, local_flags);
		if(!slabp)
			return NULL;
	}
	else{
		slabp = objp+colour_off;
		colour_off += L1_CACHE_ALIGN(cachep->num * sizeof(kmem_bufctl_t) +
			sizeof(slab_t);		
	}
	slabp->inuse = 0;
	slabp->colouroff = colour_off;
	slabp->s_mem = objp+colour_off;

	return slabp;
}
//Most important function in slab
/*kmem_cache_grow: allocate memory, link pages for the slab, initialize obj in the slab and add slab to the cache 
 *@cachep: the cache above the slab
 *@flags: flags for slab
 */
//TODO flags
static int kmem_cache_grow(kmem_cache_t *cachep, int flags)
{
	int 		order;
	uint32_t  	save_flags;
	uint32_t 	ctor_flags;
	slab_t   	*slabp;
	size_t 		offset;

	if(flags & SLAB_NO_GROW)
		return 0;

	ctor_flags = SLAB_CTOR_CONSTRUCTOR;
	//aquire an int-safe lock for accessing the cache descriptor
	spin_lock_irqsave(&cachep->spinlock, save_flags);
	
	offset = cachep->colour_next;
	cachep->colour_next++;
	if(cachep->colour_next >= cachep->colour)
		cachep->colour_next = 0;
	offset *= cachep->colour_off;
	cachep->dflags |= DFLGS_GROWN;	//make the slab growing, avoiding to be reaped
	
	cachep->growing++;
	spin_lock_irqrestore(&cachep->spinlock, save_flags);	

	struct *Page objp = alloc_pages(1<<cachep->gfporder);
	if(!objp)
		goto failed;
	if(!(slabp = kmem_cache_slabmgmt(cachep, objp, flags)))
		goto opps1;

	//link the pages for the slab
	order = 1 << cachep->gfporder;
	do{
               //setup this page in the free list (see memlayout.h: struct page)???
               SET_PAGE_CACHE(page, cachep);
               SET_PAGE_SLAB(page, slabp);
               //this page is used for slab
               SetPageSlab(page);
               page = NEXT_PAGE(page);
	}while(--order);

	kmem_cache_init_objs(cachep, slabp, ctor_flags);

	//initialize all objects
	spin_lock_irqsave(&cachep->spinlock, save_flags);
	cachep->growing--;		//TODO why?

	list_add_before(&cachep->slabs_partial, &slabp->list);
	STATS_INC_GROWN(cachep);	//TODO not defined
	
	spin_lock_irqrestore(&cachep->spinlock, save_flags);
	
	return 1;

opps1:
	kmem_freepages(cachep,  objp);
failed:
	spin_lock_irqsave(&cachep->spinlock, save_flags);
	cachep->growing--;
	spin_lock_irqrestore(&cachep->spinlock, save_flags);
	return 0;
}

/*kmem_find_general_cachep: **off slab**, find the appropriate size cache to use and will be  
 *@size: the size of the slab descriptor
*/
kmem_cache_t *kmem_find_general_cachep(size_t size){
	cache_size_t *csizep = cache_sizes;

	for(; csizep->cs_size; csizep++){
		if(size > csizep->cs_size)
			continue;
		break;
	}
	return csizep->cs_cachep;
}
//----------------------------------------------
//					obj section
//----------------------------------------------

/*kmem_cache_init_objs: initiate all the objs in a slab
 *@cachep: the obj needed to init in
 *@slabp: same as above
 *@ctor_flags: TODO 
 */
static inline void kmem_cache_init_objs(kmem_cache_t *cachep,
		slab_t *slabp, uint32_t ctor_flags)
{
	int i;
	for(i = 0;	i < cachep->num; i++){
	//count the addr of the obj
	void* obj = slabp->s_mem+cachep->objsize*i;

	//call the constructor if available
	if(cachep->ctor)
		cachep->ctor(objp, cachep, ctor_flags);

	slab_bufctl(slabp)[i] = i+1;
	}

	slab_bufctl[i-1] = BUFCTL_END;
	slabp->free = 0;
}
/* kmem_cache_alloc_one_tail: allocate one obj from a slab
 * @cachep: the cache obj alloc from
 * @slabp: the slab obj alloc from
 */
static inline void *kmem_cache_alloc_one_tail(kmem_cache_t *cachep,
			slab_t *slabp)
{
	void* objp;
	
	//set objp point to the first free obj in the slab
	//update the new num of used obj(free)
	slabp->inuse++;
	objp = slabp->s_mem + slabp->free*cachep->objsize;
	slabp->free = slab_bufctl(slabp)[slabp->free];

	//if free exceeds the end mark, move it from the partial list to the full
	if(unlikely(slabp->free == BUFCTL_END)){
		list_del(&slabp->list);
		list_add(&cachep->slabs_full, &slabp->list);
	}
	return objp;
}

/* kmem_cache_alloc_one: alloc a new slab from the list, 
 *  defined as a macro rather a inline function
 * @cachep: the cache slab allocate from
 */
#define kmem_cache_alloc_one(cachep)			\
({												\
	struct list_head *slabs_partial, *entry;	\
 	slab_t *slabp;								\
												\
 	slabs_partial = &(cachep)->slabs_partial;	\
 	entry = slabs_partial->next;				\
 	if(unlikely(entry == slabs_partial)){		\
 		struct list_head *slabs_free;			\
 		slabs_free = &(cachep)->slabs_free;		\
 		entry = slabs_free->next;				\
		if(unlikely(entry == slabs_free))		\
 			goto alloc_new_slab;				\
 		list_del(entry);						\
 		list_add(slabs_partial, entry);			\
 	}											\
												\
 	slabp = list_str(entry, slab_t, list);		\
 	kmem_cache_alloc_one_tail(cachep, slabp);	\
})
/*__kmem_cache_alloc: main function to alloc object
 *@cachep: cache to allocate from
 *flags: flags need to specify
 */
//TODO here is somehow really a mess
static inline void *__kmem_cache_alloc(kmem_cache_t *cachep, uint32_t flags)
{
	uint32_t save_flags;
	void* objp;

	//XXX for dma use check
	//kmem_cache_alloc_head(cachep, flags);

try_again:
	local_intr_save(save_flags);	//disable interrupt and save flags

	objp = kmem_cache_alloc_one(cachep);

	local_intr_restore(save_flags);
	return objp;
alloc_new_slab:

	local_irq_restore(save_flags);
	if(kmem_cache_grow(cachep, flags)
			goto try_again;
	return NULL;
}
void BUG(char* bug_location, char* bug_info){
    kprintf("%s:%s\n", bug_location, bug_info);
    assert(0);
}
