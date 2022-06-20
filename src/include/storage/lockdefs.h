/*-------------------------------------------------------------------------
 *
 * lockdefs.h
 *	   Frontend exposed parts of postgres' low level lock mechanism
 *
 * The split between lockdefs.h and lock.h is not very principled. This file
 * contains definition that have to (indirectly) be available when included by
 * FRONTEND code.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/lockdefs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOCKDEFS_H_
#define LOCKDEFS_H_

/*
 * LOCKMODE is an integer (1..N) indicating a lock type.  LOCKMASK is a bit
 * mask indicating a set of held or requested lock types (the bit 1<<mode
 * corresponds to a particular lock mode).
 * 
 * LOCKMODE 是当整型用的，而 LOCKMASK 是当位图用的
 */
typedef int LOCKMASK;
typedef int LOCKMODE;

/*
 * These are the valid values of type LOCKMODE for all the standard lock
 * methods (both DEFAULT and USER).
 */

// 常规锁模式参考 lockdefs-1.png, 相容矩阵参考 lockdefs-2.png
/* NoLock is not a lock mode, but a flag value meaning "don't get a lock" */
/* 根据兼容性表，彼此相容的3个锁（1-3级，AccessShareLock、RowShareLock、
 * RowExclusiveLock）是弱锁，4级锁 ShareUpdateExclusiveLock 比较特殊，
 * 既不是强锁也不是弱锁，5-8 级的 4 个则是强锁
 * 
 * 弱锁只保存在当前会话，从而避免频繁访问共享内存的主锁表，提高数据库的性能。
 * 虽然判断是否有强锁也需要访问共享内存中的 FastPathStrongRelationLocks，
 * 但这种访问粒度比较小
 */
#define NoLock					0

/*========= 弱锁 ====================*/
#define AccessShareLock			1	/* SELECT */
#define RowShareLock			2	/* SELECT FOR UPDATE/FOR SHARE */
#define RowExclusiveLock		3	/* INSERT, UPDATE, DELETE */
/*========== 不是强锁也不是弱锁 =======*/
#define ShareUpdateExclusiveLock 4	/* VACUUM (non-FULL),ANALYZE, CREATE INDEX
									 * CONCURRENTLY */
/*========== 强锁 ===================*/
#define ShareLock				5	/* CREATE INDEX (WITHOUT CONCURRENTLY) */
#define ShareRowExclusiveLock	6	/* like EXCLUSIVE MODE, but allows ROW
									 * SHARE */
#define ExclusiveLock			7	/* blocks ROW SHARE/SELECT...FOR UPDATE */
#define AccessExclusiveLock		8	/* ALTER TABLE, DROP TABLE, VACUUM FULL,
									 * and unqualified LOCK TABLE */

typedef struct xl_standby_lock
{
	TransactionId xid;			/* xid of holder of AccessExclusiveLock */
	Oid			dbOid;
	Oid			relOid;
} xl_standby_lock;

#endif							/* LOCKDEF_H_ */
