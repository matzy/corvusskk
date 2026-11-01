
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <winhttp.h>
#include "utf8.h"
#include "imcrvmgr.h"

// Google CGI API for Japanese Input (http://www.google.co.jp/ime/cgiapi.html)
//
// imcrvmgr のパイプスレッドは単一なので、HTTPリクエストはワーカースレッドで実行し、
// パイプスレッドは最大 wait ミリ秒だけ結果を待つ。
// 間に合わなかった結果もキャッシュされ、同じ読みの次回の検索で使われる。

#define GOOGLE_DEFAULT_URL		L"https://www.google.com/transliterate?langpair=ja-Hira%7Cja&text="
#define GOOGLE_DEFAULT_ANNOTATION	L"G"
#define GOOGLE_DEFAULT_TIMEOUT	3000	// ワーカー側のHTTP全体のタイムアウト
#define GOOGLE_DEFAULT_WAIT		300		// 検索時に待つ最大時間
#define GOOGLE_MAX_TIMEOUT		60000
#define GOOGLE_MAX_WAIT			5000
#define GOOGLE_MAX_KEY_LENGTH	100
#define GOOGLE_MAX_RESPONSE		(256 * 1024)
#define GOOGLE_MAX_WORKERS		4
#define GOOGLE_MAX_CACHE		1000
#define GOOGLE_MAX_JSON_DEPTH	8
#define GOOGLE_FAIL_LIMIT		3		// 連続失敗がこの回数に達したらバックオフ
#define GOOGLE_BACKOFF_MS		30000	// バックオフ時間

namespace
{
	struct GoogleSettings
	{
		BOOL enable = FALSE;
		std::wstring url = GOOGLE_DEFAULT_URL;
		DWORD timeout = GOOGLE_DEFAULT_TIMEOUT;
		DWORD wait = GOOGLE_DEFAULT_WAIT;
		std::wstring annotation = GOOGLE_DEFAULT_ANNOTATION;
		BOOL privateSkip = TRUE;	// プライベートモードの検索では使用しない
	};

	struct GoogleRequest
	{
		HANDLE hDone;				// 完了通知 (手動リセット)
		std::wstring key;
		std::wstring result;		// hDone がシグナルされる前に書き込む
		HINTERNET hConnect = nullptr;	// mtx で保護。終了時に別スレッドから閉じて中断させる
		HINTERNET hRequest = nullptr;	// mtx で保護
		unsigned int generation = 0;	// 開始時の設定世代。設定変更後に届いた古い結果をキャッシュしないため

		explicit GoogleRequest(const std::wstring &k) : hDone(CreateEventW(nullptr, TRUE, FALSE, nullptr)), key(k) {}
		~GoogleRequest()
		{
			if (hDone != nullptr)
			{
				CloseHandle(hDone);
			}
		}
		GoogleRequest(const GoogleRequest&) = delete;
		GoogleRequest &operator=(const GoogleRequest&) = delete;
	};

	// 現在の検索がプライベートモードか
	// 辞書検索は単一のパイプスレッドで実行されるので、そのスレッドごとの値にしておく
	thread_local bool tlPrivateSearch = false;

	std::mutex mtx;
	GoogleSettings settings;
	std::map<std::wstring, std::wstring> cache;
	std::map<std::wstring, std::shared_ptr<GoogleRequest>> inflight;
	HINTERNET hSession = nullptr;
	int activeWorkers = 0;
	int failCount = 0;
	ULONGLONG backoffUntil = 0;
	bool exiting = false;
	unsigned int configGeneration = 0;	// 設定が変わるたびに増やす

	// ひらがな・長音のみのキーか
	bool IsHiraganaKey(const std::wstring &key)
	{
		if (key.empty() || key.size() > GOOGLE_MAX_KEY_LENGTH)
		{
			return false;
		}

		for (const auto &ch : key)
		{
			if (!((ch >= 0x3041 && ch <= 0x3096) || ch == 0x309D || ch == 0x309E || ch == 0x30FC))
			{
				return false;
			}
		}

		return true;
	}

	std::wstring PercentEncode(const std::string &s)
	{
		std::wstring ret;
		WCHAR pe[4];

		for (const auto &ch : s)
		{
			_snwprintf_s(pe, _TRUNCATE, L"%%%02X", (BYTE)ch);
			ret += pe;
		}

		return ret;
	}

	// mtx を保持して呼ぶ
	void CloseRequestHandles(GoogleRequest &req)
	{
		if (req.hRequest != nullptr)
		{
			WinHttpCloseHandle(req.hRequest);
			req.hRequest = nullptr;
		}
		if (req.hConnect != nullptr)
		{
			WinHttpCloseHandle(req.hConnect);
			req.hConnect = nullptr;
		}
	}

	// mtx を保持して呼ぶ
	HINTERNET GetSession()
	{
		if (hSession == nullptr && !exiting)
		{
			static const WCHAR ua[] = TEXTSERVICE_NAME L"/" TEXTSERVICE_VER;

			hSession = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
				WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			if (hSession == nullptr)
			{
				// Windows 8.1 より前
				hSession = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
					WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			}
		}

		return hSession;
	}

	// ハンドルは req に登録し、終了時に CleanUpGoogleCGIAPI が閉じて同期呼び出しを中断できるようにする
	bool HttpGet(HINTERNET session, GoogleRequest &req, const std::wstring &url, DWORD timeout, std::string &body)
	{
		bool ret = false;
		HINTERNET hConnect = nullptr;
		HINTERNET hRequest = nullptr;
		const ULONGLONG deadline = GetTickCount64() + timeout;

		// 終了処理中ならハンドルを閉じて false
		auto publish = [](HINTERNET &slot, HINTERNET h) -> bool
		{
			std::lock_guard<std::mutex> lock(mtx);

			if (h == nullptr)
			{
				return false;
			}
			if (exiting)
			{
				WinHttpCloseHandle(h);
				return false;
			}

			slot = h;
			return true;
		};

		URL_COMPONENTS uc = {};
		uc.dwStructSize = sizeof(uc);
		uc.dwSchemeLength = (DWORD)-1;
		uc.dwHostNameLength = (DWORD)-1;
		uc.dwUrlPathLength = (DWORD)-1;
		uc.dwExtraInfoLength = (DWORD)-1;

		if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc))
		{
			return false;
		}
		// 入力した読みを送るので、暗号化されない通信は許可しない
		if (uc.nScheme != INTERNET_SCHEME_HTTPS)
		{
			return false;
		}
		if (uc.dwHostNameLength == 0)
		{
			return false;
		}

		std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
		std::wstring object(uc.lpszUrlPath, uc.dwUrlPathLength + uc.dwExtraInfoLength);
		if (object.empty())
		{
			object = L"/";
		}

		hConnect = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
		if (!publish(req.hConnect, hConnect))
		{
			goto end;
		}

		hRequest = WinHttpOpenRequest(hConnect, L"GET", object.c_str(), nullptr,
			WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
			(uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
		if (!publish(req.hRequest, hRequest))
		{
			goto end;
		}

		// resolve / connect / send / receive
		// resolve は既定で無制限なので必ず設定する
		if (!WinHttpSetTimeouts(hRequest, (int)timeout, (int)timeout, (int)timeout, (int)timeout))
		{
			goto end;
		}

		if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
			!WinHttpReceiveResponse(hRequest, nullptr))
		{
			goto end;
		}

		{
			DWORD status = 0;
			DWORD size = sizeof(status);
			if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200)
			{
				goto end;
			}
		}

		while (true)
		{
			if (GetTickCount64() > deadline)
			{
				goto end;
			}

			DWORD avail = 0;
			if (!WinHttpQueryDataAvailable(hRequest, &avail))
			{
				goto end;
			}
			if (avail == 0)
			{
				break;
			}

			if (body.size() + avail > GOOGLE_MAX_RESPONSE)
			{
				goto end;
			}

			size_t pos = body.size();
			body.resize(pos + avail);
			DWORD read = 0;
			if (!WinHttpReadData(hRequest, &body[pos], avail, &read))
			{
				goto end;
			}
			body.resize(pos + read);
		}

		ret = true;

	end:
		{
			// CleanUpGoogleCGIAPI が先に閉じていれば nullptr になっている
			std::lock_guard<std::mutex> lock(mtx);
			CloseRequestHandles(req);
		}

		return ret;
	}

	// 応答の形は固定
	//   [["よみ",["候補1","候補2",...]],["よみ2",["候補",...]],...]
	// 必要な部分だけの小さなJSONパーサー (文字列・配列のみ)
	struct JsonValue
	{
		enum Type { Other, String, Array } type = Other;
		std::wstring str;
		std::vector<JsonValue> arr;
	};

	class JsonParser
	{
	public:
		explicit JsonParser(const std::wstring &s) : p(s.c_str()), e(s.c_str() + s.size()) {}

		bool Parse(JsonValue &v)
		{
			if (!ParseValue(v, 0))
			{
				return false;
			}
			SkipSpace();
			return p == e;
		}

	private:
		const WCHAR *p;
		const WCHAR *e;

		void SkipSpace()
		{
			while (p < e && (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n'))
			{
				++p;
			}
		}

		static int HexValue(WCHAR c)
		{
			if (c >= L'0' && c <= L'9') return c - L'0';
			if (c >= L'a' && c <= L'f') return c - L'a' + 10;
			if (c >= L'A' && c <= L'F') return c - L'A' + 10;
			return -1;
		}

		bool ParseString(std::wstring &out)
		{
			// *p == '"'
			++p;
			while (p < e)
			{
				WCHAR c = *p++;
				if (c == L'"')
				{
					return true;
				}
				if (c != L'\\')
				{
					out.push_back(c);
					continue;
				}
				if (p >= e)
				{
					return false;
				}
				c = *p++;
				switch (c)
				{
				case L'"':  out.push_back(L'"');  break;
				case L'\\': out.push_back(L'\\'); break;
				case L'/':  out.push_back(L'/');  break;
				case L'b':  out.push_back(L'\b'); break;
				case L'f':  out.push_back(L'\f'); break;
				case L'n':  out.push_back(L'\n'); break;
				case L'r':  out.push_back(L'\r'); break;
				case L't':  out.push_back(L'\t'); break;
				case L'u':
					{
						if (e - p < 4)
						{
							return false;
						}
						unsigned int u = 0;
						for (int i = 0; i < 4; i++)
						{
							int h = HexValue(*p++);
							if (h < 0)
							{
								return false;
							}
							u = (u << 4) | (unsigned int)h;
						}
						// サロゲートペアはUTF-16のままそのまま追加すればよい
						out.push_back((WCHAR)u);
					}
					break;
				default:
					return false;
				}
			}

			return false;
		}

		bool ParseValue(JsonValue &v, int depth)
		{
			if (depth > GOOGLE_MAX_JSON_DEPTH)
			{
				return false;
			}

			SkipSpace();
			if (p >= e)
			{
				return false;
			}

			if (*p == L'"')
			{
				v.type = JsonValue::String;
				return ParseString(v.str);
			}

			if (*p == L'[')
			{
				v.type = JsonValue::Array;
				++p;
				SkipSpace();
				if (p < e && *p == L']')
				{
					++p;
					return true;
				}
				while (true)
				{
					v.arr.emplace_back();
					if (!ParseValue(v.arr.back(), depth + 1))
					{
						return false;
					}
					SkipSpace();
					if (p >= e)
					{
						return false;
					}
					if (*p == L',')
					{
						++p;
						continue;
					}
					if (*p == L']')
					{
						++p;
						return true;
					}
					return false;
				}
			}

			if (*p == L'{')
			{
				// 想定外
				return false;
			}

			// 数値・true・false・null
			const WCHAR *start = p;
			while (p < e && *p != L',' && *p != L']' && *p != L'}' &&
				*p != L' ' && *p != L'\t' && *p != L'\r' && *p != L'\n')
			{
				++p;
			}
			v.type = JsonValue::Other;
			return p != start;
		}
	};

	// 候補をSKK辞書の候補形式にする
	std::wstring MakeSKKCandidate(const std::wstring &cand, const std::wstring &annotation)
	{
		std::wstring c;

		// 制御文字を除く
		for (const auto &ch : cand)
		{
			if (ch >= 0x20)
			{
				c.push_back(ch);
			}
		}

		if (c.empty())
		{
			return c;
		}

		// "/" ";" を含む、または実行変換 "(...)" と見なされるものは (concat "...") にする
		if (c.find_first_of(L"/;") != std::wstring::npos ||
			(c.size() > 2 && c.front() == L'(' && c.back() == L')'))
		{
			std::wstring t;

			for (const auto &ch : c)
			{
				switch (ch)
				{
				case L'\\': t += L"\\\\";  break;
				case L'"':  t += L"\\\"";  break;
				case L'/':  t += L"\\057"; break;
				case L';':  t += L"\\073"; break;
				default:    t.push_back(ch); break;
				}
			}

			c = L"(concat \"" + t + L"\")";
		}

		if (!annotation.empty())
		{
			c += L";" + annotation;
		}

		return c;
	}

	// 戻り値: 応答の解析に成功したか (候補0件でも成功)
	bool ParseResponse(const std::string &body, const std::wstring &key, const std::wstring &annotation, std::wstring &result)
	{
		result.clear();

		std::wstring wbody = utf8_string_to_wstring(body);
		if (wbody.empty())
		{
			return false;
		}

		JsonValue root;
		JsonParser parser(wbody);
		if (!parser.Parse(root) || root.type != JsonValue::Array)
		{
			return false;
		}

		std::wstring cands;
		std::vector<std::wstring> added;

		for (const auto &seg : root.arr)
		{
			// 読みが検索キー全体に一致するセグメントだけ
			if (seg.type != JsonValue::Array || seg.arr.size() < 2 ||
				seg.arr[0].type != JsonValue::String || seg.arr[0].str != key ||
				seg.arr[1].type != JsonValue::Array)
			{
				continue;
			}

			for (const auto &cand : seg.arr[1].arr)
			{
				// 読みそのものの候補は除く
				if (cand.type != JsonValue::String || cand.str.empty() || cand.str == key)
				{
					continue;
				}

				if (std::find(added.begin(), added.end(), cand.str) != added.end())
				{
					continue;
				}
				added.push_back(cand.str);

				std::wstring c = MakeSKKCandidate(cand.str, annotation);
				if (!c.empty())
				{
					cands += L"/" + c;
				}
			}
		}

		if (!cands.empty())
		{
			result = cands + L"/\n";
		}

		return true;
	}

	void GoogleWorker(std::shared_ptr<GoogleRequest> req, GoogleSettings s)
	{
		std::string body;
		std::wstring result;
		bool ok = false;
		HINTERNET session;

		{
			std::lock_guard<std::mutex> lock(mtx);
			session = GetSession();
		}

		if (session != nullptr)
		{
			// 見出し語 + "," : 全体を1文節として変換させる
			std::wstring url = s.url + PercentEncode(wstring_to_utf8_string(req->key) + ",");

			if (HttpGet(session, *req, url, s.timeout, body))
			{
				ok = ParseResponse(body, req->key, s.annotation, result);
			}
		}

		req->result = result;

		{
			std::lock_guard<std::mutex> lock(mtx);

			if (ok && req->generation != configGeneration)
			{
				// 設定が変わる前の結果は捨てる (失敗扱いにもしない)
			}
			else if (ok)
			{
				// 候補0件もキャッシュする
				if (cache.size() >= GOOGLE_MAX_CACHE)
				{
					cache.clear();
				}
				cache[req->key] = result;
				failCount = 0;
			}
			else if (!exiting && req->generation == configGeneration)
			{
				// 設定が変わる前に始まったリクエストの失敗は、新しい設定のバックオフに数えない
				if (++failCount >= GOOGLE_FAIL_LIMIT)
				{
					backoffUntil = GetTickCount64() + GOOGLE_BACKOFF_MS;
					failCount = 0;
				}
			}

			inflight.erase(req->key);
			--activeWorkers;
		}

		SetEvent(req->hDone);
	}
}

void SetGoogleCGIAPIPrivate(BOOL privateSearch)
{
	tlPrivateSearch = (privateSearch != FALSE);
}

void SetGoogleCGIAPIConfig(BOOL enable, const std::wstring &url, DWORD timeout, DWORD wait, const std::wstring &annotation, BOOL privateSkip)
{
	GoogleSettings s;

	s.enable = enable ? TRUE : FALSE;
	s.privateSkip = privateSkip ? TRUE : FALSE;
	s.url = url.empty() ? GOOGLE_DEFAULT_URL : url;
	s.timeout = (timeout == 0 || timeout > GOOGLE_MAX_TIMEOUT) ? GOOGLE_DEFAULT_TIMEOUT : timeout;
	s.wait = (wait > GOOGLE_MAX_WAIT) ? GOOGLE_DEFAULT_WAIT : wait;
	// 候補の区切り・注釈の区切りと制御文字は使えない
	s.annotation = annotation;
	s.annotation.erase(std::remove_if(s.annotation.begin(), s.annotation.end(),
		[](WCHAR ch) { return ch == L'/' || ch == L';' || ch < 0x20; }), s.annotation.end());

	std::lock_guard<std::mutex> lock(mtx);

	if (s.enable != settings.enable || s.url != settings.url ||
		s.timeout != settings.timeout || s.annotation != settings.annotation)
	{
		// 設定が変わったらキャッシュを捨てる
		cache.clear();
		failCount = 0;
		backoffUntil = 0;
		++configGeneration;
	}

	settings = s;
}

std::wstring SearchGoogleCGIAPI(const std::wstring &searchkey)
{
	GoogleSettings s;
	std::shared_ptr<GoogleRequest> req;

	if (!IsHiraganaKey(searchkey))
	{
		return std::wstring();
	}

	{
		std::lock_guard<std::mutex> lock(mtx);

		if (!settings.enable || exiting)
		{
			return std::wstring();
		}

		// プライベートモードでは問い合わせない。キャッシュも使わない
		if (tlPrivateSearch && settings.privateSkip)
		{
			return std::wstring();
		}

		s = settings;

		// キャッシュ
		auto it = cache.find(searchkey);
		if (it != cache.end())
		{
			return it->second;
		}

		// バックオフ中
		if (GetTickCount64() < backoffUntil)
		{
			return std::wstring();
		}

		auto itf = inflight.find(searchkey);
		if (itf != inflight.end())
		{
			req = itf->second;
		}
		else
		{
			if (activeWorkers >= GOOGLE_MAX_WORKERS)
			{
				return std::wstring();
			}

			req = std::make_shared<GoogleRequest>(searchkey);
			if (req->hDone == nullptr)
			{
				return std::wstring();
			}

			req->generation = configGeneration;
			inflight[searchkey] = req;
			++activeWorkers;

			try
			{
				std::thread worker(GoogleWorker, req, s);
				worker.detach();
			}
			catch (...)
			{
				inflight.erase(searchkey);
				--activeWorkers;
				return std::wstring();
			}
		}
	}

	if (s.wait == 0)
	{
		return std::wstring();
	}

	// 最大 wait ミリ秒だけ待つ
	if (WaitForSingleObject(req->hDone, s.wait) == WAIT_OBJECT_0)
	{
		return req->result;
	}

	return std::wstring();
}

void CleanUpGoogleCGIAPI()
{
	{
		std::lock_guard<std::mutex> lock(mtx);

		exiting = true;

		// 実行中のリクエストを中断させる (同期呼び出しで待っているワーカーはハンドルを閉じると戻る)
		for (auto &kv : inflight)
		{
			CloseRequestHandles(*kv.second);
		}

		if (hSession != nullptr)
		{
			WinHttpCloseHandle(hSession);
			hSession = nullptr;
		}
	}

	// ワーカーの終了を少し待つ
	for (int i = 0; i < 200; i++)
	{
		{
			std::lock_guard<std::mutex> lock(mtx);
			if (activeWorkers == 0)
			{
				break;
			}
		}
		Sleep(10);
	}
}
