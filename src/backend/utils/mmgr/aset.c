/*-------------------------------------------------------------------------
 *
 * aset.c
 *	  Allocation set definitions.
 *
 * AllocSet is our standard implementation of the abstract MemoryContext
 * type.
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/aset.c
 *
 * NOTE:
 *	This is a new (Feb. 05, 1999) implementation of the allocation set
 *	routines. AllocSet...() does not use OrderedSet...() any more.
 *	Instead it manages allocations in a block pool by itself, combining
 *	many small allocations in a few bigger blocks. AllocSetFree() normally
 *	doesn't free() memory really. It just add's the free'd area to some
 *	list for later reuse by AllocSetAlloc(). All memory blocks are free()'d
 *	at once on AllocSetReset(), which happens when the memory context gets
 *	destroyed.
 *				Jan Wieck
 *
 *	Performance improvement from Tom Lane, 8/99: for extremely large request
 *	sizes, we do want to be able to give the memory back to free() as soon
 *	as it is pfree()'d.  Otherwise we risk tying up a lot of memory in
 *	freelist entries that might never be usable.  This is specially needed
 *	when the caller is repeatedly repalloc()'ing a block bigger and bigger;
 *	the previous instances of the block were guaranteed to be wasted until
 *	AllocSetReset() under the old way.
 *
 *	Further improvement 12/00: as the code stood, request sizes in the
 *	midrange between "small" and "large" were handled very inefficiently,
 *	because any sufficiently large free chunk would be used to satisfy a
 *	request, even if it was much larger than necessary.  This led to more
 *	and more wasted space in allocated chunks over time.  To fix, get rid
 *	of the midrange behavior: we now handle only "small" power-of-2-size
 *	chunks as chunks.  Anything "large" is passed off to malloc().  Change
 *	the number of freelists to change the small/large boundary.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/memdebug.h"
#include "utils/memutils.h"

/* Define this to detail debug alloc information */
/* #define HAVE_ALLOCINFO */

/*--------------------
 * Chunk freelist k holds chunks of size 1 << (k + ALLOC_MINBITS),
 * for k = 0 .. ALLOCSET_NUM_FREELISTS-1.
 *
 * Note that all chunks in the freelists have power-of-2 sizes.  This
 * improves recyclability: we may waste some space, but the wasted space
 * should stay pretty constant as requests are made and released.
 *
 * A request too large for the last freelist is handled by allocating a
 * dedicated block from malloc().  The block still has a block header and
 * chunk header, but when the chunk is freed we'll return the whole block
 * to malloc(), not put it on our freelists.
 *
 * CAUTION: ALLOC_MINBITS must be large enough so that
 * 1<<ALLOC_MINBITS is at least MAXALIGN,
 * or we may fail to align the smallest chunks adequately.
 * 8-byte alignment is enough on all currently known machines.
 *
 * With the current parameters, request sizes up to 8K are treated as chunks,
 * larger requests go into dedicated blocks.  Change ALLOCSET_NUM_FREELISTS
 * to adjust the boundary point; and adjust ALLOCSET_SEPARATE_THRESHOLD in
 * memutils.h to agree.  (Note: in contexts with small maxBlockSize, we may
 * set the allocChunkLimit to less than 8K, so as to avoid space wastage.)
 *--------------------
 */

#define ALLOC_MINBITS		3	/* smallest chunk size is 8 bytes */
#define ALLOCSET_NUM_FREELISTS	11
#define ALLOC_CHUNK_LIMIT	(1 << (ALLOCSET_NUM_FREELISTS-1+ALLOC_MINBITS))
/* Size of largest chunk that we use a fixed size for */
#define ALLOC_CHUNK_FRACTION	4
/* We allow chunks to be at most 1/4 of maxBlockSize (less overhead) */

/*--------------------
 * The first block allocated for an allocset has size initBlockSize.
 * Each time we have to allocate another block, we double the block size
 * (if possible, and without exceeding maxBlockSize), so as to reduce
 * the bookkeeping load on malloc().
 *
 * Blocks allocated to hold oversize chunks do not follow this rule, however;
 * they are just however big they need to be to hold that single chunk.
 *--------------------
 */

#define ALLOC_BLOCKHDRSZ	MAXALIGN(sizeof(AllocBlockData))
#define ALLOC_CHUNKHDRSZ	sizeof(struct AllocChunkData)

typedef struct AllocBlockData *AllocBlock;	/* forward reference */
typedef struct AllocChunkData *AllocChunk;

/*
 * AllocPointer
 *		Aligned pointer which may be a member of an allocation set.
 */
typedef void *AllocPointer;

/*
 * AllocSetContext is our standard implementation of MemoryContext.
 *
 * Note: header.isReset means there is nothing for AllocSetReset to do.
 * This is different from the aset being physically empty (empty blocks list)
 * because we may still have a keeper block.  It's also different from the set
 * being logically empty, because we don't attempt to detect pfree'ing the
 * last active chunk.
 */
// 可以参考 https://zhuanlan.zhihu.com/p/350049053
typedef struct AllocSetContext
{
	// 对应该内存上下文的头部信息
	MemoryContextData header;	/* Standard memory-context fields */
	/* Info about storage allocated in this context: */
	// 内存块链表，记录内存上下文向操作系统申请的连续大块内存
	AllocBlock	blocks;			/* head of list of blocks in this set */
	// 该内存上下文中空闲内存片的数组，组织该内存上下文里所有内存块中已释放的内存片的链表结构
	AllocChunk	freelist[ALLOCSET_NUM_FREELISTS];	/* free chunk lists */
	/* Allocation parameters for this context: */
	// 初始内存块的大小  initBlockSize <= nextBlockSize <= maxBlockSize
	Size		initBlockSize;	/* initial block size */
	// 最大内存块大小
	Size		maxBlockSize;	/* maximum block size */
	// 下一个要分配的内存块的大小，分配一个新的内存块是采用
	Size		nextBlockSize;	/* next block size to allocate */
	// 分配内存片的尺寸阈值，在分配内存块是用到,该值有效时，是为了运行是替换 ALLOC_CHUNK_LIMIT 这个宏
	// 申请的内存超过此阀值直接分配新的block
	Size		allocChunkLimit;	/* effective chunk size limit */
	/* keeper 中的内存块在内存上下文重置时保留不被释放, 为防止一些需要频繁重置的小的内存
	 * 上下文重复的进行malloc，重置时保留第一次申请的内存块 
	 */
	AllocBlock	keeper;			/* if not NULL, keep this block over resets */
} AllocSetContext;

typedef AllocSetContext *AllocSet;

/*
 * AllocBlock
 *		An AllocBlock is the unit of memory that is obtained by aset.c
 *		from malloc().  It contains one or more AllocChunks, which are
 *		the units requested by palloc() and freed by pfree().  AllocChunks
 *		cannot be returned to malloc() individually, instead they are put
 *		on freelists by pfree() and re-used by the next palloc() that has
 *		a matching request size.
 *
 *		AllocBlockData is the header data for a block --- the usable space
 *		within the block begins at the next alignment boundary.
 */
// AllocBlockData是进程用来分配、释放的内存单元，通过malloc申请
typedef struct AllocBlockData // AllocSet 管理内存中对应的 内存块
{
	// 指向AllocBlockData所属的AllocSetContext
	AllocSet	aset;			/* aset that owns this block */
	// 在AllocSetContext中的blocks prev block
	AllocBlock	prev;			/* prev block in aset's blocks list, if any */
	// 在AllocSetContext中的blocks next block
	AllocBlock	next;			/* next block in aset's blocks list, if any */
	// 可用空闲区域的起始地址
	char	   *freeptr;		/* start of free space in this block */
	// 可用空闲区域的结束地址
	char	   *endptr;			/* end of space in this block */
}			AllocBlockData;

/*
 * AllocChunk
 *		The prefix of each piece of memory in an AllocBlock
 */
/*
	AllocChunkData是用户存放数据的内存单元，通过palloc申请，其中已经使用
	的AllocChunkData的aset将会指向所属的AllocSetContextData；如果是空
	闲未使用的将会指向freelist中的下一个成员变量（没有则为空）。
	一个AllocBlockData具有一个或多个AllocChunkData
*/
typedef struct AllocChunkData
{
	/* size is always the size of the usable space in the chunk */
	// 当前AllocChunkData的长度
	Size		size;
#ifdef MEMORY_CONTEXT_CHECKING
	/* when debugging memory usage, also store actual requested size */
	/* this is zero in a free chunk */
	// 在debugging模式下会记录实际请求的长度
	Size		requested_size;
#if MAXIMUM_ALIGNOF > 4 && SIZEOF_VOID_P == 4
	// 对齐时使用 
	Size		padding;
#endif

#endif							/* MEMORY_CONTEXT_CHECKING */

	/* aset is the owning aset if allocated, or the freelist link if free */
	// 当此AllocChunkData正在使用时，指向所属的AllocSetContext；未使用则指向下
	// 一个相同大小的空闲AllocChunkData（没有则为空）
	void	   *aset;

	/* there must not be any padding to reach a MAXALIGN boundary here! */
}			AllocChunkData;

/*
 * AllocPointerIsValid
 *		True iff pointer is valid allocation pointer.
 */
#define AllocPointerIsValid(pointer) PointerIsValid(pointer)

/*
 * AllocSetIsValid
 *		True iff set is valid allocation set.
 */
#define AllocSetIsValid(set) PointerIsValid(set)

#define AllocPointerGetChunk(ptr)	\
					((AllocChunk)(((char *)(ptr)) - ALLOC_CHUNKHDRSZ))
#define AllocChunkGetPointer(chk)	\
					((AllocPointer)(((char *)(chk)) + ALLOC_CHUNKHDRSZ))

/*
 * These functions implement the MemoryContext API for AllocSet contexts.
 */
static void *AllocSetAlloc(MemoryContext context, Size size);
static void AllocSetFree(MemoryContext context, void *pointer);
static void *AllocSetRealloc(MemoryContext context, void *pointer, Size size);
static void AllocSetInit(MemoryContext context);
static void AllocSetReset(MemoryContext context);
static void AllocSetDelete(MemoryContext context);
static Size AllocSetGetChunkSpace(MemoryContext context, void *pointer);
static bool AllocSetIsEmpty(MemoryContext context);
static void AllocSetStats(MemoryContext context, int level, bool print,
			  MemoryContextCounters *totals);

#ifdef MEMORY_CONTEXT_CHECKING
static void AllocSetCheck(MemoryContext context);
#endif

/*
 * This is the virtual function table for AllocSet contexts.
 */
static MemoryContextMethods AllocSetMethods = {
	AllocSetAlloc,
	AllocSetFree,
	AllocSetRealloc,
	AllocSetInit,
	AllocSetReset,
	AllocSetDelete,
	AllocSetGetChunkSpace,
	AllocSetIsEmpty,
	AllocSetStats
#ifdef MEMORY_CONTEXT_CHECKING
	,AllocSetCheck
#endif
};

/*
 * Table for AllocSetFreeIndex
 */
#define LT16(n) n, n, n, n, n, n, n, n, n, n, n, n, n, n, n, n

static const unsigned char LogTable256[256] =
{
	0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4,
	LT16(5), LT16(6), LT16(6), LT16(7), LT16(7), LT16(7), LT16(7),
	LT16(8), LT16(8), LT16(8), LT16(8), LT16(8), LT16(8), LT16(8), LT16(8)
};

/* ----------
 * Debug macros
 * ----------
 */
#ifdef HAVE_ALLOCINFO
#define AllocFreeInfo(_cxt, _chunk) \
			fprintf(stderr, "AllocFree: %s: %p, %zu\n", \
				(_cxt)->header.name, (_chunk), (_chunk)->size)
#define AllocAllocInfo(_cxt, _chunk) \
			fprintf(stderr, "AllocAlloc: %s: %p, %zu\n", \
				(_cxt)->header.name, (_chunk), (_chunk)->size)
#else
#define AllocFreeInfo(_cxt, _chunk)
#define AllocAllocInfo(_cxt, _chunk)
#endif

/* ----------
 * AllocSetFreeIndex -
 *
 *		Depending on the size of an allocation compute which freechunk
 *		list of the alloc set it belongs to.  Caller must have verified
 *		that size <= ALLOC_CHUNK_LIMIT.
 * ----------
 */
static inline int
AllocSetFreeIndex(Size size)
{
	int			idx;
	unsigned int t,
				tsize;

	if (size > (1 << ALLOC_MINBITS))
	{
		tsize = (size - 1) >> ALLOC_MINBITS;

		/*
		 * At this point we need to obtain log2(tsize)+1, ie, the number of
		 * not-all-zero bits at the right.  We used to do this with a
		 * shift-and-count loop, but this function is enough of a hotspot to
		 * justify micro-optimization effort.  The best approach seems to be
		 * to use a lookup table.  Note that this code assumes that
		 * ALLOCSET_NUM_FREELISTS <= 17, since we only cope with two bytes of
		 * the tsize value.
		 */
		t = tsize >> 8;
		idx = t ? LogTable256[t] + 8 : LogTable256[tsize];

		Assert(idx < ALLOCSET_NUM_FREELISTS);
	}
	else
		idx = 0;

	return idx;
}


/*
 * Public routines
 */


/*
 * AllocSetContextCreate
 *		Create a new AllocSet context.
 *
 * parent: parent context, or NULL if top-level context
 * name: name of context (for debugging only, need not be unique)
 * minContextSize: minimum context size
 * initBlockSize: initial allocation block size
 * maxBlockSize: maximum allocation block size
 *
 * Notes: the name string will be copied into context-lifespan storage.
 * Most callers should abstract the context size parameters using a macro
 * such as ALLOCSET_DEFAULT_SIZES.
 */
MemoryContext
AllocSetContextCreate(MemoryContext parent,
					  const char *name,
					  Size minContextSize,
					  Size initBlockSize,
					  Size maxBlockSize)
{
	AllocSet	set;

	StaticAssertStmt(offsetof(AllocChunkData, aset) + sizeof(MemoryContext) ==
					 MAXALIGN(sizeof(AllocChunkData)),
					 "padding calculation in AllocChunkData is wrong");

	/*
	 * First, validate allocation parameters.  (If we're going to throw an
	 * error, we should do so before the context is created, not after.)  We
	 * somewhat arbitrarily enforce a minimum 1K block size.
	 */
	if (initBlockSize != MAXALIGN(initBlockSize) ||
		initBlockSize < 1024)
		elog(ERROR, "invalid initBlockSize for memory context: %zu",
			 initBlockSize);
	if (maxBlockSize != MAXALIGN(maxBlockSize) ||
		maxBlockSize < initBlockSize ||
		!AllocHugeSizeIsValid(maxBlockSize))	/* must be safe to double */
		elog(ERROR, "invalid maxBlockSize for memory context: %zu",
			 maxBlockSize);
	if (minContextSize != 0 &&
		(minContextSize != MAXALIGN(minContextSize) ||
		 minContextSize <= ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ))
		elog(ERROR, "invalid minContextSize for memory context: %zu",
			 minContextSize);

	/* Do the type-independent part of context creation */
	/* TopMemoryContext 不为空的时候，从 TopMemoryContext 分配，
	 * 如果没有 TopMemoryContext 则调用 malloc 构造 TopMemoryContext
	 */
	set = (AllocSet) MemoryContextCreate(T_AllocSetContext,
										 sizeof(AllocSetContext),
										 &AllocSetMethods,
										 parent,
										 name);

	/* Save allocation parameters */
	set->initBlockSize = initBlockSize;
	set->maxBlockSize = maxBlockSize;
	set->nextBlockSize = initBlockSize;

	/*
	 * Compute the allocation chunk size limit for this context.  It can't be
	 * more than ALLOC_CHUNK_LIMIT because of the fixed number of freelists.
	 * If maxBlockSize is small then requests exceeding the maxBlockSize, or
	 * even a significant fraction of it, should be treated as large chunks
	 * too.  For the typical case of maxBlockSize a power of 2, the chunk size
	 * limit will be at most 1/8th maxBlockSize, so that given a stream of
	 * requests that are all the maximum chunk size we will waste at most
	 * 1/8th of the allocated space.
	 *
	 * We have to have allocChunkLimit a power of two, because the requested
	 * and actually-allocated sizes of any chunk must be on the same side of
	 * the limit, else we get confused about whether the chunk is "big".
	 *
	 * Also, allocChunkLimit must not exceed ALLOCSET_SEPARATE_THRESHOLD.
	 */
	StaticAssertStmt(ALLOC_CHUNK_LIMIT == ALLOCSET_SEPARATE_THRESHOLD,
					 "ALLOC_CHUNK_LIMIT != ALLOCSET_SEPARATE_THRESHOLD");

	set->allocChunkLimit = ALLOC_CHUNK_LIMIT;
	while ((Size) (set->allocChunkLimit + ALLOC_CHUNKHDRSZ) >
		   (Size) ((maxBlockSize - ALLOC_BLOCKHDRSZ) / ALLOC_CHUNK_FRACTION))
		set->allocChunkLimit >>= 1;

	/*
	 * Grab always-allocated space, if requested
	 */
	if (minContextSize > 0)
	{
		Size		blksize = minContextSize;
		AllocBlock	block;

		block = (AllocBlock) malloc(blksize);
		if (block == NULL)
		{
			MemoryContextStats(TopMemoryContext);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory"),
					 errdetail("Failed while creating memory context \"%s\".",
							   name)));
		}
		block->aset = set;
		block->freeptr = ((char *) block) + ALLOC_BLOCKHDRSZ;
		block->endptr = ((char *) block) + blksize;
		block->prev = NULL;
		block->next = set->blocks;
		if (block->next)
			block->next->prev = block;
		set->blocks = block;
		/* Mark block as not to be released at reset time */
		set->keeper = block;

		/* Mark unallocated space NOACCESS; leave the block header alone. */
		VALGRIND_MAKE_MEM_NOACCESS(block->freeptr,
								   blksize - ALLOC_BLOCKHDRSZ);
	}

	return (MemoryContext) set;
}

/*
 * AllocSetInit
 *		Context-type-specific initialization routine.
 *
 * This is called by MemoryContextCreate() after setting up the
 * generic MemoryContext fields and before linking the new context
 * into the context tree.  We must do whatever is needed to make the
 * new context minimally valid for deletion.  We must *not* risk
 * failure --- thus, for example, allocating more memory is not cool.
 * (AllocSetContextCreate can allocate memory when it gets control
 * back, however.)
 */
static void
AllocSetInit(MemoryContext context)
{
	/*
	 * Since MemoryContextCreate already zeroed the context node, we don't
	 * have to do anything here: it's already OK.
	 */
}

/*
 * AllocSetReset
 *		Frees all memory which is allocated in the given set.
 *
 * Actually, this routine has some discretion about what to do.
 * It should mark all allocated chunks freed, but it need not necessarily
 * give back all the resources the set owns.  Our actual implementation is
 * that we hang onto any "keeper" block specified for the set.  In this way,
 * we don't thrash malloc() when a context is repeatedly reset after small
 * allocations, which is typical behavior for per-tuple contexts.
 */
static void
AllocSetReset(MemoryContext context)
{
	AllocSet	set = (AllocSet) context;
	AllocBlock	block;

	AssertArg(AllocSetIsValid(set));

#ifdef MEMORY_CONTEXT_CHECKING
	/* Check for corruption and leaks before freeing */
	AllocSetCheck(context);
#endif

	/* Clear chunk freelists */
	MemSetAligned(set->freelist, 0, sizeof(set->freelist));

	block = set->blocks;

	/* New blocks list is either empty or just the keeper block */
	/* 设置blocks指向keeper，reset不对keeper进行释放，只进行重置 */
	set->blocks = set->keeper;

	while (block != NULL)
	{
		AllocBlock	next = block->next;

		if (block == set->keeper)
		{
			/* Reset the block, but don't return it to malloc */
			char	   *datastart = ((char *) block) + ALLOC_BLOCKHDRSZ;

#ifdef CLOBBER_FREED_MEMORY
			wipe_mem(datastart, block->freeptr - datastart);
#else
			/* wipe_mem() would have done this */
			VALGRIND_MAKE_MEM_NOACCESS(datastart, block->freeptr - datastart);
#endif
			block->freeptr = datastart;
			block->prev = NULL;
			block->next = NULL;
		}
		else
		{
			/* Normal case, release the block */
#ifdef CLOBBER_FREED_MEMORY
			wipe_mem(block, block->freeptr - ((char *) block));
#endif
			/* 非keeper，直接释放 */
			free(block);
		}
		block = next;
	}

	/* Reset block size allocation sequence, too */
	set->nextBlockSize = set->initBlockSize;
}

/*
 * AllocSetDelete
 *		Frees all memory which is allocated in the given set,
 *		in preparation for deletion of the set.
 *
 * Unlike AllocSetReset, this *must* free all resources of the set.
 * But note we are not responsible for deleting the context node itself.
 */
static void
AllocSetDelete(MemoryContext context)
{
	AllocSet	set = (AllocSet) context;
	AllocBlock	block = set->blocks;

	AssertArg(AllocSetIsValid(set));

#ifdef MEMORY_CONTEXT_CHECKING
	/* Check for corruption and leaks before freeing */
	AllocSetCheck(context);
#endif

	/* Make it look empty, just in case... */
	MemSetAligned(set->freelist, 0, sizeof(set->freelist));
	set->blocks = NULL;
	set->keeper = NULL;

	/* 如果不是freelist的候选MemoryContext，则直接释放 */
	while (block != NULL)
	{
		AllocBlock	next = block->next;

#ifdef CLOBBER_FREED_MEMORY
		wipe_mem(block, block->freeptr - ((char *) block));
#endif
		free(block);
		block = next;
	}
}

/*
 * AllocSetAlloc
 *		Returns pointer to allocated memory of given size or NULL if
 *		request could not be completed; memory is added to the set.
 *
 * No request may exceed:
 *		MAXALIGN_DOWN(SIZE_MAX) - ALLOC_BLOCKHDRSZ - ALLOC_CHUNKHDRSZ
 * All callers use a much-lower limit.
 */
static void *
AllocSetAlloc(MemoryContext context, Size size)
{
	AllocSet	set = (AllocSet) context;
	AllocBlock	block;
	AllocChunk	chunk;
	int			fidx;
	Size		chunk_size;
	Size		blksize;

	AssertArg(AllocSetIsValid(set));

	/*
	 * If requested size exceeds maximum for chunks, allocate an entire block
	 * for this request.
	 */
	/* 当申请的内存空间size大于内存片阀值，则直接申请新的AllocBlockData */
	if (size > set->allocChunkLimit)
	{
		/* 对齐字节 */
		chunk_size = MAXALIGN(size);
		/* 预留AllocBlockData，AllocChunkData空间 */
		blksize = chunk_size + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ;
		block = (AllocBlock) malloc(blksize);
		/* malloc申请失败直接返回null */
		if (block == NULL)
			return NULL;
		/* 当前block指向所属的AllocSetContextData */
		block->aset = set;
		/* 由于是为内存申请单独申请的block，所以不需要有多个chunk，所以
		   freeptr和endptr都指向最后的地址，表示没有空闲空间 */
		block->freeptr = block->endptr = ((char *) block) + blksize;

		/* 建立内存片chunk */
		chunk = (AllocChunk) (((char *) block) + ALLOC_BLOCKHDRSZ);
		/* 当前chunk指向所属的AllocSetContextData */
		chunk->aset = set;
		chunk->size = chunk_size;
#ifdef MEMORY_CONTEXT_CHECKING
		/* Valgrind: Will be made NOACCESS below. */
		chunk->requested_size = size;
		/* set mark to catch clobber of "unused" space */
		if (size < chunk_size)
			set_sentinel(AllocChunkGetPointer(chunk), size);
#endif
#ifdef RANDOMIZE_ALLOCATED_MEMORY
		/* fill the allocated space with junk */
		randomize_mem((char *) AllocChunkGetPointer(chunk), size);
#endif

		/*
		 * Stick the new block underneath the active allocation block, if any,
		 * so that we don't lose the use of the space remaining therein.
		 */
		/* 将当前block加入到blocks中 */
		if (set->blocks != NULL)
		{
			/* 如果存在blocks，则将此block加入到blocks的第二个位置 */
			block->prev = set->blocks;
			block->next = set->blocks->next;
			if (block->next)
				block->next->prev = block;
			set->blocks->next = block;
		}
		else
		{
			/* 如果不存在blocks，则将blocks直接指向此block */
			block->prev = NULL;
			block->next = NULL;
			set->blocks = block;
		}

		AllocAllocInfo(set, chunk);

		/*
		 * Chunk's metadata fields remain DEFINED.  The requested allocation
		 * itself can be NOACCESS or UNDEFINED; our caller will soon make it
		 * UNDEFINED.  Make extra space at the end of the chunk, if any,
		 * NOACCESS.
		 */
		VALGRIND_MAKE_MEM_NOACCESS((char *) chunk + ALLOC_CHUNKHDRSZ,
								   chunk_size - ALLOC_CHUNKHDRSZ);

		return AllocChunkGetPointer(chunk);
	}

	/*
	 * Request is small enough to be treated as a chunk.  Look in the
	 * corresponding free list to see if there is a free chunk we could reuse.
	 * If one is found, remove it from the free list, make it again a member
	 * of the alloc set and return its data address.
	 */
	/* 根据size计算log2(size)+1 */
	fidx = AllocSetFreeIndex(size);
	/* 选取空闲的chunk */
	chunk = set->freelist[fidx];
	if (chunk != NULL)
	{
		Assert(chunk->size >= size);
		/* 如果不为null，将此chunk从freelist中移除，并返回 */
		set->freelist[fidx] = (AllocChunk) chunk->aset;

		chunk->aset = (void *) set;

#ifdef MEMORY_CONTEXT_CHECKING
		/* Valgrind: Free list requested_size should be DEFINED. */
		chunk->requested_size = size;
		VALGRIND_MAKE_MEM_NOACCESS(&chunk->requested_size,
								   sizeof(chunk->requested_size));
		/* set mark to catch clobber of "unused" space */
		if (size < chunk->size)
			set_sentinel(AllocChunkGetPointer(chunk), size);
#endif
#ifdef RANDOMIZE_ALLOCATED_MEMORY
		/* fill the allocated space with junk */
		randomize_mem((char *) AllocChunkGetPointer(chunk), size);
#endif

		AllocAllocInfo(set, chunk);
		return AllocChunkGetPointer(chunk);
	}

	/*
	 * Choose the actual chunk size to allocate.
	 */
	/* 如果不存在空闲的chunk，就需要申请新的chunk，需要计算其size */
	chunk_size = (1 << ALLOC_MINBITS) << fidx;
	Assert(chunk_size >= size);

	/*
	 * If there is enough room in the active allocation block, we will put the
	 * chunk into that block.  Else must start a new one.
	 */
	/* 查看是否有合适的block可以申请chunk */
	if ((block = set->blocks) != NULL)
	{
		/* 计算是否有足够的空闲空间 */
		Size		availspace = block->endptr - block->freeptr;

		/* 
		* 如果空闲空间不足，则将已有的空闲空间划分为chunk。
		* 这么做的原因是blocks的第一个block作为活跃内存块，
		* 并且当前设计只对block做一次检查，避免不必要的消耗。
		* block 剩余的空间不足，直接把该 block 剩余的空间
		* 分配成 chunk，挂到 freelist 数组中
		*/
		if (availspace < (chunk_size + ALLOC_CHUNKHDRSZ))
		{
			/*
			 * The existing active (top) block does not have enough room for
			 * the requested allocation, but it might still have a useful
			 * amount of space in it.  Once we push it down in the block list,
			 * we'll never try to allocate more space from it. So, before we
			 * do that, carve up its free space into chunks that we can put on
			 * the set's freelists.
			 *
			 * Because we can only get here when there's less than
			 * ALLOC_CHUNK_LIMIT left in the block, this loop cannot iterate
			 * more than ALLOCSET_NUM_FREELISTS-1 times.
			 */
			/* 对空闲空间进行申请chunk，空闲chuck最小为(1 << ALLOC_MINBITS) + ALLOC_CHUNKHDRSZ */
			while (availspace >= ((1 << ALLOC_MINBITS) + ALLOC_CHUNKHDRSZ))
			{
				Size		availchunk = availspace - ALLOC_CHUNKHDRSZ;
				int			a_fidx = AllocSetFreeIndex(availchunk);

				/*
				 * In most cases, we'll get back the index of the next larger
				 * freelist than the one we need to put this chunk on.  The
				 * exception is when availchunk is exactly a power of 2.
				 */
				/* 确定合适的chunk size，因为block所有的内容都是经过对齐的，因此能够找到合适的大小 */
				if (availchunk != ((Size) 1 << (a_fidx + ALLOC_MINBITS)))
				{
					a_fidx--;
					Assert(a_fidx >= 0);
					availchunk = ((Size) 1 << (a_fidx + ALLOC_MINBITS));
				}
				/* 找到后直接使用此chunk */
				chunk = (AllocChunk) (block->freeptr);

				/* Prepare to initialize the chunk header. */
				VALGRIND_MAKE_MEM_UNDEFINED(chunk, ALLOC_CHUNKHDRSZ);

				block->freeptr += (availchunk + ALLOC_CHUNKHDRSZ);
				availspace -= (availchunk + ALLOC_CHUNKHDRSZ);

				chunk->size = availchunk;
#ifdef MEMORY_CONTEXT_CHECKING
				chunk->requested_size = 0;	/* mark it free */
#endif
				/* 将chunk加入到freelist内 */
				chunk->aset = (void *) set->freelist[a_fidx];
				set->freelist[a_fidx] = chunk;
			}

			/* Mark that we need to create a new block */
			/* 设置block为null，设置创建新block的标记 */
			// 由于没有找到合适的 chunk，从新分配 block
			block = NULL;
		}
	}

	/*
	 * Time to create a new regular (multi-chunk) block?
	 */
	/* 如果block是null，创建新的block */
	if (block == NULL)
	{
		Size		required_size;

		/*
		 * The first such block has size initBlockSize, and we double the
		 * space in each succeeding block, but not more than maxBlockSize.
		 */
		blksize = set->nextBlockSize;
		set->nextBlockSize <<= 1;
		if (set->nextBlockSize > set->maxBlockSize)
			set->nextBlockSize = set->maxBlockSize;

		/*
		 * If initBlockSize is less than ALLOC_CHUNK_LIMIT, we could need more
		 * space... but try to keep it a power of 2.
		 */
		required_size = chunk_size + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ;
		/* 如果下一次应申请的block的大小小于当前申请的内存大小，则左移一位，知道大于等于申请的大小 */
		while (blksize < required_size)
			blksize <<= 1;

		/* Try to allocate it */
		block = (AllocBlock) malloc(blksize);

		/*
		 * We could be asking for pretty big blocks here, so cope if malloc
		 * fails.  But give up if there's less than a meg or so available...
		 */
		/* 
		* 如果malloc失败，则继续申请，失败是因为可能blksize远远大于required_size，
		* 也超出当前剩余内存。当申请的内存小于1MB或blksize < required_size则放弃申请。
		*/
		while (block == NULL && blksize > 1024 * 1024)
		{
			blksize >>= 1;
			if (blksize < required_size)
				break;
			block = (AllocBlock) malloc(blksize);
		}

		if (block == NULL)
			return NULL;

		block->aset = set;
		block->freeptr = ((char *) block) + ALLOC_BLOCKHDRSZ;
		block->endptr = ((char *) block) + blksize;

		/*
		 * If this is the first block of the set, make it the "keeper" block.
		 * Formerly, a keeper block could only be created during context
		 * creation, but allowing it to happen here lets us have fast reset
		 * cycling even for contexts created with minContextSize = 0; that way
		 * we don't have to force space to be allocated in contexts that might
		 * never need any space.  Don't mark an oversize block as a keeper,
		 * however.
		 */
		if (set->keeper == NULL && blksize == set->initBlockSize)
			set->keeper = block;

		/* Mark unallocated space NOACCESS. */
		VALGRIND_MAKE_MEM_NOACCESS(block->freeptr,
								   blksize - ALLOC_BLOCKHDRSZ);

		block->prev = NULL;
		block->next = set->blocks;
		if (block->next)
			block->next->prev = block;
		set->blocks = block;
	}

	/*
	 * OK, do the allocation
	 */
	chunk = (AllocChunk) (block->freeptr);

	/* Prepare to initialize the chunk header. */
	VALGRIND_MAKE_MEM_UNDEFINED(chunk, ALLOC_CHUNKHDRSZ);

	block->freeptr += (chunk_size + ALLOC_CHUNKHDRSZ);
	Assert(block->freeptr <= block->endptr);

	chunk->aset = (void *) set;
	chunk->size = chunk_size;
#ifdef MEMORY_CONTEXT_CHECKING
	chunk->requested_size = size;
	VALGRIND_MAKE_MEM_NOACCESS(&chunk->requested_size,
							   sizeof(chunk->requested_size));
	/* set mark to catch clobber of "unused" space */
	if (size < chunk->size)
		set_sentinel(AllocChunkGetPointer(chunk), size);
#endif
#ifdef RANDOMIZE_ALLOCATED_MEMORY
	/* fill the allocated space with junk */
	randomize_mem((char *) AllocChunkGetPointer(chunk), size);
#endif

	AllocAllocInfo(set, chunk);
	return AllocChunkGetPointer(chunk);
}

/*
 * AllocSetFree
 *		Frees allocated memory; memory is removed from the set.
 */
static void
AllocSetFree(MemoryContext context, void *pointer)
{
	AllocSet	set = (AllocSet) context;
	AllocChunk	chunk = AllocPointerGetChunk(pointer);

	AllocFreeInfo(set, chunk);

#ifdef MEMORY_CONTEXT_CHECKING
	VALGRIND_MAKE_MEM_DEFINED(&chunk->requested_size,
							  sizeof(chunk->requested_size));
	/* Test for someone scribbling on unused space in chunk */
	if (chunk->requested_size < chunk->size)
		if (!sentinel_ok(pointer, chunk->requested_size))
			elog(WARNING, "detected write past chunk end in %s %p",
				 set->header.name, chunk);
#endif
	/* chunk大小超过chunk阀值，则直接释放此chunk所属的block */
	if (chunk->size > set->allocChunkLimit)
	{
		/*
		 * Big chunks are certain to have been allocated as single-chunk
		 * blocks.  Just unlink that block and return it to malloc().
		 */
		AllocBlock	block = (AllocBlock) (((char *) chunk) - ALLOC_BLOCKHDRSZ);

		/*
		 * Try to verify that we have a sane block pointer: it should
		 * reference the correct aset, and freeptr and endptr should point
		 * just past the chunk.
		 */
		if (block->aset != set ||
			block->freeptr != block->endptr ||
			block->freeptr != ((char *) block) +
			(chunk->size + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ))
			elog(ERROR, "could not find block containing chunk %p", chunk);

		/* OK, remove block from aset's list and free it */
		/* 从所属的blocks中移除 */
		if (block->prev)
			block->prev->next = block->next;
		else
			set->blocks = block->next;
		if (block->next)
			block->next->prev = block->prev;
#ifdef CLOBBER_FREED_MEMORY
		wipe_mem(block, block->freeptr - ((char *) block));
#endif
		free(block);
	}
	else
	{
		/* Normal case, put the chunk into appropriate freelist */
		/* 一般大小，则加入到freelist中 */
		int			fidx = AllocSetFreeIndex(chunk->size);
		/* chunk的aset指向freelist */
		chunk->aset = (void *) set->freelist[fidx];

#ifdef CLOBBER_FREED_MEMORY
		wipe_mem(pointer, chunk->size);
#endif

#ifdef MEMORY_CONTEXT_CHECKING
		/* Reset requested_size to 0 in chunks that are on freelist */
		chunk->requested_size = 0;
#endif
		set->freelist[fidx] = chunk;
	}
}

/*
 * AllocSetRealloc
 *		Returns new pointer to allocated memory of given size or NULL if
 *		request could not be completed; this memory is added to the set.
 *		Memory associated with given pointer is copied into the new memory,
 *		and the old memory is freed.
 *
 * Without MEMORY_CONTEXT_CHECKING, we don't know the old request size.  This
 * makes our Valgrind client requests less-precise, hazarding false negatives.
 * (In principle, we could use VALGRIND_GET_VBITS() to rediscover the old
 * request size.)
 */
static void *
AllocSetRealloc(MemoryContext context, void *pointer, Size size)
{
	AllocSet	set = (AllocSet) context;
	AllocChunk	chunk = AllocPointerGetChunk(pointer);
	Size		oldsize = chunk->size;

#ifdef MEMORY_CONTEXT_CHECKING
	VALGRIND_MAKE_MEM_DEFINED(&chunk->requested_size,
							  sizeof(chunk->requested_size));
	/* Test for someone scribbling on unused space in chunk */
	if (chunk->requested_size < oldsize)
		if (!sentinel_ok(pointer, chunk->requested_size))
			elog(WARNING, "detected write past chunk end in %s %p",
				 set->header.name, chunk);
#endif

	/*
	 * Chunk sizes are aligned to power of 2 in AllocSetAlloc(). Maybe the
	 * allocated area already is >= the new size.  (In particular, we always
	 * fall out here if the requested size is a decrease.)
	 */
	/* 如果realloc的大小，小于chunk大小，直接返回 */
	if (oldsize >= size)
	{
#ifdef MEMORY_CONTEXT_CHECKING
		Size		oldrequest = chunk->requested_size;

#ifdef RANDOMIZE_ALLOCATED_MEMORY
		/* We can only fill the extra space if we know the prior request */
		if (size > oldrequest)
			randomize_mem((char *) pointer + oldrequest,
						  size - oldrequest);
#endif

		chunk->requested_size = size;
		VALGRIND_MAKE_MEM_NOACCESS(&chunk->requested_size,
								   sizeof(chunk->requested_size));

		/*
		 * If this is an increase, mark any newly-available part UNDEFINED.
		 * Otherwise, mark the obsolete part NOACCESS.
		 */
		if (size > oldrequest)
			VALGRIND_MAKE_MEM_UNDEFINED((char *) pointer + oldrequest,
										size - oldrequest);
		else
			VALGRIND_MAKE_MEM_NOACCESS((char *) pointer + size,
									   oldsize - size);

		/* set mark to catch clobber of "unused" space */
		if (size < oldsize)
			set_sentinel(pointer, size);
#else							/* !MEMORY_CONTEXT_CHECKING */

		/*
		 * We don't have the information to determine whether we're growing
		 * the old request or shrinking it, so we conservatively mark the
		 * entire new allocation DEFINED.
		 */
		VALGRIND_MAKE_MEM_NOACCESS(pointer, oldsize);
		VALGRIND_MAKE_MEM_DEFINED(pointer, size);
#endif

		return pointer;
	}
	/* 如果此chunk的size大于内存片的阀值，则表示这是单独的block，直接对block进行处理 */
	if (oldsize > set->allocChunkLimit)
	{
		/*
		 * The chunk must have been allocated as a single-chunk block.  Use
		 * realloc() to make the containing block bigger with minimum space
		 * wastage.
		 */
		AllocBlock	block = (AllocBlock) (((char *) chunk) - ALLOC_BLOCKHDRSZ);
		Size		chksize;
		Size		blksize;

		/*
		 * Try to verify that we have a sane block pointer: it should
		 * reference the correct aset, and freeptr and endptr should point
		 * just past the chunk.
		 */
		if (block->aset != set ||
			block->freeptr != block->endptr ||
			block->freeptr != ((char *) block) +
			(chunk->size + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ))
			elog(ERROR, "could not find block containing chunk %p", chunk);

		/* Do the realloc */
		/* 使用realloc对此block进行处理 */
		chksize = MAXALIGN(size);
		blksize = chksize + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ;
		block = (AllocBlock) realloc(block, blksize);
		if (block == NULL)
			return NULL;
		block->freeptr = block->endptr = ((char *) block) + blksize;

		/* Update pointers since block has likely been moved */
		/* 在realloc后的block申请chunk */
		chunk = (AllocChunk) (((char *) block) + ALLOC_BLOCKHDRSZ);
		pointer = AllocChunkGetPointer(chunk);
		if (block->prev)
			block->prev->next = block;
		else
			set->blocks = block;
		if (block->next)
			block->next->prev = block;
		chunk->size = chksize;

#ifdef MEMORY_CONTEXT_CHECKING
#ifdef RANDOMIZE_ALLOCATED_MEMORY
		/* We can only fill the extra space if we know the prior request */
		randomize_mem((char *) pointer + chunk->requested_size,
					  size - chunk->requested_size);
#endif

		/*
		 * realloc() (or randomize_mem()) will have left the newly-allocated
		 * part UNDEFINED, but we may need to adjust trailing bytes from the
		 * old allocation.
		 */
		VALGRIND_MAKE_MEM_UNDEFINED((char *) pointer + chunk->requested_size,
									oldsize - chunk->requested_size);

		chunk->requested_size = size;
		VALGRIND_MAKE_MEM_NOACCESS(&chunk->requested_size,
								   sizeof(chunk->requested_size));

		/* set mark to catch clobber of "unused" space */
		if (size < chunk->size)
			set_sentinel(pointer, size);
#else							/* !MEMORY_CONTEXT_CHECKING */

		/*
		 * We don't know how much of the old chunk size was the actual
		 * allocation; it could have been as small as one byte.  We have to be
		 * conservative and just mark the entire old portion DEFINED.
		 */
		VALGRIND_MAKE_MEM_DEFINED(pointer, oldsize);
#endif

		/* Make any trailing alignment padding NOACCESS. */
		VALGRIND_MAKE_MEM_NOACCESS((char *) pointer + size, chksize - size);

		return pointer;
	}
	else
	{
		/*
		 * Small-chunk case.  We just do this by brute force, ie, allocate a
		 * new chunk and copy the data.  Since we know the existing data isn't
		 * huge, this won't involve any great memcpy expense, so it's not
		 * worth being smarter.  (At one time we tried to avoid memcpy when it
		 * was possible to enlarge the chunk in-place, but that turns out to
		 * misbehave unpleasantly for repeated cycles of
		 * palloc/repalloc/pfree: the eventually freed chunks go into the
		 * wrong freelist for the next initial palloc request, and so we leak
		 * memory indefinitely.  See pgsql-hackers archives for 2007-08-11.)
		 */
		/* 如果realloc的大小，大于chunk大小 */
		AllocPointer newPointer;

		/* allocate new chunk */
		/* 申请新的chunk */
		newPointer = AllocSetAlloc((MemoryContext) set, size);

		/* leave immediately if request was not completed */
		if (newPointer == NULL)
			return NULL;

		/*
		 * AllocSetAlloc() just made the region NOACCESS.  Change it to
		 * UNDEFINED for the moment; memcpy() will then transfer definedness
		 * from the old allocation to the new.  If we know the old allocation,
		 * copy just that much.  Otherwise, make the entire old chunk defined
		 * to avoid errors as we copy the currently-NOACCESS trailing bytes.
		 */
		VALGRIND_MAKE_MEM_UNDEFINED(newPointer, size);
#ifdef MEMORY_CONTEXT_CHECKING
		oldsize = chunk->requested_size;
#else
		VALGRIND_MAKE_MEM_DEFINED(pointer, oldsize);
#endif

		/* transfer existing data (certain to fit) */
		/* 将原有内容复制到新的chunk内 */
		memcpy(newPointer, pointer, oldsize);

		/* free old chunk */
		/* 释放原来的chunk */
		AllocSetFree((MemoryContext) set, pointer);

		return newPointer;
	}
}

/*
 * AllocSetGetChunkSpace
 *		Given a currently-allocated chunk, determine the total space
 *		it occupies (including all memory-allocation overhead).
 */
static Size
AllocSetGetChunkSpace(MemoryContext context, void *pointer)
{
	AllocChunk	chunk = AllocPointerGetChunk(pointer);

	return chunk->size + ALLOC_CHUNKHDRSZ;
}

/*
 * AllocSetIsEmpty
 *		Is an allocset empty of any allocated space?
 */
static bool
AllocSetIsEmpty(MemoryContext context)
{
	/*
	 * For now, we say "empty" only if the context is new or just reset. We
	 * could examine the freelists to determine if all space has been freed,
	 * but it's not really worth the trouble for present uses of this
	 * functionality.
	 */
	if (context->isReset)
		return true;
	return false;
}

/*
 * AllocSetStats
 *		Compute stats about memory consumption of an allocset.
 *
 * level: recursion level (0 at top level); used for print indentation.
 * print: true to print stats to stderr.
 * totals: if not NULL, add stats about this allocset into *totals.
 */
/*
	遍历当前AllocSetContextData的blocks，统计内存申请总量，block数，
	未使用的chunk数，以及使用的内存大小。
*/
static void
AllocSetStats(MemoryContext context, int level, bool print,
			  MemoryContextCounters *totals)
{
	AllocSet	set = (AllocSet) context;
	Size		nblocks = 0;
	Size		freechunks = 0;
	Size		totalspace = 0;
	Size		freespace = 0;
	AllocBlock	block;
	int			fidx;

	/* 遍历blocks，确定使用了多少block */
	for (block = set->blocks; block != NULL; block = block->next)
	{
		nblocks++;
		totalspace += block->endptr - ((char *) block);
		freespace += block->endptr - block->freeptr;
	}
	/* 查找空闲chunk链表，计算空闲chunk数量，以及空闲空间大小 */
	for (fidx = 0; fidx < ALLOCSET_NUM_FREELISTS; fidx++)
	{
		AllocChunk	chunk;

		for (chunk = set->freelist[fidx]; chunk != NULL;
			 chunk = (AllocChunk) chunk->aset)
		{
			freechunks++;
			freespace += chunk->size + ALLOC_CHUNKHDRSZ;
		}
	}

	if (print)
	{
		int			i;

		for (i = 0; i < level; i++)
			fprintf(stderr, "  ");
		fprintf(stderr,
				"%s: %zu total in %zd blocks; %zu free (%zd chunks); %zu used\n",
				set->header.name, totalspace, nblocks, freespace, freechunks,
				totalspace - freespace);
	}

	if (totals)
	{
		totals->nblocks += nblocks;
		totals->freechunks += freechunks;
		totals->totalspace += totalspace;
		totals->freespace += freespace;
	}
}


#ifdef MEMORY_CONTEXT_CHECKING

/*
 * AllocSetCheck
 *		Walk through chunks and check consistency of memory.
 *
 * NOTE: report errors as WARNING, *not* ERROR or FATAL.  Otherwise you'll
 * find yourself in an infinite loop when trouble occurs, because this
 * routine will be entered again when elog cleanup tries to release memory!
 */
static void
AllocSetCheck(MemoryContext context)
{
	AllocSet	set = (AllocSet) context;
	char	   *name = set->header.name;
	AllocBlock	prevblock;
	AllocBlock	block;

	for (prevblock = NULL, block = set->blocks;
		 block != NULL;
		 prevblock = block, block = block->next)
	{
		char	   *bpoz = ((char *) block) + ALLOC_BLOCKHDRSZ;
		long		blk_used = block->freeptr - bpoz;
		long		blk_data = 0;
		long		nchunks = 0;

		/*
		 * Empty block - empty can be keeper-block only
		 */
		if (!blk_used)
		{
			if (set->keeper != block)
				elog(WARNING, "problem in alloc set %s: empty block %p",
					 name, block);
		}

		/*
		 * Check block header fields
		 */
		if (block->aset != set ||
			block->prev != prevblock ||
			block->freeptr < bpoz ||
			block->freeptr > block->endptr)
			elog(WARNING, "problem in alloc set %s: corrupt header in block %p",
				 name, block);

		/*
		 * Chunk walker
		 */
		while (bpoz < block->freeptr)
		{
			AllocChunk	chunk = (AllocChunk) bpoz;
			Size		chsize,
						dsize;

			chsize = chunk->size;	/* aligned chunk size */
			VALGRIND_MAKE_MEM_DEFINED(&chunk->requested_size,
									  sizeof(chunk->requested_size));
			dsize = chunk->requested_size;	/* real data */
			if (dsize > 0)		/* not on a free list */
				VALGRIND_MAKE_MEM_NOACCESS(&chunk->requested_size,
										   sizeof(chunk->requested_size));

			/*
			 * Check chunk size
			 */
			if (dsize > chsize)
				elog(WARNING, "problem in alloc set %s: req size > alloc size for chunk %p in block %p",
					 name, chunk, block);
			if (chsize < (1 << ALLOC_MINBITS))
				elog(WARNING, "problem in alloc set %s: bad size %zu for chunk %p in block %p",
					 name, chsize, chunk, block);

			/* single-chunk block? */
			if (chsize > set->allocChunkLimit &&
				chsize + ALLOC_CHUNKHDRSZ != blk_used)
				elog(WARNING, "problem in alloc set %s: bad single-chunk %p in block %p",
					 name, chunk, block);

			/*
			 * If chunk is allocated, check for correct aset pointer. (If it's
			 * free, the aset is the freelist pointer, which we can't check as
			 * easily...)
			 */
			if (dsize > 0 && chunk->aset != (void *) set)
				elog(WARNING, "problem in alloc set %s: bogus aset link in block %p, chunk %p",
					 name, block, chunk);

			/*
			 * Check for overwrite of "unallocated" space in chunk
			 */
			if (dsize > 0 && dsize < chsize &&
				!sentinel_ok(chunk, ALLOC_CHUNKHDRSZ + dsize))
				elog(WARNING, "problem in alloc set %s: detected write past chunk end in block %p, chunk %p",
					 name, block, chunk);

			blk_data += chsize;
			nchunks++;

			bpoz += ALLOC_CHUNKHDRSZ + chsize;
		}

		if ((blk_data + (nchunks * ALLOC_CHUNKHDRSZ)) != blk_used)
			elog(WARNING, "problem in alloc set %s: found inconsistent memory block %p",
				 name, block);
	}
}

#endif							/* MEMORY_CONTEXT_CHECKING */
