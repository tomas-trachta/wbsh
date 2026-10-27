/**
 * @file uia_range.cpp
 * @brief ITextRangeProvider over one pane: positions that follow history as it scrolls away.
 */

#include "uia.h"

#include <algorithm>
#include <vector>

namespace wbshterm {

	namespace uia_detail {

		// Lets a range given back by UI Automation be recognised as one of
		// ours, and its positions read, without a public interface for it.
		static const IID kRangeIid = { 0x6a9f2c1e, 0x3b7d, 0x4e55,
			{ 0x9a, 0x41, 0x2f, 0x0c, 0x8e, 0x5b, 0x71, 0xd3 } };

		// A client asking for all of the text gets this much: history can
		// run to millions of rows, and reading them all would stall the
		// window that answers on its own thread.
		static const int kMaxTextLength = 1 << 20;

		static wbshterm::TextUnit unitOf(::TextUnit unit) {
			switch (unit) {
			case TextUnit_Character:
				return wbshterm::TextUnit::kCharacter;
			case TextUnit_Format:
				return wbshterm::TextUnit::kFormat;
			case TextUnit_Word:
				return wbshterm::TextUnit::kWord;
			case TextUnit_Line:
				return wbshterm::TextUnit::kLine;
			case TextUnit_Paragraph:
				return wbshterm::TextUnit::kParagraph;
			case TextUnit_Page:
				return wbshterm::TextUnit::kPage;
			case TextUnit_Document:
				return wbshterm::TextUnit::kDocument;
			default:
				return wbshterm::TextUnit::kDocument;
			}
		}

		static std::wstring lowered(const std::wstring& text) {
			std::wstring out(text.size(), L'\0');
			const int count = static_cast<int>(text.size());
			if (count == 0) return out;

			const int done = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(),
				count, &out[0], count, nullptr, nullptr, 0);
			return done == count ? out : text;
		}

		static LONG pixels(float dips, unsigned int dpi) {
			return static_cast<LONG>(dips * static_cast<float>(dpi)
				/ static_cast<float>(USER_DEFAULT_SCREEN_DPI) + 0.5f);
		}

	} /* namespace uia_detail */

	/**
	 * @brief Two positions of a pane's TextModel, end excluded.
	 *
	 * Every call first catches the positions up with the screen: rows
	 * that fell out of history move them up, and a new width keeps each
	 * on its row and column.
	 */
	class UiaRange : public ITextRangeProvider {
	public:
		UiaRange(UiaRoot* root, Pane& pane, long long start, long long end);

		IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override;
		IFACEMETHODIMP_(ULONG) AddRef() override;
		IFACEMETHODIMP_(ULONG) Release() override;

		IFACEMETHODIMP Clone(ITextRangeProvider** out) override;
		IFACEMETHODIMP Compare(ITextRangeProvider* other, BOOL* out) override;
		IFACEMETHODIMP CompareEndpoints(TextPatternRangeEndpoint which,
			ITextRangeProvider* other, TextPatternRangeEndpoint other_endpoint,
			int* out) override;
		IFACEMETHODIMP ExpandToEnclosingUnit(::TextUnit unit) override;
		IFACEMETHODIMP FindAttribute(TEXTATTRIBUTEID attribute, VARIANT value, BOOL backward,
			ITextRangeProvider** out) override;
		IFACEMETHODIMP FindText(BSTR text, BOOL backward, BOOL ignore_case,
			ITextRangeProvider** out) override;
		IFACEMETHODIMP GetAttributeValue(TEXTATTRIBUTEID attribute, VARIANT* out) override;
		IFACEMETHODIMP GetBoundingRectangles(SAFEARRAY** out) override;
		IFACEMETHODIMP GetEnclosingElement(IRawElementProviderSimple** out) override;
		IFACEMETHODIMP GetText(int max_length, BSTR* out) override;
		IFACEMETHODIMP Move(::TextUnit unit, int count, int* out) override;
		IFACEMETHODIMP MoveEndpointByUnit(TextPatternRangeEndpoint which, ::TextUnit unit,
			int count, int* out) override;
		IFACEMETHODIMP MoveEndpointByRange(TextPatternRangeEndpoint which,
			ITextRangeProvider* other, TextPatternRangeEndpoint other_endpoint) override;
		IFACEMETHODIMP Select() override;
		IFACEMETHODIMP AddToSelection() override;
		IFACEMETHODIMP RemoveFromSelection() override;
		IFACEMETHODIMP ScrollIntoView(BOOL align_to_top) override;
		IFACEMETHODIMP GetChildren(SAFEARRAY** out) override;

	private:
		virtual ~UiaRange() = default;

		bool refresh();
		long long rewidth(long long at, int columns) const;
		TextModel model() const;
		long long& endpoint(TextPatternRangeEndpoint which);
		void keepOrder(TextPatternRangeEndpoint moved);
		UiaRange* ours(ITextRangeProvider* other) const;
		void appendRowBox(std::vector<double>& boxes, int row, int first, int last) const;

		std::atomic<ULONG>              references_{ 1 };
		Microsoft::WRL::ComPtr<UiaRoot> root_;
		Pane*                           pane_;
		long long                       start_;
		long long                       end_;
		std::uint64_t                   forgotten_;
		int                             columns_;
	};

	UiaRange::UiaRange(UiaRoot* root, Pane& pane, long long start, long long end)
		: root_(root),
		  pane_(&pane),
		  start_(std::min(start, end)),
		  end_(std::max(start, end)),
		  forgotten_(pane.screen().forgottenRows()),
		  columns_(std::max(1, pane.screen().columns())) {
	}

	ITextRangeProvider* createTextRange(UiaRoot* root, Pane& pane, long long start,
			long long end) {
		return new UiaRange(root, pane, start, end);
	}

	IFACEMETHODIMP UiaRange::QueryInterface(REFIID riid, void** out) {
		if (out == nullptr) return E_POINTER;

		*out = nullptr;
		if (riid == __uuidof(IUnknown) || riid == __uuidof(ITextRangeProvider)) {
			*out = static_cast<ITextRangeProvider*>(this);
		} else if (riid == uia_detail::kRangeIid) {
			*out = this;
		} else {
			return E_NOINTERFACE;
		}

		AddRef();
		return S_OK;
	}

	IFACEMETHODIMP_(ULONG) UiaRange::AddRef() {
		return ++references_;
	}

	IFACEMETHODIMP_(ULONG) UiaRange::Release() {
		const ULONG left = --references_;
		if (left == 0) delete this;
		return left;
	}

	long long UiaRange::rewidth(long long at, int columns) const {
		const long long row    = at / columns_;
		const long long column = std::min<long long>(at % columns_, columns - 1);
		return row * columns + column;
	}

	bool UiaRange::refresh() {
		const UiaHost* host = root_->host();
		if (host == nullptr || !host->uiaPaneAlive(pane_)) return false;

		const Screen& screen = pane_->screen();
		const long long shift = static_cast<long long>(screen.forgottenRows() - forgotten_)
			* columns_;
		start_ -= shift;
		end_   -= shift;
		forgotten_ = screen.forgottenRows();

		const int columns = std::max(1, screen.columns());
		if (columns != columns_) {
			start_   = rewidth(std::max(0LL, start_), columns);
			end_     = rewidth(std::max(0LL, end_), columns);
			columns_ = columns;
		}

		const TextModel text = model();
		start_ = text.clamp(start_);
		end_   = std::max(start_, text.clamp(end_));
		return true;
	}

	TextModel UiaRange::model() const {
		return TextModel(pane_->screen(), pane_->screen().rows());
	}

	long long& UiaRange::endpoint(TextPatternRangeEndpoint which) {
		return which == TextPatternRangeEndpoint_Start ? start_ : end_;
	}

	// An endpoint moved past the other one drags it along.
	void UiaRange::keepOrder(TextPatternRangeEndpoint moved) {
		if (start_ <= end_) return;

		if (moved == TextPatternRangeEndpoint_Start) end_ = start_;
		else start_ = end_;
	}

	// The caller holds its own reference, so the one the lookup took is
	// handed straight back.
	UiaRange* UiaRange::ours(ITextRangeProvider* other) const {
		if (other == nullptr) return nullptr;

		void* found = nullptr;
		if (FAILED(other->QueryInterface(uia_detail::kRangeIid, &found))) return nullptr;

		UiaRange* range = static_cast<UiaRange*>(found);
		range->Release();
		if (range->pane_ != pane_ || !range->refresh()) return nullptr;
		return range;
	}

	IFACEMETHODIMP UiaRange::Clone(ITextRangeProvider** out) {
		if (out == nullptr) return E_POINTER;

		*out = nullptr;
		if (!refresh()) return UIA_E_ELEMENTNOTAVAILABLE;

		*out = new UiaRange(root_.Get(), *pane_, start_, end_);
		return S_OK;
	}

	IFACEMETHODIMP UiaRange::Compare(ITextRangeProvider* other, BOOL* out) {
		if (out == nullptr) return E_POINTER;
		if (!refresh()) return UIA_E_ELEMENTNOTAVAILABLE;

		const UiaRange* range = ours(other);
		*out = range != nullptr && range->start_ == start_ && range->end_ == end_;
		return S_OK;
	}

	IFACEMETHODIMP UiaRange::CompareEndpoints(TextPatternRangeEndpoint which,
			ITextRangeProvider* other, TextPatternRangeEndpoint other_endpoint, int* out) {
		if (out == nullptr) return E_POINTER;
		if (!refresh()) return UIA_E_ELEMENTNOTAVAILABLE;

		UiaRange* range = ours(other);
		if (range == nullptr) return E_INVALIDARG;

		const long long difference = endpoint(which) - range->endpoint(other_endpoint);
		*out = difference < 0 ? -1 : (difference > 0 ? 1 : 0);
		return S_OK;
	}

	IFACEMETHODIMP UiaRange::ExpandToEnclosingUnit(::TextUnit unit) {
		if (!refresh()) return UIA_E_ELEMENTNOTAVAILABLE;

		const TextModel text = model();
		start_ = text.unitStart(start_, uia_detail::unitOf(unit));
		end_   = text.nextBoundary(start_, uia_detail::unitOf(unit));
		return S_OK;
	}

	// Every cell reads the same to a screen reader, so no attribute is
	// ever found.
	IFACEMETHODIMP UiaRange::FindAttribute(TEXTATTRIBUTEID, VARIANT, BOOL,
			ITextRangeProvider** out) {
		if (out == nullptr) return E_POINTER;

		*out = nullptr;
		return refresh() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
	}

	IFACEMETHODIMP UiaRange::FindText(BSTR text, BOOL backward, BOOL ignore_case,
			ITextRangeProvider** out) {
		if (out == nullptr) return E_POINTER;

		*out = nullptr;
		if (!refresh()) return UIA_E_ELEMENTNOTAVAILABLE;
		if (text == nullptr) return E_INVALIDARG;

		std::vector<long long> positions;
		std::wstring haystack = model().text(start_, end_, uia_detail::kMaxTextLength, &positions);
		std::wstring needle(text, ::SysStringLen(text));
		if (needle.empty()) return E_INVALIDARG;

		if (ignore_case) {
			haystack = uia_detail::lowered(haystack);
			needle   = uia_detail::lowered(needle);
		}

		const std::size_t at = backward ? haystack.rfind(needle) : haystack.find(needle);
		if (at == std::wstring::npos) return S_OK;

		const std::size_t after = at + needle.size();
		const long long last = after < positions.size() ? positions[after] : end_;
		*out = new UiaRange(root_.Get(), *pane_, positions[at], last);
		return S_OK;
	}

	IFACEMETHODIMP UiaRange::GetAttributeValue(TEXTATTRIBUTEID attribute, VARIANT* out) {
		if (out == nullptr) return E_POINTER;

		out->vt = VT_EMPTY;
		if (!refresh()) return UIA_E_ELEMENTNOTAVAILABLE;

		if (attribute == UIA_IsReadOnlyAttributeId || attribute == UIA_IsHiddenAttributeId) {
			out->vt      = VT_BOOL;
			out->boolVal = VARIANT_FALSE;
			return S_OK;
		}

		out->vt = VT_UNKNOWN;
		return ::UiaGetReservedNotSupportedValue(&out->punkVal);
	}

	void UiaRange::appendRowBox(std::vector<double>& boxes, int row, int first, int last) const {
		const UiaHost& host = *root_->host();
		const UiaPaneGeometry geometry = host.uiaGeometry(*pane_);
		const int viewport_row = row - pane_->view().topRow(pane_->screen());

		const float left  = geometry.left + static_cast<float>(first) * geometry.cell_width;
		const float top   = geometry.top + static_cast<float>(viewport_row) * geometry.cell_height;
		const float width = static_cast<float>(last - first) * geometry.cell_width;

		POINT corner = { uia_detail::pixels(left, geometry.dpi),
			uia_detail::pixels(top, geometry.dpi) };
		::ClientToScreen(host.uiaWindow(), &corner);

		boxes.push_back(static_cast<double>(corner.x));
		boxes.push_back(static_cast<double>(corner.y));
		boxes.push_back(static_cast<double>(uia_detail::pixels(width, geometry.dpi)));
		boxes.push_back(static_cast<double>(uia_detail::pixels(geometry.cell_height,
			geometry.dpi)));
	}

	static HRESULT doubleArray(const std::vector<double>& values, SAFEARRAY** out) {
		SAFEARRAY* array = ::SafeArrayCreateVector(VT_R8, 0, static_cast<ULONG>(values.size()));
		if (array == nullptr) return E_OUTOFMEMORY;

		for (LONG index = 0; index < static_cast<LONG>(values.size()); ++index) {
			double value = values[static_cast<std::size_t>(index)];
			::SafeArrayPutElement(array, &index, &value);
		}

		*out = array;
		return S_OK;
	}

	// One box per row on screen; a range with no width still gets a cell's
	// worth, so a magnifier can follow the caret.
	IFACEMETHODIMP UiaRange::GetBoundingRectangles(SAFEARRAY** out) {
		if (out == nullptr) return E_POINTER;

		*out = nullptr;
		if (!refresh()) return UIA_E_ELEMENTNOTAVAILABLE;

		const Screen& screen = pane_->screen();
		const long long last = std::max(end_, std::min(start_ + 1, model().end()));
		const int top       = pane_->view().topRow(screen);
		const int first_row = static_cast<int>(start_ / columns_);
		const int last_row  = last > start_
			? static_cast<int>((last - 1) / columns_)
			: first_row - 1;
		const int bottom    = std::min(last_row, top + screen.rows() - 1);

		std::vector<double> boxes;
		for (int row = std::max(first_row, top); row <= bottom; ++row) {
			const int first = row == first_row ? static_cast<int>(start_ % columns_) : 0;
			const int end   = row == last_row
				? static_cast<int>((last - 1) % columns_) + 1
				: columns_;
			appendRowBox(boxes, row, first, end);
		}

		return doubleArray(boxes, out);
	}

	IFACEMETHODIMP UiaRange::GetEnclosingElement(IRawElementProviderSimple** out) {
		if (out == nullptr) return E_POINTER;

		*out = nullptr;
		if (!refresh()) return UIA_E_ELEMENTNOTAVAILABLE;
		return root_->QueryInterface(__uuidof(IRawElementProviderSimple),
			reinterpret_cast<void**>(out));
	}

	IFACEMETHODIMP UiaRange::GetText(int max_length, BSTR* out) {
		if (out == nullptr) return E_POINTER;

		*out = nullptr;
		if (!refresh()) return UIA_E_ELEMENTNOTAVAILABLE;

		const int limit = max_length < 0
			? uia_detail::kMaxTextLength
			: std::min(max_length, uia_detail::kMaxTextLength);
		const std::wstring text = model().text(start_, end_, limit, nullptr);
		*out = ::SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
		return *out != nullptr ? S_OK : E_OUTOFMEMORY;
	}

	// A range with width is first made one whole unit, so moving by one
	// lands on the start of the next unit whichever way it goes; a range
	// without width moves as a caret and stays without width.
	IFACEMETHODIMP UiaRange::Move(::TextUnit unit, int count, int* out) {
		if (out == nullptr) return E_POINTER;

		*out = 0;
		if (!refresh()) return UIA_E_ELEMENTNOTAVAILABLE;

		const TextModel text = model();
		const wbshterm::TextUnit step = uia_detail::unitOf(unit);
		const bool caret = start_ == end_;

		long long at = caret ? start_ : text.unitStart(start_, step);
		*out   = text.move(at, step, count);
		start_ = at;
		end_   = caret ? at : text.nextBoundary(at, step);
		return S_OK;
	}

	IFACEMETHODIMP UiaRange::MoveEndpointByUnit(TextPatternRangeEndpoint which, ::TextUnit unit,
			int count, int* out) {
		if (out == nullptr) return E_POINTER;

		*out = 0;
		if (!refresh()) return UIA_E_ELEMENTNOTAVAILABLE;

		*out = model().move(endpoint(which), uia_detail::unitOf(unit), count);
		keepOrder(which);
		return S_OK;
	}

	IFACEMETHODIMP UiaRange::MoveEndpointByRange(TextPatternRangeEndpoint which,
			ITextRangeProvider* other, TextPatternRangeEndpoint other_endpoint) {
		if (!refresh()) return UIA_E_ELEMENTNOTAVAILABLE;

		UiaRange* range = ours(other);
		if (range == nullptr) return E_INVALIDARG;

		endpoint(which) = range->endpoint(other_endpoint);
		keepOrder(which);
		return S_OK;
	}

	IFACEMETHODIMP UiaRange::Select() {
		if (!refresh()) return UIA_E_ELEMENTNOTAVAILABLE;

		TerminalView& view = pane_->view();
		if (start_ == end_) {
			view.clearSelection();
		} else {
			view.beginSelection({ static_cast<int>(start_ / columns_),
				static_cast<int>(start_ % columns_) });
			view.extendSelection({ static_cast<int>((end_ - 1) / columns_),
				static_cast<int>((end_ - 1) % columns_) });
			view.endSelection();
		}

		root_->host()->uiaRepaint();
		return S_OK;
	}

	IFACEMETHODIMP UiaRange::AddToSelection() {
		return UIA_E_INVALIDOPERATION;
	}

	IFACEMETHODIMP UiaRange::RemoveFromSelection() {
		return UIA_E_INVALIDOPERATION;
	}

	IFACEMETHODIMP UiaRange::ScrollIntoView(BOOL align_to_top) {
		if (!refresh()) return UIA_E_ELEMENTNOTAVAILABLE;

		const Screen& screen = pane_->screen();
		const int first_row = static_cast<int>(start_ / columns_);
		const int last_row  = static_cast<int>(std::max(start_, end_ - 1) / columns_);
		const int row = align_to_top ? first_row : std::max(0, last_row - screen.rows() + 1);

		pane_->view().scrollToRow(row, screen);
		root_->host()->uiaRepaint();
		return S_OK;
	}

	IFACEMETHODIMP UiaRange::GetChildren(SAFEARRAY** out) {
		if (out == nullptr) return E_POINTER;

		*out = ::SafeArrayCreateVector(VT_UNKNOWN, 0, 0);
		return *out != nullptr ? S_OK : E_OUTOFMEMORY;
	}

} /* namespace wbshterm */
