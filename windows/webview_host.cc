#include "webview_host.h"

#include <wrl.h>

#include <iomanip>
#include <iostream>
#include <sstream>

#include "util/rohelper.h"

namespace {

std::string HResultToString(HRESULT hr) {
  std::ostringstream oss;
  oss << "0x" << std::hex << std::setw(8) << std::setfill('0')
      << static_cast<unsigned long>(hr);
  switch (hr) {
    case E_ACCESSDENIED:
      oss << " (Access Denied)";
      break;
    case E_FAIL:
      oss << " (Unspecified Error)";
      break;
    case HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND):
      oss << " (File/Path Not Found)";
      break;
    case HRESULT_FROM_WIN32(ERROR_INVALID_STATE):
      oss << " (Invalid State - possibly conflicting WebView2 instance)";
      break;
    default:
      break;
  }
  return oss.str();
}

void LogWebViewError(const std::string& context, HRESULT hr) {
  std::string msg =
      "[WebView2] " + context + ": HRESULT=" + HResultToString(hr);
  std::cerr << msg << std::endl;
  OutputDebugStringA((msg + "\n").c_str());
}

// Waits for an event while pumping Windows messages on the calling thread.
// Prevents deadlocks when the signaling thread sends messages to windows
// owned by the caller (e.g. WebView2 sending messages to an HWND created
// on the platform thread).
void PumpWaitForEvent(HANDLE event) {
  while (true) {
    DWORD result =
        MsgWaitForMultipleObjects(1, &event, FALSE, INFINITE, QS_ALLINPUT);
    if (result == WAIT_OBJECT_0) {
      return;
    }
    if (result == WAIT_OBJECT_0 + 1) {
      MSG msg;
      while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
      }
    } else {
      return;
    }
  }
}

}  // namespace

using namespace Microsoft::WRL;

// ---------------------------------------------------------------------------
// STA thread: persistent thread with COM STA apartment and message pump.
// All WebView2 creation calls are dispatched here because Flutter 3.29+
// initializes the platform thread as MTA, while WebView2 requires STA.
// ---------------------------------------------------------------------------

void WebviewHost::StaThreadMain() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  sta_thread_id_ = GetCurrentThreadId();
  SetEvent(sta_ready_event_);

  HANDLE events[] = {quit_event_, work_event_};

  while (true) {
    DWORD result =
        MsgWaitForMultipleObjects(2, events, FALSE, INFINITE, QS_ALLINPUT);

    if (result == WAIT_OBJECT_0) {
      break;
    }

    // Drain work queue
    while (true) {
      std::function<void()> work;
      {
        std::lock_guard<std::mutex> lock(work_mutex_);
        if (work_queue_.empty()) {
          ResetEvent(work_event_);
          break;
        }
        work = std::move(work_queue_.front());
        work_queue_.pop();
      }
      work();
    }

    // Pump Windows messages (delivers COM/WebView2 async callbacks)
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  }

  CoUninitialize();
}

void WebviewHost::RunOnSta(std::function<void()> work) {
  if (GetCurrentThreadId() == sta_thread_id_) {
    work();
    return;
  }
  HANDLE doneEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  PostToSta([work = std::move(work), doneEvent]() {
    work();
    SetEvent(doneEvent);
  });
  PumpWaitForEvent(doneEvent);
  CloseHandle(doneEvent);
}

void WebviewHost::PostToSta(std::function<void()> work) {
  {
    std::lock_guard<std::mutex> lock(work_mutex_);
    work_queue_.push(std::move(work));
  }
  SetEvent(work_event_);
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

WebviewHost::WebviewHost(WebviewPlatform* platform) {
  compositor_ = platform->graphics_context()->CreateCompositor();

  work_event_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  quit_event_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  sta_ready_event_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);

  sta_thread_ = std::thread(&WebviewHost::StaThreadMain, this);
  WaitForSingleObject(sta_ready_event_, INFINITE);
  CloseHandle(sta_ready_event_);
  sta_ready_event_ = nullptr;
}

WebviewHost::~WebviewHost() {
  if (sta_thread_.joinable()) {
    SetEvent(quit_event_);
    sta_thread_.join();
  }
  if (work_event_) CloseHandle(work_event_);
  if (quit_event_) CloseHandle(quit_event_);
}

// ---------------------------------------------------------------------------
// WebviewHost::Create  (static factory)
// ---------------------------------------------------------------------------

// static
std::unique_ptr<WebviewHost> WebviewHost::Create(
    WebviewPlatform* platform, std::optional<std::wstring> user_data_directory,
    std::optional<std::wstring> browser_exe_path,
    std::optional<std::string> arguments) {
  wil::com_ptr<CoreWebView2EnvironmentOptions> opts;
  if (arguments.has_value()) {
    opts = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    std::wstring warguments(arguments.value().begin(), arguments.value().end());
    opts->put_AdditionalBrowserArguments(warguments.c_str());
  }

  auto host = std::unique_ptr<WebviewHost>(new WebviewHost(platform));

  HRESULT createHr = S_OK;
  HRESULT callbackHr = S_OK;
  wil::com_ptr<ICoreWebView2Environment> env;
  HANDLE doneEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

  host->PostToSta([&, doneEvent]() {
    createHr = CreateCoreWebView2EnvironmentWithOptions(
        browser_exe_path.has_value() ? browser_exe_path->c_str() : nullptr,
        user_data_directory.has_value() ? user_data_directory->c_str()
                                        : nullptr,
        opts.get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [&callbackHr, &env, doneEvent](
                HRESULT r, ICoreWebView2Environment* e) -> HRESULT {
              callbackHr = r;
              if (e) {
                e->AddRef();
                env.attach(e);
              }
              SetEvent(doneEvent);
              return S_OK;
            })
            .Get());

    if (FAILED(createHr)) {
      SetEvent(doneEvent);
    }
  });

  PumpWaitForEvent(doneEvent);
  CloseHandle(doneEvent);

  if (FAILED(createHr)) {
    LogWebViewError(
        "CreateCoreWebView2EnvironmentWithOptions returned failure", createHr);
    return {};
  }

  if (FAILED(callbackHr)) {
    LogWebViewError("Environment creation callback reported failure",
                    callbackHr);
    return {};
  }

  if (!env) {
    LogWebViewError(
        "Environment creation callback returned null environment", callbackHr);
    return {};
  }

  auto webview_env3 = env.try_query<ICoreWebView2Environment3>();
  if (!webview_env3) {
    LogWebViewError(
        "ICoreWebView2Environment3 query failed (WebView2 runtime too old?)",
        E_NOINTERFACE);
    return {};
  }

  host->webview_env_ = std::move(webview_env3);
  return host;
}

// ---------------------------------------------------------------------------
// Webview instance creation
// ---------------------------------------------------------------------------

void WebviewHost::CreateWebview(HWND hwnd, bool offscreen_only,
                                bool owns_window,
                                WebviewCreationCallback callback) {
  HRESULT syncHr = S_OK;
  HRESULT asyncHr = S_OK;
  std::unique_ptr<Webview> resultWebview;
  HANDLE doneEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

  PostToSta([&, doneEvent, hwnd, offscreen_only, owns_window]() {
    syncHr = webview_env_->CreateCoreWebView2CompositionController(
        hwnd,
        Callback<
            ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
            [&, doneEvent, hwnd, offscreen_only, owns_window](
                HRESULT hr,
                ICoreWebView2CompositionController* compositionController)
                -> HRESULT {
              asyncHr = hr;
              if (SUCCEEDED(hr) && compositionController) {
                compositionController->AddRef();
                wil::com_ptr<ICoreWebView2CompositionController> controller;
                controller.attach(compositionController);
                resultWebview.reset(new Webview(std::move(controller), this,
                                                hwnd, owns_window,
                                                offscreen_only));
              }
              SetEvent(doneEvent);
              return S_OK;
            })
            .Get());

    if (FAILED(syncHr)) {
      SetEvent(doneEvent);
    }
  });

  PumpWaitForEvent(doneEvent);
  CloseHandle(doneEvent);

  if (resultWebview) {
    callback(std::move(resultWebview), nullptr);
  } else {
    HRESULT errHr = FAILED(syncHr) ? syncHr : asyncHr;
    std::string errMsg =
        FAILED(syncHr)
            ? "CreateCoreWebView2CompositionController failed."
            : "CreateCoreWebView2CompositionController completion handler "
              "failed.";
    LogWebViewError(errMsg, errHr);
    callback(nullptr, WebviewCreationError::create(errHr, std::move(errMsg)));
  }
}

void WebviewHost::CreateWebViewCompositionController(
    HWND hwnd, CompositionControllerCreationCallback callback) {
  HRESULT syncHr = S_OK;
  HRESULT asyncHr = S_OK;
  wil::com_ptr<ICoreWebView2CompositionController> resultController;
  HANDLE doneEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

  PostToSta([&, doneEvent]() {
    syncHr = webview_env_->CreateCoreWebView2CompositionController(
        hwnd,
        Callback<
            ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
            [&asyncHr, &resultController, doneEvent](
                HRESULT hr,
                ICoreWebView2CompositionController* compositionController)
                -> HRESULT {
              asyncHr = hr;
              if (SUCCEEDED(hr) && compositionController) {
                compositionController->AddRef();
                resultController.attach(compositionController);
              }
              SetEvent(doneEvent);
              return S_OK;
            })
            .Get());

    if (FAILED(syncHr)) {
      SetEvent(doneEvent);
    }
  });

  PumpWaitForEvent(doneEvent);
  CloseHandle(doneEvent);

  if (resultController) {
    callback(std::move(resultController), nullptr);
  } else {
    HRESULT errHr = FAILED(syncHr) ? syncHr : asyncHr;
    std::string errMsg =
        FAILED(syncHr)
            ? "CreateCoreWebView2CompositionController failed."
            : "CreateCoreWebView2CompositionController completion handler "
              "failed.";
    LogWebViewError(errMsg, errHr);
    callback(nullptr, WebviewCreationError::create(errHr, errMsg));
  }
}

void WebviewHost::CreateWebViewPointerInfo(
    PointerInfoCreationCallback callback) {
  wil::com_ptr<ICoreWebView2PointerInfo> resultPointer;
  HRESULT resultHr = S_OK;
  HANDLE doneEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

  PostToSta([&, doneEvent]() {
    ICoreWebView2PointerInfo* pointer = nullptr;
    resultHr = webview_env_->CreateCoreWebView2PointerInfo(&pointer);
    if (SUCCEEDED(resultHr) && pointer) {
      pointer->AddRef();
      resultPointer.attach(pointer);
    }
    SetEvent(doneEvent);
  });

  PumpWaitForEvent(doneEvent);
  CloseHandle(doneEvent);

  if (FAILED(resultHr)) {
    callback(nullptr,
             WebviewCreationError::create(resultHr,
                                          "CreateWebViewPointerInfo failed."));
  } else {
    callback(std::move(resultPointer), nullptr);
  }
}
