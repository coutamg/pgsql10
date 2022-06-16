/*-------------------------------------------------------------------------
 *
 * transam.c
 *	  postgres transaction (commit) log interface routines
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/transam.c
 *
 * NOTES
 *	  This file contains the high level access-method interface to the
 *	  transaction system.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/clog.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "utils/snapmgr.h"

/*
 * Single-item cache for results of TransactionLogFetch.  It's worth having
 * such a cache because we frequently find ourselves repeatedly checking the
 * same XID, for example when scanning a table just after a bulk insert,
 * update, or delete.
 */
static TransactionId cachedFetchXid = InvalidTransactionId;
static XidStatus cachedFetchXidStatus;
static XLogRecPtr cachedCommitLSN;

/* Local functions */
static XidStatus TransactionLogFetch(TransactionId transactionId);


/* ----------------------------------------------------------------
 *		Postgres log access method interface
 *
 *		TransactionLogFetch
 * ----------------------------------------------------------------
 */

/*
 * TransactionLogFetch --- fetch commit status of specified transaction id
 */
static XidStatus
TransactionLogFetch(TransactionId transactionId)
{
	XidStatus	xidstatus;
	XLogRecPtr	xidlsn;

	/*
	 * Before going to the commit log manager, check our single item cache to
	 * see if we didn't just check the transaction status a moment ago.
	 */
	if (TransactionIdEquals(transactionId, cachedFetchXid))
		return cachedFetchXidStatus;

	/*
	 * Also, check to see if the transaction ID is a permanent one.
	 */
	if (!TransactionIdIsNormal(transactionId))
	{
		if (TransactionIdEquals(transactionId, BootstrapTransactionId))
			return TRANSACTION_STATUS_COMMITTED;
		if (TransactionIdEquals(transactionId, FrozenTransactionId))
			return TRANSACTION_STATUS_COMMITTED;
		return TRANSACTION_STATUS_ABORTED;
	}

	/*
	 * Get the transaction status.
	 */
	xidstatus = TransactionIdGetStatus(transactionId, &xidlsn);

	/*
	 * Cache it, but DO NOT cache status for unfinished or sub-committed
	 * transactions!  We only cache status that is guaranteed not to change.
	 */
	if (xidstatus != TRANSACTION_STATUS_IN_PROGRESS &&
		xidstatus != TRANSACTION_STATUS_SUB_COMMITTED)
	{
		cachedFetchXid = transactionId;
		cachedFetchXidStatus = xidstatus;
		cachedCommitLSN = xidlsn;
	}

	return xidstatus;
}

/* ----------------------------------------------------------------
 *						Interface functions
 *
 *		TransactionIdDidCommit
 *		TransactionIdDidAbort
 *		========
 *		   these functions test the transaction status of
 *		   a specified transaction id.
 *
 *		TransactionIdCommitTree
 *		TransactionIdAsyncCommitTree
 *		TransactionIdAbortTree
 *		========
 *		   these functions set the transaction status of the specified
 *		   transaction tree.
 *
 * See also TransactionIdIsInProgress, which once was in this module
 * but now lives in procarray.c.
 * ----------------------------------------------------------------
 */

/*
 * TransactionIdDidCommit
 *		True iff transaction associated with the identifier did commit.
 *
 * Note:
 *		Assumes transaction identifier is valid and exists in clog.
 */
bool							/* true if given transaction committed */
TransactionIdDidCommit(TransactionId transactionId)
{
	XidStatus	xidstatus;

	xidstatus = TransactionLogFetch(transactionId);

	/*
	 * If it's marked committed, it's committed.
	 */
	if (xidstatus == TRANSACTION_STATUS_COMMITTED)
		return true;

	/*
	 * If it's marked subcommitted, we have to check the parent recursively.
	 * However, if it's older than TransactionXmin, we can't look at
	 * pg_subtrans; instead assume that the parent crashed without cleaning up
	 * its children.
	 *
	 * Originally we Assert'ed that the result of SubTransGetParent was not
	 * zero. However with the introduction of prepared transactions, there can
	 * be a window just after database startup where we do not have complete
	 * knowledge in pg_subtrans of the transactions after TransactionXmin.
	 * StartupSUBTRANS() has ensured that any missing information will be
	 * zeroed.  Since this case should not happen under normal conditions, it
	 * seems reasonable to emit a WARNING for it.
	 */
	if (xidstatus == TRANSACTION_STATUS_SUB_COMMITTED)
	{
		TransactionId parentXid;

		if (TransactionIdPrecedes(transactionId, TransactionXmin))
			return false;
		parentXid = SubTransGetParent(transactionId);
		if (!TransactionIdIsValid(parentXid))
		{
			elog(WARNING, "no pg_subtrans entry for subcommitted XID %u",
				 transactionId);
			return false;
		}
		return TransactionIdDidCommit(parentXid);
	}

	/*
	 * It's not committed.
	 */
	return false;
}

/*
 * TransactionIdDidAbort
 *		True iff transaction associated with the identifier did abort.
 *
 * Note:
 *		Assumes transaction identifier is valid and exists in clog.
 */
bool							/* true if given transaction aborted */
TransactionIdDidAbort(TransactionId transactionId)
{
	XidStatus	xidstatus;

	xidstatus = TransactionLogFetch(transactionId);

	/*
	 * If it's marked aborted, it's aborted.
	 */
	if (xidstatus == TRANSACTION_STATUS_ABORTED)
		return true;

	/*
	 * If it's marked subcommitted, we have to check the parent recursively.
	 * However, if it's older than TransactionXmin, we can't look at
	 * pg_subtrans; instead assume that the parent crashed without cleaning up
	 * its children.
	 */
	if (xidstatus == TRANSACTION_STATUS_SUB_COMMITTED)
	{
		TransactionId parentXid;

		if (TransactionIdPrecedes(transactionId, TransactionXmin))
			return true;
		parentXid = SubTransGetParent(transactionId);
		if (!TransactionIdIsValid(parentXid))
		{
			/* see notes in TransactionIdDidCommit */
			elog(WARNING, "no pg_subtrans entry for subcommitted XID %u",
				 transactionId);
			return true;
		}
		return TransactionIdDidAbort(parentXid);
	}

	/*
	 * It's not aborted.
	 */
	return false;
}

/*
 * TransactionIdIsKnownCompleted
 *		True iff transaction associated with the identifier is currently
 *		known to have either committed or aborted.
 *
 * This does NOT look into pg_xact but merely probes our local cache
 * (and so it's not named TransactionIdDidComplete, which would be the
 * appropriate name for a function that worked that way).  The intended
 * use is just to short-circuit TransactionIdIsInProgress calls when doing
 * repeated tqual.c checks for the same XID.  If this isn't extremely fast
 * then it will be counterproductive.
 *
 * Note:
 *		Assumes transaction identifier is valid.
 */
bool
TransactionIdIsKnownCompleted(TransactionId transactionId)
{
	if (TransactionIdEquals(transactionId, cachedFetchXid))
	{
		/* If it's in the cache at all, it must be completed. */
		return true;
	}

	return false;
}

/*
 * TransactionIdCommitTree
 *		Marks the given transaction and children as committed
 *
 * "xid" is a toplevel transaction commit, and the xids array contains its
 * committed subtransactions.
 *
 * This commit operation is not guaranteed to be atomic, but if not, subxids
 * are correctly marked subcommit first.
 */
void
TransactionIdCommitTree(TransactionId xid, int nxids, TransactionId *xids)
{
	TransactionIdSetTreeStatus(xid, nxids, xids,
							   TRANSACTION_STATUS_COMMITTED,
							   InvalidXLogRecPtr);
}

/*
 * TransactionIdAsyncCommitTree
 *		Same as above, but for async commits.  The commit record LSN is needed.
 */
void
TransactionIdAsyncCommitTree(TransactionId xid, int nxids, TransactionId *xids,
							 XLogRecPtr lsn)
{
	TransactionIdSetTreeStatus(xid, nxids, xids,
							   TRANSACTION_STATUS_COMMITTED, lsn);
}

/*
 * TransactionIdAbortTree
 *		Marks the given transaction and children as aborted.
 *
 * "xid" is a toplevel transaction commit, and the xids array contains its
 * committed subtransactions.
 *
 * We don't need to worry about the non-atomic behavior, since any onlookers
 * will consider all the xacts as not-yet-committed anyway.
 */
void
TransactionIdAbortTree(TransactionId xid, int nxids, TransactionId *xids)
{
	TransactionIdSetTreeStatus(xid, nxids, xids,
							   TRANSACTION_STATUS_ABORTED, InvalidXLogRecPtr);
}

/*
 * TransactionIdPrecedes --- is id1 logically < id2?
 */
bool
TransactionIdPrecedes(TransactionId id1, TransactionId id2)
{
	/*
	 * If either ID is a permanent XID then we can just do unsigned
	 * comparison.  If both are normal, do a modulo-2^32 comparison.
	 */
	int32		diff;
	/* 若其中一个不是普通id，则其一定较新（较大） */
	if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
		return (id1 < id2);

	/* 两个事务ID相减后转为int 32类型 
	 * 由于int 32带符号，需要用第一位表示符号位，所以它能表示的正数比unsigned int 32
	 * 类型少一半，int 32的数据取值范围为[-2^(n-1),2^(n-1)-1]，即[-2^31,2^31-1]。
	 * 当两个txid相减结果>2^31时，转为int 32后其实是个负数（符号位从0变成了1）.
	 * 
	 * 例如：d1=2^31+101，id2=100。id1-id2=2^31+1，
	 * 用二进制表示即：100...中间30个0...001。当转为int 32后，由于第一位为符号位，
	 * 而1表示负数，所以转换后这个值其实就是-1，小于0，因此txid=2^31+101的事务反而要旧。
	 * 
	 * 上面的方法还是有问题，如果上面的100真的是非常非常旧的事务，那它确实应该被2^31+101
	 * 这个事务看见，此时上面的判断就是错的。
	 * 
	 * 也就是说如果id2确实是回卷前的txid，上面的判断方法就会出现问题。为了避免这种问题，
	 * pg必须保证一个数据库中两个有效的事务之间的年龄最多是2^31（同一个数据库中，存在的
	 * 最旧和最新两个事务txid相差不得超过2^31）
	*/
	diff = (int32) (id1 - id2);
	return (diff < 0);
}

/*
 * TransactionIdPrecedesOrEquals --- is id1 logically <= id2?
 */
bool
TransactionIdPrecedesOrEquals(TransactionId id1, TransactionId id2)
{
	int32		diff;

	if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
		return (id1 <= id2);

	diff = (int32) (id1 - id2);
	return (diff <= 0);
}

/*
 * TransactionIdFollows --- is id1 logically > id2?
 */
bool
TransactionIdFollows(TransactionId id1, TransactionId id2)
{
	int32		diff;

	if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
		return (id1 > id2);

	diff = (int32) (id1 - id2);
	return (diff > 0);
}

/*
 * TransactionIdFollowsOrEquals --- is id1 logically >= id2?
 */
bool
TransactionIdFollowsOrEquals(TransactionId id1, TransactionId id2)
{
	int32		diff;

	if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
		return (id1 >= id2);

	diff = (int32) (id1 - id2);
	return (diff >= 0);
}


/*
 * TransactionIdLatest --- get latest XID among a main xact and its children
 */
TransactionId
TransactionIdLatest(TransactionId mainxid,
					int nxids, const TransactionId *xids)
{
	TransactionId result;

	/*
	 * In practice it is highly likely that the xids[] array is sorted, and so
	 * we could save some cycles by just taking the last child XID, but this
	 * probably isn't so performance-critical that it's worth depending on
	 * that assumption.  But just to show we're not totally stupid, scan the
	 * array back-to-front to avoid useless assignments.
	 */
	result = mainxid;
	while (--nxids >= 0)
	{
		if (TransactionIdPrecedes(result, xids[nxids]))
			result = xids[nxids];
	}
	return result;
}


/*
 * TransactionIdGetCommitLSN
 *
 * This function returns an LSN that is late enough to be able
 * to guarantee that if we flush up to the LSN returned then we
 * will have flushed the transaction's commit record to disk.
 *
 * The result is not necessarily the exact LSN of the transaction's
 * commit record!  For example, for long-past transactions (those whose
 * clog pages already migrated to disk), we'll return InvalidXLogRecPtr.
 * Also, because we group transactions on the same clog page to conserve
 * storage, we might return the LSN of a later transaction that falls into
 * the same group.
 */
XLogRecPtr
TransactionIdGetCommitLSN(TransactionId xid)
{
	XLogRecPtr	result;

	/*
	 * Currently, all uses of this function are for xids that were just
	 * reported to be committed by TransactionLogFetch, so we expect that
	 * checking TransactionLogFetch's cache will usually succeed and avoid an
	 * extra trip to shared memory.
	 */
	if (TransactionIdEquals(xid, cachedFetchXid))
		return cachedCommitLSN;

	/* Special XIDs are always known committed */
	if (!TransactionIdIsNormal(xid))
		return InvalidXLogRecPtr;

	/*
	 * Get the transaction status.
	 */
	(void) TransactionIdGetStatus(xid, &result);

	return result;
}
