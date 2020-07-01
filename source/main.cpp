#include <GarrysMod/Lua/Interface.h>
#include <GarrysMod/FactoryLoader.hpp>
#include <scanning/symbolfinder.hpp>
#include <detouring/detours.h>
#include <iostream>
#include <cbase.h>
#include <eifacev21.h>
#include <iclient.h>
#include <ivoicecodec.h>
#include <unordered_set>

#if defined SYSTEM_WINDOWS
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
#elif defined SYSTEM_LINUX
	#include <dlfcn.h>
#endif

#if defined SYSTEM_WINDOWS && defined ARCHITECTURE_X86_64
	static const uint8_t GMOD_SV_BroadcastVoice_sym_sig[] = "\x48\x89\x5C\x24\x20\x56\x57\x41\x56\x48\x81\xEC\xA0\x00\x00\x00";
	static const size_t GMOD_SV_BroadcastVoice_siglen = sizeof(GMOD_SV_BroadcastVoice_sym_sig) - 1;
#elif defined SYSTEM_LINUX && defined ARCHITECTURE_X86
	static const char* GMOD_SV_BroadcastVoice_sym_sig = "_Z21SV_BroadcastVoiceDataP7IClientiPcx";
#else
	#error Missing signatures for this system!
#endif

static int crushFactor = 8;
static IVoiceCodec* g_pVoiceCodec = nullptr;
static short decompressedBuffer[11500];
static char recompressBuffer[11500*2];
static IVEngineServer* engine_ptr = nullptr;

typedef void (*SV_BroadcastVoiceData)(IClient* cl, int nBytes, char* data, int64 xuid);
MologieDetours::Detour<SV_BroadcastVoiceData>* detour_BroadcastVoiceData = nullptr;

std::unordered_set<int> afflicted_players;



void hook_BroadcastVoiceData(IClient* cl, int nBytes, char* data, int64 xuid) {
	//Check if the player is in the set of enabled players.
	//This is (and needs to be) and O(1) operation for how often this function is called. 
	//If not in the set, just hit the trampoline to ensure default behavior. 
	std::cout << "RUNNING VOICE0" << std::endl;
	std::cout << cl->GetUserID() << std::endl;
	if (afflicted_players.find(cl->GetUserID()) != afflicted_players.end()) {
		//Decompress the stream with vaudio_speex
		//Produces signed 16-bit PCM samples @ 11025 Hz
		//Output is # samples produced.
		int samples = g_pVoiceCodec->Decompress(data, nBytes, (char*)decompressedBuffer, sizeof(decompressedBuffer));
		std::cout << "RUNNING VOICE1" << std::endl;
		//Speex will return a negative (or zeroed) number if decompression fails. 
		if (samples <= 0) {
			//Just hit the trampoline at this point.
			std::cout << "Decompression failed: " << samples << std::endl;
			return detour_BroadcastVoiceData->GetOriginalFunction()(cl, nBytes, data, xuid);
		}

		std::cout << "RUNNING VOICE" << std::endl;

		//Bit crush the stream
		for (int i = 0; i < samples; i++) {
			short* ptr = &decompressedBuffer[i];

			//Signed shorts range from -32768 to 32767
			//Let's quantize that a bit
			float f = (float)*ptr;
			f /= crushFactor;
			*ptr = (short)f;
			*ptr *= crushFactor;
		}

		//Recompress the stream
		int bytesWritten = g_pVoiceCodec->Compress((char*)decompressedBuffer, samples, recompressBuffer, sizeof(recompressBuffer), true);

		//Broadcast voice data with our update compressed data.
		return detour_BroadcastVoiceData->GetOriginalFunction()(cl, bytesWritten, recompressBuffer, xuid);
	}
	else {
		return detour_BroadcastVoiceData->GetOriginalFunction()(cl, nBytes, data, xuid);
	}
}

void LoadSpeex() {
	#ifdef SYSTEM_WINDOWS
		LoadLibrary("bin/win64/vaudio_speex.dll");
	#elif SYSTEM_LINUX
		dlopen("bin/vaudio_speex.so", RTLD_NOW);
	#endif
}

LUA_FUNCTION_STATIC(zsutil_crush) {
	crushFactor = (int)LUA->GetNumber(1);
	return 0;
}

LUA_FUNCTION_STATIC(zsutil_enable8bit) {
	int id = LUA->GetNumber(1);
	bool b = LUA->GetBool(2);

	if (b) {
		afflicted_players.insert(id);
	}
	else {
		afflicted_players.erase(id);
	}

	return 0;
}


GMOD_MODULE_OPEN()
{
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);

	LUA->PushString("zsutil_crush");
	LUA->PushCFunction(zsutil_crush);
	LUA->SetTable(-3);

	LUA->PushString("Enable8Bit");
	LUA->PushCFunction(zsutil_enable8bit);
	LUA->SetTable(-3);

	SourceSDK::FactoryLoader engine_loader("engine");
	SymbolFinder symfinder;

	#ifdef SYSTEM_WINDOWS
		void* sv_bcast = symfinder.FindPattern(engine_loader.GetModule(), GMOD_SV_BroadcastVoice_sym_sig, GMOD_SV_BroadcastVoice_siglen, engine_loader.GetModule());
	#elif SYSTEM_LINUX
		void* sv_bcast = symfinder.FindSymbol(engine_loader.GetModule(), GMOD_SV_BroadcastVoice_sym_sig);
	#endif
	if (sv_bcast == nullptr) {
		std::cout << "Could not locate SV_BrodcastVoice symbol!" << std::endl;
		return 0;
	}

	engine_ptr = engine_loader.GetInterface<IVEngineServer>("VEngineServer021");
	if (engine_ptr == nullptr) {
		std::cout << "Could not locate IVEngineServer!" << std::endl;
		return 0;
	}

	try {
		detour_BroadcastVoiceData = new MologieDetours::Detour<SV_BroadcastVoiceData>((SV_BroadcastVoiceData)sv_bcast, hook_BroadcastVoiceData);
	} catch (...) {
		std::cout << "SV_BroadcastVoiceData Detour failed!" << std::endl;
		return 0;
	}

	LoadSpeex();
	
	SourceSDK::FactoryLoader speex("vaudio_speex");
	if (speex.GetModule() == nullptr) {
		std::cout << "Could not load Speex!" << std::endl;
		return 0;
	}

	IVoiceCodec* codec = speex.GetInterface<IVoiceCodec>("vaudio_speex");
	bool res = codec->Init(4);
	std::cout << "INIT STATUS: " << res << std::endl;
	g_pVoiceCodec = codec;

	afflicted_players = std::unordered_set<int>();

	//If we want to use the speex codec, we need to disable steam voice to force it to be used by clients.
	engine_ptr->GMOD_RawServerCommand("sv_use_steam_voice 0");
	return 0;
}

GMOD_MODULE_CLOSE()
{
	delete detour_BroadcastVoiceData;
	g_pVoiceCodec->Release();
	g_pVoiceCodec = nullptr;
	engine_ptr = nullptr;
	return 0;
}