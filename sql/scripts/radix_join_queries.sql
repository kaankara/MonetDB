-- Table creation
-- CREATE TABLE r (key integer, value integer);
-- COPY 256000000 RECORDS INTO r FROM '/path/to/data1.tbl' USING DELIMITERS ',';
-- CREATE TABLE s (key integer, value integer);
-- COPY 256000000 RECORDS INTO s FROM '/path/to/data2.tbl' USING DELIMITERS ',';

-- Equal sized tables
select count(*) FROM r, s where r.key = s.key;
select count(*) FROM r, s where r.key = s.key and r.key < 128000000 and s.key < 128000000;
select count(*) FROM r, s where r.key = s.key and r.key < 64000000 and s.key < 64000000;
select count(*) FROM r, s where r.key = s.key and r.key < 32000000 and s.key < 32000000;
select count(*) FROM r, s where r.key = s.key and r.key < 16000000 and s.key < 16000000;
select count(*) FROM r, s where r.key = s.key and r.key < 8000000 and s.key < 8000000;
select count(*) FROM r, s where r.key = s.key and r.key < 4000000 and s.key < 4000000;
select count(*) FROM r, s where r.key = s.key and r.key < 2000000 and s.key < 2000000;
select count(*) FROM r, s where r.key = s.key and r.key < 1000000 and s.key < 1000000;
select count(*) FROM r, s where r.key = s.key and r.key < 500000 and s.key < 500000;
select count(*) FROM r, s where r.key = s.key and r.key < 250000 and s.key < 250000;
select count(*) FROM r, s where r.key = s.key and r.key < 125000 and s.key < 125000;
select count(*) FROM r, s where r.key = s.key and r.key < 62500 and s.key < 62500;

-- Different sized tables, s 256e6
select count(*) FROM r, s where r.key = s.key and r.key < 16000000;
select count(*) FROM r, s where r.key = s.key and r.key < 8000000;
select count(*) FROM r, s where r.key = s.key and r.key < 4000000;
select count(*) FROM r, s where r.key = s.key and r.key < 2000000;
select count(*) FROM r, s where r.key = s.key and r.key < 1000000;
select count(*) FROM r, s where r.key = s.key and r.key < 500000;
select count(*) FROM r, s where r.key = s.key and r.key < 250000;
select count(*) FROM r, s where r.key = s.key and r.key < 125000;
select count(*) FROM r, s where r.key = s.key and r.key < 62500;

-- Different sized tables s 16e6
select count(*) FROM r, s where r.key = s.key and r.key < 16000000 and s.key < 16000000;
select count(*) FROM r, s where r.key = s.key and r.key < 8000000 and s.key < 16000000;
select count(*) FROM r, s where r.key = s.key and r.key < 4000000 and s.key < 16000000;
select count(*) FROM r, s where r.key = s.key and r.key < 2000000 and s.key < 16000000;
select count(*) FROM r, s where r.key = s.key and r.key < 1000000 and s.key < 16000000;
select count(*) FROM r, s where r.key = s.key and r.key < 500000 and s.key < 16000000;
select count(*) FROM r, s where r.key = s.key and r.key < 250000 and s.key < 16000000;
select count(*) FROM r, s where r.key = s.key and r.key < 125000 and s.key < 16000000;
select count(*) FROM r, s where r.key = s.key and r.key < 62500 and s.key < 16000000;
