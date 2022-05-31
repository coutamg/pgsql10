-- STUDENT （学号，学生姓名，学生性别）
CREATE TABLE STUDENT (sno INT primary key, sname VARCHAR(10) , ssex INT);

-- COURSE （课程编号，制呈名，教师编号）
CREATE TABLE COURSE (cno INT primary key, cname VARCHAR(10), tno INT);

-- SCORE （学号， 课程编号， 分数）
CREATE TABLE SCORE (sno INT, cno INT, degree INT) ;

-- TEACHER （教师编号， 教师姓名 ， 教师性别）
CREATE TABLE TEACHER (tno INT primary key, tname VARCHAR (10), tsex INT) ;