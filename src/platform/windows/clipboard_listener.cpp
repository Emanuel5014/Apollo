/**
 * @file src/platform/windows/clipboard_listener.cpp
 * @brief Clipboard change notifications for the streamfork clipboard push.
 */
// platform includes
#include <Windows.h>

// standard includes
#include <future>
#include <thread>

// local includes
#include "misc.h"
#include "src/logging.h"
#include "src/platform/common.h"

using namespace std::literals;

namespace platf {
  namespace {
    constexpr auto WINDOW_CLASS_NAME = L"ApolloStreamforkClipboardListener";

    // Reads CF_UNICODETEXT as UTF-8 with LF line endings. Returns false if the
    // clipboard holds no text or can't be opened.
    bool read_clipboard_text(HWND hwnd, std::string &text) {
      if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        return false;
      }

      // The application that just changed the clipboard may still have it open
      bool opened = false;
      for (int i = 0; i < 5 && !(opened = OpenClipboard(hwnd)); i++) {
        Sleep(10);
      }
      if (!opened) {
        BOOST_LOG(debug) << "Clipboard listener: unable to open the clipboard"sv;
        return false;
      }

      bool ok = false;
      if (auto data = GetClipboardData(CF_UNICODETEXT)) {
        if (auto wide = static_cast<const wchar_t *>(GlobalLock(data))) {
          text = to_utf8(wide);
          GlobalUnlock(data);
          ok = true;
        }
      }
      CloseClipboard();

      if (ok) {
        // Normalize CRLF to LF for the wire format
        std::string normalized;
        normalized.reserve(text.size());
        for (size_t i = 0; i < text.size(); i++) {
          if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            continue;
          }
          normalized += text[i];
        }
        text = std::move(normalized);
      }

      return ok;
    }

    class clipboard_listener_t: public deinit_t {
    public:
      explicit clipboard_listener_t(std::function<void(std::string)> on_text_changed):
          on_text_changed {std::move(on_text_changed)} {
        std::promise<HWND> window_promise;
        auto window_future = window_promise.get_future();

        thread = std::thread {[this, promise = std::move(window_promise)]() mutable {
          run(promise);
        }};

        hwnd = window_future.get();
      }

      ~clipboard_listener_t() override {
        if (hwnd) {
          PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        if (thread.joinable()) {
          thread.join();
        }
      }

      bool started() const {
        return hwnd != nullptr;
      }

    private:
      static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        auto self = reinterpret_cast<clipboard_listener_t *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg) {
          case WM_CLIPBOARDUPDATE:
            if (self) {
              std::string text;
              if (read_clipboard_text(hwnd, text)) {
                self->on_text_changed(std::move(text));
              }
            }
            return 0;
          case WM_CLOSE:
            RemoveClipboardFormatListener(hwnd);
            DestroyWindow(hwnd);
            return 0;
          case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
          default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
      }

      void run(std::promise<HWND> &window_promise) {
        WNDCLASSEXW wc {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = window_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = WINDOW_CLASS_NAME;

        // Registration fails harmlessly if the class already exists from a previous listener
        RegisterClassExW(&wc);

        // A message-only window is enough to receive clipboard notifications
        HWND window = CreateWindowExW(0, WINDOW_CLASS_NAME, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (!window) {
          BOOST_LOG(warning) << "Clipboard listener: CreateWindowEx() failed: "sv << GetLastError();
          window_promise.set_value(nullptr);
          return;
        }

        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

        if (!AddClipboardFormatListener(window)) {
          BOOST_LOG(warning) << "Clipboard listener: AddClipboardFormatListener() failed: "sv << GetLastError();
          DestroyWindow(window);
          window_promise.set_value(nullptr);
          return;
        }

        window_promise.set_value(window);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
          TranslateMessage(&msg);
          DispatchMessageW(&msg);
        }
      }

      std::function<void(std::string)> on_text_changed;
      std::thread thread;
      HWND hwnd = nullptr;
    };
  }  // namespace

  std::unique_ptr<deinit_t>
  watch_clipboard(std::function<void(std::string)> on_text_changed) {
    auto listener = std::make_unique<clipboard_listener_t>(std::move(on_text_changed));
    if (!listener->started()) {
      return nullptr;
    }

    BOOST_LOG(info) << "Clipboard listener started"sv;
    return listener;
  }
}  // namespace platf
