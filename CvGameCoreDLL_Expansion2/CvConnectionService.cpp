/*	-------------------------------------------------------------------------------------------------------
	Optional Windows named-pipe transport for the Vox Deorum bridge.

	The pipe thread only reads and writes serialized strings.  It never calls
	Lua and never accesses the game model.  Requests are handed to the game
	thread through m_incomingQueue and responses are returned through
	m_outgoingQueue.
	------------------------------------------------------------------------------------------------------- */

#include "CvGameCoreDLLPCH.h"
#include "CvConnectionService.h"

#include "FireWorks/FILogFile.h"
#include "ICvDLLScriptSystem.h"
#include "Lua/CvLuaSupport.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace
{
	const char* const kPipeDelimiter = "!@#$%^!";
	const char* const kDefaultPipeId = "vox-deorum-bridge";
	const DWORD kPipeBufferSize = 262144;
	const size_t kReadBufferSize = 65536;
	const size_t kOutgoingChunkSize = 32768;
	const size_t kMaximumMessageBuffer = 8 * 1024 * 1024;
	const size_t kMaximumLogMessage = 3800;

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
	// The protocol router is added in ipc-lua-protocol.  This boundary is
	// intentionally on the game thread even in the transport-only commit.
	UNUSED_VARIABLE(message);
	return true;
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
