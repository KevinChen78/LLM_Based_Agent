# PostgreSQL 初始化走查(检索后端)

一次性安装与初始化。完成后 retrieval_service 默认以 `backend=postgres` 运行,
结构化过滤(city/category/district/价格/人数)下推到 SQL,BM25 排序仍在 Python
(行为与 JSON 后端逐字节一致,由 `scripts/test_pg_retrieval.py` 双后端对比保证)。

## 1. 安装 PostgreSQL 17(原生 Windows)

从 <https://www.postgresql.org/download/windows/> 下载 EDB 安装包,安装为
Windows 服务(开机自启)。安装过程中**记下 postgres 超级用户密码**。
pgAdmin 可选不装。

## 2. 建用户与数据库(注意编码)

中文 locale 的 Windows 上,安装器创建的集群默认编码可能不是 UTF-8;
**必须显式指定 `ENCODING 'UTF8' TEMPLATE template0`**,否则中文数据会损坏。

用安装时设置的超级用户打开 psql(开始菜单 "SQL Shell (psql)"):

```sql
CREATE USER agent WITH PASSWORD '<选一个密码>';
CREATE DATABASE groupbuy OWNER agent ENCODING 'UTF8' TEMPLATE template0;
```

验证:

```
psql -U agent -d groupbuy -c "SHOW server_encoding;"
-- 期望: UTF8
```

## 3. 建表 + 装依赖 + 播种

在项目根目录:

```powershell
psql -U agent -d groupbuy -f sql/001_schema.sql
pip install -r requirements.txt
python scripts/pg_seed.py
```

期望输出:`deals: 5320 synced ... merchants: 5320 synced ... kb: 22 synced ...`

抽查:

```
psql -U agent -d groupbuy -c "SELECT count(*) FROM groupbuy_items; SELECT count(*) FROM merchants; SELECT count(*) FROM kb_passages;"
-- 期望: 5320 / 5320 / 22
```

## 4. 配置 retrieval_service

创建 `retrieval_service/.env.local`(已被 .gitignore 忽略):

```
RETRIEVAL_BACKEND=postgres
PG_DSN=host=127.0.0.1 port=5432 dbname=groupbuy user=agent password=<第2步的密码>
```

也可以用 libpq 标准变量写法(`PGHOST/PGPORT/PGDATABASE/PGUSER/PGPASSWORD`),
`PG_DSN` 留空即可。两种方式选一个;注意 `_load_env_file` 是 setdefault 语义,
`.env` 先于 `.env.local` 加载——同一个键只在一个文件里定义。

## 5. 启动验证

```powershell
python retrieval_service/main.py
# 期望 banner: backend=postgres  deals=5320  kb=22

curl http://localhost:8001/v1/health
# 期望包含 "backend":"postgres", "deal_count":5320
```

若 PG 未启动/未播种/缺 psycopg,服务会打印 WARNING 并降级为 `backend=json`
(数据来自 data/*.json)——健康检查里的 `backend` 字段可用来判断当前实际后端。

一致性校验(双后端起两个实例,跑 20 个请求的 diff 矩阵):

```powershell
python scripts/test_pg_retrieval.py
# 期望: PASS: all 20 cases identical across backends (deals=5320, kb=22)
# 无 PG 环境时打印 SKIP 并退出 0,不阻塞开发
```

## 6. pgvector + 向量召回(可选增强)

向量通道让"情侣约会"「吃点辣的」这类**同义/口语查询**也能命中(BM25 只管字面匹配)。
架构:fastembed(ONNX,无 torch)加载 BAAI/bge-small-zh-v1.5(512 维),
查询向量与商品向量在 PG 内做余弦 ANN(pgvector `<=>`),与 BM25 按
**RRF(倒数排名融合,k=60)** 合并;结构化过滤同样下推到向量 SQL。

### 6.1 编译安装 pgvector(Windows 无官方二进制,需源码构建)

前置:VS2022(含 C++ 桌面开发负载)+ PostgreSQL 17 已装。
在**管理员** "x64 Native Tools Command Prompt for VS 2022" 中:

```bat
cd %TEMP%
git clone --branch v0.8.1 https://github.com/pgvector/pgvector.git
cd pgvector
set "PGROOT=C:\Program Files\PostgreSQL\17"
nmake /F Makefile.win
nmake /F Makefile.win install    rem 需要管理员(写入 Program Files)
```

(install 把 vector.dll 拷进 PG 的 lib、control/SQL 拷进 share\extension;
非管理员会"拒绝访问"。)

### 6.2 迁移 + 生成向量

```powershell
# CREATE EXTENSION 需要超级用户 —— 用 postgres 跑这个迁移文件
psql -U postgres -d groupbuy -f sql/002_vector.sql

pip install -r requirements.txt      # 新增 fastembed
python scripts/pg_embed.py           # 首次下载模型 ~100MB
# 国内网络:先 set HF_ENDPOINT=https://hf-mirror.com
#          再 set HF_HUB_DISABLE_XET=1(镜像不支持 xet 协议,会 401)
```

`pg_embed.py` 默认只补 `embedding IS NULL` 的行(reseed 后增量),`--force` 全量重建,
`--check` 干跑。embed 的文本与服务 BM25 语料完全一致(共享 `retrieval_service/dealtext.py`)。

### 6.3 验证

```powershell
python retrieval_service/main.py
# banner 应有: vector channel on (BAAI/bge-small-zh-v1.5, 5320 embeddings)
curl http://localhost:8001/v1/health
# 应有 "vector":"on", "vector_model":"BAAI/bge-small-zh-v1.5"

python scripts/test_pg_vector.py     # 语义召回断言 + 对比证明 + 回归
```

开关:`RETRIEVAL_VECTOR=on|off`(默认 on;fastembed/向量列/embedding 任一缺失
自动降级纯 BM25 并 WARNING)。kb_passages 保持纯 BM25(22 条,无需向量)。

注意行为变化:向量通道开启后,**非空查询几乎总有结果**(余弦最近邻永远存在),
"BM25 无命中 → 评分排序兜底"只在结构化过滤结果为空时才会触发。

## 7. 重新播种工作流

JSON 生成器仍是数据源。数据变更后:

```powershell
python scripts/gen_wuhan_deals.py     # 或 gen_city_deals.py / gen_knowledge.py
python scripts/pg_seed.py             # 幂等 upsert + 清理过期行
python scripts/pg_embed.py            # 给新增行补向量(默认只处理 embedding IS NULL)
# 重启 retrieval_service(内存 BM25 语料是启动时构建的,不热加载)
```

`python scripts/pg_seed.py --check` 可干跑查看 JSON 与 DB 的漂移。

## 8. e2e 说明

`scripts/e2e_multi_turn.py --real` 会在 8001 端口未占用时自启 retrieval_service:
PG 可用时服务以 postgres 后端启动,e2e 走的就是 PG 路径;PG 不可用时降级 json,
e2e 依然全绿。确定性的 PG 验证以 `test_pg_retrieval.py` 为准。

## 表结构概览(sql/001_schema.sql)

- `merchants`(merchant_id PK, name, city, ...)——播种时从 deals 派生,
  name 暂为 merchant_id 占位,待正式商户目录数据
- `groupbuy_items`(item_id PK → merchants FK, 14 个业务列 + status/attributes/
  valid_* 预留列;索引: (city,category,price)、district、price)
- `kb_passages`(id PK 'kb-NNN', category/title/content/source/tags)

与项目规划_修订版 §5.1 的两处有意偏离(见 001_schema.sql 头部注释):
省略 `embedding VECTOR(768)`(pgvector 留给向量召回阶段);
min_people/max_people/tags/description 提升为一级列(过滤下推需要)。
