/*
 * Copyright (C) 2026 RenoDX contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <bcrypt.h>
#include <d3d11_1.h>
#include <Windows.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <embed/0xC046E20B.h>
#include <include/reshade.hpp>

#include "../../mods/swapchain.hpp"
#include "../../utils/detour.hpp"
#include "../../utils/log.hpp"
#include "../../utils/resource.hpp"

#pragma comment(lib, "bcrypt.lib")

namespace renodx::games::ffxiv::dalamud_overlay {

namespace api = reshade::api;
namespace log = renodx::utils::log;
using Microsoft::WRL::ComPtr;
using Overlay = void (*)(api::effect_runtime*);
using Event = reshade::addon_event;
using RegisterEvent = void (*)(void*, Event, void*);
using UnregisterAddon = void (*)(void*);
using PSSetShader = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
using PSSetShaderResources = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using CreatePixelShader = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
using CreateVertexShader = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);

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
inline PSSetShader ps_set_shader = nullptr;
inline void* ps_set_shader_address = nullptr;
inline PSSetShaderResources ps_set_shader_resources = nullptr;
inline void* ps_set_shader_resources_address = nullptr;
inline CreatePixelShader create_pixel_shader = nullptr;
inline CreateVertexShader create_vertex_shader = nullptr;
inline void* create_pixel_shader_address = nullptr;
inline void* create_vertex_shader_address = nullptr;
inline bool native_hooks = false;
inline bool attached = false;
struct Runtime {
  api::effect_runtime* runtime = nullptr;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device> device;
  struct DirectPQ {
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<ID3D11PixelShader> shader;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11PixelShader> original_shader;
    ComPtr<ID3D11Buffer> original_constants;
    UINT first_constant = 0;
    UINT constant_count = 0;
    bool bound = false;
    bool attempted = false;
    HRESULT result = S_OK;
  } direct_pq;
  float ui_nits = 203.f;
  bool correction_candidate = false;
  api::color_space color_space = api::color_space::unknown;
  bool presented = false;
  bool direct_pq_enabled = false;
  uint64_t presentation = 0;
  uint64_t buffer_zero = 0;
  uint64_t native_buffer_zero = 0;
  uint64_t proxy = 0;
  uint64_t presentation_identity = 0;
  uint64_t buffer_zero_identity = 0;
  uint64_t native_buffer_zero_identity = 0;
  uint64_t proxy_identity = 0;
  bool native_supported = false;
};
inline std::array<Runtime, 8> runtimes;
inline thread_local Runtime* active = nullptr;
inline thread_local bool observing = false;

// Tags are assigned only after hashing the original bytecode at creation on
// this runtime's native device. No shader-pointer guessing or CLR inspection.
inline constexpr GUID imgui_fingerprint = {0x6825f779, 0xebc4, 0x4cec, {0xb7, 0x4a, 0xac, 0x38, 0x3e, 0x25, 0x99, 0x03}};
inline constexpr UINT imgui_ps_tag = 0x15003502;
inline constexpr UINT imgui_vs_tag = 0x15003501;

inline void RestoreDirectPQ(Runtime* state) {
  auto& direct = state->direct_pq;
  if (!direct.bound) return;
  direct.bound = false;
  // Restore only bindings still owned by us. In particular, blur and the
  // renderer's state backup may already have supplied a different PS/b0.
  ComPtr<ID3D11PixelShader> pixel;
  state->context->PSGetShader(&pixel, nullptr, nullptr);
  if (pixel.Get() == direct.shader.Get()) ps_set_shader(state->context.Get(), direct.original_shader.Get(), nullptr, 0);
  ComPtr<ID3D11Buffer> constants;
  direct.context->PSGetConstantBuffers(0, 1, &constants);
  if (constants.Get() == direct.constants.Get()) {
    direct.context->PSSetConstantBuffers1(0, 1, direct.original_constants.GetAddressOf(),
                                          &direct.first_constant, &direct.constant_count);
  }
  direct.original_shader.Reset();
  direct.original_constants.Reset();
}

inline void RejectCorrection() {
  if (active != nullptr) {
    active->correction_candidate = false;
    RestoreDirectPQ(active);
  }
}

inline void TagImGuiShader(ID3D11Device* device, const void* bytes, SIZE_T size, ID3D11DeviceChild* shader, UINT tag) {
  if (shader == nullptr || bytes == nullptr || size != (tag == imgui_ps_tag ? 740 : 996)) return;
  bool supported = false;
  for (const auto& state : runtimes) supported |= state.native_supported && state.device.Get() == device;
  if (!supported) return;
  // Pinned 15.0.3.5 Renderers/imgui-{frag,vertex}.hlsl.bytes, SHA256.
  constexpr std::array<uint8_t, 32> pixel_digest = {
      0xB6, 0xE7, 0xF2, 0xFE, 0xB0, 0x0C, 0xC7, 0x87, 0xD3, 0xF0, 0x39, 0xDF, 0xD8, 0xDD, 0x02, 0x72,
      0x0B, 0x59, 0xF7, 0x49, 0x7E, 0xD9, 0x36, 0xB4, 0xFD, 0x4B, 0x85, 0xD1, 0x6E, 0x39, 0x83, 0x4A};
  constexpr std::array<uint8_t, 32> vertex_digest = {
      0xB6, 0x2A, 0x4A, 0xAE, 0xEE, 0x2B, 0x9F, 0x9E, 0xA9, 0xF1, 0x83, 0x7A, 0xA6, 0x0E, 0x75, 0x85,
      0xA0, 0xB2, 0xD6, 0xF1, 0x88, 0x96, 0x62, 0xFE, 0xDF, 0x0C, 0x12, 0x2E, 0x69, 0xD6, 0x57, 0x2E};
  std::array<uint8_t, 32> digest = {};
  if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(static_cast<const UCHAR*>(bytes)),
                 static_cast<ULONG>(size), digest.data(), static_cast<ULONG>(digest.size()))
          >= 0
      && digest == (tag == imgui_ps_tag ? pixel_digest : vertex_digest)) {
    shader->SetPrivateData(imgui_fingerprint, sizeof(tag), &tag);
  }
}

inline HRESULT STDMETHODCALLTYPE CreatePixelShaderHook(ID3D11Device* device, const void* bytes, SIZE_T size,
                                                       ID3D11ClassLinkage* linkage, ID3D11PixelShader** shader) {
  const HRESULT result = create_pixel_shader(device, bytes, size, linkage, shader);
  if (SUCCEEDED(result) && shader != nullptr && linkage == nullptr) TagImGuiShader(device, bytes, size, *shader, imgui_ps_tag);
  return result;
}

inline HRESULT STDMETHODCALLTYPE CreateVertexShaderHook(ID3D11Device* device, const void* bytes, SIZE_T size,
                                                        ID3D11ClassLinkage* linkage, ID3D11VertexShader** shader) {
  const HRESULT result = create_vertex_shader(device, bytes, size, linkage, shader);
  if (SUCCEEDED(result) && shader != nullptr && linkage == nullptr) TagImGuiShader(device, bytes, size, *shader, imgui_vs_tag);
  return result;
}

inline bool IsImGuiShader(ID3D11DeviceChild* shader, UINT expected) {
  UINT tag = 0;
  UINT size = sizeof(tag);
  return shader != nullptr && SUCCEEDED(shader->GetPrivateData(imgui_fingerprint, &size, &tag))
         && size == sizeof(tag) && tag == expected;
}

// Unsupported auxiliary work stops further direct-PQ correction in this callback.
// Restore the active binding before forwarding unsupported work. Earlier direct
// draws cannot be rolled back. Every original operation executes.
// Slot-specific types keep each ABI and trampoline separate.
template <UINT Slot, typename... Args>
struct AuxiliaryOperation {
  using Function = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, Args...);
  static inline Function original = nullptr;
  static inline void* address = nullptr;
  static void STDMETHODCALLTYPE Hook(ID3D11DeviceContext* context, Args... args) {
    if (!observing && active != nullptr) RejectCorrection();
    original(context, args...);
  }
  static renodx::utils::detour::Function Bind(void** table) {
    address = table[Slot];
    original = reinterpret_cast<Function>(address);
    return {&original, &Hook};
  }
  static bool Matches(void** table) { return table[Slot] == address; }
};

using NativeDraw = AuxiliaryOperation<13, UINT, UINT>;
using NativeDrawIndexedInstanced = AuxiliaryOperation<20, UINT, UINT, UINT, INT, UINT>;
using NativeDrawInstanced = AuxiliaryOperation<21, UINT, UINT, UINT, UINT>;
using NativeBeginQuery = AuxiliaryOperation<27, ID3D11Asynchronous*>;
using NativeEndQuery = AuxiliaryOperation<28, ID3D11Asynchronous*>;
using NativeDrawAuto = AuxiliaryOperation<38>;
using NativeDrawIndexedIndirect = AuxiliaryOperation<39, ID3D11Buffer*, UINT>;
using NativeDrawIndirect = AuxiliaryOperation<40, ID3D11Buffer*, UINT>;
using NativeDispatch = AuxiliaryOperation<41, UINT, UINT, UINT>;
using NativeDispatchIndirect = AuxiliaryOperation<42, ID3D11Buffer*, UINT>;
using NativeCopyRegion = AuxiliaryOperation<46, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*>;
using NativeCopy = AuxiliaryOperation<47, ID3D11Resource*, ID3D11Resource*>;
using NativeUpdate = AuxiliaryOperation<48, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT>;
using NativeClear = AuxiliaryOperation<50, ID3D11RenderTargetView*, const FLOAT*>;
using NativeClearUint = AuxiliaryOperation<51, ID3D11UnorderedAccessView*, const UINT*>;
using NativeClearFloat = AuxiliaryOperation<52, ID3D11UnorderedAccessView*, const FLOAT*>;
using NativeClearDepth = AuxiliaryOperation<53, ID3D11DepthStencilView*, UINT, FLOAT, UINT8>;
using NativeGenerateMips = AuxiliaryOperation<54, ID3D11ShaderResourceView*>;
using NativeResolve = AuxiliaryOperation<57, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT>;
using NativeExecute = AuxiliaryOperation<58, ID3D11CommandList*, BOOL>;
using NativeCopyRegion1 = AuxiliaryOperation<115, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*, UINT>;
using NativeUpdate1 = AuxiliaryOperation<116, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT, UINT>;
using NativeDiscardResource = AuxiliaryOperation<117, ID3D11Resource*>;
using NativeDiscardView = AuxiliaryOperation<118, ID3D11View*>;
using NativeClearView = AuxiliaryOperation<132, ID3D11View*, const FLOAT*, const D3D11_RECT*, UINT>;
using NativeDiscardView1 = AuxiliaryOperation<133, ID3D11View*, const D3D11_RECT*, UINT>;
inline std::array<renodx::utils::detour::Function, 30> native_detours;

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
    for (auto& state : runtimes) {
      RestoreDirectPQ(&state);
      state.direct_pq = {};
    }
  }
  unregister_event(module, event, callback);
  // An explicit wrong owner is allowed to fail just as it would without us.
  original_overlay = nullptr;
  owner = nullptr;
  IdentifyCallback();
}

inline void UnregisterAddonHook(void* module) {
  const std::lock_guard lock(registration_mutex);
  if (module == owner) {
    RestoreDispatch();
    for (auto& state : runtimes) {
      RestoreDirectPQ(&state);
      state.direct_pq = {};
    }
    original_overlay = nullptr;
    owner = nullptr;
  }
  unregister_addon(module);
}

inline uint64_t Identity(IUnknown* object) {
  ComPtr<IUnknown> identity;
  if (object != nullptr) object->QueryInterface(IID_PPV_ARGS(&identity));
  return reinterpret_cast<uint64_t>(identity.Get());
}

// SetupRenderState binds PS before the per-command texture and before clearing
// GS/HS/DS. Validate those later at PSSetShaderResources, never at DrawIndexed.
inline bool ValidateDirectPQ(Runtime* state, ID3D11DeviceContext* context, bool texture_bound) {
  if (!state->correction_candidate) return false;
  ComPtr<ID3D11VertexShader> vertex;
  UINT vertex_classes = 0;
  context->VSGetShader(&vertex, nullptr, &vertex_classes);
  if (vertex_classes != 0 || !IsImGuiShader(vertex.Get(), imgui_vs_tag)) {
    RejectCorrection();
    return false;
  }

  std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> views;
  ComPtr<ID3D11DepthStencilView> depth_view;
  // Query separately to avoid depending on ComPtr's representation as an array.
  ID3D11RenderTargetView* raw_views[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
  context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, raw_views, &depth_view);
  bool other_targets = depth_view != nullptr;
  for (unsigned i = 0; i < views.size(); ++i) {
    views[i].Attach(raw_views[i]);
    if (i != 0) other_targets |= views[i] != nullptr;
  }
  ComPtr<ID3D11Resource> resource;
  if (views[0] != nullptr) views[0]->GetResource(&resource);
  D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
  if (views[0] != nullptr) views[0]->GetDesc(&rtv_desc);
  if (other_targets || Identity(resource.Get()) != state->presentation_identity
      || rtv_desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM
      || rtv_desc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || rtv_desc.Texture2D.MipSlice != 0) {
    RejectCorrection();
    return false;
  }
  if (texture_bound) {
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11Buffer> constants;
    UINT pixel_classes = 0;
    context->PSGetShader(&pixel, nullptr, &pixel_classes);
    context->PSGetConstantBuffers(0, 1, &constants);
    if (pixel_classes != 0 || pixel.Get() != state->direct_pq.shader.Get()
        || constants.Get() != state->direct_pq.constants.Get()) {
      RejectCorrection();
      return false;
    }
    ComPtr<ID3D11ShaderResourceView> srv;
    context->PSGetShaderResources(0, 1, &srv);
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    if (srv != nullptr) srv->GetDesc(&srv_desc);
    // No SRGB views (hardware decoding), float/HDR views, or background images.
    // The ordinary source contract is the SDR UNORM vertex/texture product.
    bool format_allowed = false;
    switch (srv_desc.Format) {
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM:
      case DXGI_FORMAT_B8G8R8X8_UNORM:
      case DXGI_FORMAT_B4G4R4A4_UNORM:
      case DXGI_FORMAT_BC1_UNORM:
      case DXGI_FORMAT_BC2_UNORM:
      case DXGI_FORMAT_BC3_UNORM:
      case DXGI_FORMAT_BC7_UNORM:
        format_allowed = true;
        break;
      default: break;
    }
    const bool dimension_allowed = srv_desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
    if (!format_allowed || !dimension_allowed) {
      RejectCorrection();
      return false;
    }
  }
  ComPtr<ID3D11BlendState> blend;
  FLOAT factors[4] = {};
  UINT mask = 0;
  context->OMGetBlendState(&blend, factors, &mask);
  D3D11_BLEND_DESC blend_desc = {};
  if (blend != nullptr) blend->GetDesc(&blend_desc);
  const auto& rt = blend_desc.RenderTarget[0];
  if (blend_desc.AlphaToCoverageEnable || blend_desc.IndependentBlendEnable || mask != 0xFFFFFFFF
      || !rt.BlendEnable || rt.SrcBlend != D3D11_BLEND_SRC_ALPHA || rt.DestBlend != D3D11_BLEND_INV_SRC_ALPHA
      || rt.BlendOp != D3D11_BLEND_OP_ADD || rt.SrcBlendAlpha != D3D11_BLEND_INV_DEST_ALPHA
      || rt.DestBlendAlpha != D3D11_BLEND_ONE || rt.BlendOpAlpha != D3D11_BLEND_OP_ADD
      || rt.RenderTargetWriteMask != D3D11_COLOR_WRITE_ENABLE_ALL) {
    RejectCorrection();
    return false;
  }
  if (texture_bound) {
    // Extra shader stages, predicates and OM UAVs are outside the pinned ordinary
    // renderer contract. Restore the original bindings before submission.
    ComPtr<ID3D11GeometryShader> geometry;
    ComPtr<ID3D11HullShader> hull;
    ComPtr<ID3D11DomainShader> domain;
    ComPtr<ID3D11Predicate> predicate;
    BOOL predicate_value = FALSE;
    context->GSGetShader(&geometry, nullptr, nullptr);
    context->HSGetShader(&hull, nullptr, nullptr);
    context->DSGetShader(&domain, nullptr, nullptr);
    context->GetPredication(&predicate, &predicate_value);
    ID3D11UnorderedAccessView* uavs[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
    context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, D3D11_PS_CS_UAV_REGISTER_COUNT, uavs);
    bool has_uavs = false;
    for (auto* uav : uavs) {
      if (uav != nullptr) {
        has_uavs = true;
        uav->Release();
      }
    }
    if (geometry != nullptr || hull != nullptr || domain != nullptr || predicate != nullptr || has_uavs) {
      RejectCorrection();
      return false;
    }
  }

  // Only the validated single-sample HDR10 presentation target is supported.
  ComPtr<ID3D11Texture2D> texture;
  D3D11_TEXTURE2D_DESC desc = {};
  if (SUCCEEDED(resource.As(&texture))) texture->GetDesc(&desc);
  if (desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM || desc.SampleDesc.Count != 1
      || desc.SampleDesc.Quality != 0 || desc.MipLevels != 1 || desc.ArraySize != 1
      || desc.Width == 0 || desc.Height == 0 || (desc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0
      || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE
      || state->device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
    RejectCorrection();
    return false;
  }
  return true;
}

inline void STDMETHODCALLTYPE PSSetShaderHook(ID3D11DeviceContext* context, ID3D11PixelShader* pixel,
                                              ID3D11ClassInstance* const* classes, UINT class_count) {
  if (observing || active == nullptr) {
    ps_set_shader(context, pixel, classes, class_count);
    return;
  }
  auto* state = active;
  RestoreDirectPQ(state);
  if (context != state->context.Get()) {
    RejectCorrection();
    ps_set_shader(context, pixel, classes, class_count);
    return;
  }
  if (class_count != 0 || !IsImGuiShader(pixel, imgui_ps_tag) || !ValidateDirectPQ(state, context, false)) {
    ps_set_shader(context, pixel, classes, class_count);
    return;
  }
  auto& direct = state->direct_pq;
  observing = true;
  if (!direct.attempted) {
    direct.attempted = true;
    direct.result = context->QueryInterface(IID_PPV_ARGS(&direct.context));
    if (SUCCEEDED(direct.result)) {
      direct.result = state->device->CreatePixelShader(__0xC046E20B.data(), __0xC046E20B.size(), nullptr, &direct.shader);
    }
    if (SUCCEEDED(direct.result)) {
      const CD3D11_BUFFER_DESC constants_desc(16, D3D11_BIND_CONSTANT_BUFFER);
      direct.result = state->device->CreateBuffer(&constants_desc, nullptr, &direct.constants);
    }
    if (FAILED(direct.result)) {
      direct.context.Reset();
      direct.shader.Reset();
      direct.constants.Reset();
    }
  }
  if (FAILED(direct.result)) {
    RejectCorrection();
    observing = false;
    ps_set_shader(context, pixel, classes, class_count);
    return;
  }
  const float white[4] = {state->ui_nits, 0.f, 0.f, 0.f};
  context->UpdateSubresource(direct.constants.Get(), 0, nullptr, white, 0, 0);
  direct.original_shader = pixel;
  direct.context->PSGetConstantBuffers1(0, 1, &direct.original_constants, &direct.first_constant, &direct.constant_count);
  ps_set_shader(context, direct.shader.Get(), nullptr, 0);
  context->PSSetConstantBuffers(0, 1, direct.constants.GetAddressOf());
  direct.bound = true;
  observing = false;
}

inline void STDMETHODCALLTYPE PSSetShaderResourcesHook(ID3D11DeviceContext* context, UINT start, UINT count,
                                                       ID3D11ShaderResourceView* const* views) {
  ps_set_shader_resources(context, start, count, views);
  // The pinned ordinary renderer binds t0 immediately before each command.
  // Keep the SDR texture/stage/target gates without intercepting submissions.
  if (!observing && active != nullptr && active->direct_pq.bound && start == 0 && count != 0) {
    if (context != active->context.Get()) {
      RejectCorrection();
    } else {
      ValidateDirectPQ(active, context, true);
    }
  }
}

inline const std::array export_detours = {
    renodx::utils::detour::Export("ReShadeRegisterEventForAddon", &register_event, &RegisterEventHook),
    renodx::utils::detour::Export("ReShadeUnregisterEventForAddon", &unregister_event, &UnregisterEventHook),
    renodx::utils::detour::Export("ReShadeUnregisterAddon", &unregister_addon, &UnregisterAddonHook)};

inline void OnInitRuntime(api::effect_runtime* runtime) {
  if (runtime->get_device()->get_api() != api::device_api::d3d11) return;
  for (auto& state : runtimes) {
    if (state.runtime != nullptr) continue;
    state.runtime = runtime;
    // Direct PQ is the default; preserve Mode 1 as a restart-only pass-through.
    unsigned mode = 6;
    reshade::get_config_value(nullptr, "renodx-ffxiv-dalamud", "Stage3DiagnosticMode", mode);
    state.direct_pq_enabled = mode != 1;
    reinterpret_cast<ID3D11Device*>(runtime->get_device()->get_native())->GetImmediateContext(&state.context);
    state.context->GetDevice(&state.device);
    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(state.context.As(&context1)) || context1.Get() != state.context.Get()) {
      log::w("[FFXIV Dalamud] NATIVE_INACTIVE unsupported D3D11.1 context identity");
      return;
    }
    auto** table = *reinterpret_cast<void***>(state.context.Get());
    auto** device_table = *reinterpret_cast<void***>(state.device.Get());
    if (!native_hooks) {
      ps_set_shader_address = table[9];
      ps_set_shader = reinterpret_cast<PSSetShader>(ps_set_shader_address);
      ps_set_shader_resources_address = table[8];
      ps_set_shader_resources = reinterpret_cast<PSSetShaderResources>(ps_set_shader_resources_address);
      create_pixel_shader_address = device_table[15];
      create_vertex_shader_address = device_table[12];
      create_pixel_shader = reinterpret_cast<CreatePixelShader>(create_pixel_shader_address);
      create_vertex_shader = reinterpret_cast<CreateVertexShader>(create_vertex_shader_address);
      native_detours = {
          renodx::utils::detour::Function(&ps_set_shader, &PSSetShaderHook),
          renodx::utils::detour::Function(&ps_set_shader_resources, &PSSetShaderResourcesHook),
          renodx::utils::detour::Function(&create_pixel_shader, &CreatePixelShaderHook),
          renodx::utils::detour::Function(&create_vertex_shader, &CreateVertexShaderHook),
          NativeDraw::Bind(table), NativeDrawIndexedInstanced::Bind(table),
          NativeDrawInstanced::Bind(table), NativeDrawAuto::Bind(table),
          NativeBeginQuery::Bind(table), NativeEndQuery::Bind(table),
          NativeDrawIndexedIndirect::Bind(table), NativeDrawIndirect::Bind(table),
          NativeDispatch::Bind(table), NativeDispatchIndirect::Bind(table),
          NativeCopyRegion::Bind(table), NativeCopy::Bind(table),
          NativeUpdate::Bind(table), NativeClearUint::Bind(table),
          NativeClearFloat::Bind(table), NativeClearDepth::Bind(table),
          NativeGenerateMips::Bind(table),
          NativeClear::Bind(table), NativeResolve::Bind(table),
          NativeExecute::Bind(table), NativeCopyRegion1::Bind(table),
          NativeUpdate1::Bind(table), NativeDiscardResource::Bind(table),
          NativeDiscardView::Bind(table), NativeClearView::Bind(table),
          NativeDiscardView1::Bind(table)};
      try {
        renodx::utils::detour::Install(native_detours);
        native_hooks = true;
      } catch (const std::exception& error) {
        log::w("[FFXIV Dalamud] NATIVE_INACTIVE ", error.what());
      }
    }
    state.native_supported = native_hooks && table[9] == ps_set_shader_address && table[8] == ps_set_shader_resources_address
                             && state.device.Get() == reinterpret_cast<ID3D11Device*>(runtime->get_device()->get_native())
                             && device_table[15] == create_pixel_shader_address && device_table[12] == create_vertex_shader_address
                             && NativeDraw::Matches(table) && NativeDrawIndexedInstanced::Matches(table)
                             && NativeDrawInstanced::Matches(table) && NativeDrawAuto::Matches(table)
                             && NativeBeginQuery::Matches(table) && NativeEndQuery::Matches(table)
                             && NativeDrawIndexedIndirect::Matches(table) && NativeDrawIndirect::Matches(table)
                             && NativeDispatch::Matches(table) && NativeDispatchIndirect::Matches(table)
                             && NativeCopyRegion::Matches(table) && NativeCopy::Matches(table) && NativeClear::Matches(table)
                             && NativeUpdate::Matches(table) && NativeClearUint::Matches(table) && NativeClearFloat::Matches(table)
                             && NativeClearDepth::Matches(table) && NativeGenerateMips::Matches(table)
                             && NativeCopyRegion1::Matches(table) && NativeUpdate1::Matches(table)
                             && NativeDiscardResource::Matches(table) && NativeDiscardView::Matches(table)
                             && NativeClearView::Matches(table) && NativeDiscardView1::Matches(table)
                             && NativeResolve::Matches(table) && NativeExecute::Matches(table);
    return;
  }
  log::w("[FFXIV Dalamud] NATIVE_INACTIVE runtime capacity exceeded");
}

inline void OnDestroyRuntime(api::effect_runtime* runtime) {
  if (auto* state = FindRuntime(runtime)) {
    RestoreDirectPQ(state);
    *state = {};
  }
}

inline void OnInitSwapchain(api::swapchain* swapchain, bool) {
  for (auto& state : runtimes) {
    if (state.runtime == nullptr || state.runtime->get_native() != swapchain->get_native()) continue;
    RestoreDirectPQ(&state);
    state.direct_pq = {};
    state.presentation = state.buffer_zero = state.native_buffer_zero = state.proxy = 0;
  }
}

inline void OnDestroySwapchain(api::swapchain* swapchain, bool) {
  for (auto& state : runtimes) {
    if (state.runtime == nullptr || state.runtime->get_native() != swapchain->get_native()) continue;
    RestoreDirectPQ(&state);
    state.direct_pq = {};
    state.presentation = 0;
  }
}

inline void OnPresent(api::swapchain* swapchain, float ui_nits) {
  for (auto& state : runtimes) {
    if (state.runtime == nullptr || state.runtime->get_native() != swapchain->get_native()) continue;
    state.color_space = swapchain->get_color_space();
    state.ui_nits = ui_nits;
    state.presented = true;
  }
}

inline void OverlayCallback(api::effect_runtime* runtime) {
  // Preserve nested invocations and the exact x64 void(effect_runtime*) ABI.
  if (active != nullptr) {
    RejectCorrection();
    original_overlay(runtime);
    return;
  }
  auto* state = FindRuntime(runtime);
  if (state == nullptr || !state->native_supported || !state->direct_pq_enabled) {
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
  }
  state->correction_candidate = false;
  active = state;
  struct ExitScope {
    Runtime* state;
    ~ExitScope() {
      RestoreDirectPQ(state);
      active = nullptr;
      state->correction_candidate = false;
    }
  } exit_scope{state};

  // Preserve the main HDR10 single-buffer/proxy identity contract.
  auto* data = renodx::utils::data::Get<renodx::mods::swapchain::DeviceData>(runtime->get_device());
  if (state->presented && state->color_space == api::color_space::hdr10_st2084
      && data != nullptr && data->primary_swapchain_window == runtime->get_hwnd()
      && data->swapchain_proxy_passes.contains(presentation)
      && runtime->get_back_buffer_count() == 1 && state->presentation_identity != 0
      && state->presentation_identity == state->buffer_zero_identity
      && state->presentation_identity == state->native_buffer_zero_identity
      && state->proxy_identity != 0 && state->presentation_identity != state->proxy_identity
      && std::isfinite(state->ui_nits) && state->ui_nits >= 0.f) {
    state->correction_candidate = true;
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
    IdentifyCallback();  // Also handles Dalamud registering before RenoDX loads.
  } else if (reason == DLL_PROCESS_DETACH && attached) {
    const std::lock_guard lock(registration_mutex);
    RestoreDispatch();
    for (auto& state : runtimes) {
      RestoreDirectPQ(&state);
      state.direct_pq = {};
    }
    original_overlay = nullptr;
    owner = nullptr;
    reshade::unregister_event<Event::init_effect_runtime>(OnInitRuntime);
    reshade::unregister_event<Event::destroy_effect_runtime>(OnDestroyRuntime);
    reshade::unregister_event<Event::init_swapchain>(OnInitSwapchain);
    reshade::unregister_event<Event::destroy_swapchain>(OnDestroySwapchain);
    // Do not swallow detach errors and unload executable hook targets. The
    // existing Detours utility throws if it cannot restore its own hooks.
    if (native_hooks) renodx::utils::detour::Uninstall(native_detours);
    renodx::utils::detour::Uninstall(export_detours);
    native_hooks = attached = false;
    active = nullptr;
    for (auto& state : runtimes) state = {};
  }
}

}  // namespace renodx::games::ffxiv::dalamud_overlay
