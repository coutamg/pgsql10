/*-------------------------------------------------------------------------
 *
 * proc.h
 *	  per-process shared memory data structures
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/proc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PROC_H_
#define _PROC_H_

#include "access/xlogdefs.h"
#include "lib/ilist.h"
#include "storage/latch.h"
#include "storage/lock.h"
#include "storage/pg_sema.h"
#include "storage/proclist_types.h"

/*
 * Each backend advertises up to PGPROC_MAX_CACHED_SUBXIDS TransactionIds
 * for non-aborted subtransactions of its current top transaction.  These
 * have to be treated as running XIDs by other backends.
 *
 * We also keep track of whether the cache overflowed (ie, the transaction has
 * generated at least one subtransaction that didn't fit in the cache).
 * If none of the caches have overflowed, we can assume that an XID that's not
 * listed anywhere in the PGPROC array is not a running transaction.  Else we
 * have to look at pg_subtrans.
 */
#define PGPROC_MAX_CACHED_SUBXIDS 64	/* XXX guessed-at value */

struct XidCache
{
	TransactionId xids[PGPROC_MAX_CACHED_SUBXIDS];
};

/*
 * Flags for PGXACT->vacuumFlags
 *
 * Note: If you modify these flags, you need to modify PROCARRAY_XXX flags
 * in src/include/storage/procarray.h.
 *
 * PROC_RESERVED may later be assigned for use in vacuumFlags, but its value is
 * used for PROCARRAY_SLOTS_XMIN in procarray.h, so GetOldestXmin won't be able
 * to match and ignore processes with this flag set.
 */
#define		PROC_IS_AUTOVACUUM	0x01	/* is it an autovac worker? */
#define		PROC_IN_VACUUM		0x02	/* currently running lazy vacuum */
#define		PROC_IN_ANALYZE		0x04	/* currently running analyze */
#define		PROC_VACUUM_FOR_WRAPAROUND	0x08	/* set by autovac only */
#define		PROC_IN_LOGICAL_DECODING	0x10	/* currently doing logical
												 * decoding outside xact */
#define		PROC_RESERVED				0x20	/* reserved for procarray */

/* flags reset at EOXact */
#define		PROC_VACUUM_STATE_MASK \
	(PROC_IN_VACUUM | PROC_IN_ANALYZE | PROC_VACUUM_FOR_WRAPAROUND)

/*
 * We allow a small number of "weak" relation locks (AccesShareLock,
 * RowShareLock, RowExclusiveLock) to be recorded in the PGPROC structure
 * rather than the main lock table.  This eases contention on the lock
 * manager LWLocks.  See storage/lmgr/README for additional details.
 */
#define		FP_LOCK_SLOTS_PER_BACKEND 16

/*
 * An invalid pgprocno.  Must be larger than the maximum number of PGPROC
 * structures we could possibly have.  See comments for MAX_BACKENDS.
 * 无效的pg进程号.
 * 必须大于我们可能拥有的最大的PGPROC数目.
 * 详细解释见MAX_BACKENDS
 */
#define INVALID_PGPROCNO		PG_INT32_MAX

/*
 * 每个后台进程在共享内存中都有一个PGPROC结构体.
 * 全局上也存在未使用的PGPROC结构体链表,用于重用以便为新的后台进程进行分配.
 * 该数据结构的作用是: 
 *  PostgreSQL 后台进程不能直接看到彼此的内存，postmaster 
 *  也不能看到 PostgreSQL 后台进程的内存。然而，他们需要某种方式来沟通和协调，
 *  postmaster 需要一种方式来跟踪他们
 * 简单来说作用是为了进程间协同和通讯以及postmaster的跟踪
 * 
 * Each backend has a PGPROC struct in shared memory.  There is also a list of
 * currently-unused PGPROC structs that will be reallocated to new backends.
 * 每个后台进程在共享内存中都有一个PGPROC结构体.
 * 存在未使用的PGPROC结构体链表,用于为新的后台进程重新进行分配.
 *
 * links: list link for any list the PGPROC is in.  When waiting for a lock,
 * the PGPROC is linked into that lock's waitProcs queue.  A recycled PGPROC
 * is linked into ProcGlobal's freeProcs list.
 * links: PGPROC所在的链表的链接.
 *   在等待锁时,PGPROC 链接到该锁(LOCK)的 waitProcs 队列中.
 *   回收的 PGPROC 链接到 ProcGlobal 的 freeProcs 链表中.
 *
 * Note: twophase.c also sets up a dummy PGPROC struct for each currently
 * prepared transaction.  These PGPROCs appear in the ProcArray data structure
 * so that the prepared transactions appear to be still running and are
 * correctly shown as holding locks.  A prepared transaction PGPROC can be
 * distinguished from a real one at need by the fact that it has pid == 0.
 * The semaphore and lock-activity fields in a prepared-xact PGPROC are unused,
 * but its myProcLocks[] lists are valid.
 * 注意:twophase.c也会为每一个当前已准备妥当的事务配置一个虚拟的PGPROC结构.
 * 这些 PGPROCs 在数组ProcArray 数据结构中出现,以便已准备的事务看起来仍在运行,
 *   并正确的显示为持有锁.
 * 已准备妥当的事务 PGPROC 与一个真正的 PGPROC 事实上的区别是 pid == 0.
 * 在 prepared-xact PGPROC 中的信号量和活动锁域字段没有使用,但 myProcLocks[]链表是有效的.
 */
// 参考 https://www.jianshu.com/p/4a9c36bc9897
// 每一个 PGPROC 都在 InitProcGlobal 被提前分配好，放在
// ProcGlobal->allProcs 中
// 具体可以参考 proc.png
/*
  +------------------+
  | ProcGlobal		 |
  | freeProcs        |
  | +---------------+|<--+
  | | PGPROC   	    ||   |
  | | procgloballist+----+  header
  | | links.next    ||
  | +-------+-------+| <---+
  +---------|--------+	   |
		   \|/			   |
  +------------------+	   |
  | PGPROC   		 |	   |
  | procgloballist   +-----+
  | links.next       |
  +---------+--------+	
           \|/
		   ...
*/
struct PGPROC
{
	/* proc->links MUST BE FIRST IN STRUCT (see ProcSleep,ProcWakeup,etc) */
	// proc->links 必须是结构体的第一个域(参考ProcSleep,ProcWakeup...等)
    // 如进程在链表中,这是链表的链接
	// pgprocno < MaxConnections 为 normal
	// 对于 normal 的 backend,记录自己 next 的 ProcGlobal->freeProcs 的位置

	// MaxConnections <= pgprocno < MaxConnections + 
	//        autovacuum_max_workers + 1 为 AV launcher/worker
	// 对于 AV launcher/worker, 记录自己 next ProcGlobal->autovacFreeProcs 
	// 位置

	// 剩余的为 bgworker
	// 对于 bgworker, 记录自己 next ProcGlobal->bgworkerFreeProcs 位置
	SHM_QUEUE	links;			/* list link if process is in a list */
	// pgprocno < MaxConnections 为 normal
	// 对于 normal 的 backend,标记 ProcGlobal->freeProcs 的位置

	// MaxConnections <= pgprocno < MaxConnections + 
	//        autovacuum_max_workers + 1 为 AV launcher/worker
	// 对于 AV launcher/worker, 标记 ProcGlobal->autovacFreeProcs 位置

	// 剩余的为 bgworker
	// 对于 bgworker, 标记 ProcGlobal->bgworkerFreeProcs 位置
	PGPROC	  **procgloballist; /* procglobal list that owns this PGPROC */
	// 可以休眠的信号量
	PGSemaphore sem;			/* ONE semaphore to sleep on */
	// 状态为: STATUS_WAITING, STATUS_OK or STATUS_ERROR
	int			waitStatus;		/* STATUS_WAITING, STATUS_OK or STATUS_ERROR */
	// 进程通用的 latch, 用作进程通信
	Latch		procLatch;		/* generic latch for process */
	// 运行中的进程正在执行的最高层的事务本地 ID,如无运行则为 InvalidLocalTransactionId
	LocalTransactionId lxid;	/* local id of top-level transaction currently
								 * being executed by this proc, if running;
								 * else InvalidLocalTransactionId */
	int			pid;			/* Backend's process ID; 0 if prepared xact */
	// 在 InitProcGlobal 中已经 预先分配好了
	int			pgprocno;

	/* These fields are zero while a backend is still starting up: */
	//------------ 这些域在进程正在启动时为0
	// 已分配的后台进程的 backend ID
	// 在 StartTransaction 中被赋值
	BackendId	backendId;		/* This backend's backend ID (if assigned) */
	// 该进程使用的数据库ID
	Oid			databaseId;		/* OID of database this backend is using */
	Oid			roleId;			/* OID of role using this backend */

	// 如后台进程,则为T
	bool		isBackgroundWorker; /* true if background worker. */

	/*
	 * While in hot standby mode, shows that a conflict signal has been sent
	 * for the current transaction. Set/cleared while holding ProcArrayLock,
	 * though not required. Accessed without lock, if needed.
	 * 如在hot standby模式,显示已为当前事务发送冲突信号.
     * 尽管不需要,设置/清除持有的ProcArrayLock.
     * 如需要,则在没有持有锁的情况下访问.
	 */
	bool		recoveryConflictPending;

	/* Info about LWLock the process is currently waiting for, if any. */
	//-------------- 进程正在等待的LWLock相关信息
	// 等待 LW lock,为 T
	bool		lwWaiting;		/* true if waiting for an LW lock */
	// 正在等的 LWLock 锁模式
	uint8		lwWaitMode;		/* lwlock mode being waited for */
	// 等待链表中的位置
	proclist_node lwWaitLink;	/* position in LW lock wait list */

	/* Support for condition variables. */
	//-------------- 支持条件变量
	// CV 等待链表中的位置
	proclist_node cvWaitLink;	/* position in CV wait list */

	/* Info about lock the process is currently waiting for, if any. */
	//-------------- 进程正在等待的锁信息
	/* waitLock and waitProcLock are NULL if not currently waiting. */
	// 如没有在等待,则 waitLock 和 waitProcLock 为 NULL
	// 休眠...等待的锁对象
	LOCK	   *waitLock;		/* Lock object we're sleeping on ... */
	// 等待锁的每个持锁人信息
	PROCLOCK   *waitProcLock;	/* Per-holder info for awaited lock */
	// 等待的所类型
	LOCKMODE	waitLockMode;	/* type of lock we're waiting for */
	// 该进程已持有锁的类型位掩码
	LOCKMASK	heldLocks;		/* bitmask for lock types already held on this
								 * lock object by this backend */

	/*
	 * Info to allow us to wait for synchronous replication, if needed.
	 * waitLSN is InvalidXLogRecPtr if not waiting; set only by user backend.
	 * syncRepState must not be touched except by owning process or WALSender.
	 * syncRepLinks used only while holding SyncRepLock.
	 * 
	 * 允许我们等待同步复制的相关信息.
     * 如无需等待,则 waitLSN 为 InvalidXLogRecPtr; 仅允许由用户后台设置。
     * 除非拥有 process 或 WALSender，否则不能修改 syncRepState。
     * 仅在持有 SyncRepLock 时使用的 syncrepink。
	 */
	// 等待该 LSN 或者更高的 LSN
	XLogRecPtr	waitLSN;		/* waiting for this LSN or higher */
	// 同步复制的等待状态
	int			syncRepState;	/* wait state for sync rep */
	// 如进程处于 syncrep 队列中,则该值保存链表链接
	SHM_QUEUE	syncRepLinks;	/* list link if process is in syncrep queue */

	/*
	 * All PROCLOCK objects for locks held or awaited by this backend are
	 * linked into one of these lists, according to the partition number of
	 * their lock.
	 * 该后台进程持有或等待的锁相关的所有 PROCLOCK 对象链接在这些链表的末尾,
     * 根据棣属于这些锁的分区号进行区分.
	 * 
	 * 这个后端持有或等待的锁的所有PROCLOCK对象都根据其锁的分区号链接到这些列表中的一个
	 */
	SHM_QUEUE	myProcLocks[NUM_LOCK_PARTITIONS];
	// 子事务的 XIDs
	struct XidCache subxids;	/* cache for subtransaction XIDs */

	/* Support for group XID clearing. */
	/* true, if member of ProcArray group waiting for XID clear */
	/* 如果是一个 ProcArray 组的成员为true */
	bool		procArrayGroupMember;
	/* next ProcArray group member waiting for XID clear */
	/* 指向下一个 ProcArray 组的成员 */
	pg_atomic_uint32 procArrayGroupNext;

	/*
	 * latest transaction id among the transaction's main XID and
	 * subtransactions
	 * 这是在调用重置xid的函数所需要传递的参数
	 */
	TransactionId procArrayGroupMemberXid;
	// 进程的等待信息
	uint32		wait_event_info;	/* proc's wait information */

	/* Per-backend LWLock.  Protects fields below (but not group fields). */
	// 每一个后台进程一个 LWLock.保护下面的域字段(非组字段)
	LWLock		backendLock;

	/* Lock manager data, recording fast-path locks taken by this backend. */
	//---------- 锁管理数据,记录该后台进程以最快路径获得的锁
	// 每一个fast-path slot的锁模式
	/*
	 * 是一个位图，它目前是一个64位的无符号整型，其中只有48位是有用的。每3位组成一个槽，
	 * 每个槽都和PGPROC->fpRelId数组中的表对应。
	 * 当一个事务要对弱锁使用 Fast Path 时，就尝试在 PGPROC->fpLockBits 中记录当前的锁模式，
	 * 这时候事务就能够获得锁了
	 */
	uint64		fpLockBits;		/* lock modes held for each fast-path slot */
	// rel oids 的 slots
	// 弱锁保存在本事务的Fast Path中
	// PGPROC->fpRelId是一个长度为16的数组，它里面保存的是表的Oid，这就限制了每个事务可
	// 以保存的弱锁的数量
	Oid			fpRelId[FP_LOCK_SLOTS_PER_BACKEND]; /* slots for rel oids */
	// 是否持有 fast-path VXID 锁
	bool		fpVXIDLock;		/* are we holding a fast-path VXID lock? */
	// fast-path VXID 锁的 lxid
	LocalTransactionId fpLocalTransactionId;	/* lxid for fast-path VXID
												 * lock */

	/*
	 * Support for lock groups.  Use LockHashPartitionLockByProc on the group
	 * leader to get the LWLock protecting these fields.
	 */
	//--------- 支持锁组.
	//          在组 leader 中使用 LockHashPartitionLockByProc 获取 LWLock 保护这些域
    // 锁组的leader,如果"我"是其中一员
	PGPROC	   *lockGroupLeader;	/* lock group leader, if I'm a member */
	// 如果"我"是 leader,这是成员的链表
	dlist_head	lockGroupMembers;	/* list of members, if I'm a leader */
	// 成员连接,如果"我"是其中一员
	dlist_node	lockGroupLink;	/* my member link, if I'm a member */
};

/* NOTE: "typedef struct PGPROC PGPROC" appears in storage/lock.h. */

// 在 InitProcess 中被初始化
extern PGDLLIMPORT PGPROC *MyProc;
// MyPgXact 只是保存了 PGPROC 中部分关键信息，减少 cache line miss
extern PGDLLIMPORT struct PGXACT *MyPgXact;

/*
 * Prior to PostgreSQL 9.2, the fields below were stored as part of the
 * PGPROC.  However, benchmarking revealed that packing these particular
 * members into a separate array as tightly as possible sped up GetSnapshotData
 * considerably on systems with many CPU cores, by reducing the number of
 * cache lines needing to be fetched.  Thus, think very carefully before adding
 * anything else here.
 */
typedef struct PGXACT
{
	TransactionId xid;			/* id of top-level transaction currently being
								 * executed by this proc, if running and XID
								 * is assigned; else InvalidTransactionId */

	TransactionId xmin;			/* minimal running XID as it was when we were
								 * starting our xact, excluding LAZY VACUUM:
								 * vacuum must not remove tuples deleted by
								 * xid >= xmin ! */

	uint8		vacuumFlags;	/* vacuum-related flags, see above */
	bool		overflowed;
	bool		delayChkpt;		/* true if this proc delays checkpoint start;
								 * previously called InCommit */

	uint8		nxids;
} PGXACT;

/*
 * There is one ProcGlobal struct for the whole database cluster.
 */
typedef struct PROC_HDR
{
	/* Array of PGPROC structures (not including dummies for prepared txns) */
	PGPROC	   *allProcs;
	/* Array of PGXACT structures (not including dummies for prepared txns) */
	PGXACT	   *allPgXact;
	/* Length of allProcs array */
	uint32		allProcCount;
	/* Head of list of free PGPROC structures */
	PGPROC	   *freeProcs;
	/* Head of list of autovacuum's free PGPROC structures */
	PGPROC	   *autovacFreeProcs;
	/* Head of list of bgworker free PGPROC structures */
	PGPROC	   *bgworkerFreeProcs;
	/* First pgproc waiting for group XID clear */
	/* ProcArray组的第一个成员 */
	pg_atomic_uint32 procArrayGroupFirst;
	/* WALWriter process's latch */
	Latch	   *walwriterLatch;
	/* Checkpointer process's latch */
	Latch	   *checkpointerLatch;
	/* Current shared estimate of appropriate spins_per_delay value */
	int			spins_per_delay;
	/* The proc of the Startup process, since not in ProcArray */
	PGPROC	   *startupProc;
	int			startupProcPid;
	/* Buffer id of the buffer that Startup process waits for pin on, or -1 */
	int			startupBufferPinWaitBufId;
} PROC_HDR;

extern PROC_HDR *ProcGlobal;

extern PGPROC *PreparedXactProcs;

/* Accessor for PGPROC given a pgprocno. */
#define GetPGProcByNumber(n) (&ProcGlobal->allProcs[(n)])

/*
 * We set aside some extra PGPROC structures for auxiliary processes,
 * ie things that aren't full-fledged backends but need shmem access.
 *
 * Background writer, checkpointer and WAL writer run during normal operation.
 * Startup process and WAL receiver also consume 2 slots, but WAL writer is
 * launched only after startup has exited, so we only need 4 slots.
 */
#define NUM_AUXILIARY_PROCS		4

/* configurable options */
extern int	DeadlockTimeout;
extern int	StatementTimeout;
extern int	LockTimeout;
extern int	IdleInTransactionSessionTimeout;
extern bool log_lock_waits;


/*
 * Function Prototypes
 */
extern int	ProcGlobalSemas(void);
extern Size ProcGlobalShmemSize(void);
extern void InitProcGlobal(void);
extern void InitProcess(void);
extern void InitProcessPhase2(void);
extern void InitAuxiliaryProcess(void);

extern void PublishStartupProcessInformation(void);
extern void SetStartupBufferPinWaitBufId(int bufid);
extern int	GetStartupBufferPinWaitBufId(void);

extern bool HaveNFreeProcs(int n);
extern void ProcReleaseLocks(bool isCommit);

extern void ProcQueueInit(PROC_QUEUE *queue);
extern int	ProcSleep(LOCALLOCK *locallock, LockMethod lockMethodTable);
extern PGPROC *ProcWakeup(PGPROC *proc, int waitStatus);
extern void ProcLockWakeup(LockMethod lockMethodTable, LOCK *lock);
extern void CheckDeadLockAlert(void);
extern bool IsWaitingForLock(void);
extern void LockErrorCleanup(void);

extern void ProcWaitForSignal(uint32 wait_event_info);
extern void ProcSendSignal(int pid);

extern PGPROC *AuxiliaryPidGetProc(int pid);

extern void BecomeLockGroupLeader(void);
extern bool BecomeLockGroupMember(PGPROC *leader, int pid);

#endif							/* PROC_H */
