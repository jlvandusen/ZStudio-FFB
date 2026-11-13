/*This file is part of ZStudio-FFB, The Real Z Studios' fork of FFB Arcade Plugin.
ZStudio-FFB is free software : you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
ZStudio-FFB is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
GNU General Public License for more details.
You should have received a copy of the GNU General Public License
along with ZStudio-FFB.If not, see < https://www.gnu.org/licenses/>.
*/

// ZStudioFFBHost: a standalone process that turns a game's published feedback events (the ZStudio Output Protocol,
// v1 = NDJSON lines, ZSTUDIO.md) into force feedback on wheels, FFB joysticks and pads through SDL3.
//
// Roadmap steps 1 and 2 of ZSTUDIO.md. It is its own process ON PURPOSE: this code is GPL-3 and the games are not,
// so a game only writes a file and never links any of this. No dinput8 proxy, no memory reading.
//
//   ZStudioFFBHost.exe                 run: attach to the first stream in ZStudioFFB.ini, drive the chosen device
//   ZStudioFFBHost.exe --list          list haptic devices and gamepads, with what each can do
//   ZStudioFFBHost.exe --test          play a left jolt, a right jolt, a sine roll and a rumble on the device
//   ZStudioFFBHost.exe --dry-run       tail and print what each event WOULD play, without opening a device
//   ZStudioFFBHost.exe --selftest      parser + mapping + board-frame checks, no device, no file; exit 1 on a failure
//   ZStudioFFBHost.exe --board-test    light and shake every prop on the board ([Board] Port=): ember, cyan, a fade, gears
//   --board <COMn>                     the board's bridge ESP32 on this port (overrides [Board] Port=)
//
// The BOARD output (board.h): a physical game board's ESP32 props (WS2812 lights, PWM motor drivers) behind a bridge
// ESP32 on USB serial. A game line that carries board fields lights and shakes them; [Board] ForceToMotor=1 also sends
// any game's wheel force to the props' motors. It runs alongside the SDL device, or alone when there is none.
//   --ini <path>  --stream <path>      override the INI and the stream path
//
// Tuning follows upstream FFB Arcade Plugin: MinForce / MaxForce in percent, PowerMode (a square-root curve that lifts
// the low end), Reverse, and "a weaker effect does not cut a running one short unless it is 1.5x stronger".

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------------------------------------------- config

enum class FxType { Jolt, Sine, Rumble, Off };

struct IdRule
{
	bool bHasForce = false;
	double Force = 0.0;       // 0..1, replaces the computed magnitude when set
	double Gain = 1.0;        // multiplies the computed magnitude
	int LengthMs = -1;        // -1 = the default
	int PeriodMs = -1;        // sine only
	bool bHasType = false;
	FxType Type = FxType::Jolt;
};

struct Config
{
	std::wstring IniPath;
	std::vector<std::string> Streams; // first that exists wins; re-checked while none does
	std::string Device;               // "" = first haptic device, else index or a name substring
	int MinForce = 0, MaxForce = 100; // percent, upstream semantics
	bool bPowerMode = false;
	bool bReverse = false;
	int Gain = 100;                   // SDL device gain, percent
	int PollMs = 10;
	bool bLog = true;
	// The v1 line carries what the GAME played: rumble, camera shake and trauma. Their weights for a force.
	double RumbleWeight = 1.0, ShakeWeight = 0.8, TraumaWeight = 0.6;
	int JoltMs = 160, SineMs = 600, SinePeriodMs = 70, RumbleMs = 180;
	double Floor = 0.02; // events weaker than this play nothing
	std::map<std::string, IdRule> Rules;
};

static std::string Narrow(const std::wstring& W)
{
	if (W.empty()) return {};
	const int N = WideCharToMultiByte(CP_UTF8, 0, W.c_str(), (int)W.size(), nullptr, 0, nullptr, nullptr);
	std::string S(N, '\0');
	WideCharToMultiByte(CP_UTF8, 0, W.c_str(), (int)W.size(), S.data(), N, nullptr, nullptr);
	return S;
}

static std::wstring Widen(const std::string& S)
{
	if (S.empty()) return {};
	const int N = MultiByteToWideChar(CP_UTF8, 0, S.c_str(), (int)S.size(), nullptr, 0);
	std::wstring W(N, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, S.c_str(), (int)S.size(), W.data(), N);
	return W;
}

static std::wstring IniStr(const Config& C, const wchar_t* Sec, const wchar_t* Key, const wchar_t* Def)
{
	wchar_t Buf[1024];
	GetPrivateProfileStringW(Sec, Key, Def, Buf, 1024, C.IniPath.c_str());
	return Buf;
}

static double IniD(const Config& C, const wchar_t* Sec, const wchar_t* Key, double Def)
{
	const std::wstring S = IniStr(C, Sec, Key, L"");
	return S.empty() ? Def : _wtof(S.c_str());
}

static int IniI(const Config& C, const wchar_t* Sec, const wchar_t* Key, int Def)
{
	return GetPrivateProfileIntW(Sec, Key, Def, C.IniPath.c_str());
}

static std::string ExpandEnv(const std::string& S)
{
	wchar_t Buf[2048];
	const DWORD N = ExpandEnvironmentStringsW(Widen(S).c_str(), Buf, 2048);
	return (N > 0 && N <= 2048) ? Narrow(Buf) : S;
}

// Every Steam library on this PC: <SteamPath> plus each "path" in steamapps/libraryfolders.vdf. A Steam game lives in
// <library>/steamapps/common/<folder>, and a player's library can be on any drive.
static std::vector<std::string> SteamLibraries()
{
	std::vector<std::string> Out;
	wchar_t Buf[1024]; DWORD Sz = sizeof(Buf);
	if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, Buf, &Sz) != ERROR_SUCCESS)
		return Out;
	std::string Steam = Narrow(Buf);
	std::replace(Steam.begin(), Steam.end(), '/', '\\');
	Out.push_back(Steam);
	FILE* F = nullptr;
	if (_wfopen_s(&F, Widen(Steam + "\\steamapps\\libraryfolders.vdf").c_str(), L"rb") != 0 || !F) return Out;
	std::string Vdf;
	char Chunk[4096];
	for (size_t N; (N = fread(Chunk, 1, sizeof(Chunk), F)) > 0;) Vdf.append(Chunk, N);
	fclose(F);
	for (size_t P = 0; (P = Vdf.find("\"path\"", P)) != std::string::npos;)
	{
		const size_t A = Vdf.find('"', P + 6), B = A == std::string::npos ? A : Vdf.find('"', A + 1);
		if (B == std::string::npos) break;
		std::string Lib;
		for (size_t i = A + 1; i < B; ++i) { if (Vdf[i] == '\\' && i + 1 < B && Vdf[i + 1] == '\\') ++i; Lib += Vdf[i]; }
		if (std::none_of(Out.begin(), Out.end(), [&](const std::string& X) { return _stricmp(X.c_str(), Lib.c_str()) == 0; }))
			Out.push_back(Lib);
		P = B + 1;
	}
	return Out;
}

static FxType ParseType(const std::wstring& S, FxType Def)
{
	if (_wcsicmp(S.c_str(), L"jolt") == 0) return FxType::Jolt;
	if (_wcsicmp(S.c_str(), L"sine") == 0) return FxType::Sine;
	if (_wcsicmp(S.c_str(), L"rumble") == 0) return FxType::Rumble;
	if (_wcsicmp(S.c_str(), L"off") == 0) return FxType::Off;
	return Def;
}

static void LoadConfig(Config& C)
{
	const wchar_t* S = L"Settings";
	C.Device = Narrow(IniStr(C, S, L"Device", L""));
	C.MinForce = std::clamp(IniI(C, S, L"MinForce", C.MinForce), 0, 100);
	C.MaxForce = std::clamp(IniI(C, S, L"MaxForce", C.MaxForce), 0, 100);
	C.bPowerMode = IniI(C, S, L"PowerMode", 0) != 0;
	C.bReverse = IniI(C, S, L"Reverse", 0) != 0;
	C.Gain = std::clamp(IniI(C, S, L"Gain", C.Gain), 0, 100);
	C.PollMs = std::clamp(IniI(C, S, L"PollMs", C.PollMs), 1, 200);
	C.bLog = IniI(C, S, L"Logging", 1) != 0;

	const wchar_t* M = L"Mapping";
	C.RumbleWeight = IniD(C, M, L"RumbleWeight", C.RumbleWeight);
	C.ShakeWeight = IniD(C, M, L"ShakeWeight", C.ShakeWeight);
	C.TraumaWeight = IniD(C, M, L"TraumaWeight", C.TraumaWeight);
	C.JoltMs = IniI(C, M, L"JoltMs", C.JoltMs);
	C.SineMs = IniI(C, M, L"SineMs", C.SineMs);
	C.SinePeriodMs = IniI(C, M, L"SinePeriodMs", C.SinePeriodMs);
	C.RumbleMs = IniI(C, M, L"RumbleMs", C.RumbleMs);
	C.Floor = IniD(C, M, L"Floor", C.Floor);

	if (C.Streams.empty())
	{
		for (int i = 1; i <= 16; ++i)
		{
			wchar_t Key[16];
			swprintf_s(Key, L"Stream%d", i);
			const std::wstring P = IniStr(C, L"Streams", Key, L"");
			if (!P.empty()) C.Streams.push_back(ExpandEnv(Narrow(P)));
		}
		// SteamGame1=DEFY|artifacts\feeds\feedback.ndjson: <steamapps/common folder>|<path inside the install>,
		// tried in every Steam library on this PC.
		const std::vector<std::string> Libs = SteamLibraries();
		for (int i = 1; i <= 16; ++i)
		{
			wchar_t Key[16];
			swprintf_s(Key, L"SteamGame%d", i);
			const std::string V = Narrow(IniStr(C, L"Streams", Key, L""));
			const size_t Bar = V.find('|');
			if (Bar == std::string::npos) continue;
			for (const auto& L : Libs)
				C.Streams.push_back(L + "\\steamapps\\common\\" + V.substr(0, Bar) + "\\" + V.substr(Bar + 1));
		}
	}

	// Every other section is a per-id rule: [fb_shockwave_pass] Type=sine Force=80 LengthMs=900.
	std::vector<wchar_t> Names(32768);
	const DWORD N = GetPrivateProfileSectionNamesW(Names.data(), (DWORD)Names.size(), C.IniPath.c_str());
	for (const wchar_t* P = Names.data(); P < Names.data() + N && *P; P += wcslen(P) + 1)
	{
		if (!_wcsicmp(P, L"Settings") || !_wcsicmp(P, L"Mapping") || !_wcsicmp(P, L"Streams") || !_wcsicmp(P, L"Board")) continue;
		IdRule R;
		const std::wstring F = IniStr(C, P, L"Force", L"");
		if (!F.empty()) { R.bHasForce = true; R.Force = std::clamp(_wtof(F.c_str()) / 100.0, 0.0, 1.0); }
		R.Gain = IniD(C, P, L"Gain", 1.0);
		R.LengthMs = IniI(C, P, L"LengthMs", -1);
		R.PeriodMs = IniI(C, P, L"PeriodMs", -1);
		const std::wstring T = IniStr(C, P, L"Type", L"");
		if (!T.empty()) { R.bHasType = true; R.Type = ParseType(T, FxType::Jolt); }
		C.Rules[Narrow(P)] = R;
	}
}

// ------------------------------------------------------------------------------------------------------- protocol v1

struct Event
{
	std::string Id, Src;
	double T = 0, Scale = 1, Rumble = 0, Shake = 0, Trauma = 0, X = 0, Y = 0;
	bool bHasPos = false;
	int Version = 0;
};

// A v1 line is flat JSON with string and number values only; a full JSON parser would be dead weight here.
static bool FindRaw(const std::string& L, const char* Key, size_t& At)
{
	const std::string Needle = std::string("\"") + Key + "\":";
	const size_t P = L.find(Needle);
	if (P == std::string::npos) return false;
	At = P + Needle.size();
	while (At < L.size() && L[At] == ' ') ++At;
	return At < L.size();
}

static bool GetNum(const std::string& L, const char* Key, double& Out)
{
	size_t At;
	if (!FindRaw(L, Key, At)) return false;
	char* End = nullptr;
	Out = strtod(L.c_str() + At, &End);
	return End != L.c_str() + At;
}

static bool GetStr(const std::string& L, const char* Key, std::string& Out)
{
	size_t At;
	if (!FindRaw(L, Key, At) || L[At] != '"') return false;
	Out.clear();
	for (size_t i = At + 1; i < L.size(); ++i)
	{
		if (L[i] == '\\' && i + 1 < L.size()) { Out += L[++i]; continue; }
		if (L[i] == '"') return true;
		Out += L[i];
	}
	return false;
}

static bool ParseLine(const std::string& L, Event& E)
{
	if (L.empty() || L[0] != '{' || !GetStr(L, "id", E.Id)) return false;
	double V = 0;
	E.Version = GetNum(L, "v", V) ? (int)V : 0;
	GetStr(L, "src", E.Src);
	GetNum(L, "t", E.T);
	GetNum(L, "scale", E.Scale);
	GetNum(L, "rumble", E.Rumble);
	GetNum(L, "shake", E.Shake);
	GetNum(L, "trauma", E.Trauma);
	E.bHasPos = GetNum(L, "x", E.X) && GetNum(L, "y", E.Y);
	return true;
}

// ------------------------------------------------------------------------------------------------------------ mapping

struct Plan
{
	FxType Type = FxType::Off;
	double Strength = 0; // 0..1 after the curve and Min/Max: what the device is asked for
	int LengthMs = 0, PeriodMs = 0;
	int Direction = 1;   // -1 / +1 on the wheel axis
};

static const IdRule* FindRule(const Config& C, const std::string& Id)
{
	auto It = C.Rules.find(Id);
	if (It != C.Rules.end()) return &It->second;
	// Prefix rules: [fb_hit_*] matches fb_hit_tank and fb_hit_heavy.
	for (const auto& [K, R] : C.Rules)
		if (!K.empty() && K.back() == '*' && Id.compare(0, K.size() - 1, K, 0, K.size() - 1) == 0) return &R;
	return nullptr;
}

static Plan MapEvent(const Config& C, const Event& E, unsigned& DirSeed)
{
	Plan P;
	const IdRule* R = FindRule(C, E.Id);
	double M = std::max({ E.Rumble * C.RumbleWeight, E.Shake * C.ShakeWeight, E.Trauma * C.TraumaWeight });
	if (R)
	{
		if (R->bHasForce) M = R->Force * std::clamp(E.Scale, 0.0, 1.0);
		M *= R->Gain;
	}
	M = std::clamp(M, 0.0, 1.0);
	if (M < C.Floor) return P;

	// No rule: something the camera felt is a jolt, a pure rumble stays a rumble.
	P.Type = (R && R->bHasType) ? R->Type : ((E.Shake > 0 || E.Trauma > 0) ? FxType::Jolt : FxType::Rumble);
	if (P.Type == FxType::Off) return P;

	if (C.bPowerMode) M = std::sqrt(M);
	const double Lo = C.MinForce / 100.0, Hi = C.MaxForce / 100.0;
	P.Strength = Lo + (Hi - Lo) * M;
	P.LengthMs = (R && R->LengthMs > 0) ? R->LengthMs
	           : P.Type == FxType::Sine ? C.SineMs : P.Type == FxType::Rumble ? C.RumbleMs : C.JoltMs;
	P.PeriodMs = (R && R->PeriodMs > 0) ? R->PeriodMs : C.SinePeriodMs;
	// A jolt kicks the wheel alternately left and right, so a barrage reads as a shudder rather than a pull.
	DirSeed = DirSeed * 1103515245u + 12345u;
	P.Direction = ((DirSeed >> 16) & 1) ? 1 : -1;
	if (C.bReverse) P.Direction = -P.Direction;
	return P;
}

static const char* TypeName(FxType T)
{
	return T == FxType::Jolt ? "jolt" : T == FxType::Sine ? "sine" : T == FxType::Rumble ? "rumble" : "off";
}

// -------------------------------------------------------------------------------------------------------------- board

#include "board.h"

static void LoadBoard(const Config& C, Board::Settings& B)
{
	const wchar_t* S = L"Board";
	if (B.Port.empty()) B.Port = Narrow(IniStr(C, S, L"Port", L""));
	B.Baud = IniI(C, S, L"Baud", B.Baud);
	B.Brightness = std::clamp(IniI(C, S, L"Brightness", B.Brightness), 0, 100);
	B.MotorScale = std::clamp(IniI(C, S, L"MotorScale", B.MotorScale), 0, 100);
	B.MaxMotorMs = std::clamp(IniI(C, S, L"MaxMotorMs", B.MaxMotorMs), 0, Board::MotorMaxOnMs);
	B.bForceToMotor = IniI(C, S, L"ForceToMotor", 0) != 0;
	B.bLights = IniI(C, S, L"Lights", 1) != 0;
	B.bMotors = IniI(C, S, L"Motors", 1) != 0;
}

// The board cue for one line: the game's own board fields, else (ForceToMotor) the wheel's plan on every motor.
static bool BoardCueFor(const Board::Settings& B, const std::string& L, const Plan& P, Board::Cue& Out)
{
	Out = Board::Cue();
	if (Board::CueFromLine(L, Out)) return true;
	return B.bForceToMotor && Board::CueFromPlan(P, Out);
}

// ------------------------------------------------------------------------------------------------------------- device

struct Device
{
	SDL_Haptic* Haptic = nullptr;
	SDL_Gamepad* Pad = nullptr; // used only when no haptic device can be opened
	Uint32 Features = 0;
	bool bRumble = false;
	SDL_HapticEffectID ConstId = -1, SineId = -1;
	std::string Name;
	// "Do not cut a running effect short for a weaker one" (upstream TriggerSineEffect).
	Uint64 BusyUntil = 0;
	double BusyStrength = 0;

	bool IsOpen() const { return Haptic || Pad; }

	void Close()
	{
		if (Haptic) { SDL_StopHapticEffects(Haptic); SDL_CloseHaptic(Haptic); }
		if (Pad) { SDL_RumbleGamepad(Pad, 0, 0, 0); SDL_CloseGamepad(Pad); }
		*this = Device();
	}
};

static SDL_HapticEffect MakeConst(int Dir, double S, int Ms)
{
	SDL_HapticEffect Fx;
	SDL_memset(&Fx, 0, sizeof(Fx));
	Fx.type = SDL_HAPTIC_CONSTANT;
	Fx.constant.direction.type = SDL_HAPTIC_CARTESIAN;
	Fx.constant.direction.dir[0] = Dir;
	Fx.constant.length = (Uint32)Ms;
	Fx.constant.level = (Sint16)std::lround(std::clamp(S, 0.0, 1.0) * 32767.0);
	// A kick, not a push: full at once, gone by the end.
	Fx.constant.fade_length = (Uint16)std::min(Ms * 2 / 3, 65535);
	Fx.constant.fade_level = 0;
	return Fx;
}

static SDL_HapticEffect MakeSine(double S, int Ms, int PeriodMs)
{
	SDL_HapticEffect Fx;
	SDL_memset(&Fx, 0, sizeof(Fx));
	Fx.type = SDL_HAPTIC_SINE;
	Fx.periodic.direction.type = SDL_HAPTIC_CARTESIAN;
	Fx.periodic.direction.dir[0] = 1;
	Fx.periodic.length = (Uint32)Ms;
	Fx.periodic.period = (Uint16)std::clamp(PeriodMs, 10, 1000);
	Fx.periodic.magnitude = (Sint16)std::lround(std::clamp(S, 0.0, 1.0) * 32767.0);
	Fx.periodic.attack_length = (Uint16)std::min(Ms / 6, 65535);
	Fx.periodic.fade_length = (Uint16)std::min(Ms / 2, 65535);
	return Fx;
}

static bool OpenHaptic(Device& D, SDL_HapticID Id, const Config& C)
{
	D.Haptic = SDL_OpenHaptic(Id);
	if (!D.Haptic) return false;
	D.Name = SDL_GetHapticName(D.Haptic) ? SDL_GetHapticName(D.Haptic) : "haptic";
	D.Features = SDL_GetHapticFeatures(D.Haptic);
	if (D.Features & SDL_HAPTIC_GAIN) SDL_SetHapticGain(D.Haptic, C.Gain);
	if (D.Features & SDL_HAPTIC_CONSTANT)
	{
		SDL_HapticEffect Fx = MakeConst(1, 0, 1);
		D.ConstId = SDL_CreateHapticEffect(D.Haptic, &Fx);
	}
	if (D.Features & SDL_HAPTIC_SINE)
	{
		SDL_HapticEffect Fx = MakeSine(0, 1, 70);
		D.SineId = SDL_CreateHapticEffect(D.Haptic, &Fx);
	}
	D.bRumble = SDL_HapticRumbleSupported(D.Haptic) && SDL_InitHapticRumble(D.Haptic);
	return true;
}

static bool OpenDevice(Device& D, const Config& C)
{
	int N = 0;
	SDL_HapticID* Ids = SDL_GetHaptics(&N);
	const bool bIndex = !C.Device.empty() && std::all_of(C.Device.begin(), C.Device.end(), ::isdigit);
	for (int i = 0; Ids && i < N && !D.IsOpen(); ++i)
	{
		const char* Nm = SDL_GetHapticNameForID(Ids[i]);
		const std::string Name = Nm ? Nm : "";
		const bool bPick = C.Device.empty() || (bIndex ? atoi(C.Device.c_str()) == i
		                                              : Name.find(C.Device) != std::string::npos);
		if (bPick) OpenHaptic(D, Ids[i], C);
	}
	SDL_free(Ids);
	if (D.IsOpen()) return true;

	// No haptic device (or none matched): a gamepad's rumble still works through the gamepad API.
	SDL_JoystickID* Pads = SDL_GetGamepads(&N);
	for (int i = 0; Pads && i < N && !D.IsOpen(); ++i)
	{
		const char* Nm = SDL_GetGamepadNameForID(Pads[i]);
		const std::string Name = Nm ? Nm : "gamepad";
		if (!C.Device.empty() && !bIndex && Name.find(C.Device) == std::string::npos) continue;
		D.Pad = SDL_OpenGamepad(Pads[i]);
		if (D.Pad) D.Name = Name + " (gamepad rumble)";
	}
	SDL_free(Pads);
	return D.IsOpen();
}

static void Play(Device& D, const Plan& P)
{
	if (!D.IsOpen() || P.Type == FxType::Off || P.Strength <= 0) return;
	const Uint64 Now = SDL_GetTicks();
	if (Now < D.BusyUntil && P.Strength < D.BusyStrength * 1.5) return;
	D.BusyUntil = Now + (Uint64)P.LengthMs;
	D.BusyStrength = P.Strength;

	if (D.Pad)
	{
		const Uint16 Lv = (Uint16)std::lround(P.Strength * 65535.0);
		// A jolt leans on the heavy motor, a rumble on the light one, a sine on both.
		const Uint16 Low = P.Type == FxType::Rumble ? Lv / 2 : Lv;
		const Uint16 High = P.Type == FxType::Jolt ? Lv / 2 : Lv;
		SDL_RumbleGamepad(D.Pad, Low, High, (Uint32)P.LengthMs);
		return;
	}
	FxType T = P.Type;
	// Fall back down the chain when the device lacks the effect: wheels without sine jolt, pads rumble.
	if (T == FxType::Sine && D.SineId < 0) T = D.ConstId >= 0 ? FxType::Jolt : FxType::Rumble;
	if (T == FxType::Jolt && D.ConstId < 0) T = D.SineId >= 0 ? FxType::Sine : FxType::Rumble;
	if (T == FxType::Jolt)
	{
		SDL_HapticEffect Fx = MakeConst(P.Direction, P.Strength, P.LengthMs);
		if (SDL_UpdateHapticEffect(D.Haptic, D.ConstId, &Fx)) SDL_RunHapticEffect(D.Haptic, D.ConstId, 1);
	}
	else if (T == FxType::Sine)
	{
		SDL_HapticEffect Fx = MakeSine(P.Strength, P.LengthMs, P.PeriodMs);
		if (SDL_UpdateHapticEffect(D.Haptic, D.SineId, &Fx)) SDL_RunHapticEffect(D.Haptic, D.SineId, 1);
	}
	else if (D.bRumble)
	{
		SDL_PlayHapticRumble(D.Haptic, (float)P.Strength, (Uint32)P.LengthMs);
	}
}

// ------------------------------------------------------------------------------------------------------------- tailer

// The game TRUNCATES the stream when a match starts and appends one line per event (DEFY: DefyFeedbackSubsystem).
// Attach at the end (old events are history), reopen from the start when the file shrinks, keep partial lines.
struct Tailer
{
	std::string Path;
	HANDLE H = INVALID_HANDLE_VALUE;
	LONGLONG Pos = 0;
	std::string Partial;

	bool Open(const std::string& P, bool bFromEnd)
	{
		Close();
		H = CreateFileW(Widen(P).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (H == INVALID_HANDLE_VALUE) return false;
		Path = P;
		LARGE_INTEGER Sz{};
		GetFileSizeEx(H, &Sz);
		Pos = bFromEnd ? Sz.QuadPart : 0;
		Partial.clear();
		return true;
	}

	void Close()
	{
		if (H != INVALID_HANDLE_VALUE) CloseHandle(H);
		H = INVALID_HANDLE_VALUE;
	}

	template <class F> void Poll(F&& OnLine)
	{
		if (H == INVALID_HANDLE_VALUE) return;
		LARGE_INTEGER Sz{};
		if (!GetFileSizeEx(H, &Sz)) return;
		if (Sz.QuadPart < Pos) { Pos = 0; Partial.clear(); } // a new match truncated it
		while (Pos < Sz.QuadPart)
		{
			char Buf[16384];
			LARGE_INTEGER At; At.QuadPart = Pos;
			SetFilePointerEx(H, At, nullptr, FILE_BEGIN);
			DWORD Got = 0;
			const DWORD Want = (DWORD)std::min<LONGLONG>(sizeof(Buf), Sz.QuadPart - Pos);
			if (!ReadFile(H, Buf, Want, &Got, nullptr) || Got == 0) break;
			Pos += Got;
			Partial.append(Buf, Got);
			size_t Nl;
			while ((Nl = Partial.find('\n')) != std::string::npos)
			{
				std::string L = Partial.substr(0, Nl);
				Partial.erase(0, Nl + 1);
				if (!L.empty() && L.back() == '\r') L.pop_back();
				OnLine(L);
			}
		}
	}
};

// --------------------------------------------------------------------------------------------------------------- modes

static volatile bool GRun = true;

static BOOL WINAPI OnCtrl(DWORD)
{
	GRun = false;
	return TRUE;
}

static int SelfTest()
{
	int Fail = 0;
	auto Check = [&](bool b, const char* What) { if (!b) { ++Fail; printf("FAIL %s\n", What); } else printf("ok   %s\n", What); };

	Event E;
	const std::string L1 = "{\"t\":15.091,\"tick\":8520,\"id\":\"fb_shockwave_pass\",\"src\":\"hook.shock\",\"x\":68.92,"
	                       "\"y\":66.85,\"scale\":0.800,\"rumble\":0.400,\"shake\":0.500,\"trauma\":0.300,\"traumaNow\":0.3,\"v\":1}";
	Check(ParseLine(L1, E), "parse a v1 line");
	Check(E.Id == "fb_shockwave_pass" && E.Version == 1, "id and version");
	Check(std::fabs(E.Rumble - 0.4) < 1e-9 && std::fabs(E.Shake - 0.5) < 1e-9 && E.bHasPos, "numbers and position");
	Event E2;
	Check(ParseLine("{\"id\":\"a\\\"b\",\"v\":1}", E2) && E2.Id == "a\"b", "escaped quote in a string");
	Check(!ParseLine("not json", E2) && !ParseLine("{\"tick\":1}", E2), "reject a line without an id");

	Config C;
	unsigned Seed = 1;
	Plan P = MapEvent(C, E, Seed);
	Check(P.Type == FxType::Jolt && std::fabs(P.Strength - 0.4) < 1e-9, "shake beats rumble: jolt at max(0.4, 0.4, 0.18)");
	C.MinForce = 20; C.MaxForce = 80;
	P = MapEvent(C, E, Seed);
	Check(std::fabs(P.Strength - (0.2 + 0.6 * 0.4)) < 1e-9, "MinForce/MaxForce remap");
	C.bPowerMode = true;
	P = MapEvent(C, E, Seed);
	Check(std::fabs(P.Strength - (0.2 + 0.6 * std::sqrt(0.4))) < 1e-9, "PowerMode square-root curve");

	Config C2;
	IdRule R; R.bHasType = true; R.Type = FxType::Sine; R.LengthMs = 900;
	C2.Rules["fb_shockwave_*"] = R;
	P = MapEvent(C2, E, Seed);
	Check(P.Type == FxType::Sine && P.LengthMs == 900, "prefix rule picks the sine and its length");
	IdRule Off; Off.bHasType = true; Off.Type = FxType::Off;
	C2.Rules["fb_shockwave_pass"] = Off;
	Check(MapEvent(C2, E, Seed).Type == FxType::Off, "exact rule beats a prefix rule");

	Event Ui; Ui.Id = "fb_selection"; Ui.Rumble = 0.01;
	Check(MapEvent(Config(), Ui, Seed).Type == FxType::Off, "below the floor plays nothing");
	Event Rm; Rm.Id = "fb_unit_ready"; Rm.Rumble = 0.3;
	Check(MapEvent(Config(), Rm, Seed).Type == FxType::Rumble, "rumble without shake stays a rumble");

	// The board (board.h): Nightmarch's wire protocol v1, checked against its fixtures (firmware/protocol/fixtures.json).
	const unsigned char Nine[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
	Check(Board::Crc16(Nine, 9) == 0x29B1, "CRC-16/CCITT-FALSE check value");
	Check(Board::HexOf(Board::Encode(Board::MsgLed, 8, { 0x01, 0x00, 0x03, 0xc8, 0x14, 0x0a, 0x58, 0x02, 0x05 }))
	      == "a55a01070809010003c8140a5802052846", "LED frame matches the protocol fixture led_pulse_once");
	Board::Cue K;
	const std::string L2 = "{\"id\":\"fb_nm_fallen\",\"rumble\":0.9,\"terr\":7,\"fx\":4,\"rgb\":\"C81414\",\"lightMs\":1500,"
	                       "\"pri\":3,\"motor\":230,\"motorMs\":450,\"shape\":2,\"periodMs\":300,\"v\":1}";
	Check(Board::CueFromLine(L2, K) && K.Territory == 7 && K.Effect == 4 && K.Rgb == 0xC81414 && K.Motor == 230,
	      "board fields on a v1 line");
	Check(Board::MotorSteps(K) == std::vector<int>{ 230, 201, 173, 144, 115, 86, 58, 0, 0, 0 },
	      "a fade is Nightmarch Board.cs's fade (BoardTests)");
	Board::Cue H; H.Motor = 220; H.MotorMs = 2400; H.MotorShape = Board::Shape::Heartbeat; H.PeriodMs = 600;
	auto HS = Board::MotorSteps(H);
	Check(std::vector<int>(HS.begin(), HS.begin() + 14) == std::vector<int>{ 220, 220, 0, 154, 154, 0, 0, 0, 0, 0, 0, 0, 220, 220 },
	      "a heartbeat is Nightmarch Board.cs's heartbeat");
	Board::Settings Bs;
	unsigned char Sq = 0;
	auto Fr = Board::Frames(Bs, K, Sq);
	Check(Fr.size() == 9 && Fr.front().Bytes[3] == Board::MsgLed && Fr.back().Bytes[8] == 0 && Fr.back().DueMs == 350,
	      "a cue is one LED frame then RUMBLE frames, ending stopped");
	Board::Cue Long; Long.Motor = 255; Long.MotorMs = 60000; Long.MotorShape = Board::Shape::Steady;
	Check(Board::MotorSteps(Long).size() == Board::MotorMaxOnMs / Board::StepMs + 1, "no motor runs past the firmware ceiling");
	Board::Cue None;
	Check(!Board::CueFromLine(L1, None), "a line without board fields has no board cue");
	Plan Jp; Jp.Type = FxType::Jolt; Jp.Strength = 0.5; Jp.LengthMs = 200;
	Board::Settings Fm; Fm.bForceToMotor = true;
	Check(BoardCueFor(Fm, L1, Jp, None) && None.MotorShape == Board::Shape::Fade && None.Territory == 0 && !None.bHasLight,
	      "ForceToMotor: another game's jolt is a fade on every prop");

	printf("%s (%d failed)\n", Fail ? "SELFTEST FAILED" : "SELFTEST PASS", Fail);
	return Fail ? 1 : 0;
}

static int List()
{
	int N = 0;
	SDL_HapticID* Ids = SDL_GetHaptics(&N);
	printf("Haptic devices: %d\n", N);
	for (int i = 0; Ids && i < N; ++i)
	{
		SDL_Haptic* H = SDL_OpenHaptic(Ids[i]);
		const Uint32 F = H ? SDL_GetHapticFeatures(H) : 0;
		printf("  [%d] %s  constant=%d sine=%d spring=%d friction=%d gain=%d rumble=%d axes=%d\n", i,
		       SDL_GetHapticNameForID(Ids[i]) ? SDL_GetHapticNameForID(Ids[i]) : "?",
		       !!(F & SDL_HAPTIC_CONSTANT), !!(F & SDL_HAPTIC_SINE), !!(F & SDL_HAPTIC_SPRING), !!(F & SDL_HAPTIC_FRICTION),
		       !!(F & SDL_HAPTIC_GAIN), H ? (int)SDL_HapticRumbleSupported(H) : 0, H ? SDL_GetNumHapticAxes(H) : 0);
		if (H) SDL_CloseHaptic(H);
	}
	SDL_free(Ids);
	SDL_JoystickID* Pads = SDL_GetGamepads(&N);
	printf("Gamepads: %d\n", N);
	for (int i = 0; Pads && i < N; ++i)
		printf("  [%d] %s\n", i, SDL_GetGamepadNameForID(Pads[i]) ? SDL_GetGamepadNameForID(Pads[i]) : "?");
	SDL_free(Pads);
	return 0;
}

static int Test(const Config& C)
{
	Device D;
	if (!OpenDevice(D, C)) { printf("No haptic device or gamepad found.\n"); return 1; }
	printf("Device: %s\n", D.Name.c_str());
	const struct { const char* What; Plan P; } Steps[] = {
		{ "jolt left", { FxType::Jolt, 0.6, 250, 0, -1 } },
		{ "jolt right", { FxType::Jolt, 0.6, 250, 0, 1 } },
		{ "sine roll", { FxType::Sine, 0.5, 900, 70, 1 } },
		{ "rumble", { FxType::Rumble, 0.6, 500, 0, 1 } },
	};
	for (const auto& S : Steps)
	{
		printf("  %s\n", S.What);
		D.BusyUntil = 0;
		Play(D, S.P);
		SDL_Delay((Uint32)S.P.LengthMs + 400);
	}
	D.Close();
	return 0;
}

static int BoardTest(const Board::Settings& B)
{
	Board::Link Lk;
	if (!Lk.Open(B)) { printf("No board: set [Board] Port=COMn in the INI or pass --board COMn (could not open '%s').\n", B.Port.c_str()); return 1; }
	printf("Board: %s at %d baud.\n", B.Port.c_str(), B.Baud);
	struct Step { const char* What; Board::Cue C; int WaitMs; };
	auto Mk = [](int Fx, unsigned Rgb, int LightMs, int Motor, int MotorMs, Board::Shape Sh, int Period)
	{
		Board::Cue C; C.bHasLight = true; C.Effect = Fx; C.Rgb = Rgb; C.LightMs = LightMs; C.Priority = 4;
		C.Motor = Motor; C.MotorMs = MotorMs; C.MotorShape = Sh; C.PeriodMs = Period; return C;
	};
	const Step Steps[] = {
		{ "ember sweep, no motor", Mk(5, 0xF0A340, 1500, 0, 0, Board::Shape::None, 300), 1800 },
		{ "cyan strobe, gears", Mk(4, 0x5AD6C6, 1500, 255, 1500, Board::Shape::Pulse, 240), 1800 },
		{ "red pulse, a fade", Mk(3, 0xC81414, 900, 230, 450, Board::Shape::Fade, 300), 1200 },
		{ "gold breathe, a swell", Mk(2, 0xFFD23C, 1500, 200, 1200, Board::Shape::Swell, 300), 1600 },
		{ "moonlight, a heartbeat", Mk(6, 0x283A6E, 0, 220, 2400, Board::Shape::Heartbeat, 600), 2800 },
	};
	for (const auto& S : Steps)
	{
		printf("  %s\n", S.What);
		const Uint64 T0 = SDL_GetTicks();
		Lk.Play(B, S.C, T0);
		while (SDL_GetTicks() < T0 + (Uint64)S.WaitMs && Lk.IsOpen()) { Lk.Service(SDL_GetTicks()); SDL_Delay(5); }
	}
	Lk.Close();
	return 0;
}

static int Run(const Config& C, const Board::Settings& B, bool bDry)
{
	Board::Link Lk;
	Device D;
	Tailer T;
	unsigned Seed = (unsigned)GetTickCount();
	Uint64 NextScan = 0;
	size_t Events = 0;
	printf("ZStudioFFBHost: %s. Ctrl+C to stop.\n", bDry ? "dry run (no device)" : "running");
	for (const auto& S : C.Streams) printf("  stream candidate: %s\n", S.c_str());
	if (C.Streams.empty()) { printf("No stream configured: set [Streams] Stream1= in the INI or pass --stream.\n"); return 1; }

	while (GRun)
	{
		SDL_PumpEvents();
		const Uint64 Now = SDL_GetTicks();
		if (Now >= NextScan)
		{
			NextScan = Now + 2000;
			if (!bDry && !D.IsOpen() && OpenDevice(D, C)) printf("Device: %s\n", D.Name.c_str());
			if (!bDry && !B.Port.empty() && !Lk.IsOpen() && Lk.Open(B)) printf("Board: %s at %d baud.\n", B.Port.c_str(), B.Baud);
			// Follow the most recently WRITTEN candidate: a PC with both the Steam game and a dev checkout has two streams,
			// and the one a match is writing is the newest. The game may create its file only when a match starts.
			std::string Best;
			ULONGLONG BestT = 0;
			for (const auto& S : C.Streams)
			{
				WIN32_FILE_ATTRIBUTE_DATA A;
				if (!GetFileAttributesExW(Widen(S).c_str(), GetFileExInfoStandard, &A)) continue;
				const ULONGLONG W = (ULONGLONG(A.ftLastWriteTime.dwHighDateTime) << 32) | A.ftLastWriteTime.dwLowDateTime;
				if (Best.empty() || W > BestT) { Best = S; BestT = W; }
			}
			if (!Best.empty() && (T.H == INVALID_HANDLE_VALUE || _stricmp(Best.c_str(), T.Path.c_str()) != 0))
			{
				// Attach at the end, as on first attach: what was written before is history.
				if (T.Open(Best, /*bFromEnd*/ true)) printf("Attached: %s\n", Best.c_str());
			}
		}
		T.Poll([&](const std::string& L)
		{
			Event E;
			if (!ParseLine(L, E)) return;
			if (E.Version > 1 && Events == 0) printf("Note: stream is protocol v%d; this host speaks v1 fields.\n", E.Version);
			++Events;
			const Plan P = MapEvent(C, E, Seed);
			if (C.bLog && P.Type != FxType::Off)
				printf("%9.3f %-24s %-6s %.2f %4d ms%s\n", E.T, E.Id.c_str(), TypeName(P.Type), P.Strength, P.LengthMs,
				       P.Type == FxType::Jolt ? (P.Direction < 0 ? " <" : " >") : "");
			if (!bDry) Play(D, P);
			Board::Cue K;
			if (BoardCueFor(B, L, P, K))
			{
				if (bDry)
				{
					unsigned char Sq = 0;
					const auto Fr = Board::Frames(B, K, Sq);
					printf("          board: prop %d fx %d #%06X %d ms, motor %d %d ms shape %d -> %zu frames, first %s\n",
					       K.Territory, K.Effect, K.Rgb, K.LightMs, K.Motor, K.MotorMs, (int)K.MotorShape, Fr.size(),
					       Fr.empty() ? "-" : Board::HexOf(Fr.front().Bytes).c_str());
				}
				else if (Lk.IsOpen()) Lk.Play(B, K, SDL_GetTicks());
			}
		});
		Lk.Service(SDL_GetTicks());
		SDL_Delay((Uint32)C.PollMs);
	}
	T.Close();
	D.Close();
	Lk.Close();
	printf("Stopped after %zu events.\n", Events);
	return 0;
}

int main(int argc, char** argv)
{
	SetConsoleOutputCP(CP_UTF8);
	setvbuf(stdout, nullptr, _IONBF, 0); // the log is live: a launcher or a pipe sees each event as it plays
	Config C;
	wchar_t Exe[MAX_PATH];
	GetModuleFileNameW(nullptr, Exe, MAX_PATH);
	std::wstring Dir = Exe;
	Dir = Dir.substr(0, Dir.find_last_of(L"\\/") + 1);
	C.IniPath = Dir + L"ZStudioFFB.ini";

	std::string Mode = "run";
	Board::Settings BoardSt;
	for (int i = 1; i < argc; ++i)
	{
		const std::string A = argv[i];
		if (A == "--ini" && i + 1 < argc) C.IniPath = Widen(argv[++i]);
		else if (A == "--stream" && i + 1 < argc) C.Streams.push_back(ExpandEnv(argv[++i]));
		else if (A == "--list") Mode = "list";
		else if (A == "--test") Mode = "test";
		else if (A == "--dry-run") Mode = "dry";
		else if (A == "--selftest") Mode = "selftest";
		else if (A == "--board-test") Mode = "boardtest";
		else if (A == "--board" && i + 1 < argc) BoardSt.Port = argv[++i];
		else { printf("Unknown argument: %s\nSee the header of ZStudioFFBHost/main.cpp or README.md.\n", A.c_str()); return 2; }
	}
	if (Mode == "selftest") return SelfTest();

	LoadConfig(C);
	LoadBoard(C, BoardSt);
	if (!SDL_Init(SDL_INIT_HAPTIC | SDL_INIT_GAMEPAD))
	{
		printf("SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}
	SetConsoleCtrlHandler(OnCtrl, TRUE);
	int Rc = 0;
	if (Mode == "list") Rc = List();
	else if (Mode == "test") Rc = Test(C);
	else if (Mode == "boardtest") Rc = BoardTest(BoardSt);
	else Rc = Run(C, BoardSt, Mode == "dry");
	SDL_Quit();
	return Rc;
}
