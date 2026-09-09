/*	-------------------------------------------------------------------------------------------------------
	Connection service for the optional Vox Deorum bridge channel.
	------------------------------------------------------------------------------------------------------- */
#pragma once

#ifndef CIV5_CONNECTION_SERVICE_H
#define CIV5_CONNECTION_SERVICE_H

#include <windows.h>
#include <deque>
#include <string>

class FILogFile;
class ICvEngineScriptSystem1;
struct lua_State;

// A deliberately small bridge service.  The pipe thread owns transport only;
// all Lua and game work is performed by ProcessMessages on the game thread.
class CvConnectionService
{
public:
	static CvConnectionService& GetInstance();

	bool Setup();
	void Shutdown();
	void ProcessMessages();

	// Queue a serialized JSON response for the pipe thread.
	int SendMessage(const std::string& message);

private:
	CvConnectionService();
	~CvConnectionService();

	CvConnectionService(const CvConnectionService&);
	CvConnectionService& operator=(const CvConnectionService&);

	static DWORD WINAPI NamedPipeServerThread(LPVOID lpParam);
	void HandleClientConnection(HANDLE hPipe);

	void QueueIncomingMessage(const std::string& message);
	void QueueOutgoingMessage(const std::string& message);
	void RequeueOutgoingMessage(const std::string& message);
	bool DequeueOutgoingMessage(std::string& message);

	// Replaced by the protocol router in ipc-lua-protocol.  Keeping routing
	// behind this game-thread boundary makes the transport commit independently
	// buildable and keeps the pipe thread free of game work.
	bool RouteMessage(const std::string& message);

	void Log(const char* message);
	FILogFile* GetLogFile();
	FILogFile* GetPipeLogFile();
	static void SafeLogMessage(FILogFile* pLog, const char* message);

	bool WaitForShutdown(DWORD milliseconds) const;

	volatile bool m_bInitialized;
	volatile bool m_bShutdownRequested;
	volatile bool m_bClientConnected;
	bool m_bProcessingMessages;

	lua_State* m_pLuaState;
	ICvEngineScriptSystem1* m_pScriptSystem;

	HANDLE m_hThread;
	HANDLE m_hShutdownEvent;
	HANDLE m_hPipe;
	DWORD m_dwThreadId;

	std::deque<std::string> m_incomingQueue;
	std::deque<std::string> m_outgoingQueue;
	CRITICAL_SECTION m_csIncoming;
	CRITICAL_SECTION m_csOutgoing;
	CRITICAL_SECTION m_csProcessing;
};

#endif // CIV5_CONNECTION_SERVICE_H
