/*-------------------------------------------------------------------------
 *
 * pg_attribute.h
 *	  definition of the system "attribute" relation (pg_attribute)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_attribute.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_ATTRIBUTE_H
#define PG_ATTRIBUTE_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_attribute definition.  cpp turns this into
 *		typedef struct FormData_pg_attribute
 *
 *		If you change the following, make sure you change the structs for
 *		system attributes in catalog/heap.c also.
 *		You may need to change catalog/genbki.pl as well.
 * ----------------
 */
#define AttributeRelationId  1249
#define AttributeRelation_Rowtype_Id  75
/*
	pg_attribute存储有关表列的信息。数据库中的每一个表的每一个列都恰好在pg_attribute中有一行。
	（这其中也会有索引的属性项，并且事实上所有具有pg_class项的对象在这里都有属性项） entries.

	属性等同于列
*/
CATALOG(pg_attribute,1249) BKI_BOOTSTRAP BKI_WITHOUT_OIDS BKI_ROWTYPE_OID(75) BKI_SCHEMA_MACRO
{
	/* 列所属的表 */
	Oid			attrelid;		/* OID of relation containing this attribute */
	/* 列名 */
	NameData	attname;		/* name of attribute */

	/*
	 * atttypid is the OID of the instance in Catalog Class pg_type that
	 * defines the data type of this attribute (e.g. int4).  Information in
	 * that instance is redundant with the attlen, attbyval, and attalign
	 * attributes of this instance, so they had better match or Postgres will
	 * fail.
	 */
	/* 列的数据类型 */
	Oid			atttypid;

	/*
	 * attstattarget is the target number of statistics datapoints to collect
	 * during VACUUM ANALYZE of this column.  A zero here indicates that we do
	 * not wish to collect any stats about this column. A "-1" here indicates
	 * that no value has been explicitly set for this column, so ANALYZE
	 * should use the default setting.
	 */
	/* attstattarget控制由ANALYZE对此列收集的统计信息的细节层次。0值表示不会收集任何统计信息。
	 一个负值则说明直接使用系统默认的目标。正值的确切含义取决于数据类型。对于标量数据类型，
	 attstattarget既是要收集的“最常见值”的目标号，也是要创建的柱状图容器的目标号。 */
	int32		attstattarget;

	/*
	 * attlen is a copy of the typlen field from pg_type for this attribute.
	 * See atttypid comments above.
	 */
	/* 本列类型的pg_type.typlen一个拷贝  */
	int16		attlen;

	/*
	 * attnum is the "attribute number" for the attribute:	A value that
	 * uniquely identifies this attribute within its class. For user
	 * attributes, Attribute numbers are greater than 0 and not greater than
	 * the number of attributes in the class. I.e. if the Class pg_class says
	 * that Class XYZ has 10 attributes, then the user attribute numbers in
	 * Class pg_attribute must be 1-10.
	 *
	 * System attributes have attribute numbers less than 0 that are unique
	 * within the class, but not constrained to any particular range.
	 *
	 * Note that (attnum - 1) is often used as the index to an array.
	 */
	/* 列的编号。一般列从1开始向上编号。系统列（如oid）则拥有（任意）负值编号 */
	int16		attnum;

	/*
	 * attndims is the declared number of dimensions, if an array type,
	 * otherwise zero.
	 */
	/* 如果该列是一个数组类型，这里就是其维度数；否则为0。（在目前一个数组的维度数并不
	 被强制，因此任何非零值都能有效地表明“这是一个数组”。） */
	int32		attndims;

	/*
	 * fastgetattr() uses attcacheoff to cache byte offsets of attributes in
	 * heap tuples.  The value actually stored in pg_attribute (-1) indicates
	 * no cached value.  But when we copy these tuples into a tuple
	 * descriptor, we may then update attcacheoff in the copies. This speeds
	 * up the attribute walking process.
	 */
	/* 在存储中总是为-1，但是当被载入到一个内存中的行描述符后，这里可能会被更新为属性在行内的偏移  */
	int32		attcacheoff;

	/*
	 * atttypmod records type-specific data supplied at table creation time
	 * (for example, the max length of a varchar field).  It is passed to
	 * type-specific input and output functions as the third argument. The
	 * value will generally be -1 for types that do not need typmod.
	 */
	/* atttypmod记录了在表创建时提供的类型相关数据（例如一个varchar列的最大长度）。 */
	/* 它会被传递给类型相关的输入函数和长度强制函数。对于那些不需要atttypmod的类型，这个值通常总是为-1 */
	int32		atttypmod;

	/*
	 * attbyval is a copy of the typbyval field from pg_type for this
	 * attribute.  See atttypid comments above.
	 */
	/* 该列类型的pg_type.typbyval的一个拷贝 */
	bool		attbyval;

	/*----------
	 * attstorage tells for VARLENA attributes, what the heap access
	 * methods can do to it if a given tuple doesn't fit into a page.
	 * Possible values are
	 *		'p': Value must be stored plain always
	 *		'e': Value can be stored in "secondary" relation (if relation
	 *			 has one, see pg_class.reltoastrelid)
	 *		'm': Value can be stored compressed inline
	 *		'x': Value can be stored compressed inline or in "secondary"
	 * Note that 'm' fields can also be moved out to secondary storage,
	 * but only as a last resort ('e' and 'x' fields are moved first).
	 *----------
	 */
	/* 通常是该列类型的pg_type.typstorage的一个拷贝。对于可TOAST的数据类型， */
	/* 这可以在列创建后被修改以控制存储策略 */
	char		attstorage;

	/*
	 * attalign is a copy of the typalign field from pg_type for this
	 * attribute.  See atttypid comments above.
	 */
	/* 该列类型的pg_type.typalign的一个拷贝 */
	char		attalign;

	/* This flag represents the "NOT NULL" constraint */
	/* 这表示一个非空约束 */
	bool		attnotnull;

	/* Has DEFAULT value or not */
	/* 该列有一个默认值，在此情况下在pg_attrdef目录中会有一个对应项来真正记录默认值 */
	bool		atthasdef;

	/* One of the ATTRIBUTE_IDENTITY_* constants below, or '\0' */
	/* 如果是零字节(''),则不是一个标识列. 否则,a = 总是生成的,d = 默认生成的 */
	char		attidentity;

	/* Is dropped (ie, logically invisible) or not */
	/* 该列被删除且不再有效。一个删除的列仍然物理存在于表中， */
	/* 但是会被分析器忽略并因此无法通过SQL访问 */
	bool		attisdropped;

	/*
	 * This flag specifies whether this column has ever had a local
	 * definition.  It is set for normal non-inherited columns, but also for
	 * columns that are inherited from parents if also explicitly listed in
	 * CREATE TABLE INHERITS.  It is also set when inheritance is removed from
	 * a table with ALTER TABLE NO INHERIT.  If the flag is set, the column is
	 * not dropped by a parent's DROP COLUMN even if this causes the column's
	 * attinhcount to become zero.
	 */
	/* 该列是由关系本地定义的。注意一个列可以同时是本地定义和继承的 */
	bool		attislocal;

	/* Number of times inherited from direct parent relation(s) */
	/* 该列的直接祖先的编号。一个具有非零编号祖先的列不能被删除或者重命名 */
	int32		attinhcount;

	/* attribute's collation */
	/* 该列被定义的排序规则，如果该列不是一个可排序数据类型则为0 */
	Oid			attcollation;

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	/* NOTE: The following fields are not present in tuple descriptors. */

	/* Column-level access permissions */
	/* 列级访问权限 */
	aclitem		attacl[1];

	/* Column-level options */
	text		attoptions[1];

	/* Column-level FDW options */
	text		attfdwoptions[1];
#endif
} FormData_pg_attribute;

/*
 * ATTRIBUTE_FIXED_PART_SIZE is the size of the fixed-layout,
 * guaranteed-not-null part of a pg_attribute row.  This is in fact as much
 * of the row as gets copied into tuple descriptors, so don't expect you
 * can access fields beyond attcollation except in a real tuple!
 */
#define ATTRIBUTE_FIXED_PART_SIZE \
	(offsetof(FormData_pg_attribute,attcollation) + sizeof(Oid))

/* ----------------
 *		Form_pg_attribute corresponds to a pointer to a tuple with
 *		the format of pg_attribute relation.
 * ----------------
 */
typedef FormData_pg_attribute *Form_pg_attribute;

/* ----------------
 *		compiler constants for pg_attribute
 * ----------------
 */

#define Natts_pg_attribute				22
#define Anum_pg_attribute_attrelid		1
#define Anum_pg_attribute_attname		2
#define Anum_pg_attribute_atttypid		3
#define Anum_pg_attribute_attstattarget 4
#define Anum_pg_attribute_attlen		5
#define Anum_pg_attribute_attnum		6
#define Anum_pg_attribute_attndims		7
#define Anum_pg_attribute_attcacheoff	8
#define Anum_pg_attribute_atttypmod		9
#define Anum_pg_attribute_attbyval		10
#define Anum_pg_attribute_attstorage	11
#define Anum_pg_attribute_attalign		12
#define Anum_pg_attribute_attnotnull	13
#define Anum_pg_attribute_atthasdef		14
#define Anum_pg_attribute_attidentity	15
#define Anum_pg_attribute_attisdropped	16
#define Anum_pg_attribute_attislocal	17
#define Anum_pg_attribute_attinhcount	18
#define Anum_pg_attribute_attcollation	19
#define Anum_pg_attribute_attacl		20
#define Anum_pg_attribute_attoptions	21
#define Anum_pg_attribute_attfdwoptions 22


/* ----------------
 *		initial contents of pg_attribute
 *
 * The initial contents of pg_attribute are generated at compile time by
 * genbki.pl.  Only "bootstrapped" relations need be included.
 * ----------------
 */


#define		  ATTRIBUTE_IDENTITY_ALWAYS		'a'
#define		  ATTRIBUTE_IDENTITY_BY_DEFAULT 'd'

#endif							/* PG_ATTRIBUTE_H */
