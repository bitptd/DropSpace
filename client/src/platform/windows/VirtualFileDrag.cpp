#ifdef _WIN32

#include "platform/windows/VirtualFileDrag.h"

#include <objidl.h>
#include <ole2.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

namespace dropspace::windows {
namespace {

class DropSource final : public IDropSource {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_IDropSource) {
      *object = static_cast<IDropSource*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG value = --refs_;
    if (!value) delete this;
    return value;
  }
  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed,
                                               DWORD keyState) override {
    if (escapePressed) return DRAGDROP_S_CANCEL;
    if ((keyState & MK_LBUTTON) == 0) return DRAGDROP_S_DROP;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
    return DRAGDROP_S_USEDEFAULTCURSORS;
  }

 private:
  std::atomic<ULONG> refs_{1};
};

struct PayloadState {
  std::mutex mutex;
  std::condition_variable ready;
  std::vector<std::byte> bytes;
  std::wstring filePath;
  HRESULT result = E_PENDING;
};

class LazyStream final : public IStream {
 public:
  LazyStream(std::shared_ptr<PayloadState> state, std::uint64_t expectedSize,
             std::uint64_t position = 0)
      : state_(std::move(state)), expectedSize_(expectedSize), position_(position) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_ISequentialStream || iid == IID_IStream) {
      *object = static_cast<IStream*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG value = --refs_;
    if (!value) delete this;
    return value;
  }

  HRESULT STDMETHODCALLTYPE Read(void* target, ULONG requested, ULONG* read) override {
    if (read) *read = 0;
    if (!target && requested) return STG_E_INVALIDPOINTER;
    std::unique_lock lock(state_->mutex);
    state_->ready.wait(lock, [&] { return state_->result != E_PENDING; });
    if (FAILED(state_->result)) return state_->result;
    std::size_t count = 0;
    if (!state_->filePath.empty()) {
      FILE* file = _wfopen(state_->filePath.c_str(), L"rb");
      if (!file) return STG_E_READFAULT;
      if (_fseeki64(file, static_cast<__int64>(position_), SEEK_SET) != 0) {
        fclose(file); return STG_E_SEEKERROR;
      }
      count = fread(target, 1, requested, file); fclose(file); position_ += count;
    } else {
      const auto available = position_ < state_->bytes.size()
                                 ? state_->bytes.size() - static_cast<std::size_t>(position_)
                                 : 0;
      count = std::min<std::size_t>(available, requested);
      if (count) { std::memcpy(target, state_->bytes.data() + position_, count); position_ += count; }
    }
    if (read) *read = static_cast<ULONG>(count);
    return count == requested ? S_OK : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override {
    return STG_E_ACCESSDENIED;
  }
  HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin,
                                 ULARGE_INTEGER* newPosition) override {
    std::int64_t base = 0;
    if (origin == STREAM_SEEK_CUR) base = static_cast<std::int64_t>(position_);
    else if (origin == STREAM_SEEK_END) base = static_cast<std::int64_t>(expectedSize_);
    else if (origin != STREAM_SEEK_SET) return STG_E_INVALIDFUNCTION;
    if ((move.QuadPart < 0 && base < -move.QuadPart) ||
        (move.QuadPart > 0 && base > INT64_MAX - move.QuadPart)) {
      return STG_E_INVALIDFUNCTION;
    }
    const auto next = base + move.QuadPart;
    if (next < 0) return STG_E_INVALIDFUNCTION;
    position_ = static_cast<std::uint64_t>(next);
    if (newPosition) newPosition->QuadPart = position_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
  HRESULT STDMETHODCALLTYPE CopyTo(IStream* destination, ULARGE_INTEGER amount,
                                   ULARGE_INTEGER* bytesRead,
                                   ULARGE_INTEGER* bytesWritten) override {
    if (!destination) return STG_E_INVALIDPOINTER;
    if (bytesRead) bytesRead->QuadPart = 0;
    if (bytesWritten) bytesWritten->QuadPart = 0;
    std::byte buffer[64 * 1024];
    std::uint64_t remaining = amount.QuadPart;
    while (remaining) {
      const ULONG ask = static_cast<ULONG>(std::min<std::uint64_t>(remaining, sizeof(buffer)));
      ULONG got = 0;
      const HRESULT readResult = Read(buffer, ask, &got);
      if (FAILED(readResult)) return readResult;
      if (!got) break;
      ULONG written = 0;
      const HRESULT writeResult = destination->Write(buffer, got, &written);
      if (bytesRead) bytesRead->QuadPart += got;
      if (bytesWritten) bytesWritten->QuadPart += written;
      if (FAILED(writeResult) || written != got) return STG_E_MEDIUMFULL;
      remaining -= got;
      if (readResult == S_FALSE) break;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_REVERTED; }
  HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
    return STG_E_INVALIDFUNCTION;
  }
  HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
    return STG_E_INVALIDFUNCTION;
  }
  HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override {
    if (!stat) return STG_E_INVALIDPOINTER;
    std::memset(stat, 0, sizeof(*stat));
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = expectedSize_;
    stat->grfMode = STGM_READ;
    if ((flags & STATFLAG_NONAME) == 0) stat->pwcsName = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Clone(IStream** stream) override {
    if (!stream) return E_POINTER;
    *stream = new (std::nothrow) LazyStream(state_, expectedSize_, position_);
    return *stream ? S_OK : E_OUTOFMEMORY;
  }

 private:
  std::atomic<ULONG> refs_{1};
  std::shared_ptr<PayloadState> state_;
  std::uint64_t expectedSize_;
  std::uint64_t position_;
};

class DataObject final : public IDataObject, public IDataObjectAsyncCapability {
 public:
  explicit DataObject(std::vector<VirtualFileEntry> entries)
      : entries_(std::move(entries)), payloads_(entries_.size()) {
    fileDescriptorFormat_ = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
    fileContentsFormat_ = RegisterClipboardFormatW(CFSTR_FILECONTENTS);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_IDataObject) {
      *object = static_cast<IDataObject*>(this);
    } else if (iid == IID_IDataObjectAsyncCapability) {
      *object = static_cast<IDataObjectAsyncCapability*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG value = --refs_;
    if (!value) delete this;
    return value;
  }

  HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
    if (!format || !medium) return E_POINTER;
    std::memset(medium, 0, sizeof(*medium));
    const HRESULT supported = QueryGetData(format);
    if (FAILED(supported)) return supported;
    if (format->cfFormat == fileDescriptorFormat_) return descriptors(medium);

    const auto index = static_cast<std::size_t>(format->lindex);
    auto state = payloadFor(index);
    if (!state) return DV_E_LINDEX;
    auto* stream = new (std::nothrow) LazyStream(state, entries_[index].size);
    if (!stream) return E_OUTOFMEMORY;
    medium->tymed = TYMED_ISTREAM;
    medium->pstm = stream;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
    return DATA_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
    if (!format) return E_POINTER;
    if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
    if (format->cfFormat == fileDescriptorFormat_ &&
        (format->tymed & TYMED_HGLOBAL) != 0 && format->lindex == -1) return S_OK;
    if (format->cfFormat == fileContentsFormat_ &&
        (format->tymed & TYMED_ISTREAM) != 0 && format->lindex >= 0 &&
        static_cast<std::size_t>(format->lindex) < entries_.size() &&
        !entries_[format->lindex].directory) return S_OK;
    return DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) override {
    if (output) output->ptd = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** enumerator) override {
    if (!enumerator) return E_POINTER;
    *enumerator = nullptr;
    if (direction != DATADIR_GET) return E_NOTIMPL;
    FORMATETC formats[] = {
        {fileDescriptorFormat_, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        {fileContentsFormat_, nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM},
    };
    return SHCreateStdEnumFmtEtc(static_cast<UINT>(std::size(formats)), formats, enumerator);
  }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL asyncMode) override {
    asyncMode_ = asyncMode;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* asyncMode) override {
    if (!asyncMode) return E_POINTER;
    *asyncMode = asyncMode_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx*) override {
    inOperation_ = TRUE;
    AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE InOperation(BOOL* inOperation) override {
    if (!inOperation) return E_POINTER;
    *inOperation = inOperation_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EndOperation(HRESULT, IBindCtx*, DWORD) override {
    if (inOperation_) {
      inOperation_ = FALSE;
      Release();
    }
    return S_OK;
  }

 private:
  HRESULT descriptors(STGMEDIUM* medium) const {
    if (entries_.size() > UINT_MAX) return E_OUTOFMEMORY;
    const auto bytes = sizeof(FILEGROUPDESCRIPTORW) +
                       (entries_.size() - 1) * sizeof(FILEDESCRIPTORW);
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!global) return E_OUTOFMEMORY;
    auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(global));
    if (!group) {
      GlobalFree(global);
      return E_OUTOFMEMORY;
    }
    group->cItems = static_cast<UINT>(entries_.size());
    for (std::size_t index = 0; index < entries_.size(); ++index) {
      const auto& entry = entries_[index];
      auto& descriptor = group->fgd[index];
      descriptor.dwFlags = FD_ATTRIBUTES | FD_PROGRESSUI;
      descriptor.dwFileAttributes = entry.directory ? FILE_ATTRIBUTE_DIRECTORY
                                                     : FILE_ATTRIBUTE_NORMAL;
      if (!entry.directory) {
        descriptor.dwFlags |= FD_FILESIZE;
        descriptor.nFileSizeHigh = static_cast<DWORD>(entry.size >> 32);
        descriptor.nFileSizeLow = static_cast<DWORD>(entry.size & 0xffffffffULL);
      }
      std::wcsncpy(descriptor.cFileName, entry.relativePath.c_str(), MAX_PATH - 1);
      descriptor.cFileName[MAX_PATH - 1] = L'\0';
    }
    GlobalUnlock(global);
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = global;
    return S_OK;
  }

  std::shared_ptr<PayloadState> payloadFor(std::size_t index) {
    std::lock_guard lock(payloadMutex_);
    if (payloads_[index]) return payloads_[index];
    auto state = std::make_shared<PayloadState>();
    payloads_[index] = state;
    auto provider = entries_[index].provideBytes;
    auto pathProvider = entries_[index].provideFilePath;
    const auto expected = entries_[index].size;
    std::thread([state, provider = std::move(provider), pathProvider = std::move(pathProvider), expected]() mutable {
      std::vector<std::byte> bytes;
      std::wstring path;
      HRESULT result = E_FAIL;
      try {
        if (pathProvider) path = pathProvider();
        else if (provider) bytes = provider();
        if (!path.empty()) {
          WIN32_FILE_ATTRIBUTE_DATA info{};
          if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) &&
              !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            const std::uint64_t size = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
            result = size == expected ? S_OK : STG_E_READFAULT;
          }
        } else result = bytes.size() == expected ? S_OK : STG_E_READFAULT;
      } catch (...) {
        result = E_FAIL;
      }
      {
        std::lock_guard lock(state->mutex);
        state->bytes = std::move(bytes);
        state->filePath = std::move(path);
        state->result = result;
      }
      state->ready.notify_all();
    }).detach();
    return state;
  }

  std::atomic<ULONG> refs_{1};
  std::vector<VirtualFileEntry> entries_;
  std::vector<std::shared_ptr<PayloadState>> payloads_;
  std::mutex payloadMutex_;
  CLIPFORMAT fileDescriptorFormat_{};
  CLIPFORMAT fileContentsFormat_{};
  BOOL asyncMode_ = TRUE;
  BOOL inOperation_ = FALSE;
};

}  // namespace

bool startVirtualFileDrag(std::vector<VirtualFileEntry> entries,
                          std::wstring* errorMessage) {
  if (entries.empty()) {
    if (errorMessage) *errorMessage = L"No virtual entries were provided";
    return false;
  }
  const HRESULT initialized = OleInitialize(nullptr);
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
    if (errorMessage) *errorMessage = L"OleInitialize failed";
    return false;
  }
  auto* data = new (std::nothrow) DataObject(std::move(entries));
  auto* source = new (std::nothrow) DropSource();
  if (!data || !source) {
    if (data) data->Release();
    if (source) source->Release();
    if (SUCCEEDED(initialized)) OleUninitialize();
    if (errorMessage) *errorMessage = L"Out of memory";
    return false;
  }
  DWORD effect = DROPEFFECT_NONE;
  const HRESULT result = DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
  data->Release();
  source->Release();
  if (SUCCEEDED(initialized)) OleUninitialize();
  if (result != DRAGDROP_S_DROP && result != DRAGDROP_S_CANCEL) {
    if (errorMessage) *errorMessage = L"DoDragDrop failed";
    return false;
  }
  return result == DRAGDROP_S_DROP && effect == DROPEFFECT_COPY;
}

bool selfTestVirtualFileProvider(std::wstring* errorMessage) {
  const std::string payload = "virtual payload";
  wchar_t tempDirectory[MAX_PATH]{}, tempFile[MAX_PATH]{};
  GetTempPathW(MAX_PATH, tempDirectory);
  GetTempFileNameW(tempDirectory, L"dsp", 0, tempFile);
  HANDLE output = CreateFileW(tempFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
  DWORD written = 0;
  const bool filePrepared = output != INVALID_HANDLE_VALUE &&
      WriteFile(output, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) &&
      written == payload.size();
  if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
  std::vector<VirtualFileEntry> entries;
  entries.push_back({L"Root", true, 0, {}});
  entries.push_back({L"Root\\payload.txt", false, payload.size(), [payload] {
                       std::vector<std::byte> bytes(payload.size());
                       std::memcpy(bytes.data(), payload.data(), payload.size());
                       return bytes;
                     }});
  VirtualFileEntry fileBacked{L"Root\\file-backed.txt", false, payload.size(), {}};
  fileBacked.provideFilePath = [path = std::wstring(tempFile)] { return path; };
  entries.push_back(std::move(fileBacked));
  auto* data = new (std::nothrow) DataObject(std::move(entries));
  if (!data) return false;
  const auto descriptorFormat = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
  FORMATETC descriptorRequest{static_cast<CLIPFORMAT>(descriptorFormat), nullptr,
                              DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM descriptors{};
  bool valid = filePrepared && SUCCEEDED(data->GetData(&descriptorRequest, &descriptors));
  if (valid) {
    auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(descriptors.hGlobal));
    valid = group && group->cItems == 3 &&
            (group->fgd[0].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
            group->fgd[1].nFileSizeLow == payload.size();
    if (group) GlobalUnlock(descriptors.hGlobal);
    ReleaseStgMedium(&descriptors);
  }
  const auto contentsFormat = RegisterClipboardFormatW(CFSTR_FILECONTENTS);
  FORMATETC contentRequest{static_cast<CLIPFORMAT>(contentsFormat), nullptr,
                           DVASPECT_CONTENT, 1, TYMED_ISTREAM};
  STGMEDIUM content{};
  if (valid) valid = SUCCEEDED(data->GetData(&contentRequest, &content));
  if (valid) {
    char buffer[32]{};
    ULONG read = 0;
    valid = content.pstm->Read(buffer, sizeof(buffer), &read) == S_FALSE &&
            read == payload.size() && std::string(buffer, read) == payload;
    LARGE_INTEGER zero{};
    valid = valid && SUCCEEDED(content.pstm->Seek(zero, STREAM_SEEK_SET, nullptr));
    IStream* clone = nullptr;
    valid = valid && SUCCEEDED(content.pstm->Clone(&clone));
    if (clone) clone->Release();
    ReleaseStgMedium(&content);
  }
  contentRequest.lindex = 2;
  std::memset(&content, 0, sizeof(content));
  if (valid) valid = SUCCEEDED(data->GetData(&contentRequest, &content));
  if (valid) {
    char buffer[32]{}; ULONG read = 0;
    valid = content.pstm->Read(buffer, sizeof(buffer), &read) == S_FALSE &&
            read == payload.size() && std::string(buffer, read) == payload;
    ReleaseStgMedium(&content);
  }
  BOOL asyncMode = FALSE;
  valid = valid && SUCCEEDED(data->GetAsyncMode(&asyncMode)) && asyncMode;
  data->Release();
  DeleteFileW(tempFile);
  if (!valid && errorMessage) *errorMessage = L"OLE virtual provider self-test failed";
  return valid;
}

}  // namespace dropspace::windows

#endif

