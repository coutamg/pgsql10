/*-------------------------------------------------------------------------
 *
 * nodeNestloop.c
 *	  routines to support nest-loop joins
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeNestloop.c
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		ExecNestLoop	 - process a nestloop join of two plans
 *		ExecInitNestLoop - initialize the join
 *		ExecEndNestLoop  - shut down the join
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeNestloop.h"
#include "miscadmin.h"
#include "utils/memutils.h"


/* ----------------------------------------------------------------
 *		ExecNestLoop(node)
 *
 * old comments
 *		Returns the tuple joined from inner and outer tuples which
 *		satisfies the qualification clause.
 *
 *		It scans the inner relation to join with current outer tuple.
 *
 *		If none is found, next tuple from the outer relation is retrieved
 *		and the inner relation is scanned from the beginning again to join
 *		with the outer tuple.
 *
 *		NULL is returned if all the remaining outer tuples are tried and
 *		all fail to join with the inner tuples.
 *
 *		NULL is also returned if there is no tuple from inner relation.
 *
 *		Conditions:
 *		  -- outerTuple contains current tuple from outer relation and
 *			 the right son(inner relation) maintains "cursor" at the tuple
 *			 returned previously.
 *				This is achieved by maintaining a scan position on the outer
 *				relation.
 *
 *		Initial States:
 *		  -- the outer child and the inner child
 *			   are prepared to return the first tuple.
 * ----------------------------------------------------------------
 */
/*
 * FOR each tuple r in R DO
 *     IF r and s join to make a tuple t THEN
 *	       output t;
 * 
 * 为了迭代实现此方法, NestLoopState 中定义了字段 nl_NeedNewOuter 和 nl_MatchedOuter.
 * 当元组处于内层循环时, nl_NeedNewOuter 为 false,内层循环结束时 nl_NeedNewOuter 设置
 * 为 true
 * 
 * 为了能够处理 Left Outer Join 和 Anti Join, 需要知道内层循环是否找到了满足连接条件的内层
 * 元组,此信息由 nl_MatchedOuter 记录,当内层循环找到符合条件的元组时将其标记为 true.
 * 
 * NestLoop 节点的初始化工作需要为 Left Outer Join 和 Anti Join 两种可能用到空元组的情况构
 * 造空元组,并存放在 NestLoopState 的 nl_NullInnerTupleSlot 中,还将进行如下两个操作:
 *   1)将 nl_NeedNewOuter 标记为 true,表示需要获取左子节点元组。
 *   2)将 nl_MatchedOuter 标记为 false,表示没有找到与当前左子节点元组匹配的右子节点元组.
 * 
 * NestLoop 执行过程需要循环执行如下操作:
 *	 1) 如果 nl_NeedNew Outer 为true,则从左子节点获取元组,若获取的元组为 NULL 则返回空元
 *	    组并结束执行过程。如果 nl_NeedNewOuter为 false,则继续进行步骤2。
 *   2) 从右子节点获取元组,若为 NULL 表明内层扫描完成,设置 nl_NeedNewOuter 为 true,跳过
 *      步骤3继续循环。
 *   3) 判断右子节点元组是否与当前左子节点元组符合连接条件,若符合则返回连接结果。
 * 
 * 以上过程能够完成 Inner Join 的递归执行过程。但是为了支持其他几种连接则还需要如下两个
 * 特殊的处理:
 *   1) 当找到符合连接条件的元组后将 nl_MatchedOuter 标记为 true。内层扫描完毕时,通过判断
 *      nl_MatchedOuter 即可知道是否已经找到满足连接条件的元组,在处理 Left Outer Join 和
 * 		Anti Join 时需要进行与空元组的连接,然后将 nl_MatchedOuter 设置为 false。
 *   2) 当找到满足匹配条件的元组后,对于 Semi Join 和 Anti Join 方法需要设置 nl_NeedNewOuter
 *      为 true。区别在于 Anti Join 需要不满足连接条件才能返回,所以要跳过返回连接结果继续执行
 *      循环。
 */
static TupleTableSlot *
ExecNestLoop(PlanState *pstate)
{
	NestLoopState *node = castNode(NestLoopState, pstate);
	NestLoop   *nl;
	PlanState  *innerPlan;
	PlanState  *outerPlan;
	TupleTableSlot *outerTupleSlot;
	TupleTableSlot *innerTupleSlot;
	ExprState  *joinqual;
	ExprState  *otherqual;
	ExprContext *econtext;
	ListCell   *lc;

	CHECK_FOR_INTERRUPTS();

	/*
	 * get information from the node
	 */
	ENL1_printf("getting info from node");

	nl = (NestLoop *) node->js.ps.plan;
	joinqual = node->js.joinqual;
	otherqual = node->js.ps.qual;
	outerPlan = outerPlanState(node);
	innerPlan = innerPlanState(node);
	econtext = node->js.ps.ps_ExprContext;

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.
	 */
	ResetExprContext(econtext);

	/*
	 * Ok, everything is setup for the join so now loop until we return a
	 * qualifying join tuple.
	 */
	ENL1_printf("entering main loop");
	/* 每一次循环都通过 ExecProcNode 函数从计划节点状态树中获取一个元组,然后对该
	 * 元组进行相应的处理(增删查改),然后返回处理的结果。当ExecProcNode 从计划节点
	 * 状态树中再也取不到有效的元组时结束循环过程
	 */
	for (;;)
	{
		/*
		 * If we don't have an outer tuple, get the next one and reset the
		 * inner scan.
		 * 
		 * 是否需要左孩子节点数据
		 */
		if (node->nl_NeedNewOuter)
		{
			ENL1_printf("getting new outer tuple");
			/* 执行左孩子节点获取输入数据 */
			outerTupleSlot = ExecProcNode(outerPlan);

			/*
			 * if there are no more outer tuples, then the join is complete..
			 * 若获取的元组为 NULL 则返回空元组并结束执行过程
			 */
			if (TupIsNull(outerTupleSlot))
			{
				ENL1_printf("no outer tuple, ending join");
				return NULL;
			}

			ENL1_printf("saving new outer tuple information");
			econtext->ecxt_outertuple = outerTupleSlot;
			/* 结束外层循环进入内层循环 */
			node->nl_NeedNewOuter = false;
			/* 内层循环结束后重制 */
			node->nl_MatchedOuter = false;

			/*
			 * fetch the values of any outer Vars that must be passed to the
			 * inner scan, and store them in the appropriate PARAM_EXEC slots.
			 * 
			 * 参考 https://www.cnblogs.com/flying-tiger/p/8331425.html
			 * https://blog.csdn.net/cuichao1900/article/details/100394724
			 */
			foreach(lc, nl->nestParams)
			{
				NestLoopParam *nlp = (NestLoopParam *) lfirst(lc);
				int			paramno = nlp->paramno;
				ParamExecData *prm;

				prm = &(econtext->ecxt_param_exec_vals[paramno]);
				/* Param value should be an OUTER_VAR var */
				Assert(IsA(nlp->paramval, Var));
				Assert(nlp->paramval->varno == OUTER_VAR);
				Assert(nlp->paramval->varattno > 0);
				prm->value = slot_getattr(outerTupleSlot,
										  nlp->paramval->varattno,
										  &(prm->isnull));
				/* Flag parameter value as changed */
				innerPlan->chgParam = bms_add_member(innerPlan->chgParam,
													 paramno);
			}

			/*
			 * now rescan the inner plan
			 */
			ENL1_printf("rescanning inner plan");
			ExecReScan(innerPlan);
		}

		/*
		 * we have an outerTuple, try to get the next inner tuple.
		 */
		ENL1_printf("getting new inner tuple");

		/* 从右子节点获取元组 */
		innerTupleSlot = ExecProcNode(innerPlan);
		econtext->ecxt_innertuple = innerTupleSlot;

		/* 判断内层循环扫描完成, 扫描完成时 tuple is null */
		if (TupIsNull(innerTupleSlot))
		{
			ENL1_printf("no inner tuple, need new outer tuple");
			/* 内层循环扫完, 结束内层循环进入外层循环 */
			node->nl_NeedNewOuter = true;

			/* 内层扫描完毕时,通过判断 nl_MatchedOuter 即可知道是否已经找到满足连接条件的
			 * 元组 
			 */
			if (!node->nl_MatchedOuter &&
				(node->js.jointype == JOIN_LEFT ||
				 node->js.jointype == JOIN_ANTI))
			{
				/*
				 * We are doing an outer join and there were no join matches
				 * for this outer tuple.  Generate a fake join tuple with
				 * nulls for the inner tuple, and return it if it passes the
				 * non-join quals.
				 */
				econtext->ecxt_innertuple = node->nl_NullInnerTupleSlot;

				ENL1_printf("testing qualification for outer-join tuple");

				if (otherqual == NULL || ExecQual(otherqual, econtext))
				{
					/*
					 * qualification was satisfied so we project and return
					 * the slot containing the result tuple using
					 * ExecProject().
					 */
					ENL1_printf("qualification succeeded, projecting tuple");

					return ExecProject(node->js.ps.ps_ProjInfo);
				}
				else
					InstrCountFiltered2(node, 1);
			}

			/*
			 * Otherwise just return to top of loop for a new outer tuple.
			 */
			continue;
		}

		/*
		 * at this point we have a new pair of inner and outer tuples so we
		 * test the inner and outer tuples to see if they satisfy the node's
		 * qualification.
		 *
		 * Only the joinquals determine MatchedOuter status, but all quals
		 * must pass to actually return the tuple.
		 */
		ENL1_printf("testing qualification");
		/* 判断右子节点元组是否与当前左子节点元组符合连接条件 */
		if (ExecQual(joinqual, econtext))
		{
			/* 找到了满足连接条件的内层元组 */
			node->nl_MatchedOuter = true;

			/* In an antijoin, we never return a matched tuple */
			/* Anti Join 需要不满足连接条件才能返回, 所以要跳过返回连接结果继续执行 */
			if (node->js.jointype == JOIN_ANTI)
			{
				node->nl_NeedNewOuter = true;
				continue;		/* return to top of loop */
			}

			/*
			 * If we only need to join to the first matching inner tuple, then
			 * consider returning this one, but after that continue with next
			 * outer tuple.
			 * 
			 * 处理 Semi Join 有一个匹配即可
			 */
			if (node->js.single_match)
				node->nl_NeedNewOuter = true;

			if (otherqual == NULL || ExecQual(otherqual, econtext))
			{
				/*
				 * qualification was satisfied so we project and return the
				 * slot containing the result tuple using ExecProject().
				 */
				ENL1_printf("qualification succeeded, projecting tuple");

				return ExecProject(node->js.ps.ps_ProjInfo);
			}
			else
				InstrCountFiltered2(node, 1);
		}
		else
			InstrCountFiltered1(node, 1);

		/*
		 * Tuple fails qual, so free per-tuple memory and try again.
		 */
		ResetExprContext(econtext);

		ENL1_printf("qualification failed, looping");
	}
}

/* ----------------------------------------------------------------
 *		ExecInitNestLoop
 * ----------------------------------------------------------------
 * 
 * 由于 NestLoop 节点有两个子节点,因此 ExecInitNestLoop 会先调用 ExecInitNode 对其
 * 左子节点进行初始化, 并将其返回的 PlanState 结构指针存放在为 NestLoop 构造的 NestLoopState
 * 结构的 lefttree 字段中; 然后以同样的方式初始化右子节点,将返回的 PlanState 结构指针存放于
 * NestLoopState 的 righttree 字段中。同样,如果左右子节点还有下层节点,初始化过程将以完全相同的
 * 方式递归下去,直到到达查询计划树的叶子节点。而在初始化过程中构造的 PlanState 子树也会层层返回
 * 给上层节点,并被链接在上层节点的 PlanState 结构中,最终构造出完整的 PlanState 树
 * 
 * 参考 ExecInitxxx.png
 * 
 * 例如: nodeNestloop.png
 */
NestLoopState *
ExecInitNestLoop(NestLoop *node, EState *estate, int eflags)
{
	NestLoopState *nlstate;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	NL1_printf("ExecInitNestLoop: %s\n",
			   "initializing node");

	/*
	 * create state structure
	 */
	nlstate = makeNode(NestLoopState);
	nlstate->js.ps.plan = (Plan *) node;
	nlstate->js.ps.state = estate;
	/* 查询计划的执行函数 */
	nlstate->js.ps.ExecProcNode = ExecNestLoop;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &nlstate->js.ps);

	/*
	 * initialize child expressions
	 */
	nlstate->js.ps.qual =
		ExecInitQual(node->join.plan.qual, (PlanState *) nlstate);
	nlstate->js.jointype = node->join.jointype;
	nlstate->js.joinqual =
		ExecInitQual(node->join.joinqual, (PlanState *) nlstate);

	/*
	 * initialize child nodes
	 *
	 * If we have no parameters to pass into the inner rel from the outer,
	 * tell the inner child that cheap rescans would be good.  If we do have
	 * such parameters, then there is no point in REWIND support at all in the
	 * inner child, because it will always be rescanned with fresh parameter
	 * values.
	 */
	/* 调用 ExecInitNode 对其左子节点进行初始化, 并将其返回的 PlanState 结构指针存放在
	 * 为 NestLoop 构造的 NestLoopState 结构的 lefttree 字段中
	 */
	outerPlanState(nlstate) = ExecInitNode(outerPlan(node), estate, eflags);
	if (node->nestParams == NIL)
		eflags |= EXEC_FLAG_REWIND;
	else
		eflags &= ~EXEC_FLAG_REWIND;
	/* 对右子节点初始化 */
	innerPlanState(nlstate) = ExecInitNode(innerPlan(node), estate, eflags);

	/*
	 * tuple table initialization
	 * 在 Estate.es_tupleTable 中申请结果元组存储结构
	 */
	ExecInitResultTupleSlot(estate, &nlstate->js.ps);

	/*
	 * detect whether we need only consider the first matching inner tuple
	 */
	nlstate->js.single_match = (node->join.inner_unique ||
								node->join.jointype == JOIN_SEMI);

	/* set up null tuples for outer joins, if needed */
	switch (node->join.jointype)
	{
		case JOIN_INNER:
		case JOIN_SEMI:
			break;
		/* Left Outer Join 和 Anti Join 两种可能用到空元组的情况构造空元组 */
		case JOIN_LEFT:
		case JOIN_ANTI:
			nlstate->nl_NullInnerTupleSlot =
				ExecInitNullTupleSlot(estate,
									  ExecGetResultType(innerPlanState(nlstate)));
			break;
		default:
			elog(ERROR, "unrecognized join type: %d",
				 (int) node->join.jointype);
	}

	/*
	 * initialize tuple type and projection info
	 */
	ExecAssignResultTypeFromTL(&nlstate->js.ps);
	ExecAssignProjectionInfo(&nlstate->js.ps, NULL);

	/*
	 * finally, wipe the current outer tuple clean.
	 */
	/* 将 nl_NeedNewOuter 标记为 true,表示需要获取左子节点元组 */
	nlstate->nl_NeedNewOuter = true;
	/* 将 nl_MatchedOuter 标记为 false,表示没有找到与当前左子节点元组匹配的右子节点元组 */
	nlstate->nl_MatchedOuter = false;

	NL1_printf("ExecInitNestLoop: %s\n",
			   "node initialized");

	return nlstate;
}

/* ----------------------------------------------------------------
 *		ExecEndNestLoop
 *
 *		closes down scans and frees allocated storage
 * ----------------------------------------------------------------
 */
void
ExecEndNestLoop(NestLoopState *node)
{
	NL1_printf("ExecEndNestLoop: %s\n",
			   "ending node processing");

	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->js.ps);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->js.ps.ps_ResultTupleSlot);

	/*
	 * close down subplans
	 */
	ExecEndNode(outerPlanState(node));
	ExecEndNode(innerPlanState(node));

	NL1_printf("ExecEndNestLoop: %s\n",
			   "node processing ended");
}

/* ----------------------------------------------------------------
 *		ExecReScanNestLoop
 * ----------------------------------------------------------------
 */
void
ExecReScanNestLoop(NestLoopState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	/*
	 * If outerPlan->chgParam is not null then plan will be automatically
	 * re-scanned by first ExecProcNode.
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);

	/*
	 * innerPlan is re-scanned for each new outer tuple and MUST NOT be
	 * re-scanned from here or you'll get troubles from inner index scans when
	 * outer Vars are used as run-time keys...
	 */

	node->nl_NeedNewOuter = true;
	node->nl_MatchedOuter = false;
}
