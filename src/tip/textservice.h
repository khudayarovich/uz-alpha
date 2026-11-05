// The Auto keyboard: a TSF text input processor that turns sh/ch/o'/g' into
// ş/ç/ö/ğ while typing. See src/engine/engine.h for the rules.
#pragma once

#include <windows.h>
#include <msctf.h>

#include "../common/ids.h"
#include "../engine/engine.h"

extern HINSTANCE g_hInst;
void DllAddRef();
void DllRelease();

// The two keyboards this service provides; each is a TSF language profile.
enum class Mode { Auto, Manual };

// What one key press means, plus what the keyboard layout Windows put
// underneath the service would type for it.
struct KeyInfo {
    uza::Key key;
    wchar_t wanted = 0;  // the character this key should type in the current mode (0 if none)
    // True when the base layout would type something else: the text service
    // then types `wanted` itself. That is how Manual moves its keys, and how
    // both keyboards stay US QWERTY whatever layout sits underneath.
    bool overrideLayout = false;
};

// Lets the test harness pick the keyboard without registering the real profiles.
struct __declspec(uuid("08E01424-6802-4FF1-8190-3CE8DEACC6CB")) IUzAlphaMode : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetManual(BOOL manual) = 0;
};

class TextService : public ITfTextInputProcessorEx,
                    public ITfThreadMgrEventSink,
                    public ITfThreadFocusSink,
                    public ITfActiveLanguageProfileNotifySink,
                    public ITfKeyEventSink,
                    public ITfCompositionSink,
                    public ITfDisplayAttributeProvider,
                    public IUzAlphaMode {
public:
    TextService();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfTextInputProcessor / ITfTextInputProcessorEx
    STDMETHODIMP Activate(ITfThreadMgr* tm, TfClientId tid) override;
    STDMETHODIMP Deactivate() override;
    STDMETHODIMP ActivateEx(ITfThreadMgr* tm, TfClientId tid, DWORD flags) override;

    // ITfThreadMgrEventSink
    STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
    STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
    STDMETHODIMP OnSetFocus(ITfDocumentMgr* focus, ITfDocumentMgr* prev) override;
    STDMETHODIMP OnPushContext(ITfContext* ctx) override;
    STDMETHODIMP OnPopContext(ITfContext*) override { return S_OK; }

    // ITfThreadFocusSink
    STDMETHODIMP OnSetThreadFocus() override;
    STDMETHODIMP OnKillThreadFocus() override;

    // ITfActiveLanguageProfileNotifySink: the user moved between our two keyboards
    STDMETHODIMP OnActivated(REFCLSID clsid, REFGUID profile, BOOL activated) override;

    // IUzAlphaMode
    STDMETHODIMP SetManual(BOOL manual) override;

    // ITfKeyEventSink
    STDMETHODIMP OnSetFocus(BOOL foreground) override;
    STDMETHODIMP OnTestKeyDown(ITfContext* ctx, WPARAM wp, LPARAM lp, BOOL* eaten) override;
    STDMETHODIMP OnTestKeyUp(ITfContext* ctx, WPARAM wp, LPARAM lp, BOOL* eaten) override;
    STDMETHODIMP OnKeyDown(ITfContext* ctx, WPARAM wp, LPARAM lp, BOOL* eaten) override;
    STDMETHODIMP OnKeyUp(ITfContext* ctx, WPARAM wp, LPARAM lp, BOOL* eaten) override;
    STDMETHODIMP OnPreservedKey(ITfContext* ctx, REFGUID guid, BOOL* eaten) override;

    // ITfCompositionSink
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ec, ITfComposition* composition) override;

    // ITfDisplayAttributeProvider
    STDMETHODIMP EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) override;
    STDMETHODIMP GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** ppInfo) override;

    // Called back from edit sessions (they hold the document lock).
    HRESULT DoAction(TfEditCookie ec, ITfContext* ctx, const uza::Action& a, wchar_t literal);
    HRESULT DoEndComposition(TfEditCookie ec);
    HRESULT DoReadInputScope(TfEditCookie ec, ITfContext* ctx);
    HRESULT DoReadPrevious(TfEditCookie ec, ITfContext* ctx);

private:
    ~TextService();

    bool CanHandle(ITfContext* ctx);
    HRESULT Perform(ITfContext* ctx, const uza::Action& a, wchar_t literal);
    void AbandonComposition();
    void RequestEndComposition();
    void RequestInputScope(ITfContext* ctx);
    void SyncPrevious(ITfContext* ctx, const KeyInfo& ki);
    void PublishActive(bool active);
    void SetMode(Mode mode);
    void ReadActiveProfile();

    HRESULT StartComposition(TfEditCookie ec, ITfContext* ctx);
    HRESULT SetCompositionText(TfEditCookie ec, ITfContext* ctx, const wchar_t* text);
    void EndComposition(TfEditCookie ec);
    HRESULT InsertPlain(TfEditCookie ec, ITfContext* ctx, const wchar_t* text);
    void SetDisplayAttribute(TfEditCookie ec, ITfContext* ctx, ITfRange* range, bool set);

    LONG ref_ = 1;
    ITfThreadMgr* threadMgr_ = nullptr;
    TfClientId clientId_ = TF_CLIENTID_NULL;
    DWORD threadMgrSinkCookie_ = TF_INVALID_COOKIE;
    DWORD threadFocusSinkCookie_ = TF_INVALID_COOKIE;
    DWORD profileSinkCookie_ = TF_INVALID_COOKIE;
    bool keySinkAdvised_ = false;

    Mode mode_ = Mode::Auto;
    uza::Engine engine_;
    ITfComposition* composition_ = nullptr;
    ITfContext* compositionCtx_ = nullptr;  // context that owns composition_

    // Context found to be a password/PIN field; compared by address only.
    ITfContext* bypassCtx_ = nullptr;
    // Result of the last DoReadPrevious.
    bool previousKnown_ = false;
    wchar_t previousChar_ = 0;
    TfGuidAtom displayAttrAtom_ = TF_INVALID_GUIDATOM;
};

// Key classification (keys.cpp).
KeyInfo ClassifyKey(WPARAM vk, LPARAM lp, Mode mode);

// Display attribute objects (displayattr.cpp).
HRESULT CreateDisplayAttributeInfo(ITfDisplayAttributeInfo** ppInfo);
HRESULT CreateDisplayAttributeEnum(IEnumTfDisplayAttributeInfo** ppEnum);
