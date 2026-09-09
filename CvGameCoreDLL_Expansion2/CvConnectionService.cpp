/*	-------------------------------------------------------------------------------------------------------
	Optional Windows named-pipe transport for the Vox Deorum bridge.

	The pipe thread only reads and writes serialized strings.  It never calls
	Lua and never accesses the game model.  Requests are handed to the game
	thread through m_incomingQueue and responses are returned through
	m_outgoingQueue.
	------------------------------------------------------------------------------------------------------- */

#include "CvGameCoreDLLPCH.h"
#include "CvConnectionService.h"

#include "../ThirdPartyLibs/ArduinoJson.hpp"
#include "FireWorks/FILogFile.h"
#include "ICvDLLScriptSystem.h"
#include "Lua/CvLuaSupport.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <sstream>

using namespace ArduinoJson;

namespace
{
	const char* const kPipeDelimiter = "!@#$%^!";
	const char* const kDefaultPipeId = "vox-deorum-bridge";
	const DWORD kPipeBufferSize = 262144;
	const size_t kReadBufferSize = 65536;
	const size_t kOutgoingChunkSize = 32768;
	const size_t kMaximumMessageBuffer = 8 * 1024 * 1024;
	const size_t kMaximumLogMessage = 3800;
	const size_t kJsonDocumentCapacity = 1024 * 1024;
	const size_t kMaximumErrorMessage = 8192;
	const int kMaximumLuaTableDepth = 32;

	void ClosePipeHandle(HANDLE hPipe)
	{
		if (hPipe != INVALID_HANDLE_VALUE && hPipe != NULL)
		{
			DisconnectNamedPipe(hPipe);
			CloseHandle(hPipe);
		}
	}

	std::string BuildPipeName()
	{
		const char* configuredName = std::getenv("VOX_DEORUM_PIPE_NAME");
		const char* pipeId = (configuredName != NULL && configuredName[0] != '\0')
			? configuredName
			: kDefaultPipeId;

		// Keep the complete Win32 pipe name comfortably below the documented
		// named-pipe name limit.  A too-long environment value is not allowed to
		// turn into an unbounded diagnostic or an invalid endpoint.
		std::string name("\\\\.\\pipe\\tmp-app.");
		name.append(pipeId, std::min<size_t>(std::strlen(pipeId), 220));
		return name;
	}
}

CvConnectionService& CvConnectionService::GetInstance()
{
	static CvConnectionService instance;
	return instance;
}

CvConnectionService::CvConnectionService()
	: m_bInitialized(false)
	, m_bShutdownRequested(false)
	, m_bClientConnected(false)
	, m_bProcessingMessages(false)
	, m_pLuaState(NULL)
	, m_pScriptSystem(NULL)
	, m_hThread(NULL)
	, m_hShutdownEvent(NULL)
	, m_hPipe(INVALID_HANDLE_VALUE)
	, m_dwThreadId(0)
{
	// These sections live for the lifetime of the singleton.  Keeping them
	// alive across Setup/Shutdown cycles avoids a teardown race with a pump.
	InitializeCriticalSection(&m_csIncoming);
	InitializeCriticalSection(&m_csOutgoing);
	InitializeCriticalSection(&m_csProcessing);
}

CvConnectionService::~CvConnectionService()
{
	Shutdown();
	DeleteCriticalSection(&m_csIncoming);
	DeleteCriticalSection(&m_csOutgoing);
	DeleteCriticalSection(&m_csProcessing);
}

bool CvConnectionService::Setup()
{
	if (m_bInitialized)
	{
		return true;
	}

	Log("ConnectionService::Setup - initializing");

	EnterCriticalSection(&m_csIncoming);
	m_incomingQueue.clear();
	LeaveCriticalSection(&m_csIncoming);
	EnterCriticalSection(&m_csOutgoing);
	m_outgoingQueue.clear();
	LeaveCriticalSection(&m_csOutgoing);

	m_bShutdownRequested = false;
	m_bClientConnected = false;
	m_hPipe = INVALID_HANDLE_VALUE;

	m_hShutdownEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
	if (m_hShutdownEvent == NULL)
	{
		Log("ConnectionService::Setup - failed to create shutdown event");
		return false;
	}

	m_pScriptSystem = gDLL->GetScriptSystem();
	if (m_pScriptSystem == NULL)
	{
		Log("ConnectionService::Setup - script system unavailable");
		CloseHandle(m_hShutdownEvent);
		m_hShutdownEvent = NULL;
		return false;
	}

	// This state is owned by the game thread for its entire service lifetime.
	m_pLuaState = m_pScriptSystem->CreateLuaThread("ConnectionService");
	if (m_pLuaState == NULL)
	{
		Log("ConnectionService::Setup - failed to create Lua state");
		CloseHandle(m_hShutdownEvent);
		m_hShutdownEvent = NULL;
		m_pScriptSystem = NULL;
		return false;
	}

	LuaSupport::RegisterScriptData(m_pLuaState);
	// RegisterScriptData leaves type tables on the stack in this VP tree.
	// They are registration state, not results of the first external script.
	lua_settop(m_pLuaState, 0);

	m_hThread = CreateThread(NULL, 0, NamedPipeServerThread, this, 0, &m_dwThreadId);
	if (m_hThread == NULL)
	{
		Log("ConnectionService::Setup - failed to create pipe thread");
		m_pScriptSystem->FreeLuaThread(m_pLuaState);
		m_pLuaState = NULL;
		m_pScriptSystem = NULL;
		CloseHandle(m_hShutdownEvent);
		m_hShutdownEvent = NULL;
		return false;
	}

	m_bInitialized = true;
	Log("ConnectionService::Setup - initialized");
	return true;
}

void CvConnectionService::Shutdown()
{
	// Fence the game-thread pump before freeing its Lua state or queues.
	EnterCriticalSection(&m_csProcessing);
	m_bInitialized = false;
	LeaveCriticalSection(&m_csProcessing);

	m_bShutdownRequested = true;
	m_bClientConnected = false;
	if (m_hShutdownEvent != NULL)
	{
		SetEvent(m_hShutdownEvent);
	}

	// The server uses PIPE_NOWAIT and checks the event between every read,
	// write, and wait.  The pipe thread owns and closes its pipe handle; the
	// game thread never closes a handle still in use by that thread.
	if (m_hThread != NULL)
	{
		WaitForSingleObject(m_hThread, INFINITE);
		CloseHandle(m_hThread);
		m_hThread = NULL;
	}

	if (m_pScriptSystem != NULL && m_pLuaState != NULL)
	{
		m_pScriptSystem->FreeLuaThread(m_pLuaState);
	}
	m_pLuaState = NULL;
	m_pScriptSystem = NULL;

	EnterCriticalSection(&m_csIncoming);
	m_incomingQueue.clear();
	LeaveCriticalSection(&m_csIncoming);
	EnterCriticalSection(&m_csOutgoing);
	m_outgoingQueue.clear();
	LeaveCriticalSection(&m_csOutgoing);

	if (m_hShutdownEvent != NULL)
	{
		CloseHandle(m_hShutdownEvent);
		m_hShutdownEvent = NULL;
	}
	m_hPipe = INVALID_HANDLE_VALUE;
	m_dwThreadId = 0;
	m_bClientConnected = false;
}

int CvConnectionService::SendMessage(const std::string& message)
{
	if (!m_bInitialized)
	{
		return 0;
	}

	QueueOutgoingMessage(message);
	EnterCriticalSection(&m_csOutgoing);
	int queueSize = static_cast<int>(m_outgoingQueue.size());
	LeaveCriticalSection(&m_csOutgoing);
	return queueSize;
}

void CvConnectionService::QueueIncomingMessage(const std::string& message)
{
	bool locked = false;
	try
	{
		EnterCriticalSection(&m_csIncoming);
		locked = true;
		m_incomingQueue.push_back(message);
		LeaveCriticalSection(&m_csIncoming);
		locked = false;
	}
	catch (...)
	{
		if (locked)
		{
			LeaveCriticalSection(&m_csIncoming);
		}
		Log("ConnectionService::QueueIncomingMessage - allocation failed");
	}
}

void CvConnectionService::QueueOutgoingMessage(const std::string& message)
{
	bool locked = false;
	try
	{
		EnterCriticalSection(&m_csOutgoing);
		locked = true;
		m_outgoingQueue.push_back(message);
		LeaveCriticalSection(&m_csOutgoing);
		locked = false;
	}
	catch (...)
	{
		if (locked)
		{
			LeaveCriticalSection(&m_csOutgoing);
		}
		Log("ConnectionService::QueueOutgoingMessage - allocation failed");
	}
}

void CvConnectionService::RequeueOutgoingMessage(const std::string& message)
{
	bool locked = false;
	try
	{
		EnterCriticalSection(&m_csOutgoing);
		locked = true;
		m_outgoingQueue.push_front(message);
		LeaveCriticalSection(&m_csOutgoing);
		locked = false;
	}
	catch (...)
	{
		if (locked)
		{
			LeaveCriticalSection(&m_csOutgoing);
		}
		Log("ConnectionService::RequeueOutgoingMessage - allocation failed");
	}
}

bool CvConnectionService::DequeueOutgoingMessage(std::string& message)
{
	bool locked = false;
	try
	{
		EnterCriticalSection(&m_csOutgoing);
		locked = true;
		if (m_outgoingQueue.empty())
		{
			LeaveCriticalSection(&m_csOutgoing);
			return false;
		}
		message = m_outgoingQueue.front();
		m_outgoingQueue.pop_front();
		LeaveCriticalSection(&m_csOutgoing);
		locked = false;
		return true;
	}
	catch (...)
	{
		if (locked)
		{
			LeaveCriticalSection(&m_csOutgoing);
		}
		Log("ConnectionService::DequeueOutgoingMessage - allocation failed");
		return false;
	}
}

void CvConnectionService::ProcessMessages()
{
	if (!m_bInitialized)
	{
		return;
	}

	// Try rather than wait: this pump is called from the game update path and
	// must never stall it behind another pump or a pipe operation.
	if (!TryEnterCriticalSection(&m_csProcessing))
	{
		return;
	}
	if (m_bProcessingMessages)
	{
		LeaveCriticalSection(&m_csProcessing);
		return;
	}
	m_bProcessingMessages = true;

	std::deque<std::string> messages;
	try
	{
		EnterCriticalSection(&m_csIncoming);
		messages.swap(m_incomingQueue);
		LeaveCriticalSection(&m_csIncoming);

		// Snapshot the queue once.  A request arriving while a script is running
		// waits for the next update rather than extending this pump indefinitely.
		while (!messages.empty())
		{
			std::string message = messages.front();
			messages.pop_front();
			RouteMessage(message);
		}
	}
	catch (...)
	{
		// Do not lose requests that were not reached if a C++ allocation or
		// router implementation unexpectedly throws.
		try
		{
			EnterCriticalSection(&m_csIncoming);
			while (!messages.empty())
			{
				m_incomingQueue.push_front(messages.back());
				messages.pop_back();
			}
			LeaveCriticalSection(&m_csIncoming);
		}
		catch (...)
		{
			LeaveCriticalSection(&m_csIncoming);
		}
		Log("ConnectionService::ProcessMessages - router exception");
	}

	m_bProcessingMessages = false;
	LeaveCriticalSection(&m_csProcessing);
}

bool CvConnectionService::RouteMessage(const std::string& message)
{
	DynamicJsonDocument request(kJsonDocumentCapacity);
	DeserializationError parseError = deserializeJson(request, message);
	if (parseError)
	{
		std::string details("Invalid JSON request: ");
		details.append(parseError.c_str());
		SendProtocolError("", "INVALID_JSON", details);
		return true;
	}

	JsonVariant typeValue = request["type"];
	const char* rawType = typeValue.as<const char*>();
	if (rawType == NULL)
	{
		SendProtocolError("", "INVALID_REQUEST", "Request is missing a string 'type' field");
		return true;
	}

	std::string messageType(rawType);
	JsonVariant idValue = request["id"];
	const char* rawId = idValue.as<const char*>();
	std::string id = rawId != NULL ? rawId : "";
	if (id.size() > 1024)
	{
		id.resize(1024);
	}

	if (messageType == "lua_execute")
	{
		JsonVariant scriptValue = request["script"];
		const char* rawScript = scriptValue.as<const char*>();
		if (rawScript == NULL || rawId == NULL)
		{
			SendProtocolError(id, "INVALID_REQUEST", "lua_execute requires string fields 'script' and 'id'");
			return true;
		}

		// ArduinoJson string accessors alias the request document.  Copy both
		// values before any handler can allocate another document or re-enter the
		// router, so no handler retains a pointer into a mutable JSON buffer.
		std::string script(rawScript);
		HandleLuaExecute(script, id);
		return true;
	}

	SendProtocolError(id, "UNSUPPORTED_MESSAGE", "Only lua_execute is supported by this IPC overlay");
	return true;
}

namespace
{
	void SetJsonNull(JsonVariant parent, const char* key)
	{
		if (key != NULL && parent.is<JsonObject>())
		{
			parent[key] = (char*)0;
		}
		else if (parent.is<JsonArray>())
		{
			parent.add((char*)0);
		}
		else
		{
			parent.set((char*)0);
		}
	}

	void SetJsonBoolean(JsonVariant parent, const char* key, bool value)
	{
		if (key != NULL && parent.is<JsonObject>())
		{
			parent[key] = value;
		}
		else if (parent.is<JsonArray>())
		{
			parent.add(value);
		}
		else
		{
			parent.set(value);
		}
	}

	void SetJsonInteger(JsonVariant parent, const char* key, int value)
	{
		if (key != NULL && parent.is<JsonObject>())
		{
			parent[key] = value;
		}
		else if (parent.is<JsonArray>())
		{
			parent.add(value);
		}
		else
		{
			parent.set(value);
		}
	}

	void SetJsonNumber(JsonVariant parent, const char* key, double value)
	{
		if (key != NULL && parent.is<JsonObject>())
		{
			parent[key] = value;
		}
		else if (parent.is<JsonArray>())
		{
			parent.add(value);
		}
		else
		{
			parent.set(value);
		}
	}

	void SetJsonString(JsonVariant parent, const char* key, const char* value)
	{
		if (value == NULL)
		{
			SetJsonNull(parent, key);
		}
		else if (key != NULL && parent.is<JsonObject>())
		{
			parent[key] = value;
		}
		else if (parent.is<JsonArray>())
		{
			parent.add(value);
		}
		else
		{
			parent.set(value);
		}
	}

	void SetJsonString(JsonVariant parent, const char* key, const std::string& value)
	{
		SetJsonString(parent, key, value.c_str());
	}

	void ConvertLuaToJsonValue(lua_State* state, int index, JsonVariant parent, const char* key, int depth);

	void ConvertLuaValuesToJson(lua_State* state, int firstIndex, int valueCount, DynamicJsonDocument& response)
	{
		if (valueCount <= 0)
		{
			response["result"] = (char*)0;
		}
		else if (valueCount == 1)
		{
			ConvertLuaToJsonValue(state, firstIndex, response.as<JsonObject>(), "result", 0);
		}
		else
		{
			JsonArray result = response.createNestedArray("result");
			if (!result)
			{
				return;
			}
			for (int i = 0; i < valueCount; ++i)
			{
				ConvertLuaToJsonValue(state, firstIndex + i, result, NULL, 0);
			}
		}
	}

	void ConvertLuaToJsonValue(lua_State* state, int index, JsonVariant parent, const char* key, int depth)
	{
		if (index < 0 && index > LUA_REGISTRYINDEX)
		{
			index = lua_gettop(state) + index + 1;
		}

		const int type = lua_type(state, index);
		if (type == LUA_TTABLE && depth >= kMaximumLuaTableDepth)
		{
			SetJsonString(parent, key, "<table depth limit>");
			return;
		}

		switch (type)
		{
		case LUA_TNIL:
			SetJsonNull(parent, key);
			break;

		case LUA_TBOOLEAN:
			SetJsonBoolean(parent, key, lua_toboolean(state, index) != 0);
			break;

		case LUA_TNUMBER:
		{
			const double number = lua_tonumber(state, index);
			if (number != number || number > DBL_MAX || number < -DBL_MAX)
			{
				SetJsonNull(parent, key);
			}
			else if (std::floor(number) == number && number >= INT_MIN && number <= INT_MAX)
			{
				SetJsonInteger(parent, key, static_cast<int>(number));
			}
			else
			{
				SetJsonNumber(parent, key, number);
			}
			break;
		}

		case LUA_TSTRING:
			SetJsonString(parent, key, lua_tostring(state, index));
			break;

		case LUA_TTABLE:
		{
			const int tableIndex = index;
			bool isArray = true;
			int keyCount = 0;
			int maximumIndex = 0;

			lua_pushnil(state);
			while (lua_next(state, tableIndex) != 0)
			{
				++keyCount;
				if (lua_type(state, -2) != LUA_TNUMBER)
				{
					isArray = false;
				}
				else
				{
					const double numericKey = lua_tonumber(state, -2);
					if (std::floor(numericKey) != numericKey || numericKey < 1 || numericKey > INT_MAX)
					{
						isArray = false;
					}
					else if (static_cast<int>(numericKey) > maximumIndex)
					{
						maximumIndex = static_cast<int>(numericKey);
					}
				}
				lua_pop(state, 1);
			}

			if (keyCount == 0 || keyCount != maximumIndex)
			{
				isArray = false;
			}
			if (isArray)
			{
				for (int i = 1; i <= maximumIndex; ++i)
				{
					lua_pushnumber(state, static_cast<lua_Number>(i));
					lua_gettable(state, tableIndex);
					if (lua_isnil(state, -1))
					{
						isArray = false;
						lua_pop(state, 1);
						break;
					}
					lua_pop(state, 1);
				}
			}

			if (isArray)
			{
				JsonArray array;
				if (key != NULL && parent.is<JsonObject>())
				{
					array = parent.as<JsonObject>().createNestedArray(key);
				}
				else if (parent.is<JsonArray>())
				{
					array = parent.as<JsonArray>().createNestedArray();
				}
				else
				{
					array = parent.to<JsonArray>();
				}

				if (!array)
				{
					SetJsonNull(parent, key);
					break;
				}

				for (int i = 1; i <= maximumIndex; ++i)
				{
					lua_pushnumber(state, static_cast<lua_Number>(i));
					lua_gettable(state, tableIndex);
					ConvertLuaToJsonValue(state, -1, array, NULL, depth + 1);
					lua_pop(state, 1);
				}
			}
			else
			{
				JsonObject object;
				if (key != NULL && parent.is<JsonObject>())
				{
					object = parent.as<JsonObject>().createNestedObject(key);
				}
				else if (parent.is<JsonArray>())
				{
					object = parent.as<JsonArray>().createNestedObject();
				}
				else
				{
					object = parent.to<JsonObject>();
				}

				if (!object)
				{
					SetJsonNull(parent, key);
					break;
				}

				lua_pushnil(state);
				while (lua_next(state, tableIndex) != 0)
				{
					const int valueIndex = lua_gettop(state);
					const int keyIndex = valueIndex - 1;
					std::string keyText;

					if (lua_type(state, keyIndex) == LUA_TSTRING)
					{
						const char* stringKey = lua_tostring(state, keyIndex);
						keyText = stringKey != NULL ? stringKey : "";
					}
					else if (lua_type(state, keyIndex) == LUA_TNUMBER)
					{
						const double numericKey = lua_tonumber(state, keyIndex);
						std::stringstream keyStream;
						if (std::floor(numericKey) == numericKey && numericKey >= INT_MIN && numericKey <= INT_MAX)
						{
							keyStream << static_cast<int>(numericKey);
						}
						else
						{
							keyStream << std::setprecision(17) << numericKey;
						}
						keyText = keyStream.str();
					}
					else if (lua_type(state, keyIndex) == LUA_TBOOLEAN)
					{
						keyText = lua_toboolean(state, keyIndex) ? "true" : "false";
					}
					else
					{
						keyText = "<";
						keyText += lua_typename(state, lua_type(state, keyIndex));
						keyText += ">";
					}

					if (keyText.size() > 1024)
					{
						keyText.resize(1024);
					}
					JsonVariant child = object[keyText.c_str()];
					ConvertLuaToJsonValue(state, valueIndex, child, NULL, depth + 1);
					lua_pop(state, 1);
				}
			}
			break;
		}

		case LUA_TFUNCTION:
		case LUA_TUSERDATA:
		case LUA_TTHREAD:
		case LUA_TLIGHTUSERDATA:
		{
			std::string description("<");
			description += lua_typename(state, type);
			description += ">";
			SetJsonString(parent, key, description);
			break;
		}

		default:
			SetJsonNull(parent, key);
			break;
		}
	}

	std::string CleanLuaErrorMessage(const char* errorMessage)
	{
		if (errorMessage == NULL)
		{
			return "Unknown Lua error";
		}

		std::string cleaned(errorMessage);
		if (cleaned.size() > kMaximumErrorMessage)
		{
			cleaned.resize(kMaximumErrorMessage);
			cleaned.append(" [...truncated]");
		}

		const size_t lastColon = cleaned.rfind(':');
		if (lastColon != std::string::npos && lastColon > 0)
		{
			const size_t previousColon = cleaned.rfind(':', lastColon - 1);
			if (previousColon != std::string::npos)
			{
				cleaned = cleaned.substr(previousColon + 1);
				while (!cleaned.empty() && cleaned[0] == ' ')
				{
					cleaned.erase(0, 1);
				}
			}
		}
		return cleaned;
	}
}

void CvConnectionService::HandleLuaExecute(const std::string& script, const std::string& id)
{
	if (m_pLuaState == NULL)
	{
		SendProtocolError(id, "LUA_STATE_UNAVAILABLE", "The connection Lua state is unavailable");
		return;
	}

	bool hadGameCoreLock = false;
	if (gDLL != NULL)
	{
		hadGameCoreLock = gDLL->HasGameCoreLock();
		if (hadGameCoreLock)
		{
			gDLL->ReleaseGameCoreLock();
		}
	}

	const int stackBase = lua_gettop(m_pLuaState);
	const int executionResult = luaL_dostring(m_pLuaState, script.c_str());
	ProcessLuaResult(m_pLuaState, executionResult, id, stackBase);

	if (hadGameCoreLock)
	{
		gDLL->GetGameCoreLock();
	}
}

void CvConnectionService::ProcessLuaResult(lua_State* state, int executionResult, const std::string& id, int stackBase)
{
	DynamicJsonDocument response(kJsonDocumentCapacity);
	try
	{
		response["type"] = "lua_response";
		response["id"] = id;

		if (executionResult == 0)
		{
			response["success"] = true;
			int valueCount = lua_gettop(state) - stackBase;
			if (valueCount < 0)
			{
				valueCount = 0;
			}
			ConvertLuaValuesToJson(state, stackBase + 1, valueCount, response);
			if (response.overflowed())
			{
				lua_settop(state, stackBase);
				SendProtocolError(id, "LUA_RESULT_SERIALIZATION_ERROR", "Lua result exceeded the IPC JSON capacity");
				return;
			}
		}
		else
		{
			response["success"] = false;
			const char* rawError = (lua_gettop(state) > stackBase) ? lua_tostring(state, -1) : NULL;
			response["error"]["code"] = "LUA_EXECUTION_ERROR";
			response["error"]["message"] = CleanLuaErrorMessage(rawError);
		}
	}
	catch (...)
	{
		lua_settop(state, stackBase);
		SendProtocolError(id, "LUA_RESULT_SERIALIZATION_ERROR", "Could not serialize the Lua result");
		return;
	}

	// Only values added by this execution belong to this response.  This also
	// removes a Lua error object without disturbing an outer caller's frame.
	lua_settop(state, stackBase);

	try
	{
		std::string serialized;
		serializeJson(response, serialized);
		if (response.overflowed() || serialized.empty())
		{
			SendProtocolError(id, "LUA_RESULT_SERIALIZATION_ERROR", "Could not serialize the Lua result");
			return;
		}
		SendMessage(serialized);
	}
	catch (...)
	{
		SendProtocolError(id, "LUA_RESULT_SERIALIZATION_ERROR", "Could not serialize the Lua result");
	}
}

void CvConnectionService::SendProtocolError(const std::string& id, const char* code, const std::string& message)
{
	DynamicJsonDocument response(16384);
	response["type"] = "lua_response";
	response["id"] = id;
	response["success"] = false;
	response["error"]["code"] = code != NULL ? code : "IPC_ERROR";
	response["error"]["message"] = message;

	try
	{
		std::string serialized;
		serializeJson(response, serialized);
		if (!response.overflowed() && !serialized.empty())
		{
			SendMessage(serialized);
		}
	}
	catch (...)
	{
		Log("ConnectionService::SendProtocolError - serialization failed");
	}
}

DWORD WINAPI CvConnectionService::NamedPipeServerThread(LPVOID lpParam)
{
	CvConnectionService* service = static_cast<CvConnectionService*>(lpParam);
	const std::string pipeName = BuildPipeName();

	while (!service->m_bShutdownRequested)
	{
		HANDLE hPipe = CreateNamedPipeA(
			pipeName.c_str(),
			PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT,
			1,
			kPipeBufferSize,
			kPipeBufferSize,
			0,
			NULL);

		if (hPipe == INVALID_HANDLE_VALUE)
		{
			service->Log("ConnectionService pipe thread - CreateNamedPipe failed");
			service->WaitForShutdown(50);
			continue;
		}

		service->m_hPipe = hPipe;
		bool connected = false;
		while (!service->m_bShutdownRequested)
		{
			BOOL result = ConnectNamedPipe(hPipe, NULL);
			DWORD error = GetLastError();
			if (result || error == ERROR_PIPE_CONNECTED)
			{
				connected = true;
				break;
			}
			if (error != ERROR_PIPE_LISTENING && error != ERROR_NO_DATA)
			{
				service->Log("ConnectionService pipe thread - ConnectNamedPipe failed");
				break;
			}
			service->WaitForShutdown(20);
		}

		if (service->m_bShutdownRequested || !connected)
		{
			ClosePipeHandle(hPipe);
			service->m_hPipe = INVALID_HANDLE_VALUE;
			continue;
		}

		service->m_bClientConnected = true;
		service->HandleClientConnection(hPipe);
		service->m_bClientConnected = false;
		ClosePipeHandle(hPipe);
		service->m_hPipe = INVALID_HANDLE_VALUE;

		if (!service->m_bShutdownRequested)
		{
			service->WaitForShutdown(20);
		}
	}

	service->m_bClientConnected = false;
	return 0;
}

void CvConnectionService::HandleClientConnection(HANDLE hPipe)
{
	std::string incoming;
	incoming.reserve(kPipeBufferSize);
	char readBuffer[kReadBufferSize];

	while (m_bClientConnected && !m_bShutdownRequested)
	{
		std::string outgoing;
		while (DequeueOutgoingMessage(outgoing))
		{
			outgoing.append(kPipeDelimiter);
			size_t offset = 0;
			bool writeSucceeded = true;
			while (offset < outgoing.size())
			{
				const size_t remaining = outgoing.size() - offset;
				const DWORD chunk = static_cast<DWORD>(std::min(kOutgoingChunkSize, remaining));
				DWORD written = 0;
				if (!WriteFile(hPipe, outgoing.data() + offset, chunk, &written, NULL) || written == 0)
				{
					writeSucceeded = false;
					break;
				}
				offset += written;
			}

			if (!writeSucceeded)
			{
				const DWORD error = GetLastError();
				if (error == ERROR_NO_DATA || error == ERROR_PIPE_BUSY)
				{
					outgoing.erase(outgoing.size() - std::strlen(kPipeDelimiter));
					RequeueOutgoingMessage(outgoing);
				}
				return;
			}
		}

		DWORD bytesRead = 0;
		BOOL readSucceeded = ReadFile(hPipe, readBuffer, static_cast<DWORD>(sizeof(readBuffer)), &bytesRead, NULL);
		if (!readSucceeded)
		{
			const DWORD error = GetLastError();
			if (error == ERROR_NO_DATA || error == ERROR_PIPE_LISTENING)
			{
				WaitForShutdown(10);
				continue;
			}
			if (error == ERROR_MORE_DATA)
			{
				// A message-mode read can report a fragment when the bridge writes
				// more than the read buffer.  Keep this fragment and read again.
				if (bytesRead > 0)
				{
					incoming.append(readBuffer, bytesRead);
				}
			}
			else
			{
				return;
			}
		}
		else if (bytesRead > 0)
		{
			incoming.append(readBuffer, bytesRead);
		}

		if (incoming.size() > kMaximumMessageBuffer)
		{
			incoming.clear();
			Log("ConnectionService pipe thread - incomplete message buffer exceeded limit");
			continue;
		}

		size_t frameStart = 0;
		while (true)
		{
			const size_t delimiter = incoming.find(kPipeDelimiter, frameStart);
			if (delimiter == std::string::npos)
			{
				break;
			}

			if (delimiter > frameStart)
			{
				QueueIncomingMessage(incoming.substr(frameStart, delimiter - frameStart));
			}
			frameStart = delimiter + std::strlen(kPipeDelimiter);
		}
		if (frameStart > 0)
		{
			incoming.erase(0, frameStart);
		}
	}
}

bool CvConnectionService::WaitForShutdown(DWORD milliseconds) const
{
	if (m_hShutdownEvent == NULL)
	{
		Sleep(milliseconds);
		return false;
	}
	return WaitForSingleObject(m_hShutdownEvent, milliseconds) == WAIT_OBJECT_0;
}

void CvConnectionService::Log(const char* message)
{
	if (message == NULL)
	{
		return;
	}

	const bool pipeThread = (GetCurrentThreadId() == m_dwThreadId && m_dwThreadId != 0);
	// The pipe thread must not query GC or any other game state.  Main-thread
	// logging still follows the normal VP logging option.
	if (!pipeThread && !GC.getLogging())
	{
		return;
	}

	FILogFile* pLog = pipeThread ? GetPipeLogFile() : GetLogFile();
	if (pLog == NULL)
	{
		return;
	}

	std::string bounded(message);
	if (bounded.size() > kMaximumLogMessage)
	{
		bounded.resize(kMaximumLogMessage);
		bounded.append(" [...truncated]");
	}

	std::stringstream line;
	SYSTEMTIME time;
	GetLocalTime(&time);
	line << "[" << std::setfill('0') << std::setw(2) << time.wHour << ":"
		 << std::setfill('0') << std::setw(2) << time.wMinute << ":"
		 << std::setfill('0') << std::setw(2) << time.wSecond << "."
		 << std::setfill('0') << std::setw(3) << time.wMilliseconds << "] "
		 << bounded;
	SafeLogMessage(pLog, line.str().c_str());
}

FILogFile* CvConnectionService::GetLogFile()
{
	return LOGFILEMGR.GetLog("connection.log", FILogFile::kDontTimeStamp);
}

FILogFile* CvConnectionService::GetPipeLogFile()
{
	return LOGFILEMGR.GetLog("connection-pipe.log", FILogFile::kDontTimeStamp);
}

void CvConnectionService::SafeLogMessage(FILogFile* pLog, const char* message)
{
	if (pLog == NULL || message == NULL)
	{
		return;
	}

#if defined(_MSC_VER)
	__try
	{
		pLog->Msg("%s", message);
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		// Logging is diagnostic; never let a malformed/closed log sink take the
		// game or pipe thread down with it.
	}
#else
	pLog->Msg("%s", message);
#endif
}
