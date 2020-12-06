#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>
#include "threads/thread.h"
#include "threads/synch.h"

struct page;

/* Frame table entry. */
struct frame
  {
    void *kpage;   /* Kernel virtual page mapped to a frame. */
    struct thread *__owner;
    struct page *page;
    struct lock lock;
    bool pinned;
    struct list_elem list_elem;
  };

void frame_init (void);
struct frame *frame_alloc (struct page *);
void frame_free (struct frame *);
struct frame *frame_lookup (void *);

void frame_table_lock (void);
void frame_table_unlock (void);

void frame_lock_acquire (struct frame *);
void frame_lock_release (struct frame *);
bool frame_lock_try_acquire (struct frame *);

bool frame_try_pin (struct frame *);
void frame_unpin (struct frame *);

#endif /* vm/frame.h */