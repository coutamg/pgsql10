## 1. 查看数据库
    psql -l 
    在 psql 命令行中可用 \l 

## 2. 查看表
    psql 命令行中
        \d          显示所有的表
        \d 表名      显示表结构
        \d 索引名    显示索引信息
        \d 通配符
        \d+ xxx     显示更加详细的信息，例如表列的注释，OID等

    其他相关的：
        \dt     只显示匹配的表
        \di     只显示索引
        \ds     只显示序列
        \dv     只显示视图
        \df     只显示函数


## 3. 切换数据库
    \c database

## 4. psql 连接数据库
    psql -l <hostname or ip> -p <port> [数据库名称] [用户名称]   
    上述参数可以通过指定环境变量
    export PGDATABASE=testdb
    export PGHOST=192.168.56.11
    export PGPORT=5432
    export PGUSER=postgres

## 5. 显示 sql 执行时间
    \timing on

## 6. 列出所有的 schema 
    \dn
    
    schema 可以理解为一个命名空间或目录，等同于 mysql 中的 database，
    一个 pgsql 中的一个 database 下可以有多个 schema，一个实例(pgsql进程)
    只包含一个 database, 应用连接到一个database时，只能访问这个database中的
    数据。
    

## 7. 列出所有表空间 
    \db
    表空间就是一个目录，把所有表的数据文件放在这个表空间下

## 8. 列出所有角色或用户
    \dg   列出角色
    \du   列出用户
    上面两个命令等价，因为 pg 中角色和用户不区分

## 9. 列出表权限情况
    \dp 
    \z

## 10. 指定字符集
    客户端与服务端编码不一致的时候，会显示乱码
    \encoding xxx 指定客户端的编码

## 11. \pset 命令
    用于设置输出格式
    \pset border 0 输出内容无边框
    \pset border 1 边框在内部
    \pset border 2 内外都有边框

## 12. \x 命令
    把表中的每一行数据拆分成单行展示, 与 mysql 命令行后面加 \G 类似
    也可以用 psql -x 来完成

## 13. \echo 用于输出一行信息

## 14. \i 运行 sql 脚本
    \i a.sql 

## 15. \? 显示更多命令

## 16 \set AUTOCOMMIT off
    设置自动关闭事物提交，AUTOCOMMIT 一定要大写,使用小写不会报错，但是修改不起作用

## 17. 获得 psql 中实际执行的 SQL 语句
    在 psql 命令启动的时候加 -E 参数，就可以把 psql 中以 "\" 开头命令的实际执行 SQL 打印出来

    在 psql 命令行中可以用 \set ECHO_HIDDEN on|off 来控制显示一个命令的实际 SQL

# expanded display
    \x 竖式显示 

# 修改pg 编译 copy_file_range 错误
sed -i "s/copy_file_range/copy_file_chunk/g" src/bin/pg_rewind/copy_fetch.c
