#pragma once

/**
 * @file uia.h
 * @brief The terminal as UI Automation sees it, so Narrator and NVDA can read it.
 */

#include "pane.h"
#include "uia_host.h"
#include "uia_text.h"

#include <windows.h>
#include <ole2.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace wbshterm {

	/**
	 * @brief The window's one automation element, carrying the text pattern.
	 *
	 * The text is the focused pane's, history included. COM threading is
	 * asked for, so every call comes in on the window's own thread and
	 * reads the screen as safely as painting does. Once the window goes,
	 * disconnect() turns every later call into "element not available".
	 */
	class UiaRoot : public IRawElementProviderSimple, public ITextProvider {
	public:
		explicit UiaRoot(UiaHost* host) : host_(host) {}

		UiaHost* host() const { return host_; }
		void disconnect() { host_ = nullptr; }

		IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override;
		IFACEMETHODIMP_(ULONG) AddRef() override;
		IFACEMETHODIMP_(ULONG) Release() override;

		IFACEMETHODIMP get_ProviderOptions(ProviderOptions* out) override;
		IFACEMETHODIMP GetPatternProvider(PATTERNID pattern, IUnknown** out) override;
		IFACEMETHODIMP GetPropertyValue(PROPERTYID property, VARIANT* out) override;
		IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** out) override;

		IFACEMETHODIMP GetSelection(SAFEARRAY** out) override;
		IFACEMETHODIMP GetVisibleRanges(SAFEARRAY** out) override;
		IFACEMETHODIMP RangeFromChild(IRawElementProviderSimple* child,
			ITextRangeProvider** out) override;
		IFACEMETHODIMP RangeFromPoint(UiaPoint point, ITextRangeProvider** out) override;
		IFACEMETHODIMP get_DocumentRange(ITextRangeProvider** out) override;
		IFACEMETHODIMP get_SupportedTextSelection(SupportedTextSelection* out) override;

	private:
		virtual ~UiaRoot() = default;

		HRESULT singleRange(Pane& pane, long long start, long long end, SAFEARRAY** out);

		std::atomic<ULONG> references_{ 1 };
		UiaHost*           host_ = nullptr;
	};

	/** A text range over one pane, as positions of a TextModel. */
	ITextRangeProvider* createTextRange(UiaRoot* root, Pane& pane, long long start,
		long long end);

} /* namespace wbshterm */
