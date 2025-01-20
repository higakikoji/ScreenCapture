#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>

#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3d11.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <Windows.Graphics.Capture.Interop.h>

#pragma comment(lib, "windowsapp.lib")

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::System;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using namespace winrt::Windows::Graphics::Capture;

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

class GraphicsCapture {
public:
  using Callback = std::function<void(ID3D11Texture2D *, int w, int h)>;

  GraphicsCapture();
  ~GraphicsCapture();
  bool start(HWND hwnd, bool free_threaded, const Callback &callback);
  bool start(HMONITOR hmon, bool free_threaded, const Callback &callback);
  void stop();

private:
  template<class CreateCaptureItem>
  bool startImpl(bool free_threaded, const Callback &callback, const CreateCaptureItem &cci);

  void onFrameArrived(
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const &sender,
    winrt::Windows::Foundation::IInspectable const &args);

private:
  com_ptr<ID3D11Device> m_device;
  com_ptr<ID3D11DeviceContext> m_context;

  IDirect3DDevice m_device_rt{ nullptr };
  Direct3D11CaptureFramePool m_frame_pool{ nullptr };
  GraphicsCaptureItem m_capture_item{ nullptr };
  GraphicsCaptureSession m_capture_session{ nullptr };
  Direct3D11CaptureFramePool::FrameArrived_revoker m_frame_arrived;

  Callback m_callback;
};

GraphicsCapture::GraphicsCapture() {
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  flags |= D3D11_CREATE_DEVICE_DEBUG;

  ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, m_device.put(), nullptr, nullptr);
  m_device->GetImmediateContext(m_context.put());

  auto dxgi = m_device.as<IDXGIDevice>();
  com_ptr<::IInspectable> device_rt;
  ::CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), device_rt.put());
  m_device_rt = device_rt.as<IDirect3DDevice>();
}

GraphicsCapture::~GraphicsCapture() {
  stop();
}

void GraphicsCapture::stop() {
  m_frame_arrived.revoke();
  m_capture_session = nullptr;
  if (m_frame_pool) {
    m_frame_pool.Close();
    m_frame_pool = nullptr;
  }
  m_capture_item = nullptr;
  m_callback = {};
}

template<class CreateCaptureItem>
bool GraphicsCapture::startImpl(bool free_threaded, const Callback &callback, const CreateCaptureItem &cci) {
  stop();
  m_callback = callback;

  auto factory = get_activation_factory<GraphicsCaptureItem>();
  auto interop = factory.as<IGraphicsCaptureItemInterop>();
  cci(interop);

  if (m_capture_item) {
    auto size = m_capture_item.Size();
    if (free_threaded)
      m_frame_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(m_device_rt, DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, size);
    else
      m_frame_pool = Direct3D11CaptureFramePool::Create(m_device_rt, DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, size);
    m_frame_arrived = m_frame_pool.FrameArrived(auto_revoke, { this, &GraphicsCapture::onFrameArrived });

    m_capture_session = m_frame_pool.CreateCaptureSession(m_capture_item);
    m_capture_session.StartCapture();
    return true;
  } else {
    return false;
  }
}

bool GraphicsCapture::start(HWND hwnd, bool free_threaded, const Callback &callback) {
  return startImpl(free_threaded, callback, [&](auto interop) {
    interop->CreateForWindow(hwnd, guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), put_abi(m_capture_item));
    });
}

bool GraphicsCapture::start(HMONITOR hmon, bool free_threaded, const Callback &callback) {
  return startImpl(free_threaded, callback, [&](auto interop) {
    interop->CreateForMonitor(hmon, guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), put_abi(m_capture_item));
    });
}

void GraphicsCapture::onFrameArrived(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const &sender, winrt::Windows::Foundation::IInspectable const &args) {
  auto frame = sender.TryGetNextFrame();
  auto size = frame.ContentSize();

  com_ptr<ID3D11Texture2D> surface;
  frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>()->GetInterface(guid_of<ID3D11Texture2D>(), surface.put_void());
  m_callback(surface.get(), size.Width, size.Height);
}

static bool ReadTexture(ID3D11Texture2D *tex, int width, int height, const std::function<void(void *, int)> &callback) {
  com_ptr<ID3D11Device> device;
  com_ptr<ID3D11DeviceContext> ctx;
  tex->GetDevice(device.put());
  device->GetImmediateContext(ctx.put());

  com_ptr<ID3D11Query> query_event;
  {
    D3D11_QUERY_DESC qdesc = { D3D11_QUERY_EVENT , 0 };
    device->CreateQuery(&qdesc, query_event.put());
  }

  // create staging texture
  com_ptr<ID3D11Texture2D> staging;
  {
    D3D11_TEXTURE2D_DESC tmp;
    tex->GetDesc(&tmp);
    D3D11_TEXTURE2D_DESC desc{ (UINT)width, (UINT)height, 1, 1, tmp.Format, { 1, 0 }, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0 };
    device->CreateTexture2D(&desc, nullptr, staging.put());
  }

  {
    D3D11_BOX box{};
    box.right = width;
    box.bottom = height;
    box.back = 1;
    ctx->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, tex, 0, &box);
    ctx->End(query_event.get());
    ctx->Flush();
  }

  int wait_count = 0;
  while (ctx->GetData(query_event.get(), nullptr, 0, 0) == S_FALSE) {
    ++wait_count; // just for debug
  }

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (SUCCEEDED(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    D3D11_TEXTURE2D_DESC desc{};
    staging->GetDesc(&desc);

    callback(mapped.pData, mapped.RowPitch);
    ctx->Unmap(staging.get(), 0);
    return true;
  }
  return false;
}

static bool SaveAsJPG(const char *path, int w, int h, int src_stride, const void *data, bool flip_y = 0) {
  //return false; // for profile

  std::vector<byte> buf(w * h * 4);
  int dst_stride = w * 4;
  auto src = (const byte *)data;
  auto dst = (byte *)buf.data();
  if (flip_y) {
    for (int i = 0; i < h; ++i) {
      auto s = src + (src_stride * (h - i - 1));
      auto d = dst + (dst_stride * i);
      for (int j = 0; j < w; ++j) {
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = s[3];
        s += 4;
        d += 4;
      }
    }
  } else {
    for (int i = 0; i < h; ++i) {
      auto s = src + (src_stride * i);
      auto d = dst + (dst_stride * i);
      for (int j = 0; j < w; ++j) {
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = s[3];
        s += 4;
        d += 4;
      }
    }
  }
  return stbi_write_jpg(path, w, h, 4, buf.data(), dst_stride);
}

void ScreenCapture(const char *const path) {
  HMONITOR target = ::MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);

  GraphicsCapture capture;

  std::mutex mutex;
  std::condition_variable cond;

  auto callback = [&](ID3D11Texture2D *surface, int w, int h) {
    ReadTexture(surface, w, h, [&](void *data, int stride) {
      SaveAsJPG(path, w, h, stride, data);
      });
    cond.notify_one();
    };

  std::unique_lock<std::mutex> lock(mutex);
  if (capture.start(target, true, callback)) {
    cond.wait(lock);

    capture.stop();
  }
}
