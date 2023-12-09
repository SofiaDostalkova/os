// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct {

  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf buckets[NBUCKET];
  struct spinlock lks[NBUCKET];
} bcache;

static int
myhash(int x){
  return x%NBUCKET;
}
static void
bufinit(struct buf* b, uint dev, uint blockno)
{
  b->dev = dev;
  b->blockno = blockno;
  b->valid = 0;
  b->refcnt = 1;
} 

void
binit(void)
{
  struct buf *b;
  for (int i = 0; i<NBUCKET; i++)
  initlock(&bcache.lks[i], "bcache");

  // Create linked list of buffers
  for(int i=0;i<NBUCKET; i++){
  bcache.buckets[i].prev = &bcache.buckets[i];
  bcache.buckets[i].next = &bcache.buckets[i];
  }

  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.buckets[0].next;
    b->prev = &bcache.buckets[0];
    initsleeplock(&b->lock, "buffer");
    bcache.buckets[0].next->prev = b;
    bcache.buckets[0].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  int id = myhash(blockno);

  acquire(&bcache.lks[id]);

  // Is the block already cached?
  for(b = bcache.buckets[id].next; b != &bcache.buckets[id]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lks[id]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  //check for a buffer not in the cache. in this case,
  //employ the LRU strat based on the ticks
  //select the buffer with minimum ticks for eviction from the
  //id-th hash bucket.  minimum ticks imply that the buffer
  //is the least recently used among many buffers (b->refcnt==0)
  //that have not been accessed recently
  //
  //
  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
   struct buf *victm = 0;
   uint minticks = ticks;
  for(b = bcache.buckets[id].next; b != &bcache.buckets[id]; b = b->next){
    if(b->refcnt == 0 && b->lastuse<=minticks) {
      minticks = b->lastuse;
      victm =  b;
    }
  }

  if(!victm)
    goto steal;

  //directly overwrite the buffer to be evicted, without the need to write back
  //its old content to the disk. set the valid flag to 0  to ensure reading
  //the latest data
  bufinit(victm,dev,blockno);

  release(&bcache.lks[id]);
  acquiresleep(&victm->lock);
  return victm;

steal:
  //dig a buffer from another hash bucket
  for(int i=0; i<NBUCKET; i++){
  if(i==id)
    continue;
  acquire(&bcache.lks[i]);
  minticks = ticks;
  for(b = bcache.buckets[i].next; b != &bcache.buckets[i]; b = b->next){
    if(b->refcnt == 0 && b->lastuse<=minticks){
      minticks = b->lastuse;
      victm = b;
    }
  }
  if (!victm){
    release(&bcache.lks[i]);
    continue;
  }

  bufinit(victm,dev,blockno);
  
  //take victm out of the i-th hash bucket
  victm->next->prev = victm->prev;
  victm->prev->next = victm->next;
  release(&bcache.lks[i]);

  //attach victm to the id-th  bucket
  victm->next = bcache.buckets[id].next;
  bcache.buckets[id].next->prev = victm;
  bcache.buckets[id].next = victm;
  victm->prev = &bcache.buckets[id];

  release(&bcache.lks[id]);
  acquiresleep(&victm->lock);
  return victm;
  }

  release(&bcache.lks[id]);
  panic("bget: no buf");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  //if data in the buffer buf is outdated, it needs to be reread
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int id = myhash(b->blockno);

  acquire(&bcache.lks[id]);
  b->refcnt--;
  if (b->refcnt == 0) {
    b->lastuse = ticks;
    // no one is waiting for it.
  //  b->next->prev = b->prev;
   // b->prev->next = b->next;
   // b->next = bcache.head.next;
   // b->prev = &bcache.head;
   // bcache.head.next->prev = b;
   // bcache.head.next = b;
  }
  
  release(&bcache.lks[id]);
}

void
bpin(struct buf *b) {
  int id = myhash(b-> blockno);
  acquire(&bcache.lks[id]);
  b->refcnt++;
  release(&bcache.lks[id]);
}

void
bunpin(struct buf *b) {
  int id = myhash(b->blockno);
  acquire(&bcache.lks[id]);
  b->refcnt--;
  release(&bcache.lks[id]);
}


