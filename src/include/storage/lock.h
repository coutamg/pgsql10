/*-------------------------------------------------------------------------
 *
 * lock.h
 *	  POSTGRES low-level lock mechanism
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/lock.h
 * 参考 https://blog.csdn.net/Hehuyi_In/article/details/124641464
 *-------------------------------------------------------------------------
 */
#ifndef LOCK_H_
#define LOCK_H_

#ifdef FRONTEND
#error "lock.h may not be included from frontend code"
#endif

#include "storage/lockdefs.h"
#include "storage/backendid.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
/*
 * pg中的锁可以分为3个层次：
 * 
 * 自旋锁（Spin Lock）：是一种和硬件结合的互斥锁，借用了硬件提供的原子操作的原语来对一
 *   些共享变量进行封锁，通常适用于临界区比较小的情况。特点是：封锁时间很短、无死锁检测
 *   机制和等待队列、事务结束时不会自动释放SpinLock。
 * 
 * 轻量锁（Lightweight Lock）：负责保护共享内存中的数据结构，有共享和排他两种模式，
 *   类似Oracle中的latch。特点是：封锁时间较短、无死锁检测机制、有等待队列、事务结
 *   束时会自动释放。
 * 
 * 常规锁（Regular Lock）：就是通常说的对数据库对象的锁。按照锁粒度，可以分为表锁、
 *   页锁、行锁等；按照等级，pg锁一共有8个等级。特点是：封锁时间可以很长、有死锁检
 *   测机制和等待队列、事务结束时会自动释放。
 * /

/* struct PGPROC is declared in proc.h, but must forward-reference it */
typedef struct PGPROC PGPROC;

typedef struct PROC_QUEUE
{
	SHM_QUEUE	links;			/* head of list of PGPROC objects */
	int			size;			/* number of entries in list */
} PROC_QUEUE;

/* GUC variables */
extern int	max_locks_per_xact;

#ifdef LOCK_DEBUG
extern int	Trace_lock_oidmin;
extern bool Trace_locks;
extern bool Trace_userlocks;
extern int	Trace_lock_table;
extern bool Debug_deadlocks;
#endif							/* LOCK_DEBUG */


/*
 * Top-level transactions are identified by VirtualTransactionIDs comprising
 * the BackendId of the backend running the xact, plus a locally-assigned
 * LocalTransactionId.  These are guaranteed unique over the short term,
 * but will be reused after a database restart; hence they should never
 * be stored on disk.
 *
 * Note that struct VirtualTransactionId can not be assumed to be atomically
 * assignable as a whole.  However, type LocalTransactionId is assumed to
 * be atomically assignable, and the backend ID doesn't change often enough
 * to be a problem, so we can fetch or assign the two fields separately.
 * We deliberately refrain from using the struct within PGPROC, to prevent
 * coding errors from trying to use struct assignment with it; instead use
 * GET_VXID_FROM_PGPROC().
 * 
 * 执行dml操作时，才会为事务分配事务id。不过，即使没有事务id，事务也会用一个虚拟
 * 事务id来代表自己.
 * 
 * 虚拟事务id由两部分组成：backendId（后台进程id，会话独有）+ 
 *                     localTransactionId（进程维护的本地事务id）
 */
typedef struct
{
	BackendId	backendId;		/* determined at backend startup */
	LocalTransactionId localTransactionId;	/* backend-local transaction id */
} VirtualTransactionId;

#define InvalidLocalTransactionId		0
#define LocalTransactionIdIsValid(lxid) ((lxid) != InvalidLocalTransactionId)
#define VirtualTransactionIdIsValid(vxid) \
	(((vxid).backendId != InvalidBackendId) && \
	 LocalTransactionIdIsValid((vxid).localTransactionId))
#define VirtualTransactionIdEquals(vxid1, vxid2) \
	((vxid1).backendId == (vxid2).backendId && \
	 (vxid1).localTransactionId == (vxid2).localTransactionId)
#define SetInvalidVirtualTransactionId(vxid) \
	((vxid).backendId = InvalidBackendId, \
	 (vxid).localTransactionId = InvalidLocalTransactionId)
#define GET_VXID_FROM_PGPROC(vxid, proc) \
	((vxid).backendId = (proc).backendId, \
	 (vxid).localTransactionId = (proc).lxid)

/* MAX_LOCKMODES cannot be larger than the # of bits in LOCKMASK */
#define MAX_LOCKMODES		10

#define LOCKBIT_ON(lockmode) (1 << (lockmode))
#define LOCKBIT_OFF(lockmode) (~(1 << (lockmode)))


/*
 * This data structure defines the locking semantics associated with a
 * "lock method".  The semantics specify the meaning of each lock mode
 * (by defining which lock modes it conflicts with).
 * All of this data is constant and is kept in const tables.
 *
 * numLockModes -- number of lock modes (READ,WRITE,etc) that
 *		are defined in this lock method.  Must be less than MAX_LOCKMODES.
 *
 * conflictTab -- this is an array of bitmasks showing lock
 *		mode conflicts.  conflictTab[i] is a mask with the j-th bit
 *		turned on if lock modes i and j conflict.  Lock modes are
 *		numbered 1..numLockModes; conflictTab[0] is unused.
 *
 * lockModeNames -- ID strings for debug printouts.
 *
 * trace_flag -- pointer to GUC trace flag for this lock method.  (The
 * GUC variable is not constant, but we use "const" here to denote that
 * it can't be changed through this reference.)
 * 
 * 参考 https://blog.csdn.net/Hehuyi_In/article/details/124773243
 * 
 * 如果要对一个表进行操作，通常会通过 heap_open 打开这个表，并在打开时指定需要的锁模式。
 * 之后会有一系列函数将锁模式传递下去，最终通过 LockRelationOid 函数将表的 Oid 和
 * lockmode 联系在一起。如下:
 * # define heap_open(r,l)
 * Relation table_open(Oid relationId, LOCKMODE lockmode)
 * Relation relation_open(Oid relationId, LOCKMODE lockmode)
 * void LockRelationOid(Oid relid, LOCKMODE lockmode)
 * 
 * 该结构体定义了与"lock method"关联的锁的语义
 */
typedef struct LockMethodData
{
	/* 锁的模式数量 */
	int			numLockModes;
	/* 标志冲突的（二维）数组，mode i 和 mode j 冲突则 
	 * conflictTab [i] 的第 j 位 = 1，
	 * conflictTab[0]未使用
	 * 
	 * 由于 LOCKMASK 是个位图，所以 conflictTab 可以认为是个二维数组
	 * 
	 * 是常规锁模式的相容矩阵
	 */
	const LOCKMASK *conflictTab;
	const char *const *lockModeNames;
	const bool *trace_flag;
} LockMethodData;

typedef const LockMethodData *LockMethod;

/*
 * Lock methods are identified by LOCKMETHODID.  (Despite the declaration as
 * uint16, we are constrained to 256 lockmethods by the layout of LOCKTAG.)
 * Lock methods的id，由于 LOCKTAG 对应字段为8位，所以最多指定256个
 */
typedef uint16 LOCKMETHODID;

/* These identify the known lock methods */
/* 两个预定义的 lock methods */
#define DEFAULT_LOCKMETHOD	1
#define USER_LOCKMETHOD		2

/*
 * LOCKTAG is the key information needed to look up a LOCK item in the
 * lock hashtable.  A LOCKTAG value uniquely identifies a lockable object.
 *
 * The LockTagType enum defines the different kinds of objects we can lock.
 * We can handle up to 256 different LockTagTypes.
 */
typedef enum LockTagType
{
	/* 表锁 */
	LOCKTAG_RELATION,			/* whole relation */
	/* ID info for a relation is DB OID + REL OID; DB OID = 0 if shared */
	/* 对表进行extend时加锁 */
	LOCKTAG_RELATION_EXTEND,	/* the right to extend a relation */
	/* same ID info as RELATION */
	/* 页锁 */
	LOCKTAG_PAGE,				/* one page of a relation */
	/* ID info for a page is RELATION info + BlockNumber */
	/* 行锁 */
	LOCKTAG_TUPLE,				/* one physical tuple */
	/* ID info for a tuple is PAGE info + OffsetNumber */
	/* 事务锁，在分配事务 id 时对这个事务 id 加锁，由于行并发更新时做事务等待 */
	LOCKTAG_TRANSACTION,		/* transaction (for waiting for xact done) */
	/* ID info for a transaction is its TransactionId */
	/* 虚拟事务id */
	LOCKTAG_VIRTUALTRANSACTION, /* virtual transaction (ditto) */
	/* ID info for a virtual transaction is its VirtualTransactionId */
	/* upsert语句中需要等待confirm的行加锁 */
	LOCKTAG_SPECULATIVE_TOKEN,	/* speculative insertion Xid and token */
	/* ID info for a transaction is its TransactionId */
	LOCKTAG_OBJECT,				/* non-relation database object */
	/* ID info for an object is DB OID + CLASS OID + OBJECT OID + SUBID */

	/*
	 * Note: object ID has same representation as in pg_depend and
	 * pg_description, but notice that we are constraining SUBID to 16 bits.
	 * Also, we use DB OID = 0 for shared objects such as tablespaces.
	 */
	LOCKTAG_USERLOCK,			/* reserved for old contrib/userlock code */
	LOCKTAG_ADVISORY			/* advisory user locks */
} LockTagType;

#define LOCKTAG_LAST_TYPE	LOCKTAG_ADVISORY

extern const char *const LockTagTypeNames[];

/*
 * The LOCKTAG struct is defined with malice aforethought to fit into 16
 * bytes with no padding.  Note that this would need adjustment if we were
 * to widen Oid, BlockNumber, or TransactionId to more than 32 bits.
 *
 * We include lockmethodid in the locktag so that a single hash table in
 * shared memory can store locks of different lockmethods.
 * 
 * 常规锁不仅可以对表加锁，也可以对各类对象加锁。LOCKTAG 结构体的成员变量没有特定的
 * 含义，完全取决于具体类型
 */
// LOCKTAG 结构体中的成员变量没有特定的含义，它们的含义完全取决于使用者
typedef struct LOCKTAG
{
	uint32		locktag_field1; /* a 32-bit ID field */
	uint32		locktag_field2; /* a 32-bit ID field */
	uint32		locktag_field3; /* a 32-bit ID field */
	uint16		locktag_field4; /* a 16-bit ID field */
	uint8		locktag_type;	/* see enum LockTagType */
	uint8		locktag_lockmethodid;	/* lockmethod indicator */
} LOCKTAG;
// 见 lock-LOCKTAG.png
/*
 * These macros define how we map logical IDs of lockable objects into
 * the physical fields of LOCKTAG.  Use these to set up LOCKTAG values,
 * rather than accessing the fields directly.  Note multiple eval of target!
 */
#define SET_LOCKTAG_RELATION(locktag,dboid,reloid) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (reloid), \
	 (locktag).locktag_field3 = 0, \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_RELATION, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_RELATION_EXTEND(locktag,dboid,reloid) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (reloid), \
	 (locktag).locktag_field3 = 0, \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_RELATION_EXTEND, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_PAGE(locktag,dboid,reloid,blocknum) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (reloid), \
	 (locktag).locktag_field3 = (blocknum), \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_PAGE, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_TUPLE(locktag,dboid,reloid,blocknum,offnum) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (reloid), \
	 (locktag).locktag_field3 = (blocknum), \
	 (locktag).locktag_field4 = (offnum), \
	 (locktag).locktag_type = LOCKTAG_TUPLE, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_TRANSACTION(locktag,xid) \
	((locktag).locktag_field1 = (xid), \
	 (locktag).locktag_field2 = 0, \
	 (locktag).locktag_field3 = 0, \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_TRANSACTION, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_VIRTUALTRANSACTION(locktag,vxid) \
	((locktag).locktag_field1 = (vxid).backendId, \
	 (locktag).locktag_field2 = (vxid).localTransactionId, \
	 (locktag).locktag_field3 = 0, \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_VIRTUALTRANSACTION, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_SPECULATIVE_INSERTION(locktag,xid,token) \
	((locktag).locktag_field1 = (xid), \
	 (locktag).locktag_field2 = (token),		\
	 (locktag).locktag_field3 = 0, \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_SPECULATIVE_TOKEN, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_OBJECT(locktag,dboid,classoid,objoid,objsubid) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (classoid), \
	 (locktag).locktag_field3 = (objoid), \
	 (locktag).locktag_field4 = (objsubid), \
	 (locktag).locktag_type = LOCKTAG_OBJECT, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_ADVISORY(locktag,id1,id2,id3,id4) \
	((locktag).locktag_field1 = (id1), \
	 (locktag).locktag_field2 = (id2), \
	 (locktag).locktag_field3 = (id3), \
	 (locktag).locktag_field4 = (id4), \
	 (locktag).locktag_type = LOCKTAG_ADVISORY, \
	 (locktag).locktag_lockmethodid = USER_LOCKMETHOD)


/*
 * Per-locked-object lock information:
 *
 * tag -- uniquely identifies the object being locked
 * grantMask -- bitmask for all lock types currently granted on this object.
 * waitMask -- bitmask for all lock types currently awaited on this object.
 * procLocks -- list of PROCLOCK objects for this lock.
 * waitProcs -- queue of processes waiting for this lock.
 * requested -- count of each lock type currently requested on the lock
 *		(includes requests already granted!!).
 * nRequested -- total requested locks of all types.
 * granted -- count of each lock type currently granted on the lock.
 * nGranted -- total granted locks of all types.
 *
 * Note: these counts count 1 for each backend.  Internally to a backend,
 * there may be multiple grabs on a particular lock, but this is not reflected
 * into shared memory.
 */
/*
tag - 
    该键域用于标记共享内存lock哈希表中的hashing locks.标记tag的内容本质上定义了
    一个独立的可锁定对象.关于已支持的可锁定对象类型的详细信息可参考include/storage/lock.h.
    之所以定义为一个单独的结构是为了确保能够把归零正确的字节数.
    编译器可能插入到结构体中的所有对齐字节数正确被归零是很很重要的,否则的话哈希的计算会是随机的.
	(当前来看,定义结构体LOCKTAG以避免对齐字节)
grantMask -
    该bitmask表示在给定的可锁定对象上持有了哪些类型的locks.
    该字段用于确定新申请的锁是否会与现存的锁存在冲突.
    冲突通过 grantMask 和请求锁类型的冲突表条目的 bitwise AND操作实现.
waitMask -
    该字段标记了正在等待的锁类型.当且仅当requested[i] > granted[i],waitMask中的第1位为1.
procLocks -
    与 lock object 相关的 PROCLOCK 结构体在共享内存中的队列.
    注意链表中存在 granted 和 waiting PROCLOCKs.
    (实际上,同一个 PROCLOCK 可能有已授予的locks但正在等待更多的锁) 
waitProcs -
    对应等待其他后台进程释放锁的后台进程的 PGPROC 结构体在共享内存中的队列.
    进程结构体保存了用于确定在锁释放时是否需要唤醒的相关信息.
nRequested -
    该字段保存了尝试获取该锁的次数.计数包括因为冲突而处于休眠状态的次数.
    如果一个进程第一次请求读然后请求写时可能会导致该进程被多次统计. 
requested -
    该字段保存了尝试获取多少种锁类型.只有1 -> MAX_LOCKMODES-1被使用,因为这对应了锁类型常量.
    计算requested数组的和应等于nRequested. 
nGranted -
    成功获取该锁的次数.该计数不包括因为冲突而等待的次数.因此该计数规则与nRequested一样.
granted -
    保存每种类型有多少锁.1 -> MAX_LOCKMODES-1是有用的.
    与requested类似,granted[]数组的和应等于nGranted.
nGranted的的范围为[0,nRequested],对于每一个granted[i]范围为[0,requested[i]].
如果所有请求变为0,那么LOCK对象不再需要,会通过free释放.
*/
// 加锁函数 LockAcquire
/*
 * 主锁表（LOCK结构体）：保存一个锁对象所有相关信息
 * 参考 https://blog.csdn.net/Hehuyi_In/article/details/124773243
 */
typedef struct LOCK
{
	/* hash key */
	// 锁对象的唯一 ID
	LOCKTAG		tag;			/* unique identifier of lockable object */

	/* data */
	/* 该对象已经持有的锁模式 */
	LOCKMASK	grantMask;		/* bitmask for lock types already granted */
	/* 等待队列中请求该 锁对象 的 锁模式 */
	LOCKMASK	waitMask;		/* bitmask for lock types awaited */
	// 这个锁上所有 PROCLOCK
	SHM_QUEUE	procLocks;		/* list of PROCLOCK objects assoc. with lock */
	// 等在这个锁上的 PGPROC
	PROC_QUEUE	waitProcs;		/* list of PGPROC objects waiting on lock */
	// 记录持有和等待该锁的会话数
	int			requested[MAX_LOCKMODES];	/* counts of requested locks */
	// requested 数组的和
	int			nRequested;		/* total of requested[] array */
	/* 持有该锁的会话数 */
	int			granted[MAX_LOCKMODES]; /* counts of granted locks */
	/* granted数组的元素数 */
	int			nGranted;		/* total of granted[] array */
} LOCK;

#define LOCK_LOCKMETHOD(lock) ((LOCKMETHODID) (lock).tag.locktag_lockmethodid)


/*
 * We may have several different backends holding or awaiting locks
 * on the same lockable object.  We need to store some per-holder/waiter
 * information for each such holder (or would-be holder).  This is kept in
 * a PROCLOCK struct.
 *
 * PROCLOCKTAG is the key information needed to look up a PROCLOCK item in the
 * proclock hashtable.  A PROCLOCKTAG value uniquely identifies the combination
 * of a lockable object and a holder/waiter for that object.  (We can use
 * pointers here because the PROCLOCKTAG need only be unique for the lifespan
 * of the PROCLOCK, and it will never outlive the lock or the proc.)
 *
 * Internally to a backend, it is possible for the same lock to be held
 * for different purposes: the backend tracks transaction locks separately
 * from session locks.  However, this is not reflected in the shared-memory
 * state: we only track which backend(s) hold the lock.  This is OK since a
 * backend can never block itself.
 *
 * The holdMask field shows the already-granted locks represented by this
 * proclock.  Note that there will be a proclock object, possibly with
 * zero holdMask, for any lock that the process is currently waiting on.
 * Otherwise, proclock objects whose holdMasks are zero are recycled
 * as soon as convenient.
 *
 * releaseMask is workspace for LockReleaseAll(): it shows the locks due
 * to be released during the current call.  This must only be examined or
 * set by the backend owning the PROCLOCK.
 *
 * Each PROCLOCK object is linked into lists for both the associated LOCK
 * object and the owning PGPROC object.  Note that the PROCLOCK is entered
 * into these lists as soon as it is created, even if no lock has yet been
 * granted.  A PGPROC that is waiting for a lock to be granted will also be
 * linked into the lock's waitProcs queue.
 * 
 * 每个 PROCLOCK 对象被链接到关联的 LOCK 对象和所属的 PGPROC 对象的列表中。注意，一旦
 * 创建 PROCLOCK，它就会被输入到这些列表中，即使还没有授予锁。等待授予锁的 PGPROC 也将
 * 被链接到锁的 waitProcs 队列中
 */
/*
  进程锁表被用来保存当前进程（会话）的事务锁的状态，它保存的是PROCLOCK结构体，
  这个结构体的主要作用就是建立锁和会话（锁的申请者）的关系
*/
typedef struct PROCLOCKTAG
{
	/* NB: we assume this struct contains no padding! */
	/* 指向每个锁会话信息 */
	LOCK	   *myLock;			/* link to per-lockable-object information */
	/* 指向进程 PGPROC */
	PGPROC	   *myProc;			/* link to PGPROC of owning backend */
} PROCLOCKTAG;

/*
 * 进程锁表（PROCLOCK结构体）：保存一个锁对象中与当前会话（进程）相关的信息
 * 参考 https://blog.csdn.net/Hehuyi_In/article/details/124773243
 */
typedef struct PROCLOCK
{
	/* tag PROCLOCK 的唯一 ID*/
	PROCLOCKTAG tag;			/* unique identifier of proclock object */

	/* data */
	// 并行执行时，并行会话的leader
	PGPROC	   *groupLeader;	/* proc's lock group leader, or proc itself */
	// 当前会话在这个对象上持有的锁模式
	LOCKMASK	holdMask;		/* bitmask for lock types currently held */
	// lockReleaseAll 专用，记录需要释放的锁模式
	LOCKMASK	releaseMask;	/* bitmask for lock types to be released */
	// LOCK 结构体中的 procLocks 记录了这个 PROCLOCK
	SHM_QUEUE	lockLink;		/* list link in LOCK's list of proclocks */
	// PGPROC 结构体中的 myProcLocks 记录了这个 PROCLOCK
	SHM_QUEUE	procLink;		/* list link in PGPROC's list of proclocks */
} PROCLOCK;

#define PROCLOCK_LOCKMETHOD(proclock) \
	LOCK_LOCKMETHOD(*((proclock).tag.myLock))

/*
 * Each backend also maintains a local hash table with information about each
 * lock it is currently interested in.  In particular the local table counts
 * the number of times that lock has been acquired.  This allows multiple
 * requests for the same lock to be executed without additional accesses to
 * shared memory.  We also track the number of lock acquisitions per
 * ResourceOwner, so that we can release just those locks belonging to a
 * particular ResourceOwner.
 *
 * When holding a lock taken "normally", the lock and proclock fields always
 * point to the associated objects in shared memory.  However, if we acquired
 * the lock via the fast-path mechanism, the lock and proclock fields are set
 * to NULL, since there probably aren't any such objects in shared memory.
 * (If the lock later gets promoted to normal representation, we may eventually
 * update our locallock's lock/proclock fields after finding the shared
 * objects.)
 *
 * Caution: a locallock object can be left over from a failed lock acquisition
 * attempt.  In this case its lock/proclock fields are untrustworthy, since
 * the shared lock object is neither held nor awaited, and hence is available
 * to be reclaimed.  If nLocks > 0 then these pointers must either be valid or
 * NULL, but when nLocks == 0 they should be considered garbage.
 */
typedef struct LOCALLOCKTAG
{
	LOCKTAG		lock;			/* identifies the lockable object */
	LOCKMODE	mode;			/* lock mode for this table entry */
} LOCALLOCKTAG;

typedef struct LOCALLOCKOWNER
{
	/*
	 * Note: if owner is NULL then the lock is held on behalf of the session;
	 * otherwise it is held on behalf of my current transaction.
	 *
	 * Must use a forward struct reference to avoid circularity.
	 */
	struct ResourceOwnerData *owner;
	int64		nLocks;			/* # of times held by this owner */
} LOCALLOCKOWNER;

/* https://blog.csdn.net/Hehuyi_In/article/details/124773243 
 * 本地锁表(LOCALLOCK 结构体)：对于重复申请的锁进行计数，避免频繁访问
 * 		主锁表和进程锁表，相当于一层缓存
 * 
 * 快速路径（fast path）：对弱锁的访问保存到本进程，避免频繁访问主锁表(LOCK)
 * 和进程锁表(PROCLOCK)
 * 
 * 每个会话都保存了一个本地锁表 —— 当事务重复在同一个对象上申请同类型锁时，
 * 无须做冲突检测，只要将这个锁记录在本地即可，避免频繁访问主锁表和进程锁表。
 * 
 * 本地锁表的名字是 LockMethodLocalHash, 这是个 hash 表。
 * 本地锁表的判断和获取在 LockAcquireExtended 函数
 */
typedef struct LOCALLOCK
{
	/* tag */
	/* 本地锁唯一id：LOCKTAG + LOCKMODE hash key */
	LOCALLOCKTAG tag;			/* unique identifier of locallock entry */

	/* data  */
	/* 关联主锁表对象 */
	LOCK	   *lock;			/* associated LOCK object, if any */
	/* 锁对应的进程对象, 关联进程锁表对象 */
	PROCLOCK   *proclock;		/* associated PROCLOCK object, if any */
	/* LOCKTAG 的 hash 值 */
	uint32		hashcode;		/* copy of LOCKTAG's hash value */
	/* 本地有多少次持有该锁，类似引用计数, 本地持有该锁的次数（类似计数器） */
	int64		nLocks;			/* total number of times lock is held */
	/* 相关的 ResourceOwners 数量, lockOwners 数组的实际大小 */
	int			numLockOwners;	/* # of relevant ResourceOwners */
	/* ResourceOwners数组的最大长度 */
	int			maxLockOwners;	/* allocated size of array */
	// 锁 Fast Path 需要的标记，是否持有了强锁
	bool		holdsStrongLockCount;	/* bumped FastPathStrongRelationLocks */
	/* 长度可以动态变化的数组 */
	LOCALLOCKOWNER *lockOwners; /* dynamically resizable array */
} LOCALLOCK;

#define LOCALLOCK_LOCKMETHOD(llock) ((llock).tag.lock.locktag_lockmethodid)


/*
 * These structures hold information passed from lmgr internals to the lock
 * listing user-level functions (in lockfuncs.c).
 */

typedef struct LockInstanceData
{
	LOCKTAG		locktag;		/* tag for locked object */
	LOCKMASK	holdMask;		/* locks held by this PGPROC */
	LOCKMODE	waitLockMode;	/* lock awaited by this PGPROC, if any */
	BackendId	backend;		/* backend ID of this PGPROC */
	LocalTransactionId lxid;	/* local transaction ID of this PGPROC */
	int			pid;			/* pid of this PGPROC */
	int			leaderPid;		/* pid of group leader; = pid if no group */
	bool		fastpath;		/* taken via fastpath? */
} LockInstanceData;

typedef struct LockData
{
	int			nelements;		/* The length of the array */
	LockInstanceData *locks;	/* Array of per-PROCLOCK information */
} LockData;

typedef struct BlockedProcData
{
	int			pid;			/* pid of a blocked PGPROC */
	/* Per-PROCLOCK information about PROCLOCKs of the lock the pid awaits */
	/* (these fields refer to indexes in BlockedProcsData.locks[]) */
	int			first_lock;		/* index of first relevant LockInstanceData */
	int			num_locks;		/* number of relevant LockInstanceDatas */
	/* PIDs of PGPROCs that are ahead of "pid" in the lock's wait queue */
	/* (these fields refer to indexes in BlockedProcsData.waiter_pids[]) */
	int			first_waiter;	/* index of first preceding waiter */
	int			num_waiters;	/* number of preceding waiters */
} BlockedProcData;

typedef struct BlockedProcsData
{
	BlockedProcData *procs;		/* Array of per-blocked-proc information */
	LockInstanceData *locks;	/* Array of per-PROCLOCK information */
	int		   *waiter_pids;	/* Array of PIDs of other blocked PGPROCs */
	int			nprocs;			/* # of valid entries in procs[] array */
	int			maxprocs;		/* Allocated length of procs[] array */
	int			nlocks;			/* # of valid entries in locks[] array */
	int			maxlocks;		/* Allocated length of locks[] array */
	int			npids;			/* # of valid entries in waiter_pids[] array */
	int			maxpids;		/* Allocated length of waiter_pids[] array */
} BlockedProcsData;


/* Result codes for LockAcquire() */
typedef enum
{
	LOCKACQUIRE_NOT_AVAIL,		/* lock not available, and dontWait=true */
	LOCKACQUIRE_OK,				/* lock successfully acquired */
	LOCKACQUIRE_ALREADY_HELD	/* incremented count for lock already held */
} LockAcquireResult;

/* Deadlock states identified by DeadLockCheck() */
typedef enum
{
	DS_NOT_YET_CHECKED,			/* no deadlock check has run yet */
	DS_NO_DEADLOCK,				/* no deadlock detected */
	DS_SOFT_DEADLOCK,			/* deadlock avoided by queue rearrangement */
	DS_HARD_DEADLOCK,			/* deadlock, no way out but ERROR */
	DS_BLOCKED_BY_AUTOVACUUM	/* no deadlock; queue blocked by autovacuum
								 * worker */
} DeadLockState;

/*
 * The lockmgr's shared hash tables are partitioned to reduce contention.
 * To determine which partition a given locktag belongs to, compute the tag's
 * hash code with LockTagHashCode(), then apply one of these macros.
 * NB: NUM_LOCK_PARTITIONS must be a power of 2!
 */
#define LockHashPartition(hashcode) \
	((hashcode) % NUM_LOCK_PARTITIONS)
#define LockHashPartitionLock(hashcode) \
	(&MainLWLockArray[LOCK_MANAGER_LWLOCK_OFFSET + \
		LockHashPartition(hashcode)].lock)
#define LockHashPartitionLockByIndex(i) \
	(&MainLWLockArray[LOCK_MANAGER_LWLOCK_OFFSET + (i)].lock)

/*
 * The deadlock detector needs to be able to access lockGroupLeader and
 * related fields in the PGPROC, so we arrange for those fields to be protected
 * by one of the lock hash partition locks.  Since the deadlock detector
 * acquires all such locks anyway, this makes it safe for it to access these
 * fields without doing anything extra.  To avoid contention as much as
 * possible, we map different PGPROCs to different partition locks.  The lock
 * used for a given lock group is determined by the group leader's pgprocno.
 */
#define LockHashPartitionLockByProc(leader_pgproc) \
	LockHashPartitionLock((leader_pgproc)->pgprocno)

/*
 * function prototypes
 */
extern void InitLocks(void);
extern LockMethod GetLocksMethodTable(const LOCK *lock);
extern LockMethod GetLockTagsMethodTable(const LOCKTAG *locktag);
extern uint32 LockTagHashCode(const LOCKTAG *locktag);
extern bool DoLockModesConflict(LOCKMODE mode1, LOCKMODE mode2);
extern LockAcquireResult LockAcquire(const LOCKTAG *locktag,
			LOCKMODE lockmode,
			bool sessionLock,
			bool dontWait);
extern LockAcquireResult LockAcquireExtended(const LOCKTAG *locktag,
					LOCKMODE lockmode,
					bool sessionLock,
					bool dontWait,
					bool report_memory_error);
extern void AbortStrongLockAcquire(void);
extern bool LockRelease(const LOCKTAG *locktag,
			LOCKMODE lockmode, bool sessionLock);
extern void LockReleaseAll(LOCKMETHODID lockmethodid, bool allLocks);
extern void LockReleaseSession(LOCKMETHODID lockmethodid);
extern void LockReleaseCurrentOwner(LOCALLOCK **locallocks, int nlocks);
extern void LockReassignCurrentOwner(LOCALLOCK **locallocks, int nlocks);
extern bool LockHasWaiters(const LOCKTAG *locktag,
			   LOCKMODE lockmode, bool sessionLock);
extern VirtualTransactionId *GetLockConflicts(const LOCKTAG *locktag,
				 LOCKMODE lockmode);
extern void AtPrepare_Locks(void);
extern void PostPrepare_Locks(TransactionId xid);
extern int LockCheckConflicts(LockMethod lockMethodTable,
				   LOCKMODE lockmode,
				   LOCK *lock, PROCLOCK *proclock);
extern void GrantLock(LOCK *lock, PROCLOCK *proclock, LOCKMODE lockmode);
extern void GrantAwaitedLock(void);
extern void RemoveFromWaitQueue(PGPROC *proc, uint32 hashcode);
extern Size LockShmemSize(void);
extern LockData *GetLockStatusData(void);
extern BlockedProcsData *GetBlockerStatusData(int blocked_pid);

extern xl_standby_lock *GetRunningTransactionLocks(int *nlocks);
extern const char *GetLockmodeName(LOCKMETHODID lockmethodid, LOCKMODE mode);

extern void lock_twophase_recover(TransactionId xid, uint16 info,
					  void *recdata, uint32 len);
extern void lock_twophase_postcommit(TransactionId xid, uint16 info,
						 void *recdata, uint32 len);
extern void lock_twophase_postabort(TransactionId xid, uint16 info,
						void *recdata, uint32 len);
extern void lock_twophase_standby_recover(TransactionId xid, uint16 info,
							  void *recdata, uint32 len);

extern DeadLockState DeadLockCheck(PGPROC *proc);
extern PGPROC *GetBlockingAutoVacuumPgproc(void);
extern void DeadLockReport(void) pg_attribute_noreturn();
extern void RememberSimpleDeadLock(PGPROC *proc1,
					   LOCKMODE lockmode,
					   LOCK *lock,
					   PGPROC *proc2);
extern void InitDeadLockChecking(void);

extern int	LockWaiterCount(const LOCKTAG *locktag);

#ifdef LOCK_DEBUG
extern void DumpLocks(PGPROC *proc);
extern void DumpAllLocks(void);
#endif

/* Lock a VXID (used to wait for a transaction to finish) */
extern void VirtualXactLockTableInsert(VirtualTransactionId vxid);
extern void VirtualXactLockTableCleanup(void);
extern bool VirtualXactLock(VirtualTransactionId vxid, bool wait);

#endif							/* LOCK_H */
