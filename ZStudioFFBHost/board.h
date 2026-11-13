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

// The board output: ESP32 props on a physical game board, through a USB-tethered bridge ESP32 that fans frames out over
// ESP-NOW. Each prop has WS2812/SK6812 LEDs (driven over RMT) and a PWM-driven vibration motor on a motor driver.
//
// The wire is the Nightmarch hardware protocol v1 (Nightmarch/firmware/protocol/protocol.toml is the authority):
//   A5 5A | ver | type | seq | len | payload | crcLo crcHi     CRC-16/CCITT-FALSE over ver..payload, little-endian
// This host sends only two of its messages:
//   LED    0x07  territory u16, effect u8, r u8, g u8, b u8, duration_ms u16, priority u8
//   RUMBLE 0x08  territory u16, strength u8, duration_ms u16
// plus HELLO 0x01 when the port opens. Territory 0 is every prop. A node's motor takes the NEWEST rumble frame's strength
// and length, so a motor SHAPE (a fade, a swell, gears, a heartbeat) is a train of short frames, one per 50 ms step,
// each lasting three steps, ending with strength 0. The node clamps every rumble to NM_RUMBLE_MAX_ON_MS (2500) on its own.
//
// Where the cue comes from:
//   * A game that knows its board (Nightmarch) puts it on the v1 line as flat extra fields: terr, fx, rgb ("F0A340"),
//     lightMs, pri, motor (0..255), motorMs, shape, periodMs. The shapes and the 50 ms step match Nightmarch's
//     src/Nightmarch.RulesEngine/Play/Board.cs exactly (--selftest checks a vector).
//   * Any other game: with [Board] ForceToMotor=1, the force this host would play on a wheel also plays on every prop's
//     motor (a jolt is a fade, a sine is a pulse at its period, a rumble is steady), with no light.

#pragma once

#include <deque>

namespace Board
{
	// Must match the firmware's nm_board.h and Nightmarch's Board.cs.
	constexpr int MotorMaxOnMs = 2500;
	constexpr int StepMs = 50;
	constexpr int MotorStallFloor = 40; // a vibration motor below about a fifth of full hums instead of turning
	constexpr unsigned char ProtocolVersion = 1;
	constexpr unsigned char MsgHello = 0x01, MsgHelloAck = 0x02, MsgLed = 0x07, MsgRumble = 0x08, MsgLog = 0x10, MsgSafe = 0x0F;
	constexpr int MaxPayload = 56;

	enum class Shape { None = 0, Steady = 1, Fade = 2, Swell = 3, Pulse = 4, Heartbeat = 5 };

	struct Cue
	{
		bool bHasLight = false;
		int Territory = 0;
		int Effect = 0;
		unsigned Rgb = 0;
		int LightMs = 0;
		int Priority = 0;
		int Motor = 0;      // 0..255
		int MotorMs = 0;
		Shape MotorShape = Shape::None;
		int PeriodMs = 300;
	};

	struct Settings
	{
		std::string Port;          // "COM5"; empty = no board
		int Baud = 921600;         // the bridge's NM_SERIAL_BAUD
		int Brightness = 100;      // percent, scales every colour this host sends
		int MotorScale = 100;      // percent, scales every motor strength
		int MaxMotorMs = MotorMaxOnMs;
		bool bForceToMotor = false; // games without board fields: their wheel force on every prop's motor
		bool bLights = true, bMotors = true;
	};

	inline unsigned short Crc16(const unsigned char* D, size_t N)
	{
		unsigned short C = 0xFFFF;
		for (size_t i = 0; i < N; ++i)
		{
			C ^= (unsigned short)(D[i] << 8);
			for (int b = 0; b < 8; ++b) C = (C & 0x8000) ? (unsigned short)((C << 1) ^ 0x1021) : (unsigned short)(C << 1);
		}
		return C;
	}

	inline std::vector<unsigned char> Encode(unsigned char Type, unsigned char Seq, const std::vector<unsigned char>& Payload)
	{
		std::vector<unsigned char> F = { 0xA5, 0x5A, ProtocolVersion, Type, Seq, (unsigned char)Payload.size() };
		F.insert(F.end(), Payload.begin(), Payload.end());
		const unsigned short C = Crc16(F.data() + 2, F.size() - 2);
		F.push_back((unsigned char)(C & 0xFF));
		F.push_back((unsigned char)(C >> 8));
		return F;
	}

	inline void PutU16(std::vector<unsigned char>& P, int V)
	{
		P.push_back((unsigned char)(V & 0xFF));
		P.push_back((unsigned char)((V >> 8) & 0xFF));
	}

	// The motor's strength per 50 ms step, ending with 0. Identical to Nightmarch Board.MotorSteps.
	inline std::vector<int> MotorSteps(const Cue& C)
	{
		std::vector<int> S;
		if (C.Motor <= 0 || C.MotorMs <= 0 || C.MotorShape == Shape::None) return S;
		const int Ms = std::min(C.MotorMs, MotorMaxOnMs);
		const int N = std::max(1, (Ms + StepMs - 1) / StepMs);
		const int Period = std::clamp(C.PeriodMs, StepMs * 2, 2000);
		for (int i = 0; i < N; ++i)
		{
			const double T = N == 1 ? 0 : i / (double)(N - 1);
			const double At = i * (double)StepMs;
			double K = 1;
			switch (C.MotorShape)
			{
			case Shape::Fade: K = 1 - T; break;
			case Shape::Swell: K = T; break;
			case Shape::Pulse: K = std::fmod(At, Period) < Period / 2 ? 1 : 0; break;
			case Shape::Heartbeat:
			{
				const double Ph = std::fmod(At, Period) / Period;
				K = Ph < 0.15 ? 1 : Ph < 0.25 ? 0 : Ph < 0.4 ? 0.7 : 0;
				break;
			}
			default: K = 1; break;
			}
			const int V = (int)(C.Motor * K + 0.5);
			S.push_back(V < MotorStallFloor ? 0 : V);
		}
		S.push_back(0);
		return S;
	}

	struct Timed
	{
		Uint64 DueMs;
		std::vector<unsigned char> Bytes;
	};

	// A cue as frames with send times relative to now: the LED at 0, then a RUMBLE per running step (a stopped motor once).
	inline std::vector<Timed> Frames(const Settings& St, const Cue& C, unsigned char& Seq)
	{
		std::vector<Timed> Out;
		if (C.bHasLight && St.bLights)
		{
			auto Sc = [&](unsigned V) { return (unsigned char)std::clamp((int)((V & 0xFF) * St.Brightness / 100), 0, 255); };
			std::vector<unsigned char> P;
			PutU16(P, C.Territory);
			P.push_back((unsigned char)C.Effect);
			P.push_back(Sc(C.Rgb >> 16)); P.push_back(Sc(C.Rgb >> 8)); P.push_back(Sc(C.Rgb));
			PutU16(P, std::clamp(C.LightMs, 0, 65535));
			P.push_back((unsigned char)std::clamp(C.Priority, 0, 255));
			Out.push_back({ 0, Encode(MsgLed, Seq++, P) });
		}
		if (C.Motor > 0 && St.bMotors)
		{
			Cue M = C;
			M.Motor = std::clamp(C.Motor * St.MotorScale / 100, 0, 255);
			M.MotorMs = std::min(C.MotorMs, std::clamp(St.MaxMotorMs, 0, MotorMaxOnMs));
			const std::vector<int> S = MotorSteps(M);
			int Last = -1;
			for (size_t i = 0; i < S.size(); ++i)
			{
				if (S[i] == 0 && Last == 0) continue;
				Last = S[i];
				std::vector<unsigned char> P;
				PutU16(P, C.Territory);
				P.push_back((unsigned char)S[i]);
				PutU16(P, S[i] == 0 ? 0 : StepMs * 3);
				Out.push_back({ (Uint64)(i * StepMs), Encode(MsgRumble, Seq++, P) });
			}
		}
		return Out;
	}

	// The board fields on a v1 line. False when the line has none.
	inline bool CueFromLine(const std::string& L, Cue& C)
	{
		double V;
		const bool bFx = GetNum(L, "fx", V);
		if (bFx) C.Effect = (int)V;
		const bool bMotor = GetNum(L, "motor", V) && V > 0;
		if (bMotor) C.Motor = std::clamp((int)V, 0, 255);
		if (!bFx && !bMotor) return false;
		C.bHasLight = bFx;
		if (GetNum(L, "terr", V)) C.Territory = std::clamp((int)V, 0, 65535);
		std::string Hex;
		if (GetStr(L, "rgb", Hex)) C.Rgb = (unsigned)strtoul(Hex.c_str(), nullptr, 16) & 0xFFFFFF;
		if (GetNum(L, "lightMs", V)) C.LightMs = (int)V;
		if (GetNum(L, "pri", V)) C.Priority = (int)V;
		if (GetNum(L, "motorMs", V)) C.MotorMs = (int)V;
		if (GetNum(L, "shape", V)) C.MotorShape = (Shape)std::clamp((int)V, 0, 5);
		if (GetNum(L, "periodMs", V)) C.PeriodMs = (int)V;
		if (C.Motor > 0 && C.MotorShape == Shape::None) C.MotorShape = Shape::Steady;
		return true;
	}

	// A game without board fields: the wheel's plan on every prop's motor (ForceToMotor).
	inline bool CueFromPlan(const Plan& P, Cue& C)
	{
		if (P.Type == FxType::Off || P.Strength <= 0) return false;
		C = Cue();
		C.Motor = std::clamp((int)(P.Strength * 255 + 0.5), 0, 255);
		C.MotorMs = P.LengthMs;
		C.MotorShape = P.Type == FxType::Jolt ? Shape::Fade : P.Type == FxType::Sine ? Shape::Pulse : Shape::Steady;
		C.PeriodMs = std::max(P.PeriodMs * 2, StepMs * 2); // a wheel's sine is far faster than a motor can follow
		return true;
	}

	inline std::string HexOf(const std::vector<unsigned char>& B)
	{
		std::string S;
		char T[4];
		for (unsigned char c : B) { snprintf(T, sizeof T, "%02x", c); S += T; }
		return S;
	}

	// The USB serial link to the bridge, and a queue of frames to send at their times.
	struct Link
	{
		HANDLE H = INVALID_HANDLE_VALUE;
		std::string Port;
		unsigned char Seq = 0;
		std::deque<Timed> Queue;
		std::vector<unsigned char> In;
		Uint64 NextTry = 0;

		bool IsOpen() const { return H != INVALID_HANDLE_VALUE; }

		bool Open(const Settings& St)
		{
			Close();
			if (St.Port.empty()) return false;
			const std::string Path = "\\\\.\\" + St.Port;
			H = CreateFileA(Path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
			if (H == INVALID_HANDLE_VALUE) return false;
			DCB D{};
			D.DCBlength = sizeof D;
			GetCommState(H, &D);
			D.BaudRate = (DWORD)St.Baud; D.ByteSize = 8; D.Parity = NOPARITY; D.StopBits = ONESTOPBIT;
			D.fBinary = TRUE; D.fDtrControl = DTR_CONTROL_ENABLE; D.fRtsControl = RTS_CONTROL_ENABLE;
			D.fOutxCtsFlow = FALSE; D.fOutxDsrFlow = FALSE; D.fOutX = FALSE; D.fInX = FALSE;
			if (!SetCommState(H, &D)) { Close(); return false; }
			COMMTIMEOUTS T{};
			T.ReadIntervalTimeout = MAXDWORD; // reads return at once with what is there
			T.WriteTotalTimeoutConstant = 50;
			SetCommTimeouts(H, &T);
			Port = St.Port;
			Write(Encode(MsgHello, Seq++, {}));
			return true;
		}

		void Close()
		{
			if (H != INVALID_HANDLE_VALUE) CloseHandle(H);
			H = INVALID_HANDLE_VALUE;
			Queue.clear();
		}

		void Write(const std::vector<unsigned char>& B)
		{
			if (!IsOpen()) return;
			DWORD W = 0;
			if (!WriteFile(H, B.data(), (DWORD)B.size(), &W, nullptr) || W != B.size())
			{
				printf("Board: lost %s.\n", Port.c_str());
				Close();
			}
		}

		// A new cue on a prop replaces the motor frames still waiting for that prop: the newest event wins, as on a node.
		void Play(const Settings& St, const Cue& C, Uint64 Now)
		{
			auto F = Frames(St, C, Seq);
			if (C.Motor > 0)
				Queue.erase(std::remove_if(Queue.begin(), Queue.end(), [&](const Timed& T)
				{
					return T.Bytes.size() > 7 && T.Bytes[3] == MsgRumble && (T.Bytes[6] | (T.Bytes[7] << 8)) == C.Territory;
				}), Queue.end());
			for (auto& T : F)
			{
				T.DueMs += Now;
				Queue.push_back(std::move(T));
			}
			std::stable_sort(Queue.begin(), Queue.end(), [](const Timed& A, const Timed& B) { return A.DueMs < B.DueMs; });
		}

		// Send what is due, and print what the bridge says (its HELLO_ACK, SAFE mode, its log lines).
		void Service(Uint64 Now)
		{
			while (IsOpen() && !Queue.empty() && Queue.front().DueMs <= Now)
			{
				Write(Queue.front().Bytes);
				if (!Queue.empty()) Queue.pop_front();
			}
			if (!IsOpen()) return;
			unsigned char Buf[256];
			DWORD Got = 0;
			if (ReadFile(H, Buf, sizeof Buf, &Got, nullptr) && Got) In.insert(In.end(), Buf, Buf + Got);
			for (;;)
			{
				size_t S = 0;
				while (S + 1 < In.size() && !(In[S] == 0xA5 && In[S + 1] == 0x5A)) ++S;
				In.erase(In.begin(), In.begin() + (std::min)(S, In.size()));
				if (In.size() < 8) break;
				const size_t Len = In[5], Total = 8 + Len;
				if (Len > 200) { In.erase(In.begin()); continue; }
				if (In.size() < Total) break;
				const unsigned short C = Crc16(In.data() + 2, 4 + Len);
				if ((In[Total - 2] | (In[Total - 1] << 8)) != C) { In.erase(In.begin()); continue; }
				const unsigned char Type = In[3];
				const unsigned char* P = In.data() + 6;
				if (Type == MsgHelloAck && Len >= 26)
					printf("Board: bridge on %s answers, protocol v%d%s.\n", Port.c_str(), P[14],
					       P[14] == ProtocolVersion ? "" : " -- NOT v1: it will ignore this host (SAFE mode)");
				else if (Type == MsgSafe && Len >= 3) printf("Board: the bridge entered SAFE mode (reason %d): outputs off.\n", P[0]);
				else if (Type == MsgLog && Len >= 1) printf("Board log: %.*s\n", (int)Len - 1, (const char*)P + 1);
				In.erase(In.begin(), In.begin() + Total);
			}
		}
	};
}
