
#include "imcrvtip.h"
#include "TextService.h"
#include "CandidateList.h"

static LPCWSTR c_PreservedKeyDesc[PRESERVEDKEY_NUM] = {L"ON", L"OFF"};
static const GUID c_guidPreservedKeyOnOff[PRESERVEDKEY_NUM] = {c_guidPreservedKeyOn, c_guidPreservedKeyOff};

static LPCWSTR c_PrivateModeKeyDesc[PRIVATEMODEKEY_NUM] = {L"Private ON", L"Private OFF"};
static const GUID c_guidPrivateModeKeyOnOff[PRIVATEMODEKEY_NUM] = {c_guidPrivateModeKeyOn, c_guidPrivateModeKeyOff};

//キー透過で透過させない修飾キー
static BOOL _IsModifierVK(WPARAM wParam)
{
	switch (wParam)
	{
	case VK_SHIFT:
	case VK_CONTROL:
	case VK_MENU:
	case VK_LSHIFT:
	case VK_RSHIFT:
	case VK_LCONTROL:
	case VK_RCONTROL:
	case VK_LMENU:
	case VK_RMENU:
	case VK_LWIN:
	case VK_RWIN:
		return TRUE;
	default:
		return FALSE;
	}
}

BOOL CTextService::_IsKeyEaten(ITfContext *pContext, WPARAM wParam)
{
	if (_IsKeyboardDisabled())
	{
		return FALSE;
	}

	if (!_IsKeyboardOpen())
	{
		return FALSE;
	}

	if (_pCandidateList && _pCandidateList->_IsContextCandidateWindow(pContext))
	{
		return FALSE;
	}

	if (_IsComposing())
	{
		if (inputmode != im_ascii)
		{
			return TRUE;
		}
	}

	BYTE vk_ctrl = keystate[VK_CONTROL] & 0x80;
	BYTE vk_kana = keystate[VK_KANA] & 0x01;

	WCHAR ch = _GetCh((BYTE)wParam);
	BYTE sf = _GetSf((BYTE)wParam, ch);

	//確定状態で処理する機能
	switch (inputmode)
	{
	case im_jlatin:
	case im_ascii:
		switch (sf)
		{
		case SKK_JMODE:
		case SKK_RECONVERT:
		case SKK_QUOTE:
			return TRUE;
			break;
		default:
			break;
		}
		break;
	case im_hiragana:
	case im_katakana:
		switch (sf)
		{
		case SKK_CONV_POINT:
		case SKK_KANA:
		case SKK_CONV_CHAR:
		case SKK_JLATIN:
		case SKK_ASCII:
		case SKK_ABBREV:
		case SKK_RECONVERT:
		case SKK_QUOTE:
			return TRUE;
			break;
		case SKK_DIRECT:
			if (cx_setbydirect && !inputkey && roman.empty())
			{
				return FALSE;
			}
			break;
		default:
			break;
		}
		break;
	case im_katakana_ank:
		switch (sf)
		{
		case SKK_KANA:
		case SKK_CONV_CHAR:
		case SKK_JLATIN:
		case SKK_ASCII:
		case SKK_RECONVERT:
		case SKK_QUOTE:
			return TRUE;
			break;
		case SKK_DIRECT:
			if (cx_setbydirect && !inputkey && roman.empty())
			{
				return FALSE;
			}
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	//無効
	if (_IsKeyVoid(ch, (BYTE)wParam))
	{
		return TRUE;
	}

	//処理しないCtrlキー
	if (vk_ctrl != 0)
	{
		return FALSE;
	}

	//ASCIIモード、かなキーロックOFF
	if (inputmode == im_ascii && vk_kana == 0)
	{
		return FALSE;
	}

	if (ch >= L'\x20')
	{
		return TRUE;
	}

	return FALSE;
}

//キー透過の状態遷移
//TSF は OnTestKeyDown/OnTestKeyUp が FALSE を返すと OnKeyDown/OnKeyUp を呼ばないため、
//どちらから何回呼ばれても同じ結果になるようにする
//既知の制限: 透過中のキー X を離す前に別のキー Y を押すと idle に戻るため、
//X の KeyUp は通常の判定になり、アプリに届かないことがある
QuoteResult CTextService::_QuoteKeyDown(WPARAM wParam, BOOL eaten)
{
	if (_IsModifierVK(wParam))
	{
		return qr_none;
	}

	switch (quotestate)
	{
	case qs_armed:
		if (wParam == quotearmvk && quotearmheld)
		{
			return qr_eat;
		}
		quotestate = qs_passing;
		quotearmvk = 0;
		quotearmheld = FALSE;
		quotevk = wParam;
		quoteupvk = 0;
		return qr_pass;
	case qs_passing:
		if (wParam == quotevk)
		{
			return qr_pass;
		}
		quotestate = qs_idle;
		quotevk = 0;
		break;
	default:
		break;
	}

	quoteupvk = 0;

	WCHAR ch = _GetCh((BYTE)wParam);
	BYTE sf = _GetSf((BYTE)wParam, ch);

	if (eaten && sf == SKK_QUOTE && !_IsComposing() && roman.empty() && kana.empty() && !inputkey)
	{
		quotestate = qs_armed;
		quotearmvk = wParam;
		quotearmheld = TRUE;
		return qr_eat;
	}

	return qr_none;
}

QuoteResult CTextService::_QuoteKeyUp(WPARAM wParam)
{
	switch (quotestate)
	{
	case qs_armed:
		if (wParam == quotearmvk)
		{
			quotearmheld = FALSE;
		}
		return qr_none;
	case qs_passing:
		if (wParam == quotevk)
		{
			quotestate = qs_idle;
			quotevk = 0;
			quoteupvk = wParam;
			return qr_pass;
		}
		break;
	default:
		break;
	}

	if (quoteupvk != 0 && wParam == quoteupvk)
	{
		return qr_pass;
	}

	return qr_none;
}

STDAPI CTextService::OnSetFocus(BOOL fForeground)
{
	return S_OK;
}

STDAPI CTextService::OnTestKeyDown(ITfContext *pic, WPARAM wParam, LPARAM lParam, BOOL *pfEaten)
{
	if (pfEaten == nullptr)
	{
		return E_INVALIDARG;
	}

	_GetKeyboardState();

	BOOL eaten = _IsKeyEaten(pic, wParam);
	QuoteResult qr = _QuoteKeyDown(wParam, eaten);

	switch (qr)
	{
	case qr_eat:
		*pfEaten = TRUE;
		break;
	case qr_pass:
		*pfEaten = FALSE;
		break;
	default:
		*pfEaten = eaten;
		break;
	}

	_EndInputModeWindow();

	if (qr == qr_none && !_IsKeyboardDisabled() && _IsKeyboardOpen() && !_IsComposing())
	{
		WCHAR ch = _GetCh((BYTE)wParam);
		if (_IsKeyVoid(ch, (BYTE)wParam))
		{
			_UpdateLanguageBar();
		}
	}

	return S_OK;
}

STDAPI CTextService::OnKeyDown(ITfContext *pic, WPARAM wParam, LPARAM lParam, BOOL *pfEaten)
{
	if (pfEaten == nullptr)
	{
		return E_INVALIDARG;
	}

	_GetKeyboardState();

	BOOL eaten = _IsKeyEaten(pic, wParam);
	QuoteResult qr = _QuoteKeyDown(wParam, eaten);

	if (qr != qr_none)
	{
		*pfEaten = (qr == qr_eat);
		return S_OK;
	}

	*pfEaten = eaten;

	if (*pfEaten)
	{
		_InvokeKeyHandler(pic, wParam, lParam, SKK_NULL);
	}

	return S_OK;
}

STDAPI CTextService::OnTestKeyUp(ITfContext *pic, WPARAM wParam, LPARAM lParam, BOOL *pfEaten)
{
	if (pfEaten == nullptr)
	{
		return E_INVALIDARG;
	}

	_GetKeyboardState();

	BOOL eaten = _IsKeyEaten(pic, wParam);
	*pfEaten = (_QuoteKeyUp(wParam) == qr_pass) ? FALSE : eaten;

	return S_OK;
}

STDAPI CTextService::OnKeyUp(ITfContext *pic, WPARAM wParam, LPARAM lParam, BOOL *pfEaten)
{
	if (pfEaten == nullptr)
	{
		return E_INVALIDARG;
	}

	_GetKeyboardState();

	BOOL eaten = _IsKeyEaten(pic, wParam);
	*pfEaten = (_QuoteKeyUp(wParam) == qr_pass) ? FALSE : eaten;

	return S_OK;
}

STDAPI CTextService::OnPreservedKey(ITfContext *pic, REFGUID rguid, BOOL *pfEaten)
{
	if (pic == nullptr || pfEaten == nullptr)
	{
		return E_INVALIDARG;
	}

	_GetKeyboardState();

	BOOL fOpen = _IsKeyboardOpen();

	if (IsEqualGUID(rguid, c_guidPreservedKeyOn))
	{
		if (!fOpen)
		{
			inputmode = im_disable;

			_SetKeyboardOpen(TRUE);
		}
		else
		{
			_UpdateLanguageBar();
		}

		*pfEaten = TRUE;
	}
	else if (IsEqualGUID(rguid, c_guidPreservedKeyOff))
	{
		if (fOpen)
		{
			_ClearComposition();

			_SetKeyboardOpen(FALSE);
		}
		else
		{
			_UpdateLanguageBar();
		}

		*pfEaten = TRUE;
	}
	else if (IsEqualGUID(rguid, c_guidPrivateModeKeyOn))
	{
		if (!_IsPrivateMode())
		{
			_TogglePrivateMode();
		}

		{
			_UninitPrivateModeKey(0);	//ON
			_UninitPrivateModeKey(1);	//OFF

			_InitPrivateModeKey(1);		//OFF
			_InitPrivateModeKey(0);		//ON 未使用だがキーは拾う 重複するキーは上書きされない
		}

		if (_pCandidateList != nullptr)
		{
			_pCandidateList->_Redraw();
		}

		_UpdateLanguageBar();

		*pfEaten = TRUE;
	}
	else if (IsEqualGUID(rguid, c_guidPrivateModeKeyOff))
	{
		if (_IsPrivateMode())
		{
			_TogglePrivateMode();
		}

		{
			_UninitPrivateModeKey(0);	//ON
			_UninitPrivateModeKey(1);	//OFF

			_InitPrivateModeKey(0);		//ON
			_InitPrivateModeKey(1);		//OFF 未使用だがキーは拾う 重複するキーは上書きされない
		}

		if (_pCandidateList != nullptr)
		{
			_pCandidateList->_Redraw();
		}

		_UpdateLanguageBar();

		*pfEaten = TRUE;
	}
	else
	{
		*pfEaten = FALSE;
	}

	return S_OK;
}

BOOL CTextService::_InitKeyEventSink()
{
	HRESULT hr = E_FAIL;

	CComPtr<ITfKeystrokeMgr> pKeystrokeMgr;
	if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_PPV_ARGS(&pKeystrokeMgr))) && (pKeystrokeMgr != nullptr))
	{
		hr = pKeystrokeMgr->AdviseKeyEventSink(_ClientId, static_cast<ITfKeyEventSink *>(this), TRUE);
	}

	return SUCCEEDED(hr);
}

void CTextService::_UninitKeyEventSink()
{
	CComPtr<ITfKeystrokeMgr> pKeystrokeMgr;
	if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_PPV_ARGS(&pKeystrokeMgr))) && (pKeystrokeMgr != nullptr))
	{
		pKeystrokeMgr->UnadviseKeyEventSink(_ClientId);
	}
}

BOOL CTextService::_InitPreservedKey(int onoff)
{
	BOOL fRet = TRUE;
	HRESULT hr;

	if (onoff != 0 && onoff != 1)
	{
		return FALSE;
	}

	CComPtr<ITfKeystrokeMgr> pKeystrokeMgr;
	if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_PPV_ARGS(&pKeystrokeMgr))) && (pKeystrokeMgr != nullptr))
	{
		for (int i = 0; i < MAX_PRESERVEDKEY; i++)
		{
			if (preservedkey[onoff][i].uVKey == 0 && preservedkey[onoff][i].uModifiers == 0)
			{
				break;
			}

			hr = pKeystrokeMgr->PreserveKey(_ClientId, c_guidPreservedKeyOnOff[onoff],
				&preservedkey[onoff][i], c_PreservedKeyDesc[onoff], (ULONG)wcslen(c_PreservedKeyDesc[onoff]));

			if (FAILED(hr))
			{
				fRet = FALSE;
			}
		}
	}
	else
	{
		fRet = FALSE;
	}

	return fRet;
}

void CTextService::_UninitPreservedKey(int onoff)
{
	HRESULT hr;

	if (onoff != 0 && onoff != 1)
	{
		return;
	}

	CComPtr<ITfKeystrokeMgr> pKeystrokeMgr;
	if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_PPV_ARGS(&pKeystrokeMgr))) && (pKeystrokeMgr != nullptr))
	{
		for (int i = 0; i < MAX_PRESERVEDKEY; i++)
		{
			if (preservedkey[onoff][i].uVKey == 0 && preservedkey[onoff][i].uModifiers == 0)
			{
				break;
			}

			hr = pKeystrokeMgr->UnpreserveKey(c_guidPreservedKeyOnOff[onoff], &preservedkey[onoff][i]);
		}
	}
}

BOOL CTextService::_InitPrivateModeKey(int onoff)
{
	HRESULT hr = E_FAIL;

	if (onoff != 0 && onoff != 1)
	{
		return FALSE;
	}

	CComPtr<ITfKeystrokeMgr> pKeystrokeMgr;
	if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_PPV_ARGS(&pKeystrokeMgr))) && (pKeystrokeMgr != nullptr))
	{
		hr = pKeystrokeMgr->PreserveKey(_ClientId, c_guidPrivateModeKeyOnOff[onoff],
			&privatemodekey[onoff], c_PrivateModeKeyDesc[onoff], (ULONG)wcslen(c_PrivateModeKeyDesc[onoff]));
	}

	return SUCCEEDED(hr);
}

void CTextService::_UninitPrivateModeKey(int onoff)
{
	HRESULT hr = E_FAIL;

	if (onoff != 0 && onoff != 1)
	{
		return;
	}

	CComPtr<ITfKeystrokeMgr> pKeystrokeMgr;
	if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_PPV_ARGS(&pKeystrokeMgr))) && (pKeystrokeMgr != nullptr))
	{
		hr = pKeystrokeMgr->UnpreserveKey(c_guidPrivateModeKeyOnOff[onoff], &privatemodekey[onoff]);
	}
}
