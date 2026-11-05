// Display attribute for the pending letter: no underline, no colours, so the
// short-lived composition looks like ordinary typed text.
#include <new>

#include "textservice.h"

namespace {

class DisplayAttributeInfo : public ITfDisplayAttributeInfo {
public:
    DisplayAttributeInfo() { DllAddRef(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(ITfDisplayAttributeInfo))) {
            *ppv = static_cast<ITfDisplayAttributeInfo*>(this);
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

    STDMETHODIMP GetGUID(GUID* guid) override {
        if (!guid) return E_INVALIDARG;
        *guid = GUID_UzAlphaDisplayAttr;
        return S_OK;
    }
    STDMETHODIMP GetDescription(BSTR* desc) override {
        if (!desc) return E_INVALIDARG;
        *desc = SysAllocString(L"UzAlpha pending letter");
        return *desc ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP GetAttributeInfo(TF_DISPLAYATTRIBUTE* da) override {
        if (!da) return E_INVALIDARG;
        ZeroMemory(da, sizeof *da);
        da->crText.type = TF_CT_NONE;
        da->crBk.type = TF_CT_NONE;
        da->crLine.type = TF_CT_NONE;
        da->lsStyle = TF_LS_NONE;
        da->fBoldLine = FALSE;
        da->bAttr = TF_ATTR_INPUT;
        return S_OK;
    }
    STDMETHODIMP SetAttributeInfo(const TF_DISPLAYATTRIBUTE*) override { return E_NOTIMPL; }
    STDMETHODIMP Reset() override { return S_OK; }

private:
    ~DisplayAttributeInfo() { DllRelease(); }
    LONG ref_ = 1;
};

class DisplayAttributeEnum : public IEnumTfDisplayAttributeInfo {
public:
    explicit DisplayAttributeEnum(ULONG index = 0) : index_(index) { DllAddRef(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IEnumTfDisplayAttributeInfo))) {
            *ppv = static_cast<IEnumTfDisplayAttributeInfo*>(this);
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

    STDMETHODIMP Clone(IEnumTfDisplayAttributeInfo** ppEnum) override {
        if (!ppEnum) return E_INVALIDARG;
        *ppEnum = new (std::nothrow) DisplayAttributeEnum(index_);
        return *ppEnum ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP Next(ULONG count, ITfDisplayAttributeInfo** rgInfo, ULONG* fetched) override {
        ULONG got = 0;
        if (rgInfo && count > 0 && index_ == 0) {
            if (FAILED(CreateDisplayAttributeInfo(&rgInfo[0]))) return E_OUTOFMEMORY;
            index_ = 1;
            got = 1;
        }
        if (fetched) *fetched = got;
        return got == count ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override {
        index_ = 0;
        return S_OK;
    }
    STDMETHODIMP Skip(ULONG count) override {
        if (count > 0 && index_ == 0) index_ = 1;
        return S_OK;
    }

private:
    ~DisplayAttributeEnum() { DllRelease(); }
    LONG ref_ = 1;
    ULONG index_;
};

}  // namespace

HRESULT CreateDisplayAttributeInfo(ITfDisplayAttributeInfo** ppInfo) {
    *ppInfo = new (std::nothrow) DisplayAttributeInfo();
    return *ppInfo ? S_OK : E_OUTOFMEMORY;
}

HRESULT CreateDisplayAttributeEnum(IEnumTfDisplayAttributeInfo** ppEnum) {
    *ppEnum = new (std::nothrow) DisplayAttributeEnum();
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}
