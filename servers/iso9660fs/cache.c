/* The file system maintains a buffer cache to reduce the number of disk
 * accesses needed.  Whenever a read or write to the disk is done, a check is
 * first made to see if the block is in the cache.  This file manages the
 * cache.
 *
 * The entry points into this file are:
 *   get_block:	  request to fetch a block for reading or writing from cache
 *   put_block:	  return a block previously requested with get_block
 *
 * Private functions:
 *   read_block:  read physically the block
 */

#include "inc.h"
#include <minix/com.h>
#include <minix/bdev.h>
#include "buf.h"

static int read_block(struct buf *);

struct buf *bp_to_pickup = buf; /* This is a pointer to the next node in the
					  * buffer cache to pick up*/


/*===========================================================================*
 *				get_block				     *
 *===========================================================================*/
struct buf *get_block(block)
register block_t block;		/* which block is wanted? */
{
  register struct buf *bp, *free_bp;

  free_bp = NULL;

  /* Find if the block is already loaded */
  for (bp = &buf[0]; bp < &buf[NR_BUFS]; bp++)
    if (bp->b_blocknr == block) {
      /* Block found. Increment count and return it  */
      bp->b_count++;
      return bp;
    } else
      if (bp == bp_to_pickup) {
	if (bp->b_count == 0)
	  free_bp = bp;
	else			/* Increment the node to pickup */
	  if (bp_to_pickup < &buf[NR_BUFS] - 1)
	    bp_to_pickup++;
	  else
	    bp_to_pickup = buf;
      }

  if (free_bp == NULL &&
      bp_to_pickup == buf &&
      bp_to_pickup->b_count == 0)
    free_bp = bp_to_pickup;

  if (free_bp != NULL) {
    /* Set fields of data structure */
    free_bp->b_blocknr = block;
    if (read_block(free_bp) != OK) return NULL;
    free_bp->b_count = 1;

    if (bp_to_pickup < &buf[NR_BUFS] - 1)
      bp_to_pickup++;
    else
      bp_to_pickup = buf;

    return free_bp;
  } else {
    /* No free blocks. Return NULL */
    return NULL;
  }
}

/*===========================================================================*
 *				put_block				     *
 *===========================================================================*/
void put_block(bp)
register struct buf *bp;	/* pointer to the buffer to be released */
{
  if (bp == NULL) return;	/* it is easier to check here than in caller */

  bp->b_count--;		/* there is one use fewer now */
}

/*===========================================================================*
 *				read_block				     *
 *===========================================================================*/
static int read_block(bp)
register struct buf *bp;	/* buffer pointer */
{
  int r;
  u64_t pos;
  vir_bytes block_size;

  block_size = v_pri.logical_block_size_l; /* The block size is indicated by
					    * the superblock */


  pos = mul64u(bp->b_blocknr, block_size); /* get absolute position */
  r = bdev_read(fs_dev, pos, bp->b_data, block_size, BDEV_NOFLAGS);
  if (r != (ssize_t) block_size) {
    if (r == OK) r = END_OF_FILE;
    else printf("ISOFS(%d) I/O error on device %d/%d, block %u\n",
	     SELF_E, (fs_dev>>MAJOR)&BYTE, (fs_dev>>MINOR)&BYTE,
	     bp->b_blocknr);

    rdwt_err = r;
    return EINVAL;
  }    

  return OK;
}
