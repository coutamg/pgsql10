/*-------------------------------------------------------------------------
 *
 * nodeRecursiveunion.c
 *	  routines to handle RecursiveUnion nodes.
 *
 * To implement UNION (without ALL), we need a hashtable that stores tuples
 * already seen.  The hash key is computed from the grouping columns.
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeRecursiveunion.c
 *
 *-------------------------------------------------------------------------
 *
 * RecursiveUnion节点用于处理递归定义的 UNION 语句
 * 
 * postgres=# explain WITH RECURSIVE t(n)AS(
 * VALUES (1)
 * UNION ALL
 * SELECT n +1 FROM t WHERE n < 100)
 * select sum(n) FROM t;
 *                                QUERY PLAN
 * -------------------------------------------------------------------------
 *  Aggregate  (cost=3.65..3.66 rows=1 width=4)
 *    CTE t
 *      ->  Recursive Union  (cost=0.00..2.95 rows=31 width=4)
 *            ->  Result  (cost=0.00..0.01 rows=1 width=0)
 *            ->  WorkTable Scan on t t_1  (cost=0.00..0.23 rows=3 width=4)
 *                  Filter: (n < 100)
 *    ->  CTE Scan on t  (cost=0.00..0.62 rows=31 width=4)
 * (7 rows)
 * 
 * 该査询定义了一个临时表并将 VALUES 子句给出的元组作为 t 的初始值，“SELECT n + 1 FROM t 
 * WHERE n < 100 ”将 t 中属性 n 小于 100 的元组的 n 值加 1 并返回，UNION ALL操作将
 * SELECT 语句产生的新元组集合合并到临时表 t 中。 以上的 SELECT 和 UNION 操作将递归地执行
 * 下去,直到 SELECT 语句没有新元组输出为止。最后执行 “SELECT sum (n) FROM t” 对 t 中元
 * 组的 n 值做求和操作。
 *
 * 上述査询由 RecursiveUnion 节点处理。有一个初始输入集作为递归过程的初始数据(如上例中
 * “VALUES(1)”)，然后进行递归部分(上例中的 “SELECTn + 1 FROM t WHERE n < 100”)的处理
 * 得到输出，并将新得到的输出与初始输入合并后作为下次递归的输入(例如第一次递归处理时新的输出集
 * 为{2},与初始输入合并后为{1, 2})
 */
#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeRecursiveunion.h"
#include "miscadmin.h"
#include "utils/memutils.h"



/*
 * Initialize the hash table to empty.
 */
static void
build_hash_table(RecursiveUnionState *rustate)
{
	RecursiveUnion *node = (RecursiveUnion *) rustate->ps.plan;

	Assert(node->numCols > 0);
	Assert(node->numGroups > 0);

	rustate->hashtable = BuildTupleHashTable(node->numCols,
											 node->dupColIdx,
											 rustate->eqfunctions,
											 rustate->hashfunctions,
											 node->numGroups,
											 0,
											 rustate->tableContext,
											 rustate->tempContext,
											 false);
}


/* ----------------------------------------------------------------
 *		ExecRecursiveUnion(node)
 *
 *		Scans the recursive query sequentially and returns the next
 *		qualifying tuple.
 *
 * 1. evaluate non recursive term and assign the result to RT
 *
 * 2. execute recursive terms
 *
 * 2.1 WT := RT
 * 2.2 while WT is not empty repeat 2.3 to 2.6. if WT is empty returns RT
 * 2.3 replace the name of recursive term with WT
 * 2.4 evaluate the recursive term and store into WT
 * 2.5 append WT to RT
 * 2.6 go back to 2.2
 * ----------------------------------------------------------------
 * 
 * 执行流程可以参考 nodeRecursiveunion_exec.png
 * 参考 https://www.cnblogs.com/flying-tiger/p/8378677.html
 */
static TupleTableSlot *
ExecRecursiveUnion(PlanState *pstate)
{
	RecursiveUnionState *node = castNode(RecursiveUnionState, pstate);
	PlanState  *outerPlan = outerPlanState(node);
	PlanState  *innerPlan = innerPlanState(node);
	RecursiveUnion *plan = (RecursiveUnion *) node->ps.plan;
	TupleTableSlot *slot;
	bool		isnew;

	CHECK_FOR_INTERRUPTS();

	/* 1. Evaluate non-recursive term */
	if (!node->recursing)
	{
		for (;;)
		{
			/* 执行左子节点 plan */
			slot = ExecProcNode(outerPlan);
			if (TupIsNull(slot))
				break;
			if (plan->numCols > 0)
			{
				/* Find or build hashtable entry for this tuple's group */
				/* 如果需要去重,则需要把返回的元组加入到 Hash表(hashtable)中 */
				LookupTupleHashEntry(node->hashtable, slot, &isnew);
				/* Must reset temp context after each hashtable lookup */
				MemoryContextReset(node->tempContext);
				/* Ignore tuple if already seen */
				if (!isnew)
					continue;
			}
			/* Each non-duplicate tuple goes to the working table ... */
			/* 将初始集存储在 working_table(图中缩写为 WT)指向的元组缓存结构中 */
			tuplestore_puttupleslot(node->working_table, slot);
			/* ... and to the caller */
			/* 将获取的元组直接返回 */
			return slot;
		}
		/* 递归初始化执行完毕 */
		node->recursing = true;
	}

	/* 2. Execute recursive term */
	for (;;)
	{
		/* 当处理完毕所有的左子节点计划后, 会执行右子节点计划以获取结果元组
		 * 其中, 右子节点计划中的 WorkTableScan 会以 working_table 为扫描对象
		 */
		slot = ExecProcNode(innerPlan);
		if (TupIsNull(slot))
		{
			/* Done if there's nothing in the intermediate table */
			/* working_table 没有满足要求的数据了, 一般需要两次走到这里才会结束 */
			if (node->intermediate_empty)
				break;

			/* done with old working table ... */
			tuplestore_end(node->working_table);

			/* intermediate table becomes working table */
			/* 每当 working_table 被扫描完毕, RecursiveUnion 节点的执行流程会将
			 * intermediate_table 赋值给 working_table 然后再次执行右子节点计划
			 * 获取元组, 直到无元组输出为止
			 */
			node->working_table = node->intermediate_table;

			/* create new empty intermediate table */
			node->intermediate_table = tuplestore_begin_heap(false, false,
															 work_mem);
			node->intermediate_empty = true;

			/* reset the recursive term */
			innerPlan->chgParam = bms_add_member(innerPlan->chgParam,
												 plan->wtParam);

			/* and continue fetching from recursive term */
			continue;
		}

		if (plan->numCols > 0)
		{
			/* Find or build hashtable entry for this tuple's group */
			/* 如果需要去重,右子节点计划的所有输出也会被存入同一个 Hash 表(hashtable),
			 * 若重复则不会输出
			 */
			LookupTupleHashEntry(node->hashtable, slot, &isnew);
			/* Must reset temp context after each hashtable lookup */
			MemoryContextReset(node->tempContext);
			/* Ignore tuple if already seen */
			if (!isnew)
				continue;
		}

		/* Else, tuple is good; stash it in intermediate table ... */
		/* working_table 有数据的时候, 则不能停止递归 */
		node->intermediate_empty = false;
		/* 右子节点扫描的结果缓存在另一个元组存储结构 intermediate_table */
		tuplestore_puttupleslot(node->intermediate_table, slot);
		/* ... and return it */
		/* 右子节点计划返回的结果元组将作为 RecursiveUnion 节点的输出 */
		return slot;
	}

	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecInitRecursiveUnionScan
 * ----------------------------------------------------------------
 */
RecursiveUnionState *
ExecInitRecursiveUnion(RecursiveUnion *node, EState *estate, int eflags)
{
	RecursiveUnionState *rustate;
	ParamExecData *prmdata;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	rustate = makeNode(RecursiveUnionState);
	rustate->ps.plan = (Plan *) node;
	rustate->ps.state = estate;
	rustate->ps.ExecProcNode = ExecRecursiveUnion;

	rustate->eqfunctions = NULL;
	rustate->hashfunctions = NULL;
	rustate->hashtable = NULL;
	rustate->tempContext = NULL;
	rustate->tableContext = NULL;

	/* initialize processing state */
	rustate->recursing = false;
	rustate->intermediate_empty = true;
	/* RecureiveUnionState 节点的 working_table 和 intermediate_table 字段初始化
	 * 元组缓存结构
	 */
	rustate->working_table = tuplestore_begin_heap(false, false, work_mem);
	rustate->intermediate_table = tuplestore_begin_heap(false, false, work_mem);

	/*
	 * If hashing, we need a per-tuple memory context for comparisons, and a
	 * longer-lived context to store the hash table.  The table can't just be
	 * kept in the per-query context because we want to be able to throw it
	 * away when rescanning.
	 * 
	 * 根据 numCols 是否为 0 来辨别是否需要在合并时进行去重
	 * 申请两个内存上下文用于去重操作
	 */
	if (node->numCols > 0)
	{
		rustate->tempContext =
			AllocSetContextCreate(CurrentMemoryContext,
								  "RecursiveUnion",
								  ALLOCSET_DEFAULT_SIZES);
		rustate->tableContext =
			AllocSetContextCreate(CurrentMemoryContext,
								  "RecursiveUnion hash table",
								  ALLOCSET_DEFAULT_SIZES);
	}

	/*
	 * Make the state structure available to descendant WorkTableScan nodes
	 * via the Param slot reserved for it.
	 */
	prmdata = &(estate->es_param_exec_vals[node->wtParam]);
	Assert(prmdata->execPlan == NULL);
	prmdata->value = PointerGetDatum(rustate);
	prmdata->isnull = false;

	/*
	 * Miscellaneous initialization
	 *
	 * RecursiveUnion plans don't have expression contexts because they never
	 * call ExecQual or ExecProject.
	 */
	Assert(node->plan.qual == NIL);

	/*
	 * RecursiveUnion nodes still have Result slots, which hold pointers to
	 * tuples, so we have to initialize them.
	 */
	ExecInitResultTupleSlot(estate, &rustate->ps);

	/*
	 * Initialize result tuple type and projection info.  (Note: we have to
	 * set up the result type before initializing child nodes, because
	 * nodeWorktablescan.c expects it to be valid.)
	 */
	ExecAssignResultTypeFromTL(&rustate->ps);
	rustate->ps.ps_ProjInfo = NULL;

	/*
	 * initialize child nodes
	 */
	outerPlanState(rustate) = ExecInitNode(outerPlan(node), estate, eflags);
	innerPlanState(rustate) = ExecInitNode(innerPlan(node), estate, eflags);

	/*
	 * If hashing, precompute fmgr lookup data for inner loop, and create the
	 * hash table.
	 * 
	 * 根据 numCols 是否为0来辨别是否需要在合并时进行去重
	 * 根据 dupOpemtors 字段初始化 RccursiveUnionState 节点的 eqfunctions 和
	 * hashfunctions 字段
	 */
	if (node->numCols > 0)
	{
		execTuplesHashPrepare(node->numCols,
							  node->dupOperators,
							  &rustate->eqfunctions,
							  &rustate->hashfunctions);
		/* 创建去重操作用的 Hash 表 */
		build_hash_table(rustate);
	}

	return rustate;
}

/* ----------------------------------------------------------------
 *		ExecEndRecursiveUnionScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndRecursiveUnion(RecursiveUnionState *node)
{
	/* Release tuplestores */
	tuplestore_end(node->working_table);
	tuplestore_end(node->intermediate_table);

	/* free subsidiary stuff including hashtable */
	if (node->tempContext)
		MemoryContextDelete(node->tempContext);
	if (node->tableContext)
		MemoryContextDelete(node->tableContext);

	/*
	 * clean out the upper tuple table
	 */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	/*
	 * close down subplans
	 */
	ExecEndNode(outerPlanState(node));
	ExecEndNode(innerPlanState(node));
}

/* ----------------------------------------------------------------
 *		ExecReScanRecursiveUnion
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecReScanRecursiveUnion(RecursiveUnionState *node)
{
	PlanState  *outerPlan = outerPlanState(node);
	PlanState  *innerPlan = innerPlanState(node);
	RecursiveUnion *plan = (RecursiveUnion *) node->ps.plan;

	/*
	 * Set recursive term's chgParam to tell it that we'll modify the working
	 * table and therefore it has to rescan.
	 */
	innerPlan->chgParam = bms_add_member(innerPlan->chgParam, plan->wtParam);

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.  Because of above, we only have to do this to the
	 * non-recursive term.
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);

	/* Release any hashtable storage */
	if (node->tableContext)
		MemoryContextResetAndDeleteChildren(node->tableContext);

	/* And rebuild empty hashtable if needed */
	if (plan->numCols > 0)
		build_hash_table(node);

	/* reset processing state */
	node->recursing = false;
	node->intermediate_empty = true;
	tuplestore_clear(node->working_table);
	tuplestore_clear(node->intermediate_table);
}
