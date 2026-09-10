// main.cpp - ShadowLimitFix SKSE plugin entry
// Goal: remove the engine 4 shadow-casting light limit, up to 127 true
// shadow lights. Standalone DLL, no CS / no ENB dependency, ENB coexist.
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <Windows.h>
#include <dbghelp.h>

#include "HookUtil.h"

#pragma comment(lib, "dbghelp.lib")

namespace ShadowLimitFixNS
{
	void Install();
	// Self-owned shadow atlas (ShadowAtlas.cpp). Created at kDataLoaded where
	// the D3D11 device exists (PostLoad's forwarder is still null).
	void InstallShadowAtlas();
#if ENABLE_P1B
	// fix9 (23:3x): material-pass probe moved here from Install() (which runs
	// at kPostLoad when BSGraphics::Renderer::GetRuntimeData().context is
	// still null - the probe silently returned in fix8 and logged nothing).
	// kDataLoaded fires after the renderer/D3D context exist, so the vtable
	// hook can actually install.
	namespace P1
	{
		void InstallMaterialPassProbe();
	}
#endif
}

// fix17c (2026-09-04 01:3x): load-path trace. The 01:25 session crashed /
// exited before SKSEPlugin_Load ran (ShadowLimitFix.log was never re-created)
// while EngineFixes PreLoad completed. spdlog is unavailable that early and
// skse64.log buffers are lost on hard exits, so trace every SKSE entry point
// to a raw append file. Next run tells us exactly how far load got:
//   trace line "Query"     -> SKSE enumerated us and called Query
//   trace line "Load"      -> SKSE accepted us and called Load (SKSE::Init next)
//   trace line "Init done" -> spdlog alive; ShadowLimitFix.log must exist
// If even "Query" is missing, SKSE never enumerated this DLL (VFS/modlist) or
// died earlier in the plugin load loop (another plugin's Load).
static void TraceLoad(const char* a_stage)
{
	if (auto dir = SKSE::log::log_directory(); dir) {
		const auto p = *dir / "ShadowLimitFix_trace.txt";
		if (auto* f = std::fopen(p.string().c_str(), "a")) {
			std::fprintf(f, "[%08X] %s\n", GetCurrentThreadId(), a_stage);
			std::fclose(f);
		}
	}
}

// Global exception filter: capture the crash call stack that CrashLogger
// misses (hard crashes in native code / during DLL teardown).
// NOTE: registered as UEF only. A VEH was tried but our handler re-entered
// on a secondary exception (fopen/fprintf inside a VEH with FIRST priority
// recursed until a 60MB log), so VEH is DISABLED - rely on CrashLogger's
// dump instead.
LONG WINAPI SLFExceptionFilter(EXCEPTION_POINTERS* a_ep)
{
	// Write to a separate file - spdlog may be unsafe mid-crash.
	// NOTE: resolve the SKSE log dir (OneDrive/MO2-safe); a bare relative
	// "Data/SKSE/..." fails under MO2 (virtual Data dir).
	if (auto dir = SKSE::log::log_directory(); dir) {
		const auto p = *dir / "ShadowLimitFix_crash.txt";
		if (auto* f = std::fopen(p.string().c_str(), "a")) {
			std::fprintf(f, "[SLF] EXCEPTION code=%08X at %p thread=%lu\n",
				static_cast<unsigned>(a_ep->ExceptionRecord->ExceptionCode),
				a_ep->ExceptionRecord->ExceptionAddress,
				GetCurrentThreadId());
			if (a_ep->ContextRecord) {
				auto* c = a_ep->ContextRecord;
				std::fprintf(f, "  RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\n", c->Rax, c->Rbx, c->Rcx, c->Rdx);
				std::fprintf(f, "  RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX\n", c->Rsi, c->Rdi, c->Rbp, c->Rsp);
				std::fprintf(f, "  R8 =%016llX R9 =%016llX R10=%016llX R11=%016llX\n", c->R8, c->R9, c->R10, c->R11);
				std::fprintf(f, "  R12=%016llX R13=%016llX R14=%016llX R15=%016llX\n", c->R12, c->R13, c->R14, c->R15);
			}
			std::fclose(f);
		}
	}
	return EXCEPTION_CONTINUE_SEARCH;  // let CrashLogger / default handler also run
}

#define DLLEXPORT __declspec(dllexport)

// Earliest possible trace point: CRT static-init runs inside LoadLibrary,
// before SKSE ever calls Query. If even this line is missing, the DLL was
// never mapped (VFS/modlist) or the process died before plugin loading.
namespace
{
	struct LoadTraceInit
	{
		LoadTraceInit() { TraceLoad("static-init"); }
	};
	LoadTraceInit g_loadTraceInit;
}

// Version declaration (iron rule: UsesAddressLibrary + UsesUpdatedStructs,
// never UsesNoStructs / CompatibleVersions - SKSE 2.2.6 fatal)
extern "C" DLLEXPORT constinit auto SKSEPlugin_Version = []() noexcept {
	SKSE::PluginVersionData v;
	v.PluginVersion(1);
	v.PluginName("ShadowLimitFix");
	v.UsesAddressLibrary();
	v.UsesUpdatedStructs();
	return v;
}();

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface*, SKSE::PluginInfo* a_info)
{
	TraceLoad("Query");
	a_info->infoVersion = SKSE::PluginInfo::kVersion;
	a_info->name = SKSEPlugin_Version.pluginName;
	a_info->version = SKSEPlugin_Version.pluginVersion;
	return true;
}

namespace
{
	void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
			case SKSE::MessagingInterface::kPostLoad:
				TraceLoad("PostLoad");
				SKSE::log::info("[SLF] PostLoad - installing hooks");
				ShadowLimitFixNS::Install();
				break;
			case SKSE::MessagingInterface::kDataLoaded:
				TraceLoad("DataLoaded");
				SKSE::log::info("[SLF] DataLoaded");
				// fix108 (2026-09-10): the self-owned shadow atlas needs the D3D11
				// device, which exists by kDataLoaded (PostLoad's forwarder is
				// still null). Step 1 = create the texture + tiles only; no
				// render/shader wiring yet.
				ShadowLimitFixNS::InstallShadowAtlas();
#if ENABLE_P1B
				// fix18 (2026-09-04): material-pass probe DISABLED. It is the
				// ONLY SLF code that runs on the render worker thread
				// (36564/38392). Evidence: crash-2026-09-04-01-46-41 AV'd in
				// the probe's roster dump; fix17d SEH-guarded those reads
				// ('?@' never fired -> reads succeeded on freed-but-mapped
				// memory, all-empty accum) but the session STILL died with a
				// clean exit (no WER/CrashLogger dump) right after the worker
				// thread's material-pass probe -> crash moved past the probe
				// into engine material code using cell-change-freed lights.
				// Pure-observation code with a timing side effect; ship fix18
				// without it. Re-enable only with a main-thread/low-rate
				// sampling strategy. Shadow rendering (P1b dispatch + 127
				// slices) is UNAFFECTED.
				// ShadowLimitFixNS::P1::InstallMaterialPassProbe();
#endif
				break;
			default:
				break;
		}
	}
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	TraceLoad("Load");
	// SKSE::Init auto-inits spdlog (ShadowLimitFix.log)
	SKSE::Init(a_skse);
	TraceLoad("Init done");
	SKSE::log::info("[SLF] ShadowLimitFix loaded");

	// Allocate trampoline pool for install_context_hook (P1b depth-buffer
	// redirects etc.). Without this the pool is empty and any trampoline
	// allocation fails with "Failed to handle allocation request".
	// CS uses 1 << 12 (4KB); bump if P1b needs more.
	SKSE::AllocTrampoline(1 << 12);
	SKSE::log::info("[SLF] trampoline pool allocated (4KB)");

	// Global crash capture (writes Data/SKSE/ShadowLimitFix_crash.txt).
	// UEF only - VEH re-entered on secondary exceptions (60MB recursion log,
	// crash-2026-09-02-02-36). CrashLogger's dump is the primary evidence.
	SetUnhandledExceptionFilter(SLFExceptionFilter);
	SKSE::log::info("[SLF] crash filter installed");

	auto* messaging = SKSE::GetMessagingInterface();
	if (!messaging) {
		SKSE::log::error("[SLF] messaging interface missing");
		return false;
	}
	messaging->RegisterListener("SKSE", MessageHandler);
	return true;
}
