/*	-------------------------------------------------------------------------------------------------------
	SqliteLogger implementation. See SqliteLogger.h for documentation and usage examples.
	------------------------------------------------------------------------------------------------------- */

#include "CvGameCoreDLLPCH.h"
#include "SqliteLogger.h"
#include "CustomMods.h"
#include "AIUnitDecisionTrace.h"

#include <string.h>
#include <set>

// must be included after all other headers
#include "LintFree.h"

//	-----------------------------------------------------------------------------------------------
SqliteLogger::SqliteLogger() :
	m_kConnection(),
	m_bOpenAttempted(false),
	m_bOpen(false),
	m_registeredTableSchemas(),
	m_preparedInserts()
{
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::~SqliteLogger()
{
	for (InsertMap::iterator it = m_preparedInserts.begin(); it != m_preparedInserts.end(); ++it)
	{
		delete it->second;
	}
	m_preparedInserts.clear();

	if (m_bOpen)
	{
		m_kConnection.Close();
		m_bOpen = false;
	}
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger& SqliteLogger::getInstance()
{
	static SqliteLogger s_kInstance;
	return s_kInstance;
}

//	-----------------------------------------------------------------------------------------------
const char* SqliteLogger::SqlTypeName(Database::ColumnTypes eType)
{
	switch (eType)
	{
	case Database::COLTYPE_INT:
		return "INTEGER";
	case Database::COLTYPE_FLOAT:
		return "REAL";
	case Database::COLTYPE_TEXT:
		return "TEXT";
	case Database::COLTYPE_BOOL:
		return "BOOLEAN";
	case Database::COLTYPE_NULL:
	default:
		return "NULL";
	}
}

//	-----------------------------------------------------------------------------------------------
bool SqliteLogger::EnsureOpen()
{
	if (!MOD_SQLITE_LOGGING)
		return false;

	if (m_bOpen)
		return true;

	if (m_bOpenAttempted)
		return false;

	m_bOpenAttempted = true;

	CvString strDatabasePath = gDLL->GetCacheFolderPath();
	strDatabasePath += "stats.db";

	if (m_kConnection.Open(strDatabasePath.c_str(), Database::OPEN_CREATE | Database::OPEN_READWRITE | Database::OPEN_FULLMUTEX))
	{
		m_bOpen = true;
		EnsureDictionaryTable();
	}
	else
	{
		ASSERT(false, "SqliteLogger: failed to open stats.db");
		m_bOpen = false;
	}

	return m_bOpen;
}

//	-----------------------------------------------------------------------------------------------
void SqliteLogger::EnsureDictionaryTable()
{
	if (!m_bOpen)
		return;

	if (!m_kConnection.Execute(
		"CREATE TABLE IF NOT EXISTS uuid_dictionary ("
		"id INTEGER PRIMARY KEY AUTOINCREMENT, "
		"uuid_hex TEXT UNIQUE NOT NULL)"))
	{
		ASSERT(false, "SqliteLogger: failed to create uuid_dictionary table");
	}
}

//	-----------------------------------------------------------------------------------------------
int SqliteLogger::ResolveGameId(const std::string& strUuidHex)
{
	if (!EnsureOpen())
		return 0;

	if (strUuidHex.empty())
		return 0;

	// Insert the UUID if it is not already present. The UNIQUE constraint makes this idempotent.
	{
		Database::Results kInsert;
		if (m_kConnection.Execute(kInsert, "INSERT OR IGNORE INTO uuid_dictionary (uuid_hex) VALUES (?)"))
		{
			kInsert.Bind(1, strUuidHex.c_str());
			kInsert.Execute();
			kInsert.Reset();
		}
	}

	// Look up the (now guaranteed) row to retrieve its auto-incremented id.
	int iId = 0;
	{
		Database::Results kSelect;
		if (m_kConnection.Execute(kSelect, "SELECT id FROM uuid_dictionary WHERE uuid_hex = ?"))
		{
			kSelect.Bind(1, strUuidHex.c_str());
			if (kSelect.Step())
				iId = kSelect.GetInt(0);
			kSelect.Reset();
		}
	}

	return iId;
}

//	-----------------------------------------------------------------------------------------------
bool SqliteLogger::IsEnabled()
{
	return EnsureOpen();
}

//	-----------------------------------------------------------------------------------------------
bool SqliteLogger::TableSchemaMatches(const std::string& strTableName, const TableDef& kColumns)
{
	std::string strSql = "PRAGMA table_info(\"";
	strSql += strTableName;
	strSql += "\")";

	Database::Results kResults;
	if (!m_kConnection.Execute(kResults, strSql.c_str()))
		return false;

	size_t uiColumn = 0;
	while (kResults.Step())
	{
		// PRAGMA table_info columns: 0=cid, 1=name, 2=type, ...
		const char* szName = kResults.GetText(1);
		const char* szType = kResults.GetText(2);

		if (uiColumn >= kColumns.size())
		{
			// Live table has more columns than expected.
			kResults.Reset();
			return false;
		}

		const ColumnDef& kExpected = kColumns[uiColumn];
		if (szName == NULL || _stricmp(szName, kExpected.name.c_str()) != 0)
		{
			kResults.Reset();
			return false;
		}
		if (szType == NULL || _stricmp(szType, SqlTypeName(kExpected.type)) != 0)
		{
			kResults.Reset();
			return false;
		}

		++uiColumn;
	}

	kResults.Reset();

	// Match only if every expected column was found and there were no extras.
	return (uiColumn == kColumns.size()) && (uiColumn > 0);
}

//	-----------------------------------------------------------------------------------------------
void SqliteLogger::CreateTable(const std::string& strTableName, const TableDef& kColumns)
{
	std::string strSql = "CREATE TABLE \"";
	strSql += strTableName;
	strSql += "\" (";

	for (size_t i = 0; i < kColumns.size(); ++i)
	{
		if (i > 0)
			strSql += ", ";

		strSql += "\"";
		strSql += kColumns[i].name;
		strSql += "\" ";
		strSql += SqlTypeName(kColumns[i].type);
	}

	strSql += ")";

	if (!m_kConnection.Execute(strSql.c_str()))
	{
		ASSERT(false, "SqliteLogger: failed to create table");
	}
}

//	-----------------------------------------------------------------------------------------------
void SqliteLogger::RegisterTable(const std::string& strTableName, const TableDef& kColumns)
{
	if (!EnsureOpen())
		return;

	if (kColumns.empty())
	{
		ASSERT(false, "SqliteLogger: cannot register a table with no columns");
		return;
	}

	// Every table implicitly starts with a GameId (INT) and Turn (INT) column. These are
	// prepended to the caller-supplied schema here and bound automatically in BeginLogRow(),
	// so callers must neither declare nor bind them. GameId is an integer id into uuid_dictionary.
	TableDef kFullColumns;
	kFullColumns.reserve(kColumns.size() + 2);
	kFullColumns.push_back(ColumnDef("GameId", Database::COLTYPE_INT));
	kFullColumns.push_back(ColumnDef("Turn", Database::COLTYPE_INT));
	for (size_t i = 0; i < kColumns.size(); ++i)
		kFullColumns.push_back(kColumns[i]);

	// Reject duplicate column names (case-insensitive). This also catches callers that try to
	// re-declare the implicit GameId/Turn columns.
	for (size_t i = 0; i < kFullColumns.size(); ++i)
	{
		for (size_t j = i + 1; j < kFullColumns.size(); ++j)
		{
			if (_stricmp(kFullColumns[i].name.c_str(), kFullColumns[j].name.c_str()) == 0)
			{
				ASSERT(false, "SqliteLogger: duplicate column name in table definition (note: GameId and Turn are implicit and must not be declared)");
				return;
			}
		}
	}

	// Drop any cached prepared insert; it may be stale if we recreate the table below.
	InsertMap::iterator itInsert = m_preparedInserts.find(strTableName);
	if (itInsert != m_preparedInserts.end())
	{
		delete itInsert->second;
		m_preparedInserts.erase(itInsert);
	}

	if (!TableSchemaMatches(strTableName, kFullColumns))
	{
		// Table is missing or has a different schema: drop (if present) and (re)create.
		std::string strDrop = "DROP TABLE IF EXISTS \"";
		strDrop += strTableName;
		strDrop += "\"";
		m_kConnection.Execute(strDrop.c_str());

		CreateTable(strTableName, kFullColumns);
	}

	m_registeredTableSchemas[strTableName] = kFullColumns;
}

//	-----------------------------------------------------------------------------------------------
Database::Results* SqliteLogger::GetOrCreateInsert(const std::string& strTableName)
{
	InsertMap::iterator it = m_preparedInserts.find(strTableName);
	if (it != m_preparedInserts.end())
		return it->second;

	SchemaMap::iterator itSchema = m_registeredTableSchemas.find(strTableName);
	if (itSchema == m_registeredTableSchemas.end())
		return NULL;

	const TableDef& kColumns = itSchema->second;

	std::string strSql = "INSERT INTO \"";
	strSql += strTableName;
	strSql += "\" VALUES (";
	for (size_t i = 0; i < kColumns.size(); ++i)
	{
		if (i > 0)
			strSql += ",";
		strSql += "?";
	}
	strSql += ")";

	Database::Results* pkResults = new Database::Results();
	if (!m_kConnection.Execute(*pkResults, strSql.c_str()))
	{
		ASSERT(false, "SqliteLogger: failed to prepare insert statement");
		delete pkResults;
		return NULL;
	}

	m_preparedInserts[strTableName] = pkResults;
	return pkResults;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::Statement SqliteLogger::BeginLogRow(const std::string& strTableName)
{
	if (!EnsureOpen())
		return Statement(this, strTableName, NULL, NULL, false);

	SchemaMap::iterator itSchema = m_registeredTableSchemas.find(strTableName);
	if (itSchema == m_registeredTableSchemas.end())
	{
		ASSERT(false, "SqliteLogger: BeginLogRow called for an unregistered table");
		return Statement(this, strTableName, NULL, NULL, false);
	}

	Database::Results* pkResults = GetOrCreateInsert(strTableName);
	if (pkResults == NULL)
		return Statement(this, strTableName, NULL, NULL, false);

	// Clear any state/bindings from the previous row before starting a new one.
	pkResults->Reset();

	// The implicit GameId and Turn columns lead every table; bind them automatically here so that
	// callers only ever bind their own columns (in declaration order).
	Statement kStatement(this, strTableName, pkResults, &itSchema->second, true);
	kStatement.bind(GC.getGame().getGameDatabaseId());
	kStatement.bind(GC.getGame().getElapsedGameTurns());
	return kStatement;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::BatchWriter SqliteLogger::BeginLogBatch(const std::string& strTableName)
{
	if (!EnsureOpen())
		return BatchWriter(this, strTableName, NULL, NULL, 0, 0, 0, false);

	SchemaMap::iterator itSchema = m_registeredTableSchemas.find(strTableName);
	if (itSchema == m_registeredTableSchemas.end())
	{
		ASSERT(false, "SqliteLogger: BeginLogBatch called for an unregistered table");
		return BatchWriter(this, strTableName, NULL, NULL, 0, 0, 0, false);
	}

	Database::Results* pkResults = GetOrCreateInsert(strTableName);
	if (pkResults == NULL)
		return BatchWriter(this, strTableName, NULL, NULL, 0, 0, 0, false);

	// GameId and Turn are constant for the lifetime of a batch (same game, same turn), so capture
	// them once here instead of re-reading them for every buffered row.
	const int iGameId = GC.getGame().getGameDatabaseId();
	const int iTurn = GC.getGame().getElapsedGameTurns();

	// Higher values buffer more rows in memory before writing, reducing total write time; clamp to
	// at least one row so a misconfigured (zero/negative) value still makes forward progress.
	int iMaxRows = GD_INT_GET(SQLITE_LOGGING_BATCHED_BUFFER_ROWS_MAX);
	if (iMaxRows < 1)
		iMaxRows = 1;

	return BatchWriter(this, strTableName, &itSchema->second, pkResults, iGameId, iTurn, iMaxRows, true);
}

//	===============================================================================================
//	SqliteLogger::Statement
//	===============================================================================================

//	-----------------------------------------------------------------------------------------------
SqliteLogger::Statement::Statement(SqliteLogger* pkOwner, const std::string& strTableName, Database::Results* pkResults, const TableDef* pkSchema, bool bEnabled) :
	m_pkOwner(pkOwner),
	m_strTableName(strTableName),
	m_pkResults(pkResults),
	m_pkSchema(pkSchema),
	m_bEnabled(bEnabled),
	m_bValid(bEnabled),
	m_iBindIndex(0)
{
}

//	-----------------------------------------------------------------------------------------------
bool SqliteLogger::Statement::expectColumn(Database::ColumnTypes eActual)
{
	if (!m_bEnabled || !m_bValid)
		return false;

	if (m_pkSchema == NULL || m_iBindIndex >= (int)m_pkSchema->size())
	{
		ASSERT(false, "SqliteLogger: too many values bound for table");
		m_bValid = false;
		return false;
	}

	const Database::ColumnTypes eExpected = (*m_pkSchema)[m_iBindIndex].type;

	// The bind() overloads normalize float and double to COLTYPE_FLOAT, so an exact match is sufficient.
	const bool bMatch = (eExpected == eActual);

	if (!bMatch)
	{
		ASSERT(false, "SqliteLogger: bound value type does not match the registered column type");
		m_bValid = false;
		return false;
	}

	return true;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::Statement& SqliteLogger::Statement::bind(int iValue)
{
	if (expectColumn(Database::COLTYPE_INT))
	{
		m_pkResults->Bind(m_iBindIndex + 1, iValue);
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::Statement& SqliteLogger::Statement::bind(bool bValue)
{
	if (expectColumn(Database::COLTYPE_BOOL))
	{
		m_pkResults->Bind(m_iBindIndex + 1, bValue ? 1 : 0);
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::Statement& SqliteLogger::Statement::bind(float fValue)
{
	if (expectColumn(Database::COLTYPE_FLOAT))
	{
		m_pkResults->Bind(m_iBindIndex + 1, fValue);
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::Statement& SqliteLogger::Statement::bind(double dValue)
{
	if (expectColumn(Database::COLTYPE_FLOAT))
	{
		m_pkResults->Bind(m_iBindIndex + 1, dValue);
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::Statement& SqliteLogger::Statement::bind(const char* szValue)
{
	if (expectColumn(Database::COLTYPE_TEXT))
	{
		m_pkResults->Bind(m_iBindIndex + 1, (szValue != NULL) ? szValue : "");
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::Statement& SqliteLogger::Statement::bind(const std::string& strValue)
{
	if (expectColumn(Database::COLTYPE_TEXT))
	{
		m_pkResults->Bind(m_iBindIndex + 1, strValue.c_str());
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
void SqliteLogger::Statement::execute()
{
	if (!m_bEnabled || !m_bValid || m_pkResults == NULL || m_pkSchema == NULL)
		return;

	if (m_iBindIndex != (int)m_pkSchema->size())
	{
		ASSERT(false, "SqliteLogger: execute() called before all columns were bound");
		return;
	}

	if (!m_pkResults->Execute())
	{
		ASSERT(false, "SqliteLogger: failed to execute insert");
	}

	// Leave the statement ready for the next row.
	m_pkResults->Reset();
}

//	===============================================================================================
//	SqliteLogger::BatchWriter
//	===============================================================================================

//	-----------------------------------------------------------------------------------------------
SqliteLogger::BatchWriter::BatchWriter(SqliteLogger* pkOwner, const std::string& strTableName, const TableDef* pkSchema, Database::Results* pkResults, int iGameId, int iTurn, int iMaxRows, bool bEnabled) :
	m_pkOwner(pkOwner),
	m_strTableName(strTableName),
	m_pkSchema(pkSchema),
	m_pkResults(pkResults),
	m_iGameId(iGameId),
	m_iTurn(iTurn),
	m_iMaxRows(iMaxRows),
	m_bEnabled(bEnabled),
	m_buffer(),
	m_scratch()
{
}

//	-----------------------------------------------------------------------------------------------
int SqliteLogger::BatchWriter::CallerColumnCount() const
{
	// Every schema implicitly leads with GameId and Turn, which are not bound per row in a batch.
	if (m_pkSchema == NULL || m_pkSchema->size() < 2)
		return 0;
	return (int)m_pkSchema->size() - 2;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::BatchWriter::RowBuilder SqliteLogger::BatchWriter::BeginLogRow()
{
	// Discard any half-built row left over from a previous (incomplete) BeginLogRow.
	m_scratch.clear();
	return RowBuilder(this, m_bEnabled && m_pkResults != NULL && m_pkSchema != NULL);
}

//	-----------------------------------------------------------------------------------------------
void SqliteLogger::BatchWriter::flush()
{
	if (!m_bEnabled || m_pkResults == NULL || m_pkSchema == NULL)
		return;

	const int iCallerCols = CallerColumnCount();
	if (iCallerCols <= 0 || m_buffer.empty())
	{
		m_buffer.clear();
		return;
	}

	const size_t uiRows = m_buffer.size() / (size_t)iCallerCols;

	// Write every buffered row inside a single transaction so the per-row commit overhead is paid
	// once for the whole batch rather than once per row.
	m_pkOwner->m_kConnection.BeginTransaction();

	for (size_t uiRow = 0; uiRow < uiRows; ++uiRow)
	{
		m_pkResults->Reset();
		m_pkResults->Bind(1, m_iGameId);
		m_pkResults->Bind(2, m_iTurn);

		const size_t uiBase = uiRow * (size_t)iCallerCols;
		for (int iCol = 0; iCol < iCallerCols; ++iCol)
		{
			const BatchCell& kCell = m_buffer[uiBase + (size_t)iCol];
			const int iParam = iCol + 3; // 1=GameId, 2=Turn, caller columns follow

			switch (kCell.eType)
			{
			case Database::COLTYPE_TEXT:
				m_pkResults->Bind(iParam, kCell.strValue.c_str());
				break;
			case Database::COLTYPE_FLOAT:
				m_pkResults->Bind(iParam, kCell.dValue);
				break;
			case Database::COLTYPE_BOOL:
			case Database::COLTYPE_INT:
			default:
				m_pkResults->Bind(iParam, kCell.iValue);
				break;
			}
		}

		if (!m_pkResults->Execute())
		{
			ASSERT(false, "SqliteLogger: failed to execute batched insert");
		}
	}

	// Intentional: a failed row above asserts but does not abort the batch, and this is a plain
	// commit with no rollback path. For best-effort stats logging, committing the rows that did
	// succeed is preferable to discarding the whole batch on a single bad row.
	m_pkOwner->m_kConnection.CommitTransaction();

	m_pkResults->Reset();
	m_buffer.clear();
}

//	===============================================================================================
//	SqliteLogger::BatchWriter::RowBuilder
//	===============================================================================================

//	-----------------------------------------------------------------------------------------------
SqliteLogger::BatchWriter::RowBuilder::RowBuilder(BatchWriter* pkBatch, bool bEnabled) :
	m_pkBatch(pkBatch),
	m_bEnabled(bEnabled),
	m_bValid(bEnabled),
	m_iBindIndex(0)
{
}

//	-----------------------------------------------------------------------------------------------
bool SqliteLogger::BatchWriter::RowBuilder::expectColumn(Database::ColumnTypes eActual)
{
	if (!m_bEnabled || !m_bValid)
		return false;

	const int iCallerCols = m_pkBatch->CallerColumnCount();
	if (m_iBindIndex >= iCallerCols)
	{
		ASSERT(false, "SqliteLogger: too many values bound for batched row");
		m_bValid = false;
		return false;
	}

	// Caller columns start after the implicit GameId/Turn columns in the schema.
	const Database::ColumnTypes eExpected = (*m_pkBatch->m_pkSchema)[m_iBindIndex + 2].type;
	if (eExpected != eActual)
	{
		ASSERT(false, "SqliteLogger: bound value type does not match the registered column type");
		m_bValid = false;
		return false;
	}

	return true;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::BatchWriter::RowBuilder& SqliteLogger::BatchWriter::RowBuilder::bind(int iValue)
{
	if (expectColumn(Database::COLTYPE_INT))
	{
		BatchCell kCell;
		kCell.eType = Database::COLTYPE_INT;
		kCell.iValue = iValue;
		m_pkBatch->m_scratch.push_back(kCell);
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::BatchWriter::RowBuilder& SqliteLogger::BatchWriter::RowBuilder::bind(bool bValue)
{
	if (expectColumn(Database::COLTYPE_BOOL))
	{
		BatchCell kCell;
		kCell.eType = Database::COLTYPE_BOOL;
		kCell.iValue = bValue ? 1 : 0;
		m_pkBatch->m_scratch.push_back(kCell);
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::BatchWriter::RowBuilder& SqliteLogger::BatchWriter::RowBuilder::bind(float fValue)
{
	if (expectColumn(Database::COLTYPE_FLOAT))
	{
		BatchCell kCell;
		kCell.eType = Database::COLTYPE_FLOAT;
		kCell.dValue = (double)fValue;
		m_pkBatch->m_scratch.push_back(kCell);
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::BatchWriter::RowBuilder& SqliteLogger::BatchWriter::RowBuilder::bind(double dValue)
{
	if (expectColumn(Database::COLTYPE_FLOAT))
	{
		BatchCell kCell;
		kCell.eType = Database::COLTYPE_FLOAT;
		kCell.dValue = dValue;
		m_pkBatch->m_scratch.push_back(kCell);
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::BatchWriter::RowBuilder& SqliteLogger::BatchWriter::RowBuilder::bind(const char* szValue)
{
	if (expectColumn(Database::COLTYPE_TEXT))
	{
		BatchCell kCell;
		kCell.eType = Database::COLTYPE_TEXT;
		kCell.strValue = (szValue != NULL) ? szValue : "";
		m_pkBatch->m_scratch.push_back(kCell);
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
SqliteLogger::BatchWriter::RowBuilder& SqliteLogger::BatchWriter::RowBuilder::bind(const std::string& strValue)
{
	if (expectColumn(Database::COLTYPE_TEXT))
	{
		BatchCell kCell;
		kCell.eType = Database::COLTYPE_TEXT;
		kCell.strValue = strValue;
		m_pkBatch->m_scratch.push_back(kCell);
		++m_iBindIndex;
	}
	return *this;
}

//	-----------------------------------------------------------------------------------------------
void SqliteLogger::BatchWriter::RowBuilder::addRowToBatch()
{
	if (!m_bEnabled || !m_bValid)
	{
		m_pkBatch->m_scratch.clear();
		return;
	}

	if (m_iBindIndex != m_pkBatch->CallerColumnCount())
	{
		ASSERT(false, "SqliteLogger: addRowToBatch() called before all columns were bound");
		m_pkBatch->m_scratch.clear();
		return;
	}

	// Move the completed row's cells into the flat batch buffer, then reset the scratch row.
	for (size_t i = 0; i < m_pkBatch->m_scratch.size(); ++i)
		m_pkBatch->m_buffer.push_back(m_pkBatch->m_scratch[i]);
	m_pkBatch->m_scratch.clear();

	// Auto-flush once the buffer reaches the configured row cap to bound peak memory usage.
	const int iCallerCols = m_pkBatch->CallerColumnCount();
	if (iCallerCols > 0)
	{
		const size_t uiRows = m_pkBatch->m_buffer.size() / (size_t)iCallerCols;
		if (uiRows >= (size_t)m_pkBatch->m_iMaxRows)
			m_pkBatch->flush();
	}
}


 namespace AIUnitDecisionTrace {
	namespace {
		int nextDecision=0, decision=0, ordinal=0, player=-1, city=-1, kind=-1, object=-1;
		int nextFlavorState=0, flavorState=-1;
		int nextObservation=0;
		CvString decisionType, decisionTrigger, selectedAction;
		CvString currentOrderObjectType, winnerKindName, winnerObjectType;
		bool consumedFlavor=false, observedAction=false;
		bool conflictingFlavorStates=false;
		std::set<std::pair<int,int> > persistedStateFlavors;
		bool registered=false;
		bool inDecision=false;
		struct Candidate {
			int player,city,kind,object,base,sanity,finalWeight,turns,eligible,selected,f,b,pool; bool hasBase,hasTerm,hasScore;
			struct Part { int flavor,effective,coefficient,product; CvString status; Components components; Part(int f,int e,int c,int p,const char* st,const Components& parts):flavor(f),effective(e),coefficient(c),product(p),status(st),components(parts) {} };
			std::vector<Part> flavors;
			CvString reason, flavorStatus;
			Candidate():player(-1),city(-1),kind(-1),object(-1),base(0),sanity(0),finalWeight(0),turns(-1),eligible(-1),selected(0),f(0),b(0),pool(-1),hasBase(false),hasTerm(false),hasScore(false),flavorStatus("NOT_APPLICABLE"){
			}
		};
		typedef std::pair<int,int> Key;
		std::map<Key,Candidate> candidates;
		CvString selectionMode;
		SqliteLogger::BatchWriter* termBatch=NULL;
		std::vector<Key> postPool;
		const char* termNames[]={
			"","FLAVOR_CACHE","CACHED_WEIGHT","ROLE_RECOMMENDATION","RECOMMEND_DURATION","BASE_COMPRESSION","PURCHASE_AND_CITY","CITY_MATURITY","MAINTENANCE","PLACEMENT","COUNTS","FORCE_LIMIT","MINOR_FORCE","LANDMASS","RESOURCE_REQUIRED","RESOURCE_SURPLUS","SPECIAL_ROLE","CITY_SUPPORT","SUICIDE_NUKE","PURCHASE_SURPLUS","EXPLORATION_CARRIER","NAVAL_DEFICIT","LAND_FORCE_DEFICIT","AIR_BALANCE","ANTI_AIR","CONQUEST","DIPLOMAT","SPACESHIP","RELIGION","TRADE","TRAIT","SETTLER","ARCHAEOLOGIST","WORKBOAT","WORKER","WAR_PRESSURE","BEST_CITY","PROMOTION","OPERATION_ROLE","DOMAIN_COUNTS","CONTINUATION","COMPLETION_FREE","ROLE_BALANCE","FINAL"
		};
		const char* Name(const char* contract) {
			int i=contract && contract[0]=='t'?atoi(contract+1):0;
			return i>0 && i<=43?termNames[i]:contract;
		} const char* KindName(int k) {
			if(k==CITY_BUILDABLE_UNIT) return "UNIT";
			if(k==CITY_BUILDABLE_UNIT_FOR_OPERATION) return "UNIT_FOR_OPERATION";
			if(k==CITY_BUILDABLE_UNIT_FOR_ARMY) return "UNIT_FOR_ARMY";
			if(k==PROCESS_UNIT_PROXY) return "PROCESS_UNIT_PROXY";
			if(k==CITY_BUILDABLE_BUILDING) return "BUILDING";
			if(k==CITY_BUILDABLE_PROJECT) return "PROJECT";
			if(k==CITY_BUILDABLE_PROCESS) return "PROCESS";
			return "UNKNOWN";
		} int OrderKind(int order) {
			return order==ORDER_TRAIN ? CITY_BUILDABLE_UNIT : order==ORDER_CONSTRUCT ? CITY_BUILDABLE_BUILDING : order==ORDER_CREATE ? CITY_BUILDABLE_PROJECT : order==ORDER_MAINTAIN ? CITY_BUILDABLE_PROCESS : -1;
		} CvString Symbol(int k,int obj) {
			if(obj<0)return "UNKNOWN";
			CvBaseInfo* info=NULL;
			if(k==PROCESS_UNIT_PROXY || k==CITY_BUILDABLE_UNIT || k==CITY_BUILDABLE_UNIT_FOR_OPERATION || k==CITY_BUILDABLE_UNIT_FOR_ARMY)info=GC.getUnitInfo((UnitTypes)obj);
			else if(k==CITY_BUILDABLE_BUILDING)info=GC.getBuildingInfo((BuildingTypes)obj);
			else if(k==CITY_BUILDABLE_PROCESS)info=GC.getProcessInfo((ProcessTypes)obj);
			else if(k==CITY_BUILDABLE_PROJECT)info=GC.getProjectInfo((ProjectTypes)obj);
			return info?info->GetType():"UNKNOWN";
		}
		void Register(const char* name,const char* const* columns,int count) {
			TableDef schema;
			for(int i=0;i<count;++i)
				schema.push_back(ColumnDef(columns[i]+2, columns[i][0]=='I' ? Database::COLTYPE_INT : Database::COLTYPE_TEXT));
			GET_SQLITE_LOGGER().RegisterTable(name,schema);
		}
		CvString PlayerName(int p) {
			return p>=0 && p<MAX_PLAYERS ? GET_PLAYER((PlayerTypes)p).getCivilizationShortDescription() : "UNKNOWN";
		}
		CvString CityName(int p,int c) {
			if(p<0 || p>=MAX_PLAYERS) return "UNKNOWN";
			CvCity* pCity=GET_PLAYER((PlayerTypes)p).getCity(c);
			if(pCity) return pCity->getName();
			return "UNKNOWN";
		}
		void Ensure() {
			if(registered)return;
			const char* d[]={"I:ObservationID","I:DecisionID","T:Player","T:City","I:FlavorStateID","T:DecisionType","T:DecisionTrigger","T:SelectionMode","T:WinnerKindName","T:WinnerObjectType","T:CurrentOrderObjectType","T:ProposedAction"};
			Register("AIProductionDecision",d,12);
			const char* c[]={"I:DecisionID","T:Player","T:City","T:DecisionType","T:SelectionMode","T:BuildableKindName","T:ObjectType","I:InitialWeight","I:SanityWeight","I:FinalWeight","I:EstimatedTurnsLeft","I:PoolIndex","I:Selected","T:Reason","T:InitialWeightBreakdownStatus"};
			Register("AIProductionCandidate",c,15);
			const char* t[]={"I:DecisionID","I:Sequence","T:Player","T:City","T:BuildableKindName","T:ObjectType","I:BeforeValue","I:Operand","I:AfterValue","T:Step","T:Target","T:VariableName","T:Operation","T:Reason"};
			Register("AIProductionTerm",t,14);
			const char* f[]={"I:DecisionID","T:Player","T:City","T:ObjectType","T:FlavorType","I:EffectiveCityFlavor","I:UnitFlavorWeight","I:Contribution","T:BuildableKindName","T:EffectiveCityFlavorBreakdownStatus"};
			Register("AIUnitFlavorContribution",f,10);
			const char* fc[]={"I:FlavorStateID","T:Player","T:City","T:FlavorType","T:ComponentSource","I:ComponentValue"};
			Register("AICityFlavorComponent",fc,6);
			const char* e[]={"I:ObservationID","I:DecisionID","T:Player","T:City","T:EventType","T:BuildableKindName","T:ObjectType","T:Currency","T:Result"};
			Register("AIProductionEvent",e,9);
			registered=true;
		}
		void Term(const char* id,const char* target,const char* op,int before,int operand,int after,int /*site*/,const char* event,const char* reason) {
			if(!Enabled())return;
			Ensure();
			candidates[Key(kind,object)].hasTerm=true;
			const char* variable="";
			const char* semanticTarget=target;
			if (!strcmp(target,"iBonus")) { variable=target; semanticTarget="SANITY_BONUS"; }
			else if (!strcmp(target,"iFlavorWeight")) {
				variable=target;
				semanticTarget=!strcmp(id,"t005") ? "SANITY_BASE" : "SANITY_SCORE";
			}
			else if (!strcmp(target,"sanity")) semanticTarget="SANITY_SCORE";
			else if (!strcmp(target,"recommendation")) semanticTarget="RECOMMENDATION_WEIGHT";
			else if (!strcmp(target,"competition")) semanticTarget="COMPETITION_WEIGHT";
			else if (!strcmp(target,"selection")) semanticTarget="SELECTION";
			if (!strcmp(op,"DURATION") && !strcmp(target,"competition")) variable="iTurns";
			const char* step=Name(id);
			if (!strcmp(op,"CUTOFF")) step="SELECTION_CUTOFF";
			else if (!strcmp(op,"DRAW")) step=id;
			else if (!strcmp(op,"SELECTED")) step="CANDIDATE_SELECTED";
			else if (!strcmp(op,"RECOMMENDED")) step="ROLE_RECOMMENDED";
			else if (!id[0] && !strcmp(op,"DURATION")) step="DURATION_REWEIGHTING";
			else if (!id[0]) step=event;
			termBatch->BeginLogRow().bind(decision).bind(++ordinal).bind(PlayerName(player).c_str()).bind(CityName(player,city).c_str()).bind(KindName(kind)).bind(Symbol(kind,object).c_str()).bind(before).bind(operand).bind(after).bind(step).bind(semanticTarget).bind(variable).bind(op).bind(reason).addRowToBatch();
		}
	}
	int CurrentDecision() { return Enabled() ? decision : 0; }
	void DecisionContext(const char* trigger) { if (Enabled()) decisionTrigger=trigger; }
	void DecisionAction(const char* action) { if (Enabled()) selectedAction=action; }
	void Duration(int before,int turns,int after) {
		if (!Enabled()) return;
		Candidate& c=candidates[Key(kind,object)]; c.hasScore=true;
		if (c.sanity==0) c.sanity=before;
		c.finalWeight=after;
		if (before!=after) Term("","competition","DURATION",before,turns,after,0,"APPLIED","");
	}
	void ProductionEvent(const CvCity* c,const char* type,int order,int obj,int origin,const char* currency,const char* result) {
		if (!Configured() || !GET_SQLITE_LOGGER().IsEnabled() || !c || obj<0) return;
		Ensure();
		const int k=OrderKind(order);
		if (Enabled() && origin==decision && player==c->getOwner() && city==c->GetID()) observedAction=true;
		GET_SQLITE_LOGGER().BeginLogRow("AIProductionEvent").bind(++nextObservation).bind(origin).bind(PlayerName(c->getOwner()).c_str()).bind(c->getName().c_str()).bind(type).bind(KindName(k)).bind(Symbol(k,obj).c_str()).bind(currency).bind(result).execute();
	}
	Action::Action(const CvCity* c,int origin,int order,int obj,const char* type,const char* currency):m_city(c),m_decision(origin),m_kind(order),m_object(obj),m_type(type),m_currency(currency),m_success(false),m_enabled(false) {
		// Shared purchase dispatchers also call these APIs with NO_UNIT/NO_BUILDING.
		m_enabled=Configured() && GET_SQLITE_LOGGER().IsEnabled() && obj>=0;
	}
	void Action::Success(const char* type) { m_success=true; if (type) m_type=type; }
	Action::~Action() {
		if (m_enabled) ProductionEvent(m_city,m_type,m_kind,m_object,m_decision,m_currency,m_success ? "APPLIED" : "FAILED");
	}
	bool Configured(){
		return MOD_AI_UNIT_DECISION_LOGGING && MOD_SQLITE_LOGGING;
	}
	bool Enabled(){
		return Configured() && inDecision && decision!=0;
	}
	bool Active(){
		return Enabled();
	}
	void SetCandidate(int k,int obj){
		if(!Enabled())return;
		kind=k;
		object=obj;

	}
	void SetObject(int obj){
		if(Enabled())object=obj;
	}
	Scope::Scope(const char* caller,int p,int c):m_root(false),m_active(false),m_kind(kind),m_object(object),m_player(player),m_city(city),m_previousDecision(decision),m_previousInDecision(inDecision) {
		if(!Configured())return;

		if(!GET_SQLITE_LOGGER().IsEnabled())return;

		if(!inDecision && termBatch==NULL && (strcmp(caller,"ChooseProduction")==0 || strcmp(caller,"ChooseHurry")==0)) {
			m_root=true;
			inDecision=true;

			Ensure();

			termBatch=new SqliteLogger::BatchWriter(GET_SQLITE_LOGGER().BeginLogBatch("AIProductionTerm"));

			decision=++nextDecision;
			decisionType=caller;
			decisionTrigger="UNKNOWN";
			selectedAction="NONE";
			currentOrderObjectType="UNKNOWN";
			winnerKindName=winnerObjectType="UNKNOWN";
			consumedFlavor=observedAction=false;
			const CvCity* traceCity=GET_PLAYER((PlayerTypes)p).getCity(c);
			const OrderData* traceHead=traceCity ? traceCity->headOrderQueueNode() : NULL;
			if (traceHead) currentOrderObjectType=Symbol(OrderKind(traceHead->eOrderType),traceHead->iData1);

			ordinal=0;
			flavorState=-1;
			conflictingFlavorStates=false;
			candidates.clear();
			selectionMode="NO_CHOICE";
			postPool.clear();
		}    m_active=decision!=0;
		if(m_active){
			player=p;
			city=c;
			m_caller=caller;
			// A process can evaluate a free unit using a fixed sanity input, not its own process identity.
			if (!strcmp(caller,"CheckUnitBuildSanity") && kind==CITY_BUILDABLE_PROCESS) kind=PROCESS_UNIT_PROXY;
		}
	}
	Scope::~Scope(){
		if(!m_active)return;
		if(m_root){
			// Discard read-only touches before testing scope content, regardless of mode or state ID.
			for (std::map<Key,Candidate>::iterator it=candidates.begin(); it!=candidates.end();) {
				const Candidate& c=it->second;
				if (!c.hasScore && !c.hasTerm && c.reason.empty() && c.pool<0 && !c.selected) candidates.erase(it++);
				else ++it;
			}

			termBatch->flush();

			delete termBatch;
			termBatch=NULL;
			for(std::map<Key,Candidate>::const_iterator it=candidates.begin();it!=candidates.end();++it){
				const Candidate& c=it->second;
				CvString symbol=Symbol(c.kind,c.object);
				CvString playerName=PlayerName(c.player), cityName=CityName(c.player,c.city);
				GET_SQLITE_LOGGER().BeginLogRow("AIProductionCandidate").bind(decision).bind(playerName.c_str()).bind(cityName.c_str()).bind(decisionType.c_str()).bind(selectionMode.c_str()).bind(KindName(c.kind)).bind(symbol.c_str()).bind(c.base).bind(c.sanity).bind(c.finalWeight).bind(c.turns).bind(c.eligible==1 ? c.pool : -1).bind(c.selected).bind(c.reason.c_str()).bind(c.flavorStatus.c_str()).execute();
				for(size_t i=0;i<c.flavors.size();++i) {
					const Candidate::Part& f=c.flavors[i];
					const char* flavorName=GC.getFlavorTypes((FlavorTypes)f.flavor).c_str();
					if (!conflictingFlavorStates && flavorState>=0 && f.status!="UNAVAILABLE" && persistedStateFlavors.insert(std::make_pair(flavorState,f.flavor)).second) {
						for (size_t j=0; j<f.components.size(); ++j) {
							const Component& part=f.components[j];
							if (part.value) GET_SQLITE_LOGGER().BeginLogRow("AICityFlavorComponent").bind(flavorState).bind(playerName.c_str()).bind(cityName.c_str()).bind(flavorName).bind(part.source.c_str()).bind(part.value).execute();
						}
					}
					GET_SQLITE_LOGGER().BeginLogRow("AIUnitFlavorContribution").bind(decision).bind(playerName.c_str()).bind(cityName.c_str()).bind(symbol.c_str()).bind(flavorName).bind(f.effective).bind(f.coefficient).bind(f.product).bind(KindName(c.kind)).bind(conflictingFlavorStates ? "UNAVAILABLE" : f.status.c_str()).execute();
				}
			}

			if (!candidates.empty() || ordinal>0 || consumedFlavor || observedAction || selectedAction!="NONE") {
				// Allocate only as the finalized summary is emitted. Events retain native emission order.
				GET_SQLITE_LOGGER().BeginLogRow("AIProductionDecision").bind(++nextObservation).bind(decision).bind(PlayerName(player).c_str()).bind(CityName(player,city).c_str()).bind(conflictingFlavorStates ? -1 : flavorState).bind(m_caller).bind(decisionTrigger.c_str()).bind(selectionMode.c_str()).bind(winnerKindName.c_str()).bind(winnerObjectType.c_str()).bind(currentOrderObjectType.c_str()).bind(selectedAction.c_str()).execute();
			}
			decision=m_previousDecision;
			inDecision=m_previousInDecision;
		}decision=m_previousDecision;
		inDecision=m_previousInDecision;
		kind=m_kind;
		object=m_object;
		player=m_player;
		city=m_city;
	}
	void Arithmetic(const char* id,int,int,int unit,int site,const char* target,const char* op,int before,int operand,int after){
		if(!Enabled())return;
		object=unit;
		if(strcmp(target,"iBonus") && strcmp(target,"iFlavorWeight"))return;
		Candidate& c=candidates[Key(kind,object)];
		if(strcmp(target,"iBonus")==0)c.b=after;
		else {c.f=after;if(strcmp(id,"t043")==0 && strcmp(op,"+=")==0)c.b=0;}
		const bool root=strcmp(id,"t005")==0 && strcmp(target,"iFlavorWeight")==0;
		if (!root && before==after) return;
		const char* operation = !strcmp(op,"+=")?"ADD":!strcmp(op,"-=")?"SUBTRACT":!strcmp(op,"*=")?"MULTIPLY":!strcmp(op,"/=")?"DIVIDE":"ASSIGN";
		const char* strategy="";
		if (!strcmp(id,"t042") && site>=124 && site<=132) {
			const char* names[]={"MILITARYAISTRATEGY_NEED_ARCHER","MILITARYAISTRATEGY_NEED_SIEGE","MILITARYAISTRATEGY_NEED_SKIRMISHER","MILITARYAISTRATEGY_NEED_MOBILE","MILITARYAISTRATEGY_NEED_NAVAL_MELEE","MILITARYAISTRATEGY_NEED_NAVAL_RANGED","MILITARYAISTRATEGY_NEED_SUBMARINE","MILITARYAISTRATEGY_NEED_BOMBER","MILITARYAISTRATEGY_NEED_FIGHTER"};
			strategy=names[site-124];
		}
		Term(id,target,root?"ROOT":operation,before,root?10:operand,after,site,"APPLIED",strategy);
	}
	int NewFlavorState() { return ++nextFlavorState; }
	void ConsumeFlavorState(int state) {
		if (!Enabled() || state<0) return;
		consumedFlavor=true;
		if (flavorState<0 && !conflictingFlavorStates) flavorState=state;
		else if (flavorState!=state) { flavorState=-1; conflictingFlavorStates=true; }
	}
	void FlavorStatus(bool complete) {
		if (!Enabled() || object<0) return;
		Candidate& c=candidates[Key(kind,object)]; c.flavors.clear(); c.flavorStatus=complete?"COMPLETE":"UNAVAILABLE";
	}
	void Flavor(int unit,int flavor,int effective,int coefficient,int product,const Components& parts,bool available) {
		if (!Enabled() || unit!=object || !product) return;
		int sum=0; for(size_t i=0;i<parts.size();++i) sum+=parts[i].value;
		const char* status=!available ? "UNAVAILABLE" : sum==effective ? "COMPLETE" : "INCOMPLETE";
		candidates[Key(kind,object)].flavors.push_back(Candidate::Part(flavor,effective,coefficient,product,status,parts));
	}
	void Event(const char* event,const char* id,int,int,int k,int obj,int before,int after,const char* field){
		if(!Enabled())return;
		if(strcmp(event,"NO_PRODUCTION_CHOICE")==0 || strcmp(event,"HURRY_NO_CHOICE")==0 || strcmp(event,"FALLBACK_PRE")==0 || strncmp(event,"FILTER_",7)==0 || strcmp(event,"UNIT_ENUMERATED")==0 || strcmp(event,"ROLE_ENUMERATED")==0 || strcmp(event,"HURRY_UNIT_ENUMERATED")==0) return;
		if(k==-1)k=kind;
		if(obj<0 && (strcmp(event,"DEMAND_RECOMMENDATION")==0 || strcmp(event,"HURRY_DEMAND")==0)){
			return;
		}
		if(obj<0)obj=object;
		if(obj<0)return;
		kind=k;
		object=obj;
		Candidate& c=candidates[Key(k,obj)];
		if(strcmp(event,"CACHED_WEIGHT")==0) { c.base=before; c.hasBase=true; }
		if(strcmp(event,"NONFLAVOR_BASE")==0) { c.base=before; c.hasBase=true; c.flavors.clear(); c.flavorStatus="NOT_APPLICABLE"; }
		if(strcmp(event,"INPUT")==0 && strcmp(id,"t005")==0){c.hasScore=true;c.f=before;c.b=0; if(!c.hasBase) { c.base=before; c.hasBase=true; }}
		c.player=player;
		c.city=city;
		c.kind=k;
		c.object=obj;
		if((strcmp(event,"PRE")==0 && !c.hasBase) || strcmp(event,"BASE_WEIGHT")==0) { c.base=before; c.hasBase=true; }
		if(strcmp(event,"PRE")==0) { c.hasScore=true; c.turns=after; }
		if(strcmp(event,"POST")==0 || strcmp(event,"POST_FALLBACK")==0){
			c.finalWeight=before;
			c.turns=after;
			c.pool=(int)postPool.size();
			postPool.push_back(Key(k,obj));
		}  // The shared logger cannot bind NULL. Non-arithmetic events use zero placeholders;
		// their operation and reason carry the semantics, not those numeric columns.
		if(strcmp(event,"SELECTED")==0 || strcmp(event,"HURRY_SELECTED")==0){
			c.selected=1;
			winnerKindName=KindName(k);
			winnerObjectType=Symbol(k,obj);
			Term(id,"selection","SELECTED",0,0,0,0,"SELECTED",strcmp(event,"HURRY_SELECTED")==0 ? (after ? "FAITH" : "GOLD") : "");
		}  if(strstr(event,"CONTINUED") || strstr(event,"OVERRIDE"))selectionMode=event;
		if(strncmp(event,"FILTER_",7)==0)c.reason=event;
		if(strcmp(event,"RETURN")==0){
			c.sanity=after;
			if(after<=0)c.reason=field;
			if(after<=0) Term(id,"sanity","REJECT",before,0,after,0,"REJECTED",field);
			return;
		}  if(strcmp(event,"RECOMMEND_DURATION")==0)c.hasScore=true;
		if(strcmp(event,"RECOMMEND_DURATION")==0 && before!=after)Term("t004","recommendation","DURATION",before,0,after,0,"APPLIED","");
		if(strcmp(event,"ROLE_RECOMMENDED")==0)Term("t004","recommendation","RECOMMENDED",0,0,0,0,"RECOMMENDED","");
		if(strstr(event,"OVERRIDE") || strstr(event,"CONTINUED"))Term(event,"competition","ASSIGN",0,0,0,0,event,"");
	}
	void Weighted(const char* event,int a,int b,int c){
		if(!Enabled())return;
		selectionMode="WEIGHTED";
		if(strcmp(event,"POOL")==0 && a>=0 && a<(int)postPool.size())candidates[postPool[a]].eligible=b>=c?1:0;
		if(strcmp(event,"MODE_NO_CUTOFF")==0 || strcmp(event,"MODE_UNIFORM")==0)for(size_t i=0;i<postPool.size();++i)candidates[postPool[i]].eligible=1;
		if(strcmp(event,"CUTOFF")==0) Term("","selection","CUTOFF",b,a,c,0,"APPLIED","highest_weight;percent;cutoff_weight");
		else if(strcmp(event,"WEIGHTED_DRAW")==0 || strcmp(event,"UNIFORM_DRAW")==0) {
			CvString detail; detail.Format("%s; draw=%d; %s=%d",event,a,strcmp(event,"UNIFORM_DRAW")==0 ? "pool_size" : "total_weight",b);
			Term(strcmp(event,"UNIFORM_DRAW")==0 ? "UNIFORM_SELECTION_DRAW" : "WEIGHTED_SELECTION_DRAW","selection","DRAW",0,0,0,0,"APPLIED",detail.c_str());
		}
	}
}
