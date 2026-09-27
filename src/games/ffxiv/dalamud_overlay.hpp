/*
 * Copyright (C) 2026 RenoDX contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <bcrypt.h>
#include <d3d11.h>
#include <intrin.h>
#include <Windows.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <include/reshade.hpp>

#include "../../mods/swapchain.hpp"
#include "../../utils/detour.hpp"
#include "../../utils/log.hpp"
#include "../../utils/resource.hpp"
#include "./dalamud_roundtrip.hpp"

#pragma comment(lib, "bcrypt.lib")

namespace renodx::games::ffxiv::dalamud_overlay {

namespace api = reshade::api;
namespace log = renodx::utils::log;
using Microsoft::WRL::ComPtr;
using Overlay = void (*)(api::effect_runtime*);
using Event = reshade::addon_event;
using RegisterEvent = void (*)(void*, Event, void*);
using UnregisterAddon = void (*)(void*);
using DrawIndexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);

// This adapter deliberately supports one inspected ReShade binary, not a guessed
// std::vector ABI. 6.8.0.2155 x64, SHA256 below, PE timestamp 0x6A6F6BB3.
// RegisterEventForAddon @ 40CB0 appends to addon_event_list @ 49BDD0 (24-byte
// entries), and to the owner's separate event_callbacks. draw_gui @ D3783
// invokes event 81 from 49C568. UnregisterEventForAddon @ 410C0 erases from both.
// Only the dispatch pointer is exchanged: the ORIGINAL managed thunk remains in
// the ownership record. Restore it BEFORE native unregister and on our unload.
// No erase/reinsert, owner reassignment, delegate-code patch, or CLR inspection.
// ReShade requires event registration/unregistration and addon unload to be
// quiescent with dispatch; this adapter retains that existing lifecycle contract.
struct DispatchList {
  void** begin;
  void** end;
  void** capacity;
};
static_assert(sizeof(DispatchList) == 24);
static_assert(static_cast<unsigned>(Event::reshade_overlay) == 81);

inline HMODULE reshade_module = nullptr;
inline DispatchList* overlay_list = nullptr;
inline decltype(&GetModuleHandleExW)* owner_resolver = nullptr;
inline RegisterEvent register_event = nullptr;
inline RegisterEvent unregister_event = nullptr;
inline UnregisterAddon unregister_addon = nullptr;
inline Overlay original_overlay = nullptr;
inline HMODULE owner = nullptr;
inline std::recursive_mutex registration_mutex;
inline DrawIndexed draw_indexed = nullptr;
inline void* draw_indexed_address = nullptr;
inline bool native_hooks = false;
inline bool attached = false;
inline std::atomic_uint native_diagnostic_budget = 128;

struct Runtime {
  api::effect_runtime* runtime = nullptr;
  ComPtr<ID3D11DeviceContext> context;
  BackgroundRoundTrip round_trip;
  api::color_space color_space = api::color_space::unknown;
  uint64_t frame = 0;
  uint64_t effects_frame = 0;
  uint64_t entered_frame = 0;
  uint64_t exited_frame = 0;
  unsigned trace_frames = 3;
  unsigned draws = 0;
  unsigned main_draws = 0;
  uint64_t presentation = 0;
  uint64_t buffer_zero = 0;
  uint64_t native_buffer_zero = 0;
  uint64_t proxy = 0;
  uint64_t presentation_identity = 0;
  uint64_t buffer_zero_identity = 0;
  uint64_t native_buffer_zero_identity = 0;
  uint64_t proxy_identity = 0;
  std::array<uint64_t, 16> seen_presentations = {};
  unsigned presentation_count = 0;
  std::array<ID3D11RenderTargetView*, 32> seen_views = {};
  unsigned seen_count = 0;
  bool native_supported = false;
  bool post_draw_seen = false;
  bool drawing_observed = false;
};
inline std::array<Runtime, 8> runtimes;
inline thread_local Runtime* active = nullptr;
inline thread_local Runtime* after_overlay = nullptr;
inline thread_local bool observing = false;

inline Runtime* FindRuntime(api::effect_runtime* runtime) {
  for (auto& state : runtimes) {
    if (state.runtime == runtime) return &state;
  }
  return nullptr;
}

inline bool SupportedReShade(HMODULE module) {
  if (module == nullptr) return false;
  auto* base = reinterpret_cast<uint8_t*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
      || nt->FileHeader.TimeDateStamp != 0x6A6F6BB3 || nt->OptionalHeader.SizeOfImage != 0x59A000) return false;
  if (GetProcAddress(module, "ReShadeRegisterEventForAddon") != reinterpret_cast<FARPROC>(base + 0x40CB0)
      || GetProcAddress(module, "ReShadeUnregisterEventForAddon") != reinterpret_cast<FARPROC>(base + 0x410C0)
      || GetProcAddress(module, "ReShadeUnregisterAddon") != reinterpret_cast<FARPROC>(base + 0x40930)) return false;
  // Check the loaded image too: fail closed if these instructions were replaced.
  constexpr uint8_t registration[] = {0x48, 0x8D, 0x05, 0x81, 0xB0, 0x45, 0x00};
  constexpr uint8_t unregistration[] = {0x4C, 0x8D, 0x2D, 0x5F, 0xAC, 0x45, 0x00};
  constexpr uint8_t dispatch[] = {0x48, 0x8B, 0x05, 0xF7, 0x8D, 0x3C, 0x00, 0x48, 0x2B, 0xF8,
                                  0x48, 0xC1, 0xFF, 0x03, 0x48, 0x85, 0xFF, 0x74, 0x1A};
  if (std::memcmp(base + 0x40D48, registration, sizeof(registration)) != 0
      || std::memcmp(base + 0x4116A, unregistration, sizeof(unregistration)) != 0
      || std::memcmp(base + 0xD376A, dispatch, sizeof(dispatch)) != 0) return false;

  wchar_t path[MAX_PATH] = {};
  if (GetModuleFileNameW(module, path, MAX_PATH) == 0) return false;
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size = {};
  HANDLE mapping = nullptr;
  void* bytes = nullptr;
  std::array<uint8_t, 32> digest = {};
  bool valid = false;
  if (GetFileSizeEx(file, &size) && size.QuadPart == 5592064
      && (mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr)) != nullptr
      && (bytes = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)) != nullptr) {
    constexpr std::array<uint8_t, 32> expected = {
        0x0C, 0xEE, 0x63, 0xF9, 0xC9, 0xF1, 0x3F, 0x3A, 0xC9, 0x09, 0xC5, 0xB4, 0x90, 0x3F, 0x4D, 0xBB,
        0x4B, 0x71, 0x9A, 0x7A, 0xB3, 0xB4, 0xF1, 0x3B, 0x0D, 0xEA, 0xF8, 0x3C, 0x81, 0x4B, 0x94, 0xF7};
    valid = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, static_cast<PUCHAR>(bytes),
                       static_cast<ULONG>(size.QuadPart), digest.data(), static_cast<ULONG>(digest.size()))
                >= 0
            && digest == expected;
  }
  if (bytes != nullptr) UnmapViewOfFile(bytes);
  if (mapping != nullptr) CloseHandle(mapping);
  CloseHandle(file);
  return valid;
}

inline bool SupportedOwner(HMODULE module) {
  wchar_t path[MAX_PATH] = {};
  if (module == nullptr || GetModuleFileNameW(module, path, MAX_PATH) == 0) return false;
  const auto* filename = wcsrchr(path, L'\\');
  if (filename == nullptr || _wcsicmp(filename + 1, L"Dalamud.dll") != 0) return false;
  // Read the standard VERSIONINFO resource of the resolved assembly module.
  HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));
  if (resource == nullptr || SizeofResource(module, resource) < 92) return false;
  const auto* data = static_cast<const uint8_t*>(LockResource(LoadResource(module, resource)));
  if (data == nullptr || std::memcmp(data + 6, L"VS_VERSION_INFO", 32) != 0) return false;
  const auto* version = reinterpret_cast<const VS_FIXEDFILEINFO*>(data + 40);
  return version->dwSignature == 0xFEEF04BD && version->dwFileVersionMS == 0x000F0000
         && version->dwFileVersionLS == 0x00030005;
}

inline void OverlayCallback(api::effect_runtime* runtime);

inline bool ValidList() {
  const auto begin = reinterpret_cast<uintptr_t>(overlay_list->begin);
  const auto end = reinterpret_cast<uintptr_t>(overlay_list->end);
  const auto capacity = reinterpret_cast<uintptr_t>(overlay_list->capacity);
  if (begin == 0) return end == 0 && capacity == 0;
  if (end < begin || capacity < end || capacity - begin > 4096 * sizeof(void*)
      || (begin | end | capacity) % alignof(void*) != 0) return false;
  MEMORY_BASIC_INFORMATION memory = {};
  return VirtualQuery(overlay_list->begin, &memory, sizeof(memory)) == sizeof(memory)
         && memory.State == MEM_COMMIT && (memory.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) != 0
         && capacity <= reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
}

inline void RestoreDispatch() {
  if (original_overlay == nullptr) return;
  if (!ValidList()) throw std::runtime_error("Dalamud dispatch list changed ABI while interception was active");
  for (auto** slot = overlay_list->begin; slot != overlay_list->end; ++slot) {
    InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot),
                                      reinterpret_cast<void*>(original_overlay), reinterpret_cast<void*>(&OverlayCallback));
  }
}

inline void IdentifyCallback() {
  if (!ValidList()) {
    log::w("[FFXIV Dalamud] INACTIVE dispatch-list validation failed");
    return;
  }
  if (original_overlay != nullptr) return;
  void** candidate = nullptr;
  HMODULE candidate_owner = nullptr;
  for (auto** slot = overlay_list->begin; slot != overlay_list->end; ++slot) {
    HMODULE resolved = nullptr;
    // Use ReShade's CURRENT import, including Dalamud's own delegate resolver.
    // Calling our ordinary GetModuleHandleExW would miss the managed thunk.
    if (!(*owner_resolver)(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(*slot), &resolved)
        || !SupportedOwner(resolved)) continue;
    if (candidate != nullptr) {
      log::w("[FFXIV Dalamud] INACTIVE ambiguous Dalamud overlay registrations");
      return;
    }
    candidate = slot;
    candidate_owner = resolved;
  }
  if (candidate == nullptr) return;
  owner = candidate_owner;
  original_overlay = reinterpret_cast<Overlay>(*candidate);
  if (InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(candidate), reinterpret_cast<void*>(&OverlayCallback),
                                        reinterpret_cast<void*>(original_overlay))
      != reinterpret_cast<void*>(original_overlay)) {
    original_overlay = nullptr;
    owner = nullptr;
    return;
  }
  log::i("[FFXIV Dalamud] CALLBACK_FOUND version=15.0.3.5 owner=", owner,
         " thunk=", reinterpret_cast<void*>(original_overlay), " event=81 index=", candidate - overlay_list->begin,
         " ownership=original registration_order=unchanged (mode-3 drawing requires native draw evidence)");
}

inline void RegisterEventHook(void* module, Event event, void* callback) {
  if (event != Event::reshade_overlay) {
    register_event(module, event, callback);
    return;
  }
  const std::lock_guard lock(registration_mutex);
  // Let ReShade and Dalamud resolve ownership and register the ORIGINAL first.
  RestoreDispatch();
  original_overlay = nullptr;
  owner = nullptr;
  register_event(module, event, callback);
  IdentifyCallback();
}

inline void UnregisterEventHook(void* module, Event event, void* callback) {
  if (event != Event::reshade_overlay) {
    unregister_event(module, event, callback);
    return;
  }
  const std::lock_guard lock(registration_mutex);
  const bool was_dalamud = callback == reinterpret_cast<void*>(original_overlay);
  RestoreDispatch();
  if (was_dalamud) {
    for (auto& state : runtimes) state.round_trip.Reset("callback_unregister");
  }
  unregister_event(module, event, callback);
  // An explicit wrong owner is allowed to fail just as it would without us.
  original_overlay = nullptr;
  owner = nullptr;
  if (was_dalamud) log::i("[FFXIV Dalamud] CALLBACK_UNREGISTER original thunk restored before native unregister");
  IdentifyCallback();
}

inline void UnregisterAddonHook(void* module) {
  const std::lock_guard lock(registration_mutex);
  if (module == owner) {
    RestoreDispatch();
    for (auto& state : runtimes) state.round_trip.Reset("owner_unregister");
    original_overlay = nullptr;
    owner = nullptr;
    log::i("[FFXIV Dalamud] OWNER_UNREGISTER original dispatch restored; native ownership cleanup follows");
  }
  unregister_addon(module);
}

inline uint64_t Identity(IUnknown* object) {
  ComPtr<IUnknown> identity;
  if (object != nullptr) object->QueryInterface(IID_PPV_ARGS(&identity));
  return reinterpret_cast<uint64_t>(identity.Get());
}

inline void ObserveTarget(Runtime* state, ID3D11DeviceContext* context) {
  if (!state->drawing_observed) {
    state->drawing_observed = true;
    state->trace_frames = 3;
  }
  ComPtr<ID3D11RenderTargetView> view;
  context->OMGetRenderTargets(1, &view, nullptr);
  if (view == nullptr) return;
  ComPtr<ID3D11Resource> resource;
  view->GetResource(&resource);
  const uint64_t identity = Identity(resource.Get());
  if (identity != 0 && identity == state->presentation_identity) ++state->main_draws;
  for (unsigned i = 0; i < state->seen_count; ++i) {
    if (state->seen_views[i] == view.Get()) return;
  }
  if (state->seen_count == state->seen_views.size()) return;
  state->seen_views[state->seen_count++] = view.Get();  // Identity only; never retain a backbuffer reference.
  unsigned budget = native_diagnostic_budget.load();
  while (budget != 0 && !native_diagnostic_budget.compare_exchange_weak(budget, budget - 1)) {
  }
  if (budget == 0) return;
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11Device> device;
  resource->GetDevice(&device);
  D3D11_TEXTURE2D_DESC desc = {};
  D3D11_RENDER_TARGET_VIEW_DESC view_desc = {};
  if (SUCCEEDED(resource.As(&texture))) texture->GetDesc(&desc);
  view->GetDesc(&view_desc);
  log::i("[FFXIV Dalamud] NATIVE_DRAW runtime=", state->runtime, " frame=", state->frame,
         " device=", device.Get(), " immediate_context=", context, " context_type=", context->GetType(),
         " rtv=", view.Get(), " resource=", resource.Get(), " resource_iunknown=", log::AsPtr(identity), " format=", static_cast<unsigned>(desc.Format),
         " dimensions=", desc.Width, "x", desc.Height, " samples=", desc.SampleDesc.Count,
         " rtv_format=", static_cast<unsigned>(view_desc.Format),
         " matches_presentation=", identity != 0 && identity == state->presentation_identity,
         " matches_buffer0=", identity != 0 && identity == state->buffer_zero_identity,
         " matches_native_buffer0=", identity != 0 && identity == state->native_buffer_zero_identity,
         " matches_proxy=", identity != 0 && identity == state->proxy_identity);
}

inline void STDMETHODCALLTYPE DrawIndexedHook(ID3D11DeviceContext* context, UINT count, UINT start, INT base) {
  if (!observing && active != nullptr && active->context.Get() == context) {
    observing = true;
    ++active->draws;
    ObserveTarget(active, context);
    observing = false;
  } else if (!observing && active == nullptr && after_overlay != nullptr
             && after_overlay->context.Get() == context && !after_overlay->post_draw_seen) {
    HMODULE caller = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(_ReturnAddress()), &caller);
    if (caller == reshade_module) {
      after_overlay->post_draw_seen = true;
      if (after_overlay->trace_frames != 0) {
        log::i("[FFXIV Dalamud] RESHADE_NATIVE_DRAW_AFTER_EXIT runtime=", after_overlay->runtime,
               " frame=", after_overlay->frame, " context=", context, " caller=", _ReturnAddress());
      }
    }
  }
  draw_indexed(context, count, start, base);
}

inline const std::array native_detours = {
    renodx::utils::detour::Function(&draw_indexed, &DrawIndexedHook)};
inline const std::array export_detours = {
    renodx::utils::detour::Export("ReShadeRegisterEventForAddon", &register_event, &RegisterEventHook),
    renodx::utils::detour::Export("ReShadeUnregisterEventForAddon", &unregister_event, &UnregisterEventHook),
    renodx::utils::detour::Export("ReShadeUnregisterAddon", &unregister_addon, &UnregisterAddonHook)};

inline void OnInitRuntime(api::effect_runtime* runtime) {
  if (runtime->get_device()->get_api() != api::device_api::d3d11) return;
  for (auto& state : runtimes) {
    if (state.runtime != nullptr) continue;
    state.runtime = runtime;
    reinterpret_cast<ID3D11Device*>(runtime->get_device()->get_native())->GetImmediateContext(&state.context);
    auto** table = *reinterpret_cast<void***>(state.context.Get());
    if (!native_hooks) {
      draw_indexed_address = table[12];
      draw_indexed = reinterpret_cast<DrawIndexed>(draw_indexed_address);
      try {
        renodx::utils::detour::Install(native_detours);
        native_hooks = true;
      } catch (const std::exception& error) {
        log::w("[FFXIV Dalamud] NATIVE_INACTIVE ", error.what());
      }
    }
    state.native_supported = native_hooks && table[12] == draw_indexed_address;
    log::i("[FFXIV Dalamud] RUNTIME_INIT runtime=", runtime, " native_swapchain=", reinterpret_cast<void*>(runtime->get_native()),
           " device=", reinterpret_cast<void*>(runtime->get_device()->get_native()), " immediate_context=", state.context.Get(),
           " native_supported=", state.native_supported);
    return;
  }
  log::w("[FFXIV Dalamud] NATIVE_INACTIVE runtime capacity exceeded");
}

inline void OnDestroyRuntime(api::effect_runtime* runtime) {
  if (auto* state = FindRuntime(runtime)) {
    if (after_overlay == state) after_overlay = nullptr;
    log::i("[FFXIV Dalamud] RUNTIME_DESTROY runtime=", runtime);
    state->round_trip.Reset("runtime_destroy");
    *state = {};
  }
}

inline void OnInitSwapchain(api::swapchain* swapchain, bool resize) {
  for (auto& state : runtimes) {
    if (state.runtime == nullptr || state.runtime->get_native() != swapchain->get_native()) continue;
    state.round_trip.Reset("swapchain_init");
    state.presentation = state.buffer_zero = state.native_buffer_zero = state.proxy = 0;
    state.seen_count = state.presentation_count = 0;
    state.drawing_observed = false;
    state.trace_frames = 3;
    log::i("[FFXIV Dalamud] SWAPCHAIN_RESET runtime=", state.runtime, " resize=", resize);
  }
}

inline void OnDestroySwapchain(api::swapchain* swapchain, bool resize) {
  for (auto& state : runtimes) {
    if (state.runtime == nullptr || state.runtime->get_native() != swapchain->get_native()) continue;
    // destroy_swapchain is dispatched BEFORE the underlying ResizeBuffers.
    state.round_trip.Reset(resize ? "before_resize" : "swapchain_destroy");
    state.presentation = 0;
  }
}

inline bool OnOpenOverlay(api::effect_runtime* runtime, bool open, api::input_source) {
  if (auto* state = FindRuntime(runtime)) {
    state->trace_frames = 3;
    log::i("[FFXIV Dalamud] TRACE_REARM runtime=", runtime, " reshade_gui_open=", open);
  }
  return false;
}

inline void OnPresent(api::swapchain* swapchain) {
  for (auto& state : runtimes) {
    if (state.runtime == nullptr || state.runtime->get_native() != swapchain->get_native()) continue;
    state.color_space = swapchain->get_color_space();
    ++state.frame;
    state.draws = state.main_draws = 0;
    state.post_draw_seen = false;
    if (after_overlay == &state) after_overlay = nullptr;
    if (original_overlay != nullptr && state.trace_frames != 0) {
      auto* data = renodx::utils::data::Get<renodx::mods::swapchain::DeviceData>(swapchain->get_device());
      log::i("[FFXIV Dalamud] AFTER_RENODX_PRESENT runtime=", state.runtime, " frame=", state.frame,
             " proxy_pass_available=", data != nullptr && data->swapchain_proxy_passes.contains(swapchain->get_current_back_buffer().handle));
    }
  }
}

inline void OnFinishEffects(api::effect_runtime* runtime, api::command_list*, api::resource_view rtv, api::resource_view) {
  if (auto* state = FindRuntime(runtime)) {
    state->effects_frame = state->frame;
    if (original_overlay != nullptr && state->trace_frames != 0) {
      log::i("[FFXIV Dalamud] EFFECTS_FINISHED runtime=", runtime, " frame=", state->frame,
             " rtv=", log::AsPtr(rtv.handle));
    }
  }
}

inline void OnReShadePresent(api::effect_runtime* runtime) {
  if (auto* state = FindRuntime(runtime)) {
    if (original_overlay != nullptr && state->trace_frames != 0) {
      log::i("[FFXIV Dalamud] RESHADE_PRESENT runtime=", runtime, " frame=", state->frame,
             " entered=", state->entered_frame == state->frame, " exited=", state->exited_frame == state->frame,
             " reshade_native_draw_after_exit=", state->post_draw_seen,
             " (after GUI; before underlying Present; OptiScaler handoff requires external capture)");
      --state->trace_frames;
    }
    if (after_overlay == state) after_overlay = nullptr;
  }
}

inline void OverlayCallback(api::effect_runtime* runtime) {
  // Preserve nested invocations and the exact x64 void(effect_runtime*) ABI.
  if (active != nullptr) {
    original_overlay(runtime);
    return;
  }
  auto* state = FindRuntime(runtime);
  if (state == nullptr || !state->native_supported) {
    original_overlay(runtime);
    return;
  }
  const uint64_t presentation = runtime->get_current_back_buffer().handle;
  if (state->presentation != presentation) {
    state->presentation = presentation;
    state->buffer_zero = runtime->get_back_buffer(0).handle;
    ComPtr<ID3D11Resource> native_buffer;
    reinterpret_cast<IDXGISwapChain*>(runtime->get_native())->GetBuffer(0, IID_PPV_ARGS(&native_buffer));
    state->native_buffer_zero = reinterpret_cast<uint64_t>(native_buffer.Get());
    state->proxy = 0;
    if (presentation != 0) {
      renodx::utils::resource::GetResourceInfo(api::resource{presentation}, [&state](const auto& info) {
        state->proxy = info.clone.handle;
      });
    }
    state->presentation_identity = Identity(reinterpret_cast<IUnknown*>(presentation));
    state->buffer_zero_identity = Identity(reinterpret_cast<IUnknown*>(state->buffer_zero));
    state->native_buffer_zero_identity = Identity(native_buffer.Get());
    state->proxy_identity = Identity(reinterpret_cast<IUnknown*>(state->proxy));
    // Buffer rotation must not produce one log per frame indefinitely.
    bool seen = false;
    for (unsigned i = 0; i < state->presentation_count; ++i) seen |= state->seen_presentations[i] == presentation;
    if (!seen && state->presentation_count < state->seen_presentations.size()) {
      state->seen_presentations[state->presentation_count++] = presentation;
      const auto desc = runtime->get_device()->get_resource_desc(api::resource{presentation});
      log::i("[FFXIV Dalamud] PRESENTATION runtime=", runtime, " native_swapchain=", reinterpret_cast<void*>(runtime->get_native()),
             " hwnd=", runtime->get_hwnd(),
             " current_index=", runtime->get_current_back_buffer_index(), " buffer_count=", runtime->get_back_buffer_count(),
             " format=", static_cast<unsigned>(desc.texture.format), " dimensions=", desc.texture.width, "x", desc.texture.height,
             " current_resource=", log::AsPtr(presentation), " buffer0=", log::AsPtr(state->buffer_zero),
             " native_buffer0=", log::AsPtr(state->native_buffer_zero), " renodx_proxy=", log::AsPtr(state->proxy),
             " presentation_iunknown=", log::AsPtr(state->presentation_identity), " proxy_iunknown=", log::AsPtr(state->proxy_identity));
    }
  }
  state->entered_frame = state->frame;
  active = state;
  if (state->trace_frames != 0) {
    log::i("[FFXIV Dalamud] ENTER runtime=", runtime, " frame=", state->frame,
           " after_renodx_present=", state->frame != 0, " effects_finished_this_frame=", state->effects_frame == state->frame);
  }
  struct ExitScope {
    Runtime* state;
    ~ExitScope() {
      active = nullptr;
      after_overlay = state;
      state->exited_frame = state->frame;
      if (state->trace_frames != 0) {
        log::i("[FFXIV Dalamud] EXIT runtime=", state->runtime, " frame=", state->frame,
               " native_indexed_draws=", state->draws, " presentation_draws=", state->main_draws);
      }
    }
  } exit_scope{state};

  // Restrict the no-UI experiment to the main HDR10 contract proven in the
  // Stage 1 capture. Detached/other-format/multi-buffer runtimes pass through.
  auto* data = renodx::utils::data::Get<renodx::mods::swapchain::DeviceData>(runtime->get_device());
  if (state->frame != 0 && state->color_space == api::color_space::hdr10_st2084
      && data != nullptr && data->primary_swapchain_window == runtime->get_hwnd()
      && data->swapchain_proxy_passes.contains(presentation)
      && runtime->get_back_buffer_count() == 1 && state->presentation_identity != 0
      && state->presentation_identity == state->buffer_zero_identity
      && state->presentation_identity == state->native_buffer_zero_identity
      && state->proxy_identity != 0 && state->presentation_identity != state->proxy_identity) {
    ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(reinterpret_cast<ID3D11Resource*>(presentation)->QueryInterface(IID_PPV_ARGS(&texture)))) {
      struct ObservationGuard {
        bool previous = observing;
        ~ObservationGuard() { observing = previous; }
      } observation_guard;
      observing = true;
      state->round_trip.Run(runtime, state->context.Get(), texture.Get(), state->presentation_identity, state->frame, state->trace_frames != 0);
    } else {
      state->round_trip.Skip("presentation is not Texture2D");
    }
  } else {
    state->round_trip.Skip("outside validated main HDR10 single-buffer contract");
  }
  original_overlay(runtime);
}

inline void Use(DWORD reason) {
  if (reason == DLL_PROCESS_ATTACH) {
    wchar_t executable[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const auto* filename = wcsrchr(executable, L'\\');
    if (filename == nullptr || _wcsicmp(filename + 1, L"ffxiv_dx11.exe") != 0) return;
    reshade_module = reshade::internal::get_reshade_module_handle();
    if (!SupportedReShade(reshade_module)) {
      log::w("[FFXIV Dalamud] INACTIVE unsupported ReShade binary; no interception installed");
      return;
    }
    overlay_list = reinterpret_cast<DispatchList*>(reinterpret_cast<uint8_t*>(reshade_module) + 0x49C568);
    owner_resolver = reinterpret_cast<decltype(owner_resolver)>(reinterpret_cast<uint8_t*>(reshade_module) + 0x3D9068);
    if (!ValidList()) return;
    try {
      if (!renodx::utils::detour::Install(reshade_module, export_detours).Complete()) {
        renodx::utils::detour::Uninstall(export_detours);
        log::w("[FFXIV Dalamud] INACTIVE registration detours unavailable");
        return;
      }
    } catch (const std::exception& error) {
      log::w("[FFXIV Dalamud] INACTIVE ", error.what());
      return;
    }
    attached = true;
    reshade::register_event<Event::init_effect_runtime>(OnInitRuntime);
    reshade::register_event<Event::destroy_effect_runtime>(OnDestroyRuntime);
    reshade::register_event<Event::init_swapchain>(OnInitSwapchain);
    reshade::register_event<Event::destroy_swapchain>(OnDestroySwapchain);
    reshade::register_event<Event::reshade_finish_effects>(OnFinishEffects);
    reshade::register_event<Event::reshade_present>(OnReShadePresent);
    reshade::register_event<Event::reshade_open_overlay>(OnOpenOverlay);
    IdentifyCallback();  // Also handles Dalamud registering before RenoDX loads.
    log::i("[FFXIV Dalamud] ARMED stage=2 background-PQ-roundtrip-only ReShade=6.8.0.2155 SHA256=0CEE63F9C9F13F3AC909C5B4903F4DBB4B719A7AB3B4F13B0DEAF83C814B94F7");
  } else if (reason == DLL_PROCESS_DETACH && attached) {
    const std::lock_guard lock(registration_mutex);
    RestoreDispatch();
    for (auto& state : runtimes) state.round_trip.Reset("addon_detach");
    original_overlay = nullptr;
    owner = nullptr;
    reshade::unregister_event<Event::init_effect_runtime>(OnInitRuntime);
    reshade::unregister_event<Event::destroy_effect_runtime>(OnDestroyRuntime);
    reshade::unregister_event<Event::init_swapchain>(OnInitSwapchain);
    reshade::unregister_event<Event::destroy_swapchain>(OnDestroySwapchain);
    reshade::unregister_event<Event::reshade_finish_effects>(OnFinishEffects);
    reshade::unregister_event<Event::reshade_present>(OnReShadePresent);
    reshade::unregister_event<Event::reshade_open_overlay>(OnOpenOverlay);
    // Do not swallow detach errors and unload executable hook targets. The
    // existing Detours utility throws if it cannot restore its own hooks.
    if (native_hooks) renodx::utils::detour::Uninstall(native_detours);
    renodx::utils::detour::Uninstall(export_detours);
    native_hooks = attached = false;
    active = after_overlay = nullptr;
    for (auto& state : runtimes) state = {};
    log::i("[FFXIV Dalamud] DETACHED dispatch restored; all native hooks removed");
  }
}

}  // namespace renodx::games::ffxiv::dalamud_overlay
