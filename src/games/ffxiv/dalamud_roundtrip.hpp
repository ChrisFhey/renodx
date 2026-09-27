/*
 * Copyright (C) 2026 RenoDX contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>

#include <cstdint>

#include <embed/0x4453AD2C.h>
#include <embed/0xE67E155C.h>
#include <embed/final_vertex_shader.h>

#include "../../utils/log.hpp"

namespace renodx::games::ffxiv::dalamud_overlay {

// Stage 2 only: finish BOTH passes before calling Dalamud. Nothing draws UI
// into this working surface. Values stay BT.2020; 1.0 linear = 80 nits.
// RenderPass's usual restoration uses ReShade-tracked state, which cannot
// describe peeled native calls. Use the existing fullscreen VS and native
// D3D11.1 context-state isolation instead, without extending shared utilities.
// https://learn.microsoft.com/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-swapdevicecontextstate
struct BackgroundRoundTrip {
  template <typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  ComPtr<ID3D11DeviceContext1> context;
  ComPtr<ID3DDeviceContextState> private_state;
  ComPtr<ID3D11VertexShader> vertex_shader;
  ComPtr<ID3D11PixelShader> decode_shader;
  ComPtr<ID3D11PixelShader> encode_shader;
  ComPtr<ID3D11RasterizerState> rasterizer;
  ComPtr<ID3D11DepthStencilState> depth_stencil;
  ComPtr<ID3D11BlendState> rgb_write_mask;
  ComPtr<ID3D11Texture2D> snapshot;
  ComPtr<ID3D11ShaderResourceView> snapshot_srv;
  ComPtr<ID3D11Texture2D> working;
  ComPtr<ID3D11ShaderResourceView> working_srv;
  ComPtr<ID3D11RenderTargetView> working_rtv;
  ComPtr<ID3D11RenderTargetView> presentation_rtv;
  D3D11_VIEWPORT viewport = {};
  uint64_t presentation_identity = 0;
  uint64_t executions = 0;
  bool failed = false;
  const char* skip_reason = nullptr;

  void Reset(const char* reason) {
    if (presentation_identity != 0) {
      renodx::utils::log::i("[FFXIV Dalamud] ROUNDTRIP_RELEASE reason=", reason,
                            " presentation_iunknown=", renodx::utils::log::AsPtr(presentation_identity),
                            " executions=", executions);
    }
    // The private context state is cleared before restoring the caller, so it
    // never retains a hidden presentation RTV across ResizeBuffers/teardown.
    *this = {};
  }

  void Skip(const char* reason) {
    if (skip_reason == reason) return;
    renodx::utils::log::w("[FFXIV Dalamud] ROUNDTRIP_SKIP reason=", reason);
    skip_reason = reason;
  }

  bool Initialize(ID3D11Device* device, ID3D11DeviceContext* native_context, ID3D11Texture2D* presentation) {
    D3D11_TEXTURE2D_DESC desc = {};
    presentation->GetDesc(&desc);
    // Only the resource contract established by the Stage 1 runtime capture.
    if (desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM || desc.SampleDesc.Count != 1
        || desc.SampleDesc.Quality != 0 || desc.MipLevels != 1 || desc.ArraySize != 1
        || desc.Width == 0 || desc.Height == 0 || (desc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0
        || native_context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE
        || device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
      Skip("unsupported presentation descriptor/context");
      return false;
    }

    // Resource/shader creation is transactional: no rendering occurs until
    // every object exists. Failed attempts are not retried every frame.
    const auto check = [](HRESULT result, const char* operation) {
      if (SUCCEEDED(result)) return true;
      renodx::utils::log::w("[FFXIV Dalamud] ROUNDTRIP_CREATE_FAILED operation=", operation,
                            " hr=", renodx::utils::log::AsHex(static_cast<uint32_t>(result)));
      return false;
    };
    ComPtr<ID3D11Device1> device1;
    if (!check(device->QueryInterface(IID_PPV_ARGS(&device1)), "ID3D11Device1")
        || !check(native_context->QueryInterface(IID_PPV_ARGS(&context)), "ID3D11DeviceContext1")) return false;
    const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
    if (!check(device1->CreateDeviceContextState(
                   (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) != 0
                       ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED
                       : 0,
                   &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &private_state),
               "CreateDeviceContextState")) return false;

    if (!check(device->CreateVertexShader(__final_vertex_shader.data(), __final_vertex_shader.size(), nullptr, &vertex_shader), "fullscreen VS")
        || !check(device->CreatePixelShader(__0x4453AD2C.data(), __0x4453AD2C.size(), nullptr, &decode_shader), "PQ decode PS")
        || !check(device->CreatePixelShader(__0xE67E155C.data(), __0xE67E155C.size(), nullptr, &encode_shader), "PQ encode PS")) return false;

    CD3D11_RASTERIZER_DESC rasterizer_desc(D3D11_DEFAULT);
    rasterizer_desc.CullMode = D3D11_CULL_NONE;
    CD3D11_DEPTH_STENCIL_DESC depth_desc(D3D11_DEFAULT);
    depth_desc.DepthEnable = FALSE;
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    CD3D11_BLEND_DESC blend_desc(D3D11_DEFAULT);
    // Preserve the original two alpha bits EXACTLY; alpha never undergoes PQ.
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED
                                                       | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (!check(device->CreateRasterizerState(&rasterizer_desc, &rasterizer), "rasterizer")
        || !check(device->CreateDepthStencilState(&depth_desc, &depth_stencil), "depth stencil")
        || !check(device->CreateBlendState(&blend_desc, &rgb_write_mask), "RGB write mask")) return false;

    // A snapshot avoids relying on a shader-readable swapchain or sampling an
    // RTV while it is bound. This is a GPU copy; no mapping/readback or flush.
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (!check(device->CreateTexture2D(&desc, nullptr, &snapshot), "PQ snapshot")
        || !check(device->CreateShaderResourceView(snapshot.Get(), nullptr, &snapshot_srv), "PQ snapshot SRV")) return false;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (!check(device->CreateTexture2D(&desc, nullptr, &working), "linear FP16 texture")
        || !check(device->CreateShaderResourceView(working.Get(), nullptr, &working_srv), "linear FP16 SRV")
        || !check(device->CreateRenderTargetView(working.Get(), nullptr, &working_rtv), "linear FP16 RTV")
        || !check(device->CreateRenderTargetView(presentation, nullptr, &presentation_rtv), "PQ presentation RTV")) return false;

    D3D11_TEXTURE2D_DESC actual = {};
    working->GetDesc(&actual);
    if (actual.Format != desc.Format || actual.Width != desc.Width || actual.Height != desc.Height
        || actual.SampleDesc.Count != 1 || actual.MipLevels != 1 || actual.ArraySize != 1) {
      Skip("created working texture does not match requested FP16 descriptor");
      return false;
    }
    viewport = {0.f, 0.f, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0.f, 1.f};
    renodx::utils::log::i("[FFXIV Dalamud] ROUNDTRIP_CREATED device=", device, " context=", native_context,
                          " presentation=", presentation, " snapshot=", snapshot.Get(), " working=", working.Get(),
                          " working_rtv=", working_rtv.Get(), " format=", static_cast<unsigned>(actual.Format), "(R16G16B16A16_FLOAT) dimensions=", actual.Width, "x", actual.Height,
                          " samples=1 linear_unit_nits=80 alpha_write=disabled boundary=before_Dalamud");
    return true;
  }

  void Run(reshade::api::effect_runtime* runtime, ID3D11DeviceContext* native_context,
           ID3D11Texture2D* presentation, uint64_t identity, uint64_t frame, bool trace) {
    if (presentation_identity != identity) {
      Reset("presentation_changed");
      presentation_identity = identity;
      if (!Initialize(reinterpret_cast<ID3D11Device*>(runtime->get_device()->get_native()), native_context, presentation)) {
        Reset("initialization_failed");
        presentation_identity = identity;
        failed = true;
      }
    }
    if (failed) return;
    skip_reason = nullptr;

    {
      // Full native state, including predicates, UAVs, SRVs at every stage,
      // shader class instances and SO bindings. No tracked-state assumptions.
      struct RestoreContext {
        ID3D11DeviceContext1* context;
        ComPtr<ID3DDeviceContextState> previous;
        ~RestoreContext() {
          context->ClearState();  // Clear only our private state, never pixels.
          context->SwapDeviceContextState(previous.Get(), nullptr);
        }
      } restore{context.Get(), {}};
      context->SwapDeviceContextState(private_state.Get(), &restore.previous);

      context->CopyResource(snapshot.Get(), presentation);
      context->IASetInputLayout(nullptr);
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      context->VSSetShader(vertex_shader.Get(), nullptr, 0);
      context->RSSetState(rasterizer.Get());
      context->RSSetViewports(1, &viewport);
      context->OMSetDepthStencilState(depth_stencil.Get(), 0);
      context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

      context->OMSetRenderTargets(1, working_rtv.GetAddressOf(), nullptr);
      context->PSSetShaderResources(0, 1, snapshot_srv.GetAddressOf());
      context->PSSetShader(decode_shader.Get(), nullptr, 0);
      context->Draw(3, 0);

      // Remove the working RTV before making the same texture an SRV.
      context->OMSetRenderTargets(0, nullptr, nullptr);
      context->PSSetShaderResources(0, 1, working_srv.GetAddressOf());
      context->OMSetRenderTargets(1, presentation_rtv.GetAddressOf(), nullptr);
      context->OMSetBlendState(rgb_write_mask.Get(), nullptr, 0xFFFFFFFF);
      context->PSSetShader(encode_shader.Get(), nullptr, 0);
      context->Draw(3, 0);
    }

    ++executions;
    if (executions <= 3 || trace) {
      renodx::utils::log::i("[FFXIV Dalamud] ROUNDTRIP_PQ_DECODE_SUBMITTED runtime=", runtime, " frame=", frame,
                            " executions=", executions, " snapshot=", snapshot.Get(), " working=", working.Get());
      renodx::utils::log::i("[FFXIV Dalamud] ROUNDTRIP_PQ_WRITEBACK_SUBMITTED runtime=", runtime, " frame=", frame,
                            " executions=", executions, " presentation=", presentation,
                            " native_state=restored alpha=untouched before_Dalamud=1");
    }
    if (executions == 64) {
      renodx::utils::log::i("[FFXIV Dalamud] ROUNDTRIP_REUSED runtime=", runtime,
                            " executions=64 working=", working.Get(), " snapshot=", snapshot.Get(), " resource_creations_since_create=0");
    }
  }
};

}  // namespace renodx::games::ffxiv::dalamud_overlay
