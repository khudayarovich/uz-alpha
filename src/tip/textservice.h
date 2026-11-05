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

// What one key press means to the engine, plus what the active keyboard
// layout would type for it.
struct KeyInfo {
    uza::Key key;
    wchar_t usChar = 0;  // the US-English character of this physical key (0 if none)
    // True when the base layout would type something other than the US
    // character: the text service then types the US character itself, so Auto
    // is always plain QWERTY whatever layout Windows put underneath it.
    bool overrideLayout = false;
};

class TextService : public ITfTextInputProcessorEx,
                    public ITfThreadMgrEventSink,
                    public ITfThreadFocusSink,
                    public ITfKeyEventSink,
                    public ITfCompositionSink,
                    public ITfDisplayAttributeProvider {
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
    bool keySinkAdvised_ = false;

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
KeyInfo ClassifyKey(WPARAM vk, LPARAM lp);

// Display attribute objects (displayattr.cpp).
HRESULT CreateDisplayAttributeInfo(ITfDisplayAttributeInfo** ppInfo);
HRESULT CreateDisplayAttributeEnum(IEnumTfDisplayAttributeInfo** ppEnum);
