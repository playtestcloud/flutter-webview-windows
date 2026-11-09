#pragma once

#include <flutter/event_channel.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>
#include <flutter/texture_registrar.h>

#include <memory>
#include <mutex>
#include <queue>

#include "graphics_context.h"
#include "texture_bridge.h"
#include "webview.h"

class WebviewBridge {
 public:
  WebviewBridge(flutter::BinaryMessenger* messenger,
                flutter::TextureRegistrar* texture_registrar,
                GraphicsContext* graphics_context,
                std::unique_ptr<Webview> webview);
  ~WebviewBridge();

  TextureBridge* texture_bridge() const { return texture_bridge_.get(); }

  int64_t texture_id() const { return texture_id_; }
  bool IsHttpAuthSupported() const {
    return event_window_ && webview_->IsHttpAuthSupported();
  }

 private:
  std::unique_ptr<flutter::TextureVariant> flutter_texture_;
  std::unique_ptr<TextureBridge> texture_bridge_;
  std::unique_ptr<Webview> webview_;
  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> event_sink_;
  std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>>
      event_channel_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>>
      method_channel_;

  flutter::TextureRegistrar* texture_registrar_;
  int64_t texture_id_;

  HWND event_window_ = nullptr;
  std::mutex event_mutex_;
  std::queue<flutter::EncodableValue> pending_events_;
  bool is_disposing_ = false;
  static LRESULT CALLBACK EventWindowProc(HWND hwnd, UINT message,
                                        WPARAM wparam, LPARAM lparam);
  void DispatchEvents();

  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void RegisterEventHandlers();

  template <typename T>
  bool EmitEvent(const T& value) {
    std::lock_guard<std::mutex> lock(event_mutex_);
    if (is_disposing_ || !event_window_ ||
        !PostMessageW(event_window_, WM_APP, 0, 0))
      return false;
    pending_events_.push(flutter::EncodableValue(value));
    return true;
  }

  void OnPermissionRequested(
      const std::string& url, WebviewPermissionKind permissionKind,
      bool is_user_initiated,
      Webview::WebviewPermissionRequestedCompleter completer);
};
