#ifndef __BONDRIVER_PROXY_H__
#define __BONDRIVER_PROXY_H__
#include <windows.h>
#include <tchar.h>
#include <process.h>
#include <list>
#include <queue>
#include "IBonDriver3.h"

#if _DEBUG
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#define new new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define DETAILLOG	0
#endif

#define TUNER_NAME	"BonDriverProxy"
#define WAIT_TIME	10	// GetTsStream()�̌�ŁAdwRemain��0�������ꍇ�ɑ҂���(ms)

////////////////////////////////////////////////////////////////////////////////

static char g_Host[64];
static unsigned short g_Port;
static char g_BonDriver[MAX_PATH];
static BOOL g_ChannelLock;
static size_t g_PacketFifoSize;
static size_t g_TsFifoSize;
static DWORD g_TsPacketBufSize;
static int g_ConnectTimeOut;
static BOOL g_UseMagicPacket;
static char g_TargetMac[6];
static char g_TargetHost[64];
static unsigned short g_TargetPort;

int Init(HMODULE hModule)
{
	char szIniPath[MAX_PATH + 16] = { '\0' };
	GetModuleFileNameA(hModule, szIniPath, MAX_PATH);
	char *p = strrchr(szIniPath, '.');
	if (!p)
		return -1;
	p++;
	strcpy(p, "ini");

	GetPrivateProfileStringA("OPTION", "ADDRESS", "127.0.0.1", g_Host, sizeof(g_Host), szIniPath);
	g_Port = (unsigned short)GetPrivateProfileIntA("OPTION", "PORT", 1192, szIniPath);
	GetPrivateProfileStringA("OPTION", "BONDRIVER", "BonDriver_ptmr.dll", g_BonDriver, sizeof(g_BonDriver), szIniPath);
	g_ChannelLock = (BOOL)GetPrivateProfileIntA("OPTION", "CHANNEL_LOCK", 0, szIniPath);

	g_ConnectTimeOut = GetPrivateProfileIntA("OPTION", "CONNECT_TIMEOUT", 5, szIniPath);
	g_UseMagicPacket = (BOOL)GetPrivateProfileIntA("OPTION", "USE_MAGICPACKET", 0, szIniPath);
	if (g_UseMagicPacket)
	{
		char mac[32];
		GetPrivateProfileStringA("MAGICPACKET", "TARGET_ADDRESS", g_Host, g_TargetHost, sizeof(g_TargetHost), szIniPath);
		g_TargetPort = (unsigned short)GetPrivateProfileIntA("MAGICPACKET", "TARGET_PORT", g_Port, szIniPath);
		memset(mac, 0, sizeof(mac));
		GetPrivateProfileStringA("MAGICPACKET", "TARGET_MACADDRESS", "", mac, sizeof(mac), szIniPath);
		for (int i = 0; i < 6; i++)
		{
			BYTE b = 0;
			p = &mac[i * 3];
			for (int j = 0; j < 2; j++)
			{
				if ('0' <= *p && *p <= '9')
					b = b * 0x10 + (*p - '0');
				else if ('A' <= *p && *p <= 'F')
					b = b * 0x10 + (*p - 'A' + 10);
				else if ('a' <= *p && *p <= 'f')
					b = b * 0x10 + (*p - 'a' + 10);
				else
					return -2;
				p++;
			}
			g_TargetMac[i] = b;
		}
	}

	g_PacketFifoSize = GetPrivateProfileIntA("SYSTEM", "PACKET_FIFO_SIZE", 16, szIniPath);
	g_TsFifoSize = GetPrivateProfileIntA("SYSTEM", "TS_FIFO_SIZE", 32, szIniPath);
	g_TsPacketBufSize = GetPrivateProfileIntA("SYSTEM", "TSPACKET_BUFSIZE", (188 * 1024), szIniPath);

	return 0;
}

////////////////////////////////////////////////////////////////////////////////

class cCriticalSection {
	CRITICAL_SECTION m_c;
public:
	cCriticalSection(){ ::InitializeCriticalSection(&m_c); }
	~cCriticalSection(){ ::DeleteCriticalSection(&m_c); }
	void Enter(){ ::EnterCriticalSection(&m_c); }
	void Leave(){ ::LeaveCriticalSection(&m_c); }
};

class cLock {
	cCriticalSection &m_c;
public:
	cLock(cCriticalSection &ref) : m_c(ref) { m_c.Enter(); }
	~cLock(){ m_c.Leave(); }
};

#define LOCK(key) cLock __Lock__(key)

class cEvent {
	HANDLE m_h;
	DWORD m_dwWait;
public:
	cEvent(BOOL bManualReset = FALSE, BOOL bInitialState = FALSE, DWORD dwMilliseconds = INFINITE)
	{
		m_dwWait = dwMilliseconds;
		m_h = ::CreateEvent(NULL, bManualReset, bInitialState, NULL);
	}
	~cEvent(){ ::CloseHandle(m_h); }
	BOOL IsSet(){ return (::WaitForSingleObject(m_h, 0) == WAIT_OBJECT_0); }
	DWORD Wait(){ return ::WaitForSingleObject(m_h, m_dwWait); }
	BOOL Set(){ return ::SetEvent(m_h); }
	BOOL Reset(){ return ::ResetEvent(m_h); }
	operator HANDLE () const { return m_h; }
};

////////////////////////////////////////////////////////////////////////////////

enum enumCommand {
	eSelectBonDriver = 0,
	eCreateBonDriver,
	eOpenTuner,
	eCloseTuner,
	eSetChannel1,
	eGetSignalLevel,
	eWaitTsStream,
	eGetReadyCount,
	eGetTsStream,
	ePurgeTsStream,
	eRelease,

	eGetTunerName,
	eIsTunerOpening,
	eEnumTuningSpace,
	eEnumChannelName,
	eSetChannel2,
	eGetCurSpace,
	eGetCurChannel,

	eGetTotalDeviceNum,
	eGetActiveDeviceNum,
	eSetLnbPower,
};

__declspec(align(1)) struct stPacketHead {
	BYTE m_bSync;
	BYTE m_bCommand;
	BYTE m_bReserved1;
	BYTE m_bReserved2;
	DWORD m_dwBodyLength;
};

__declspec(align(1)) struct stPacket {
	stPacketHead head;
	BYTE payload[1];
};

#define SYNC_BYTE	0xff
class cPacketHolder {
	friend class cProxyServer;
	friend class cProxyClient;
	union {
		stPacket *m_pPacket;
		BYTE *m_pBuff;
	};
	size_t m_Size;

	inline void init(size_t PayloadSize)
	{
		m_pBuff = new BYTE[sizeof(stPacketHead) + PayloadSize];
		::memset(m_pBuff, 0, sizeof(stPacketHead));
		m_pPacket->head.m_bSync = SYNC_BYTE;
		m_pPacket->head.m_dwBodyLength = ::htonl((DWORD)PayloadSize);
		m_Size = sizeof(stPacketHead) + PayloadSize;
	}

public:
	cPacketHolder(size_t PayloadSize)
	{
		init(PayloadSize);
	}

	cPacketHolder(enumCommand eCmd, size_t PayloadSize)
	{
		init(PayloadSize);
		SetCommand(eCmd);
	}

	~cPacketHolder()
	{
		if (m_pBuff)
		{
			delete[] m_pBuff;
			m_pBuff = NULL;
		}
	}
	inline BOOL IsValid(){ return (m_pPacket->head.m_bSync == SYNC_BYTE); }
	inline BOOL IsTS(){ return (m_pPacket->head.m_bCommand == (BYTE)eGetTsStream); }
	inline enumCommand GetCommand(){ return (enumCommand)m_pPacket->head.m_bCommand; }
	inline void SetCommand(enumCommand eCmd){ m_pPacket->head.m_bCommand = (BYTE)eCmd; }
	inline DWORD GetBodyLength(){ return ::ntohl(m_pPacket->head.m_dwBodyLength); }
};

class cPacketFifo : protected std::queue<cPacketHolder *> {
	const size_t m_fifoSize;
	cCriticalSection m_Lock;
	cEvent m_Event;

public:
	cPacketFifo() : m_fifoSize(g_PacketFifoSize), m_Event(TRUE, FALSE){}
	~cPacketFifo()
	{
		LOCK(m_Lock);
		while (!empty())
		{
			cPacketHolder *p = front();
			pop();
			delete p;
		}
	}

	void Push(cPacketHolder *p)
	{
		LOCK(m_Lock);
		if (size() >= m_fifoSize)
		{
#if _DEBUG
			_RPT1(_CRT_WARN, "Packet Queue OVERFLOW : size[%d]\n", size());
#endif
			// TS�̏ꍇ�̂݃h���b�v
			if (p->IsTS())
			{
				delete p;
				return;
			}
		}
		push(p);
		m_Event.Set();
	}

	void Pop(cPacketHolder **p)
	{
		LOCK(m_Lock);
		if (!empty())
		{
			*p = front();
			pop();
			if (empty())
				m_Event.Reset();
		}
		else
			m_Event.Reset();
	}

	HANDLE GetEventHandle()
	{
		return (HANDLE)m_Event;
	}

#if _DEBUG
	inline size_t Size()
	{
		return size();
	}
#endif
};

////////////////////////////////////////////////////////////////////////////////

class cProxyServer {
	IBonDriver *m_pIBon;
	IBonDriver2 *m_pIBon2;
	IBonDriver3 *m_pIBon3;
	HMODULE m_hModule;
	SOCKET m_s;
	cEvent m_Error;
	char m_strBonDriver[MAX_PATH];
	BOOL m_bTunerOpen;
	HANDLE m_hTsRead;
	BOOL * volatile m_pStopTsRead;
	cCriticalSection *m_pTsLock;
	DWORD *m_ppos;
	BOOL m_bChannelLock;
#if _DEBUG
public:
#endif
	cPacketFifo m_fifoSend;
	cPacketFifo m_fifoRecv;

#if _DEBUG
private:
#endif
	DWORD Process();
	int ReceiverHelper(char *pDst, int left);
	static DWORD WINAPI Receiver(LPVOID pv);
	void makePacket(enumCommand eCmd, BOOL b);
	void makePacket(enumCommand eCmd, DWORD dw);
	void makePacket(enumCommand eCmd, LPCTSTR str);
	void makePacket(enumCommand eCmd, BYTE *pSrc, DWORD dwSize, float fSignalLevel);
	static DWORD WINAPI Sender(LPVOID pv);
	static DWORD WINAPI TsReader(LPVOID pv);

	BOOL SelectBonDriver(LPCSTR p);
	IBonDriver *CreateBonDriver();

	// IBonDriver
	const BOOL OpenTuner(void);
	void CloseTuner(void);
	void PurgeTsStream(void);

	// IBonDriver2
	LPCTSTR EnumTuningSpace(const DWORD dwSpace);
	LPCTSTR EnumChannelName(const DWORD dwSpace, const DWORD dwChannel);
	const BOOL SetChannel(const DWORD dwSpace, const DWORD dwChannel);

	// IBonDriver3
	const DWORD GetTotalDeviceNum(void);
	const DWORD GetActiveDeviceNum(void);
	const BOOL SetLnbPower(const BOOL bEnable);

public:
	cProxyServer();
	~cProxyServer();
	void setSocket(SOCKET s){ m_s = s; }
	static DWORD WINAPI Reception(LPVOID pv);
};

////////////////////////////////////////////////////////////////////////////////

struct TS_DATA {
	BYTE *pbBuff;
	DWORD dwSize;
	TS_DATA(void)
	{
		pbBuff = NULL;
		dwSize = 0;
	}
	~TS_DATA(void)
	{
		if (pbBuff)
			delete[] pbBuff;
	}
};

class cTSFifo : protected std::queue<TS_DATA *> {
	const size_t m_fifoSize;
	cCriticalSection m_Lock;
	cEvent m_Event;

public:
	cTSFifo() : m_fifoSize(g_TsFifoSize), m_Event(TRUE, FALSE){}
	~cTSFifo(){ Flush(); }

	void Flush()
	{
		LOCK(m_Lock);
		while (!empty())
		{
			TS_DATA *p = front();
			pop();
			delete p;
		}
	}

	void Push(TS_DATA *p)
	{
		LOCK(m_Lock);
		if (size() >= m_fifoSize)
		{
#if _DEBUG
			_RPT1(_CRT_WARN, "TS Queue OVERFLOW : size[%d]\n", size());
#endif
			TS_DATA *pDel = front();
			pop();
			delete pDel;
		}
		push(p);
		m_Event.Set();
	}

	void Pop(TS_DATA **p)
	{
		LOCK(m_Lock);
		if (!empty())
		{
			*p = front();
			pop();
			if (empty())
				m_Event.Reset();
		}
		else
			m_Event.Reset();
	}

	HANDLE GetEventHandle()
	{
		return (HANDLE)m_Event;
	}

	inline size_t Size()
	{
		return size();
	}
};

////////////////////////////////////////////////////////////////////////////////

class cProxyClient : public IBonDriver3 {
	SOCKET m_s;
//	HANDLE m_hThread;
	volatile int m_iEndCount;
	cEvent m_Error;
	cEvent m_SingleShot;
	cPacketFifo m_fifoSend;
	cPacketFifo m_fifoRecv;
	cTSFifo m_fifoTS;
	TS_DATA *m_LastBuff;
	BOOL m_bRes;
	DWORD m_dwRes;
	DWORD m_dwBufPos;
	TCHAR *m_pBuf[8];
	TCHAR *m_pRes;
	float m_fSignalLevel;
	DWORD m_dwSpace;
	DWORD m_dwChannel;
	BOOL m_bBonDriver;
	BOOL m_bTuner;
	BOOL m_bRereased;
	cCriticalSection m_writeLock;
	cCriticalSection m_readLock;	// �ꉞ���b�N���Ă邯�ǁA�����ɂ͖{�����߂Ă郍�b�N�͕ۏ؂ł��ĂȂ�

	DWORD Process();
	int ReceiverHelper(char *pDst, int left);
	static DWORD WINAPI Receiver(LPVOID pv);
	void makePacket(enumCommand eCmd);
	void makePacket(enumCommand eCmd, LPCSTR);
	void makePacket(enumCommand eCmd, BOOL b);
	void makePacket(enumCommand eCmd, DWORD dw);
	void makePacket(enumCommand eCmd, DWORD dw1, DWORD dw2);
	void makePacket(enumCommand eCmd, DWORD dw1, DWORD dw2, BOOL b);
	static DWORD WINAPI Sender(LPVOID pv);
	void TsFlush(){ m_fifoTS.Flush(); }
	void SleepLock(int n){ while (m_iEndCount != n){ ::Sleep(1); }; }

public:
	cProxyClient();
	~cProxyClient();
	void setSocket(SOCKET s){ m_s = s; }
//	void setThreadHandle(HANDLE h){ m_hThread = h; }
	BOOL IsError(){ return m_Error.IsSet(); }
	void WaitSingleShot(){ m_SingleShot.Wait(); }
	static DWORD WINAPI ProcessEntry(LPVOID pv);

	BOOL SelectBonDriver();
	BOOL CreateBonDriver();

	// IBonDriver
	const BOOL OpenTuner(void);
	void CloseTuner(void);
	const BOOL SetChannel(const BYTE bCh);
	const float GetSignalLevel(void);
	const DWORD WaitTsStream(const DWORD dwTimeOut = 0);
	const DWORD GetReadyCount(void);
	const BOOL GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain);
	const BOOL GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain);
	void PurgeTsStream(void);
	void Release(void);

	// IBonDriver2
	LPCTSTR GetTunerName(void);
	const BOOL IsTunerOpening(void);
	LPCTSTR EnumTuningSpace(const DWORD dwSpace);
	LPCTSTR EnumChannelName(const DWORD dwSpace, const DWORD dwChannel);
	const BOOL SetChannel(const DWORD dwSpace, const DWORD dwChannel);
	const DWORD GetCurSpace(void);
	const DWORD GetCurChannel(void);

	// IBonDriver3
	const DWORD GetTotalDeviceNum(void);
	const DWORD GetActiveDeviceNum(void);
	const BOOL SetLnbPower(const BOOL bEnable);
};
#endif	// __BONDRIVER_PROXY_H__