// Needs to be first otherwise compile errors, c++ is so annoying
#include "date.h"

#include "../MQ2Plugin.h"

#include <fstream>
#include <ostream>
#include <regex>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <Psapi.h>

PreSetup("MQ2Profiler");

// 55 8B EC 81 EC ? ? 00 00 53 C7 45 ? 00 00 00 00
const unsigned char * DoNextCommandPattern = (const unsigned char *)"\x55\x8B\xEC\x81\xEC\x00\x00\x00\x00\x53\xC7\x45\x00\x00\x00\x00\x00";
const char * DoNextCommandPatternMask = "xxxxx??xxxxx?xxxx";

DETOUR_TRAMPOLINE_EMPTY(VOID __cdecl Call_T(PSPAWNINFO pChar, PCHAR szLine));
DETOUR_TRAMPOLINE_EMPTY(VOID __cdecl Return_T(PSPAWNINFO pChar, PCHAR szLine));
DETOUR_TRAMPOLINE_EMPTY(VOID __cdecl EndMacro_T(PSPAWNINFO pChar, PCHAR szLine));
DETOUR_TRAMPOLINE_EMPTY(VOID __cdecl DoEvents_T(PSPAWNINFO pChar, PCHAR szLine));
DETOUR_TRAMPOLINE_EMPTY(BOOL __cdecl DoNextCommand_T(PMACROBLOCK pMacroBlock) noexcept);

VOID __cdecl Call_D(PSPAWNINFO pChar, PCHAR szLine);
VOID __cdecl Return_D(PSPAWNINFO pChar, PCHAR szLine);
VOID __cdecl EndMacro_D(PSPAWNINFO pChar, PCHAR szLine);
VOID __cdecl DoEvents_D(PSPAWNINFO pChar, PCHAR szLine);
BOOL __cdecl DoNextCommand_D(PMACROBLOCK pMacroBlock);

DWORD callAddr;
DWORD returnAddr;
DWORD endMacroAddr;
DWORD doEventsAddr;
DWORD doNextCommandAddr;

VOID __cdecl ProfileCommand(PSPAWNINFO pChar, PCHAR szLine);

unsigned __int64 commandCount = 0;

void LogError(const std::string & error);
LONGLONG GetMicroseconds(LARGE_INTEGER start, LARGE_INTEGER end);
std::string ArgsToCSV(const std::vector<std::string> & args);

class StackFrame
{
public:
	StackFrame(const std::string & subroutine, const std::vector<std::string> & args)
		: m_subroutine(subroutine),
		m_args(args),
		m_returned(false)
	{
		QueryPerformanceCounter(&m_startPerfCount);
		m_startCommandCount = commandCount;
	}

	void AddChild(const StackFrame & child)
	{
		m_children.push_back(child);
	}

	void End(const std::string & returnValue)
	{
		QueryPerformanceCounter(&m_endPerfCount);
		m_endCommandCount = commandCount;
		m_returnValue = returnValue;
		m_returned = true;
	}

	float getMillisecondsInc() const
	{
		return GetMicroseconds(m_startPerfCount, m_endPerfCount) / 1000.0f;
	}

	float getMillisecondsEx() const
	{
		auto total = getMillisecondsInc();
		for each (auto child in m_children)
			total -= child.getMillisecondsInc();

		return total;
	}

	unsigned __int64 getCommandsInc() const
	{
		return m_endCommandCount - m_startCommandCount;
	}

	unsigned __int64 getCommandsEx() const
	{
		auto total = getCommandsInc();
		for each (auto child in m_children)
			total -= child.getCommandsInc();

		return total;
	}

	const std::string ToCsv(int depth, LARGE_INTEGER startPerfCount) const
	{
		// os << "Command Count,Seconds Since Start,Stack Depth,Subroutine,Subroutine (tabbed),Commands (inc Children),Commands (ex Children),ms inc, ms ex,Called Children,Return Value,Arguments"

		std::stringstream ss;

		ss << m_startCommandCount << ","
			<< GetMicroseconds(startPerfCount, m_startPerfCount) / 1000000.0 << ","
			<< depth << ","
			<< m_subroutine << ",";

		for (int i = 1; i < depth; i++)
			ss << "  ";

		ss << m_subroutine << ","
			<< getCommandsInc() << ","
			<< getCommandsEx() << ","
			<< getMillisecondsInc() << ","
			<< getMillisecondsEx() << ","
			<< m_children.size() << ","
			<< "\"" << m_returnValue << "\","
			<< ArgsToCSV(m_args)
			<< std::endl;

		for each (auto child in m_children)
			ss << child.ToCsv(depth + 1, startPerfCount);

		return ss.str();
	}

private:
	std::string m_subroutine;
	std::vector<std::string> m_args;
	std::string m_returnValue;
	bool m_returned;
	unsigned __int64 m_startCommandCount;
	unsigned __int64 m_endCommandCount;
	LARGE_INTEGER m_startPerfCount;
	LARGE_INTEGER m_endPerfCount;
	std::vector<StackFrame> m_children;
};

class Profile
{
public:
	Profile(const std::string & name)
		: m_name(name)
	{
		QueryPerformanceCounter(&m_startPerfCount);
		m_startCommandCount = commandCount;
	}

	void Call(const std::string & subroutine, const std::vector<std::string> & args)
	{
		// Create a new stackframe, push it on the top of the stack
		m_callStack.push(StackFrame(subroutine, args));
	}

	void Return(const std::string & returnValue)
	{
		if (m_callStack.size() == 0)
		{
			return;
		}

		// End the current stack frame, and add it to the parent's children
		auto top = m_callStack.top();
		top.End(returnValue);
		if (m_callStack.size() > 1)
		{
			m_callStack.pop();
			m_callStack.top().AddChild(top);
		}
	}

	const std::string & GetName() { return m_name; }

	void End()
	{
		while (m_callStack.size() > 1)
			Return("#N/A");

		m_callStack.top().End("#N/A");
	}

	friend std::ostream & operator<<(std::ostream & os, const Profile & profile)
	{
		os << "Command Count,Seconds Since Start,Stack Depth,Subroutine,Subroutine (tabbed),Commands (inc Children),Commands (ex Children),ms inc, ms ex,Called Children,Return Value,Arguments" << std::endl;

		if (profile.m_callStack.size() == 1)
			os << profile.m_callStack.top().ToCsv(1, profile.m_startPerfCount);
		
		return os;
	}

private:
	std::string m_name;
	std::stack<StackFrame> m_callStack;
	unsigned __int64 m_startCommandCount;
	LARGE_INTEGER m_startPerfCount;
};

std::unique_ptr<Profile> g_pProfile;

DWORD FixJump(DWORD stubAddr)
{
	if (*(PBYTE)stubAddr == 0xE9)
		return stubAddr + 5 + (*(int*)(stubAddr + 1));
	return stubAddr;
}

PLUGIN_API VOID InitializePlugin()
{
	HMODULE hMQ2Main = GetModuleHandleA("MQ2Main.dll");
	callAddr = FixJump((DWORD)GetProcAddress(hMQ2Main, "Call"));
	returnAddr = FixJump((DWORD)GetProcAddress(hMQ2Main, "Return"));
	endMacroAddr = FixJump((DWORD)GetProcAddress(hMQ2Main, "EndMacro"));
	doEventsAddr = FixJump((DWORD)GetProcAddress(hMQ2Main, "DoEvents"));

	MODULEINFO MQ2ModuleInfo = { 0 };
	GetModuleInformation(GetCurrentProcess(), hMQ2Main, &MQ2ModuleInfo, sizeof(MODULEINFO));

	doNextCommandAddr = FindPattern((uintptr_t)MQ2ModuleInfo.lpBaseOfDll, MQ2ModuleInfo.SizeOfImage, DoNextCommandPattern, DoNextCommandPatternMask);

	EzDetour(callAddr, Call_D, Call_T);
	EzDetour(returnAddr, Return_D, Return_T);
	EzDetour(endMacroAddr, EndMacro_D, EndMacro_T);
	EzDetour(doEventsAddr, DoEvents_D, DoEvents_T);
	if (doNextCommandAddr)
		EzDetour(doNextCommandAddr, DoNextCommand_D, DoNextCommand_T);

	AddCommand("/profile", ProfileCommand);
}

PLUGIN_API VOID ShutdownPlugin()
{
	RemoveDetour(callAddr);
	RemoveDetour(returnAddr);
	RemoveDetour(endMacroAddr);
	RemoveDetour(doEventsAddr);
	if (doNextCommandAddr)
		RemoveDetour(doNextCommandAddr);

	RemoveCommand("/profile");
}

void ArgsToVector(PCHAR szLine, std::vector<std::string> & vec)
{
	auto i = 1;
	while (true)
	{
		char szArg[2048] = { 0 };
		GetArg(szArg, szLine, i++);

		if (!*szArg)
			break;

		vec.push_back(szArg);
	}
}

VOID __cdecl Call_D(PSPAWNINFO pChar, PCHAR szLine)
{
	Call_T(pChar, szLine);

	if (!g_pProfile)
		return;

	if (!gMacroBlock)
	{
		LogError("Something went wrong in call");
		return;
	}

	std::vector<std::string> args;
	ArgsToVector(szLine, args);
	auto subroutine = args.front();
	args.erase(args.begin()); // meh
	
	g_pProfile->Call(subroutine, args);
}

VOID __cdecl Return_D(PSPAWNINFO pChar, PCHAR szLine)
{
	Return_T(pChar, szLine);

	if (!g_pProfile)
		return;

	if (!gMacroBlock)
	{
		LogError("Something went wrong in return");
		return;
	}

	g_pProfile->Return(szLine);
}

VOID __cdecl ProfileCommand(PSPAWNINFO pChar, PCHAR szLine)
{
	Macro(pChar, szLine);

	if (g_pProfile)
	{
		LogError("Macro started when a profile already exists");
		return;
	}

	if (!gMacroBlock)
		return;

	std::vector<std::string> args;
	ArgsToVector(szLine, args);
	args.erase(args.begin()); // first arg is macro name, remove it

	auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	char dateTime[32] = { 0 };
	//tm today = {0};
	//time_t tm;
	//tm = time(NULL);
	//localtime_s(&today,&tm);
	std::strftime(dateTime, 32, "%Y%m%d_%H%M%S", std::localtime(&time));

	commandCount = 0;

	// strip directories if any
	char * pMacroName = strrchr(gszMacroName, '\\') + 1;
	if (pMacroName == (char *)1) pMacroName = strrchr(gszMacroName, '//') + 1;
	if (pMacroName == (char *)1) pMacroName = gszMacroName;

	WriteChatf("\aw[MQ2Profiler]\ax Starting profile");
	g_pProfile = std::make_unique<Profile>(Profile(std::string(pMacroName) + "_" + dateTime));
	g_pProfile->Call("Main", args);
}

VOID __cdecl EndMacro_D(PSPAWNINFO pChar, PCHAR szLine)
{
	EndMacro_T(pChar, szLine);

	if (!g_pProfile)
		return;

	g_pProfile->End();

	auto profileDirectoryPath = std::string(gszMacroPath) + "\\profiles\\";
	if (!std::filesystem::exists(profileDirectoryPath))
		std::filesystem::create_directory(profileDirectoryPath);

	std::ofstream logFile(profileDirectoryPath + g_pProfile->GetName() + ".csv");
	logFile << *g_pProfile;
	WriteChatf("\aw[MQ2Profiler]\ax Saved profile to %s", g_pProfile->GetName().c_str());
	g_pProfile.reset();
}

VOID __cdecl DoEvents_D(PSPAWNINFO pChar, PCHAR szLine)
{
	// DoEvents changes the instruction pointer and puts stuff on the stack, but doesn't call Call

	auto beforeStack = gMacroStack;
	DoEvents_T(pChar, szLine);

	if (!g_pProfile)
		return;

	if (beforeStack != gMacroStack)
	{
		if (!gMacroBlock)
		{
			LogError("Something went wrong in bind");
			return;
		}
        
		// My compile of MQ2Main gives different struct layouts for MACROBLOCK to whatever VV is compiled with so this crashes

		auto line = gMacroBlock->Line.find(gMacroBlock->CurrIndex);
		if (line != gMacroBlock->Line.end())
		{
			std::smatch matches;

			if (!std::regex_search(line->second.Command, matches, std::regex("Sub ([^\\(]+)")))
			{
				LogError("Couldn't match bind sub");
				return;
			}

			std::vector<std::string> args;

			auto pParam = gMacroStack->Parameters;
			while (pParam)
			{
				char szArg[MAX_STRING] = { 0 };

				if (pParam->Var.Type->ToString(pParam->Var.VarPtr, szArg))
					args.push_back(szArg);
				else
					args.push_back("NULL");

				pParam = pParam->pNext;
			}

			g_pProfile->Call(matches[1].str().c_str(), args);
		}
		else
		{
			LogError("Something went wrong in bind");
		}
	}
}

BOOL __cdecl DoNextCommand_D(PMACROBLOCK pMacroBlock)
{
	if (DoNextCommand_T(pMacroBlock))
	{
		commandCount++;
		return TRUE;
	}

	return FALSE;
}

LONGLONG GetMicroseconds(LARGE_INTEGER start, LARGE_INTEGER end)
{
	LARGE_INTEGER elapsed, frequency;
	elapsed.QuadPart = end.QuadPart - start.QuadPart;
	QueryPerformanceFrequency(&frequency);
	elapsed.QuadPart *= 1000000;
	elapsed.QuadPart /= frequency.QuadPart;
	return elapsed.QuadPart;
}

std::string ArgsToCSV(const std::vector<std::string> & args)
{
	std::string ret;

	if (args.size() == 0)
		return ret;

	// no string replace in STL ???
	ret = "\"" + std::regex_replace(args[0], std::regex("\""), "\"\"") + "\"";

	for (int i = 1; i < (int)args.size(); i++)
		ret += ",\"" + std::regex_replace(args[i], std::regex("\""), "\"\"") + "\"";

	return ret;
}

void LogError(const std::string & error)
{
	WriteChatf("\aw[MQ2Profiler] \arError: %s", error.c_str());
}

