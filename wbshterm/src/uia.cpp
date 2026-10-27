/**
 * @file uia.cpp
 * @brief The automation element of the window and its text pattern.
 */

#include "uia.h"

#include <algorithm>

#pragma comment(lib, "uiautomationcore.lib")

namespace wbshterm {

	static HRESULT putBool(VARIANT* out, bool value) {
		out->vt      = VT_BOOL;
		out->boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
		return S_OK;
	}

	static HRESULT putString(VARIANT* out, const wchar_t* text) {
		out->bstrVal = ::SysAllocString(text);
		if (out->bstrVal == nullptr) return E_OUTOFMEMORY;

		out->vt = VT_BSTR;
		return S_OK;
	}

	static long long positionOf(int row, int column, int columns) {
		return static_cast<long long>(row) * std::max(1, columns) + column;
	}

	IFACEMETHODIMP UiaRoot::QueryInterface(REFIID riid, void** out) {
		if (out == nullptr) return E_POINTER;

		*out = nullptr;
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) {
			*out = static_cast<IRawElementProviderSimple*>(this);
		} else if (riid == __uuidof(ITextProvider)) {
			*out = static_cast<ITextProvider*>(this);
		} else {
			return E_NOINTERFACE;
		}

		AddRef();
		return S_OK;
	}

	IFACEMETHODIMP_(ULONG) UiaRoot::AddRef() {
		return ++references_;
	}

	IFACEMETHODIMP_(ULONG) UiaRoot::Release() {
		const ULONG left = --references_;
		if (left == 0) delete this;
		return left;
	}

	// COM threading routes every call through the window's own thread,
	// which is the only one that may read a screen.
	IFACEMETHODIMP UiaRoot::get_ProviderOptions(ProviderOptions* out) {
		if (out == nullptr) return E_POINTER;

		*out = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider
			| ProviderOptions_UseComThreading);
		return S_OK;
	}

	IFACEMETHODIMP UiaRoot::GetPatternProvider(PATTERNID pattern, IUnknown** out) {
		if (out == nullptr) return E_POINTER;

		*out = nullptr;
		if (host_ == nullptr) return UIA_E_ELEMENTNOTAVAILABLE;
		if (pattern != UIA_TextPatternId) return S_OK;

		*out = static_cast<ITextProvider*>(this);
		AddRef();
		return S_OK;
	}

	IFACEMETHODIMP UiaRoot::GetPropertyValue(PROPERTYID property, VARIANT* out) {
		if (out == nullptr) return E_POINTER;

		out->vt = VT_EMPTY;
		if (host_ == nullptr) return UIA_E_ELEMENTNOTAVAILABLE;

		switch (property) {
		case UIA_ControlTypePropertyId:
			out->vt   = VT_I4;
			out->lVal = UIA_TextControlTypeId;
			return S_OK;
		case UIA_NamePropertyId:
			return putString(out, L"wbshterm");
		case UIA_AutomationIdPropertyId:
			return putString(out, L"Terminal");
		case UIA_ProviderDescriptionPropertyId:
			return putString(out, L"wbshterm terminal text");
		case UIA_IsKeyboardFocusablePropertyId:
		case UIA_IsTextPatternAvailablePropertyId:
			return putBool(out, true);
		case UIA_HasKeyboardFocusPropertyId:
			return putBool(out, ::GetFocus() == host_->uiaWindow());
		default:
			return S_OK;
		}
	}

	IFACEMETHODIMP UiaRoot::get_HostRawElementProvider(IRawElementProviderSimple** out) {
		if (out == nullptr) return E_POINTER;

		*out = nullptr;
		if (host_ == nullptr) return UIA_E_ELEMENTNOTAVAILABLE;
		return ::UiaHostProviderFromHwnd(host_->uiaWindow(), out);
	}

	HRESULT UiaRoot::singleRange(Pane& pane, long long start, long long end, SAFEARRAY** out) {
		*out = nullptr;
		SAFEARRAY* ranges = ::SafeArrayCreateVector(VT_UNKNOWN, 0, 1);
		if (ranges == nullptr) return E_OUTOFMEMORY;

		ITextRangeProvider* range = createTextRange(this, pane, start, end);
		LONG index = 0;
		const HRESULT result = ::SafeArrayPutElement(ranges, &index, range);
		range->Release();
		if (FAILED(result)) {
			::SafeArrayDestroy(ranges);
			return result;
		}

		*out = ranges;
		return S_OK;
	}

	// With nothing selected the selection is the cursor, as a range with
	// no width: that is how a screen reader follows the caret.
	IFACEMETHODIMP UiaRoot::GetSelection(SAFEARRAY** out) {
		if (out == nullptr) return E_POINTER;

		Pane* pane = host_ != nullptr ? host_->uiaFocusedPane() : nullptr;
		if (pane == nullptr) return UIA_E_ELEMENTNOTAVAILABLE;

		const Screen& screen = pane->screen();
		const TerminalView& view = pane->view();
		if (view.hasSelection()) {
			const GridPoint first = view.selectionStart();
			const GridPoint last  = view.selectionEnd();
			return singleRange(*pane, positionOf(first.row, first.column, screen.columns()),
				positionOf(last.row, last.column, screen.columns()) + 1, out);
		}

		const long long caret = positionOf(screen.scrollbackRows() + screen.cursor().row,
			screen.cursor().column, screen.columns());
		return singleRange(*pane, caret, caret, out);
	}

	IFACEMETHODIMP UiaRoot::GetVisibleRanges(SAFEARRAY** out) {
		if (out == nullptr) return E_POINTER;

		Pane* pane = host_ != nullptr ? host_->uiaFocusedPane() : nullptr;
		if (pane == nullptr) return UIA_E_ELEMENTNOTAVAILABLE;

		const Screen& screen = pane->screen();
		const int top = pane->view().topRow(screen);
		return singleRange(*pane, positionOf(top, 0, screen.columns()),
			positionOf(top + screen.rows(), 0, screen.columns()), out);
	}

	IFACEMETHODIMP UiaRoot::RangeFromChild(IRawElementProviderSimple*,
			ITextRangeProvider** out) {
		if (out == nullptr) return E_POINTER;

		*out = nullptr;
		return E_INVALIDARG;
	}

	// A point between panes or outside them all lands on the focused
	// pane's nearest cell.
	IFACEMETHODIMP UiaRoot::RangeFromPoint(UiaPoint point, ITextRangeProvider** out) {
		if (out == nullptr) return E_POINTER;

		*out = nullptr;
		Pane* focused = host_ != nullptr ? host_->uiaFocusedPane() : nullptr;
		if (focused == nullptr) return UIA_E_ELEMENTNOTAVAILABLE;

		POINT client = { static_cast<LONG>(point.x), static_cast<LONG>(point.y) };
		::ScreenToClient(host_->uiaWindow(), &client);

		const UiaPaneGeometry window = host_->uiaGeometry(*focused);
		const float scale = static_cast<float>(USER_DEFAULT_SCREEN_DPI)
			/ static_cast<float>(window.dpi);
		const float x = static_cast<float>(client.x) * scale;
		const float y = static_cast<float>(client.y) * scale;

		Pane* under = host_->uiaPaneAt(x, y);
		Pane& pane = under != nullptr ? *under : *focused;
		const UiaPaneGeometry geometry = host_->uiaGeometry(pane);
		const Screen& screen = pane.screen();
		const int column = std::min(std::max(static_cast<int>((x - geometry.left)
			/ geometry.cell_width), 0), screen.columns() - 1);
		const int row = std::min(std::max(static_cast<int>((y - geometry.top)
			/ geometry.cell_height), 0), screen.rows() - 1);

		const long long at = positionOf(pane.view().topRow(screen) + row, column, screen.columns());
		*out = createTextRange(this, pane, at, at);
		return S_OK;
	}

	IFACEMETHODIMP UiaRoot::get_DocumentRange(ITextRangeProvider** out) {
		if (out == nullptr) return E_POINTER;

		*out = nullptr;
		Pane* pane = host_ != nullptr ? host_->uiaFocusedPane() : nullptr;
		if (pane == nullptr) return UIA_E_ELEMENTNOTAVAILABLE;

		const TextModel text(pane->screen(), pane->screen().rows());
		*out = createTextRange(this, *pane, 0, text.end());
		return S_OK;
	}

	IFACEMETHODIMP UiaRoot::get_SupportedTextSelection(SupportedTextSelection* out) {
		if (out == nullptr) return E_POINTER;

		*out = SupportedTextSelection_Single;
		return S_OK;
	}

	bool Accessibility::answerGetObject(UiaHost* host, WPARAM wparam, LPARAM lparam,
			LRESULT& out_result) {
		if (static_cast<long>(lparam) != static_cast<long>(UiaRootObjectId)) return false;

		if (root_ == nullptr) root_ = new UiaRoot(host);
		out_result = ::UiaReturnRawElementProvider(host->uiaWindow(), wparam, lparam, root_);
		return true;
	}

	void Accessibility::textChanged() {
		if (root_ == nullptr || !::UiaClientsAreListening()) return;

		::UiaRaiseAutomationEvent(root_, UIA_Text_TextChangedEventId);
		::UiaRaiseAutomationEvent(root_, UIA_Text_TextSelectionChangedEventId);
	}

	void Accessibility::selectionChanged() {
		if (root_ == nullptr || !::UiaClientsAreListening()) return;

		::UiaRaiseAutomationEvent(root_, UIA_Text_TextSelectionChangedEventId);
	}

	// Clients may hold on to the element past the window; from here on
	// they hear that it is gone rather than reaching into a dead window.
	void Accessibility::disconnect(HWND window) {
		if (root_ == nullptr) return;

		::UiaReturnRawElementProvider(window, 0, 0, nullptr);
		root_->disconnect();
		::UiaDisconnectProvider(root_);
		root_->Release();
		root_ = nullptr;
	}

	Accessibility::~Accessibility() {
		if (root_ == nullptr) return;

		root_->disconnect();
		root_->Release();
	}

} /* namespace wbshterm */
