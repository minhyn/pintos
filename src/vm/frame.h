#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>
#include "threads/thread.h"
#include "threads/palloc.h"
#include "threads/synch.h"

struct page;

/* Frame table entry. */
struct frame
  {
    void *kpage;   /* Kernel virtual page mapped to a frame. */
    struct thread *owner;
    struct page *suppl;
    struct lock lock;
    struct list_elem list_elem;
  };

void frame_init (void);
void *frame_alloc (enum palloc_flags, struct page *);
void frame_free (void *);
void frame_free_all (void);
struct frame *frame_lookup (void *);
struct frame *frame_get_victim (void);

void frame_table_lock (void);
void frame_table_unlock (void);
void frame_unlock (void *);

#endif /* vm/frame.h */