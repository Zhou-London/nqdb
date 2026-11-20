# nqdb 美股证券主数据：数据来源建议

调查日期 2026-11-19。下文只采用核查员确认过或已更正的结论。没能确认的内容统一放在第 5 节。

**结论先说：** 由 Agent 把关来源、再写 shell 脚本录入，这条路行得通，但有一个前提要接受。2025 年以前的代码变更历史，没有免费的官方来源（[CTA 规格](https://www.ctaplan.com/publicdocs/ctaplan/CTA_Symbol_File_Specification.pdf)、[Nasdaq Daily List](https://www.nasdaqtrader.com/Trader.aspx?id=DailyListPD)、[SR-NASDAQ-2026-062](https://www.sec.gov/files/rules/sro/nasdaq/2026/34-106034.pdf)）。所以要么付费买厂商数据（首选 Databento），要么接受“历史只能从今天开始自己攒”。

---

## 1. 推荐方案

### (a) 当前股票池与主上市所
- **主源（免费）：** Nasdaq Trader 的 [nasdaqtraded.txt](https://www.nasdaqtrader.com/dynamic/SymDir/nasdaqtraded.txt)，一个文件就覆盖所有交易所，带 Listing Exchange、ETF、Test Issue 字段。也可以用 [nasdaqlisted.txt](https://www.nasdaqtrader.com/dynamic/SymDir/nasdaqlisted.txt) 加 [otherlisted.txt](https://www.nasdaqtrader.com/dynamic/SymDir/otherlisted.txt) 两个文件。
- **交叉核对（免费）：**
  - 非 Nasdaq 上市的股票用 [CTA Symbol File](https://ftp.nyse.com/cta_symbol_files)，对比字段是 Primary Listing Market Participant ID。
  - CIK 和粗粒度交易所标签用 SEC 的 [company_tickers_exchange.json](https://www.sec.gov/files/company_tickers_exchange.json)。

### (b) 代码变更历史（带生效日期）
- **主源（付费）：** Databento corporate actions 里的 LCC（Local Code Change）事件，2018-05-01 起（[文档](https://databento.com/docs/datasets/corporate-actions.md)）。Databento security master 提供 2005-01-01 起的 point-in-time 记录，FB→META 改名后 listing_id 和 security_id 都不变（[文档](https://databento.com/docs/venues-and-datasets/security-master)）。
- **交叉核对（付费）：** Massive（原 Polygon）的 Ticker Events，2003-09-10 起，官方示例里就有 FB→META 2022-06-09（[文档](https://massive.com/docs/rest/stocks/corporate-actions/ticker-events.md)）。另一个选择是 Sharadar ACTIONS，1998 年起（[llms.txt](https://sharadar.com/llms.txt)）。这两家都有个人使用限制，见第 2 节。
- **免费，只能从现在往后攒：**
  - CTA Symbol File 的每日存档从 2025-05-05 开始；按规格，Prior Security Symbol 字段 2025-11-03 起生效。只覆盖非 Nasdaq 上市的股票（[目录](https://ftp.nyse.com/cta_symbol_files)、[规格](https://www.ctaplan.com/publicdocs/ctaplan/CTA_Symbol_File_Specification.pdf)）。
  - Nasdaq 文件只能每天自己存档、做差分。
- **单个事件的人工核对：** 用发行人自己的公告，例如 [Meta 公告](https://investor.atmeta.com/investor-news/press-release-details/2022/Meta-Platforms-Inc.-to-Change-Ticker-Symbol-to-META-on-June-9/default.aspx)：2022-06-09 起改用 META，仍在 Nasdaq，CUSIP 不变。可以把它当作验收测试用例。

### (c) 已退市股票
- **主源（免费，官方）：** SEC 的 Form 25-NSE（交易所提交）和 Form 25（发行人自愿退市）。交易所提交的 25-NSE 会出现在被退市公司自己的 submissions filings 列表里（[CIK1120193 示例](https://data.sec.gov/submissions/CIK0001120193.json)），所以按 CIK 就能查到，也可以扫 [full-index form.idx](https://www.sec.gov/Archives/edgar/full-index/)。
  - 生效日期：根据 [Rule 12d2-2(d)(1)](https://www.law.cornell.edu/cfr/text/17/240.12d2-2)，退市在提交后第 10 天生效。这不等于最后交易日。
- **交叉核对（付费）：** Databento security master 的 delisting_date 和 listing_status 字段（[API 参考](https://databento.com/docs/api-reference-reference/security-master)）。

### (d) 稳定标识符
- **证券级（share class），免费：** OpenFIGI 的 Composite FIGI。官方说 FIGI 一经分配不会改变，标准免费、无再使用限制，采用 MIT 许可（[API 文档](https://www.openfigi.com/api/documentation)、[FAQ](https://www.openfigi.com/about/faq)）。
- **发行人级，免费：** SEC CIK，不会被回收重用（[SEC](https://www.sec.gov/search-filings/edgar-search-assistance/accessing-edgar-data)）。但它是发行人级的：BRK-A 和 BRK-B 共用 CIK 1067983（[company_tickers_exchange.json](https://www.sec.gov/files/company_tickers_exchange.json)）。
- **付费：** Databento 的 security_id 和 listing_id。
- **交叉核对：** FIGI 与 CIK 互相核对，再与 Databento 记录里的 figi 和 cik 字段核对。
- **不要拿 CUSIP 或 ISIN 当主键**，见第 3 节。

### 免费与付费
- **免费：** Nasdaq symbol directory、CTA Symbol File、SEC 的所有数据、OpenFIGI、ISO MIC 列表。
- **付费：**
  - Databento reference data。价格见第 4 节，有未知项。
  - Massive，个人档 Stocks Advanced 每月 $199（[pricing](https://massive.com/pricing)）。
  - Sharadar，含 ACTIONS 的全历史最便宜档是 Prices 计划，每月 $39 或每年 $299（[llms.txt](https://sharadar.com/llms.txt)）。
  - Nasdaq Daily List，按 [SR-NASDAQ-2026-062](https://www.sec.gov/files/rules/sro/nasdaq/2026/34-106034.pdf) 现行费用为每月 $3,500。

---

## 2. 每个来源的细节

| 来源 | 访问方式 | 格式 / 鉴权 | 限速 | 历史深度 | 存储与许可 |
|---|---|---|---|---|---|
| nasdaqtraded / nasdaqlisted / otherlisted | `https://www.nasdaqtrader.com/dynamic/SymDir/*.txt`，也有 FTP `ftp://ftp.nasdaqtrader.com/symboldirectory`（[定义页](https://www.nasdaqtrader.com/trader.aspx?id=symboldirdefs)） | 竖线分隔，无鉴权，最后一行是 `File Creation Time` | 未公布；“一天内多次更新”，没有固定时间表 | 只有当天 | 免费；没找到专门的许可条款 |
| TradingSystemAddsDeletes.txt | [链接](https://www.nasdaqtrader.com/dynamic/SymDir/TradingSystemAddsDeletes.txt) | 竖线分隔；实际表头是 `Symbol\|Company Name\|NASDAQ Action\|BX Action\|PSX Action\|Effective Date\|Primary Listing Market` | — | 只有当天 | 记录的是交易系统的增删，不是上市事件 |
| CTA Symbol File | [ftp.nyse.com/cta_symbol_files](https://ftp.nyse.com/cta_symbol_files)，文件名 `CTA.Symbol.File.YYYYMMDD.csv` | 逗号分隔，HTTPS 公开可下，无鉴权；每个交易日美东午夜前发布，数据截至 20:00（[规格 v1.2](https://www.ctaplan.com/publicdocs/ctaplan/CTA_Symbol_File_Specification.pdf)） | — | 2025-05-05 起，约 400 个文件 | 规格只说“参与者和数据订阅者”可以下载，**没有写明免费，也没有再分发条款** |
| SEC company_tickers_exchange.json | [链接](https://www.sec.gov/files/company_tickers_exchange.json) | JSON，行格式 `[cik, name, ticker, exchange]`；顶层结构请在解析时自行确认 | 每秒最多 10 次，必须声明 User-Agent（[SEC](https://www.sec.gov/os/accessing-edgar-data)） | 只有当前 | 公共数据；SEC 声明“不保证准确性和覆盖范围” |
| SEC submissions API | `https://data.sec.gov/submissions/CIK##########.json`；每晚约美东 3:00 发布 submissions.zip（[API 页](https://www.sec.gov/search-filings/edgar-application-programming-interfaces)） | JSON，无 key，不支持 CORS | 同上 | 曾用名带起止日期；代码和交易所只有当前值；申报记录覆盖 EDGAR 全部历史 | 公共数据 |
| EDGAR full-index / daily-index | [full-index](https://www.sec.gov/Archives/edgar/full-index/)、[daily-index](https://www.sec.gov/Archives/edgar/daily-index/) | 文本索引，每晚约美东 22:00 起更新 | 同上 | 1994 年第 3 季度起 | 公共数据 |
| OpenFIGI v3 | `POST https://api.openfigi.com/v3/mapping`（[文档](https://www.openfigi.com/api/documentation)） | JSON；API key 可选且免费 | mapping：无 key 每分钟 25 次、每次 10 个 job；有 key 每分钟 250 次、每次 100 个 job。search/filter：无 key 每分钟 5 次，有 key 每分钟 20 次。2026-11-19 所见 | 无历史，无 as-of 参数；`includeUnlistedEquities=true` 时会返回已退市证券 | MIT，可存储、可再分发 |
| ISO 10383 MIC | [CSV](https://www.iso20022.org/sites/default/files/ISO10383_MIC/ISO10383_MIC.csv) | CSV，无鉴权 | — | 每个 MIC 带创建和失效日期 | 免费；再分发条款未核对 |
| Databento security master | 客户端库 `Reference.security_master.get_last` / `get_range`；每次请求最多 2,000 个代码（[API 参考](https://databento.com/docs/api-reference-reference/security-master)） | 用现有的 Databento key | 每个 endpoint 每 IP 每秒 100 次，所有 API 每 IP 最多 100 个并发连接，超限返回 429 加 Retry-After（[rate limits](https://databento.com/docs/api-reference-reference/basics/rate-limits)） | PIT 记录从 2005-01-01 起，每周更新（[docs](https://databento.com/docs/venues-and-datasets/security-master)） | 计划结构：Standard 按 ISIN 计数，每年 1,000 个，超额请求返回授权错误；另有 Unlimited (US only)，覆盖 10,000+ 个美国代码（[billing](https://databento.com/docs/portal/billing)）。没找到退订后能否保留数据的条款 |
| Databento corporate actions | `Reference.corporate_actions.get_range` | 同上 | 同上 | 2018-05-01 起，每天更新 4 次（[docs](https://databento.com/docs/datasets/corporate-actions.md)） | 2024-08 的博客写“每月 $299 起”（[blog](https://databento.com/blog/corporate-actions)），可能已过时 |
| Massive Ticker Events | `/vX/reference/tickers/{id}/events`，id 可以是代码、CUSIP 或 Composite FIGI；标为实验性接口（[docs](https://massive.com/docs/rest/stocks/corporate-actions/ticker-events.md)） | REST，需要 key | 未确认 | 2003-09-10 起；Basic 档只有 2 年 | 套餐写着“individual use only”；[Market Data Terms 第 8 节](https://massive.com/legal/market-data-terms-of-service)要求退订后 30 天内删除数据，备份最长保留 90 天 |
| Sharadar TICKERS / ACTIONS | `https://api.sharadar.com/v1.0`，批量包用 `years=full`，返回 302 跳转到 zip；有现成的 SQLite schema（[llms.txt](https://sharadar.com/llms.txt)） | CSV，需要 key | 未确认 | TICKERS 从 1990-06 起，ACTIONS 从 1998-01 起 | 只限自然人个人使用，禁止商业或机构使用；退订后 30 天内必须删除数据（[terms](https://sharadar.com/terms)） |
| Nasdaq Daily List | SFTP 或网页，需要先签数据协议 | 竖线分隔 | — | 1999-05-24 起 | 付费，现行每月 $3,500（[SR-NASDAQ-2026-062](https://www.sec.gov/files/rules/sro/nasdaq/2026/34-106034.pdf)） |

**被排除的来源：**
- EODHD：改名历史从 2022-07-22 才开始，覆盖不到 FB→META（[EODHD](https://eodhd.com/financial-apis/us-stock-symbol-rename-history-api)）。
- Norgate：不提供历史代码，客户端只能在 Windows 上运行（[FAQ](https://norgatedata.com/data-package-faq.php)、[NDU FAQ](https://norgatedata.com/ndu-faq.php)）。
- QuantConnect：文档说数据“只能在 LEAN 中使用”（[docs](https://www.quantconnect.com/docs/v2/writing-algorithms/datasets/quantconnect/us-equity-security-master.md)）。
- FINRA OTC Daily List：只覆盖 OTC（[指南](https://www.finra.org/sites/default/files/OTCE_Daily_List_User_Guide.pdf)）。
- NYSE Corporate Actions 和 Cboe DataShop：付费，而且各自只覆盖本所上市的证券（[NYSE](https://www.nyse.com/market-data/corporate-actions/corporate-actions-for-nyse-group-listings)、[Cboe](https://datashop.cboe.com/listings-distributions-and-corporate-actions)）。
- CRSP：只面向机构授权（[Morningstar](https://indexes.morningstar.com/research-data-products/crsp-us-stock-databases)）。

---

## 3. 已知坑

### 股票类别的标点写法
同一只股票在不同来源里写法不同：
- SEC 用短横线，如 `BRK-B`（[json](https://www.sec.gov/files/company_tickers_exchange.json)）。
- Nasdaq 文件的 ACT、CQS、Nasdaq 三列都用点，如 `BRK.B`（[otherlisted](https://www.nasdaqtrader.com/dynamic/SymDir/otherlisted.txt)）。
- CQS 从 2018-06-01 起改用点，以前用斜杠，如 `BRK/B`。NYSE/CMS 用裸后缀或空格。优先股在 CQS、CMS、Cboe 里分别写成 `p`、`PR`、`-`（[Cboe 规格](https://cdn.cboe.com/resources/techspec/technical-specifications-cboe-titanium-u-s-equitiesoptionsfutures-symbology-reference.md)）。
- Nasdaq 的优先股用 `$`，如 `CFR$B`。
- Nasdaq QIX 协议输出空格（[FAQ](https://m.nasdaqtrader.com/content/technicalsupport/specifications/symbology_faq.pdf)）。
- Databento 的 raw_symbol 统一用 Nasdaq Integrated 后缀，即点号形式（[symbology](https://databento.com/docs/api-reference-data/basics/symbology)）。但 security master 里 USNYSE 证券的 `nasdaq_symbol` 可能不带分隔符，如 `BRKB`，所以 Databento 内部关联要用 isin、us_code 或 figi（[docs](https://databento.com/docs/venues-and-datasets/security-master)）。

**建议：** 库里统一存点号形式，入库时做规范化。

### 代码重用
- Databento 不会回溯修改历史代码，同一个代码在不同日期可能对应不同证券。instrument_id 由发布方分配，有的发布方每天重新映射（[symbology](https://databento.com/docs/standards-and-conventions/symbology)）。所以主键不能用裸代码，也不能用 instrument_id，只能用“标识符加日期区间”。
- Massive 只返回某个代码**当前持有者**的事件历史，例子是 PARA；已被收购的代码会返回 404，例子是 TWTR；delisted_utc 比最后一根 bar 晚（[KB](https://massive.com/knowledge-base/article/how-does-massive-handle-ticker-changes-and-acquisitions)）。

### 换交易所
- Sunoco LP 和 SunocoCorp 从 NYSE 转到 TXSE：NYSE 最后交易日 2026-10-02，TXSE 预计 2026-10-05 开始交易（[新闻稿](https://www.sunocolp.com/press-release/item/sunoco-lp-and-sunococorp-llc-to-transfer-listings-to-the-texas-stock-exchange-2026)、[8-K 附件](https://www.sec.gov/Archives/edgar/data/0001552275/000155227526000075/ex991sunandsuncdelistnysel.htm)）。Energy Transfer 也宣布了同样的迁移（[finviz](https://finviz.com/news/390863/energy-transfer-to-transfer-stock-exchange-listings-to-texas-stock-exchange)）。
- otherlisted.txt 已经用代码 `F` 表示 TXSE，例如 SUN、ET、CFR，但定义页没有更新，也没列出 F。
- **对 schema 的要求：**
  - 主上市所必须是带日期的历史表，不能只有一列。
  - 离开 NYSE 时提交的 Form 25 不能当成退市。
  - 交易所代码要按来源分别映射：CTA 用 F 表示 TXSE、M 表示 NYSE Texas（[规格](https://www.ctaplan.com/publicdocs/ctaplan/CTA_Symbol_File_Specification.pdf)）；SEC 文件的标签是 `TXSE`。
- **MIC 本身也会变（[CSV](https://www.iso20022.org/sites/default/files/ISO10383_MIC/ISO10383_MIC.csv)）：**
  - 名称变化：XCHI 现在叫 NYSE Texas，XBOS 现在叫 Nasdaq Texas。
  - 新增：TXSE 于 2025-11-24 创建；MEMX 于 2026-03-23 改挂到新的运营 MIC XMEM 之下。
  - 暗池段（NYSD、BOSD、TXSD）不能当作上市场所。
  - CSV 里有重复行，需要去重。
  - 存 MIC 代码，不要存名称。

### 退市判断
- 很多 25-NSE 针对的是债券、优先股，或者到期、赎回的证券。例如 [NDAQ 的这份 25-NSE](https://www.sec.gov/Archives/edgar/data/1120193/000135445721000997/0001354457-21-000997-index.html) 就不是普通股退市。必须解析证券类别描述，否则会录入假的普通股退市。
- SEC 的公司更名日期不是代码变更日期。Meta 在 EDGAR 里的旧名 Facebook Inc 到 2021-10-27 为止，代码变更却是 2022-06-09（[submissions](https://data.sec.gov/submissions/CIK0001326801.json)）。
- submissions 里的 tickers 数组不区分证券类型，例如里面有 METAM，需要过滤。

### SEC 的 User-Agent 要求
请求头里必须声明 User-Agent，格式是“公司名 + 管理员邮箱”，总频率每秒不超过 10 次，否则可能被封 IP（[SEC](https://www.sec.gov/os/accessing-edgar-data)）。联系信息由业主自己定，脚本不要擅自填入任何个人信息。

### CUSIP 许可
- 订阅 CGS 产品，或者通过授权厂商间接拿到 CUSIP，都需要 CGS 许可。只在屏幕上显示、不导出的情况除外。
- 少于 500 个标识符可以免费，但仍然要签许可。
- 免责条款明确提到“存入数据库”（[CGS](https://www.cusip.com/services/license-fees.html)）。
- CGS 条款把美国 ISIN 也算作许可数据。
- FINRA 也说明，清算结算以外的用途需要 CGS 许可（[FINRA](https://www.finra.org/filing-reporting/trace/sample-files-usage-disclaimer)）。

### 最小报价单位（Rule 612）
- 现行规则：价格不低于 $1.00 的股票，报价单位是 $0.01。低于 $1 是 $0.0001，这个数字来自现行 Rule 612 本身，本次没有重新核对原文。
- 2024 年修订（[SEC 2024-137](https://www.sec.gov/newsroom/press-releases/2024-137)）引入 $0.005 的半分报价单位：按 3 个月的 TWAQS 判定，TWAQS 不超过 $0.015 就用半分。1–3 月的数据决定 5 月起的单位，7–9 月决定 11 月起的单位，每次判定后固定 6 个月（[Davis Polk](https://www.davispolk.com/insights/client-update/reg-nms-resized-sec-adjusts-tick-sizes-lowers-access-fees-and-accelerates)）。该规则只管报价，不管成交价。
- 生效时间两次推迟：先推到 2026 年 11 月（[2025-130](https://www.sec.gov/newsroom/press-releases/2025-130-sec-issues-exemptive-order-regarding-compliance-certain-rules-under-regulation-nms)），再由 2026-06-11 的命令推到 **2027 年 11 月第一个工作日**，Rule 610(c) 也一并推迟（[34-105656](https://www.sec.gov/files/rules/exorders/2026/34-105656.pdf)）。
- **对 nqdb 的要求：** 报价单位表要带日期，并能逐只股票、按半年分配。

### 其他
- CTA 目录里同一天可能有多个带时间戳的文件版本，取最新的一个（[目录](https://ftp.nyse.com/cta_symbol_files)）。
- TradingSystemAddsDeletes.txt 实际表头里是 `BX Action`，定义页写的是 `NTX Action`。按表头名称解析，不要按列位置。
- **Databento 已知问题，2026-12-10 修复上线（[blog](https://databento.com/blog/reference-api-changes-2026-11)）：**
  - 修复前，历史 point-in-time 记录里的 listing_status 全部显示最新状态。
  - 代码或 ISIN 变更之后，用新标识符查询可能返回 422。
  - **建议：** 首次全量回填安排在 2026-12-10 之后，或者到时重新拉一遍。

---

## 4. 需要业主决定的事

1. **是否购买 Databento reference data。** 全美股票加上已退市的远超 1,000 个 ISIN，Standard 计划不够用，需要 Unlimited (US only)，价格没查到（[billing](https://databento.com/docs/portal/billing)）。请在账户里或问销售确认：
   - 现有计划是否已包含 security master 或 corporate actions；
   - Unlimited 的价格；
   - 退订后本地数据能否保留。
2. **用途是个人还是商业。** Sharadar 和 Massive 都只限个人使用，退订后 30 天内必须删除数据。如果 nqdb 要支持任何商业用途，或者数据要比订阅活得久，这两家都不合适。
3. **需要多深的历史。** 2018 年以前的代码变更，Databento corporate actions 覆盖不到，要靠 security master 的 PIT 记录（2005 年起）或第二家厂商补。需要决定是否值得。
4. **是否存 CUSIP 或 ISIN。** 存就要处理 CGS 许可。建议不存，或者只作为非主键属性、先确认许可再存。
5. **SEC User-Agent 用什么联系信息。**
6. **ETF 现在纳入还是以后再说。** ETF 的标志字段现成可用，但 Cboe 上市数据要付费。
7. **是否现在就开始每日存档** nasdaqtraded.txt、otherlisted.txt、TradingSystemAddsDeletes.txt 和 CTA 文件。不花钱，越早开始，攒下的免费历史越长。

---

## 5. 未能确认的点

- FB 和 META 在 OpenFIGI 里是否对应同一个 Composite FIGI。需要实际调一次 API。
- Databento 里有没有 FB→META、生效日 2022-06-09 的 LCC 记录。可以用现有 key 查一次 `corporate_actions.get_range` 验证。
- Databento 退市证券覆盖的具体深度、Unlimited (US only) 的价格、存储与退订条款。security master 每个额外代码的单价，文档和搜索摘要说法冲突。
- CTA 文件的 Prior Security Symbol 字段是否真的有值；该文件的许可和再分发条款。
- company_tickers_exchange.json 的更新频率、退市代码多久被移除、是否会单独标出 NYSE American 和 NYSE Arca。
- 电子化 Form 25 的起始年份。约 2006 年的说法只有第三方来源。full-index 是否每周六重建。
- Nasdaq symbol directory 文件的许可条款。nasdaqtraded.txt 的 Listing Exchange 完整代码表。
- Nasdaq 按 SR-NASDAQ-2026-062 将要发布的免费 FTP 文件：上线时间未知。按该文件的定义它很可能不含代码变更，也没有历史回溯。
- Sharadar ACTIONS 用什么动作代码表示代码变更，是否包含 FB→META。
- FMP、Alpha Vantage、Tiingo、EODData 的细节，以及 Nasdaq 交易暂停 RSS 的历史日期参数。
- 有没有免费的官方“首次上市日期”来源。候选包括存档快照里的首次出现日期、CTA 的 IPO 标志、S-1/424B 申报日期，但都不等于首个交易日。
- 2025-05 以前的 CTA 文件能否在别处免费获取。
- 用 EDGAR 的 8-K 和 XBRL `dei:TradingSymbol` 抽查代码变更日期是否可行。
- 2024 年以前 Databento 的价格结构，以及 corporate actions 是否仍是每月 $299。
