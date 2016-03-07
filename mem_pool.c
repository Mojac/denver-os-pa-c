/*
 * Created by Ivo Georgiev on 2/9/16.
 * Edited by Michael Palme on 3/6/16.
 */

#include <stdlib.h>
#include <assert.h>
#include <stdio.h> // for perror()

#include "mem_pool.h"

/*************/
/*           */
/* Constants */
/*           */
/*************/
static const float      MEM_FILL_FACTOR                 = 0.75;
static const unsigned   MEM_EXPAND_FACTOR               = 2;

static const unsigned   MEM_POOL_STORE_INIT_CAPACITY    = 20;
static const float      MEM_POOL_STORE_FILL_FACTOR      = 0.75;
static const unsigned   MEM_POOL_STORE_EXPAND_FACTOR    = 2;

static const unsigned   MEM_NODE_HEAP_INIT_CAPACITY     = 40;
static const float      MEM_NODE_HEAP_FILL_FACTOR       = 0.75;
static const unsigned   MEM_NODE_HEAP_EXPAND_FACTOR     = 2;

static const unsigned   MEM_GAP_IX_INIT_CAPACITY        = 40;
static const float      MEM_GAP_IX_FILL_FACTOR          = 0.75;
static const unsigned   MEM_GAP_IX_EXPAND_FACTOR        = 2;



/*********************/
/*                   */
/* Type declarations */
/*                   */
/*********************/
typedef struct _node
{
    alloc_t alloc_record;
    unsigned used;
    unsigned allocated;
    struct _node *next, *prev; // doubly-linked list for gap deletion
} node_t, *node_pt;

typedef struct _gap
{
    size_t size;
    node_pt node;
} gap_t, *gap_pt;

typedef struct _pool_mgr
{
    pool_t pool;
    node_pt node_heap;
    unsigned total_nodes;
    unsigned used_nodes;
    gap_pt gap_ix;
    unsigned gap_ix_capacity;
} pool_mgr_t, *pool_mgr_pt;



/***************************/
/*                         */
/* Static global variables */
/*                         */
/***************************/
static pool_mgr_pt *pool_store = NULL; // an array of pointers, only expand
static unsigned pool_store_size = 0;
static unsigned pool_store_capacity = 0;



/********************************************/
/*                                          */
/* Forward declarations of static functions */
/*                                          */
/********************************************/
static alloc_status _mem_resize_pool_store();
static alloc_status _mem_resize_node_heap(pool_mgr_pt pool_mgr);
static alloc_status _mem_resize_gap_ix(pool_mgr_pt pool_mgr);
static alloc_status
        _mem_add_to_gap_ix(pool_mgr_pt pool_mgr,
                           size_t size,
                           node_pt node);
static alloc_status
        _mem_remove_from_gap_ix(pool_mgr_pt pool_mgr,
                                size_t size,
                                node_pt node);
static alloc_status _mem_sort_gap_ix(pool_mgr_pt pool_mgr);



/****************************************/
/*                                      */
/* Definitions of user-facing functions */
/*                                      */
/****************************************/
alloc_status mem_init()
{
    if(pool_store == NULL)
    {// allocate the pool store with initial capacity
        pool_store = calloc(MEM_POOL_STORE_INIT_CAPACITY, sizeof(pool_mgr_pt));
        pool_store_size = 0;
        pool_store_capacity = MEM_POOL_STORE_INIT_CAPACITY;
        return ALLOC_OK;
    }
    else
    {// ensure that it's called only once until mem_free
        return ALLOC_FAIL;
    }
}//End mem_init

alloc_status mem_free()
{
    if(pool_store == NULL)
    {// ensure that it's called only once for each mem_init
        return ALLOC_FAIL;
    }


    for(int dealloc = 0; dealloc < pool_store_size; dealloc++)
    {// make sure all pool managers have been deallocated
        if(pool_store[dealloc] != NULL)
        {// can free the pool store array
            mem_pool_close((pool_pt) pool_store[dealloc]);
        }
    }


    // update static variables
    free(pool_store);
    pool_store = NULL;
    pool_store_size = 0;
    pool_store_capacity = 0;
    return ALLOC_OK;

}//End mem_free

pool_pt mem_pool_open(size_t size, alloc_policy policy)
{
    if(pool_store == NULL)
    {// make sure there the pool store is allocated
        return NULL;
    }

    // expand the pool store, if necessary
    _mem_resize_pool_store();

    // allocate a new mem pool mgr
    pool_mgr_t *pool_manager;
    pool_manager = calloc(1, sizeof(pool_mgr_t));

    if(pool_manager == NULL)
    {// check success, on error return null
        return NULL;
    }

    // allocate a new memory pool
    (*pool_manager).pool.mem = malloc(size);

    if((*pool_manager).pool.mem == NULL)
    {// check success, on error deallocate mgr and return null
        free(pool_manager);
        return NULL;
    }

    // allocate a new node heap
    (*pool_manager).node_heap = calloc(MEM_NODE_HEAP_INIT_CAPACITY, sizeof(node_t));
    (*pool_manager).total_nodes = MEM_NODE_HEAP_INIT_CAPACITY;

    if((*pool_manager).node_heap == NULL)
    {// check success, on error deallocate mgr/pool and return null
        free((*pool_manager).pool.mem);
        free(pool_manager);
        return NULL;
    }

    // allocate a new gap index
    (*pool_manager).gap_ix = calloc(MEM_GAP_IX_INIT_CAPACITY, sizeof(gap_t));
    (*pool_manager).gap_ix_capacity = MEM_GAP_IX_INIT_CAPACITY;

    if((*pool_manager).gap_ix == NULL)
    {// check success, on error deallocate mgr/pool/heap and return null
        free((*pool_manager).pool.mem);
        free((*pool_manager).node_heap);
        free(pool_manager);
        return NULL;
    }

    // assign all the pointers and update meta data:
    //   initialize top node of node heap
    (*pool_manager).pool.total_size = size;
    (*pool_manager).pool.alloc_size = 0;
    (*pool_manager).pool.policy = policy;
    (*pool_manager).pool.num_gaps = 1;
    (*pool_manager).pool.num_allocs = 0;

    //   initialize top node of gap index
    (*pool_manager).node_heap[0].next = NULL;
    (*pool_manager).node_heap[0].prev = NULL;
    (*pool_manager).node_heap[0].allocated = 0;
    (*pool_manager).node_heap[0].used = 1;
    (*pool_manager).node_heap[0].alloc_record.size = (*pool_manager).pool.total_size;
    (*pool_manager).node_heap[0].alloc_record.mem = (*pool_manager).pool.mem;
    (*pool_manager).used_nodes = 1;

    //   initialize pool mgr
    (*pool_manager).gap_ix[0].node = &(*pool_manager).node_heap[0];

    //   link pool mgr to pool store
    pool_store[pool_store_size] = pool_manager;
    pool_store_size++;

    // return the address of the mgr, cast to (pool_pt)
    return (pool_pt) pool_manager;
}//End mem_pool_open

alloc_status mem_pool_close(pool_pt pool)
{
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    pool_mgr_pt pool_mgr = (pool_mgr_pt) pool;

    if(pool_mgr == NULL)
    {// check if this pool is allocated
        return ALLOC_FAIL;
    }

    for(int parser = 0; parser < (*pool_mgr).gap_ix_capacity; parser++)
    {// check if pool has only one gap
        if((*(*pool_mgr).gap_ix).node != NULL)
        {
            return ALLOC_FAIL;
        }
    }


    // check if it has zero allocations
    if((*pool_mgr).pool.num_allocs != 0)
    {
        return ALLOC_FAIL;
    }

    // free memory pool
    free((*pool).mem);

    // free node heap
    free((*pool_mgr).node_heap);

    // free gap index
    free((*pool_mgr).gap_ix);

    for(int parser = 0; parser < pool_store_capacity; parser++)
    {// find mgr in pool store and set to null
        if(pool_store[parser] == pool_mgr)
        {
            pool_store[parser] = NULL;
        }
    }

    // free mgr
    free(pool_mgr);

    return ALLOC_OK;
}//End mem_pool_close

alloc_pt mem_new_alloc(pool_pt pool, size_t size)
{
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    pool_mgr_pt pool_mgr = (pool_mgr_pt) pool;

    gap_pt good_gap = NULL;
    for(int parser = 0; parser < (*pool_mgr).gap_ix_capacity && good_gap == NULL; parser++)
    {// check if any gaps, return null if none
        if((*(*pool_mgr).gap_ix).size >= size)
        {
            good_gap = (*pool_mgr).gap_ix;
        }
    }
    if(good_gap == NULL)
    {
        return NULL;
    }

    if(_mem_resize_node_heap(pool_mgr) == ALLOC_FAIL)
    {// expand heap node, if necessary, quit on error
        return NULL;
    }

    if((*pool_mgr).used_nodes >= (*pool_mgr).total_nodes)
    {// check used nodes fewer than total nodes, quit on error
        return NULL;
    }

    // get a node for allocation:
    node_pt temp_node = NULL;

    if((*pool).policy == FIRST_FIT)
    {// if FIRST_FIT
        for(node_pt good_node = (*pool_mgr).node_heap; good_node != NULL && temp_node == NULL; good_node = (*good_node).next)
        {//find the first sufficient node in the node heap
            if((*good_node).alloc_record.size >= size && (*good_node).allocated == 0)
            {
                temp_node = good_node;
            }
        }
    }
    else
    {// if BEST_FIT
        for(int parser = 0; parser <= (*pool_mgr).gap_ix_capacity && temp_node == NULL; parser++)
        {//find the first sufficient node in the gap index
            if((*pool_mgr).gap_ix->size >= size)
            {
                temp_node = (*(*pool_mgr).gap_ix).node;
            }
        }
    }

    if(temp_node == NULL)
    {// check if node found
        return NULL;
    }

    // update metadata (num_allocs, alloc_size)
    (*pool).num_allocs++;
    (*pool).alloc_size += size;

    // calculate the size of the remaining gap, if any
    size_t remaining_gap = (*temp_node).alloc_record.size - size;

    // remove node from gap index
    (*good_gap).node = NULL;

    // convert gap_node to an allocation node of given size
    (*temp_node).allocated = 1;
    (*temp_node).alloc_record.size = size;

    // adjust node heap:
    //   if remaining gap, need a new node
    node_pt unused_node = NULL;

    if(remaining_gap > 0)
    {
        for(node_pt parse_node = (*pool_mgr).node_heap; parse_node != NULL && unused_node == NULL; parse_node = (*parse_node).next)
        {//   find an unused one in the node heap
            if((*parse_node).allocated == 0)
            {
                unused_node = parse_node;
            }
        }

        if(unused_node == NULL)
        {//   make sure one was found
            return NULL;
        }

        //   initialize it to a gap node
        (*good_gap).node = unused_node;
        (*unused_node).allocated = 0;
        (*unused_node).used = 1;
        (*unused_node).alloc_record.size = remaining_gap;
        (*unused_node).alloc_record.mem = (*temp_node).alloc_record.mem + size;

        //   update metadata (used_nodes)
        (*pool_mgr).used_nodes++;
    }

    //   update linked list (new node right after the node for allocation)

    //   add to gap index
    _mem_add_to_gap_ix(pool_mgr, remaining_gap, temp_node);

    //   check if successful
    if(temp_node == NULL)
    {
        return NULL;
    }

    // return allocation recorpd by casting the node to (alloc_pt)
    return (alloc_pt) temp_node;
}//End mem_new_alloc

alloc_status mem_del_alloc(pool_pt pool, alloc_pt alloc)
{
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    // get node from alloc by casting the pointer to (node_pt)
    // find the node in the node heap
    // this is node-to-delete
    // make sure it's found
    // convert to gap node
    // update metadata (num_allocs, alloc_size)
    // if the next node in the list is also a gap, merge into node-to-delete
    //   remove the next node from gap index
    //   check success
    //   add the size to the node-to-delete
    //   update node as unused
    //   update metadata (used nodes)
    //   update linked list:
    /*
                    if (next->next) {
                        next->next->prev = node_to_del;
                        node_to_del->next = next->next;
                    } else {
                        node_to_del->next = NULL;
                    }
                    next->next = NULL;
                    next->prev = NULL;
     */

    // this merged node-to-delete might need to be added to the gap index
    // but one more thing to check...
    // if the previous node in the list is also a gap, merge into previous!
    //   remove the previous node from gap index
    //   check success
    //   add the size of node-to-delete to the previous
    //   update node-to-delete as unused
    //   update metadata (used_nodes)
    //   update linked list
    /*
                    if (node_to_del->next) {
                        prev->next = node_to_del->next;
                        node_to_del->next->prev = prev;
                    } else {
                        prev->next = NULL;
                    }
                    node_to_del->next = NULL;
                    node_to_del->prev = NULL;
     */
    //   change the node to add to the previous node!
    // add the resulting node to the gap index
    // check success

    return ALLOC_FAIL;
}//End mem_del_alloc

void mem_inspect_pool(pool_pt pool,
                      pool_segment_pt *segments,
                      unsigned *num_segments)
{
    // get the mgr from the pool
    // allocate the segments array with size == used_nodes
    // check successful
    // loop through the node heap and the segments array
    //    for each node, write the size and allocated in the segment
    // "return" the values:
    /*
                    *segments = segs;
                    *num_segments = pool_mgr->used_nodes;
     */
}//End mem_inspect_pool



/***********************************/
/*                                 */
/* Definitions of static functions */
/*                                 */
/***********************************/
static alloc_status _mem_resize_pool_store()
{
    // check if necessary
    /*
                if (((float) pool_store_size / pool_store_capacity)
                    > MEM_POOL_STORE_FILL_FACTOR) {...}
     */
    // don't forget to update capacity variables

    return ALLOC_FAIL;
}//End _mem_resize_pool_store

static alloc_status _mem_resize_node_heap(pool_mgr_pt pool_mgr)
{
    // see above

    return ALLOC_FAIL;
}//End _mem_resize_node_heap

static alloc_status _mem_resize_gap_ix(pool_mgr_pt pool_mgr)
{
    // see above

    return ALLOC_FAIL;
}//End _mem_resize_gap_ix

static alloc_status _mem_add_to_gap_ix(pool_mgr_pt pool_mgr,
                                       size_t size,
                                       node_pt node)
{

    // expand the gap index, if necessary (call the function)
    // add the entry at the end
    // update metadata (num_gaps)
    // sort the gap index (call the function)
    // check success

    return ALLOC_FAIL;
}//End _mem_add_to_gap_ix

static alloc_status _mem_remove_from_gap_ix(pool_mgr_pt pool_mgr,
                                            size_t size,
                                            node_pt node)
{
    // find the position of the node in the gap index
    // loop from there to the end of the array:
    //    pull the entries (i.e. copy over) one position up
    //    this effectively deletes the chosen node
    // update metadata (num_gaps)
    // zero out the element at position num_gaps!

    return ALLOC_FAIL;
}//End _mem_remove_from_gap_ix

// note: only called by _mem_add_to_gap_ix, which appends a single entry
static alloc_status _mem_sort_gap_ix(pool_mgr_pt pool_mgr)
{
    // the new entry is at the end, so "bubble it up"
    // loop from num_gaps - 1 until but not including 0:
    //    if the size of the current entry is less than the previous (u - 1)
    //    or if the sizes are the same but the current entry points to a
    //    node with a lower address of pool allocation address (mem)
    //       swap them (by copying) (remember to use a temporary variable)

    return ALLOC_FAIL;
}//End _mem_sort_gap_ix


