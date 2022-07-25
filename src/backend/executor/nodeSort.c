/*-------------------------------------------------------------------------
 *
 * nodeSort.c
 *	  Routines to handle sorting of relations.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeSort.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeSort.h"
#include "miscadmin.h"
#include "utils/tuplesort.h"


/* ----------------------------------------------------------------
 *		ExecSort
 *
 *		Sorts tuples from the outer subtree of the node using tuplesort,
 *		which saves the results in a temporary file or memory. After the
 *		initial call, returns a tuple from the file with each call.
 *
 *		Conditions:
 *		  -- none.
 *
 *		Initial States:
 *		  -- the outer child is prepared to return the first tuple.
 * ----------------------------------------------------------------
 * 
 * Sort 节点(排序节点)用于对于下层节点的输出结果进行排序，该节点只有左子节点。排序节点先
 * 将下层节点返回的所有元组缓存起来，然后进行排序。由于缓存结果可能很多，因此不可避免地会
 * 用到临时文件进行存储，这种情况下 Sort 节点将使用外排序方法
 * 
 * 参考 https://www.cnblogs.com/flying-tiger/p/8120046.html
 * 
 * postgres=# explain select  id,xxx  from test order by id ;
 * 
 *                               QUERY PLAN
 * ----------------------------------------------------------------------
 *  Sort  (cost=2904727.34..2929714.96 rows=9995048 width=34)
 *    Sort Key: id
 *    ->  Seq Scan on test  (cost=0.00..376141.48 rows=9995048 width=34)
 * (3 行)
 * 
 * postgresql 内部给了很多针对性的调试宏和参数，这里记录一下。
 *
 * 在 postgresql.conf 文件中有一个 trace_sort 参数可以在日志中打印 Sort 中的节点的
 * 初始化执行和结束信息
 */
static TupleTableSlot *
ExecSort(PlanState *pstate)
{
	SortState  *node = castNode(SortState, pstate);
	EState	   *estate;
	ScanDirection dir;
	Tuplesortstate *tuplesortstate;
	TupleTableSlot *slot;

	CHECK_FOR_INTERRUPTS();

	/*
	 * get state info from node
	 */
	SO1_printf("ExecSort: %s\n",
			   "entering routine");

	estate = node->ss.ps.state;
	dir = estate->es_direction;
	tuplesortstate = (Tuplesortstate *) node->tuplesortstate;

	/*
	 * If first time through, read all tuples from outer plan and pass them to
	 * tuplesort.c. Subsequent calls just fetch tuples from tuplesort.
	 */

	if (!node->sort_Done)
	{
		Sort	   *plannode = (Sort *) node->ss.ps.plan;
		PlanState  *outerNode;
		TupleDesc	tupDesc;

		SO1_printf("ExecSort: %s\n",
				   "sorting subplan");

		/*
		 * Want to scan subplan in the forward direction while creating the
		 * sorted data.
		 */
		estate->es_direction = ForwardScanDirection;

		/*
		 * Initialize tuplesort module.
		 */
		SO1_printf("ExecSort: %s\n",
				   "calling tuplesort_begin");

		outerNode = outerPlanState(node);
		tupDesc = ExecGetResultType(outerNode);
		/* 首次调用中会首先通过 tuplesort_begin_heap 对元组缓存结构初始化，返回一个
		 * Tuplesortstate 结构
		 */
		tuplesortstate = tuplesort_begin_heap(tupDesc,
											  plannode->numCols,
											  plannode->sortColIdx,
											  plannode->sortOperators,
											  plannode->collations,
											  plannode->nullsFirst,
											  work_mem,
											  node->randomAccess);
		/* 针对是否存在 limit 字段设置返回元组的 bound 数以及其它的一些 flags */
		if (node->bounded)
			tuplesort_set_bound(tuplesortstate, node->bound);
		node->tuplesortstate = (void *) tuplesortstate;

		/*
		 * Scan the subplan and feed all the tuples to tuplesort.
		 */

		for (;;)
		{
			/* 执行下层节点获取元组 */
			slot = ExecProcNode(outerNode);

			if (TupIsNull(slot))
				break;
			/* 下层元组加入 tuplesortstate */
			tuplesort_puttupleslot(tuplesortstate, slot);
		}

		/*
		 * Complete the sort.
		 * 获取到下层节点的所有元组之后，调用 tuplesort_performsort 对缓存元组进行排序
		 */
		tuplesort_performsort(tuplesortstate);

		/*
		 * restore to user specified direction
		 */
		estate->es_direction = dir;

		/*
		 * finally set the sorted flag to true
		 */
		node->sort_Done = true;
		node->bounded_Done = node->bounded;
		node->bound_Done = node->bound;
		SO1_printf("ExecSort: %s\n", "sorting done");
	}

	SO1_printf("ExecSort: %s\n",
			   "retrieving tuple from tuplesort");

	/*
	 * Get the first or next tuple from tuplesort. Returns NULL if no more
	 * tuples.  Note that we only rely on slot tuple remaining valid until the
	 * next fetch from the tuplesort.
	 * 
	 * 第一次调用结束后, 后续对 Sort 节点的执行都将直接调用 tuplesort_gettupleslot 从缓
	 * 存中返回一个元组。比如说你要返回 100 个元组，这里就要重复执行1+99(这个1是最开始的第
	 * 一次)次得到结果
	 */
	slot = node->ss.ps.ps_ResultTupleSlot;
	(void) tuplesort_gettupleslot(tuplesortstate,
								  ScanDirectionIsForward(dir),
								  false, slot, NULL);
	/* 输出元组 */
	return slot;
}

/* ----------------------------------------------------------------
 *		ExecInitSort
 *
 *		Creates the run-time state information for the sort node
 *		produced by the planner and initializes its outer subtree.
 * ----------------------------------------------------------------
 * 
 * 由于 Sort 节点仅对元组进行排序，不需要做投影和选择操作，因此在 Sort 节点的初始
 * 化过程(ExecInitSort 函数)中不会对 Plan 结构中的投影(targetlist)和选择(qual)链表
 * 进行初始化
 */
SortState *
ExecInitSort(Sort *node, EState *estate, int eflags)
{
	SortState  *sortstate;

	SO1_printf("ExecInitSort: %s\n",
			   "initializing sort node");

	/*
	 * create state structure
	 */
	sortstate = makeNode(SortState);
	sortstate->ss.ps.plan = (Plan *) node;
	sortstate->ss.ps.state = estate;
	sortstate->ss.ps.ExecProcNode = ExecSort;

	/*
	 * We must have random access to the sort output to do backward scan or
	 * mark/restore.  We also prefer to materialize the sort output if we
	 * might be called on to rewind and replay it many times.
	 */
	sortstate->randomAccess = (eflags & (EXEC_FLAG_REWIND |
										 EXEC_FLAG_BACKWARD |
										 EXEC_FLAG_MARK)) != 0;

	sortstate->bounded = false;
	sortstate->sort_Done = false;
	sortstate->tuplesortstate = NULL;

	/*
	 * Miscellaneous initialization
	 *
	 * Sort nodes don't initialize their ExprContexts because they never call
	 * ExecQual or ExecProject.
	 */

	/*
	 * tuple table initialization
	 *
	 * sort nodes only return scan tuples from their sorted relation.
	 */
	ExecInitResultTupleSlot(estate, &sortstate->ss.ps);
	ExecInitScanTupleSlot(estate, &sortstate->ss);

	/*
	 * initialize child nodes
	 *
	 * We shield the child node from the need to support REWIND, BACKWARD, or
	 * MARK/RESTORE.
	 */
	eflags &= ~(EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK);

	outerPlanState(sortstate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * initialize tuple type.  no need to initialize projection info because
	 * this node doesn't do projections.
	 */
	ExecAssignResultTypeFromTL(&sortstate->ss.ps);
	ExecAssignScanTypeFromOuterPlan(&sortstate->ss);
	sortstate->ss.ps.ps_ProjInfo = NULL;

	SO1_printf("ExecInitSort: %s\n",
			   "sort node initialized");

	return sortstate;
}

/* ----------------------------------------------------------------
 *		ExecEndSort(node)
 * ----------------------------------------------------------------
 */
void
ExecEndSort(SortState *node)
{
	SO1_printf("ExecEndSort: %s\n",
			   "shutting down sort node");

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	/* must drop pointer to sort result tuple */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	/*
	 * Release tuplesort resources
	 */
	if (node->tuplesortstate != NULL)
		tuplesort_end((Tuplesortstate *) node->tuplesortstate);
	node->tuplesortstate = NULL;

	/*
	 * shut down the subplan
	 */
	ExecEndNode(outerPlanState(node));

	SO1_printf("ExecEndSort: %s\n",
			   "sort node shutdown");
}

/* ----------------------------------------------------------------
 *		ExecSortMarkPos
 *
 *		Calls tuplesort to save the current position in the sorted file.
 * ----------------------------------------------------------------
 */
void
ExecSortMarkPos(SortState *node)
{
	/*
	 * if we haven't sorted yet, just return
	 */
	if (!node->sort_Done)
		return;

	tuplesort_markpos((Tuplesortstate *) node->tuplesortstate);
}

/* ----------------------------------------------------------------
 *		ExecSortRestrPos
 *
 *		Calls tuplesort to restore the last saved sort file position.
 * ----------------------------------------------------------------
 */
void
ExecSortRestrPos(SortState *node)
{
	/*
	 * if we haven't sorted yet, just return.
	 */
	if (!node->sort_Done)
		return;

	/*
	 * restore the scan to the previously marked position
	 */
	tuplesort_restorepos((Tuplesortstate *) node->tuplesortstate);
}

void
ExecReScanSort(SortState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	/*
	 * If we haven't sorted yet, just return. If outerplan's chgParam is not
	 * NULL then it will be re-scanned by ExecProcNode, else no reason to
	 * re-scan it at all.
	 */
	if (!node->sort_Done)
		return;

	/* must drop pointer to sort result tuple */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	/*
	 * If subnode is to be rescanned then we forget previous sort results; we
	 * have to re-read the subplan and re-sort.  Also must re-sort if the
	 * bounded-sort parameters changed or we didn't select randomAccess.
	 *
	 * Otherwise we can just rewind and rescan the sorted output.
	 */
	if (outerPlan->chgParam != NULL ||
		node->bounded != node->bounded_Done ||
		node->bound != node->bound_Done ||
		!node->randomAccess)
	{
		node->sort_Done = false;
		tuplesort_end((Tuplesortstate *) node->tuplesortstate);
		node->tuplesortstate = NULL;

		/*
		 * if chgParam of subnode is not null then plan will be re-scanned by
		 * first ExecProcNode.
		 */
		if (outerPlan->chgParam == NULL)
			ExecReScan(outerPlan);
	}
	else
		tuplesort_rescan((Tuplesortstate *) node->tuplesortstate);
}
