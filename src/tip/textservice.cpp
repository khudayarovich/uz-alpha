#include "textservice.h"

#include <new>

// Instantiate GUID_PROP_INPUTSCOPE (InputScope.h only declares it otherwise).
#include <initguid.h>
#include <InputScope.h>

#include "trace.h"

using uza::Action;
using uza::Edit;

#ifdef UZA_TRACE
#include <cstdarg>
#include <cstdio>

void UzaTrace(const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof line - 2, fmt, args);
    va_end(args);
    if (n < 0) n = 0;
    if (n > (int)sizeof line - 3) n = (int)sizeof line - 3;
    line[n++] = '\n';
    line[n] = 0;
    OutputDebugStringA(line);

    wchar_t path[MAX_PATH];
    const DWORD len = GetEnvironmentVariableW(L"UZALPHA_TRACE", path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    const HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, line, (DWORD)n, &written, nullptr);
    CloseHandle(f);
}
#endif

namespace {

// Is this process inside an app container (Store apps, the Start menu search)?
// Windows does not let such a process press keys.
bool InAppContainer() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD inContainer = 0, size = 0;
    const BOOL ok = GetTokenInformation(token, TokenIsAppContainer, &inContainer, sizeof inContainer, &size);
    CloseHandle(token);
    return ok && inContainer != 0;
}

enum class SessionKind { Action, EndComposition, InputScope, ReadPrevious };

// One unit of work done under the document lock.
class EditSession : public ITfEditSession {
public:
    EditSession(TextService* svc, ITfContext* ctx, SessionKind kind, const Action& action = Action(),
                wchar_t literal = 0)
        : svc_(svc), ctx_(ctx), kind_(kind), action_(action), literal_(literal) {
        svc_->AddRef();
        if (ctx_) ctx_->AddRef();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(ITfEditSession))) {
            *ppv = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        switch (kind_) {
            case SessionKind::Action: return svc_->DoAction(ec, ctx_, action_, literal_);
            case SessionKind::EndComposition: return svc_->DoEndComposition(ec);
            case SessionKind::InputScope: return svc_->DoReadInputScope(ec, ctx_);
            case SessionKind::ReadPrevious: return svc_->DoReadPrevious(ec, ctx_);
        }
        return E_UNEXPECTED;
    }

private:
    ~EditSession() {
        if (ctx_) ctx_->Release();
        svc_->Release();
    }

    LONG ref_ = 1;
    TextService* svc_;
    ITfContext* ctx_;
    SessionKind kind_;
    Action action_;
    wchar_t literal_;
};

// The text service may be created without COM being initialised on the thread
// (COM-less activation), so prefer the msctf factory function.
ITfCategoryMgr* CreateCategoryMgr() {
    ITfCategoryMgr* cat = nullptr;
    typedef HRESULT(WINAPI * Factory)(ITfCategoryMgr**);
    if (HMODULE msctf = GetModuleHandleW(L"msctf.dll")) {
        if (Factory make = (Factory)GetProcAddress(msctf, "TF_CreateCategoryMgr")) {
            if (SUCCEEDED(make(&cat)) && cat) return cat;
        }
    }
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(ITfCategoryMgr), (void**)&cat))) {
        return cat;
    }
    return nullptr;
}

bool CompartmentIsSet(ITfCompartmentMgr* mgr, REFGUID guid) {
    ITfCompartment* c = nullptr;
    if (FAILED(mgr->GetCompartment(guid, &c)) || !c) return false;
    VARIANT v;
    VariantInit(&v);
    const bool set = SUCCEEDED(c->GetValue(&v)) && v.vt == VT_I4 && v.lVal != 0;
    VariantClear(&v);
    c->Release();
    return set;
}

}  // namespace

TextService::TextService() { DllAddRef(); }

TextService::~TextService() { DllRelease(); }

STDMETHODIMP TextService::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(ITfTextInputProcessor)) ||
        IsEqualIID(riid, __uuidof(ITfTextInputProcessorEx))) {
        *ppv = static_cast<ITfTextInputProcessorEx*>(this);
    } else if (IsEqualIID(riid, __uuidof(ITfThreadMgrEventSink))) {
        *ppv = static_cast<ITfThreadMgrEventSink*>(this);
    } else if (IsEqualIID(riid, __uuidof(ITfThreadFocusSink))) {
        *ppv = static_cast<ITfThreadFocusSink*>(this);
    } else if (IsEqualIID(riid, __uuidof(ITfActiveLanguageProfileNotifySink))) {
        *ppv = static_cast<ITfActiveLanguageProfileNotifySink*>(this);
    } else if (IsEqualIID(riid, __uuidof(IUzAlphaMode))) {
        *ppv = static_cast<IUzAlphaMode*>(this);
    } else if (IsEqualIID(riid, __uuidof(ITfKeyEventSink))) {
        *ppv = static_cast<ITfKeyEventSink*>(this);
    } else if (IsEqualIID(riid, __uuidof(ITfCompositionSink))) {
        *ppv = static_cast<ITfCompositionSink*>(this);
    } else if (IsEqualIID(riid, __uuidof(ITfDisplayAttributeProvider))) {
        *ppv = static_cast<ITfDisplayAttributeProvider*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) TextService::AddRef() { return InterlockedIncrement(&ref_); }

STDMETHODIMP_(ULONG) TextService::Release() {
    const LONG r = InterlockedDecrement(&ref_);
    if (r == 0) delete this;
    return r;
}

// ---------------------------------------------------------------------------
// Activation

STDMETHODIMP TextService::Activate(ITfThreadMgr* tm, TfClientId tid) { return ActivateEx(tm, tid, 0); }

STDMETHODIMP TextService::ActivateEx(ITfThreadMgr* tm, TfClientId tid, DWORD) {
    if (!tm) return E_INVALIDARG;
    threadMgr_ = tm;
    threadMgr_->AddRef();
    clientId_ = tid;
    canResend_ = !InAppContainer();
    claimedVk_ = resendVk_ = 0;

    ITfSource* source = nullptr;
    if (SUCCEEDED(tm->QueryInterface(__uuidof(ITfSource), (void**)&source))) {
        if (FAILED(source->AdviseSink(__uuidof(ITfThreadMgrEventSink),
                                      static_cast<ITfThreadMgrEventSink*>(this), &threadMgrSinkCookie_))) {
            threadMgrSinkCookie_ = TF_INVALID_COOKIE;
        }
        if (FAILED(source->AdviseSink(__uuidof(ITfThreadFocusSink),
                                      static_cast<ITfThreadFocusSink*>(this), &threadFocusSinkCookie_))) {
            threadFocusSinkCookie_ = TF_INVALID_COOKIE;
        }
        if (FAILED(source->AdviseSink(__uuidof(ITfActiveLanguageProfileNotifySink),
                                      static_cast<ITfActiveLanguageProfileNotifySink*>(this), &profileSinkCookie_))) {
            profileSinkCookie_ = TF_INVALID_COOKIE;
        }
        source->Release();
    }
    ReadActiveProfile();

    ITfKeystrokeMgr* keys = nullptr;
    if (SUCCEEDED(tm->QueryInterface(__uuidof(ITfKeystrokeMgr), (void**)&keys))) {
        const HRESULT hrKeys = keys->AdviseKeyEventSink(tid, static_cast<ITfKeyEventSink*>(this), TRUE);
        TRACE("ActivateEx tid=%lu AdviseKeyEventSink=%08lx", (unsigned long)tid, hrKeys);
        keySinkAdvised_ = SUCCEEDED(hrKeys);
        keys->Release();
    }
    if (!keySinkAdvised_) {
        Deactivate();
        return E_FAIL;
    }

    if (ITfCategoryMgr* cat = CreateCategoryMgr()) {
        if (FAILED(cat->RegisterGUID(GUID_UzAlphaDisplayAttr, &displayAttrAtom_))) {
            displayAttrAtom_ = TF_INVALID_GUIDATOM;
        }
        cat->Release();
    }

    ITfDocumentMgr* dm = nullptr;
    if (SUCCEEDED(tm->GetFocus(&dm)) && dm) {
        ITfContext* ctx = nullptr;
        if (SUCCEEDED(dm->GetTop(&ctx)) && ctx) {
            RequestInputScope(ctx);
            ctx->Release();
        }
        dm->Release();
    }

    BOOL focused = FALSE;
    if (SUCCEEDED(tm->IsThreadFocus(&focused)) && focused) PublishActive(true);
    return S_OK;
}

STDMETHODIMP TextService::Deactivate() {
    if (composition_ && compositionCtx_ && threadMgr_) {
        // Best effort: end the composition properly so its text is committed.
        if (EditSession* es = new (std::nothrow) EditSession(this, compositionCtx_, SessionKind::EndComposition)) {
            HRESULT hrSession = E_FAIL;
            compositionCtx_->RequestEditSession(clientId_, es, TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
            es->Release();
        }
    }
    AbandonComposition();
    engine_.reset();
    bypassCtx_ = nullptr;

    if (threadMgr_) {
        PublishActive(false);

        if (keySinkAdvised_) {
            ITfKeystrokeMgr* keys = nullptr;
            if (SUCCEEDED(threadMgr_->QueryInterface(__uuidof(ITfKeystrokeMgr), (void**)&keys))) {
                keys->UnadviseKeyEventSink(clientId_);
                keys->Release();
            }
            keySinkAdvised_ = false;
        }

        ITfSource* source = nullptr;
        if (SUCCEEDED(threadMgr_->QueryInterface(__uuidof(ITfSource), (void**)&source))) {
            if (threadMgrSinkCookie_ != TF_INVALID_COOKIE) source->UnadviseSink(threadMgrSinkCookie_);
            if (threadFocusSinkCookie_ != TF_INVALID_COOKIE) source->UnadviseSink(threadFocusSinkCookie_);
            if (profileSinkCookie_ != TF_INVALID_COOKIE) source->UnadviseSink(profileSinkCookie_);
            source->Release();
        }
        threadMgrSinkCookie_ = threadFocusSinkCookie_ = profileSinkCookie_ = TF_INVALID_COOKIE;

        threadMgr_->Release();
        threadMgr_ = nullptr;
    }
    clientId_ = TF_CLIENTID_NULL;
    return S_OK;
}

// Tell the tray app whether the focused thread is typing through Auto.
void TextService::PublishActive(bool active) {
    if (!threadMgr_) return;
    ITfCompartmentMgr* mgr = nullptr;
    if (FAILED(threadMgr_->GetGlobalCompartment(&mgr)) || !mgr) return;
    ITfCompartment* c = nullptr;
    if (SUCCEEDED(mgr->GetCompartment(GUID_UzAlphaActiveCompartment, &c)) && c) {
        const LONG me = (LONG)GetCurrentThreadId();
        bool write = active;
        if (!active) {
            // Only clear the flag if it is still ours.
            VARIANT cur;
            VariantInit(&cur);
            write = SUCCEEDED(c->GetValue(&cur)) && cur.vt == VT_I4 && cur.lVal == me;
            VariantClear(&cur);
        }
        if (write) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_I4;
            v.lVal = active ? me : 0;
            c->SetValue(clientId_, &v);
        }
        c->Release();
    }
    if (active) {
        // And which of the two keyboards it is.
        ITfCompartment* m = nullptr;
        if (SUCCEEDED(mgr->GetCompartment(GUID_UzAlphaModeCompartment, &m)) && m) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_I4;
            v.lVal = mode_ == Mode::Manual ? UZA_MODE_MANUAL : UZA_MODE_AUTO;
            m->SetValue(clientId_, &v);
            m->Release();
        }
    }
    mgr->Release();
}

// ---------------------------------------------------------------------------
// Which keyboard: Auto or Manual

void TextService::SetMode(Mode mode) {
    if (mode != mode_) {
        if (composition_) RequestEndComposition();
        engine_.reset();
        mode_ = mode;
        TRACE("mode: %s", mode == Mode::Manual ? "manual" : "auto");
    }
    BOOL focused = FALSE;
    if (threadMgr_ && SUCCEEDED(threadMgr_->IsThreadFocus(&focused)) && focused) PublishActive(true);
}

// The profile the service was activated with decides the mode.
void TextService::ReadActiveProfile() {
    ITfInputProcessorProfiles* legacy = nullptr;
    typedef HRESULT(WINAPI * Factory)(ITfInputProcessorProfiles**);
    if (HMODULE msctf = GetModuleHandleW(L"msctf.dll")) {
        if (Factory make = (Factory)GetProcAddress(msctf, "TF_CreateInputProcessorProfiles")) make(&legacy);
    }
    if (!legacy) {
        CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                         __uuidof(ITfInputProcessorProfiles), (void**)&legacy);
    }
    if (!legacy) return;
    ITfInputProcessorProfileMgr* profiles = nullptr;
    if (SUCCEEDED(legacy->QueryInterface(__uuidof(ITfInputProcessorProfileMgr), (void**)&profiles)) && profiles) {
        TF_INPUTPROCESSORPROFILE p = {};
        if (SUCCEEDED(profiles->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &p)) &&
            p.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR && IsEqualCLSID(p.clsid, CLSID_UzAlphaTip)) {
            SetMode(IsEqualGUID(p.guidProfile, GUID_UzAlphaManualProfile) ? Mode::Manual : Mode::Auto);
        }
        profiles->Release();
    }
    legacy->Release();
}

STDMETHODIMP TextService::OnActivated(REFCLSID clsid, REFGUID profile, BOOL activated) {
    if (activated && IsEqualCLSID(clsid, CLSID_UzAlphaTip)) {
        SetMode(IsEqualGUID(profile, GUID_UzAlphaManualProfile) ? Mode::Manual : Mode::Auto);
    }
    return S_OK;
}

STDMETHODIMP TextService::SetManual(BOOL manual) {
    SetMode(manual ? Mode::Manual : Mode::Auto);
    return S_OK;
}

STDMETHODIMP TextService::SetResend(BOOL allowed) {
    canResend_ = allowed != FALSE;
    return S_OK;
}

// ---------------------------------------------------------------------------
// Focus tracking

STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr* focus, ITfDocumentMgr*) {
    if (composition_) RequestEndComposition();
    engine_.reset();
    bypassCtx_ = nullptr;
    if (focus) {
        ITfContext* ctx = nullptr;
        if (SUCCEEDED(focus->GetTop(&ctx)) && ctx) {
            RequestInputScope(ctx);
            ctx->Release();
        }
    }
    return S_OK;
}

STDMETHODIMP TextService::OnPushContext(ITfContext* ctx) {
    if (ctx) RequestInputScope(ctx);
    return S_OK;
}

STDMETHODIMP TextService::OnSetThreadFocus() {
    PublishActive(true);
    return S_OK;
}

STDMETHODIMP TextService::OnKillThreadFocus() {
    PublishActive(false);
    return S_OK;
}

STDMETHODIMP TextService::OnSetFocus(BOOL foreground) {
    if (!foreground) {
        if (composition_) RequestEndComposition();
        engine_.reset();
    }
    return S_OK;
}

// Password and PIN fields: stay out of the way completely.
void TextService::RequestInputScope(ITfContext* ctx) {
    EditSession* es = new (std::nothrow) EditSession(this, ctx, SessionKind::InputScope);
    if (!es) return;
    HRESULT hrSession = E_FAIL;
    ctx->RequestEditSession(clientId_, es, TF_ES_ASYNCDONTCARE | TF_ES_READ, &hrSession);
    es->Release();
}

HRESULT TextService::DoReadInputScope(TfEditCookie ec, ITfContext* ctx) {
    ITfReadOnlyProperty* prop = nullptr;
    if (FAILED(ctx->GetAppProperty(GUID_PROP_INPUTSCOPE, &prop)) || !prop) return S_OK;

    TF_SELECTION sel = {};
    ULONG fetched = 0;
    if (SUCCEEDED(ctx->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched)) && fetched == 1 && sel.range) {
        VARIANT var;
        VariantInit(&var);
        if (SUCCEEDED(prop->GetValue(ec, sel.range, &var)) && var.vt == VT_UNKNOWN && var.punkVal) {
            ITfInputScope* scope = nullptr;
            if (SUCCEEDED(var.punkVal->QueryInterface(__uuidof(ITfInputScope), (void**)&scope)) && scope) {
                InputScope* scopes = nullptr;
                UINT count = 0;
                if (SUCCEEDED(scope->GetInputScopes(&scopes, &count)) && scopes) {
                    for (UINT i = 0; i < count; ++i) {
                        switch (scopes[i]) {
                            case IS_PASSWORD:
                            case IS_NUMERIC_PASSWORD:
                            case IS_NUMERIC_PIN:
                            case IS_ALPHANUMERIC_PIN:
                            case IS_ALPHANUMERIC_PIN_SET:
                                bypassCtx_ = ctx;
                                break;
                            default:
                                break;
                        }
                    }
                    CoTaskMemFree(scopes);
                }
                scope->Release();
            }
        }
        VariantClear(&var);
        sel.range->Release();
    }
    prop->Release();
    return S_OK;
}

// The apostrophe becomes ʼ only after a letter. Typing keeps the engine's own
// memory right, but a mouse click can move the caret anywhere, so when an
// apostrophe arrives outside a digraph ask the document what is really before
// the caret. Documents that only expose part of their text (transitory ones:
// browsers, terminals, classic edit boxes) cannot answer; memory is used there.
void TextService::SyncPrevious(ITfContext* ctx, const KeyInfo& ki) {
    if (ki.key.kind != uza::KeyKind::Char || ki.key.ch != uza::kApostrophe || engine_.composing()) return;

    TF_STATUS status = {};
    if (FAILED(ctx->GetStatus(&status)) || (status.dwStaticFlags & TF_SS_TRANSITORY)) return;

    EditSession* es = new (std::nothrow) EditSession(this, ctx, SessionKind::ReadPrevious);
    if (!es) return;
    previousKnown_ = false;
    HRESULT hrSession = E_FAIL;
    const HRESULT hr = ctx->RequestEditSession(clientId_, es, TF_ES_SYNC | TF_ES_READ, &hrSession);
    es->Release();
    if (SUCCEEDED(hr) && SUCCEEDED(hrSession) && previousKnown_) engine_.setPrevious(previousChar_);
}

HRESULT TextService::DoReadPrevious(TfEditCookie ec, ITfContext* ctx) {
    TF_SELECTION sel = {};
    ULONG fetched = 0;
    if (FAILED(ctx->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched)) || fetched != 1 || !sel.range) {
        return S_OK;
    }
    ITfRange* range = nullptr;
    if (SUCCEEDED(sel.range->Clone(&range)) && range) {
        LONG moved = 0;
        if (SUCCEEDED(range->Collapse(ec, TF_ANCHOR_START)) &&
            SUCCEEDED(range->ShiftStart(ec, -1, &moved, nullptr))) {
            if (moved == 0) {
                previousKnown_ = true;  // start of the document
                previousChar_ = 0;
            } else {
                WCHAR buf[2] = {};
                ULONG got = 0;
                if (SUCCEEDED(range->GetText(ec, 0, buf, 1, &got)) && got == 1) {
                    previousKnown_ = true;
                    previousChar_ = buf[0];
                }
            }
        }
        range->Release();
    }
    sel.range->Release();
    return S_OK;
}

bool TextService::CanHandle(ITfContext* ctx) {
    if (!ctx || ctx == bypassCtx_) return false;

    ITfCompartmentMgr* mgr = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ITfCompartmentMgr), (void**)&mgr)) && mgr) {
        const bool off = CompartmentIsSet(mgr, GUID_COMPARTMENT_KEYBOARD_DISABLED) ||
                         CompartmentIsSet(mgr, GUID_COMPARTMENT_EMPTYCONTEXT);
        mgr->Release();
        if (off) return false;
    }

    TF_STATUS status = {};
    if (SUCCEEDED(ctx->GetStatus(&status)) && (status.dwDynamicFlags & TF_SD_READONLY)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Keys

// A key that ends the open letter but types nothing itself (Enter, an arrow,
// Ctrl+C) has to reach the application after the letter. Neither of the two
// plain ways does that:
//   - finishing the letter in OnTestKeyDown and answering "not mine": the key
//     goes straight on, and our text, which travels another way, arrives
//     behind it (Enter ran ahead of the last letter of a line);
//   - claiming the key and answering "not eaten" from OnKeyDown: the key is
//     never delivered at all.
// So the key is claimed, the letter is finished in OnKeyDown, and the key is
// pressed again through the system; it comes back with nothing open and is
// let through. The same is done for any claimed key that turns out not to be
// ours after all, so that no key is ever lost.
bool TextService::CanResend(WPARAM vk) const {
    // Text sent as packets carries its character where we cannot read it.
    return canResend_ && vk != VK_PACKET && vk != VK_PROCESSKEY;
}

void TextService::Resend(WPARAM vk, LPARAM lp) {
    INPUT in[2] = {};
    for (INPUT& i : in) {
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = (WORD)vk;
        i.ki.wScan = (WORD)((lp >> 16) & 0xFF);
        i.ki.dwFlags = (lp & (1 << 24)) ? KEYEVENTF_EXTENDEDKEY : 0;
    }
    // Still held down: let it go and press it again, so that the application
    // sees a fresh press and not a repeat (the user's own release ends it).
    // Already released: a whole press.
    const bool held = (GetAsyncKeyState((int)vk) & 0x8000) != 0;
    in[held ? 0 : 1].ki.dwFlags |= KEYEVENTF_KEYUP;

    resendVk_ = vk;
    resendTick_ = GetTickCount();
    resendSeen_ = false;
    if (SendInput(2, in, sizeof(INPUT)) != 2) {
        TRACE("Resend vk=%02x failed (%lu)", (unsigned)vk, GetLastError());
        canResend_ = false;  // not allowed in this process: do not claim such keys again
        resendVk_ = 0;
    }
}

bool TextService::IsResent(WPARAM vk) {
    if (!resendVk_) return false;
    if (GetTickCount() - resendTick_ > 2000) {
        resendVk_ = 0;
        return false;
    }
    return vk == resendVk_;
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext* ctx, WPARAM wp, LPARAM lp, BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    *eaten = FALSE;
    claimedVk_ = 0;

    if (IsResent(wp)) {
        // Our own press coming back: it belongs to the application now.
        resendSeen_ = true;
        if (composition_) RequestEndComposition();
        engine_.reset();
        return S_OK;
    }

    if (!CanHandle(ctx)) {
        if (composition_) RequestEndComposition();
        engine_.reset();
        return S_OK;
    }

    const KeyInfo ki = ClassifyKey(wp, lp, mode_);
    if (mode_ == Mode::Manual) {
        // No digraphs here: a key either types its own character or is one of
        // the moved keys, which the service types itself in OnKeyDown.
        *eaten = ki.overrideLayout ? TRUE : FALSE;
        if (*eaten) claimedVk_ = wp;
        return S_OK;
    }
    SyncPrevious(ctx, ki);
    const Action peek = engine_.peek(ki.key);
    const bool endsLetter = peek.commit && !peek.eat;  // Enter, an arrow, Ctrl+C... with a letter open
    const bool claim = peek.eat || ki.overrideLayout || (endsLetter && CanResend(wp));
    TRACE("TestKeyDown vk=%02x kind=%d ch=%04x override=%d -> claim=%d", (unsigned)wp, (int)ki.key.kind,
          (unsigned)ki.key.ch, (int)ki.overrideLayout, (int)claim);
    if (claim) {
        // The document is only ever changed in OnKeyDown: text written from
        // here reaches the application behind the key it belongs in front of.
        claimedVk_ = wp;
        *eaten = TRUE;
        return S_OK;
    }

    // Not ours. The engine still takes note of the key (the apostrophe rule
    // looks at the letter before it): hosts do not call OnKeyDown for a key
    // that was not claimed here. Where keys cannot be pressed again, a key
    // that ends the open letter finishes it from here, the old way.
    const Action a = engine_.apply(ki.key);
    if (!a.nothing() && FAILED(Perform(ctx, a, 0))) {
        AbandonComposition();
        engine_.reset();
    }
    return S_OK;
}

STDMETHODIMP TextService::OnKeyDown(ITfContext* ctx, WPARAM wp, LPARAM lp, BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    *eaten = FALSE;
    const bool claimed = claimedVk_ != 0 && claimedVk_ == wp;
    claimedVk_ = 0;

    if (IsResent(wp)) {
        resendSeen_ = true;
        if (composition_) RequestEndComposition();
        engine_.reset();
        return S_OK;
    }

    KeyDown(ctx, wp, lp, eaten);

    // The host was told this key is ours. Whatever we did not type ourselves
    // goes back to the application, behind any text written just now.
    if (!*eaten && claimed && CanResend(wp)) {
        Resend(wp, lp);
        if (resendVk_) *eaten = TRUE;
    }
    return S_OK;
}

void TextService::KeyDown(ITfContext* ctx, WPARAM wp, LPARAM lp, BOOL* eaten) {
    if (!CanHandle(ctx)) {
        if (composition_) RequestEndComposition();
        engine_.reset();
        return;
    }

    const KeyInfo ki = ClassifyKey(wp, lp, mode_);
    if (mode_ == Mode::Manual) {
        if (!ki.overrideLayout) return;
        const HRESULT hrManual = Perform(ctx, Action(), ki.wanted);
        TRACE("KeyDown (manual) vk=%02x -> %04x: %08lx", (unsigned)wp, (unsigned)ki.wanted, hrManual);
        *eaten = SUCCEEDED(hrManual) ? TRUE : FALSE;
        return;
    }
    SyncPrevious(ctx, ki);
    const Action a = engine_.apply(ki.key);
    // A key the engine passes, on a base layout that would type something
    // else: type the wanted character ourselves.
    const wchar_t literal = (!a.eat && ki.overrideLayout) ? ki.wanted : 0;
    if (a.nothing() && !literal) return;

    const HRESULT hr = Perform(ctx, a, literal);
    TRACE("KeyDown vk=%02x edit=%d commit=%d text=%04x literal=%04x -> %08lx", (unsigned)wp, (int)a.edit,
          (int)a.commit, (unsigned)a.text[0], (unsigned)literal, hr);
    if (FAILED(hr)) {
        // No document lock: plain typing for this key.
        AbandonComposition();
        engine_.reset();
        return;
    }
    *eaten = a.eat || literal != 0;
}

// The release of a key we pressed again ourselves ends its special treatment.
STDMETHODIMP TextService::OnTestKeyUp(ITfContext*, WPARAM wp, LPARAM, BOOL* eaten) {
    if (eaten) *eaten = FALSE;
    if (resendVk_ == wp && resendSeen_) resendVk_ = 0;
    return S_OK;
}

STDMETHODIMP TextService::OnKeyUp(ITfContext*, WPARAM wp, LPARAM, BOOL* eaten) {
    if (eaten) *eaten = FALSE;
    if (resendVk_ == wp && resendSeen_) resendVk_ = 0;
    return S_OK;
}

STDMETHODIMP TextService::OnPreservedKey(ITfContext*, REFGUID, BOOL* eaten) {
    if (eaten) *eaten = FALSE;
    return S_OK;
}

HRESULT TextService::Perform(ITfContext* ctx, const Action& a, wchar_t literal) {
    EditSession* es = new (std::nothrow) EditSession(this, ctx, SessionKind::Action, a, literal);
    if (!es) return E_OUTOFMEMORY;
    HRESULT hrSession = E_FAIL;
    const HRESULT hr = ctx->RequestEditSession(clientId_, es, TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
    es->Release();
    return FAILED(hr) ? hr : hrSession;
}

// ---------------------------------------------------------------------------
// Document edits (inside an edit session)

HRESULT TextService::DoAction(TfEditCookie ec, ITfContext* ctx, const Action& a, wchar_t literal) {
    const bool expectsOpen = a.commit || a.edit == Edit::Replace || a.edit == Edit::ReplaceAndEnd;
    // A leftover composition the engine has already forgotten (focus moved and
    // its asynchronous end has not run yet): close it first.
    if (composition_ && !expectsOpen) EndComposition(ec);
    if (a.commit) EndComposition(ec);

    HRESULT hr = S_OK;
    switch (a.edit) {
        case Edit::None:
            break;
        case Edit::Start:
            hr = StartComposition(ec, ctx);
            if (SUCCEEDED(hr)) hr = SetCompositionText(ec, ctx, a.text);
            break;
        case Edit::Replace:
            if (composition_) {
                hr = SetCompositionText(ec, ctx, a.text);
            } else {
                hr = InsertPlain(ec, ctx, a.text);
                engine_.reset();
            }
            break;
        case Edit::ReplaceAndEnd:
            if (composition_) {
                hr = SetCompositionText(ec, ctx, a.text);
                EndComposition(ec);
            } else if (a.text[0]) {
                hr = InsertPlain(ec, ctx, a.text);
            }
            break;
        case Edit::Insert:
            hr = InsertPlain(ec, ctx, a.text);
            break;
    }

    if (SUCCEEDED(hr) && literal) {
        const wchar_t text[2] = {literal, 0};
        hr = InsertPlain(ec, ctx, text);
    }
    return hr;
}

HRESULT TextService::DoEndComposition(TfEditCookie ec) {
    EndComposition(ec);
    return S_OK;
}

HRESULT TextService::StartComposition(TfEditCookie ec, ITfContext* ctx) {
    ITfInsertAtSelection* insert = nullptr;
    ITfContextComposition* compositions = nullptr;
    ITfRange* range = nullptr;

    HRESULT hr = ctx->QueryInterface(__uuidof(ITfInsertAtSelection), (void**)&insert);
    if (SUCCEEDED(hr)) hr = insert->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, &range);
    if (SUCCEEDED(hr)) hr = ctx->QueryInterface(__uuidof(ITfContextComposition), (void**)&compositions);
    if (SUCCEEDED(hr)) {
        hr = compositions->StartComposition(ec, range, static_cast<ITfCompositionSink*>(this), &composition_);
    }
    if (SUCCEEDED(hr) && !composition_) hr = E_FAIL;  // the application refused
    if (SUCCEEDED(hr)) {
        compositionCtx_ = ctx;
        compositionCtx_->AddRef();
    }

    if (range) range->Release();
    if (compositions) compositions->Release();
    if (insert) insert->Release();
    return hr;
}

HRESULT TextService::SetCompositionText(TfEditCookie ec, ITfContext* ctx, const wchar_t* text) {
    if (!composition_) return E_UNEXPECTED;
    ITfRange* range = nullptr;
    HRESULT hr = composition_->GetRange(&range);
    if (FAILED(hr) || !range) return FAILED(hr) ? hr : E_FAIL;

    hr = range->SetText(ec, 0, text, lstrlenW(text));
    if (SUCCEEDED(hr)) {
        SetDisplayAttribute(ec, ctx, range, true);

        // Caret after the text.
        ITfRange* caret = nullptr;
        if (SUCCEEDED(range->Clone(&caret)) && caret) {
            caret->Collapse(ec, TF_ANCHOR_END);
            TF_SELECTION sel = {};
            sel.range = caret;
            sel.style.ase = TF_AE_NONE;
            sel.style.fInterimChar = FALSE;
            ctx->SetSelection(ec, 1, &sel);
            caret->Release();
        }
    }
    range->Release();
    return hr;
}

void TextService::EndComposition(TfEditCookie ec) {
    if (!composition_) return;
    ITfRange* range = nullptr;
    if (compositionCtx_ && SUCCEEDED(composition_->GetRange(&range)) && range) {
        SetDisplayAttribute(ec, compositionCtx_, range, false);
        range->Release();
    }
    composition_->EndComposition(ec);
    AbandonComposition();
}

// Plain committed text, delivered as a start-set-end composition (the form
// every application understands).
HRESULT TextService::InsertPlain(TfEditCookie ec, ITfContext* ctx, const wchar_t* text) {
    if (composition_) EndComposition(ec);
    HRESULT hr = StartComposition(ec, ctx);
    if (SUCCEEDED(hr)) hr = SetCompositionText(ec, ctx, text);
    EndComposition(ec);
    return hr;
}

void TextService::SetDisplayAttribute(TfEditCookie ec, ITfContext* ctx, ITfRange* range, bool set) {
    if (displayAttrAtom_ == TF_INVALID_GUIDATOM) return;
    ITfProperty* prop = nullptr;
    if (FAILED(ctx->GetProperty(GUID_PROP_ATTRIBUTE, &prop)) || !prop) return;
    if (set) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = (LONG)displayAttrAtom_;
        prop->SetValue(ec, range, &v);
    } else {
        prop->Clear(ec, range);
    }
    prop->Release();
}

// Drop our references without touching the document.
void TextService::AbandonComposition() {
    if (composition_) {
        composition_->Release();
        composition_ = nullptr;
    }
    if (compositionCtx_) {
        compositionCtx_->Release();
        compositionCtx_ = nullptr;
    }
}

// End the open composition as soon as its document can be locked.
void TextService::RequestEndComposition() {
    if (!composition_) return;
    if (!compositionCtx_) {
        AbandonComposition();
        return;
    }
    EditSession* es = new (std::nothrow) EditSession(this, compositionCtx_, SessionKind::EndComposition);
    if (!es) {
        AbandonComposition();
        return;
    }
    HRESULT hrSession = E_FAIL;
    const HRESULT hr = compositionCtx_->RequestEditSession(clientId_, es,
                                                           TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hrSession);
    es->Release();
    if (FAILED(hr)) AbandonComposition();
}

// The application ended our composition (click elsewhere, focus loss...).
STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie ec, ITfComposition* composition) {
    ITfRange* range = nullptr;
    if (composition && compositionCtx_ && SUCCEEDED(composition->GetRange(&range)) && range) {
        SetDisplayAttribute(ec, compositionCtx_, range, false);
        range->Release();
    }
    TRACE("OnCompositionTerminated");
    AbandonComposition();
    engine_.reset();
    return S_OK;
}

// ---------------------------------------------------------------------------
// Display attributes

STDMETHODIMP TextService::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) {
    if (!ppEnum) return E_INVALIDARG;
    return CreateDisplayAttributeEnum(ppEnum);
}

STDMETHODIMP TextService::GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** ppInfo) {
    if (!ppInfo) return E_INVALIDARG;
    *ppInfo = nullptr;
    if (!IsEqualGUID(guid, GUID_UzAlphaDisplayAttr)) return E_INVALIDARG;
    return CreateDisplayAttributeInfo(ppInfo);
}
