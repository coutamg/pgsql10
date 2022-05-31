create table student ( 
	ID varchar(5),
	name varchar(20) not null, 
	dept_name varchar(20), 
	tot_cred numeric(3,0) 
	check (tot_cred >= 0), 
	primary key (ID)
);

SELECT 
  n.nspname as "Schema",
  c.relname as "Name", -- 表对象的名称
  CASE c.relkind 
  	WHEN 'r' THEN 'table' 
  	WHEN 'v' THEN 'view' 
  	WHEN 'm' THEN 'materialized view' 
  	WHEN 'i' THEN 'index' 
  	WHEN 'S' THEN 'sequence' 
  	WHEN 's' THEN 'special' 
  	WHEN 'f' THEN 'foreign table' 
  	WHEN 'p' THEN 'partitioned table' 
  	WHEN 'I' THEN 'partitioned index' 
  END as "Type",

  pg_catalog.pg_get_userbyid(c.relowner) as "Owner", 

  CASE c.relam 
    WHEN 2 THEN 'heap' 
   END as "Storage"

FROM pg_catalog.pg_class c LEFT JOIN pg_catalog.pg_namespace n 
     ON n.oid = c.relnamespace

WHERE c.relkind IN ('r','p','s','') -- relkind 数据库对象类型，r:表，s:序列, i:索引
	AND n.nspname !~ '^pg_toast'
  	AND c.relname OPERATOR(pg_catalog.~) '^(student)$' COLLATE pg_catalog.default
  	AND pg_catalog.pg_table_is_visible(c.oid)

ORDER BY 1,2;

-- COLLATE: 排序规则
-- OPERATOR: 主要用于PostgreSQL语句的WHERE子句中执行操作
-- 			Arithmetic, Comparison, Logical, Bitwise 操作

-- n.nspname !~ '^pg_toast': !~ 是 posix 正则表达式, 意思是不匹配正则表达式，区分大小写。
--							 '^pg_toast' 则是正则表达式，匹配以 pg_toast 开头的字符
--				 			 返回值: t (表示正则表达式匹配成功)。 f (表示正则表达式匹配失败)	
--	这里的意思是 n.nspname 不能是以 pg_toast 开头的字符


              List of relations
 Schema |  Name   | Type  |  Owner  | Storage
--------+---------+-------+---------+---------
 public | student | table | gpadmin |


-- 查询 namespace 数据
select * from pg_namespace;

  oid  |        nspname        | nspowner |              nspacl
-------+-----------------------+----------+----------------------------------
    99 | pg_toast              |       10 |
  6104 | pg_aoseg              |       10 |
  7012 | pg_bitmapindex        |       10 |
  6108 | pg_ostore             |       10 |
 12854 | pg_temp_0132767       |       10 |
 12855 | pg_toast_temp_0132767 |       10 |
    11 | pg_catalog            |       10 | {gpadmin=UC/gpadmin,=U/gpadmin}
  2200 | public                |       10 | {gpadmin=UC/gpadmin,=UC/gpadmin}
 12911 | information_schema    |       10 | {gpadmin=UC/gpadmin,=U/gpadmin}
 13184 | gp_toolkit            |       10 | {gpadmin=UC/gpadmin,=U/gpadmin}
(10 rows)


-- 查询 student 在 pg_class 中的数据
select oid, relname, relnamespace, relkind, relam from pg_class order by oid desc limit 10 ;

  oid  |            relname             | relnamespace | relkind | relam
-------+--------------------------------+--------------+---------+-------
 16390 | pg_ostore_stats_16384          |         6108 | r       |  5105
 16387 | pg_ostore_16384                |         6108 | r       |  5105
 16384 | student                        |         2200 | r       |  5104
 13343 | gp_workfile_mgr_used_diskspace |        13184 | v       |     0
 13338 | gp_workfile_usage_per_query    |        13184 | v       |     0
 13335 | gp_workfile_usage_per_segment  |        13184 | v       |     0
 13332 | gp_workfile_entries            |        13184 | v       |     0
 13327 | gp_resgroup_status_per_segment |        13184 | v       |     0
 13324 | gp_resgroup_status_per_host    |        13184 | v       |     0
 13321 | gp_resgroup_status             |        13184 | v       |     0
(10 rows)


-- 查看 index 对应的 oid select relfilenode from pg_catalog.pg_class where relname='idx_xxx';
select * from pg_class where oid = 16384;
-[ RECORD 1 ]-------+--------
oid                 | 16384
relname             | student	-- 表、索引、视图等的名字
relnamespace        | 2200	   	-- 表所在 schema 的 OID, 表 和 关系(relation) 是同一层次概念, 关系可能包括 view，index等
reltype             | 16386		-- 表一行数据类型在 pg_type 中的 OID
reloftype           | 0			-- 类型表，复合类型的 OID
relowner            | 10		-- 表的所有者
relam               | 5104		-- 索引表，字段为 索引方式(B-tree, hash 等等)的 OID
relfilenode         | 16384		-- 表在磁盘文件的文件名
reltablespace       | 0			-- 表空间。如果为零，则意味着使用该数据库的缺省表空间
relpages            | 0			-- 表的磁盘页面数 规划器划用 由VACUUM,ANALYZE 和几个 DDL 命令，比如CREATE INDEX更新
reltuples           | 0			-- 表的元组数 规划器划用 由VACUUM,ANALYZE 和几个 DDL 命令，比如CREATE INDEX更新
relallvisible       | 0			-- 表的可见映射中标记所有可见的页的数目 规划器使用 由VACUUM,ANALYZE 和几个 DDL 命令，比如CREATE INDEX更新
reltoastrelid       | 0			-- 与表关联的 TOAST 表的 OID，如果没有为 0
relhasindex         | f 		-- 表是否有索引
relisshared         | f 		-- 表是否在整个集群中由所有数据库共享则
relpersistence      | p    		-- p = permanent table（永久表）, u = unlogged table（未加载的表）, t = temporary table （临时表）
relkind             | r 		-- r = ordinary table（普通表）, i = index（索引）, S = sequence（序列）, v = view（视图）, m = materialized view（物化视图）, c = composite type（复合类型）, t = TOAST table（TOAST 表）, f = foreign table（外部表）
relnatts            | 4			-- 用户字段数目，隐藏列不在此
relchecks           | 1			-- CHECK约束的数目
relhasrules         | f 		-- 表有（或曾经有）规则就为真
relhastriggers      | f 		-- 表有(或者曾经有)触发器，则为真
relhassubclass      | f 		-- 表或者索引有(或者曾经有)任何继承的子表，为真
relrowsecurity      | f 		-- 表上启用了行级安全性则为真
relforcerowsecurity | f 		-- 行级安全性（启用时）也适用于表拥有者则为真
relispopulated      | t 		-- 表已被填充则为真（对于所有关系该列都为真，但对于某些物化视图却不是）
relreplident        | d    		-- 行形成“replica identity”的列： d = 默认 (主键，如果存在), n = 无, f = 所有列 i = 索引的indisreplident被设置或者为默认
relispartition      | f    		-- 表或索引是一个分区，则为真
relrewrite          | 0 		-- 要求表重写的DDL操作期间被写入的新关系，这个域包含原始关系的OID，否则为0
relfrozenxid        | 491 		-- 在此之前的所有事务ID在表中已经被替换为一个永久的（“冻结的”) 事务ID。这用于跟踪表是否需要被清理，以便阻止事务ID回卷或者允许pg_xact被收缩
relminmxid          | 1			-- 在此之前的多事务ID在表中已经被替换为一个事务ID。这被用于跟踪表是否需要被清理，以阻止 多事务ID回卷或者允许pg_multixact被收缩
relacl              |
reloptions          |
relpartbound        |


-- 查询 student 的属性表 
select * from pg_attribute where attrelid = 16384;
-- 列所属的表
attrelid     	16384			16384		16384	16384	16384	16384	16384	16384	16384	16384		16384
-- 列名
attname       	gp_segment_id	tableoid	cmax	xmax	cmin	xmin	ctid	id	 	name	dept_name	tot_cred
-- 列的数据类型，pg_type.oid
atttypid      	23				26			29		28		29		28		27		1043	1043	1043		1700
-- attstattarget控制由ANALYZE对此列收集的统计信息的细节层次。0值表示不会收集任何统计信息。一个负值则说明直接使用系统默认的目标。正值的确切含义取决于数据类型
attstattarget 	0				0			0		0		0		0		0		-1		-1		-1			-1
-- 本列类型的pg_type.typlen一个拷贝
attlen        	4				8			4		4		4		4		20		-1		-1		-1			-1
-- 列的编号。一般列从1开始向上编号。系统列（如ctid）则拥有（任意）负值编号。
attnum        	-7				-6			-5		-4		-3		-2		-1		1		2		3			4
-- 如果该列是一个数组类型，这里就是其维度数；否则为0
attndims      	0				0			0		0		0		0		0		0		0		0			0
-- 在存储中总是为-1，但是当被载入到一个内存中的行描述符后，这里可能会被更新为属性在行内的偏移
attcacheoff   	-1				-1			-1		-1		-1		-1		-1		-1		-1		-1			-1
-- 记录了在表创建时提供的类型相关数据（例如一个varchar列的最大长度）。它会被传递给类型相关的输入函数和长度强制函数。对于那些不需要atttypmod的类型，这个值通常总是为-1
atttypmod     	-1				-1			-1		-1		-1		-1		-1		9		24		24			196612
-- 	该列类型的pg_type.typbyval的一个拷贝
attbyval      	t				t			t		t		 t		 t		f		f		f		f			f
-- 通常是该列类型的pg_type.typstorage的一个拷贝。对于可TOAST的数据类型，这可以在列创建后被修改以控制存储策略
attstorage    	p				p			p		p		 p		 p		p		x		x		x			m
-- 该列类型的pg_type.typalign的一个拷贝
attalign      	i				d			i		i		 i		 i		s		i		i		i			i
-- 	这表示一个非空约束
attnotnull    	t				t			t		t		 t		 t		t		f		t		f			f
-- 该列有一个默认表达式或生成的表达式，在此情况下在pg_attrdef目录中会有一个对应项来真正定义该表达式
atthasdef     	f				f			f		f		 f		 f		f		f		f		f			f
-- 该列在行中完全缺失时会用到这个列的值，如果在行创建之后增加一个有非易失DEFAULT值的列，就会发生这种情况。实际使用的值被存放在attmissingval列中
atthasmissing 	f				f			f		f		 f		 f		f		f		f		f			f
-- 如果是一个零字节（''），则不是一个标识列。否则，a = 总是生成，d = 默认生成。
attidentity   					 			 								  		 		 		   		 
-- 如果为零字节('')，则不是生成的列。否则，s = stored。
attgenerated  					 			 								  		 		    	   		           
-- 该列被删除且不再有效。一个删除的列仍然物理存在于表中，但是会被分析器忽略并因此无法通过SQL访问
attisdropped  	f				f			f		f		 f		 f		f		f		f		f			f
-- 该列是由表本地定义的。注意一个列可以同时是本地定义和继承的
attislocal    	t				t			t		t		 t		 t		t		t		t		t			t
-- 该列的直接祖先的编号。一个具有非零编号祖先的列不能被删除或者重命名
attinhcount   	0				0			0		0		0		0		0		0		0		0			0
-- 列被定义的排序规则，如果该列不是一个可排序数据类型则为0
attcollation  	0				0			0		0		0		0		0		100		100		100			0
attacl        											
attoptions    											
attfdwoptions 											
attmissingval 											

-- 查询 student pg_type 内容
select * from pg_type where oid = 1043;
-[ RECORD 1 ]--+-----------------
oid            | 1043				-- 行标识符
typname        | varchar 			-- 数据类型的名字
typnamespace   | 11 				-- 包含此类型的名字空间的OID pg_namespace.oid
typowner       | 10					-- 类型的拥有者
typlen         | -1					-- 对于一个固定尺寸的类型，typlen是该类型内部表示的字节数。对于一个变长类型，typlen为负值。-1表示一个“varlena”类型（具有长度字），-2表示一个以空值结尾的C字符串
typbyval       | f 					-- 判断内部传递这个类型的数值时是通过传值还是传引用。。变长类型总是传引用。注意即使长度允许传值， typbyval也可以为假
typtype        | b 					-- typtype可以是： b表示一个基类， c表示一个组合类型（例如一个表的行类型）， d表示一个域， e表示一个枚举类型， p表示一个伪类型，或 r表示一个范围类型
typcategory    | S 					-- typcategory是一种任意的数据类型分类，它被分析器用来决定哪种隐式转换“更好”
typispreferred | f 					-- 如果此类型在它的typcategory中是一个更好的转换目标，此列为真
typisdefined   | t 					-- 如果此类型已被定义则为真，如果此类型只是一个表示还未定义类型的占位符则为假。当typisdefined为假，除了类型名字、名字空间和OID之外什么都不能被依赖
typdelim       | , 					-- 在分析数组输入时，分隔两个此类型值的字符。注意该分隔符是与数组元素数据类型相关联的， 而不是和数组的数据类型关联
typrelid       | 0					-- 如果这是一个复合类型（见typtype）， 那么这个列指向pg_class中定义对应表的项（对于自由存在的复合类型，pg_class项并不表示一个表，但不管怎样该类型的pg_attribute项需要链接到它）。对非复合类型此列为零
typelem        | 0 					-- 如果typelem不为0，则它标识pg_type里面的另外一行。 当前类型可以被加上下标得到一个值为类型typelem的数组来描述。 一个“真的”数组类型是变长的（typlen = -1），但是一些定长的（typlen > 0）类型也拥有非零的typelem，比如name和point。 如果一个定长类型拥有一个typelem， 则它的内部形式必须是某个typelem数据类型的值，不能有其它数据
typarray       | 1015				-- 不是0，则它标识pg_type中的另一行，这一行是一个将此类型作为元素的“真的”数组类型
typinput       | varcharin
typoutput      | varcharout
typreceive     | varcharrecv
typsend        | varcharsend
typmodin       | varchartypmodin
typmodout      | varchartypmodout
typanalyze     | -					
typalign       | i 					-- 当存储此类型值时要求的对齐性质; c = char对齐, s = short对齐, i = int对齐, d = double对齐
typstorage     | x 					-- 变长类型（typlen = -1）可被TOAST, p：值必须平面存储. e：值可以被存储在一个“二级”关系. m：值可以被压缩线内存储. x：值可以被压缩线内存储或存储在“二级”存储 
typnotnull     | f 					-- 表示类型上的一个非空约束
typbasetype    | 0
typtypmod      | -1
typndims       | 0
typcollation   | 100 				-- 指定此类型的排序规则
typdefaultbin  |
typdefault     |
typacl         |
