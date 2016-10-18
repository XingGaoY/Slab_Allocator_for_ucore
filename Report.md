# Draft
## Goals
* Complete all the eight lab_codes and review the books;
* Finish the Slab Allocator for Ucore+;
* Apply slab to one or two modules of Ucore+;

## Finished Work
* The first three labs
* Review of Bonwick's paper and some linux implementation
* Finished the create part of slab and debugging

## Previous Slab
Just an allocator for objects smaller than one page or
composed with many pages, without consider different object.

Slabs are the linklists of same size objects. And the only
api are kmalloc and kfree. No buffer is set for later allocating.

No reaping mechanism, if memory is used up, the allocator has no way 
to destroy slabs.

## Need to Do
* Because a lot of functions are using kmalloc and kfree. If I 
want to merge my slab to master tree, I need to test carefully 
in order not to cause any future troubles.

* Better implementation. I checked two version of linux(2.6 & 4.8), 
data structures are a lot different. I want to make this allocator 
faster and more rubust, so details are important, and linux is a good 
way to learn how to deal with details.

* Labs. I'm also carrying on completing lab_codes.